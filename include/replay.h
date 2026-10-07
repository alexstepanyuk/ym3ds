#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
/* A retry starts at byte zero. Validate the existing prefix before appending. */
bool mp3_replay_write(FILE *writer, FILE *prefix, size_t prefix_size,
                      size_t *verified, const char *data, size_t bytes,
                      size_t *stored, size_t limit);
