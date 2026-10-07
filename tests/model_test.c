#include "model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    YmTrack queue[4] = {0};
    for (int i = 0; i < 4; ++i) queue[i].loaded = true;
    queue[0].available = queue[2].available = queue[3].available = true;
    assert(ym_track_next(queue, 4, 0, 1, false, 0) == 2);
    assert(ym_track_next(queue, 4, 0, -1, false, 0) == 3);
    assert(ym_track_next(queue, 4, 3, 1, false, 0) == 0);
    for (unsigned choice = 0; choice < 200; ++choice) {
        int next = ym_track_next(queue, 4, 2, 1, true, choice);
        assert(next == 0 || next == 3);
    }
    assert(ym_track_next(queue, 0, 0, 1, true, 0) == -1);
    queue[0].available = queue[3].available = false;
    assert(ym_track_next(queue, 4, 2, 1, true, 0) == 2);
    queue[2].available = false;
    assert(ym_track_next(queue, 4, 2, -1, false, 0) == -1);
    assert(ym_track_next(queue, 4, 2, 1, true, 0) == -1);
    queue[1].loaded = false;
    assert(ym_track_next(queue, 4, 0, 1, false, 0) == 1);
    YmTrack *large = NULL;
    char *library = malloc(60000);
    assert(library);
    size_t used = (size_t)sprintf(library, "{\"result\":{\"library\":{\"tracks\":[");
    for (unsigned i = 0; i < 2000; ++i)
        used += (size_t)sprintf(library + used, "%s{\"id\":%u}", i ? "," : "", i + 1);
    strcpy(library + used, "]}}}");
    assert(ym_track_list_parse(library, false, &large) == 2000);
    assert(!strcmp(large[1999].id, "2000") && !large[1999].loaded);
    assert(ym_track_next(large, 2000, 1998, 1, false, 0) == 1999);
    free(large); free(library);
    assert(ym_track_list_parse("{\"result\":{\"tracks\":[{\"id\":\"123\"},{\"id\":\"../bad\"},{\"id\":456}]}}", true, &large) == 2);
    assert(!strcmp(large[1].id, "456")); free(large);
    assert(ym_track_list_parse("{\"result\":{\"library\":{\"tracks\":[]}}}", false, &large) == 0);
    free(large);
    assert(ym_track_list_parse("{}", false, &large) == -1 && !large);
    YmPlaylist *playlists = NULL;
    assert(ym_playlists_parse("{\"result\":[{\"kind\":1000,\"title\":\"Мой список\",\"trackCount\":1200}]}", false, "4294967297", &playlists) == 1);
    assert(!strcmp(playlists[0].owner, "4294967297") && playlists[0].track_count == 1200); free(playlists);
    assert(ym_playlists_parse("{\"result\":[{\"playlist\":{\"kind\":\"42\",\"uid\":\"999\",\"title\":\"Чужой\"}}]}", true, "1", &playlists) == 1);
    assert(!strcmp(playlists[0].owner, "999")); free(playlists);
    char cover[512];
    assert(!ym_cover_url("avatars.yandex.net/get-music/123/%%", cover, sizeof(cover)));
    assert(!strcmp(cover, "https://avatars.yandex.net/get-music/123/100x100"));
    assert(!ym_cover_url("http://avatars.mds.yandex.net/get-music/123/%%", cover, sizeof(cover)));
    assert(ym_cover_url("https://evil.example/x", cover, sizeof(cover)) == -1);
    assert(ym_cover_url("avatars.yandex.net.evil.example/x", cover, sizeof(cover)) == -1);
    assert(ym_cover_url("avatars.yandex.net/x\nheader", cover, sizeof(cover)) == -1);
    assert(ym_cover_url("avatars.yandex.net/x/%%", cover, 8) == -1);
    puts("УСПЕШНО: API, UTF-8, 2000 треков, плейлисты, обложки, переходы и перемешивание");
    return 0;
}
