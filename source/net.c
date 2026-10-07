#include "net.h"
#include "replay.h"
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define JSON_LIMIT (2 * 1024 * 1024)
#define AUDIO_LIMIT (32 * 1024 * 1024)
/* Accessed only by the network worker. Signed requests need UTC, while the
 * console RTC may contain local time. Learn UTC from verified HTTPS replies. */
static time_t server_offset;

typedef struct {
    App *app;
    int generation;
    char *data;
    size_t size;
    FILE *file;
    const char *stage;
    time_t server_date;
    Mp3Stream *stream;
    size_t limit;
    bool silent;
    FILE *prefix;
    size_t prefix_size, verified, received;
} Transfer;

static size_t read_header(char *data, size_t size, size_t count, void *user) {
    if (size && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    Transfer *t = user;
    if (n > 6 && n < 80 && !strncasecmp(data, "Date:", 5)) {
        char date[80];
        memcpy(date, data + 5, n - 5);
        date[n - 5] = 0;
        time_t parsed = curl_getdate(date, NULL);
        if (parsed > 1700000000) t->server_date = parsed;
    }
    return n;
}

static int progress(void *user, curl_off_t total, curl_off_t now,
                    curl_off_t upload_total, curl_off_t upload_now) {
    (void)now; (void)upload_total; (void)upload_now;
    Transfer *t = user;
    if (t->file) {
        LightLock_Lock(&t->app->lock);
        t->app->downloaded_kb = (unsigned)(t->size / 1024);
        t->app->download_total_kb = total > 0 && total <= AUDIO_LIMIT ? (unsigned)((total + 1023) / 1024) : 0;
        LightLock_Unlock(&t->app->lock);
    }
    return app_cancelled(t->app, t->generation) || (t->stream && atomic_load(&t->stream->stop));
}

static size_t write_body(char *data, size_t size, size_t n, void *user) {
    Transfer *t = user;
    if (size && n > SIZE_MAX / size) return 0;
    size_t bytes = size * n;
    size_t limit = t->limit ? t->limit : t->file ? AUDIO_LIMIT : JSON_LIMIT;
    if (app_cancelled(t->app, t->generation) ||
        (t->stream && atomic_load(&t->stream->stop))) return 0;
    if (t->file) {
        if (!mp3_replay_write(t->file, t->prefix, t->prefix_size, &t->verified,
                              data, bytes, &t->size, limit)) return 0;
        t->received += bytes;
        if (t->stream) atomic_store(&t->stream->available, (unsigned)t->size);
        return bytes;
    }
    if (bytes > limit - t->size) return 0;
    char *next = realloc(t->data, t->size + bytes + 1);
    if (!next) return 0;
    t->data = next;
    memcpy(next + t->size, data, bytes);
    t->size += bytes;
    next[t->size] = 0;
    return bytes;
}

static CURL *request(Transfer *t, const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO, YM_CA_FILE);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, t->silent ? 4L : 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, t->silent ? 8L : t->file ? 300L : 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "YM3DS/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, t);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    return curl;
}

static bool transfer_stopped(Transfer *t) {
    return app_cancelled(t->app, t->generation) || (t->stream && atomic_load(&t->stream->stop));
}

static bool retry_wait(Transfer *t, unsigned delay_ms) {
    /* A closed lid may disconnect Wi-Fi even though apt sleep is denied.
     * Preserve the producer and pause state until the console is reopened. */
    while (atomic_load(&t->app->lid_closed)) {
        if (transfer_stopped(t)) return false;
        svcSleepThread(50000000);
    }
    for (unsigned elapsed = 0; elapsed < delay_ms; elapsed += 50) {
        if (transfer_stopped(t)) return false;
        svcSleepThread(50000000);
    }
    return !transfer_stopped(t);
}

static int perform(CURL *curl, Transfer *t) {
    CURLcode rc = CURLE_OK;
    long status = 0;
    for (unsigned attempt = 0;; ++attempt) {
        if (transfer_stopped(t)) return -1;
        /* A lid/network transition can invalidate curl's internal multi
         * polling state. Retries use a new easy/multi session, retaining
         * only request options and the verified file prefix. */
        CURL *active = attempt ? curl_easy_duphandle(curl) : curl;
        if (!active) { rc = CURLE_OUT_OF_MEMORY; break; }
        rc = curl_easy_perform(active);
        curl_easy_getinfo(active, CURLINFO_RESPONSE_CODE, &status);
        if (active != curl) curl_easy_cleanup(active);
        if (transfer_stopped(t)) return -1;
        bool transient = rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT ||
            rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_RECV_ERROR ||
            rc == CURLE_SEND_ERROR || rc == CURLE_PARTIAL_FILE || rc == CURLE_GOT_NOTHING ||
            rc == CURLE_BAD_FUNCTION_ARGUMENT;
        if (t->silent || !transient || attempt >= 5) break;
        app_status(t->app, "Восстанавливаю соединение… %u/5", attempt + 1);
        if (!retry_wait(t, attempt < 3 ? 1000U << attempt : 8000U)) return -1;
        if (t->file) {
            if (t->prefix) fclose(t->prefix);
            t->prefix = fopen(YM_AUDIO_FILE ".part", "rb");
            if (!t->prefix) { rc = CURLE_READ_ERROR; break; }
            t->prefix_size = t->size; t->verified = 0; t->received = 0;
        } else {
            free(t->data); t->data = NULL; t->size = 0; t->server_date = 0;
        }
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    }
    if (app_cancelled(t->app, t->generation) || (t->stream && atomic_load(&t->stream->stop))) return -1;
    if (t->silent) return rc == CURLE_OK && status == 200 && t->size ? 0 : -1;
    if (rc == CURLE_HTTP_RETURNED_ERROR) {
        if (t->stream) atomic_store(&t->stream->failed, true);
        app_status(t->app, "%s: HTTP %ld", t->stage, status); return -1;
    }
    if (rc != CURLE_OK) {
        if (t->stream) atomic_store(&t->stream->failed, true);
        const char *message = "Ошибка сетевого запроса";
        switch (rc) {
            case CURLE_COULDNT_RESOLVE_HOST: message = "Не удалось найти сервер (DNS)"; break;
            case CURLE_COULDNT_CONNECT: message = "Нет соединения с сервером"; break;
            case CURLE_OPERATION_TIMEDOUT: message = "Превышено время ожидания"; break;
            case CURLE_PEER_FAILED_VERIFICATION: message = "Проверь дату консоли и файл cacert.pem"; break;
            case CURLE_SSL_CACERT_BADFILE: message = "Не удалось прочитать cacert.pem"; break;
            case CURLE_WRITE_ERROR: message = "Ошибка записи или превышен лимит данных"; break;
            case CURLE_BAD_FUNCTION_ARGUMENT: message = "Ошибка аргумента или состояния curl"; break;
            default: break;
        }
        app_status(t->app, "%s (код %d)", message, (int)rc);
        return -1;
    }
    if (!t->file && t->server_date) server_offset = t->server_date - time(NULL);
    if (status != 200 || !t->size || (t->file && (t->verified != t->prefix_size || t->received != t->size))) {
        if (t->stream) atomic_store(&t->stream->failed, true);
        app_status(t->app, "%s: HTTP %ld", t->stage, status);
        return -1;
    }
    return 0;
}

int net_json(App *app, int gen, const char *path, const char *token,
             const char *post, char **out) {
    *out = NULL;
    char url[1024], auth[512];
    if (*path != '/' || strlen(path) + 29 >= sizeof(url) || strlen(token) > 450) return -1;
    snprintf(url, sizeof(url), "https://api.music.yandex.net%s", path);
    snprintf(auth, sizeof(auth), "Authorization: OAuth %s", token);
    Transfer t = {.app = app, .generation = gen,
        .stage = !strncmp(path, "/get-file-info?", 15) ? "Получение MP3" : "API библиотеки"};
    CURL *curl = request(&t, url);
    if (!curl) { app_status(app, "Не удалось создать HTTP-запрос"); return -1; }
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    if (!headers) { curl_easy_cleanup(curl); return -1; }
    struct curl_slist *next = curl_slist_append(headers,
        "X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.23.2");
    if (!next) { curl_slist_free_all(headers); curl_easy_cleanup(curl); return -1; }
    headers = next;
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    /* Do not forward credentials through redirects. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    if (post) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
    int rc = perform(curl, &t);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    memset(auth, 0, sizeof(auth));
    if (rc) free(t.data);
    else *out = t.data;
    return rc;
}

int net_cover(App *app, int gen, const char *uri, unsigned char **bytes, size_t *size) {
    *bytes = NULL; *size = 0;
    char url[512];
    if (ym_cover_url(uri, url, sizeof(url))) return -1;
    Transfer t = {.app = app, .generation = gen, .stage = "Обложка", .limit = 512 * 1024, .silent = true};
    CURL *curl = request(&t, url);
    if (!curl) return -1;
    /* No redirects and no authorization headers for artwork. */
    int rc = perform(curl, &t);
    curl_easy_cleanup(curl);
    if (rc) free(t.data);
    else { *bytes = (unsigned char *)t.data; *size = t.size; }
    return rc;
}

int net_download(App *app, int gen, const char *url, Mp3Stream *stream) {
    if (strncmp(url, "https://", 8)) return -1;
    const char *temp = YM_AUDIO_FILE ".part";
    Transfer t = {.app = app, .generation = gen, .file = fopen(temp, "wb"), .stage = "Скачивание MP3", .stream = stream};
    if (!t.file) { atomic_store(&stream->failed, true); app_status(app, "Не удалось открыть кэш на SD"); return -1; }
    CURL *curl = request(&t, url);
    int rc = -1;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        rc = perform(curl, &t);
        curl_easy_cleanup(curl);
    } else { atomic_store(&stream->failed, true); app_status(app, "Не удалось создать HTTP-запрос"); }
    if (fclose(t.file)) { rc = -1; atomic_store(&stream->failed, true); app_status(app, "Ошибка записи на SD"); }
    if (t.prefix) fclose(t.prefix);
    if (!rc && app_cancelled(app, gen)) rc = -1;
    if (!rc) {
        LightLock_Lock(&app->lock);
        app->download_complete = true;
        app->downloaded_kb = (unsigned)((t.size + 1023) / 1024);
        app->download_total_kb = app->downloaded_kb;
        LightLock_Unlock(&app->lock);
    }
    return rc;
}

/* Protocol adapted from amdray/yandex_music_psp, ym_api_download.c (MIT).
 * This is a client protocol key, not a user's OAuth token. */
int net_file_info_path(const char *id, bool high_quality, char *out, size_t cap) {
    static const char secret[] = "kzqU4XhfCaY6B6JTHODeq5";
    if (!ym_id_valid(id)) return -1;
    time_t now = time(NULL) + server_offset;
    if (now < 1700000000) return -1;
    char input[128], encoded[128], b64[64];
    unsigned char digest[32];
    const char *quality = high_quality ? "hq" : "nq";
    int n = snprintf(input, sizeof(input), "%lld%s%smp3raw", (long long)now, id, quality);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || n < 0 || (size_t)n >= sizeof(input) ||
        mbedtls_md_hmac(md, (const unsigned char *)secret, strlen(secret),
                        (const unsigned char *)input, (size_t)n, digest)) return -1;
    size_t len = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &len, digest, sizeof(digest))) return -1;
    while (len && b64[len - 1] == '=') --len;
    size_t used = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)b64[i];
        if (c == '+' || c == '/') {
            snprintf(encoded + used, sizeof(encoded) - used, "%%%02X", c);
            used += 3;
        } else encoded[used++] = (char)c;
    }
    encoded[used] = 0;
    n = snprintf(out, cap, "/get-file-info?ts=%lld&trackId=%s&quality=%s&codecs=mp3&sign=%s&transports=raw",
                 (long long)now, id, quality, encoded);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}
