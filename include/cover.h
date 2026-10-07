#pragma once
#include <stddef.h>
/* 128x128 tiled ABGR texture; visible artwork occupies 100x100. */
unsigned char *ym_cover_decode(const unsigned char *bytes, size_t size);
