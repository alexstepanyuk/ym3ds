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
    if (app->audio_ready) ndspChnSetPaused(0, false);
    LightLock_Unlock(&app->lock);
}

void app_toggle_pause(App *app) {
    /* Send the DSP command from the UI immediately, even when the decoder
     * is busy reading SD data. The app lock also protects channel reset. */
    LightLock_Lock(&app->lock);
    if (app->audio_ready && app->playing >= 0) {
        bool paused = !atomic_load(&app->paused);
        atomic_store(&app->paused, paused);
        ndspChnSetPaused(0, paused);
    }
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

typedef struct {
    App *app;
    int generation, result;
    const char *url;
    Mp3Stream stream;
} Download;

static void download_worker(void *user) {
    Download *d = user;
    d->result = net_download(d->app, d->generation, d->url, &d->stream);
    if (d->result && !atomic_load(&d->stream.stop)) atomic_store(&d->stream.failed, true);
    atomic_store(&d->stream.done, true);
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
    Download download = {.app = app, .generation = gen, .url = url};
    mp3_stream_init(&download.stream);
    /* Truncate before starting either reader or writer, so an old .part
     * can never be mistaken for the newly selected track. */
    FILE *empty = fopen(YM_AUDIO_FILE ".part", "wb");
    if (!empty) { app_status(app, "Не удалось открыть кэш на SD"); return; }
    if (fclose(empty)) { app_status(app, "Ошибка записи на SD"); return; }
    s32 priority = 0x31;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    app_status(app, "Буферизация: загружаю начало…");
    Thread downloader = threadCreate(download_worker, &download, 128 * 1024, priority + 1, 0, false);
    if (!downloader) { remove(YM_AUDIO_FILE ".part"); app_status(app, "Не удалось запустить загрузку"); return; }
    LightLock_Lock(&app->lock);
    app->playing = index;
    LightLock_Unlock(&app->lock);
    /* About 2.7 s at 192 kbit/s. Small files start once fully downloaded. */
    while (atomic_load(&download.stream.available) < 64 * 1024 &&
           !atomic_load(&download.stream.done) && !app_cancelled(app, gen))
        svcSleepThread(2000000);
    rc = -1;
    if (!app_cancelled(app, gen) && !atomic_load(&download.stream.failed)) {
        LightLock_Lock(&app->lock);
        if (!atomic_load(&download.stream.failed)) strcpy(app->status, "Воспроизведение");
        LightLock_Unlock(&app->lock);
        rc = player_play(app, gen, &download.stream);
    }
    /* Reader is closed now. Stop and join writer before touching its file
     * or returning (Download and URL live on this stack). */
    atomic_store(&download.stream.stop, true);
    threadJoin(downloader, U64_MAX);
    threadFree(downloader);
    if (!download.result && !app_cancelled(app, gen)) {
        remove(YM_AUDIO_FILE);
        if (rename(YM_AUDIO_FILE ".part", YM_AUDIO_FILE)) {
            app_status(app, "Ошибка сохранения кэша"); return;
        }
    } else remove(YM_AUDIO_FILE ".part");
    if (!app_cancelled(app, gen) && !atomic_load(&download.stream.failed))
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
            app->download_total_kb = 0; app->download_complete = false;
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
