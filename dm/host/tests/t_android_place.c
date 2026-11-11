// Large memory pools placed by parts (dm/gfx/vk_android/vka_place.h): which pools are placed by parts, and the part an ask places: the granules it touches, counted from
// the pool's start and clipped to it; an ask over the whole pool (or outside it) places the whole pool. Joining parts: the run of segments an ask across parts lies in
// (vkp_join), and a model of placing by parts with joins (vka_mem.c mtl_pool_need, cover, vka_mem_join_due) in which every ask, straddling ones included, ends in one segment
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/vk_android/vka_place.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MIB (1ull << 20)


// The model: the pool's segments as vka_mem.c's table holds them (sorted, disjoint), placed as cover does (each gap in the part a segment of its own) and joined as
// vka_mem_join_due does (the run vkp_join names becomes one segment)
typedef struct { uint64_t lo, hi; } seg;
enum { MAXSEG = 4096 };
static seg g_s[MAXSEG]; static int g_n;
static void ins(uint64_t lo, uint64_t hi) {
    int i = 0; while (i < g_n && g_s[i].lo < lo) i++;
    memmove(&g_s[i + 1], &g_s[i], (size_t)(g_n - i) * sizeof *g_s); g_s[i] = (seg){lo, hi}; g_n++;
}
static void cover(uint64_t lo, uint64_t hi) {
    for (uint64_t a = lo; a < hi;) {
        int k = -1; for (int i = 0; i < g_n; i++) if (g_s[i].lo <= a && a < g_s[i].hi) k = i;
        if (k >= 0) { a = g_s[k].hi; continue; }
        uint64_t e = hi; for (int i = 0; i < g_n; i++) if (g_s[i].lo > a && g_s[i].lo < e) e = g_s[i].lo;
        ins(a, e); a = e;
    }
}
static void joinrun(int k0, int k1) { g_s[k0].hi = g_s[k1].hi; memmove(&g_s[k0 + 1], &g_s[k1 + 1], (size_t)(g_n - k1 - 1) * sizeof *g_s); g_n -= k1 - k0; }
static uint64_t rnd(uint64_t *x) { *x ^= *x << 13; *x ^= *x >> 7; *x ^= *x << 17; return *x; }
// One ask as mtl_pool_need and the next submission's join handle it; returns the segments the ask lay across before the join (1: none)
static int ask(uint64_t mem, uint64_t size, uint64_t a, uint64_t e, uint64_t G, int join_on) {
    uint64_t sa = a > mem ? a : mem, se = e < mem + size ? e : mem + size, lo, hi; int k0, k1;
    if (vkp_join(g_s, sizeof *g_s, g_n, sa, se, &k0, &k1) == 1) return 1;   // placed already, in one piece
    if (!vkp_part(mem, size, a, e, G, &lo, &hi)) return 0;
    cover(lo, hi);
    int c = vkp_join(g_s, sizeof *g_s, g_n, sa, se, &k0, &k1);
    if (c > 1 && join_on) joinrun(k0, k1);
    return c;
}

int main(void) {
    const uint64_t G = 64 * MIB, MIN = 1024 * MIB;
    // which pools
    CHECK(vkp_by_parts(0xdac00000ull, MIN, G), "Pokopia's 3.4 GiB pool is placed by parts");
    CHECK(!vkp_by_parts(0x14c00000ull, MIN, G), "a 332 MiB pool is placed whole");
    CHECK(!vkp_by_parts(0xdac00000ull, 0, G), "part minimum 0: whole");
    CHECK(!vkp_by_parts(96 * MIB, 64 * MIB, G), "less than two granules: whole");
    CHECK(vkp_by_parts(128 * MIB, 64 * MIB, G), "two granules: by parts");

    // the part: Pokopia's first ask, a buffer 0x25c0000 into the pool (the granule it lies in)
    uint64_t mem = 0x41479fb000ull, size = 0xdac00000ull, lo = 0, hi = 0;
    CHECK(vkp_part(mem, size, mem + 0x25c0000, mem + 0x25c0000 + 0x10000, G, &lo, &hi) && lo == mem && hi == mem + G, "first granule: %#llx-%#llx", (unsigned long long)lo, (unsigned long long)hi);
    // an ask across a granule's end takes both granules
    CHECK(vkp_part(mem, size, mem + G - 4096, mem + G + 4096, G, &lo, &hi) && lo == mem && hi == mem + 2 * G, "two granules");
    // an ask that ends on a granule's end takes no more
    CHECK(vkp_part(mem, size, mem + G, mem + 2 * G, G, &lo, &hi) && lo == mem + G && hi == mem + 2 * G, "exactly one granule");
    // the last granule is short (3,500 MiB = 54 granules + 44 MiB): clipped to the pool
    CHECK(vkp_part(mem, size, mem + size - 4096, mem + size, G, &lo, &hi) && lo == mem + 54 * G && hi == mem + size, "last, short granule: %#llx-%#llx", (unsigned long long)lo, (unsigned long long)hi);
    // an ask past the pool's end is clipped to it
    CHECK(vkp_part(mem, size, mem + size - 4096, mem + size + G, G, &lo, &hi) && lo == mem + 54 * G && hi == mem + size, "past the end: clipped");
    // an ask that starts before the pool is clipped to its start
    CHECK(vkp_part(mem, size, mem - 4096, mem + 4096, G, &lo, &hi) && lo == mem && hi == mem + G, "before the start: clipped");
    // whole-pool asks (nvnMemoryPoolGetBufferAddress) and asks outside: 0, the whole pool
    CHECK(!vkp_part(mem, size, mem, mem + size, G, &lo, &hi), "the pool's own range: whole");
    CHECK(!vkp_part(mem, size, mem - G, mem + size + G, G, &lo, &hi), "more than the pool: whole");
    CHECK(!vkp_part(mem, size, mem + size, mem + size + 4096, G, &lo, &hi), "after the pool: not a part");
    CHECK(!vkp_part(mem, size, mem - 8192, mem, G, &lo, &hi), "before the pool: not a part");
    CHECK(!vkp_part(mem, size, mem + 4096, mem + 4096, G, &lo, &hi), "empty ask: not a part");
    // a pool of two granules: an ask over both is the whole pool
    CHECK(!vkp_part(mem, 2 * G, mem + 4096, mem + 2 * G - 4096, G, &lo, &hi), "both granules of a two-granule pool: whole");
    // an ask near the top of the address space does not wrap
    uint64_t top = 0xffffffffffff0000ull - 4 * G;
    CHECK(vkp_part(top, 4 * G, top + 3 * G + 4096, top + 4 * G, G, &lo, &hi) && lo == top + 3 * G && hi == top + 4 * G, "near the top");

    // vkp_join: the run of segments that holds a range
    { seg t[] = {{0x1000, 0x2000}, {0x2000, 0x3000}, {0x3000, 0x5000}, {0x6000, 0x7000}}; int k0 = -1, k1 = -1;
      CHECK(vkp_join(t, sizeof *t, 4, 0x1100, 0x1200, &k0, &k1) == 1 && k0 == 0 && k1 == 0, "inside one segment");
      CHECK(vkp_join(t, sizeof *t, 4, 0x1000, 0x2000, &k0, &k1) == 1 && k0 == 0, "a whole segment, to its end");
      CHECK(vkp_join(t, sizeof *t, 4, 0x1f00, 0x2100, &k0, &k1) == 2 && k0 == 0 && k1 == 1, "across two segments that touch");
      CHECK(vkp_join(t, sizeof *t, 4, 0x1f00, 0x4000, &k0, &k1) == 3 && k0 == 0 && k1 == 2, "across three");
      CHECK(vkp_join(t, sizeof *t, 4, 0x4f00, 0x6100, &k0, &k1) == 0, "across a gap: not all placed");
      CHECK(vkp_join(t, sizeof *t, 4, 0x0f00, 0x1100, &k0, &k1) == 0, "starting before the first segment");
      CHECK(vkp_join(t, sizeof *t, 4, 0x6f00, 0x7100, &k0, &k1) == 0, "past the last segment's end");
      CHECK(vkp_join(t, sizeof *t, 4, 0x5000, 0x5100, &k0, &k1) == 0, "in the gap");
      CHECK(vkp_join(t, sizeof *t, 4, 0x1100, 0x1100, &k0, &k1) == 0, "empty range");
      CHECK(vkp_join(t, sizeof *t, 0, 0x1100, 0x1200, &k0, &k1) == 0, "no segments");
      struct { uint64_t lo, hi; void *mem, *buf; int refs; } w[] = {{0x1000, 0x2000, 0, 0, 1}, {0x2000, 0x3000, 0, 0, 1}};   // a stride of vka_mem.c's seg_t's kind
      CHECK(vkp_join(w, sizeof *w, 2, 0x1800, 0x2800, &k0, &k1) == 2 && k0 == 0 && k1 == 1, "a wider stride"); }

    // Pokopia's straddle (v0.2.6 log, 16 MiB granules): a part placed earlier, then a buffer from its last granule into the next one
    { const uint64_t g = 16 * MIB; uint64_t pm = 0x41479fb000ull, ps = 0xdac00000ull, b = 0x41a99dab00ull, bl = 0x20a60; g_n = 0;
      uint64_t gi = (b - pm) / g; CHECK(b + bl > pm + (gi + 1) * g, "the buffer crosses a granule's end");
      CHECK(ask(pm, ps, b - 0x100000, b - 0xff000, g, 1) == 1 && g_n == 1, "the first part: its granule");
      int c = ask(pm, ps, b, b + bl, g, 0); int k0, k1;
      CHECK(c == 2 && g_n == 2, "without joins the buffer lies across two segments (%d, %d segments)", c, g_n);
      CHECK(vkp_join(g_s, sizeof *g_s, g_n, b, b + bl, &k0, &k1) == 2, "a lookup over it needs two");
      g_n = 0; ask(pm, ps, b - 0x100000, b - 0xff000, g, 1);
      CHECK(ask(pm, ps, b, b + bl, g, 1) == 2 && g_n == 1 && vkp_join(g_s, sizeof *g_s, g_n, b, b + bl, &k0, &k1) == 1, "joined: one segment holds it");
      CHECK(g_s[0].lo == pm + gi * g && g_s[0].hi == pm + (gi + 2) * g, "the joined segment is the two granules");
      // both parts placed by two other asks first, then a buffer over both: nothing to place (mtl_pool_need's join_placed), still joined
      g_n = 0; uint64_t edge = pm + (gi + 1) * g;
      ask(pm, ps, edge - 0x2000, edge - 0x1000, g, 1); ask(pm, ps, edge + 0x1000, edge + 0x2000, g, 1);
      CHECK(g_n == 2, "two parts from two asks");
      CHECK(ask(pm, ps, edge - 0x800, edge + 0x800, g, 1) == 2 && g_n == 1 && vkp_join(g_s, sizeof *g_s, g_n, edge - 0x800, edge + 0x800, &k0, &k1) == 1, "a buffer over two parts placed earlier: joined"); }

    // The model over many asks (buffers of 256 B to 48 MiB anywhere in a 3.4 GiB pool of 16 MiB granules): every ask lies in one segment once its submission begins,
    // the segments stay sorted, disjoint and granule-aligned, and they cover exactly the granules an ask touched
    { const uint64_t g = 16 * MIB, pm = 0x41479fb000ull, ps = 0xdac00000ull; const uint64_t ng = (ps + g - 1) / g;
      static uint8_t touched[256]; uint64_t x = 0x9e3779b97f4a7c15ull; int nstraddle = 0, nask = 0, bad = 0;
      static uint64_t as[400], es[400];
      g_n = 0; memset(touched, 0, sizeof touched);
      for (int i = 0; i < 400; i++) {
          uint64_t len = (rnd(&x) % 32 == 0) ? 1 + rnd(&x) % (48 * MIB) : 256 + rnd(&x) % (512 * 1024), off = rnd(&x) % ps;
          if (off + len > ps) len = ps - off;
          uint64_t a = pm + off, e = a + len; as[nask] = a; es[nask++] = e;
          for (uint64_t k = off / g; k <= (off + len - 1) / g; k++) touched[k] = 1;
          if (ask(pm, ps, a, e, g, 1) > 1) nstraddle++;
          int k0, k1; if (vkp_join(g_s, sizeof *g_s, g_n, a, e, &k0, &k1) != 1) bad++;
      }
      CHECK(!bad, "%d asks not in one segment after their join", bad);
      CHECK(nstraddle > 0, "the model met straddles (%d)", nstraddle);
      int k0, k1; bad = 0; for (int i = 0; i < nask; i++) if (vkp_join(g_s, sizeof *g_s, g_n, as[i], es[i], &k0, &k1) != 1) bad++;
      CHECK(!bad, "%d earlier asks no longer in one segment at the end", bad);
      uint64_t cov = 0; for (int i = 0; i < g_n; i++) {
          CHECK(g_s[i].lo < g_s[i].hi && (i == 0 || g_s[i - 1].hi <= g_s[i].lo), "segment %d sorted and disjoint", i);
          CHECK((g_s[i].lo - pm) % g == 0 && ((g_s[i].hi - pm) % g == 0 || g_s[i].hi == pm + ps), "segment %d granule-aligned", i);
          cov += g_s[i].hi - g_s[i].lo; }
      uint64_t want = 0; for (uint64_t k = 0; k < ng; k++) if (touched[k]) want += (k + 1) * g <= ps ? g : ps - k * g;
      CHECK(cov < ps, "part of the pool stays not placed (%llu MiB placed)", (unsigned long long)(cov / MIB));
      CHECK(cov == want, "placed %llu MiB, the granules touched %llu MiB", (unsigned long long)(cov / MIB), (unsigned long long)(want / MIB));
      printf("t_android_place: model: %d asks, %d across parts, all joined; %d segments, %llu MiB placed\n", nask, nstraddle, g_n, (unsigned long long)(cov / MIB)); }

    if (fails) { printf("t_android_place: %d FAILED\n", fails); return 1; }
    printf("t_android_place: ok\n");
    return 0;
}
