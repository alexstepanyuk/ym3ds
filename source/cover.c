#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 512
#include "stb_image.h"
#include "cover.h"
#include <stdlib.h>
#include <limits.h>

unsigned char *ym_cover_decode(const unsigned char *bytes, size_t size) {
    int w, h, channels;
    if (!bytes || size > INT_MAX || !stbi_info_from_memory(bytes, (int)size, &w, &h, &channels) ||
        w < 1 || h < 1 || w > 512 || h > 512) return NULL;
    unsigned char *source = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (!source) return NULL;
    unsigned char *pixels = calloc(128 * 128, 4);
    if (pixels) for (unsigned y = 0; y < 100; ++y) for (unsigned x = 0; x < 100; ++x) {
        unsigned ty = 127 - y;
        unsigned morton = (x & 1) | ((ty & 1) << 1) | ((x & 2) << 1) |
            ((ty & 2) << 2) | ((x & 4) << 2) | ((ty & 4) << 3);
        size_t offset = (((ty / 8) * 16 + x / 8) * 64 + morton) * 4;
        const unsigned char *p = source + (((size_t)y * h / 100) * w + x * (unsigned)w / 100) * 4;
        pixels[offset] = p[3]; pixels[offset + 1] = p[2];
        pixels[offset + 2] = p[1]; pixels[offset + 3] = p[0];
    }
    stbi_image_free(source);
    return pixels;
}
