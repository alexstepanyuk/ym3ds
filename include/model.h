#pragma once
#include <stddef.h>
#include <stdbool.h>

#define YM_ID_SIZE 32
#define YM_TEXT_SIZE 192

typedef struct {
    char id[YM_ID_SIZE];
    char title[YM_TEXT_SIZE];
    char artist[YM_TEXT_SIZE];
    unsigned duration_ms;
    bool available;
    bool loaded;
    char cover[256];
} YmTrack;

typedef struct {
    char owner[YM_ID_SIZE], kind[YM_ID_SIZE];
    char title[YM_TEXT_SIZE];
    unsigned track_count;
} YmPlaylist;
int ym_track_list_parse(const char *json, bool playlist, YmTrack **out);
int ym_playlists_parse(const char *json, bool liked, const char *owner, YmPlaylist **out);
int ym_cover_url(const char *uri, char *out, size_t capacity);

/* Portable parsing: no console services or network access. */
int ym_token_parse(const char *text, char *out, size_t capacity);
int ym_account_parse(const char *json, char uid[YM_ID_SIZE]);
int ym_likes_parse(const char *json, YmTrack *out, size_t capacity);
int ym_tracks_parse(const char *json, YmTrack *tracks, size_t count);
int ym_download_parse(const char *json, char *url, size_t capacity);
bool ym_id_valid(const char *id);
void ym_text_copy(char *out, size_t capacity, const char *text);
int ym_track_next(const YmTrack *tracks, int count, int current, int direction,
                  bool shuffle, unsigned random_value);
