#include "model.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

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
            ++resolved;
        }
    }
    cJSON_Delete(root);
    return resolved;
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
