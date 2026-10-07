#include "cover.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t pixel(unsigned x, unsigned y) {
    unsigned m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
        ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return (((y / 8) * 16 + x / 8) * 64 + m) * 4;
}
int main(void) {
    FILE *f = fopen("tests/fixtures/cover.png", "rb");
    assert(f);
    unsigned char png[256];
    size_t n = fread(png, 1, sizeof(png), f);
    fclose(f);
    unsigned char *pixels = ym_cover_decode(png, n);
    assert(pixels);
    const unsigned char red[] = {255, 0, 0, 255}, green[] = {255, 0, 255, 0};
    const unsigned char blue[] = {255, 255, 0, 0}, white[] = {255, 255, 255, 255};
    assert(!memcmp(pixels + pixel(0, 0), red, 4));
    assert(!memcmp(pixels + pixel(127, 0), green, 4));
    assert(!memcmp(pixels + pixel(0, 127), blue, 4));
    assert(!memcmp(pixels + pixel(127, 127), white, 4));
    /* An opaque source must cover every texture pixel: padding would let
     * the fallback record show through, as seen on the console. */
    for (size_t i = 0; i < 128 * 128; ++i) assert(pixels[i * 4] == 255);
    free(pixels);
    assert(!ym_cover_decode((const unsigned char *)"bad PNG", 7));
    assert(!ym_cover_decode(png, 12));
    puts("УСПЕШНО: PNG, размер обложки, цвета, ориентация, повреждённые данные");
    return 0;
}
