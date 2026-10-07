#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Writer flushes before publishing bytes; only the reader owns its FILE. */
typedef struct {
    atomic_uint available;
    atomic_bool done, failed, stop;
} Mp3Stream;
typedef struct {
    FILE *file;
    Mp3Stream *stream;
    size_t position;
    bool failed;
    void *user;
    bool (*cancelled)(void *user);
    void (*wait)(void *user);
} Mp3Reader;
void mp3_stream_init(Mp3Stream *stream);
size_t mp3_stream_read(void *buffer, size_t size, void *user);
int mp3_stream_seek(uint64_t position, void *user);
