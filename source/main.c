#include "app.h"
#include <citro2d.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static App app;
static C2D_TextBuf text_buffer;
static u32 white, muted, accent, background;
static bool battery_ready;
static u8 battery_level, charging;
static C3D_Tex cover_texture;
static bool cover_texture_ready, cover_visible;
static unsigned uploaded_cover;
static unsigned char cover_upload[128 * 128 * 4];
static const Tex3DS_SubTexture cover_subtexture = {
    .width = 128, .height = 128, .left = 0, .top = 1,
    .right = 1, .bottom = 0
};
#define LIST_ROWS 6
#define LIST_Y 38
#define LIST_ROW_HEIGHT 28

static void status_bar(int width) {
    static time_t last_poll;
    time_t now = time(NULL);
    if (now != last_poll && battery_ready) {
        PTMU_GetBatteryLevel(&battery_level);
        PTMU_GetBatteryChargeState(&charging);
        last_poll = now;
    }
    unsigned wifi = osGetWifiStrength();
    for (int i = 0; i < 3; ++i)
        C2D_DrawRectSolid(12 + i * 5, 18 - i * 4, 0, 3, 4 + i * 4, wifi > (unsigned)i ? white : muted);
    float bx = width - 36;
    C2D_DrawRectSolid(bx, 9, 0, 22, 10, muted);
    C2D_DrawRectSolid(bx + 22, 12, 0, 2, 4, muted);
    C2D_DrawRectSolid(bx + 1, 10, 0, 20, 8, background);
    for (int i = 0; battery_ready && i < battery_level && i < 5; ++i)
        C2D_DrawRectSolid(bx + 2 + i * 4, 11, 0, 3, 6, charging ? accent : white);
}

static void text(const char *s, float x, float y, float scale, float width, u32 color, bool wrap) {
    C2D_Text t;
    C2D_TextParse(&t, text_buffer, s);
    C2D_TextOptimize(&t);
    if (wrap) C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0, scale, scale, color, width);
    else {
        float w;
        C2D_TextGetDimensions(&t, scale, scale, &w, NULL);
        if (w > width) scale *= width / w;
        C2D_DrawText(&t, C2D_WithColor, x, y, 0, scale, scale, color);
    }
}

static void draw(C3D_RenderTarget *top, C3D_RenderTarget *bottom, int selected, bool exiting,
                 bool settings_open, int setting_selected) {
    YmTrack current = {0}, rows[LIST_ROWS] = {0};
    char status[256], label[128], collection[YM_TEXT_SIZE];
    int count, playing, start;
    unsigned kb, total_kb;
    bool download_complete;
    bool loading, shuffle, repeat_one, high_quality, playlist_view, update_cover = false;
    unsigned position;
    int rate, bitrate;
    LightLock_Lock(&app.lock);
    playlist_view = app.playlist_view;
    count = playlist_view ? app.playlist_count : app.count;
    strcpy(collection, playlist_view ? "Плейлисты" : app.collection);
    playing = app.playing;
    loading = app.loading;
    shuffle = app.shuffle;
    repeat_one = app.repeat_one;
    high_quality = app.high_quality;
    kb = app.downloaded_kb;
    total_kb = app.download_total_kb;
    download_complete = app.download_complete;
    position = app.position_ms;
    rate = app.sample_rate;
    bitrate = app.bitrate_kbps;
    strcpy(status, app.status);
    start = selected / LIST_ROWS * LIST_ROWS;
    for (int i = 0; i < LIST_ROWS && start + i < count; ++i) {
        if (!playlist_view) rows[i] = app.tracks[start + i];
        else {
            YmPlaylist *p = &app.playlists[start + i];
            strcpy(rows[i].title, p->title);
            if (start + i) snprintf(rows[i].artist, sizeof(rows[i].artist), "%u треков", p->track_count);
            else strcpy(rows[i].artist, "Любимые песни");
            rows[i].available = true;
        }
    }
    if (playing >= 0 && playing < app.count) current = app.tracks[playing];
    else if (!playlist_view && selected >= 0 && selected < app.count) current = app.tracks[selected];
    if (cover_texture_ready && uploaded_cover != app.cover_revision) {
        uploaded_cover = app.cover_revision;
        cover_visible = app.cover_pixels != NULL;
        if (cover_visible) { memcpy(cover_upload, app.cover_pixels, sizeof(cover_upload)); update_cover = true; }
    }
    bool draw_cover = cover_visible && !strcmp(current.id, app.cover_id) && current.id[0];
    LightLock_Unlock(&app.lock);
    bool paused = playing >= 0 && atomic_load(&app.paused);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    if (update_cover) C3D_TexUpload(&cover_texture, cover_upload);
    C2D_TextBufClear(text_buffer);
    C2D_TargetClear(top, background);
    C2D_SceneBegin(top);
    status_bar(400);
    char clock_label[16] = "--:--";
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (local) strftime(clock_label, sizeof(clock_label), "%H:%M", local);
    text(clock_label, 177, 5, 0.43f, 60, white, false);
    text("YM3DS", 45, 5, 0.40f, 80, muted, false);
    text("Сейчас играет", 12, 28, 0.57f, 300, white, false);
    C2D_DrawRectSolid(12, 49, 0, 376, 1, muted);
    /* Fallback record when artwork is unavailable. */
    C2D_DrawRectSolid(12, 63, 0, 132, 132, C2D_Color32(49, 49, 49, 255));
    C2D_DrawCircleSolid(78, 129, 0, 49, C2D_Color32(28, 28, 28, 255));
    C2D_DrawCircleSolid(78, 129, 0, 31, C2D_Color32(44, 44, 44, 255));
    C2D_DrawCircleSolid(78, 129, 0, 15, accent);
    C2D_DrawCircleSolid(78, 129, 0, 3, background);
    if (draw_cover) C2D_DrawImageAt((C2D_Image){&cover_texture, &cover_subtexture}, 12, 63, 0, NULL, 132.0f / 128, 132.0f / 128);
    text(high_quality ? "MP3 · 320" : "MP3 · 192", 159, 62, 0.36f, 140, accent, false);
    text(current.title[0] ? current.title : "Яндекс Музыка", 159, 84, 0.57f, 229, white, false);
    text(current.artist, 159, 108, 0.43f, 229, muted, false);
    unsigned shown_position = playing >= 0 ? position : 0;
    snprintf(label, sizeof(label), "%u:%02u", shown_position / 60000, shown_position / 1000 % 60);
    text(label, 159, 139, 0.43f, 60, white, false);
    snprintf(label, sizeof(label), "%u:%02u", current.duration_ms / 60000, current.duration_ms / 1000 % 60);
    text(current.duration_ms ? label : "--:--", 349, 139, 0.43f, 39, white, false);
    float progress = current.duration_ms ? (float)shown_position / current.duration_ms : 0;
    if (progress > 1) progress = 1;
    C2D_DrawRectSolid(159, 160, 0, 229, 4, muted);
    float downloaded = download_complete ? 1 : total_kb ? (float)kb / total_kb : 0;
    if (downloaded > 1) downloaded = 1;
    if (playing >= 0 && downloaded > 0)
        C2D_DrawRectSolid(159, 160, 0, 229 * downloaded, 4, C2D_Color32(112, 151, 178, 255));
    if (progress > 0) C2D_DrawRectSolid(159, 160, 0, 229 * progress, 4, accent);
    if (rate) snprintf(label, sizeof(label), "%.1f кГц", rate / 1000.0);
    else strcpy(label, "MP3 / SD");
    text(label, 159, 173, 0.40f, 100, muted, false);
    if (bitrate) {
        snprintf(label, sizeof(label), "%d кбит/с", bitrate);
        text(label, 287, 173, 0.40f, 101, muted, false);
    }
    text(exiting ? "Завершаю операцию…" : paused ? "Пауза — Y: продолжить" : status,
         12, 207, 0.40f, 376, paused ? accent : muted, false);
    if (loading && !download_complete && kb) {
        if (total_kb) snprintf(label, sizeof(label), "%s: %u%% · %u КБ", paused ? "Пауза / загрузка" : "Загрузка", (unsigned)(downloaded * 100), kb);
        else snprintf(label, sizeof(label), "%s: %u КБ", paused ? "Пауза / загружено" : "Загружено", kb);
        text(label, 159, 190, 0.35f, 229, accent, false);
    } else if (playing >= 0) {
        text(paused ? "Пауза" : "Воспроизведение", 159, 190, 0.35f, 229, accent, false);
    }

    C2D_TargetClear(bottom, background);
    C2D_SceneBegin(bottom);
    if (settings_open) {
        text("Настройки", 12, 8, 0.57f, 296, white, false);
        C2D_DrawRectSolid(12, 30, 0, 296, 1, muted);
        for (int i = 0; i < 3; ++i) {
            float y = 43 + i * 36;
            if (i == setting_selected) C2D_DrawRectSolid(8, y + 2, 0, 3, 24, accent);
            if (i == 0) snprintf(label, sizeof(label), "Битрейт: %d кбит/с", high_quality ? 320 : 192);
            else if (i == 1) snprintf(label, sizeof(label), "Перемешивание: %s", shuffle ? "вкл" : "выкл");
            else snprintf(label, sizeof(label), "Повтор песни: %s", repeat_one ? "вкл" : "выкл");
            text(label, 18, y, 0.49f, 290, white, false);
        }
        text("Битрейт — со следующего запуска", 12, 188, 0.35f, 296, muted, false);
        text("↑/↓: пункт  ←/→ или A: изменить", 12, 210, 0.37f, 296, white, false);
        text("SELECT / B: закрыть  Y: пауза", 12, 226, 0.37f, 296, muted, false);
    } else {
    ym_text_copy(label, 81, collection);
    size_t label_size = strlen(label);
    snprintf(label + label_size, sizeof(label) - label_size, "  %d/%d", count ? selected + 1 : 0, count);
    text(label, 12, 8, 0.53f, 296, white, false);
    C2D_DrawRectSolid(12, 30, 0, 296, 1, muted);
    for (int i = 0; i < LIST_ROWS && start + i < count; ++i) {
        float y = LIST_Y + i * LIST_ROW_HEIGHT;
        if (start + i == selected) C2D_DrawRectSolid(8, y + 2, 0, 3, 23, accent);
        text(rows[i].title, 18, y, 0.44f, 290, rows[i].available ? white : muted, false);
        text(rows[i].artist, 18, y + 15, 0.38f, 290, muted, false);
    }
    if (!count) text("X — библиотека\nТокен: /3ds/ym3ds/config/token.txt", 12, 58, 0.45f, 296, muted, true);
    C2D_DrawRectSolid(12, 208, 0, 296, 1, muted);
    text(playlist_view ? "A: открыть  X: любимые  B: назад" : "A: играть  Y: пауза  L/R: трек", 12, 211, 0.40f, 296, white, false);
    text("SELECT: настройки  X: списки  B: стоп", 12, 226, 0.37f, 296, muted, false);
    }
    C3D_FrameEnd(0);
}

int main(void) {
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) { gfxExit(); return 1; }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { C3D_Fini(); gfxExit(); return 1; }
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    text_buffer = C2D_TextBufNew(4096);
    if (!top || !bottom || !text_buffer) {
        if (text_buffer) C2D_TextBufDelete(text_buffer);
        C2D_Fini(); C3D_Fini(); gfxExit(); return 1;
    }
    white = C2D_Color32(239, 240, 242, 255);
    muted = C2D_Color32(156, 160, 172, 255);
    accent = C2D_Color32(250, 225, 35, 255);
    background = C2D_Color32(29, 29, 29, 255);
    battery_ready = R_SUCCEEDED(ptmuInit());
    LightLock_Init(&app.lock);
    atomic_init(&app.generation, 1);
    atomic_init(&app.quitting, false);
    atomic_init(&app.paused, false);
    atomic_init(&app.seek_seconds, 0);
    srand((unsigned)time(NULL));
    strcpy(app.collection, "Мне нравится");
    cover_texture_ready = C3D_TexInit(&cover_texture, 128, 128, GPU_RGBA8);
    if (cover_texture_ready) C3D_TexSetFilter(&cover_texture, GPU_LINEAR, GPU_LINEAR);
    app.job = -1;
    app.playing = -1;
    mkdir("sdmc:/3ds", 0777);
    mkdir(YM_BASE, 0777);
    mkdir(YM_BASE "/config", 0777);
    mkdir(YM_BASE "/cache", 0777);
    FILE *token = fopen(YM_TOKEN_FILE, "rb");
    if (token) fclose(token);
    else {
        token = fopen(YM_TOKEN_FILE, "wb");
        if (token) { fputs("YANDEX_TOKEN = \"\"\n", token); fclose(token); }
    }
    bool soc_ready = false, curl_ready = false;
    void *soc_buffer = memalign(0x1000, 0x100000);
    if (soc_buffer) soc_ready = R_SUCCEEDED(socInit(soc_buffer, 0x100000));
    if (soc_ready) curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    app.audio_ready = R_SUCCEEDED(ndspInit());
    if (app.audio_ready) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    Thread worker = NULL;
    if (curl_ready) {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        /* Core 0 works on original and New 3DS. New hardware may boost CPU. */
        osSetSpeedupEnable(true);
        worker = threadCreate(app_worker, &app, 128 * 1024, priority + 1, 0, false);
    }
    if (worker) app_request(&app, -2);
    else app_status(&app, "Не удалось запустить сеть или рабочий поток");
    int selected = 0;
    unsigned library_revision = 0;
    unsigned playback_revision = 0;
    u32 shoulder = 0;
    u64 shoulder_started = 0, shoulder_repeat = 0;
    bool shoulder_seeking = false;
    bool settings_open = false;
    int setting_selected = 0;
    bool old_sleep_allowed = aptIsSleepAllowed();
    aptSetSleepAllowed(false);
    bool lcd_ready = R_SUCCEEDED(gspLcdInit()), lid_closed = false;
    while (aptMainLoop()) {
        u8 shell = 1;
        if (battery_ready && R_SUCCEEDED(PTMU_GetShellState(&shell))) {
            bool closed = shell == 0;
            if (closed != lid_closed) {
                if (lcd_ready) {
                    if (closed) GSPLCD_PowerOffAllBacklights();
                    else GSPLCD_PowerOnAllBacklights();
                }
                GSPGPU_SetLcdForceBlack(closed ? 1 : 0);
                lid_closed = closed;
            }
        }
        if (lid_closed) {
            shoulder = 0;
            svcSleepThread(50000000);
            continue;
        }
        hidScanInput();
        u32 keys = hidKeysDown();
        u32 held = hidKeysHeld(), released = hidKeysUp();
        u32 track_key = 0;
        if (keys & KEY_START) break;
        LightLock_Lock(&app.lock);
        bool playlist_view = app.playlist_view;
        int count = playlist_view ? app.playlist_count : app.count;
        if (library_revision != app.library_revision) { selected = 0; library_revision = app.library_revision; }
        bool busy = app.loading;
        int playing = app.playing;
        if (!playlist_view && playing >= 0 && playing < app.count &&
            playback_revision != app.playback_revision) {
            selected = playing;
            playback_revision = app.playback_revision;
        }
        LightLock_Unlock(&app.lock);
        if (selected >= count) selected = count ? count - 1 : 0;
        if (keys & KEY_SELECT) { settings_open = !settings_open; shoulder = 0; }
        if (settings_open) {
            if ((keys & KEY_UP) && setting_selected > 0) --setting_selected;
            if ((keys & KEY_DOWN) && setting_selected < 2) ++setting_selected;
            if (keys & (KEY_A | KEY_LEFT | KEY_RIGHT)) {
                LightLock_Lock(&app.lock);
                if (setting_selected == 0) app.high_quality = !app.high_quality;
                else if (setting_selected == 1) app.shuffle = !app.shuffle;
                else app.repeat_one = !app.repeat_one;
                LightLock_Unlock(&app.lock);
            }
            if (keys & KEY_Y) app_toggle_pause(&app);
            if (keys & KEY_B) settings_open = false;
            draw(top, bottom, selected, false, settings_open, setting_selected);
            continue;
        }
        if ((keys & KEY_DOWN) && selected + 1 < count) ++selected;
        if ((keys & KEY_UP) && selected > 0) --selected;
        if ((keys & KEY_RIGHT) && count) selected = selected + LIST_ROWS < count ? selected + LIST_ROWS : count - 1;
        if (keys & KEY_LEFT) selected = selected >= LIST_ROWS ? selected - LIST_ROWS : 0;
        if ((keys & KEY_TOUCH) && worker && !busy) {
            touchPosition touch;
            hidTouchRead(&touch);
            if (touch.py >= LIST_Y && touch.py < LIST_Y + LIST_ROWS * LIST_ROW_HEIGHT) {
                int tapped = selected / LIST_ROWS * LIST_ROWS + (touch.py - LIST_Y) / LIST_ROW_HEIGHT;
                if (tapped < count) { selected = tapped; app_request(&app, playlist_view ? -4 - selected : selected); }
            }
        }
        if ((keys & KEY_A) && worker && count) {
            if (playlist_view) app_request(&app, -4 - selected);
            else app_request(&app, selected);
        }
        if ((keys & KEY_X) && worker) app_request(&app, playlist_view ? -2 : -3);
        u64 now_ms = osGetTime();
        if (keys & KEY_Y) app_toggle_pause(&app);
        if (keys & (KEY_L | KEY_R)) {
            shoulder = keys & KEY_L ? KEY_L : KEY_R;
            shoulder_started = shoulder_repeat = now_ms;
            shoulder_seeking = false;
        }
        if (shoulder && (held & shoulder) && now_ms - shoulder_started >= 600) {
            if (!shoulder_seeking || now_ms - shoulder_repeat >= 600) {
                if (playing >= 0 && !playlist_view)
                    atomic_fetch_add(&app.seek_seconds, shoulder == KEY_L ? -10 : 10);
                shoulder_seeking = true;
                shoulder_repeat = now_ms;
            }
        }
        if (shoulder && (released & shoulder)) {
            if (!shoulder_seeking) track_key = shoulder;
            shoulder = 0;
        }
        if (track_key && worker && count && !playlist_view) {
            LightLock_Lock(&app.lock);
            int base = app.playing >= 0 ? app.playing : selected;
            bool previous = track_key == KEY_L;
            int next = ym_track_next(app.tracks, app.count, base, previous ? -1 : 1,
                                     !previous && app.shuffle, (unsigned)rand());
            LightLock_Unlock(&app.lock);
            if (next >= 0) { selected = next; app_request(&app, next); }
            else app_status(&app, "Нет доступных треков");
        }
        if (keys & KEY_B) {
            if (playlist_view) {
                app_request(&app, -1);
                LightLock_Lock(&app.lock); app.playlist_view = false; ++app.library_revision; LightLock_Unlock(&app.lock);
            }
            else { app_request(&app, -1); app_status(&app, "Остановлено"); }
        }
        draw(top, bottom, selected, false, settings_open, setting_selected);
    }
    atomic_store(&app.quitting, true);
    if (lid_closed) {
        if (lcd_ready) GSPLCD_PowerOnAllBacklights();
        GSPGPU_SetLcdForceBlack(0);
    }
    if (lcd_ready) gspLcdExit();
    aptSetSleepAllowed(old_sleep_allowed);
    if (worker) {
        while (R_FAILED(threadJoin(worker, 0))) {
            /* Keep exit responsive while curl finishes/cancels its request. */
            draw(top, bottom, selected, true, false, 0);
        }
        threadFree(worker);
    }
    if (app.audio_ready) ndspExit();
    if (battery_ready) ptmuExit();
    if (curl_ready) curl_global_cleanup();
    if (soc_ready) socExit();
    free(soc_buffer);
    free(app.tracks); free(app.playlists); free(app.cover_pixels);
    C3D_FrameSync();
    if (cover_texture_ready) C3D_TexDelete(&cover_texture);
    C2D_TextBufDelete(text_buffer);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
