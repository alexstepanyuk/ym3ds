#include "app.h"
#include "net.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_status(App *app, const char *format, ...) {
    LightLock_Lock(&app->lock);
    va_list args;
    va_start(args, format);
    vsnprintf(app->status, sizeof(app->status), format, args);
    va_end(args);
    LightLock_Unlock(&app->lock);
}

bool app_cancelled(App *app, int gen) {
    return atomic_load(&app->quitting) || atomic_load(&app->generation) != gen;
}

void app_request(App *app, int job) {
    LightLock_Lock(&app->lock);
    app->job = job;
    atomic_fetch_add(&app->generation, 1);
    atomic_store(&app->paused, false);
    LightLock_Unlock(&app->lock);
}

static int load_token(char *token, size_t cap) {
    char text[1024];
    FILE *f = fopen(YM_TOKEN_FILE, "rb");
    if (!f) return -1;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    int too_long = fgetc(f) != EOF || ferror(f);
    fclose(f);
    text[n] = 0;
    int rc = too_long ? -1 : ym_token_parse(text, token, cap);
    memset(text, 0, sizeof(text));
    return rc;
}

static int library_load(App *app, int gen, const char *token) {
    char *json = NULL, uid[YM_ID_SIZE], path[128];
    app_status(app, "Проверяю аккаунт…");
    if (net_json(app, gen, "/account/status", token, NULL, &json)) return -1;
    int rc = ym_account_parse(json, uid);
    free(json);
    if (rc) { app_status(app, "Неизвестный ответ аккаунта"); return -1; }
    snprintf(path, sizeof(path), "/users/%s/likes/tracks", uid);
    app_status(app, "Загружаю любимые треки…");
    if (net_json(app, gen, path, token, NULL, &json)) return -1;
    YmTrack *tracks = calloc(YM_MAX_TRACKS, sizeof(*tracks));
    if (!tracks) { free(json); app_status(app, "Не хватает памяти"); return -1; }
    int count = ym_likes_parse(json, tracks, YM_MAX_TRACKS);
    free(json);
    if (count < 0) { free(tracks); app_status(app, "Неизвестный ответ списка треков"); return -1; }
    for (int start = 0; start < count; start += 25) {
        char post[25 * YM_ID_SIZE + 32] = "track-ids=";
        int end = start + 25 < count ? start + 25 : count;
        for (int i = start; i < end; ++i) {
            if (i != start) strcat(post, ",");
            strcat(post, tracks[i].id);
        }
        if (net_json(app, gen, "/tracks", token, post, &json)) { free(tracks); return -1; }
        rc = ym_tracks_parse(json, tracks + start, (size_t)(end - start));
        free(json);
        if (rc < 0) { free(tracks); app_status(app, "Неизвестный ответ названий треков"); return -1; }
    }
    LightLock_Lock(&app->lock);
    if (!app_cancelled(app, gen)) {
        memcpy(app->tracks, tracks, (size_t)count * sizeof(*tracks));
        app->count = count;
    }
    LightLock_Unlock(&app->lock);
    free(tracks);
    if (app_cancelled(app, gen)) return -1;
    app_status(app, count ? "Готово: %d треков (максимум 100)" : "В любимом пока нет треков", count);
    return 0;
}

static void play(App *app, int gen, int index, const char *token) {
    YmTrack track;
    LightLock_Lock(&app->lock);
    if (index < 0 || index >= app->count) { LightLock_Unlock(&app->lock); return; }
    track = app->tracks[index];
    bool ready = app->audio_ready;
    LightLock_Unlock(&app->lock);
    if (!ready) { app_status(app, "Нет прошивки DSP: нужен dspfirm.cdc"); return; }
    if (!track.available) { app_status(app, "Этот трек недоступен"); return; }
    char path[512], url[2048], *json = NULL;
    if (net_file_info_path(track.id, path, sizeof(path))) {
        app_status(app, "Проверь дату и время консоли"); return;
    }
    app_status(app, "Получаю MP3…");
    if (net_json(app, gen, path, token, NULL, &json)) return;
    int rc = ym_download_parse(json, url, sizeof(url));
    free(json);
    if (rc) { app_status(app, "API не вернул открытый MP3/raw"); return; }
    app_status(app, "Загружаю трек на SD…");
    if (net_download(app, gen, url)) return;
    if (app_cancelled(app, gen)) return;
    LightLock_Lock(&app->lock);
    app->playing = index;
    LightLock_Unlock(&app->lock);
    app_status(app, "Воспроизведение");
    rc = player_play(app, gen);
    if (!app_cancelled(app, gen))
        app_status(app, rc ? "Ошибка декодирования MP3" : "Трек завершён");
}

void app_worker(void *arg) {
    App *app = arg;
    while (!atomic_load(&app->quitting)) {
        LightLock_Lock(&app->lock);
        int job = app->job;
        int gen = atomic_load(&app->generation);
        app->job = -1;
        if (job != -1) {
            app->loading = true; app->downloaded_kb = 0;
            app->position_ms = 0; app->sample_rate = 0; app->bitrate_kbps = 0;
        }
        LightLock_Unlock(&app->lock);
        if (job == -1) { svcSleepThread(10000000); continue; }
        char token[512] = {0};
        if (load_token(token, sizeof(token))) app_status(app, "Заполни config/token.txt на SD");
        else if (job == -2) library_load(app, gen, token);
        else play(app, gen, job, token);
        memset(token, 0, sizeof(token));
        LightLock_Lock(&app->lock);
        app->loading = false;
        app->playing = -1;
        LightLock_Unlock(&app->lock);
    }
}
