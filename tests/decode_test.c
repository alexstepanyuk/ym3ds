/* Check bounded callback decoding against whole-file decoding on a real MP3.
 * Usage: decode_test <mp3 fixture>; no network or console required. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static size_t read_file(void *buf, size_t n, void *user) { return fread(buf, 1, n, user); }
static int seek_file(uint64_t pos, void *user) { return pos <= LONG_MAX ? fseek(user, (long)pos, SEEK_SET) : -1; }

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
    printf("УСПЕШНО: блочное чтение MP3, %zu отсчётов, %d Гц, %d каналов\n", offset, full.hz, full.channels);
    mp3dec_ex_close(stream);
    free(stream); free(full.buffer); free(full_decoder); free(bytes); fclose(file);
    return 0;
}
