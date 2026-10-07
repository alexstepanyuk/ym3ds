#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#define MINIMP3_IO_SIZE (32 * 1024)
#include "minimp3_ex.h"
#include "app.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Four PCM blocks buffer decoder scheduling jitter. NDSP does resampling. */
#define BLOCKS 4
#define SAMPLES 8192

typedef struct { App *app; int generation; bool waiting; } Playback;
static bool cancelled(void *user) {
    Playback *p = user;
    return app_cancelled(p->app, p->generation);
}
static void wait_data(void *user) {
    Playback *p = user;
    if (!p->waiting) { app_status(p->app, "Буферизация: жду данные…"); p->waiting = true; }
    ndspChnSetPaused(0, atomic_load(&p->app->paused));
    svcSleepThread(2000000);
}
static size_t read_mp3(void *buf, size_t size, void *user) {
    Mp3Reader *r = user;
    size_t n = mp3_stream_read(buf, size, r);
    Playback *p = r->user;
    if (p->waiting && n && !cancelled(p)) {
        LightLock_Lock(&p->app->lock);
        if (!atomic_load(&r->stream->failed)) {
            strcpy(p->app->status, "Воспроизведение"); p->waiting = false;
        }
        LightLock_Unlock(&p->app->lock);
    }
    return n;
}

int player_play(App *app, int gen, Mp3Stream *stream) {
    mp3dec_ex_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return -1;
    FILE *file = fopen(YM_AUDIO_FILE ".part", "rb");
    if (!file) { free(decoder); return -1; }
    Playback playback = {.app = app, .generation = gen};
    Mp3Reader reader = {.file = file, .stream = stream, .user = &playback,
        .cancelled = cancelled, .wait = wait_data};
    mp3dec_io_t io = {.read = read_mp3, .read_data = &reader, .seek = mp3_stream_seek, .seek_data = &reader};
    if (mp3dec_ex_open_cb(decoder, &io, MP3D_DO_NOT_SCAN)) {
        mp3dec_ex_close(decoder); fclose(file); free(decoder); return -1;
    }
    int channels = decoder->info.channels;
    int sample_rate = decoder->info.hz;
    LightLock_Lock(&app->lock);
    app->sample_rate = sample_rate;
    app->bitrate_kbps = decoder->info.bitrate_kbps;
    LightLock_Unlock(&app->lock);
    if ((channels != 1 && channels != 2) || decoder->info.hz < 8000 || decoder->info.hz > 48000) {
        mp3dec_ex_close(decoder); fclose(file); free(decoder); return -1;
    }
    s16 *pcm = linearAlloc(BLOCKS * SAMPLES * sizeof(s16));
    if (!pcm) { mp3dec_ex_close(decoder); fclose(file); free(decoder); return -1; }
    ndspWaveBuf buffers[BLOCKS] = {0};
    ndspChnReset(0);
    ndspChnSetFormat(0, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(0, (float)sample_rate);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix);
    for (int i = 0; i < BLOCKS; ++i) buffers[i].data_pcm16 = pcm + i * SAMPLES;
    int rc = 0, index = 0;
    uint64_t completed_samples = 0;
    while (!app_cancelled(app, gen)) {
        bool paused = atomic_load(&app->paused);
        ndspChnSetPaused(0, paused);
        ndspWaveBuf *b = &buffers[index];
        if (paused || (b->status != NDSP_WBUF_DONE && b->status != NDSP_WBUF_FREE)) {
            svcSleepThread(2000000); continue;
        }
        if (b->status == NDSP_WBUF_DONE) {
            completed_samples += b->nsamples;
            LightLock_Lock(&app->lock);
            app->position_ms = (unsigned)(completed_samples * 1000 / sample_rate);
            LightLock_Unlock(&app->lock);
        }
        size_t n = mp3dec_ex_read(decoder, b->data_pcm16, SAMPLES);
        if (decoder->last_error || reader.failed || ferror(file) || atomic_load(&stream->failed)) { rc = -1; break; }
        if (!n) {
            /* Drain the queued final blocks without dropping the end. */
            bool queued;
            do {
                ndspChnSetPaused(0, atomic_load(&app->paused));
                queued = false;
                for (int i = 0; i < BLOCKS; ++i)
                    if (buffers[i].status == NDSP_WBUF_QUEUED || buffers[i].status == NDSP_WBUF_PLAYING) queued = true;
                if (queued) svcSleepThread(2000000);
            } while (queued && !app_cancelled(app, gen));
            break;
        }
        /* ndspChnGetRate returns a DSP rate ratio, not Hertz. Compare the
         * decoder against the original stream format instead. */
        if (decoder->info.channels != channels || decoder->info.hz != sample_rate || n % channels) {
            rc = -1; break;
        }
        b->nsamples = (u32)(n / channels);
        b->looping = false;
        DSP_FlushDataCache(b->data_pcm16, (u32)(n * sizeof(s16)));
        ndspChnWaveBufAdd(0, b);
        index = (index + 1) % BLOCKS;
    }
    ndspChnWaveBufClear(0);
    ndspChnSetPaused(0, false);
    /* Channel commands reach DSP asynchronously. Keep PCM alive while the
     * stop command passes through both DSP command buffers. */
    u32 stopped_at = ndspGetFrameCount();
    for (int i = 0; i < 50 && (u32)(ndspGetFrameCount() - stopped_at) < 2; ++i)
        svcSleepThread(2000000);
    linearFree(pcm);
    mp3dec_ex_close(decoder);
    fclose(file);
    free(decoder);
    return rc;
}
