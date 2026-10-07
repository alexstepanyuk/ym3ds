#include "model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char token[32], uid[YM_ID_SIZE], url[128], text[8];
    assert(!ym_token_parse("YANDEX_TOKEN = \"sample-token\"\r\n", token, sizeof(token)));
    assert(!strcmp(token, "sample-token"));
    const char *bad[] = {"", "YANDEX_TOKEN", "YANDEX_TOKEN = \"\"", "YANDEX_TOKEN = \"x\nInjected\"",
                         "YANDEX_TOKEN = \"x\" extra", "PREFIX_YANDEX_TOKEN = \"x\""};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        assert(ym_token_parse(bad[i], token, sizeof(token)) == -1);
        assert(!*token);
    }
    assert(ym_token_parse("YANDEX_TOKEN=\"longtoken\"", token, 4) == -1);
    assert(!ym_account_parse("{\"result\":{\"account\":{\"uid\":4294967297}}}", uid));
    assert(!strcmp(uid, "4294967297"));
    assert(ym_account_parse("{\"result\":{\"account\":{\"uid\":1.5}}}", uid) == -1);
    assert(ym_account_parse("{\"result\":{\"account\":{\"uid\":\"../evil\"}}}", uid) == -1);
    YmTrack tracks[2] = {0};
    assert(ym_likes_parse("{\"result\":{\"library\":{\"tracks\":[{\"id\":\"123\"},{\"id\":456},{\"id\":789}]}}}", tracks, 2) == 2);
    assert(!strcmp(tracks[0].id, "123") && !strcmp(tracks[1].id, "456"));
    assert(!tracks[0].available);
    /* API may reorder results and omit rights-blocked metadata. */
    assert(ym_tracks_parse("{\"result\":[{\"id\":456,\"title\":\"Нет прав\",\"error\":\"no-rights\"},"
                           "{\"id\":\"123\",\"title\":\"Музыка\",\"available\":true,\"artists\":[{\"name\":\"A\"},{\"name\":\"Б\"}]}]}", tracks, 2) == 2);
    assert(tracks[0].available && !tracks[1].available);
    assert(!strcmp(tracks[0].title, "Музыка"));
    assert(!strcmp(tracks[0].artist, "A, Б"));
    assert(ym_likes_parse("{\"result\":{}}", tracks, 2) == -1);
    assert(!ym_download_parse("{\"result\":{\"downloadInfo\":{\"codec\":\"mp3\",\"transport\":\"raw\",\"key\":null,\"urls\":[\"https://cdn.example/audio\"]}}}", url, sizeof(url)));
    assert(!strcmp(url, "https://cdn.example/audio"));
    assert(ym_download_parse("{\"result\":{\"downloadInfo\":{\"codec\":\"mp3\",\"transport\":\"raw\",\"key\":\"secret\",\"url\":\"https://cdn.example/audio\"}}}", url, sizeof(url)) == -1);
    assert(!*url);
    assert(ym_download_parse("{\"result\":{\"downloadInfo\":{\"codec\":\"mp3\",\"transport\":\"raw\",\"url\":\"http://cdn.example/audio\"}}}", url, sizeof(url)) == -1);
    assert(ym_download_parse("not JSON", url, sizeof(url)) == -1);
    ym_text_copy(text, sizeof(text), "АБВГД");
    assert(!strcmp(text, "АБВ"));
    ym_text_copy(text, sizeof(text), "a\nb");
    assert(!strcmp(text, "a b"));
    puts("УСПЕШНО: токен, 64-битные ID, порядок метаданных, права, HTTPS, UTF-8");
    return 0;
}
