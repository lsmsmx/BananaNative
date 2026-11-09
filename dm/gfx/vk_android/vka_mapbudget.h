// The process's memory maps budget (vka_mem.c maps_tick), free of Vulkan so the Mac's unit tests (dm/host/tests/t_android_mapbudget.c, dm/gfx/tests/t_dirty_maps.c)
// can check it. Linux gives a process at most vm.max_map_count mappings (65,530 by default); past it any mmap, mprotect or munmap that needs one more fails with ENOMEM,
// in whatever thread asks (docs/reference/memory.md "Write tracking of small textures fills the process's memory maps"). The dirty-page tracker's write protections
// split placed pool memory, whose kernel driver mappings (kgsl) never merge again: a split stays until the mapping goes. So the count is read (the lines of
// /proc/self/maps) and the tracker made coarser as it climbs, so that new splits land on fewer boundaries; past the budget the placed segments with the most
// pieces get their CPU map made again in one piece (compaction), which is the only way a driver mapping's pieces become one again.
#ifndef VKA_MAPBUDGET_H
#define VKA_MAPBUDGET_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { VKMB_FIRST_PCT = 40,     // from this share of vm.max_map_count on, the tracker works in blocks of the base size (whatever DM_DIRTY_BLOCK_AT says) ...
       VKMB_STEP_PCT = 10,      // ... and in blocks 4 times larger at every further 10 % ...
       VKMB_STEPS = 3,          // ... up to 3 steps (64 KiB base: 256 KiB at 50 %, 1 MiB at 60 %, 4 MiB at 70 %)
       VKMB_BUDGET_PCT = 75,    // compaction from here (DM_VK_MAPS_BUDGET) ...
       VKMB_TARGET_PCT = 55,    // ... down to here
       VKMB_MIN_PIECES = 16 };  // a segment of fewer pieces is not worth its compaction (it stamps every page of it: its textures upload again)

// The tracker's block size at count maps of max (base: its first block size, 64 KiB on Android); 0 = no change asked for (under VKMB_FIRST_PCT, or nothing known)
static inline size_t vkmb_block(long count, long max, size_t base) {
    if (max <= 0 || count < 0 || !base || count * 100 < (long)VKMB_FIRST_PCT * max) return 0;
    size_t b = base;
    for (int s = 1; s <= VKMB_STEPS && count * 100 >= (long)(VKMB_FIRST_PCT + s * VKMB_STEP_PCT) * max; s++) b <<= 2;
    return b;
}
// Maps to shed by compaction: down to target_pct of max once at budget_pct or over; 0 under the budget
static inline long vkmb_shed(long count, long max, int budget_pct, int target_pct) {
    if (max <= 0 || count * 100 < (long)budget_pct * max) return 0;
    long t = max * target_pct / 100;
    return count > t ? count - t : 0;
}
// The segments to compact: the most pieces first (a segment of p pieces becomes one: p - 1 maps fewer), until shed is covered or maxpick are taken; segments under
// VKMB_MIN_PIECES are left. Their indexes go to idx; the count picked is returned and *covered says how many maps they shed
static inline int vkmb_pick(const int *pieces, int n, long shed, int *idx, int maxpick, long *covered) {
    int k = 0; long got = 0;
    while (k < maxpick && got < shed) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (pieces[i] < VKMB_MIN_PIECES) continue;
            int taken = 0; for (int j = 0; j < k; j++) if (idx[j] == i) { taken = 1; break; }
            if (!taken && (best < 0 || pieces[i] > pieces[best])) best = i;
        }
        if (best < 0) break;
        idx[k++] = best; got += pieces[best] - 1;
    }
    if (covered) *covered = got;
    return k;
}

// One line of /proc/self/maps ("lo-hi perms offset dev inode   name", without its newline): its start address, and whether it is the GPU kernel driver's (kgsl). 0 = not a line
static inline int vkmb_line(const char *ln, size_t len, uint64_t *start, int *kgsl) {
    uint64_t a = 0; size_t i = 0;
    for (; i < len; i++) {
        char c = ln[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        a = a << 4 | (uint64_t)d;
    }
    if (!i || i >= len || ln[i] != '-') return 0;
    *start = a;
    *kgsl = 0;
    for (size_t j = i; j + 4 <= len; j++) if (!memcmp(ln + j, "kgsl", 4)) { *kgsl = 1; break; }
    return 1;
}
// The segment (sorted, disjoint [lo, hi)) holding a, or -1
static inline int vkmb_seg(const uint64_t *lo, const uint64_t *hi, int n, uint64_t a) {
    int l = 0, h = n - 1, hit = -1;
    while (l <= h) { int m = (l + h) / 2; if (lo[m] <= a) { hit = m; l = m + 1; } else h = m - 1; }
    return hit >= 0 && a < hi[hit] ? hit : -1;
}

// ---------- What the maps outside the guest block are (diagnostic, DM_VK_MAPS_HIST) ----------
// The budget above only sheds placed memory's pieces; when the maps outside the guest block are what climbs (a 0400486024DEC000 run: 6,926 -> 49,277 in 20 s, compaction
// covering 0 of them), the count alone does not say whose they are. So the scan can sort those lines into groups of (name, permissions, size bucket) and the maps thread
// logs the largest. Digits in a name are folded to '#' so per-thread or per-file names ([anon:stack_and_tls:12345], a temp file with a counter) make one group.
// No allocation: near the limit malloc may need a new mapping itself, so the table is fixed and what does not fit is counted as dropped.
enum { VKMB_NAME = 64, VKMB_HCAP = 1024, VKMB_NBUCKET = 6 };
static const char *const vkmb_bucket_name[VKMB_NBUCKET] = {"<=4K", "<=64K", "<=1M", "<=16M", "<=256M", ">256M"};
static inline int vkmb_bucket(uint64_t n) {
    return n <= 4096 ? 0 : n <= 65536 ? 1 : n <= (1u << 20) ? 2 : n <= (16u << 20) ? 3 : n <= (256u << 20) ? 4 : 5;
}
// A line's fields: start, end, permissions ("rw-p"), and its name (what follows the inode; empty for plain anonymous memory). 0 = not a line
static inline int vkmb_fields(const char *ln, size_t len, uint64_t *lo, uint64_t *hi, char perms[5], const char **name, size_t *nlen) {
    uint64_t v[2] = {0, 0}; size_t i = 0;
    for (int f = 0; f < 2; f++) {
        size_t s = i;
        for (; i < len; i++) {
            char c = ln[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v[f] = v[f] << 4 | (uint64_t)d;
        }
        if (i == s || i >= len || ln[i] != (f ? ' ' : '-')) return 0;
        i++;
    }
    if (i + 4 > len) return 0;
    memcpy(perms, ln + i, 4); perms[4] = 0; i += 4;
    for (int f = 0; f < 3; f++) {   // offset, dev, inode
        while (i < len && ln[i] == ' ') i++;
        while (i < len && ln[i] != ' ') i++;
    }
    while (i < len && ln[i] == ' ') i++;
    size_t e = len; while (e > i && (ln[e - 1] == ' ' || ln[e - 1] == '\r')) e--;
    *lo = v[0]; *hi = v[1]; *name = ln + i; *nlen = e - i;
    return 1;
}
typedef struct { char name[VKMB_NAME]; char perms[5]; uint8_t bucket, used; long n; uint64_t bytes; } vkmb_hent;
typedef struct { vkmb_hent e[VKMB_HCAP]; long lines, dropped; uint64_t bytes; } vkmb_hist;
static inline void vkmb_hist_reset(vkmb_hist *h) { memset(h, 0, sizeof *h); }
// A name with its digit runs folded to one '#', cut to VKMB_NAME - 1 bytes; "(anonymous)" for an empty one
static inline void vkmb_norm(const char *s, size_t n, char out[VKMB_NAME]) {
    if (!n) { strcpy(out, "(anonymous)"); return; }
    size_t o = 0;
    for (size_t i = 0; i < n && o < VKMB_NAME - 1; i++) {
        if (s[i] >= '0' && s[i] <= '9') { if (!o || out[o - 1] != '#') out[o++] = '#'; }
        else out[o++] = s[i];
    }
    out[o] = 0;
}
// One line into the table (open addressing on the key's hash)
static inline void vkmb_hist_add(vkmb_hist *h, const char *ln, size_t len) {
    uint64_t lo, hi; char perms[5]; const char *nm; size_t nl;
    if (!vkmb_fields(ln, len, &lo, &hi, perms, &nm, &nl) || hi < lo) return;
    char key[VKMB_NAME]; vkmb_norm(nm, nl, key);
    uint64_t sz = hi - lo; int b = vkmb_bucket(sz);
    uint32_t x = 2166136261u;
    for (const char *p = key; *p; p++) x = (x ^ (uint8_t)*p) * 16777619u;
    for (int i = 0; i < 4; i++) x = (x ^ (uint8_t)perms[i]) * 16777619u;
    x = (x ^ (uint32_t)b) * 16777619u;
    h->lines++; h->bytes += sz;
    for (int k = 0; k < VKMB_HCAP; k++) {
        vkmb_hent *e = &h->e[(x + (uint32_t)k) % VKMB_HCAP];
        if (!e->used) { e->used = 1; strcpy(e->name, key); memcpy(e->perms, perms, 5); e->bucket = (uint8_t)b; }
        else if (e->bucket != b || memcmp(e->perms, perms, 4) || strcmp(e->name, key)) continue;
        e->n++; e->bytes += sz; return;
    }
    h->dropped++;
}
// The indexes of the (at most k) largest groups by maps, largest first; how many there are
static inline int vkmb_hist_top(const vkmb_hist *h, int *idx, int k) {
    int n = 0;
    if (k <= 0) return 0;
    for (int i = 0; i < VKMB_HCAP; i++) {
        if (!h->e[i].used) continue;
        int j;
        if (n < k) idx[j = n++] = i;
        else if (h->e[i].n > h->e[idx[k - 1]].n) idx[j = k - 1] = i;
        else continue;
        while (j > 0 && h->e[idx[j - 1]].n < h->e[idx[j]].n) { int t = idx[j - 1]; idx[j - 1] = idx[j]; idx[j] = t; j--; }
    }
    return n;
}
#endif
