#include "stream.h"
#include <limits.h>

void mp3_stream_init(Mp3Stream *s) {
    atomic_init(&s->available, 0);
    atomic_init(&s->done, false);
    atomic_init(&s->failed, false);
    atomic_init(&s->stop, false);
}

size_t mp3_stream_read(void *buffer, size_t size, void *user) {
    Mp3Reader *r = user;
    size_t copied = 0;
    /* minimp3 treats a short callback read as EOF. Fill the request unless
     * the producer really finished, failed, or the user stopped playback. */
    while (copied < size) {
        if (r->cancelled(r->user) || atomic_load(&r->stream->stop) ||
            atomic_load(&r->stream->failed)) return 0;
        bool done = atomic_load(&r->stream->done);
        size_t available = atomic_load(&r->stream->available);
        if (available > r->position) {
            size_t n = available - r->position;
            if (n > size - copied) n = size - copied;
            clearerr(r->file);
            size_t got = fread((char *)buffer + copied, 1, n, r->file);
            if (got != n) {
                r->failed = true;
                atomic_store(&r->stream->stop, true);
                return 0;
            }
            copied += got;
            r->position += got;
        } else if (done) break;
        else r->wait(r->user);
    }
    return copied;
}

int mp3_stream_seek(uint64_t position, void *user) {
    Mp3Reader *r = user;
    if (position > LONG_MAX || position > atomic_load(&r->stream->available)) return -1;
    clearerr(r->file);
    if (fseek(r->file, (long)position, SEEK_SET)) return -1;
    r->position = (size_t)position;
    return 0;
}
