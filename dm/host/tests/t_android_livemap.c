// The texture cache's live images by storage address (dm/gfx/vk_android/vka_livemap.h; vka_tex.c "Live images"): against a plain list walked as vka_tex.c walked it
// before, through random adds and removals with many images at one address, the chains offer exactly the live images of an address, in the order they came, the places
// stay consistent, and the table grows past its first size
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct img { const uint8_t *mem; int live_ix; struct img *mnext; int id, live; } img;
#define VKA_LIVE_T img
#include "../../gfx/vk_android/vka_livemap.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

enum { N = 6000, ADDRS = 700 };
static uint8_t arena[ADDRS * 64];
static img ims[N];
static int order[N], norder;   // the adds in order (a reference of what came when)

static void check_all(img **v, int n, const vkl_index *x, int with_table) {
    for (int i = 0; i < n; i++) CHECK(v[i]->live_ix == i + 1 && v[i]->live, "place %d: live_ix %d", i, v[i]->live_ix);
    int cnt = 0; for (int i = 0; i < N; i++) cnt += ims[i].live;
    CHECK(cnt == n, "live %d, list %d", cnt, n);
    for (int a = 0; a < ADDRS; a++) {
        const uint8_t *m = arena + a * 64;
        static int want[N]; int nw = 0;   // the live images of m: in the order they were added (with_table: each added once since the list was empty), else by count
        if (with_table) { for (int o = 0; o < norder; o++) { img *p = &ims[order[o]]; if (p->live && p->mem == m) want[nw++] = p->id; } }
        else for (int i = 0; i < N; i++) if (ims[i].live && ims[i].mem == m) want[nw++] = i;
        static int got[N]; int ng = 0, k;
        for (img *c = vkl_first(v, n, x, m, &k); c; c = vkl_next(v, n, x, c, m, &k)) if (c->mem == m) got[ng++] = c->id;
        CHECK(ng == nw, "address %d: %d offered, %d live", a, ng, nw);
        if (with_table && ng == nw) for (int j = 0; j < ng; j++) CHECK(got[j] == want[j], "address %d: %d-th is %d, %d came first", a, j, got[j], want[j]);
    }
}

int main(void) {
    img **v = NULL; int n = 0, cap = 0; vkl_index x = {0};
    for (int i = 0; i < N; i++) { ims[i].id = i; ims[i].mem = (rnd() % 23) ? arena + (rnd() % ADDRS) * 64 : NULL; }
    for (int step = 0; step < 40000; step++) {
        int i = (int)(rnd() % N); img *p = &ims[i];
        if (!p->live && (rnd() % 3)) { CHECK(vkl_add(&v, &n, &cap, &x, p), "add"); p->live = 1; }
        else if (p->live) { CHECK(vkl_del(v, &n, &x, p), "del"); p->live = 0; CHECK(!p->live_ix && !p->mnext, "removed image keeps its links"); }
        else CHECK(!vkl_del(v, &n, &x, p), "an image not live was removed");
        if (step % 4000 == 0) check_all(v, n, &x, 0);
    }
    check_all(v, n, &x, 0);
    CHECK(x.cap >= (size_t)n * 2 && x.cap >= 1024, "table %zu for %d images", x.cap, n);
    // In-order check: a fresh list where every add is in the reference
    for (int i = 0; i < N; i++) if (ims[i].live) { CHECK(vkl_del(v, &n, &x, &ims[i]), "drain"); ims[i].live = 0; }
    CHECK(n == 0, "drained to %d", n);
    norder = 0;
    for (int i = 0; i < 3000; i++) { int j = (int)(rnd() % N); if (ims[j].live) continue; CHECK(vkl_add(&v, &n, &cap, &x, &ims[j]), "add"); ims[j].live = 1; order[norder++] = j; }
    for (int i = 0; i < 400; i++) { int j = order[rnd() % norder]; if (ims[j].live) { vkl_del(v, &n, &x, &ims[j]); ims[j].live = 0; } }
    check_all(v, n, &x, 1);
    // No table (its allocation failed): every live image is offered, the caller filters
    vkl_index none = {0}; int k, seen = 0;
    for (img *c = vkl_first(v, n, &none, arena, &k); c; c = vkl_next(v, n, &none, c, arena, &k)) seen++;
    CHECK(seen == n, "without a table %d of %d offered", seen, n);
    free(v); free(x.b);
    if (fails) { printf("t_android_livemap: %d failures\n", fails); return 1; }
    printf("t_android_livemap: ok (%d live at the end, table %zu)\n", n, x.cap);
    return 0;
}
