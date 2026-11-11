// Placing large memory pools by parts (vka_mem.c mtl_pool_need; docs/reference/memory.md "Placed pools commit what the game only reserved"): the range arithmetic, free of
// Vulkan so the Mac's unit test (dm/host/tests/t_android_place.c) can check it. A pool of at least the part minimum is placed in parts: when the GPU is given [a, e) in it,
// the granules of the pool that [a, e) touches become placed memory (granules counted from the pool's start), not the whole pool. An ask that covers the whole pool
// (nvnMemoryPoolGetBufferAddress) places all of it. The caller aligns the part outward to the placed-map alignment, as it does a whole pool.
// An ask across a part placed earlier and one placed now lies in two segments; vkp_join finds the run of segments to join into one (vka_mem.c "Joining parts")
#ifndef VKA_PLACE_H
#define VKA_PLACE_H
#include <stddef.h>
#include <stdint.h>

// 1 = the pool [mem, mem + size) is placed by parts (part_min 0: never; a pool of less than two granules is placed whole, a part would be most of it)
static inline int vkp_by_parts(uint64_t size, uint64_t part_min, uint64_t granule) {
    return part_min && granule && size >= part_min && size >= 2 * granule;
}
// The part of the pool to place for the ask [a, e): the granules it touches, clipped to the pool, into [*lo, *hi). Returns 0 when the ask covers the whole pool or lies
// outside it (the caller places the whole pool, as before), 1 for a part
static inline int vkp_part(uint64_t mem, uint64_t size, uint64_t a, uint64_t e, uint64_t granule, uint64_t *lo, uint64_t *hi) {
    uint64_t end = mem + size;
    if (!granule || e <= a || a >= end || e <= mem || (a <= mem && e >= end)) return 0;
    uint64_t s = a > mem ? a - mem : 0, t = (e < end ? e : end) - mem;   // [s, t): the ask in the pool, as offsets
    s -= s % granule;
    t = t % granule ? t + (granule - t % granule) : t;
    if (t > size || t < s) t = size;   // (the last granule may be short; t < s: overflow of the round up)
    if (s == 0 && t == size) return 0;
    *lo = mem + s; *hi = mem + t;
    return 1;
}
// Joining parts (vka_mem.c "Joining parts"): the run of placed segments that holds [a, e). segs: n segments sorted by start and disjoint, each starting with its bounds
// [lo, hi) as two uint64_t, stride bytes apart (vka_mem.c's seg_t). Returns the number of segments from *k0 to *k1 when they hold all of [a, e) end to end (1: one
// segment holds it, nothing to join), 0 when some byte of it lies in no segment or between two that do not touch
static inline int vkp_join(const void *segs, size_t stride, int n, uint64_t a, uint64_t e, int *k0, int *k1) {
#define VKP_LO(i) (((const uint64_t *)((const char *)segs + (size_t)(i) * stride))[0])
#define VKP_HI(i) (((const uint64_t *)((const char *)segs + (size_t)(i) * stride))[1])
    if (e <= a || n <= 0) return 0;
    int lo = 0, hi = n - 1, k = -1;   // the last segment starting at or before a
    while (lo <= hi) { int m = lo + (hi - lo) / 2; if (VKP_LO(m) <= a) { k = m; lo = m + 1; } else hi = m - 1; }
    if (k < 0 || a >= VKP_HI(k)) return 0;
    int j = k;
    while (VKP_HI(j) < e) { if (j + 1 >= n || VKP_LO(j + 1) != VKP_HI(j)) return 0; j++; }
    *k0 = k; *k1 = j;
    return j - k + 1;
#undef VKP_LO
#undef VKP_HI
}
#endif
