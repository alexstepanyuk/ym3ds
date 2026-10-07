#include "app.h"
#include <citro2d.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static App app;
static C2D_TextBuf text_buffer;
static u32 white, muted, accent, background;

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
    YmTrack current = {0}, rows[6] = {0};
    char status[256], label[64];
    int count, playing, start;
    unsigned kb;
    bool loading;
    LightLock_Lock(&app.lock);
    count = app.count;
    playing = app.playing;
    loading = app.loading;
    kb = app.downloaded_kb;
    strcpy(status, app.status);
    start = selected / 6 * 6;
    for (int i = 0; i < 6 && start + i < count; ++i) rows[i] = app.tracks[start + i];
    if (playing >= 0 && playing < count) current = app.tracks[playing];
    else if (selected < count) current = app.tracks[selected];
    LightLock_Unlock(&app.lock);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(text_buffer);
    C2D_TargetClear(top, background);
    C2D_SceneBegin(top);
    text("YM3DS", 18, 12, 0.85f, 364, accent, false);
    text(current.title[0] ? current.title : "Яндекс Музыка · прототип", 18, 54, 0.65f, 364, white, true);
    text(current.artist, 18, 120, 0.50f, 364, muted, false);
    text(exiting ? "Завершаю операцию…" : status, 18, 154, 0.48f, 364, muted, true);
    if (loading && playing < 0 && kb) {
        snprintf(label, sizeof(label), "Загружено: %u КБ", kb);
        text(label, 18, 210, 0.45f, 364, accent, false);
    } else if (playing >= 0) {
        text(atomic_load(&app.paused) ? "Пауза" : "Играет", 18, 210, 0.45f, 364, accent, false);
    }

    C2D_TargetClear(bottom, background);
    C2D_SceneBegin(bottom);
    snprintf(label, sizeof(label), "Любимое · %d/%d", count ? selected + 1 : 0, count);
    text(label, 12, 8, 0.6f, 296, white, false);
    for (int i = 0; i < 6 && start + i < count; ++i) {
        float y = 38 + i * 28;
        if (start + i == selected) C2D_DrawRectSolid(6, y, 0, 308, 27, C2D_Color32(45, 43, 24, 255));
        text(rows[i].title, 12, y + 3, 0.46f, 296, rows[i].available ? white : muted, false);
    }
    if (!count) text("X — загрузить список\nТокен: /3ds/ym3ds/config/token.txt", 12, 58, 0.45f, 296, muted, true);
    text("A: играть  Y: пауза  B: стоп", 12, 211, 0.40f, 296, accent, false);
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
    accent = C2D_Color32(255, 215, 45, 255);
    background = C2D_Color32(18, 20, 25, 255);
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
        if ((keys & KEY_RIGHT) && count) selected = selected + 6 < count ? selected + 6 : count - 1;
        if (keys & KEY_LEFT) selected = selected >= 6 ? selected - 6 : 0;
        if ((keys & KEY_TOUCH) && worker && !busy) {
            touchPosition touch;
            hidTouchRead(&touch);
            if (touch.py >= 38 && touch.py < 206) {
                int tapped = selected / 6 * 6 + (touch.py - 38) / 28;
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
    if (curl_ready) curl_global_cleanup();
    if (soc_ready) socExit();
    free(soc_buffer);
    C2D_TextBufDelete(text_buffer);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
