// pack_modules (pack_modules.h): the module list of an Android pack, from the "modules" row of its pack.tsv. No game files.
#include "../pack_modules.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

static char dir[256];

static void put(const char *text) {
    char path[300]; snprintf(path, sizeof path, "%s/pack.tsv", dir);
    FILE *f = fopen(path, "w"); assert(f); fputs(text, f); fclose(f);
}
static int get(const char **names, int max) { static char buf[512]; return pack_modules(dir, buf, sizeof buf, names, max); }

int main(void) {
    snprintf(dir, sizeof dir, "%s/pm_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(dir));
    const char *n[8];
    assert(get(n, 8) == 0);   // no pack.tsv: the profile's list
    put("format\t1\ntitle\t0400C3F00006E000\nversion\tv0\nmodules\tmain subsdk0 sdk\nbuild\tmain=aa\n");
    assert(get(n, 8) == 3 && !strcmp(n[0], "main") && !strcmp(n[1], "subsdk0") && !strcmp(n[2], "sdk"));   // 1.0.0: one module less
    put("modules\tmain subsdk0 subsdk1 sdk\r\n");
    assert(get(n, 8) == 4 && !strcmp(n[2], "subsdk1") && !strcmp(n[3], "sdk"));   // 1.8.0, a CRLF row
    put("format\t1\nversion\tv0\n");
    assert(get(n, 8) == 0);   // no modules row
    put("modules\t\n");
    assert(get(n, 8) == 0);   // an empty row
    put("modules\tmain subsdk0 subsdk1 sdk\n");
    assert(get(n, 3) == 0);   // more names than the profile has room for: not trusted
    put("modules\tmain ../evil sdk\n");
    assert(get(n, 8) == 0);   // not a plain module name
    put("modules\tmain .hidden\n");
    assert(get(n, 8) == 0);
    char path[300]; snprintf(path, sizeof path, "%s/pack.tsv", dir); unlink(path); rmdir(dir);
    puts("t_pack_modules: ok");
    return 0;
}
