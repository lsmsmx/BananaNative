// The texture cache's size and eviction policy (vka_tex.c vka_tex_prune_locked), free of Vulkan so the Mac's unit test (dm/host/tests/t_android_texlimit.c) can check it.
// The limit is the most image memory the device can hold without pushing the game or the system out of RAM, measured, not guessed: the images' own bytes plus what the
// process could still allocate (the smaller of Android's MemAvailable less a reserve and the image heap's budget, VK_EXT_memory_budget, less what else uses that heap), never
// under a floor, and no more than the images it holds while the system stalls on memory (vktl_pressure_limit). DM_VK_IMG_LIMIT_MB, when set, replaces it. Eviction is least recently used first and never takes an image used in the current or the previous frame: when
// everything over the limit is in use the cache stays over it (a working set larger than the limit would otherwise be uploaded again every frame: the thrash
// docs/reference/memory.md "Texture cache thrash" describes)
#ifndef VKA_TEXLIMIT_H
#define VKA_TEXLIMIT_H
#include <stdint.h>

#define VKTL_MIB (1ull << 20)
enum { VKTL_KEEP_FRAMES = 2,      // an image used this frame or the last stays (the minimum; DM_VK_IMG_KEEP_FRAMES raises it, see vktl_evictable)
       VKTL_GPU_FRAMES = 120 };   // an image whose content is only the GPU's (rendered, not uploaded) stays this long unused, and goes after every image that can be uploaded again
#define VKTL_FLOOR (512 * VKTL_MIB)

typedef struct {
    uint64_t mem_total, mem_avail;     // /proc/meminfo MemTotal and MemAvailable (0 = not known)
    uint64_t heap_size;                // the image memory type's heap (0 = not known)
    uint64_t heap_budget, heap_usage;  // VK_EXT_memory_budget for that heap (budget 0 = not available)
    uint64_t img_bytes;                // the cache's own image bytes now (part of mem_total - mem_avail and of heap_usage)
    double psi_some10;                 // /proc/pressure/memory "some avg10": the share of the last 10 s some task stalled on memory, in percent (< 0 = not known)
} vktl_in;

static inline uint64_t vktl_sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
// What RAM keeps free for the game's own growth, the rest of the process and the system: a 32nd, at least 512 MiB. Small on purpose: Android keeps MemAvailable low while
// it has cached apps and compressed swap to give (Mario Kart World runs at 0.7-1 GB of it on a 16 GB phone), so a larger reserve would put a game that fits over the limit
// On a device of 12 GB or less (MemTotal up to 12 GiB) an eighth: there the game itself needs nearly all the RAM the system leaves (Ocarina of Time on an
// Odin 2 Portal, 11 GiB: the images grew to 3.2 GiB without one prune while MemAvailable stayed above 512 MiB, and lmkd killed the game), and Android's
// compressed swap keeps MemAvailable up while the game already stalls
static inline uint64_t vktl_reserve(uint64_t mem_total) {
    if (mem_total && mem_total <= 12 * 1024 * VKTL_MIB) return mem_total / 8;
    uint64_t r = mem_total / 32; return r < 512 * VKTL_MIB ? 512 * VKTL_MIB : r;
}
// Memory pressure (Linux PSI): Android's lmkd kills the foreground game by how long tasks stall on memory, not by how much is free, so the cache follows it too.
// From VKTL_PSI_HOLD percent of the last 10 s stalled the cache stops growing (the limit is what it holds: a new image evicts an old one); from VKTL_PSI_SHRINK
// it gives an eighth back at each limit update, down to the floor. Images in use stay either way (vktl_evictable)
#define VKTL_PSI_HOLD 5.0
#define VKTL_PSI_SHRINK 15.0
static inline uint64_t vktl_pressure_limit(uint64_t lim, uint64_t img_bytes, double psi_some10) {
    if (psi_some10 >= VKTL_PSI_SHRINK) { uint64_t l = img_bytes - img_bytes / 8; if (l < lim) lim = l; }
    else if (psi_some10 >= VKTL_PSI_HOLD) { if (img_bytes < lim) lim = img_bytes; }
    return lim < VKTL_FLOOR ? VKTL_FLOOR : lim;
}
// The limit in bytes; floor when nothing is known
static inline uint64_t vktl_limit(const vktl_in *in) {
    uint64_t lim = UINT64_MAX;
    if (in->mem_total && in->mem_avail) { uint64_t l = in->img_bytes + vktl_sub(in->mem_avail, vktl_reserve(in->mem_total)); if (l < lim) lim = l; }
    if (in->heap_budget) {   // the budget is the process's whole share of the heap: what else uses it (guest RAM, buffers, other images) is not the cache's
        uint64_t other = vktl_sub(in->heap_usage, in->img_bytes);
        uint64_t l = vktl_sub(in->heap_budget - in->heap_budget / 16, other); if (l < lim) lim = l;
    } else if (in->heap_size) { uint64_t l = in->heap_size / 2; if (l < lim) lim = l; }   // (no budget: half the heap, which on a unified-memory phone is the RAM)
    if (lim == UINT64_MAX || lim < VKTL_FLOOR) lim = VKTL_FLOOR;
    if (in->psi_some10 > 0) lim = vktl_pressure_limit(lim, in->img_bytes, in->psi_some10);
    return lim;
}
// Prune down to this once over the limit (a margin, so one new image does not start another prune)
static inline uint64_t vktl_target(uint64_t limit) { return limit - limit / 16; }

// Whether an image may be evicted now: not held from outside the recording thread, not a playing video's, its last submission done, unused for VKTL_KEEP_FRAMES frames
// (VKTL_GPU_FRAMES when its content is the GPU's). last_frame 0 = never used (kept: it was just made)
// keep_frames: how many frames an image used recently is kept whatever the limit says (VKTL_KEEP_FRAMES at least; DM_VK_IMG_KEEP_FRAMES, live, sets it). With a limit
// under the working set every image that is in use is a candidate and the cache evicts what the next frames need, then uploads it again (the phone's log: 76 prunes,
// "evicted 524 images unused for 3+ frames", a frame with 488 uploads of 81 MB and 41,706 major page faults) -- a stall far worse than the memory it saved
static inline int vktl_evictable2(uint64_t cur_frame, uint64_t last_frame, int holds, int gpu_owned, int video_playing, int fence_pending, unsigned keep_frames) {
    if (holds || video_playing || fence_pending || !last_frame || cur_frame <= last_frame) return 0;
    if (gpu_owned) return cur_frame - last_frame >= (uint64_t)VKTL_GPU_FRAMES;   // (content only the GPU has: kept longer, and gone after every image that can be uploaded again)
    return cur_frame - last_frame >= (uint64_t)(keep_frames ? keep_frames : VKTL_KEEP_FRAMES);
}
static inline int vktl_evictable(uint64_t cur_frame, uint64_t last_frame, int holds, int gpu_owned, int video_playing, int fence_pending) {
    return vktl_evictable2(cur_frame, last_frame, holds, gpu_owned, video_playing, fence_pending, VKTL_KEEP_FRAMES);
}
// Eviction order: images that can be uploaded again before GPU-only ones, then least recently used first (qsort-style: < 0 = a goes first)
static inline int vktl_order(int gpu_a, uint64_t last_a, int gpu_b, uint64_t last_b) {
    if (gpu_a != gpu_b) return gpu_a - gpu_b;
    return last_a < last_b ? -1 : last_a > last_b;
}
#endif
