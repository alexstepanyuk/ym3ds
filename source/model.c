#include "model.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

static cJSON *field(const cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static const char *string(const cJSON *o) {
    return cJSON_IsString(o) && o->valuestring ? o->valuestring : "";
}

void ym_text_copy(char *out, size_t cap, const char *s) {
    if (!cap) return;
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
    }
    /* Controls would otherwise affect rendering/logs. */
    for (size_t i = 0; i < n; ++i)
        out[i] = (unsigned char)s[i] < 32 ? ' ' : s[i];
    out[n] = 0;
}

bool ym_id_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= YM_ID_SIZE) return false;
    for (; *s; ++s) if (*s < '0' || *s > '9') return false;
    return true;
}

static int id_parse(const cJSON *o, char out[YM_ID_SIZE]) {
    if (cJSON_IsString(o)) {
        if (!ym_id_valid(string(o))) return -1;
        strcpy(out, string(o));
    } else if (cJSON_IsNumber(o)) {
        double v = o->valuedouble;
        if (v < 1 || v > 9007199254740991.0) return -1;
        snprintf(out, YM_ID_SIZE, "%.0f", v);
        double parsed;
        if (sscanf(out, "%lf", &parsed) != 1 || parsed != v) return -1;
    } else return -1;
    return 0;
}

int ym_token_parse(const char *text, char *out, size_t cap) {
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!text) return -1;
    while (isspace((unsigned char)*text)) ++text;
    if (strncmp(text, "YANDEX_TOKEN", 12)) return -1;
    text += 12;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text++ != '=') return -1;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text++ != '"') return -1;
    const char *end = strchr(text, '"');
    if (!end || end == text || (size_t)(end - text) >= cap) return -1;
    for (const char *p = text; p < end; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '\\') return -1;
    for (const char *p = end + 1; *p; ++p)
        if (!isspace((unsigned char)*p)) return -1;
    memcpy(out, text, (size_t)(end - text));
    out[end - text] = 0;
    return 0;
}

int ym_account_parse(const char *json, char uid[YM_ID_SIZE]) {
    uid[0] = 0;
    cJSON *root = cJSON_Parse(json);
    int rc = id_parse(field(field(field(root, "result"), "account"), "uid"), uid);
    cJSON_Delete(root);
    return rc;
}

int ym_likes_parse(const char *json, YmTrack *out, size_t cap) {
    cJSON *root = cJSON_Parse(json);
    cJSON *items = field(field(field(root, "result"), "library"), "tracks");
    if (!cJSON_IsArray(items)) { cJSON_Delete(root); return -1; }
    size_t n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (n == cap) break;
        YmTrack t = {0};
        if (id_parse(field(item, "id"), t.id)) continue;
        /* Metadata is resolved by /tracks, not guessed from like entries. */
        snprintf(t.title, sizeof(t.title), "Трек %s", t.id);
        out[n++] = t;
    }
    cJSON_Delete(root);
    return (int)n;
}

int ym_tracks_parse(const char *json, YmTrack *tracks, size_t count) {
    cJSON *root = cJSON_Parse(json);
    cJSON *items = field(root, "result");
    if (!cJSON_IsArray(items)) { cJSON_Delete(root); return -1; }
    int resolved = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        char id[YM_ID_SIZE];
        if (id_parse(field(item, "id"), id)) continue;
        for (size_t i = 0; i < count; ++i) {
            YmTrack *t = &tracks[i];
            if (strcmp(t->id, id)) continue;
            ym_text_copy(t->title, sizeof(t->title), string(field(item, "title")));
            cJSON *duration = field(item, "durationMs");
            t->duration_ms = cJSON_IsNumber(duration) && duration->valuedouble > 0 &&
                             duration->valuedouble < 86400000 ? (unsigned)duration->valuedouble : 0;
            t->artist[0] = 0;
            cJSON *artist;
            cJSON_ArrayForEach(artist, field(item, "artists")) {
                const char *name = string(field(artist, "name"));
                if (!*name) continue;
                char joined[YM_TEXT_SIZE * 2];
                snprintf(joined, sizeof(joined), "%s%s%s", t->artist, *t->artist ? ", " : "", name);
                ym_text_copy(t->artist, sizeof(t->artist), joined);
            }
            t->available = !cJSON_IsFalse(field(item, "available")) &&
                           !*string(field(item, "error"));
            t->loaded = true;
            const char *cover = string(field(item, "coverUri"));
            if (!*cover) cover = string(field(cJSON_GetArrayItem(field(item, "albums"), 0), "coverUri"));
            if (strlen(cover) < sizeof(t->cover)) strcpy(t->cover, cover);
            ++resolved;
        }
    }
    cJSON_Delete(root);
    return resolved;
}

int ym_track_list_parse(const char *json, bool playlist, YmTrack **out) {
    *out = NULL;
    cJSON *root = cJSON_Parse(json);
    cJSON *result = field(root, "result");
    if (playlist && cJSON_IsArray(result)) result = cJSON_GetArrayItem(result, 0);
    cJSON *items = playlist ? field(result, "tracks") : field(field(result, "library"), "tracks");
    if (!cJSON_IsArray(items)) { cJSON_Delete(root); return -1; }
    int count = cJSON_GetArraySize(items);
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof(YmTrack)) { cJSON_Delete(root); return -1; }
    YmTrack *tracks = count ? calloc((size_t)count, sizeof(*tracks)) : NULL;
    if (count && !tracks) { cJSON_Delete(root); return -1; }
    int used = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (id_parse(field(item, "id"), tracks[used].id)) continue;
        snprintf(tracks[used].title, sizeof(tracks[used].title), "Трек %s", tracks[used].id);
        ++used;
    }
    cJSON_Delete(root);
    *out = tracks;
    return used;
}

int ym_playlists_parse(const char *json, bool liked, const char *owner, YmPlaylist **out) {
    *out = NULL;
    cJSON *root = cJSON_Parse(json), *items = field(root, "result");
    if (!cJSON_IsArray(items)) { cJSON_Delete(root); return -1; }
    int count = cJSON_GetArraySize(items), used = 0;
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof(YmPlaylist)) { cJSON_Delete(root); return -1; }
    YmPlaylist *list = count ? calloc((size_t)count, sizeof(*list)) : NULL;
    if (count && !list) { cJSON_Delete(root); return -1; }
    cJSON *entry;
    cJSON_ArrayForEach(entry, items) {
        cJSON *item = liked ? field(entry, "playlist") : entry;
        YmPlaylist p = {0};
        if (id_parse(field(item, "kind"), p.kind)) continue;
        if (id_parse(field(field(item, "owner"), "uid"), p.owner) &&
            id_parse(field(item, "uid"), p.owner)) {
            if (liked || !ym_id_valid(owner)) continue;
            strcpy(p.owner, owner);
        }
        ym_text_copy(p.title, sizeof(p.title), string(field(item, "title")));
        cJSON *n = field(item, "trackCount");
        if (cJSON_IsNumber(n) && n->valuedouble >= 0 && n->valuedouble < UINT_MAX) p.track_count = (unsigned)n->valuedouble;
        list[used++] = p;
    }
    cJSON_Delete(root);
    *out = list;
    return used;
}

int ym_cover_url(const char *uri, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    if (!uri || !*uri) return -1;
    const char *host = uri;
    if (!strncmp(host, "https://", 8)) host += 8;
    else if (!strncmp(host, "http://", 7)) host += 7;
    if (strncmp(host, "avatars.yandex.net/", 19) && strncmp(host, "avatars.mds.yandex.net/", 23)) return -1;
    for (const char *p = host; *p; ++p) if ((unsigned char)*p <= 32 || *p == '\\') return -1;
    const char *slot = strstr(host, "%%");
    int n = slot ? snprintf(out, cap, "https://%.*s100x100%s", (int)(slot - host), host, slot + 2) :
                   snprintf(out, cap, "https://%s", host);
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return -1; }
    return 0;
}

int ym_track_next(const YmTrack *tracks, int count, int current, int direction,
                  bool shuffle, unsigned random_value) {
    if (!tracks || count <= 0) return -1;
    if (current < 0 || current >= count) current = 0;
    if (shuffle) {
        int eligible = 0;
        for (int i = 0; i < count; ++i)
            if (i != current && (tracks[i].available || !tracks[i].loaded)) ++eligible;
        if (!eligible) return tracks[current].available || !tracks[current].loaded ? current : -1;
        unsigned chosen = random_value % (unsigned)eligible;
        for (int i = 0; i < count; ++i)
            if (i != current && (tracks[i].available || !tracks[i].loaded) && chosen-- == 0) return i;
    } else {
        for (int step = 1; step <= count; ++step) {
            int next = (current + (direction < 0 ? -step : step) + count) % count;
            if (tracks[next].available || !tracks[next].loaded) return next;
        }
    }
    return -1;
}

int ym_download_parse(const char *json, char *url, size_t cap) {
    if (!cap) return -1;
    url[0] = 0;
    cJSON *root = cJSON_Parse(json);
    cJSON *info = field(field(root, "result"), "downloadInfo");
    cJSON *key = field(info, "key");
    int rc = -1;
    if (strcmp(string(field(info, "codec")), "mp3") ||
        strcmp(string(field(info, "transport")), "raw") ||
        (key && !cJSON_IsNull(key) && !(cJSON_IsString(key) && !*string(key)))) goto done;
    const char *s = string(field(info, "url"));
    if (!*s) s = string(cJSON_GetArrayItem(field(info, "urls"), 0));
    if (strncmp(s, "https://", 8) || strlen(s) >= cap) goto done;
    for (const char *p = s; *p; ++p) if ((unsigned char)*p <= 32) goto done;
    strcpy(url, s);
    rc = 0;
done:
    cJSON_Delete(root);
    return rc;
}
