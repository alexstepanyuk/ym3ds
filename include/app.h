#pragma once
#include <3ds.h>
#include <stdatomic.h>
#include "model.h"
#include "stream.h"

#define YM_BASE "sdmc:/3ds/ym3ds"
#define YM_TOKEN_FILE YM_BASE "/config/token.txt"
#define YM_CA_FILE YM_BASE "/config/cacert.pem"
#define YM_AUDIO_FILE YM_BASE "/cache/current.mp3"

typedef struct {
    LightLock lock;
    atomic_int generation;
    atomic_bool quitting;
    atomic_bool paused;
    int job; /* -3: playlists, -2: likes, -1: none, <= -4: open playlist */
    int count;
    int playing;
    bool loading;
    bool audio_ready;
    bool shuffle, repeat_one;
    unsigned downloaded_kb;
    unsigned download_total_kb;
    bool download_complete;
    unsigned position_ms;
    int sample_rate;
    int bitrate_kbps;
    char status[256];
    YmTrack *tracks;
    YmPlaylist *playlists;
    int playlist_count;
    bool playlist_view;
    bool hydration_pending;
    unsigned library_revision;
    char uid[YM_ID_SIZE], collection[YM_TEXT_SIZE];
    unsigned char *cover_pixels;
    unsigned cover_revision;
    char cover_id[YM_ID_SIZE];
} App;

void app_status(App *app, const char *format, ...);
bool app_cancelled(App *app, int generation);
void app_worker(void *arg);
void app_request(App *app, int job);
void app_toggle_pause(App *app);
int player_play(App *app, int generation, Mp3Stream *stream);
