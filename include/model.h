#pragma once
#include <stddef.h>
#include <stdbool.h>

#define YM_MAX_TRACKS 100
#define YM_ID_SIZE 32
#define YM_TEXT_SIZE 192

typedef struct {
    char id[YM_ID_SIZE];
    char title[YM_TEXT_SIZE];
    char artist[YM_TEXT_SIZE];
    unsigned duration_ms;
    bool available;
} YmTrack;

/* Portable parsing: no console services or network access. */
int ym_token_parse(const char *text, char *out, size_t capacity);
int ym_account_parse(const char *json, char uid[YM_ID_SIZE]);
int ym_likes_parse(const char *json, YmTrack *out, size_t capacity);
int ym_tracks_parse(const char *json, YmTrack *tracks, size_t count);
int ym_download_parse(const char *json, char *url, size_t capacity);
bool ym_id_valid(const char *id);
void ym_text_copy(char *out, size_t capacity, const char *text);
