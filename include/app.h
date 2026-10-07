#pragma once
#include <3ds.h>
#include <stdatomic.h>
#include "model.h"

#define YM_BASE "sdmc:/3ds/ym3ds"
#define YM_TOKEN_FILE YM_BASE "/config/token.txt"
#define YM_CA_FILE YM_BASE "/config/cacert.pem"
#define YM_AUDIO_FILE YM_BASE "/cache/current.mp3"

typedef struct {
    LightLock lock;
    atomic_int generation;
    atomic_bool quitting;
    atomic_bool paused;
    int job; /* -2: reload, -1: none, otherwise track index */
    int count;
    int playing;
    bool loading;
    bool audio_ready;
    unsigned downloaded_kb;
    char status[256];
    YmTrack tracks[YM_MAX_TRACKS];
} App;

void app_status(App *app, const char *format, ...);
bool app_cancelled(App *app, int generation);
void app_worker(void *arg);
void app_request(App *app, int job);
int player_play(App *app, int generation);
