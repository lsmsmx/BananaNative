// The skipped-work warning's window logic (dm/gfx/vk_android/vka_skipwarn.h): device-caused skips over a threshold in two windows in a row, not before the first frames,
// not for a window with little work, once, with the causes named largest first
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_skipwarn.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A run fed one window at a time: each frame count step is VKS_WINDOW; per window `ok` draws ran, dev[] device skips, `oth` other skips
typedef struct { vkskip s; uint64_t f, work, dev[VKS_N], other; } run;
static int win(run *r, uint64_t ok, uint64_t mesh, uint64_t shader, uint64_t pipe, uint64_t oth, int th) {
    r->f += VKS_WINDOW; r->work += ok; r->dev[VKS_MESH] += mesh; r->dev[VKS_SHADER] += shader; r->dev[VKS_PIPELINE] += pipe; r->other += oth;
    return vkskip_step(&r->s, r->f, r->work, r->dev, r->other, th);
}

int main(void) {
    // the first call only starts the window; a window over the threshold after the first frames warns at the second in a row (here: 10 % skipped)
    run r = {0}; r.f = 1500;
    CHECK(vkskip_step(&r.s, r.f, 0, r.dev, 0, 5) == 0, "first call starts the window");
    CHECK(win(&r, 9000, 1000, 0, 0, 0, 5) == 0 && r.s.strikes == 1, "one window over is not enough");
    CHECK(win(&r, 9000, 0, 500, 500, 0, 5) == 1 && r.s.fired && r.s.pct == 10, "second window over warns: %d percent", r.s.pct);
    CHECK(strstr(r.s.causes, "shaders refused 500") && strstr(r.s.causes, "pipelines not made 500") && !strstr(r.s.causes, "mesh"), "causes of the last window: '%s'", r.s.causes);
    CHECK(win(&r, 1000, 9000, 0, 0, 0, 5) == 0, "once per run");

    // largest cause first, at most three named
    run q = {0}; q.f = 1500; vkskip_step(&q.s, q.f, 0, q.dev, 0, 5);
    win(&q, 1000, 100, 50, 300, 0, 5);
    CHECK(win(&q, 1000, 100, 50, 300, 0, 5) == 1 && !strncmp(q.s.causes, "pipelines not made 300, mesh", 28), "order: '%s'", q.s.causes);
    CHECK(q.s.pct == 31, "pct %d", q.s.pct);

    // under the threshold, or a window in between under it, breaks the row
    run u = {0}; u.f = 1500; vkskip_step(&u.s, u.f, 0, u.dev, 0, 5);
    CHECK(win(&u, 9000, 1000, 0, 0, 0, 5) == 0 && win(&u, 9900, 100, 0, 0, 0, 5) == 0 && u.s.strikes == 0, "1 percent does not count and resets");
    CHECK(win(&u, 9000, 1000, 0, 0, 0, 5) == 0 && u.s.strikes == 1, "the row starts again");

    // skips of other causes (a game's own, profile cuts) are not the device's: many of them never warn, but they dilute the share
    run o = {0}; o.f = 1500; vkskip_step(&o.s, o.f, 0, o.dev, 0, 5);
    for (int i = 0; i < 4; i++) CHECK(win(&o, 5000, 0, 0, 0, 5000, 5) == 0, "other skips never warn");
    run d = {0}; d.f = 1500; vkskip_step(&d.s, d.f, 0, d.dev, 0, 5);
    win(&d, 5000, 0, 0, 400, 5000, 5);
    CHECK(win(&d, 5000, 0, 0, 400, 5000, 5) == 0 && d.s.strikes == 0, "diluted: 400 of 10400 is 3.8 percent");

    // not in the first frames: windows before VKS_FIRST do not count, and a warning needs two windows after
    run e = {0}; e.f = 0; vkskip_step(&e.s, 1, 0, e.dev, 0, 5); e.f = 1;
    int w = 0; for (int i = 0; i < 2 && !w; i++) { w = win(&e, 9000, 1000, 0, 0, 0, 5); CHECK(!(w && e.f < VKS_FIRST), "no warning before frame %d (at %llu)", VKS_FIRST, (unsigned long long)e.f); }
    CHECK(!w && e.s.strikes == 0, "the first windows count nothing");
    CHECK(win(&e, 9000, 1000, 0, 0, 0, 5) == 0 && win(&e, 9000, 1000, 0, 0, 0, 5) == 1, "then two windows warn");

    // a loading screen (little work) says nothing and breaks the row
    run l = {0}; l.f = 1500; vkskip_step(&l.s, l.f, 0, l.dev, 0, 5);
    win(&l, 9000, 1000, 0, 0, 0, 5);
    CHECK(win(&l, 50, 50, 0, 0, 0, 5) == 0 && l.s.strikes == 0, "a window with little work resets");

    // off
    run z = {0}; z.f = 1500; CHECK(vkskip_step(&z.s, z.f, 0, z.dev, 0, 0) == 0 && z.s.f0 == 0, "0 = off");
    for (int i = 0; i < 4; i++) CHECK(win(&z, 1000, 9000, 0, 0, 0, 0) == 0, "off never warns");
    // a higher threshold
    run h = {0}; h.f = 1500; vkskip_step(&h.s, h.f, 0, h.dev, 0, 20);
    CHECK(win(&h, 9000, 1000, 0, 0, 0, 20) == 0 && win(&h, 9000, 1000, 0, 0, 0, 20) == 0 && !h.s.fired, "10 percent under a threshold of 20");
    // a small buffer for the causes cannot overflow: the longest three names fit
    CHECK(sizeof(((vkskip *)0)->causes) >= 3 * 64, "causes buffer");

    if (fails) { printf("t_android_skipwarn: %d failed\n", fails); return 1; }
    printf("t_android_skipwarn: ok\n");
    return 0;
}
