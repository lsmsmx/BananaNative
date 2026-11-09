// The memory maps budget's policy and parsing (dm/gfx/vk_android/vka_mapbudget.h; vka_mem.c maps_tick): the tracker's block size at each share of vm.max_map_count, the
// maps compaction sheds, which segments it takes, and the lines of /proc/self/maps as the kernel writes them (kgsl, named anonymous memory, a file, a line without a name).
// The tracker under the policy over a long session is dm/gfx/tests/t_dirty_maps.c
#include <stdio.h>
#include "../../gfx/vk_android/vka_mapbudget.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define KIB 1024ul

int main(void) {
    const long mx = 65530; const size_t base = 64 * KIB;
    // Block size by share of the limit
    CHECK(vkmb_block(0, mx, base) == 0, "nothing asked at 0");
    CHECK(vkmb_block(mx * 39 / 100, mx, base) == 0, "nothing asked under 40 %%");
    CHECK(vkmb_block(mx * 40 / 100 + 1, mx, base) == 64 * KIB, "base blocks from 40 %%");
    CHECK(vkmb_block(34000, mx, base) == 256 * KIB, "Mario Kart World's title (34,000 maps, 52 %%): 256 KiB, got %zu", vkmb_block(34000, mx, base));
    CHECK(vkmb_block(mx * 60 / 100 + 1, mx, base) == 1024 * KIB, "1 MiB from 60 %%");
    CHECK(vkmb_block(mx * 70 / 100 + 1, mx, base) == 4096 * KIB, "4 MiB from 70 %%");
    CHECK(vkmb_block(mx, mx, base) == 4096 * KIB, "4 MiB at most");
    CHECK(vkmb_block(50000, 0, base) == 0 && vkmb_block(-1, mx, base) == 0, "nothing when the count or the limit is not known");
    for (long c = 0; c < mx; c += 97) CHECK(vkmb_block(c + 97, mx, base) >= vkmb_block(c, mx, base), "never finer as the count climbs (%ld)", c);
    // What compaction sheds
    CHECK(vkmb_shed(mx * 74 / 100, mx, 75, 55) == 0, "nothing under the budget");
    long s = vkmb_shed(mx * 80 / 100, mx, 75, 55);
    CHECK(s == mx * 80 / 100 - mx * 55 / 100, "down to the target past it (%ld)", s);
    CHECK(vkmb_shed(65000, mx, 75, 55) > 28000, "at the limit: about 29,000 maps to shed");
    // Which segments: most pieces first, small ones left, the count covered
    int pc[] = {3, 400, 15, 1200, 16, 90, 1};
    int idx[8]; long cov = 0;
    int k = vkmb_pick(pc, 7, 1500, idx, 8, &cov);
    CHECK(k == 2 && idx[0] == 3 && idx[1] == 1 && cov == 1199 + 399, "the two largest cover 1,500 (k %d, cov %ld)", k, cov);
    k = vkmb_pick(pc, 7, 100000, idx, 8, &cov);
    CHECK(k == 4 && idx[2] == 5 && idx[3] == 4 && cov == 1199 + 399 + 89 + 15, "all worth it, none under %d pieces (k %d, cov %ld)", VKMB_MIN_PIECES, k, cov);
    k = vkmb_pick(pc, 7, 100000, idx, 1, &cov);
    CHECK(k == 1 && idx[0] == 3, "at most maxpick a round");
    k = vkmb_pick(pc, 7, 0, idx, 8, &cov);
    CHECK(k == 0 && cov == 0, "nothing to shed: none taken");
    // Lines of /proc/self/maps
    struct { const char *ln; int ok; uint64_t a; int kgsl; } L[] = {
        {"40fe420000-40fe442000 rw-s 2bd5f000 00:0e 1066                           /dev/kgsl-3d0", 1, 0x40fe420000ull, 1},
        {"4000000000-4010000000 ---p 00000000 00:00 0                              [anon:nxnative guest]", 1, 0x4000000000ull, 0},
        {"7a1c2d0000-7a1c2f1000 r-xp 00000000 fe:2f 2563                           /system/lib64/libc.so", 1, 0x7a1c2d0000ull, 0},
        {"7a1c400000-7a1c401000 rw-p 00000000 00:00 0 ", 1, 0x7a1c400000ull, 0},
        {"7a1c400000-7a1c401000 rw-s 00000000 00:0f 23                             anon_inode:kgsl", 1, 0x7a1c400000ull, 1},
        {"", 0, 0, 0}, {"garbage", 0, 0, 0}, {"12345", 0, 0, 0},
    };
    for (unsigned i = 0; i < sizeof L / sizeof *L; i++) {
        uint64_t a = 0; int kg = -1, ok = vkmb_line(L[i].ln, strlen(L[i].ln), &a, &kg);
        CHECK(ok == L[i].ok && (!ok || (a == L[i].a && kg == L[i].kgsl)), "line %u: ok %d a %#llx kgsl %d", i, ok, (unsigned long long)a, kg);
    }
    // A line's segment
    uint64_t lo[] = {0x1000, 0x5000, 0x9000}, hi[] = {0x3000, 0x8000, 0xa000};
    CHECK(vkmb_seg(lo, hi, 3, 0x1000) == 0 && vkmb_seg(lo, hi, 3, 0x2fff) == 0 && vkmb_seg(lo, hi, 3, 0x3000) == -1, "the first segment and its end");
    CHECK(vkmb_seg(lo, hi, 3, 0x7000) == 1 && vkmb_seg(lo, hi, 3, 0x9fff) == 2 && vkmb_seg(lo, hi, 3, 0xa000) == -1 && vkmb_seg(lo, hi, 3, 0x10) == -1, "the others, and outside");
    CHECK(vkmb_seg(lo, hi, 0, 0x1000) == -1, "no segments");
    // A line's fields (DM_VK_MAPS_HIST)
    {
        uint64_t a, z; char p[5]; const char *nm; size_t nl;
        const char *l1 = "7a1c2d0000-7a1c2f1000 r-xp 00000000 fe:2f 2563                           /system/lib64/libc.so";
        CHECK(vkmb_fields(l1, strlen(l1), &a, &z, p, &nm, &nl) && a == 0x7a1c2d0000ull && z == 0x7a1c2f1000ull && !strcmp(p, "r-xp")
              && nl == strlen("/system/lib64/libc.so") && !memcmp(nm, "/system/lib64/libc.so", nl), "a file's line");
        const char *l2 = "7a1c400000-7a1c401000 rw-p 00000000 00:00 0 ";
        CHECK(vkmb_fields(l2, strlen(l2), &a, &z, p, &nm, &nl) && nl == 0 && !strcmp(p, "rw-p"), "plain anonymous memory: no name");
        const char *l3 = "7a1c400000-7a1c401000 rw-p 00000000 00:00 0                              [anon:stack_and_tls:12345]";
        CHECK(vkmb_fields(l3, strlen(l3), &a, &z, p, &nm, &nl) && nl == strlen("[anon:stack_and_tls:12345]"), "named anonymous memory");
        CHECK(!vkmb_fields("12345", 5, &a, &z, p, &nm, &nl) && !vkmb_fields("1-2", 3, &a, &z, p, &nm, &nl), "not lines");
        char k[VKMB_NAME];
        vkmb_norm("[anon:stack_and_tls:12345]", 26, k); CHECK(!strcmp(k, "[anon:stack_and_tls:#]"), "digits folded: %s", k);
        vkmb_norm("", 0, k); CHECK(!strcmp(k, "(anonymous)"), "empty name");
        CHECK(vkmb_bucket(4096) == 0 && vkmb_bucket(4097) == 1 && vkmb_bucket(1 << 20) == 2 && vkmb_bucket(300ull << 20) == 5, "size buckets");
    }
    // The histogram: groups by (name, permissions, size), the largest first
    {
        static vkmb_hist h; vkmb_hist_reset(&h); char ln[200];
        for (int i = 0; i < 300; i++) { snprintf(ln, sizeof ln, "%llx-%llx rw-p 00000000 00:00 0   [anon:stack_and_tls:%d]", 0x100000ull + i * 0x10000ull, 0x100000ull + i * 0x10000ull + 0x1000, 1000 + i); vkmb_hist_add(&h, ln, strlen(ln)); }
        for (int i = 0; i < 50; i++) { snprintf(ln, sizeof ln, "%llx-%llx ---p 00000000 00:00 0   [anon:stack_and_tls:%d]", 0x9000000ull + i * 0x10000ull, 0x9000000ull + i * 0x10000ull + 0x1000, i); vkmb_hist_add(&h, ln, strlen(ln)); }
        for (int i = 0; i < 120; i++) { snprintf(ln, sizeof ln, "%llx-%llx rw-s 00000000 00:0e 1066   /dev/kgsl-3d0", 0x20000000ull + i * 0x100000ull, 0x20000000ull + i * 0x100000ull + 0x20000); vkmb_hist_add(&h, ln, strlen(ln)); }
        int idx[10], n = vkmb_hist_top(&h, idx, 10);
        CHECK(n == 3 && h.lines == 470 && !h.dropped, "three groups (n %d, lines %ld)", n, h.lines);
        CHECK(n == 3 && h.e[idx[0]].n == 300 && !strcmp(h.e[idx[0]].name, "[anon:stack_and_tls:#]") && !strcmp(h.e[idx[0]].perms, "rw-p") && h.e[idx[0]].bucket == 0, "the stacks first");
        CHECK(n == 3 && h.e[idx[1]].n == 120 && h.e[idx[1]].bytes == 120 * 0x20000ull && h.e[idx[2]].n == 50, "then kgsl, then the guard pages");
        CHECK(vkmb_hist_top(&h, idx, 2) == 2 && h.e[idx[0]].n == 300 && h.e[idx[1]].n == 120, "top 2 of 3");
        vkmb_hist_reset(&h);   // more groups than the table: counted, not lost
        for (int i = 0; i < VKMB_HCAP + 10; i++) { snprintf(ln, sizeof ln, "1000-2000 rw-p 00000000 00:00 0   /data/f%c%c%c", 'a' + i % 26, 'a' + i / 26 % 26, 'a' + i / 676); vkmb_hist_add(&h, ln, strlen(ln)); }
        CHECK(h.dropped == 10 && h.lines == VKMB_HCAP + 10, "a full table counts the rest as dropped (%ld)", h.dropped);
    }
    printf("t_android_mapbudget: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
