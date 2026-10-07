#pragma once
#include "app.h"
#include <curl/curl.h>

/* OAuth header only goes to the fixed API host; CDN gets no token. */
int net_json(App *app, int generation, const char *path, const char *token,
             const char *post, char **out);
int net_download(App *app, int generation, const char *url, Mp3Stream *stream);
int net_file_info_path(const char *id, bool high_quality, char *out, size_t capacity);
int net_cover(App *app, int generation, const char *uri, unsigned char **bytes, size_t *size);
