// Zero-shared image memory (dm/gfx/vk_android/vka_zshare.h): which images are candidates, images of one key share one allocation and others do not, the allocation goes
// with its last image (in any order), and the stats count each allocation once and the bytes the sharing saves
#include <stdio.h>
#include "../../gfx/vk_android/vka_zshare.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MIB (1ull << 20)

typedef struct { uint32_t fl, fmt, w, h, levels, usage; } key;   // a stand-in for the create info vka_tex.c keys on
static int bind(vkz_tab *t, const key *k, uint64_t size, uint64_t *next_h, uint64_t *h_out) {   // vka_imem_bind_zero's bookkeeping: 1 = a new allocation
    int i = vkz_ref(t, k, sizeof *k);
    if (i >= 0) { *h_out = t->e[i].h; return 0; }
    uint64_t h = (*next_h)++;
    CHECK(vkz_add(t, k, sizeof *k, h, size) >= 0, "add");
    *h_out = h; return 1;
}

int main(void) {
    // candidates: at least min_bytes and at most 32 MiB, dense, one sample, 2D, uncompressed, not linear or a display texture. The 32 MiB cap (a319e773) leaves out
    // Mario Kart World's 16384² R8 terrain targets, which zero-sharing was made for (open: docs/reference/memory.md "Zero-sharing capped at 32 MiB leaves out the
    // large targets it was made for (Android)")
    CHECK(vkz_candidate(341 * MIB, 4 * MIB, 0, 0, 1, 0, 0, 0), "a 16384² R8 target (341 MiB with its levels): a candidate with no upper limit");
    CHECK(vkz_max(0, 1, -1) == 0 && vkz_max(0, 0, -1) == VKZ_CAP && vkz_max(1, 1, -1) == VKZ_CAP, "the limit: none for color on Turnip, 32 MiB for color elsewhere and for depth anywhere");
    CHECK(vkz_max(0, 0, 0) == 0 && vkz_max(0, 1, 64) == 64 * MIB && vkz_max(1, 1, 0) == VKZ_CAP, "DM_VK_ZSHARE_MAX_MB sets the color limit (0: none), never the depth one");
    CHECK(!vkz_candidate(341 * MIB, 4 * MIB, vkz_max(0, 0, -1), 0, 1, 0, 0, 0), "a 341 MiB color image off Turnip is over the cap: not");
    CHECK(!vkz_candidate(341 * MIB, 4 * MIB, VKZ_CAP, 0, 1, 0, 0, 0), "a 341 MiB depth image is over the depth cap: not");
    CHECK(vkz_candidate(32 * MIB, 4 * MIB, VKZ_CAP, 0, 1, 0, 0, 0), "a depth image at the cap: a candidate");
    CHECK(!vkz_candidate(32 * MIB + 1, 4 * MIB, VKZ_CAP, 0, 1, 0, 0, 0), "a depth image a byte over the cap: not");
    CHECK(vkz_candidate(16 * MIB, 4 * MIB, 0, 0, 1, 0, 0, 0), "a 16 MiB target is a candidate");
    CHECK(!vkz_candidate(3 * MIB, 4 * MIB, 0, 0, 1, 0, 0, 0), "under the size: not");
    CHECK(!vkz_candidate(16 * MIB, 4 * MIB, 0, 1, 1, 0, 0, 0), "a sparse residency image: not");
    CHECK(!vkz_candidate(16 * MIB, 4 * MIB, 0, 0, 4, 0, 0, 0), "multisampled: not");
    CHECK(!vkz_candidate(16 * MIB, 4 * MIB, 0, 0, 1, 1, 0, 0), "3D: not");
    CHECK(!vkz_candidate(16 * MIB, 4 * MIB, 0, 0, 1, 0, 1, 0), "block-compressed: not");
    CHECK(!vkz_candidate(16 * MIB, 4 * MIB, 0, 0, 1, 0, 0, 1), "linear or a window texture: not");
    CHECK(!vkz_candidate(16 * MIB, 0, 0, 0, 1, 0, 0, 0), "a size of 0 turns it off");

    vkz_tab t = {0}; uint64_t nh = 100, h[8];
    key r8 = {0x400, 9, 16384, 16384, 6, 0x17}, rg16 = {0x400, 77, 8192, 8192, 1, 0x17}, r8b = r8; r8b.levels = 5;
    // four R8 targets of one create info: one allocation
    int fresh = 0; for (int i = 0; i < 4; i++) fresh += bind(&t, &r8, 341 * MIB, &nh, &h[i]);
    CHECK(fresh == 1 && h[0] == h[1] && h[1] == h[2] && h[2] == h[3], "four images of one key share one allocation (%d made)", fresh);
    CHECK(bind(&t, &rg16, 128 * MIB, &nh, &h[4]) == 1 && h[4] != h[0], "another key: its own allocation");
    CHECK(bind(&t, &r8b, 340 * MIB, &nh, &h[5]) == 1 && h[5] != h[0], "one field apart (levels): its own allocation");
    uint64_t im, al, by, sv; vkz_stats(&t, &im, &al, &by, &sv);
    CHECK(im == 6 && al == 3 && by == (341 + 128 + 340) * MIB && sv == 3 * 341 * MIB, "stats: %llu images, %llu allocations, %llu MiB, %llu MiB saved",
          (unsigned long long)im, (unsigned long long)al, (unsigned long long)(by / MIB), (unsigned long long)(sv / MIB));
    // let go in any order: the allocation goes with the last
    CHECK(vkz_unref(&t, h[2]) == 0 && vkz_unref(&t, h[0]) == 0 && vkz_unref(&t, h[3]) == 0, "three of four let go: kept");
    vkz_note_own(&t); vkz_note_own(&t);
    CHECK(vkz_unref(&t, h[1]) == 1, "the fourth: the allocation is freed");
    CHECK(vkz_unref(&t, h[1]) == -1, "and is no entry's any more");
    CHECK(vkz_find(&t, &r8, sizeof r8) < 0, "its key is gone");
    CHECK(bind(&t, &r8, 341 * MIB, &nh, &h[6]) == 1 && h[6] != h[0], "a new image of that key makes a new allocation");
    vkz_stats(&t, &im, &al, &by, &sv);
    CHECK(im == 3 && al == 3 && sv == 0 && t.made == 4 && t.owned == 2, "stats after: %llu images, %llu allocations, %llu made, %llu owned", (unsigned long long)im, (unsigned long long)al,
          (unsigned long long)t.made, (unsigned long long)t.owned);
    CHECK(vkz_add(&t, &r8, VKZ_KEY + 1, 999, 1) < 0 && vkz_ref(&t, &r8, VKZ_KEY + 1) < 0, "a key longer than VKZ_KEY is refused");
    // many keys: the table grows
    for (uint32_t i = 0; i < 1000; i++) { key k = {0x400, 9, 64 + i, 64, 1, 0x17}; uint64_t hh; bind(&t, &k, 4 * MIB, &nh, &hh); }
    vkz_stats(&t, &im, &al, NULL, NULL);
    CHECK(al == 1003 && im == 1003, "1000 more keys: %llu allocations", (unsigned long long)al);
    free(t.e);
    printf("t_android_zshare: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
