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

static void draw(C3D_RenderTarget *top, C3D_RenderTarget *bottom, int selected, bool exiting) {
    YmTrack current = {0}, rows[LIST_ROWS] = {0};
    char status[256], label[64];
    int count, playing, start;
    unsigned kb;
    bool loading;
    unsigned position;
    int rate, bitrate;
    LightLock_Lock(&app.lock);
    count = app.count;
    playing = app.playing;
    loading = app.loading;
    kb = app.downloaded_kb;
    position = app.position_ms;
    rate = app.sample_rate;
    bitrate = app.bitrate_kbps;
    strcpy(status, app.status);
    start = selected / LIST_ROWS * LIST_ROWS;
    for (int i = 0; i < LIST_ROWS && start + i < count; ++i) rows[i] = app.tracks[start + i];
    if (playing >= 0 && playing < count) current = app.tracks[playing];
    else if (selected < count) current = app.tracks[selected];
    LightLock_Unlock(&app.lock);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
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
    /* Neutral record placeholder until real album artwork is supported. */
    C2D_DrawRectSolid(12, 63, 0, 132, 132, C2D_Color32(49, 49, 49, 255));
    C2D_DrawCircleSolid(78, 129, 0, 49, C2D_Color32(28, 28, 28, 255));
    C2D_DrawCircleSolid(78, 129, 0, 31, C2D_Color32(44, 44, 44, 255));
    C2D_DrawCircleSolid(78, 129, 0, 15, accent);
    C2D_DrawCircleSolid(78, 129, 0, 3, background);
    text("MP3", 159, 62, 0.36f, 48, accent, false);
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
    if (progress > 0) C2D_DrawRectSolid(159, 160, 0, 229 * progress, 4, accent);
    if (rate) snprintf(label, sizeof(label), "%.1f кГц", rate / 1000.0);
    else strcpy(label, "MP3 / SD");
    text(label, 159, 173, 0.40f, 100, muted, false);
    if (bitrate) {
        snprintf(label, sizeof(label), "%d кбит/с", bitrate);
        text(label, 287, 173, 0.40f, 101, muted, false);
    }
    text(exiting ? "Завершаю операцию…" : status, 12, 207, 0.40f, 376, muted, false);
    if (loading && playing < 0 && kb) {
        snprintf(label, sizeof(label), "Загружено: %u КБ", kb);
        text(label, 159, 190, 0.35f, 229, accent, false);
    } else if (playing >= 0) {
        text(atomic_load(&app.paused) ? "Пауза" : "Воспроизведение", 159, 190, 0.35f, 229, accent, false);
    }

    C2D_TargetClear(bottom, background);
    C2D_SceneBegin(bottom);
    snprintf(label, sizeof(label), "Мне нравится       %d/%d", count ? selected + 1 : 0, count);
    text(label, 12, 8, 0.53f, 296, white, false);
    C2D_DrawRectSolid(12, 30, 0, 296, 1, muted);
    for (int i = 0; i < LIST_ROWS && start + i < count; ++i) {
        float y = LIST_Y + i * LIST_ROW_HEIGHT;
        if (start + i == selected) C2D_DrawRectSolid(8, y + 2, 0, 3, 23, accent);
        text(rows[i].title, 18, y, 0.44f, 290, rows[i].available ? white : muted, false);
        text(rows[i].artist, 18, y + 15, 0.38f, 290, muted, false);
    }
    if (!count) text("X — загрузить список\nТокен: /3ds/ym3ds/config/token.txt", 12, 58, 0.45f, 296, muted, true);
    C2D_DrawRectSolid(12, 208, 0, 296, 1, muted);
    text("A: играть  Y: пауза  B: стоп", 12, 211, 0.40f, 296, white, false);
    text("X: обновить  START: выход", 12, 226, 0.37f, 296, muted, false);
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
    while (aptMainLoop()) {
        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & KEY_START) break;
        LightLock_Lock(&app.lock);
        int count = app.count;
        bool busy = app.loading;
        LightLock_Unlock(&app.lock);
        if (selected >= count) selected = count ? count - 1 : 0;
        if ((keys & KEY_DOWN) && selected + 1 < count) ++selected;
        if ((keys & KEY_UP) && selected > 0) --selected;
        if ((keys & KEY_RIGHT) && count) selected = selected + LIST_ROWS < count ? selected + LIST_ROWS : count - 1;
        if (keys & KEY_LEFT) selected = selected >= LIST_ROWS ? selected - LIST_ROWS : 0;
        if ((keys & KEY_TOUCH) && worker && !busy) {
            touchPosition touch;
            hidTouchRead(&touch);
            if (touch.py >= LIST_Y && touch.py < LIST_Y + LIST_ROWS * LIST_ROW_HEIGHT) {
                int tapped = selected / LIST_ROWS * LIST_ROWS + (touch.py - LIST_Y) / LIST_ROW_HEIGHT;
                if (tapped < count) { selected = tapped; app_request(&app, selected); }
            }
        }
        if ((keys & KEY_A) && worker && count) app_request(&app, selected);
        if ((keys & KEY_X) && worker) app_request(&app, -2);
        if (keys & KEY_Y) atomic_store(&app.paused, !atomic_load(&app.paused));
        if (keys & KEY_B) { app_request(&app, -1); app_status(&app, "Остановлено"); }
        draw(top, bottom, selected, false);
    }
    atomic_store(&app.quitting, true);
    if (worker) {
        while (R_FAILED(threadJoin(worker, 0))) {
            /* Keep exit responsive while curl finishes/cancels its request. */
            draw(top, bottom, selected, true);
        }
        threadFree(worker);
    }
    if (app.audio_ready) ndspExit();
    if (battery_ready) ptmuExit();
    if (curl_ready) curl_global_cleanup();
    if (soc_ready) socExit();
    free(soc_buffer);
    C2D_TextBufDelete(text_buffer);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
