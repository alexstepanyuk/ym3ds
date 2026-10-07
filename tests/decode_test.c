/* Check bounded callback decoding against whole-file decoding on a real MP3.
 * Usage: decode_test <mp3 fixture>; no network or console required. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#define MINIMP3_IO_SIZE (32 * 1024)
#include "minimp3_ex.h"
#include "stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static size_t read_file(void *buf, size_t n, void *user) { return fread(buf, 1, n, user); }
static int seek_file(uint64_t pos, void *user) { return pos <= LONG_MAX ? fseek(user, (long)pos, SEEK_SET) : -1; }

typedef struct {
    Mp3Stream stream;
    FILE *writer;
    const unsigned char *bytes;
    size_t size, published;
    bool cancel, fail;
    unsigned waits;
} Producer;
static bool cancelled(void *user) { return ((Producer *)user)->cancel; }
/* Deterministic slow producer: each wait publishes only 4 KiB, including
 * a final short chunk. Uses separate file handles just like the console. */
static void publish(void *user) {
    Producer *p = user;
    ++p->waits;
    if (p->fail) { atomic_store(&p->stream.failed, true); return; }
    size_t n = p->size - p->published;
    if (n > 4096) n = 4096;
    assert(fwrite(p->bytes + p->published, 1, n, p->writer) == n);
    assert(!fflush(p->writer));
    p->published += n;
    atomic_store(&p->stream.available, (unsigned)p->published);
    if (p->published == p->size) atomic_store(&p->stream.done, true);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0);
    rewind(file);
    unsigned char *bytes = malloc((size_t)size);
    assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
    rewind(file);
    mp3dec_t *full_decoder = calloc(1, sizeof(*full_decoder));
    mp3dec_file_info_t full = {0};
    assert(!mp3dec_load_buf(full_decoder, bytes, (size_t)size, &full, NULL, NULL));
    assert(full.samples > 0);
    mp3dec_ex_t *stream = calloc(1, sizeof(*stream));
    mp3dec_io_t io = {.read = read_file, .read_data = file, .seek = seek_file, .seek_data = file};
    assert(!mp3dec_ex_open_cb(stream, &io, MP3D_DO_NOT_SCAN));
    assert(stream->info.channels == full.channels && stream->info.hz == full.hz);
    short pcm[8192];
    size_t offset = 0, n;
    while ((n = mp3dec_ex_read(stream, pcm, 8192))) {
        assert(!stream->last_error && offset + n <= full.samples);
        assert(!memcmp(pcm, full.buffer + offset, n * sizeof(short)));
        offset += n;
    }
    assert(!stream->last_error && offset == full.samples);
    /* Byte seeking must restart decoding without a full-file sample index.
     * Exercise backwards, forwards and rewind on the real MP3 fixture. */
    const uint64_t seek_bytes[] = {(uint64_t)size / 2, (uint64_t)size / 4, stream->start_offset};
    for (size_t i = 0; i < sizeof(seek_bytes) / sizeof(*seek_bytes); ++i) {
        assert(!mp3dec_ex_seek(stream, seek_bytes[i]));
        n = mp3dec_ex_read(stream, pcm, 8192);
        assert(n > 0 && !stream->last_error);
        assert(stream->info.hz == full.hz && stream->info.channels == full.channels);
    }
    printf("УСПЕШНО: блочное чтение MP3, %zu отсчётов, %d Гц, %d каналов\n", offset, full.hz, full.channels);
    mp3dec_ex_close(stream);
    Producer producer = {.bytes = bytes, .size = (size_t)size};
    mp3_stream_init(&producer.stream);
    producer.writer = fopen("build/host/progressive.mp3.part", "wb");
    assert(producer.writer);
    FILE *reader_file = fopen("build/host/progressive.mp3.part", "rb");
    assert(reader_file);
    Mp3Reader reader = {.file = reader_file, .stream = &producer.stream,
        .user = &producer, .cancelled = cancelled, .wait = publish};
    io = (mp3dec_io_t){.read = mp3_stream_read, .read_data = &reader,
        .seek = mp3_stream_seek, .seek_data = &reader};
    assert(!mp3dec_ex_open_cb(stream, &io, MP3D_DO_NOT_SCAN));
    offset = 0;
    while ((n = mp3dec_ex_read(stream, pcm, 8192))) {
        if (!offset) assert(producer.published < producer.size);
        assert(!stream->last_error && offset + n <= full.samples);
        assert(!memcmp(pcm, full.buffer + offset, n * sizeof(short)));
        offset += n;
    }
    assert(!stream->last_error && offset == full.samples && producer.waits > 1);
    mp3dec_ex_close(stream);
    /* EOF, seek backwards, failure while waiting and cancellation. */
    assert(!mp3_stream_seek(0, &reader));
    unsigned char probe[64];
    assert(mp3_stream_read(probe, sizeof(probe), &reader) == sizeof(probe));
    assert(!memcmp(probe, bytes, sizeof(probe)));
    assert(mp3_stream_seek((uint64_t)size + 1, &reader) == -1);
    assert(!mp3_stream_seek((uint64_t)size - 10, &reader));
    assert(mp3_stream_read(probe, sizeof(probe), &reader) == 10);
    atomic_store(&producer.stream.done, false);
    producer.fail = true;
    assert(mp3_stream_read(probe, sizeof(probe), &reader) == 0);
    assert(atomic_load(&producer.stream.failed));
    atomic_store(&producer.stream.failed, false);
    producer.cancel = true;
    assert(mp3_stream_read(probe, sizeof(probe), &reader) == 0);
    producer.cancel = false;
    atomic_store(&producer.stream.stop, true);
    assert(mp3_stream_read(probe, sizeof(probe), &reader) == 0);
    fclose(reader_file); fclose(producer.writer);
    remove("build/host/progressive.mp3.part");
    puts("УСПЕШНО: старт до конца загрузки, точное PCM, ожидание, EOF, ошибка и отмена");
    free(stream); free(full.buffer); free(full_decoder); free(bytes); fclose(file);
    return 0;
}
