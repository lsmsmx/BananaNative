// The texture cache's limit and eviction policy (dm/gfx/vk_android/vka_texlimit.h): the limit follows RAM and the heap budget, the override-free floor holds, images used in
// the current or the previous frame are never candidates, and a simulated working set larger than the limit is not uploaded again frame after frame (the thrash the old
// array-order, age-1 policy caused: Mario Kart World at a 1,536 MiB limit, Ocarina of Time at 512 MiB); a 12 GB device keeps an eighth of RAM, and memory pressure
// (PSI) stops the cache's growth, then shrinks it
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_texlimit.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MIB VKTL_MIB
#define GIB (1024 * MIB)

// A cache of n images of one size, a frame using `use` of them (ids 0..use-1, in a rotating order), evicting by the policy whenever over the limit, as vka_tex.c does on a miss
typedef struct { uint64_t last; int live, gpu; } img;
static int cmp(const void *a, const void *b) { const img *x = *(img *const *)a, *y = *(img *const *)b; return vktl_order(x->gpu, x->last, y->gpu, y->last); }
static int run(int n, int use, uint64_t each, uint64_t limit, int frames, int *uploads_last) {
    img *im = calloc((size_t)n, sizeof *im); img **c = malloc((size_t)n * sizeof *c); uint64_t bytes = 0; int uploads = 0;
    for (uint64_t f = 1; f <= (uint64_t)frames; f++) {
        int up = 0;
        for (int k = 0; k < use; k++) {
            int id = (int)((k + f * 7) % (uint64_t)use);
            if (!im[id].live) {   // a miss: prune, then make it
                if (bytes > limit) {
                    int nc = 0;
                    for (int i = 0; i < n; i++) if (im[i].live && vktl_evictable(f, im[i].last, 0, im[i].gpu, 0, 0)) c[nc++] = &im[i];
                    qsort(c, (size_t)nc, sizeof *c, cmp);
                    for (int i = 0; i < nc && bytes > vktl_target(limit); i++) { c[i]->live = 0; bytes -= each; }
                }
                im[id].live = 1; bytes += each; up++;
            }
            im[id].last = f;
        }
        uploads += up; if (f == (uint64_t)frames) *uploads_last = up;
    }
    free(im); free(c); return uploads;
}

int main(void) {
    // the limit: images plus what RAM can still give past the reserve, the budget's share past other usage, the floor
    vktl_in a = {.mem_total = 15 * GIB, .mem_avail = 4 * GIB, .img_bytes = 1 * GIB};
    CHECK(vktl_reserve(15 * GIB) == 512 * MIB, "a 15 GiB phone keeps 512 MiB");
    CHECK(vktl_limit(&a) == 1 * GIB + 4 * GIB - 512 * MIB, "RAM: images + MemAvailable - reserve (%llu MiB)", (unsigned long long)(vktl_limit(&a) >> 20));
    CHECK(vktl_reserve(64 * GIB) == 2 * GIB, "a 64 GiB machine keeps a 32nd");
    vktl_in b = a; b.heap_budget = 6 * GIB; b.heap_usage = 4 * GIB;   // 3 GiB of other use: 6 - 6/16 - 3 = 2.625 GiB
    CHECK(vktl_limit(&b) == 6 * GIB - 6 * GIB / 16 - 3 * GIB, "the budget's share is the smaller (%llu MiB)", (unsigned long long)(vktl_limit(&b) >> 20));
    vktl_in h = {.heap_size = 12 * GIB}; CHECK(vktl_limit(&h) == 6 * GIB, "no budget, no RAM figures: half the heap");
    vktl_in z = {0}; CHECK(vktl_limit(&z) == VKTL_FLOOR, "nothing known: the floor");
    vktl_in lo = {.mem_total = 4 * GIB, .mem_avail = 300 * MIB, .img_bytes = 100 * MIB}; CHECK(vktl_limit(&lo) == VKTL_FLOOR, "a tight phone: not under the floor");
    vktl_in full = {.mem_total = 15 * GIB, .mem_avail = 700 * MIB, .img_bytes = 3 * GIB}; CHECK(vktl_limit(&full) == 3 * GIB + 188 * MIB, "Mario Kart World's measured state: about its images");

    // a 12 GB device (MemTotal 11 GiB) keeps an eighth; 16 GB devices keep the old rule
    CHECK(vktl_reserve(11 * GIB) == 11 * GIB / 8, "an 11 GiB device keeps an eighth (%llu MiB)", (unsigned long long)(vktl_reserve(11 * GIB) >> 20));
    CHECK(vktl_reserve(12 * GIB) == 12 * GIB / 8 && vktl_reserve(12 * GIB + MIB) == 512 * MIB, "12 GiB is the edge");
    // the Portal's Zelda run: 6.8 GiB available at the start, so the old reserve let the images take nearly all of it
    vktl_in portal = {.mem_total = 11 * GIB, .mem_avail = 6800 * MIB, .img_bytes = 0};
    CHECK(vktl_limit(&portal) == 6800 * MIB - 11 * GIB / 8, "a fresh start on the Portal: MemAvailable less an eighth (%llu MiB)", (unsigned long long)(vktl_limit(&portal) >> 20));
    // memory pressure: below the hold mark nothing changes; at it the limit is what the cache holds; past the shrink mark an eighth less; never under the floor
    vktl_in p0 = {.mem_total = 11 * GIB, .mem_avail = 4 * GIB, .img_bytes = 2 * GIB, .psi_some10 = 1.0};
    CHECK(vktl_limit(&p0) == 2 * GIB + 4 * GIB - 11 * GIB / 8, "light pressure: the RAM rule (%llu MiB)", (unsigned long long)(vktl_limit(&p0) >> 20));
    vktl_in p1 = p0; p1.psi_some10 = VKTL_PSI_HOLD;
    CHECK(vktl_limit(&p1) == 2 * GIB, "stalling: no growth past what it holds (%llu MiB)", (unsigned long long)(vktl_limit(&p1) >> 20));
    vktl_in p2 = p0; p2.psi_some10 = 40.0;
    CHECK(vktl_limit(&p2) == 2 * GIB - 2 * GIB / 8, "stalling hard: an eighth back (%llu MiB)", (unsigned long long)(vktl_limit(&p2) >> 20));
    vktl_in p3 = {.mem_total = 11 * GIB, .mem_avail = 4 * GIB, .img_bytes = 300 * MIB, .psi_some10 = 40.0};
    CHECK(vktl_limit(&p3) == VKTL_FLOOR, "pressure never goes under the floor");
    vktl_in p4 = p0; p4.psi_some10 = -1;
    CHECK(vktl_limit(&p4) == vktl_limit(&p0), "pressure not known: the RAM rule");
    vktl_in p5 = {.mem_total = 11 * GIB, .mem_avail = 700 * MIB, .img_bytes = 2 * GIB, .psi_some10 = VKTL_PSI_HOLD};
    CHECK(vktl_limit(&p5) == 2 * GIB, "less available than the reserve: the RAM rule holds the images where they are, it never takes any back (%llu MiB)", (unsigned long long)(vktl_limit(&p5) >> 20));
    p5.psi_some10 = VKTL_PSI_SHRINK; CHECK(vktl_limit(&p5) == 2 * GIB - 2 * GIB / 8, "only pressure takes images back");

    // candidates
    CHECK(!vktl_evictable(100, 100, 0, 0, 0, 0), "used this frame: kept");
    CHECK(!vktl_evictable(100, 99, 0, 0, 0, 0), "used the previous frame: kept");
    CHECK(vktl_evictable(100, 98, 0, 0, 0, 0), "two frames unused: a candidate");
    CHECK(!vktl_evictable(100, 50, 1, 0, 0, 0), "held: kept");
    CHECK(!vktl_evictable(100, 50, 0, 0, 1, 0), "a playing video's: kept");
    CHECK(!vktl_evictable(100, 50, 0, 0, 0, 1), "its submission not done: kept");
    CHECK(!vktl_evictable(100, 0, 0, 0, 0, 0), "never stamped: kept");
    CHECK(!vktl_evictable(100, 50, 0, 1, 0, 0) && vktl_evictable(200, 50, 0, 1, 0, 0), "GPU content: only after VKTL_GPU_FRAMES");
    CHECK(vktl_order(0, 90, 1, 10) < 0 && vktl_order(0, 10, 0, 90) < 0 && vktl_order(1, 10, 1, 90) < 0, "uploadable before GPU-only, oldest first");

    // A working set (1,800 MiB, 900 images of 2 MiB) over a 1,536 MiB limit: uploaded once, then kept over the limit, not again every frame
    int last = -1, up = run(1200, 900, 2 * MIB, 1536 * MIB, 120, &last);
    CHECK(up == 900 && last == 0, "a working set over the limit: %d uploads, %d in the last frame", up, last);
    // Under the limit: one upload each
    up = run(1200, 600, 2 * MIB, 1536 * MIB, 120, &last);
    CHECK(up == 600 && last == 0, "under the limit: one upload each (%d, last frame %d)", up, last);
    if (!fails) printf("t_android_texlimit: ok\n");
    return fails != 0;
}
