// The skipped-work warning (DM_VK_SKIP_WARN; android/README.md "In-game notices"): when the replay skips a large share of a game's draws and dispatches for causes that
// point to the GPU or its driver, the player is told once per run that another driver may draw the picture. Pure counters and arithmetic, free of Android and Vulkan so the
// Mac's unit test (dm/host/tests/t_android_skipwarn.c) can check them. Only causes of the device count: a pipeline or module the driver refused, a feature the device lacks.
// Skips the game or a profile asks for (render conditions, empty draws, cuts) and ones of the replay's own state never do.
#ifndef VKA_SKIPWARN_H
#define VKA_SKIPWARN_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { VKS_MESH, VKS_SHADER, VKS_PIPELINE, VKS_FEATURE, VKS_N };
static const char *const vks_name[VKS_N] = {"mesh draws without VK_EXT_mesh_shader", "shaders refused", "pipelines not made", "features the device lacks"};
enum { VKS_WINDOW = 300, VKS_FIRST = 900, VKS_MIN_WORK = 200, VKS_DEFAULT_PCT = 5 };   // frames in a window; no verdict before this frame; fewer draws in a window say nothing

typedef struct {
    uint64_t f0, work0, other0, dev0[VKS_N];   // frame and counters at the window's start
    int strikes, fired, pct;                   // windows in a row over the threshold; warned; the share (%) that warned
    char causes[224];                          // what the warning named
} vkskip;

// Called with the cumulative counters each frame: work = draws and dispatches that ran, dev = device-caused skips by cause, other = every other skip. Returns 1 once, at
// the second window in a row whose share of device-caused skips in all draws and dispatches attempted is over threshold_pct (0 = never), with pct, causes and the frame
// count in s. A window with fewer than VKS_MIN_WORK attempts (a loading screen) says nothing and breaks the row.
static inline int vkskip_step(vkskip *s, uint64_t frame, uint64_t work, const uint64_t dev[VKS_N], uint64_t other, int threshold_pct) {
    if (threshold_pct <= 0 || s->fired) return 0;
    if (!s->f0) { s->f0 = frame ? frame : 1; s->work0 = work; s->other0 = other; memcpy(s->dev0, dev, sizeof s->dev0); return 0; }
    if (frame < s->f0 + VKS_WINDOW) return 0;
    uint64_t d[VKS_N], dt = 0; for (int i = 0; i < VKS_N; i++) { d[i] = dev[i] - s->dev0[i]; dt += d[i]; }
    uint64_t all = (work - s->work0) + dt + (other - s->other0);
    int over = all >= VKS_MIN_WORK && dt * 100 > all * (uint64_t)threshold_pct;
    int pct = all ? (int)((dt * 100 + all / 2) / all) : 0;
    int ret = 0;
    if (!over || frame < VKS_FIRST) s->strikes = 0;
    else if (++s->strikes >= 2) {
        s->fired = 1; s->pct = pct < 1 ? 1 : pct; ret = 1;
        char *o = s->causes; size_t n = sizeof s->causes; o[0] = 0;
        for (int k = 0; k < VKS_N; k++) {   // largest first, at most three
            int b = -1; for (int i = 0; i < VKS_N; i++) if (d[i] && (b < 0 || d[i] > d[b])) b = i;
            if (b < 0 || k == 3) break;
            size_t l = strlen(o); snprintf(o + l, n - l, "%s%s %llu", l ? ", " : "", vks_name[b], (unsigned long long)d[b]); d[b] = 0;
        }
    }
    s->f0 = frame; s->work0 = work; s->other0 = other; memcpy(s->dev0, dev, sizeof s->dev0);
    return ret;
}
#endif
