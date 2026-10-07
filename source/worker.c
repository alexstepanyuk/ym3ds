#include "app.h"
#include "net.h"
#include "cover.h"
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
    atomic_store(&app->seek_seconds, 0);
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

static int account_uid(App *app, int gen, const char *token, char uid[YM_ID_SIZE]) {
    char *json = NULL;
    if (net_json(app, gen, "/account/status", token, NULL, &json)) return -1;
    int rc = ym_account_parse(json, uid);
    free(json);
    if (rc) { app_status(app, "Неизвестный ответ аккаунта"); return -1; }
    LightLock_Lock(&app->lock);
    if (!app_cancelled(app, gen)) strcpy(app->uid, uid);
    LightLock_Unlock(&app->lock);
    return rc;
}

static int hydrate(App *app, int gen, int start, const char *token) {
    YmTrack batch[25];
    LightLock_Lock(&app->lock);
    int n = app->count - start;
    if (n > 25) n = 25;
    if (start < 0 || n <= 0 || app_cancelled(app, gen)) { LightLock_Unlock(&app->lock); return -1; }
    memcpy(batch, app->tracks + start, (size_t)n * sizeof(*batch));
    LightLock_Unlock(&app->lock);
    char post[25 * YM_ID_SIZE + 32] = "track-ids=", *json = NULL;
    for (int i = 0; i < n; ++i) {
        if (i) strcat(post, ",");
        strcat(post, batch[i].id);
    }
    if (net_json(app, gen, "/tracks", token, post, &json)) return -1;
    int rc = ym_tracks_parse(json, batch, (size_t)n);
    free(json);
    if (rc < 0) { app_status(app, "Неизвестный ответ названий треков"); return -1; }
    /* Missing metadata is unavailable, rather than retried forever. */
    for (int i = 0; i < n; ++i) batch[i].loaded = true;
    LightLock_Lock(&app->lock);
    if (!app_cancelled(app, gen)) memcpy(app->tracks + start, batch, (size_t)n * sizeof(*batch));
    LightLock_Unlock(&app->lock);
    return app_cancelled(app, gen) ? -1 : 0;
}

static int load_collection(App *app, int gen, const char *token, const char *path,
                           bool playlist, const char *title) {
    char *json = NULL;
    if (net_json(app, gen, path, token, NULL, &json)) return -1;
    YmTrack *tracks = NULL;
    int count = ym_track_list_parse(json, playlist, &tracks);
    free(json);
    if (count < 0) { app_status(app, "Не удалось прочитать список: формат или память"); return -1; }
    LightLock_Lock(&app->lock);
    if (app_cancelled(app, gen)) { LightLock_Unlock(&app->lock); free(tracks); return -1; }
    YmTrack *old = app->tracks;
    app->tracks = tracks; app->count = count; app->playing = -1;
    app->playlist_view = false; app->hydration_pending = count > 0;
    ym_text_copy(app->collection, sizeof(app->collection), title);
    ++app->library_revision;
    LightLock_Unlock(&app->lock);
    free(old);
    for (int start = 0; start < count; start += 25) {
        app_status(app, "Названия треков: %d/%d", start, count);
        if (hydrate(app, gen, start, token)) {
            LightLock_Lock(&app->lock);
            if (!app_cancelled(app, gen)) app->hydration_pending = false;
            LightLock_Unlock(&app->lock);
            return -1;
        }
    }
    LightLock_Lock(&app->lock);
    app->hydration_pending = false;
    LightLock_Unlock(&app->lock);
    app_status(app, count ? "Готово: %d треков" : "Список пуст", count);
    return 0;
}

static int library_load(App *app, int gen, const char *token) {
    char uid[YM_ID_SIZE], path[128];
    if (account_uid(app, gen, token, uid)) return -1;
    snprintf(path, sizeof(path), "/users/%s/likes/tracks", uid);
    return load_collection(app, gen, token, path, false, "Мне нравится");
}

static int playlists_load(App *app, int gen, const char *token) {
    char uid[YM_ID_SIZE], path[128], *json = NULL;
    if (account_uid(app, gen, token, uid)) return -1;
    snprintf(path, sizeof(path), "/users/%s/playlists/list", uid);
    if (net_json(app, gen, path, token, NULL, &json)) return -1;
    YmPlaylist *own = NULL, *liked = NULL;
    int n = ym_playlists_parse(json, false, uid, &own);
    free(json);
    if (n < 0) { app_status(app, "Не удалось прочитать плейлисты"); return -1; }
    snprintf(path, sizeof(path), "/users/%s/likes/playlists", uid);
    int m = 0;
    if (!net_json(app, gen, path, token, NULL, &json)) {
        m = ym_playlists_parse(json, true, uid, &liked);
        free(json);
        if (m < 0) m = 0;
    }
    YmPlaylist *list = calloc((size_t)n + (size_t)m + 1, sizeof(*list));
    if (!list) { free(own); free(liked); app_status(app, "Не хватает памяти для плейлистов"); return -1; }
    strcpy(list[0].title, "Мне нравится");
    if (n) memcpy(list + 1, own, (size_t)n * sizeof(*list));
    if (m) memcpy(list + 1 + n, liked, (size_t)m * sizeof(*list));
    free(own); free(liked);
    LightLock_Lock(&app->lock);
    if (app_cancelled(app, gen)) { LightLock_Unlock(&app->lock); free(list); return -1; }
    YmPlaylist *old = app->playlists;
    app->playlists = list; app->playlist_count = n + m + 1;
    app->playlist_view = true; ++app->library_revision;
    LightLock_Unlock(&app->lock);
    free(old);
    app_status(app, "A: открыть · X: любимые");
    return 0;
}

static void playlist_open(App *app, int gen, int index, const char *token) {
    YmPlaylist playlist;
    LightLock_Lock(&app->lock);
    if (index < 0 || index >= app->playlist_count) { LightLock_Unlock(&app->lock); return; }
    playlist = app->playlists[index];
    LightLock_Unlock(&app->lock);
    if (!index) { library_load(app, gen, token); return; }
    char path[160];
    snprintf(path, sizeof(path), "/users/%s/playlists/%s?rich-tracks=false", playlist.owner, playlist.kind);
    load_collection(app, gen, token, path, true, playlist.title);
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
    bool high_quality = app->high_quality;
    LightLock_Unlock(&app->lock);
    if (!track.loaded) {
        if (hydrate(app, gen, index / 25 * 25, token)) return;
        LightLock_Lock(&app->lock);
        track = app->tracks[index];
        LightLock_Unlock(&app->lock);
    }
    if (!ready) { app_status(app, "Нет прошивки DSP: нужен dspfirm.cdc"); return; }
    if (!track.available) { app_status(app, "Этот трек недоступен"); return; }
    char path[512], url[2048], *json = NULL;
    if (net_file_info_path(track.id, high_quality, path, sizeof(path))) {
        app_status(app, "Проверь дату и время консоли"); return;
    }
    app_status(app, "Получаю MP3…");
    if (net_json(app, gen, path, token, NULL, &json)) return;
    int rc = ym_download_parse(json, url, sizeof(url));
    free(json);
    if (rc) { app_status(app, "API не вернул открытый MP3/raw"); return; }
    unsigned char *image = NULL, *pixels = NULL;
    size_t image_size = 0;
    if (*track.cover && !net_cover(app, gen, track.cover, &image, &image_size))
        pixels = ym_cover_decode(image, image_size);
    free(image);
    LightLock_Lock(&app->lock);
    if (!app_cancelled(app, gen)) {
        free(app->cover_pixels); app->cover_pixels = pixels; pixels = NULL;
        strcpy(app->cover_id, track.id);
        ++app->cover_revision;
    }
    LightLock_Unlock(&app->lock);
    free(pixels);
    if (app_cancelled(app, gen)) return;
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
    ++app->playback_revision;
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
    /* Queue only a genuinely completed track. Check the generation and
     * pending job under the same lock used by manual controls, so an EOF
     * racing with B, Y, A or X cannot overwrite the user's request. */
    if (!rc && !download.result && !atomic_load(&download.stream.failed)) {
        LightLock_Lock(&app->lock);
        if (!app_cancelled(app, gen) && app->job == -1) {
            int next = app->repeat_one ? index :
                ym_track_next(app->tracks, app->count, index, 1, app->shuffle, (unsigned)rand());
            if (next >= 0 && next < app->count && (app->tracks[next].available || !app->tracks[next].loaded)) {
                app->job = next;
                atomic_fetch_add(&app->generation, 1);
            }
        }
        LightLock_Unlock(&app->lock);
    }
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
        if (job == -1) {
            int pending = -1;
            LightLock_Lock(&app->lock);
            if (app->hydration_pending && !app->playlist_view)
                for (int i = 0; i < app->count; ++i)
                    if (!app->tracks[i].loaded) { pending = i; break; }
            if (pending < 0 && !app->playlist_view) app->hydration_pending = false;
            LightLock_Unlock(&app->lock);
            if (pending >= 0) {
                char token[512] = {0};
                int rc = load_token(token, sizeof(token)) || hydrate(app, gen, pending, token);
                memset(token, 0, sizeof(token));
                if (rc) {
                    LightLock_Lock(&app->lock);
                    if (!app_cancelled(app, gen)) app->hydration_pending = false;
                    LightLock_Unlock(&app->lock);
                }
            } else svcSleepThread(10000000);
            continue;
        }
        char token[512] = {0};
        if (load_token(token, sizeof(token))) app_status(app, "Заполни config/token.txt на SD");
        else if (job == -2) library_load(app, gen, token);
        else if (job == -3) playlists_load(app, gen, token);
        else if (job <= -4) playlist_open(app, gen, -job - 4, token);
        else play(app, gen, job, token);
        memset(token, 0, sizeof(token));
        LightLock_Lock(&app->lock);
        app->loading = false;
        app->playing = -1;
        LightLock_Unlock(&app->lock);
    }
}
