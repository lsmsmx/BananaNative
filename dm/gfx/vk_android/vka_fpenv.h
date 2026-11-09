// The host's floating-point environment on the Vulkan replay's side (Android). The game's code runs natively with the FPCR its SDK sets (Ocarina of Time's threads:
// 0x1000000, FZ = flush-to-zero), and the gate saves and restores it around the host (dm/host/entry.S) but does not change it: the host's code a gate runs (NVN, HLE)
// runs with the game's FPCR, and a thread created there inherits it (Linux copies the creating thread's FP state on clone). The replay's threads are created from
// the game's NVN calls, and the driver's shader compiler runs on them (or on a game thread for an inline replay): with flush-to-zero, Qualcomm's compiler makes
// pipelines whose results differ from the same SPIR-V compiled with FPCR 0 (docs/reference/rendering.md "Vulkan compiles and replay threads ran with the game's
// flush-to-zero FPCR"). So every Vulkan-side thread the host creates sets the host's FPCR (0: round to nearest, no flush-to-zero, IEEE NaNs) when it starts, and the
// calls that compile (device creation, shader modules, pipelines) and an inline replay set it around themselves on any thread and put the caller's back afterwards.
// DM_VK_GUEST_FPCR=1 (diagnostic, restart) keeps the inherited FPCR everywhere, as before 2026-11-08. The idea and the measurement are Swap2-Native's (MIT); this is
// our own implementation. The parts here are free of Android and Vulkan so the Mac's unit test (dm/host/tests/t_android_fpenv.c) can check them
#ifndef VKA_FPENV_H
#define VKA_FPENV_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define VKA_HOST_FPCR 0ull   // a Linux process starts with FPCR 0, and so do the threads ART makes
#define VKA_FPCR_FZ (1ull << 24)

static inline uint64_t vka_fpcr_get(void) {
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v;
#else
    return VKA_HOST_FPCR;
#endif
}
static inline void vka_fpcr_set(uint64_t v) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" :: "r"(v));
#else
    (void)v;
#endif
}
// DM_VK_GUEST_FPCR's value: 1 = keep the inherited FPCR (the diagnostic)
static inline int vka_fp_guest_env(const char *e) { return e && atoi(e) > 0; }
// The host's FPCR on this thread unless guest (DM_VK_GUEST_FPCR=1); returns the FPCR the thread had, for vka_fp_leave
static inline uint64_t vka_fp_enter(int guest) {
    uint64_t v = vka_fpcr_get();
    if (!guest && v != VKA_HOST_FPCR) vka_fpcr_set(VKA_HOST_FPCR);
    return v;
}
static inline void vka_fp_leave(uint64_t was) { if (vka_fpcr_get() != was) vka_fpcr_set(was); }

// The pipeline cache's generation, in its file name: a change in how pipelines are compiled that the cache's header (vendor, device, the driver's pipelineCacheUUID)
// cannot show takes a new one, so the files of the old one are not read again. "-fp0" (2026-11-08): compiled with the host's FPCR; the files before it (no generation)
// held pipelines compiled with the game's flush-to-zero FPCR
#define VKA_PCACHE_GEN "-fp0"
// The default pipeline cache file in dir: vk_pipelines[-<driver tag>]<gen>.bin (gen "" = the name before generations). Returns snprintf's length
static inline int vka_pcache_name(char *out, size_t n, const char *dir, const char *tag, const char *gen) {
    return snprintf(out, n, "%s/vk_pipelines%s%s%s.bin", dir, tag ? "-" : "", tag ? tag : "", gen ? gen : "");
}
#endif
