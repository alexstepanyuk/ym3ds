#pragma once
#include <stddef.h>
/* Artwork fills a 128x128 tiled ABGR texture, with its top row at tile y=0. */
unsigned char *ym_cover_decode(const unsigned char *bytes, size_t size);
