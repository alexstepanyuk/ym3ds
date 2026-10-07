#include "replay.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    FILE *writer = tmpfile(), *prefix = tmpfile();
    assert(writer && prefix);
    assert(fwrite("abcdef", 1, 6, writer) == 6 && !fflush(writer));
    assert(fwrite("abcdef", 1, 6, prefix) == 6 && !fflush(prefix));
    rewind(prefix);
    size_t stored = 6, verified = 0;
    assert(mp3_replay_write(writer, prefix, 6, &verified, "ab", 2, &stored, 12));
    assert(stored == 6 && verified == 2);
    assert(mp3_replay_write(writer, prefix, 6, &verified, "cdefghi", 7, &stored, 12));
    assert(stored == 9 && verified == 6);
    rewind(writer);
    char result[12] = {0};
    assert(fread(result, 1, 9, writer) == 9 && !memcmp(result, "abcdefghi", 9));
    rewind(prefix); verified = 0;
    assert(!mp3_replay_write(writer, prefix, 6, &verified, "different", 9, &stored, 12));
    assert(stored == 9);
    verified = 6;
    assert(!mp3_replay_write(writer, prefix, 6, &verified, "longer", 6, &stored, 12));
    assert(stored == 9);
    fclose(prefix); fclose(writer);
    puts("УСПЕШНО: повтор загрузки, проверка старых байтов, отсутствие дублей и лимит");
    return 0;
}
