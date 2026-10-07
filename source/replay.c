#include "replay.h"
#include <string.h>

bool mp3_replay_write(FILE *writer, FILE *prefix, size_t prefix_size,
                      size_t *verified, const char *data, size_t bytes,
                      size_t *stored, size_t limit) {
    if (*verified > prefix_size || *stored > limit) return false;
    size_t skip = prefix_size - *verified;
    if (skip > bytes) skip = bytes;
    char check[4096];
    size_t compared = 0;
    while (compared < skip) {
        size_t n = skip - compared;
        if (n > sizeof(check)) n = sizeof(check);
        if (!prefix || fread(check, 1, n, prefix) != n || memcmp(check, data + compared, n)) return false;
        compared += n;
    }
    *verified += skip;
    data += skip; bytes -= skip;
    if (bytes > limit - *stored) return false;
    if (bytes && (fwrite(data, 1, bytes, writer) != bytes || fflush(writer))) return false;
    *stored += bytes;
    return true;
}
