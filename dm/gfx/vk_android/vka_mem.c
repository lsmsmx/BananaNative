// NVN memory pools as placed Vulkan memory (android/PORTING.md "NVN → Vulkan replay", Guest memory).
// Guest RAM stays anonymous memory, committed when touched. Only what the GPU can reach becomes Vulkan memory: when the GPU is first given an address in a memory pool (mtl_pool_need; see Pools
// below), its pages are swapped for one VkDeviceMemory of the guest type (DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED on the Adreno 840) mapped with VK_EXT_map_memory_placed at the pool's
// own address, with the bytes the pages held; finalizing the pool (mtl_pool_del) swaps anonymous memory back in, with the bytes and the protection. Driver memory is committed
// when it is allocated, so placing the whole 9 GiB heap of Donkey Kong Bananza (the earlier design: 1 GiB chunks of every opened region) made the low-memory killer end the game.
// Each placed range is a segment: one VkDeviceMemory, one VkBuffer over it, its device address. A pool is normally one segment; parts of a pool another pool's segment already covers
// are shared (counted), and a pool the driver cannot allocate in one piece is split at maxMemoryAllocationSize. Lookups (vka_addr, vka_addr_run) answer for one segment.
// The swap. A pool's pages may hold data (the game fills storage before it makes the pool) and other threads may touch them meanwhile, so neither copy may lose a write or show
// a reader zeros: placing, the range is made read-only, the new memory is mapped where the driver likes and filled from the range, unmapped, then mapped placed over the range in one
// mmap (readers see the old pages or the new ones, both with the bytes); giving back, the bytes go to fresh anonymous pages elsewhere (zero pages skipped, so they stay uncommitted)
// and those are moved over the range with mremap. A thread that writes during a swap faults, waits in the fault hook (or, for the host's own writes, in the pre-write hook) and retries
// Locks. g_pl (a mutex) is held by whoever changes the segment table: placing (mtl_pool_need, or mtl_pool_add under DM_VK_PLACE_LAZY=0), dropping references (mtl_pool_del, vka_mem_release); one placement at a time.
// g_rw (a reader-writer lock) guards the table against the lookups (vka_addr, vka_addr_run, vka_mem_chunks: the replay thread and the game's NVN calls); its write side is
// taken only for the table's own edits (an insert, a reference count, a removal), never across the work of a placement. A placement's Vulkan allocation, copy, placed map
// and prefault take about 0.7 ms a MiB on the Adreno 840, and they ran under the write lock: every lookup that missed its cache meanwhile waited (DKB's level load places
// pools of 100-500 MiB; the replay thread stood 300-500 ms in one submission, the game waited that long on its GPU fences with 3-4 ms of GPU work, and its render thread
// that long in its own NVN calls). A lookup misses no segment by this: a pool's range is not in the table until its placement ends, and no command can use the pool before
// mtl_pool_need returns. DM_VK_MEM_LOCK_WHOLE=1 holds the write lock across a placement as before (diagnostic A/B)
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_dirty.h"
#include "vka_place.h"
#include "vka_gmem.h"
#include "../../host/dm.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include "vka_mapbudget.h"
#include "vka_fd_alias.h"

typedef struct { uintptr_t lo, hi; VkDeviceMemory mem; VkBuffer buf; VkDeviceAddress bda; int refs, fd_alias; } seg_t;
static seg_t *g_sg; static int g_nsg, g_sgcap;   // sorted by lo, disjoint
// Bumped (under g_rw for writing) whenever a segment comes or goes: the replay's per-thread lookup cache (vka_addr, vka_addr_run) holds while it stands
static _Atomic uint64_t g_sg_gen = 1;
uint64_t vka_mem_gen(void) { return atomic_load_explicit(&g_sg_gen, memory_order_acquire); }   // (vka_draw.c gmem_map: the table is kept while it stands)
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t g_pl = PTHREAD_MUTEX_INITIALIZER;   // table changes and placements, one at a time (see Locks above)
static int lock_whole(void) { static int v = -1; if (v < 0) { v = VKA_ENV("DM_VK_MEM_LOCK_WHOLE", 0) != 0; if (v) VLOG("DM_VK_MEM_LOCK_WHOLE=1: placements hold the segment table's write lock (lookups wait for them)"); } return v; }
// Attribution ([longframe], vka_perf.c): time in placements and give-backs (any thread), and lookups that waited for the table's lock (the replay thread's and the others')
static _Atomic uint64_t g_place_ns, g_place_bytes, g_place_n, g_give_ns, g_lkw_rec_ns, g_lkw_oth_ns;
void vka_mem_waits(uint64_t *place_ns, uint64_t *place_bytes, uint64_t *place_n, uint64_t *give_ns, uint64_t *wait_rec_ns, uint64_t *wait_other_ns) {
    *place_ns = atomic_load(&g_place_ns); *place_bytes = atomic_load(&g_place_bytes); *place_n = atomic_load(&g_place_n); *give_ns = atomic_load(&g_give_ns);
    *wait_rec_ns = atomic_load(&g_lkw_rec_ns); *wait_other_ns = atomic_load(&g_lkw_oth_ns);
}
static uint64_t g_placed_bytes, g_placed_peak, g_placed_logged;
static _Atomic int g_maps_n = -1, g_maps_max;   // the process's memory maps at the last count (maps_tick, below) and vm.max_map_count

static int placed_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PLACED"); v = !(e && !strcmp(e, "0")); if (!v) VLOG("DM_VK_PLACED=0: memory pools stay anonymous memory (the GPU sees none)"); } return v; }
static uintptr_t pg_align(void) { return V.placed_align > 4096 ? (uintptr_t)V.placed_align : 4096; }

static int sg_find(uintptr_t a) {   // the last segment with lo <= a, or -1; caller holds g_rw
    int lo = 0, hi = g_nsg - 1, hit = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_sg[m].lo <= a) { hit = m; lo = m + 1; } else hi = m - 1; }
    return hit;
}
static void sg_insert(const seg_t *s) {   // caller holds g_rw for writing
    atomic_fetch_add(&g_sg_gen, 1);
    if (g_nsg == g_sgcap) { g_sgcap = g_sgcap ? g_sgcap * 2 : 64; g_sg = realloc(g_sg, (size_t)g_sgcap * sizeof *g_sg); }
    int i = sg_find(s->lo) + 1; memmove(&g_sg[i + 1], &g_sg[i], (size_t)(g_nsg - i) * sizeof *g_sg); g_sg[i] = *s; g_nsg++;
}

// ---------- Swaps and the threads that meet them ----------
// The range being swapped (lo < hi while one runs) and the last few finished ones with their end times. Read in the fault handler: atomics, clock_gettime and sched_yield only.
// A fault taken during a swap can reach the hook just after the swap ended; a fault in a range that finished less than RECENT_NS ago is retried. The cost of a wrong claim
// (a page another hook owns, a real bad access) is at most RECENT_NS of retrying before that hook or the crash report sees it
enum { NRECENT = 8 };
static const uint64_t RECENT_NS = 50000000ull;
static _Atomic uintptr_t g_sw_lo, g_sw_hi;
static struct { _Atomic uintptr_t lo, hi; _Atomic uint64_t end; } g_recent[NRECENT];
static _Atomic unsigned g_nrecent;
static pthread_mutex_t g_swmu = PTHREAD_MUTEX_INITIALIZER;   // one swap at a time
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int in_swap(uintptr_t a, uintptr_t b) { uintptr_t lo = atomic_load(&g_sw_lo), hi = atomic_load(&g_sw_hi); return lo < hi && a < hi && b > lo; }
static _Atomic long g_sw_owner;   // the thread that runs the swap (gettid), 0 when none
// A fault the swap's own thread takes in its range is not a writer to hold back: nothing would end the swap, and the thread (with g_pl, the dirty tracker's hold and the swap's
// lock) would spin here for ever while every other thread that needs them stops too. It goes to the next hook and, unhandled, is a crash with a backtrace
static int swap_fault(void *addr, int is_write, void *uc) {
    (void)is_write; (void)uc; uintptr_t a = (uintptr_t)addr;
    if (in_swap(a, a + 1)) { if (atomic_load(&g_sw_owner) == (long)gettid()) return 0; while (in_swap(a, a + 1)) sched_yield(); return 1; }
    uint64_t t = mono_ns();
    for (int i = 0; i < NRECENT; i++) { uint64_t e = atomic_load(&g_recent[i].end); if (e && t - e < RECENT_NS && a >= atomic_load(&g_recent[i].lo) && a < atomic_load(&g_recent[i].hi)) return 1; }
    return 0;
}
static void swap_prep(void *addr, size_t len) { uintptr_t a = (uintptr_t)addr; if (atomic_load(&g_sw_owner) == (long)gettid()) return; while (len && in_swap(a, a + len)) sched_yield(); }
static uint64_t g_sw_reads;   // g_snap_reads at the swap's start (under g_swmu)
static void swap_nomark_begin(void);
// The swap's hooks go in before nvn_dirty's (hooks run in the order they were registered): mtl_pool_add registers them, and hle_nvn.c tracks a pool only after mtl_pool_add.
// A pool is no longer placed at mtl_pool_add (DM_VK_PLACE_LAZY), so the first swap can come long after the tracker's hooks
static void swap_hooks_once(void) { dm_fault_hook_register(swap_fault); dm_guest_write_prep_register(swap_prep); }
static void swap_hooks(void) { static pthread_once_t o = PTHREAD_ONCE_INIT; pthread_once(&o, swap_hooks_once); }   // (every caller returns after they are in)
static void swap_begin(uintptr_t lo, uintptr_t hi) {
    swap_hooks();
    pthread_mutex_lock(&g_swmu); atomic_store(&g_sw_hi, 0); atomic_store(&g_sw_lo, lo); atomic_store(&g_sw_owner, (long)gettid()); atomic_store(&g_sw_hi, hi);
    swap_nomark_begin();
}
static void swap_nomark_end(uintptr_t lo, uintptr_t hi, int exact);
static void swap_end_ok(int exact) { swap_nomark_end(atomic_load(&g_sw_lo), atomic_load(&g_sw_hi), exact); }
static void swap_end(void) {
    unsigned k = atomic_fetch_add(&g_nrecent, 1) % NRECENT;
    atomic_store(&g_recent[k].end, 0); atomic_store(&g_recent[k].lo, atomic_load(&g_sw_lo)); atomic_store(&g_recent[k].hi, atomic_load(&g_sw_hi)); atomic_store(&g_recent[k].end, mono_ns());
    atomic_store(&g_sw_hi, 0); atomic_store(&g_sw_lo, 0); atomic_store(&g_sw_owner, 0);
    pthread_mutex_unlock(&g_swmu);
}

// The protection of [lo, hi) page by page as /proc/self/maps has it: runs of one protection, -1 for unmapped. 0 = could not read it.
// The text is generated from the first mapping on, and the process has thousands (the dirty tracker's write protections split the pools' mappings): one read took
// 1-5 ms, which was 75 % of all placement time on the phone (2026-11-03, `placed memory: 2048 placements: … protections read 7525 ms (5691 lines … a read)`), and the
// game's loading threads place hundreds of small pools in one go (231 pools of 7.9 MB took 1,281 ms on DKB's StageScene threads while its main thread waited: a 1.7 s
// frame). The kernel's PROCMAP_QUERY ioctl would answer per address, but SELinux refuses it to apps (EACCES).
// So the text is read once into a snapshot of the guest address space's runs, and a read answers from it while no protection in its range changed since. Changes are
// seen where they are made: this library's mmap, munmap, mprotect and mremap (defined below over the system calls, so every caller in the host binds to them) mark the
// 256 KiB blocks (PBLK_SHIFT) they touch with a new generation after the call. A range any of whose blocks changed after the snapshot began is read again (the whole text,
// once). Nothing else maps into the guest block (it is one reservation the kernel never places other mappings in). A swap (place / unplace) is the one exception: its calls
// and the Vulkan driver's placed map (made and taken down by the driver's own mmap, which this library does not see) leave the range with the protections it had, so the swap
// makes them through sw_* (the system calls, unmarked) and marks its range at its end only when the end state may differ from what a snapshot holds (swap_end_ok).
// Hits (answers from a snapshot that was not read again for them) are checked against the text: the first 16, then every 64th; a difference turns the snapshot off for
// the run ([RED], with the range and the first run that differs). DM_VK_MAPS_CACHE=0: the text each time
typedef struct { uintptr_t lo, hi; int prot; } prun;
enum { MAXRUN = 64, MAXRUN_ = MAXRUN, PBLK_SHIFT = 18 };
static _Atomic uint64_t g_pgen = 1;
static _Atomic(_Atomic uint64_t *) g_pblk; static uintptr_t g_pg_lo, g_pg_hi;   // the guest block's generation per 256 KiB block (allocated at the first read)
// Snapshot reads begun and ended (+1 each: odd while one runs)
static _Atomic uint64_t g_snap_reads;
static void prot_mark(uintptr_t a, size_t n) {   // async-signal-safe (the dirty tracker unprotects in its fault handler): atomics only
    uint64_t g = atomic_fetch_add_explicit(&g_pgen, 1, memory_order_acq_rel) + 1;
    _Atomic uint64_t *b = atomic_load_explicit(&g_pblk, memory_order_acquire);
    if (!b || !n) return;
    uintptr_t lo = a > g_pg_lo ? a : g_pg_lo, hi = a + n < g_pg_hi ? a + n : g_pg_hi;
    for (uintptr_t x = lo; x < hi; x = ((x >> PBLK_SHIFT) + 1) << PBLK_SHIFT) {   // the block's generation only rises: a thread that took an older one and stores late must not hide a newer change
        _Atomic uint64_t *e = &b[(x - g_pg_lo) >> PBLK_SHIFT]; uint64_t o = atomic_load_explicit(e, memory_order_relaxed);
        while (o < g && !atomic_compare_exchange_weak_explicit(e, &o, g, memory_order_release, memory_order_relaxed)) {}
    }
}
// The swap's own memory calls (under g_swmu): the system calls without the mark. A failed one (a protection not put back, a range not mapped) sets g_sw_fail, and the swap
// marks its range at its end
static int g_sw_fail;
static long sw_mprotect(uintptr_t a, size_t n, int prot) { long r = syscall(__NR_mprotect, a, n, prot); if (r) g_sw_fail = 1; return r; }
static void *sw_mmap_fixed(uintptr_t a, size_t n) { long r = syscall(__NR_mmap, a, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); if (r == -1 || r != (long)a) { g_sw_fail = 1; return MAP_FAILED; } plat_mem_name_guest((void *)r, n); return (void *)r; }   // (named as the guest block: it merges with its neighbours)
static void *sw_mremap_to(void *from, size_t n, uintptr_t to) { long r = syscall(__NR_mremap, from, n, n, MREMAP_MAYMOVE | MREMAP_FIXED, to); if (r == -1) { g_sw_fail = 1; return MAP_FAILED; } return (void *)r; }
static int prot_runs_text(uintptr_t lo, uintptr_t hi, prun *out, int max);
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
// Before a swap maps its range again where the driver may have taken it down (unplace; place after a failed placed map): the range is reserved with MAP_FIXED_NOREPLACE,
// which the caller's fixed map then replaces. A free range takes the reservation. An occupied one (EEXIST):
// - freed = the Vulkan memory was freed (unplace): only the driver's reservation may be there, all of the range and PROT_NONE (a driver that keeps the range after an unmap
//   with VK_MEMORY_UNMAP_RESERVE_BIT_EXT). Anything else (a mapping with access, a part unmapped: another thread's mmap(NULL), the driver's or libc's, which this library
//   does not see, given all or part of the hole) is [RED]. The text read costs 1-5 ms, under g_swmu, and a driver that keeps its reservations (Turnip) answers EEXIST
//   on every give-back (some 200 pools at a level's end: up to a second). So the driver is learned (g_resv): when the first RESV_LEARN freed give-backs all find the
//   whole range PROT_NONE, the driver keeps its reservations, no hole can open, and the rest of the run takes the msync shortcut below; a probe that finds the range
//   free (the driver took it down: the Adreno driver), or a [RED] after a free while learning, keeps the text check for the run. Decided once, logged with the reservation's line of the text. The check does not
//   look at the line's name (whether Turnip's reservation is unnamed is not known here; the logged line shows it), so another PROT_NONE mapping filling the whole hole
//   (a scudo or ART reservation) would pass
// - not freed (place's failure path, the remap experiment): the range may still hold the old pages or the driver's map; all of it mapped (msync answers ENOMEM for any
//   unmapped page) is taken as that, a part unmapped is [RED].
// The fixed map then replaces what is there (the guest's range), but not silently. (The window itself stays open, see unplace)
static int maps_line(FILE *f, char *ln, int n, int *err);
static void maps_line_of(uintptr_t a, char *out, int n) {   // the line of the text that holds a, or ""
    out[0] = 0; FILE *f = fopen("/proc/self/maps", "re"); if (!f) return;
    char ln[512]; int err = 0;
    while (maps_line(f, ln, sizeof ln, &err)) { unsigned long x, y; if (sscanf(ln, "%lx-%lx", &x, &y) == 2 && a >= x && a < y) { snprintf(out, (size_t)n, "%s", ln); break; } }
    fclose(f); char *nl = strchr(out, '\n'); if (nl) *nl = 0;
}
enum { RESV_LEARN = 4 };
static int g_resv = -1, g_resv_n;   // the driver after a free: -1 not known yet, 0 takes the range down, 1 keeps its reservation (under g_swmu)
static void sw_hole_check(uintptr_t lo, size_t n, int freed, const char *what) {
    const int freed_in = freed;   // the caller's: the Vulkan memory was freed (freed is cleared below once the driver is known to keep its reservations)
    long r = syscall(__NR_mmap, lo, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (r == (long)lo) {
        if (freed && g_resv < 0) { g_resv = 0; VLOG("placed memory: the driver's vkFreeMemory takes a pool's range down: each give-back checks the range against the text when it finds it occupied"); }
        return;
    }
    if (r != -1) { syscall(__NR_munmap, r, n); return; }   // a kernel before 4.17 took the flag as a hint
    if (freed && g_resv == 1) freed = 0;   // the driver keeps its reservations: no hole after a free
    if (errno != EEXIST || (!freed && !msync((void *)lo, n, MS_ASYNC))) return;
    prun pr[MAXRUN]; int k = prot_runs_text(lo, lo + n, pr, MAXRUN), i = 0;
    while (i < k && pr[i].prot == (freed ? PROT_NONE : pr[i].prot) && pr[i].prot >= 0) i++;   // the first piece that is not the expected mapping
    if (k && i == k) {
        if (freed && g_resv < 0 && k == 1 && ++g_resv_n >= RESV_LEARN) {
            char ln[512]; maps_line_of(lo, ln, sizeof ln); g_resv = 1;
            VLOG("placed memory: the driver keeps a pool's range reserved after vkFreeMemory (%d give-backs; its line: %s): no hole can open, the range is no longer checked against the text", RESV_LEARN, ln);
        }
        return;
    }
    g_sw_fail = 1;
    // An intruder after a free, seen while still learning the driver: the run keeps the text check (a later run of whole-range reservations must not turn it off).
    // Only a give-back after vkFreeMemory whose text read gave the range's pieces (k > 0) counts: the not-freed paths (place's failure path, a give-back whose wait
    // failed, DM_VK_UNPLACE_REMAP) and a failed text read say nothing about what the driver leaves after a free
    if (freed_in && k > 0 && g_resv < 0) {
        g_resv = 0; VLOG("placed memory: another mapping was found in a pool's range after vkFreeMemory while learning the driver: each give-back keeps checking the range against the text");
    }
    static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 8)
        VRED("%s %#lx+%#zx: the range is not as the driver left it (another mapping took all or part of it while it was not mapped; first odd piece %#lx-%#lx prot %d, %d pieces): the guest's pages replace it",
             what, (unsigned long)lo, n, i < k ? (unsigned long)pr[i].lo : 0ul, i < k ? (unsigned long)pr[i].hi : 0ul, i < k ? pr[i].prot : -2, k);
}
static void swap_nomark_begin(void) { g_sw_reads = atomic_load(&g_snap_reads); g_sw_fail = 0; }
// exact = the swap knows the range's protections and put them all back. Marked otherwise, when a call failed, or when a snapshot read ran during any part of the swap
// (it may hold the swap's passing read-only state): one began or ended meanwhile (the count moved), or one runs now (odd: it began before the swap and is still reading)
static void swap_nomark_end(uintptr_t lo, uintptr_t hi, int exact) {
    uint64_t r = atomic_load(&g_snap_reads);
    if (!exact || g_sw_fail || r != g_sw_reads || (r & 1)) prot_mark(lo, hi - lo);
}
// The memory calls of this library: the system call, then the mark (after it, so a snapshot read meanwhile is older than the mark)
// (A failed MAP_FIXED mmap or MREMAP_FIXED mremap may have unmapped its target already: marked too)
void *mmap(void *a, size_t n, int prot, int fl, int fd, off_t off) {
    long r = syscall(__NR_mmap, a, n, prot, fl, fd, off);
    if (r == -1) { if (fl & MAP_FIXED) prot_mark((uintptr_t)a, n); return MAP_FAILED; }
    prot_mark((uintptr_t)r, n); return (void *)r;
}
void *mmap64(void *a, size_t n, int prot, int fl, int fd, off64_t off) { return mmap(a, n, prot, fl, fd, (off_t)off); }
int munmap(void *a, size_t n) { long r = syscall(__NR_munmap, a, n); prot_mark((uintptr_t)a, n); return (int)r; }
int mprotect(void *a, size_t n, int prot) { long r = syscall(__NR_mprotect, a, n, prot); prot_mark((uintptr_t)a, n); return (int)r; }
void *mremap(void *a, size_t n, size_t nn, int fl, ...) {
    void *to = NULL; if (fl & MREMAP_FIXED) { va_list ap; va_start(ap, fl); to = va_arg(ap, void *); va_end(ap); }
    long r = syscall(__NR_mremap, a, n, nn, fl, to); prot_mark((uintptr_t)a, n);
    if (r == -1) { if (to) prot_mark((uintptr_t)to, nn); return MAP_FAILED; }
    prot_mark((uintptr_t)r, nn); return (void *)r;
}
static int prot_runs_text(uintptr_t lo, uintptr_t hi, prun *out, int max);
static uint64_t g_maps_lines;   // lines of /proc/self/maps read (placement phases, below)
static struct { prun *r; int n, cap; uint64_t gen; int ok, off; } g_snap;   // under g_snapmu
static pthread_mutex_t g_snapmu = PTHREAD_MUTEX_INITIALIZER;
// One line of /proc/self/maps into ln (a line longer than ln: its start, the rest skipped); 0 at the end or on an error (*err)
static int maps_line(FILE *f, char *ln, int n, int *err) {
    if (!fgets(ln, n, f)) { *err = ferror(f) != 0; return 0; }
    if (!strchr(ln, '\n')) { int c; while ((c = getc(f)) != EOF && c != '\n') {} if (ferror(f)) { *err = 1; return 0; } }
    return 1;
}
static int snap_read(void) {   // the guest block's runs from the text; caller holds g_snapmu. 0 = no snapshot (none is used until a read succeeds)
    g_snap.ok = 0;
    if (!atomic_load(&g_pblk)) {
        uint8_t *gb; size_t gs; void svc_guest_range(uint8_t **base, size_t *size); svc_guest_range(&gb, &gs);
        if (!gb || !gs) return 0;
        g_pg_lo = (uintptr_t)gb & ~(((uintptr_t)1 << PBLK_SHIFT) - 1); g_pg_hi = (uintptr_t)gb + gs;
        _Atomic uint64_t *b = calloc(((g_pg_hi - g_pg_lo) >> PBLK_SHIFT) + 1, sizeof *b); if (!b) return 0;
        atomic_store_explicit(&g_pblk, b, memory_order_release);
    }
    atomic_fetch_add(&g_snap_reads, 1);
    uint64_t g0 = atomic_load_explicit(&g_pgen, memory_order_acquire);
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) { atomic_fetch_add(&g_snap_reads, 1); return 0; }
    char ln[512]; int err = 0, ok = 1; g_snap.n = 0;
    while (maps_line(f, ln, sizeof ln, &err)) {
        unsigned long a, b; char pm[8];
        if (sscanf(ln, "%lx-%lx %7s", &a, &b, pm) != 3 || b <= g_pg_lo) continue;
        if (a >= g_pg_hi) break;
        int p = (pm[0] == 'r' ? PROT_READ : 0) | (pm[1] == 'w' ? PROT_WRITE : 0) | (pm[2] == 'x' ? PROT_EXEC : 0);
        if (g_snap.n && g_snap.r[g_snap.n - 1].prot == p && g_snap.r[g_snap.n - 1].hi == a) { g_snap.r[g_snap.n - 1].hi = b; continue; }
        if (g_snap.n == g_snap.cap) { int nc = g_snap.cap ? 2 * g_snap.cap : 4096; prun *nr = realloc(g_snap.r, (size_t)nc * sizeof *nr); if (!nr) { ok = 0; break; } g_snap.r = nr; g_snap.cap = nc; }
        g_snap.r[g_snap.n++] = (prun){a, b, p};
    }
    fclose(f);
    atomic_fetch_add(&g_snap_reads, 1);
    if (!ok || err) { g_snap.n = 0; return 0; }   // a truncated snapshot would answer "unmapped" for the rest (and the copy would zero those bytes)
    // A mark of generation g <= g0 took its number before g0 was loaded, after its call (prot_mark runs after the system call; the fetch_add and this acquire load are
    // ordered on g_pgen), so the text read after the load already shows that change: only marks above g0 make the snapshot stale. (With g0 itself the bound, the mark
    // that took g0 made every snapshot read right after it stale: a second read for nothing.)
    g_snap.gen = g0 + 1; g_snap.ok = 1;
    return 1;
}
static int snap_valid(uintptr_t lo, uintptr_t hi) {   // no block of [lo, hi) changed since the snapshot began (marked g_snap.gen or later)
    _Atomic uint64_t *b = atomic_load_explicit(&g_pblk, memory_order_acquire);
    if (!g_snap.ok || !b || lo < g_pg_lo || hi > g_pg_hi) return 0;
    for (uintptr_t x = lo; x < hi; x = ((x >> PBLK_SHIFT) + 1) << PBLK_SHIFT) if (atomic_load_explicit(&b[(x - g_pg_lo) >> PBLK_SHIFT], memory_order_acquire) >= g_snap.gen) return 0;
    return 1;
}
static int snap_runs(uintptr_t lo, uintptr_t hi, prun *out, int max) {   // [lo, hi) from the snapshot; 0 = more than max runs
    int a = 0, z = g_snap.n;   // the first run ending after lo
    while (a < z) { int m = (a + z) / 2; if (g_snap.r[m].hi <= lo) a = m + 1; else z = m; }
    int n = 0; uintptr_t at = lo;
    for (int i = a; i < g_snap.n && at < hi; i++) {
        const prun *r = &g_snap.r[i]; if (r->lo >= hi) break;
        if (r->lo > at) { if (n == max) return 0; out[n++] = (prun){at, r->lo, -1}; at = r->lo; }
        uintptr_t e = r->hi < hi ? r->hi : hi;
        if (n && out[n - 1].prot == r->prot && out[n - 1].hi == at) out[n - 1].hi = e; else { if (n == max) return 0; out[n++] = (prun){at, e, r->prot}; }
        at = e;
    }
    if (at < hi) { if (n == max) return 0; out[n++] = (prun){at, hi, -1}; }
    return n;
}
static int prot_runs(uintptr_t lo, uintptr_t hi, prun *out, int max) {
    if (!VKA_ENV("DM_VK_MAPS_CACHE", 1)) return prot_runs_text(lo, hi, out, max);
    pthread_mutex_lock(&g_snapmu);
    static uint64_t nuse, nread, nhit; static int nfail;
    if (g_snap.off) { pthread_mutex_unlock(&g_snapmu); return prot_runs_text(lo, hi, out, max); }
    int hit = 1;
    if (!snap_valid(lo, hi)) {
        int rd = snap_read();
        if (rd) nfail = 0;
        else if (++nfail >= 8) { g_snap.off = 1; VRED("placed memory: the protections snapshot could not be read 8 times in a row: read from the text from now on"); }   // (else every answer reads the text twice)
        if (!rd || !snap_valid(lo, hi)) { pthread_mutex_unlock(&g_snapmu); return prot_runs_text(lo, hi, out, max); }
        nread++; hit = 0;
    }
    int n = snap_runs(lo, hi, out, max); nuse++;
    // The check against the text, on hits only (a fresh read cannot be stale): the first 16, then every 64th. A difference is looked at up to 3 more times, 50 us apart:
    // another thread's call can be in the text before its mark (nvn_dirty's mprotect, then prot_mark); a later look finds it marked (snap_valid fails), which is no fault
    if (hit && (++nhit <= 16 || !(nhit & 63))) {
        prun o2[MAXRUN_]; int n2 = 0, d = 0, differs = 0;
        for (int pass = 0; pass < 4; pass++) {
            if (pass) { struct timespec ts = {0, 50000}; nanosleep(&ts, NULL); }
            n2 = prot_runs_text(lo, hi, o2, MAXRUN_);
            d = 0; while (d < n && d < n2 && out[d].lo == o2[d].lo && out[d].hi == o2[d].hi && out[d].prot == o2[d].prot) d++;   // the first run that differs (by field: prun has padding)
            differs = n2 && (d < n || d < n2);
            if (!differs || !snap_valid(lo, hi)) break;
        }
        if (differs && !snap_valid(lo, hi)) { memcpy(out, o2, (size_t)n2 * sizeof *out); n = n2; }   // changed meanwhile: the text's answer
        else if (differs) {
            g_snap.off = 1;
            VRED("placed memory: the protections snapshot of %#lx-%#lx (%d runs) differs from /proc/self/maps (%d runs) from run %d on: snapshot %#lx-%#lx prot %d, text %#lx-%#lx prot %d; read from the text from now on",
                 (unsigned long)lo, (unsigned long)hi, n, n2, d, d < n ? (unsigned long)out[d].lo : 0ul, d < n ? (unsigned long)out[d].hi : 0ul, d < n ? out[d].prot : -2,
                 d < n2 ? (unsigned long)o2[d].lo : 0ul, d < n2 ? (unsigned long)o2[d].hi : 0ul, d < n2 ? o2[d].prot : -2);
            memcpy(out, o2, (size_t)n2 * sizeof *out); n = n2;
        }
    }
    if (nuse == 64 || !(nuse & 4095)) VLOG("placed memory: protections from the snapshot, %llu reads of the text for %llu answers (%llu hits)", (unsigned long long)nread, (unsigned long long)nuse, (unsigned long long)nhit);
    pthread_mutex_unlock(&g_snapmu);
    return n;
}
static int prot_runs_text(uintptr_t lo, uintptr_t hi, prun *out, int max) {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return 0;
    char ln[512]; int n = 0, err = 0; uintptr_t at = lo;
    while (at < hi && maps_line(f, ln, sizeof ln, &err)) {
        g_maps_lines++;
        unsigned long a, b; char pm[8];
        if (sscanf(ln, "%lx-%lx %7s", &a, &b, pm) != 3 || b <= at) continue;
        if (a >= hi) break;
        if (a > at) { if (n == max) break; out[n++] = (prun){at, a, -1}; at = a; }
        int p = (pm[0] == 'r' ? PROT_READ : 0) | (pm[1] == 'w' ? PROT_WRITE : 0) | (pm[2] == 'x' ? PROT_EXEC : 0);
        uintptr_t e = b < hi ? b : hi;
        if (n && out[n - 1].prot == p && out[n - 1].hi == at) out[n - 1].hi = e; else { if (n == max) break; out[n++] = (prun){at, e, p}; }
        at = e;
    }
    fclose(f);
    if (err) return 0;
    if (at < hi && n < max) out[n++] = (prun){at, hi, -1};
    return at >= hi || (n && out[n - 1].hi == hi) ? n : 0;
}

// The swap's protection changes (sw_mprotect: unmarked, failures noted). A swap maps its whole range read-write, so it leaves the range as a snapshot has it only when every
// run was mapped (no -1 run, which the new map fills) and the runs were read at all (prot_known)
static void prot_apply(const prun *r, int n) { for (int i = 0; i < n; i++) if (r[i].prot >= 0 && r[i].prot != (PROT_READ | PROT_WRITE)) sw_mprotect(r[i].lo, r[i].hi - r[i].lo, r[i].prot); }
static void prot_readonly(const prun *r, int n) { for (int i = 0; i < n; i++) if (r[i].prot >= 0 && (r[i].prot & PROT_WRITE)) sw_mprotect(r[i].lo, r[i].hi - r[i].lo, r[i].prot & ~PROT_WRITE); }
// Place and unplace read the runs before swap_begin and put them back at the end, so a protection the dirty tracker (nvn_dirty.c) changed in the range in between would be
// undone (a page it unprotected made read-only again: state NONE, read-only, a writer spins; one it protected made writable: its writes missed). Placing, the tracker is
// held over the range (nvn_dirty_hold: its protected pages released, none protected until the swap ends), since a pool is placed when the GPU first gets an address in it
// (mtl_pool_need), and its textures may be tracked by then. Unplace swaps only segments no pool references, and hle_nvn.c removes a pool's tracked range before
// mtl_pool_del (unplace waits for fences, so it may not hold the tracker: the replay thread protects pages). Open (narrow): vka_mem_release over a live pool (a game
// error, [RED] there) and the guest's permission SVCs on pool memory
static int prot_known(prun *r, int *n, uintptr_t lo, uintptr_t hi) {   // 1 = the runs were read and all are mapped; with none read, [lo, hi) read-write is assumed (0)
    if (!*n) { r[0] = (prun){lo, hi, PROT_READ | PROT_WRITE}; *n = 1; return 0; }
    for (int i = 0; i < *n; i++) if (r[i].prot < 0) return 0;
    return 1;
}

// ---------- Placing and giving back ----------
static VkBuffer seg_buffer(VkDeviceSize n) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | (V.bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0)};
    VkBuffer b = VK_NULL_HANDLE; VkResult r = vk.vkCreateBuffer(V.dev, &bi, NULL, &b);
    return r ? VK_NULL_HANDLE : b;
}
static void unmap_placed(VkDeviceMemory m) { VkMemoryUnmapInfoKHR ui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .flags = V.unmap_reserve ? VK_MEMORY_UNMAP_RESERVE_BIT_EXT : 0, .memory = m}; vk.vkUnmapMemory2KHR(V.dev, &ui); }
static void copy_readable(uint8_t *to, uintptr_t lo, const prun *r, int n) {   // the readable runs of [lo, ...) into to; the rest zero
    for (int i = 0; i < n; i++) { uint8_t *d = to + (r[i].lo - lo); size_t k = r[i].hi - r[i].lo; if (r[i].prot >= 0 && (r[i].prot & PROT_READ)) memcpy(d, (const void *)r[i].lo, k); else memset(d, 0, k); }
}

// Every page of a new placed map is faulted in at once, while the map is still one mapping. The Adreno 840's kernel driver (kgsl) fills a page of its mapping on the first
// touch, and in a mapping that mprotect has split it takes the page at the touch's distance from the start of that piece, not from the start of the allocation: after the dirty
// tracker had write-protected a texture's pages (nvn_dirty.c; each protection splits the mapping), the first touch of a page past such a split read and wrote the allocation's
// first pages instead of its own. The game's writes went to the wrong memory, the GPU read zeros where the CPU had written, and the CPU read another texture's bytes: Donkey Kong
// Bananza's textures uploaded with blocks of other textures (ASTC 5x4 storage holding 8x8 blocks, which the Adreno decodes to NaN: black cores with white bloom on lamps, rocks
// and signs; magenta and wrong colours elsewhere). A page already in the page table keeps its page through any split, so none is left to be filled later (measured with
// DM_VK_NANSCAN's placed memory test, vka_tex.c ptest). The driver's memory is committed when allocated, so this commits nothing more
// No log line here: it runs inside the swap, with the dirty tracker held (nvn_dirty_hold), and a log line takes stderr's lock, which another thread may hold while it waits
// for the tracker: the game thread's frame statistics hold it while their arguments are read (the texture counts wait for the replay's texture lock), and the replay thread
// holds that lock while it waits for the tracker (nvn_dirty_protect). Pokemon Pokopia stopped there, every thread and the log with it. prefault_line logs after the swap
static uint64_t g_pf_ns, g_pf_bytes; static int g_pf_nlog, g_pf_refused;
static void prefault_line(void) {
    static int nr; if (g_pf_refused && !nr++) VLOG("placed memory: MADV_POPULATE_READ refused (errno %d): pages touched one by one", g_pf_refused);
    if (g_pf_nlog < 4 || !(g_pf_nlog & 255)) VLOG("placed memory: %.1f MiB faulted in so far, %.1f ms", g_pf_bytes / 1048576.0, g_pf_ns / 1e6); }   // (caller holds g_pl)
static void prefault(uintptr_t lo, uintptr_t hi) {
    uint64_t t0 = mono_ns();
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
    if (madvise((void *)lo, hi - lo, MADV_POPULATE_READ)) { g_pf_refused = errno ? errno : -1;
        for (uintptr_t a = lo; a < hi; a += 4096) (void)*(volatile const uint8_t *)a; }
    g_pf_ns += mono_ns() - t0; g_pf_bytes += hi - lo; g_pf_nlog++;
}

// Place [lo, hi) (no segment overlaps it; caller holds g_pl, and g_rw for writing only under DM_VK_MEM_LOCK_WHOLE). 1 = placed, 0 = failed with the range as it was; *grow = the size the driver wants when it is larger than the range
// Where a placement's time goes (logged every 256 placements): buffer and allocation, the protections read, the copy, the maps, the prefault, the protections put back
static uint64_t g_ph[6], g_ph_n;
static __thread uint64_t t_place_bytes, t_pend_ns;   // this thread's bytes placed and time in pend_wait (mtl_pool_add's attribution: other threads place while it waits)
static int place(uintptr_t lo, uintptr_t hi, VkDeviceSize *grow) {
    VkDeviceSize n = hi - lo; seg_t s = {lo, hi, .refs = 1}; VkResult r; uint64_t p0 = mono_ns(), p1, p2, p3, p4, p5;
    vka_fd_memory exported = {.fd = -1};
    if (V.fd_alias) {
        r = vka_fd_create(&vk, V.pd, V.dev, &V.memp, n, V.bda, &exported);
        if (r) { VRED("pool memory %#lx+%#llx: exported allocation %d", (unsigned long)lo, (unsigned long long)n, r); return 0; }
        s.buf = exported.buf; s.mem = exported.mem; s.fd_alias = 1;
    } else {
        if (!(s.buf = seg_buffer(n))) { VRED("pool memory %#lx+%#llx: vkCreateBuffer failed", (unsigned long)lo, (unsigned long long)n); return 0; }
        VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, s.buf, &mr);
        if (V.guest_type == UINT32_MAX || !(mr.memoryTypeBits >> V.guest_type & 1)) {
            VRED("pool memory %#lx+%#llx: the buffer takes types %#x, not the guest type %u", (unsigned long)lo, (unsigned long long)n, mr.memoryTypeBits, V.guest_type);
            vk.vkDestroyBuffer(V.dev, s.buf, NULL); return 0;
        }
        if (mr.size > n && !V.placed_range) { if (grow) *grow = mr.size; vk.vkDestroyBuffer(V.dev, s.buf, NULL); return 0; }   // a whole-memory map would cover bytes past the range
        VkMemoryAllocateFlagsInfo af = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = V.bda ? &af : NULL, .allocationSize = mr.size, .memoryTypeIndex = V.guest_type};
        if ((r = vk.vkAllocateMemory(V.dev, &ai, NULL, &s.mem))) {
            if (n <= V.max_alloc || !V.max_alloc) VRED("pool memory %#lx+%#llx: vkAllocateMemory %d (%.2f GiB placed so far)", (unsigned long)lo, (unsigned long long)n, r, g_placed_bytes / 1073741824.0);
            vk.vkDestroyBuffer(V.dev, s.buf, NULL); return 0;
        }
        vk.vkBindBufferMemory(V.dev, s.buf, s.mem, 0);
    }
    p1 = mono_ns(); g_ph[0] += p1 - p0;
    nvn_dirty_hold((const void *)lo, n);   // (see the note above prot_known; released after swap_end)
    prun pr[MAXRUN]; int npr = prot_runs(lo, hi, pr, MAXRUN), exact = prot_known(pr, &npr, lo, hi);
    p2 = mono_ns(); g_ph[1] += p2 - p1; p1 = p2;
    void *p = NULL; int ok = 0;
    swap_begin(lo, hi);
    prot_readonly(pr, npr);   // writers wait in the fault hook from here
    if ((s.fd_alias ? (p = exported.mapped, 0) : vk.vkMapMemory(V.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &p)) == 0 && p) {
        copy_readable(p, lo, pr, npr);
        p3 = mono_ns(); g_ph[2] += p3 - p2; p2 = p3;
        if (s.fd_alias) {
            void *q = (void *)syscall(__NR_mmap, lo, (size_t)n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, exported.fd, 0);
            ok = q == (void *)lo; r = ok ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED;
            if (!ok) { sw_hole_check(lo, n, 0, "exported pool memory"); if (sw_mmap_fixed(lo, n) != MAP_FAILED) memcpy((void *)lo, p, n); }
        } else {
            vk.vkUnmapMemory(V.dev, s.mem);
            VkMemoryMapPlacedInfoEXT pi = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)lo};
            VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = s.mem, .offset = 0, .size = V.placed_range ? n : VK_WHOLE_SIZE};
            void *q = NULL;
            if (!(r = vk.vkMapMemory2KHR(V.dev, &mi, &q)) && q == (void *)lo) ok = 1;   // (the driver's own mmap: not marked, see swap_end_ok below)
            else {   // the range may be gone now: put anonymous pages back with the bytes, which the memory still holds
                if (!r) unmap_placed(s.mem);
                sw_hole_check(lo, n, 0, "pool memory");
                void *a = sw_mmap_fixed(lo, n);
                if (a != MAP_FAILED && !vk.vkMapMemory(V.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &p)) { memcpy((void *)lo, p, n); vk.vkUnmapMemory(V.dev, s.mem); }
            }
        }
    }
    if (exported.fd >= 0) close(exported.fd);
    static int pf = -1; if (pf < 0) { const char *e = getenv("DM_VK_PREFAULT"); pf = !(e && !strcmp(e, "0")); if (!pf) VLOG("DM_VK_PREFAULT=0: placed maps are not faulted in (a diagnostic: DM_VK_NANSCAN's placed memory test shows whether the driver needs it)"); }
    p3 = mono_ns();
    if (ok && pf) prefault(lo, hi);   // before any mprotect splits the mapping (see prefault)
    p4 = mono_ns();
    if (!ok) { for (int i = 0; i < npr; i++) if (pr[i].prot >= 0) sw_mprotect(pr[i].lo, pr[i].hi - pr[i].lo, pr[i].prot); }
    else prot_apply(pr, npr);   // the placed map is read-write: the protections the range had
    swap_end_ok(ok && exact);   // (marked if it failed, a call failed, the runs were not all known or a snapshot was read meanwhile)
    swap_end();
    nvn_dirty_unhold();
    if (ok && pf) prefault_line();
    p5 = mono_ns();
    g_ph[3] += p3 - p2; g_ph[4] += p4 - p3; g_ph[5] += p5 - p4;
    if (!(++g_ph_n & 255)) VLOG("placed memory: %llu placements: allocation %.0f ms, protections read %.0f ms (%.0f lines of /proc/self/maps a read), copy %.0f ms, maps %.0f ms, prefault %.0f ms, protections back %.0f ms; %d memory maps of %d at the last count",
                               (unsigned long long)g_ph_n, g_ph[0] / 1e6, g_ph[1] / 1e6, (double)g_maps_lines / g_ph_n, g_ph[2] / 1e6, g_ph[3] / 1e6, g_ph[4] / 1e6, g_ph[5] / 1e6, atomic_load(&g_maps_n), atomic_load(&g_maps_max));
    if (!ok) { VRED("pool memory %#lx+%#llx: placed map %d at %p", (unsigned long)lo, (unsigned long long)n, r, p); if (s.fd_alias) vk.vkUnmapMemory(V.dev, s.mem); vk.vkDestroyBuffer(V.dev, s.buf, NULL); vk.vkFreeMemory(V.dev, s.mem, NULL); return 0; }
    if (V.bda) { VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = s.buf}; s.bda = vk.vkGetBufferDeviceAddress(V.dev, &bai); }
    if (!lock_whole()) pthread_rwlock_wrlock(&g_rw);
    sg_insert(&s);
    g_placed_bytes += n; if (g_placed_bytes > g_placed_peak) g_placed_peak = g_placed_bytes; t_place_bytes += n;
    if (!lock_whole()) pthread_rwlock_unlock(&g_rw);
    vka_mem_grew(n);   // (its log line outside the table's lock: no log line under a lock another logging thread may wait for)
    return 1;
}

// Segments taken out of the table, to be given back once the lock is dropped
typedef struct { seg_t *s; int n, cap; } segvec;
// Ranges detached and not yet given back (unplace_all runs without g_pl). A placement over one of them (a pool sharing an alignment granule with a finalized one, the
// grow path) would put new memory there that the pending give-back then copies from, unmaps and frees (the free takes the new placed range down). cover waits until the
// give-backs over its range end (pend_wait). It drops g_pl while it waits (a give-back waits for fences: other placements must not stall behind it) and cover looks at
// the table again afterwards. Lock order g_pl, g_rw, g_pendmu
static struct { uintptr_t lo, hi; } *g_pend; static int g_npend, g_pendcap;
static pthread_mutex_t g_pendmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pendcv = PTHREAD_COND_INITIALIZER;
static int pend_overlaps(uintptr_t lo, uintptr_t hi) { for (int i = 0; i < g_npend; i++) if (g_pend[i].lo < hi && lo < g_pend[i].hi) return 1; return 0; }   // under g_pendmu
// Caller holds g_pl (and g_rw for writing under DM_VK_MEM_LOCK_WHOLE); 1 = it waited, with both dropped meanwhile and taken again: the table may have changed
static int pend_wait(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_pendmu); int busy = pend_overlaps(lo, hi); pthread_mutex_unlock(&g_pendmu);
    if (!busy) return 0;
    uint64_t t0 = mono_ns();
    if (lock_whole()) pthread_rwlock_unlock(&g_rw);
    pthread_mutex_unlock(&g_pl);
    pthread_mutex_lock(&g_pendmu); while (pend_overlaps(lo, hi)) pthread_cond_wait(&g_pendcv, &g_pendmu); pthread_mutex_unlock(&g_pendmu);
    pthread_mutex_lock(&g_pl);
    if (lock_whole()) pthread_rwlock_wrlock(&g_rw);
    uint64_t dt = mono_ns() - t0; t_pend_ns += dt;
    static int nl; if (nl++ < 8) VLOG("pool memory %#lx+%#lx: waited %.1f ms for the give-back of an overlapping segment before placing", (unsigned long)lo, (unsigned long)(hi - lo), dt / 1e6);
    return 1;
}
static void pend_done(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_pendmu);
    for (int i = 0; i < g_npend; i++) if (g_pend[i].lo == lo && g_pend[i].hi == hi) { g_pend[i] = g_pend[--g_npend]; break; }
    pthread_cond_broadcast(&g_pendcv); pthread_mutex_unlock(&g_pendmu);
}
static void detach(int i, segvec *v) {   // caller holds g_pl, and g_rw for writing
    pthread_mutex_lock(&g_pendmu);
    if (g_npend == g_pendcap) { g_pendcap = g_pendcap ? 2 * g_pendcap : 16; g_pend = realloc(g_pend, (size_t)g_pendcap * sizeof *g_pend); }
    g_pend[g_npend].lo = g_sg[i].lo; g_pend[g_npend++].hi = g_sg[i].hi;
    pthread_mutex_unlock(&g_pendmu);
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 8; v->s = realloc(v->s, (size_t)v->cap * sizeof *v->s); }
    v->s[v->n++] = g_sg[i]; g_placed_bytes -= g_sg[i].hi - g_sg[i].lo;
    atomic_fetch_add(&g_sg_gen, 1);
    memmove(&g_sg[i], &g_sg[i + 1], (size_t)(g_nsg - i - 1) * sizeof *g_sg); g_nsg--;
}
// Give a detached segment back: anonymous memory with its bytes and protection over its range. Called without g_rw (a submission being recorded may hold it while its fence
// is reserved). The Vulkan memory is freed before the anonymous pages move in: the Adreno driver's vkFreeMemory takes the placed range down even after an unmap with
// VK_MEMORY_UNMAP_RESERVE_BIT_EXT, so freeing it later (once the GPU was past it) unmapped the game's heap pages there and the next access faulted (DKB, after its
// accessibility screen). So the GPU is waited for first; the game finalizes a pool only once its GPU work is done, so the wait is short. It comes before the range turns
// read-only: the completion thread writes events and counter reports into pools, and one that faults on this range would wait for the swap while the swap waits for it.
// Memory the GPU is not known to be done with (the wait ran into the hang time, or a GPU hang was declared) is never freed: it stays allocated (leaked) behind the anonymous pages,
// since a free under work the GPU still runs faults the GPU (freed anyway after 2 s before, on the way to a system restart on the phone)
static void seg_free(const seg_t *s, int waited, uint64_t fence) {
    if (waited) { if (s->fd_alias) vk.vkUnmapMemory(V.dev, s->mem); vk.vkDestroyBuffer(V.dev, s->buf, NULL); vk.vkFreeMemory(V.dev, s->mem, NULL); return; }
    static _Atomic int nk; if (atomic_fetch_add(&nk, 1) < 8) VRED("giving back pool memory %#lx+%#zx: the GPU is not known to be done with it (fence %llu%s): its Vulkan memory is kept, not freed",
                                                               (unsigned long)s->lo, (size_t)(s->hi - s->lo), (unsigned long long)fence, vka_hung() ? ", GPU hang" : " not reached in the hang time");
}
static void unplace(const seg_t *sp) {
    seg_t s = *sp; size_t n = s.hi - s.lo;
    uint64_t fence = vka_fence_last();
    int waited = vka_wait_done(fence, vka_hang_ns()) && !vka_hung();
    prun pr[MAXRUN]; int npr = prot_runs(s.lo, s.hi, pr, MAXRUN), exact = prot_known(pr, &npr, s.lo, s.hi);
    swap_begin(s.lo, s.hi);
    prot_readonly(pr, npr);
    uint8_t *t = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t != MAP_FAILED)
        for (int k = 0; k < npr; k++) {
            if (pr[k].prot < 0 || !(pr[k].prot & PROT_READ)) continue;
            for (uintptr_t a = pr[k].lo; a < pr[k].hi; a += 4096) {   // zero pages stay untouched (uncommitted)
                const uint64_t *w = (const uint64_t *)a; uint64_t o = 0; for (int j = 0; j < 512 && !o; j++) o |= w[j];
                if (o) memcpy(t + (a - s.lo), (const void *)a, 4096);
            }
        }
    if (!s.fd_alias) unmap_placed(s.mem);   // the exported alias is replaced below before its independent driver map is freed
    // DM_VK_UNPLACE_REMAP=1 (experiment, off): map the memory again where the driver likes, so that the address the driver keeps is outside the guest range, then move the
    // pages in before the free (no hole, if the driver's free takes down only its last map). Checked after the free: a range the free took down anyway is [RED]
    void *elsewhere = NULL;
    if (!s.fd_alias && waited && t != MAP_FAILED && VKA_ENV("DM_VK_UNPLACE_REMAP", 0) && (vk.vkMapMemory(V.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &elsewhere) || !elsewhere)) elsewhere = NULL;
    if (!s.fd_alias && !elsewhere) seg_free(&s, waited, fence);
    // Open: from the driver's vkFreeMemory, which takes the range down (above), to this mremap the guest range has a hole, and another thread's mmap(NULL) (the driver's,
    // libc's malloc: calls this library does not see) could be given part of it. The window is the few microseconds between the two calls, and it cannot be closed here:
    // anything mapped into the range before the free (a reservation, these pages) is taken down by the free, and the free cannot wait until after (the bug above). What
    // is done: sw_hole_check reserves the range and says so ([RED]) when another mapping took part of it, which the mremap then replaces. A driver that keeps the range
    // reserved after an unmap with VK_MEMORY_UNMAP_RESERVE_BIT_EXT, as the extension specifies (the application owns the range from the unmap on), has no hole: the
    // mremap replaces that reservation. DM_VK_UNPLACE_REMAP is the experiment for a driver that does not (not yet tried on the phone)
    if (!s.fd_alias && t != MAP_FAILED) sw_hole_check(s.lo, n, waited && !elsewhere, "giving back pool memory");
    if (t == MAP_FAILED || sw_mremap_to(t, n, s.lo) == MAP_FAILED) {
        VRED_ONCE("giving back pool memory %#lx+%#zx: no anonymous pages with its bytes (errno %d): they are lost", (unsigned long)s.lo, n, errno);
        if (t != MAP_FAILED) munmap(t, n);
        sw_mmap_fixed(s.lo, n);
    } else plat_mem_name_guest((void *)s.lo, n);   // (its name in the maps; moved pages keep their own offset, so these do not merge with neighbours until the range is given back to the system)
    if (s.fd_alias) seg_free(&s, waited, fence);
    if (elsewhere) {
        vk.vkUnmapMemory(V.dev, s.mem); seg_free(&s, waited, fence);
        if (msync((void *)s.lo, n, MS_ASYNC)) {   // the free took the range down after all: the bytes are lost
            static int nr; if (nr++ < 4) VRED("DM_VK_UNPLACE_REMAP: the driver's vkFreeMemory took down pool memory %#lx+%#zx after the anonymous pages moved in: the experiment fails on this driver (the bytes are lost)", (unsigned long)s.lo, n);
            sw_mmap_fixed(s.lo, n);
        } else { static int nl; if (!nl++) VLOG("DM_VK_UNPLACE_REMAP: pool memory %#lx+%#zx freed after the anonymous pages moved in, and the range stayed mapped", (unsigned long)s.lo, n); }
    }
    for (int k = 0; k < npr; k++) if (pr[k].prot >= 0 && pr[k].prot != (PROT_READ | PROT_WRITE)) sw_mprotect(pr[k].lo, pr[k].hi - pr[k].lo, pr[k].prot);
    swap_end_ok(t != MAP_FAILED && exact);   // (marked as well when a call failed, e.g. the mremap and its fallback map)
    swap_end();
    uint64_t left = vka_mem_placed_bytes(NULL);
    static int nlog; if (nlog++ < 4) VLOG("pool memory %#lx+%#zx given back as anonymous memory (%.2f GiB placed)", (unsigned long)s.lo, n, left / 1073741824.0);
}
static void unplace_all(segvec *v) { for (int i = 0; i < v->n; i++) { unplace(&v->s[i]); pend_done(v->s[i].lo, v->s[i].hi); } free(v->s); *v = (segvec){0}; }

// The segments one pool holds references on (by start)
typedef struct { uintptr_t *lo; int n, cap; } seglist;
static void sl_add(seglist *l, uintptr_t lo) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 2; l->lo = realloc(l->lo, (size_t)l->cap * sizeof *l->lo); } l->lo[l->n++] = lo; }
static int sl_has(const seglist *l, uintptr_t lo) { for (int i = 0; i < l->n; i++) if (l->lo[i] == lo) return 1; return 0; }
static int place_ref(uintptr_t lo, uintptr_t hi, VkDeviceSize *grow, seglist *l) { if (!place(lo, hi, grow)) return 0; sl_add(l, lo); return 1; }
// Make [lo, hi) GPU-visible: segments over what no segment covers yet, references on the ones that do, all listed in l. 0 = some part is not placed. Caller holds g_pl
// (so the table changes under no one else here: it is read without g_rw, and written under it), except while pend_wait waits: then the step starts again at a.
// A segment l lists already (a pool placed by parts holds its earlier parts there) takes no second reference
static int cover(uintptr_t lo, uintptr_t hi, seglist *l) {
    int ok = 1;
    for (uintptr_t a = lo; a < hi;) {
        int k = sg_find(a);
        if (k >= 0 && a < g_sg[k].hi) {
            if (!sl_has(l, g_sg[k].lo)) { if (!lock_whole()) pthread_rwlock_wrlock(&g_rw); g_sg[k].refs++; if (!lock_whole()) pthread_rwlock_unlock(&g_rw); sl_add(l, g_sg[k].lo); }
            a = g_sg[k].hi; continue;
        }
        uintptr_t e = k + 1 < g_nsg && g_sg[k + 1].lo < hi ? g_sg[k + 1].lo : hi;   // the gap up to the next segment
        VkDeviceSize grow = 0;
        if (pend_wait(a, e)) continue;
        if (place_ref(a, e, &grow, l)) { a = e; continue; }
        if (grow) {   // the driver wants more than the gap: take the bytes after it too, when nothing placed is there
            uintptr_t g = a + ((grow + pg_align() - 1) & ~(VkDeviceSize)(pg_align() - 1));
            if (k + 1 >= g_nsg || g_sg[k + 1].lo >= g) { if (pend_wait(a, g)) continue; if (place_ref(a, g, NULL, l)) { a = g; continue; } }
            VRED("pool memory %#lx+%#lx: the driver wants %llu bytes, which would overlap placed memory: not placed", (unsigned long)a, (unsigned long)(e - a), (unsigned long long)grow);
            ok = 0; a = e; continue;
        }
        uintptr_t piece = V.max_alloc ? (uintptr_t)V.max_alloc & ~(pg_align() - 1) : 0;
        if (piece && e - a > piece) {   // larger than one allocation can be: pieces of maxMemoryAllocationSize (a lookup answers within one piece)
            VLOG("pool memory %#lx+%#lx is larger than one allocation: placed in pieces of %lu MiB", (unsigned long)a, (unsigned long)(e - a), (unsigned long)(piece >> 20));
            for (uintptr_t b = a; b < e; b += piece) ok &= place_ref(b, b + piece < e ? b + piece : e, NULL, l);
            a = e; continue;
        }
        ok = 0; a = e;
    }
    return ok;
}
// Drop a pool's references; segments no pool needs any more are given back. Caller holds g_pl and g_rw for writing
static void uncover(seglist *l, segvec *v) {
    for (int j = 0; j < l->n; j++) { int k = sg_find(l->lo[j]); if (k >= 0 && g_sg[k].lo == l->lo[j] && --g_sg[k].refs <= 0) detach(k, v); }
    free(l->lo); *l = (seglist){0};
}

// Without placed maps (an emulator's driver), host memory the GPU sees: one allocation of the guest type mapped wherever the driver maps it, entered as a segment at that address.
// For the selftest only (a game's pools must sit at their own addresses); it stays for the process's life
void *vka_mem_mapped(size_t n) {
    if (!vka_init()) return NULL;
    seg_t s = {.refs = 1 << 30}; void *p = NULL; VkMemoryRequirements mr;
    if (!(s.buf = seg_buffer(n))) return NULL;
    vk.vkGetBufferMemoryRequirements(V.dev, s.buf, &mr);
    VkMemoryAllocateFlagsInfo af = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = V.bda ? &af : NULL, .allocationSize = mr.size, .memoryTypeIndex = V.guest_type};
    if (V.guest_type == UINT32_MAX || !(mr.memoryTypeBits >> V.guest_type & 1) || vk.vkAllocateMemory(V.dev, &ai, NULL, &s.mem) || vk.vkBindBufferMemory(V.dev, s.buf, s.mem, 0)
        || vk.vkMapMemory(V.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &p)) { vk.vkDestroyBuffer(V.dev, s.buf, NULL); if (s.mem) vk.vkFreeMemory(V.dev, s.mem, NULL); return NULL; }
    memset(p, 0, n);
    s.lo = (uintptr_t)p; s.hi = s.lo + n;
    if (V.bda) { VkBufferDeviceAddressInfo bdi = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = s.buf}; s.bda = vk.vkGetBufferDeviceAddress(V.dev, &bdi); }
    pthread_mutex_lock(&g_pl); pthread_rwlock_wrlock(&g_rw); sg_insert(&s); pthread_rwlock_unlock(&g_rw); pthread_mutex_unlock(&g_pl);
    return p;
}

// The host gives [lo, hi) back (heap shrink, UnmapPhysicalMemory). Pools live there are a game error: their segments are given back first (logged), then the host's own release runs
static void lazy_release_note(uintptr_t lo, uintptr_t hi);
int vka_mem_release(uintptr_t lo, uintptr_t hi) {
    if (!vka_ok() || hi <= lo) return 0;
    segvec v = {0};
    pthread_mutex_lock(&g_pl); pthread_rwlock_wrlock(&g_rw);
    int k = sg_find(lo); if (k < 0 || g_sg[k].hi <= lo) k++;
    while (k < g_nsg && g_sg[k].lo < hi) {
        static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) VRED("guest memory %#lx+%#lx is given back under live pool memory %#lx+%#lx: that memory leaves the GPU", (unsigned long)lo, (unsigned long)(hi - lo), (unsigned long)g_sg[k].lo, (unsigned long)(g_sg[k].hi - g_sg[k].lo));
        detach(k, &v);
    }
    pthread_rwlock_unlock(&g_rw); pthread_mutex_unlock(&g_pl);
    unplace_all(&v);
    lazy_release_note(lo, hi);   // pools not placed yet (DM_VK_PLACE_LAZY) over the range: their storage leaves the guest too
    return 0;
}

// The segments this thread found, by address block (the draws look up their vertex and uniform buffers, ~10 lookups a draw): valid while g_sg_gen stands.
// A segment is unplaced only after the last fence reserved has completed (unplace), so one still in a cache that the generation has not caught yet is not freed under
// a submission that uses it
enum { NLC = 1024 };   // direct-mapped by 64 KiB block of the address: the level's ~8,000 draws read buffers in ~1,000 segments
typedef struct { uint64_t gen; seg_t s[NLC]; } lcache;
static __thread lcache *t_lc;
// The recording thread's cache outside TLS: a __thread variable of a dlopen'ed library is reached through the dynamic TLS descriptor resolver at every access, and the
// lookups run ~10 times a draw (profile p2: the lookup 8 % of the replay thread, the resolver in it). The first thread that records a submission (vka_mem_rec_thread) owns it
// for the life of the process; any other thread keeps its own TLS cache, so the global one is only ever touched by its owner (DM_VK_LC_GLOBAL=0: TLS for every thread)
static lcache g_rlc; static _Atomic(void *) g_rlc_tp;
void vka_mem_rec_thread(void) {
    void *tp = __builtin_thread_pointer(), *none = NULL;
    if (!atomic_load_explicit(&g_rlc_tp, memory_order_relaxed) && VKA_ENV("DM_VK_LC_GLOBAL", 1)) atomic_compare_exchange_strong(&g_rlc_tp, &none, tp);
}
static const seg_t *lookup(uintptr_t a, seg_t *tmp) {   // the segment holding a, or NULL
    lcache *c;
    if (__builtin_thread_pointer() == atomic_load_explicit(&g_rlc_tp, memory_order_relaxed)) c = &g_rlc;
    else { c = t_lc; if (!c) c = t_lc = calloc(1, sizeof *c); }
    uint64_t gen = atomic_load_explicit(&g_sg_gen, memory_order_acquire);
    seg_t *e = c ? &c->s[(a >> 16) & (NLC - 1)] : NULL;
    if (e && c->gen == gen && a >= e->lo && a < e->hi) return e;
    if (pthread_rwlock_tryrdlock(&g_rw)) {   // contended: a table edit (or, under DM_VK_MEM_LOCK_WHOLE, a placement) holds it
        uint64_t t0 = mono_ns(); pthread_rwlock_rdlock(&g_rw);
        atomic_fetch_add_explicit(c == &g_rlc ? &g_lkw_rec_ns : &g_lkw_oth_ns, mono_ns() - t0, memory_order_relaxed);
    }
    gen = atomic_load_explicit(&g_sg_gen, memory_order_relaxed);
    int i = sg_find(a); const seg_t *r = NULL;
    if (i >= 0 && a < g_sg[i].hi) {
        *tmp = g_sg[i]; r = tmp;
        if (c) { if (c->gen != gen) { memset(c->s, 0, sizeof c->s); c->gen = gen; } *e = g_sg[i]; }
    }
    pthread_rwlock_unlock(&g_rw);
    return r;
}
int vka_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *bda) {
    seg_t tmp; const seg_t *g = lookup((uintptr_t)addr, &tmp);
    int ok = g && addr + len <= g->hi;
    if (ok) { if (buf) *buf = g->buf; if (off) *off = addr - g->lo; if (bda) *bda = g->bda ? g->bda + (addr - g->lo) : 0; }
    return ok;
}

// Diagnostics (DM_VK_NANSCAN): the segment holding addr and its neighbours
void vka_mem_describe(uint64_t addr, char *out, size_t n) {
    pthread_rwlock_rdlock(&g_rw);
    int i = sg_find((uintptr_t)addr); size_t o = 0;
    o += (size_t)snprintf(out + o, n - o, "%d segments; ", g_nsg);
    for (int k = i - 1; k <= i + 1 && o < n; k++) if (k >= 0 && k < g_nsg)
        o += (size_t)snprintf(out + o, n - o, "%s[%#lx-%#lx refs %d mem %#llx] ", k == i ? "*" : "", (unsigned long)g_sg[k].lo, (unsigned long)g_sg[k].hi, g_sg[k].refs, (unsigned long long)(uintptr_t)g_sg[k].mem);
    pthread_rwlock_unlock(&g_rw);
}
uint64_t vka_addr_run(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    seg_t tmp; const seg_t *g = lookup((uintptr_t)addr, &tmp); uint64_t n = 0;
    if (g) { n = g->hi - addr; if (n > len) n = len; *buf = g->buf; *off = addr - g->lo; }
    return n;
}

// Raw global memory's sink (dmsh_abi.h DMSH_GMEM_MAP_*, entry 0's words 6 and 7; vk_android_spv.py gmem_sink): the device address of a buffer of its own that the library's
// branchless helpers load from and store to when an address lies in no pool, so such an access touches no guest memory (the translator's helpers skipped it in a branch).
// The buffer is 16 KiB, the sink the 8 KiB-aligned address in it. Made once; 0 without buffer device addresses or when it could not be made (logged)
uint64_t vka_mem_sink(void) {
    static uint64_t bda; static int tried;
    if (tried || !V.bda) return bda;
    tried = 1;
    VkBuffer b; VkDeviceMemory mem; VkMemoryRequirements mr;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * VKA_GMEM_SINK_ALIGN, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &b)) { VRED("raw global memory sink: vkCreateBuffer failed"); return 0; }
    vk.vkGetBufferMemoryRequirements(V.dev, b, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, 0, 0);
    VkMemoryAllocateFlagsInfo af = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &af, .allocationSize = mr.size, .memoryTypeIndex = t};
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &mem)) { VRED("raw global memory sink: no memory"); vk.vkDestroyBuffer(V.dev, b, NULL); return 0; }
    vk.vkBindBufferMemory(V.dev, b, mem, 0);
    VkBufferDeviceAddressInfo bdi = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
    bda = vka_gmem_sink_at(vk.vkGetBufferDeviceAddress(V.dev, &bdi));   // 8 KiB aligned inside the buffer: its low word never reads as a hint (vka_gmem.h)
    return bda;   // (kept for the process's life)
}

// The placed segments in address order (raw global memory's table, vka_draw.c gmem_map): at most max, the count returned
int vka_mem_chunks(uint64_t *lo, uint64_t *hi, uint64_t *bda, int max) {
    pthread_rwlock_rdlock(&g_rw);
    int n = 0; for (int i = 0; i < g_nsg && n < max; i++) if (g_sg[i].bda) { lo[n] = g_sg[i].lo; hi[n] = g_sg[i].hi; bda[n] = g_sg[i].bda; n++; }
    if (n == max && n < g_nsg) VRED_ONCE("more than %d placed segments: raw global memory sees the first %d", max, max);
    pthread_rwlock_unlock(&g_rw);
    return n;
}

// ---------- Pools ----------
// A pool becomes placed memory when the GPU is first given an address in it (mtl_pool_need: a buffer made on it, its buffer address, a texture's address, a linear texture,
// whose GPU content is copied back into the storage), not when the game makes it. Until then it stays anonymous guest memory, committed page by page as the CPU writes it:
// the replay reads textures from guest memory on the CPU (uploads into images of their own), and NVN hands the GPU no other address of a pool. Mario Kart World 1.8.0 makes
// 4,641 MiB of pools before its first frame, of which 765 MiB hold anything that is not zero: a 2.37 GiB texture heap that holds 349 MiB of textures and no buffer,
// 1,183 MiB of physical pools (GPU_NO_ACCESS) that only back virtual pools, 371 MiB of CPU_NO_ACCESS texture pools; placed at Initialize, all of it was committed
// (docs/reference/memory.md "Placed pools commit what the game only reserved"). DM_VK_PLACE_LAZY=0 places every pool at Initialize, as before.
// Large pools are placed by parts (vka_place.h): a pool of at least DM_VK_PLACE_PART_MB (1024; 0 = whole pools, as before) becomes placed memory granule by granule
// (DM_VK_PLACE_GRANULE_MB, 16, counted from the pool's start): the granules the GPU is given an address in, not the whole pool. Pokemon Pokopia makes a 3.4 GiB pool
// before its first frame and gives the GPU a buffer 38 MiB into it: placed whole, its 3.4 GiB of driver memory (committed when allocated) and the rest made the low-memory
// killer end the game before its first frame. NVN gives the GPU an address only by a buffer, a texture or the pool's own address (which places the rest of the pool),
// so a granule no buffer or texture lies in is one the GPU is never sent to. A range given to the GPU across a granule placed earlier and one placed now lies in two
// segments: lookups over the whole range fail there (vka_addr; runs, as for copies and uniform buffers, still work), [RED] once per pool
typedef struct { uintptr_t mem; uint64_t size; seglist segs; int lazy, part; uint64_t id, pbytes; uint32_t nparts, nspan; uint64_t *gmap; } pool_ent;   // gmap: a pool placed by parts, one bit a granule placed (under g_pmu)   // lazy: 1 not (wholly) placed yet, 2 being placed (mtl_pool_need); part: placed by parts (pbytes placed so far, in nparts placements); id: unique (two pools may share a start)
static pool_ent *g_pool; static int g_npool, g_poolcap;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_lzcv = PTHREAD_COND_INITIALIZER;   // g_lzcv: a placement by mtl_pool_need ended
static _Atomic int g_nlazy; static uint64_t g_lazy_bytes, g_pool_id;   // pools not (wholly) placed yet and their bytes not placed (g_lazy_bytes, g_pool_id under g_pmu)
static _Atomic uint64_t g_lzgen = 1;   // bumped when a pool not placed yet comes (mtl_pool_add): mtl_pool_need's per-thread record of ranges with nothing left to place holds while it stands
static int lazy_on(void) { static int v = -1; if (v < 0) { v = VKA_ENV("DM_VK_PLACE_LAZY", 1) != 0; if (!v) VLOG("DM_VK_PLACE_LAZY=0: every memory pool is placed when it is made"); } return v; }
static uint64_t part_min(void) { static int64_t v = -1; if (v < 0) { v = (int64_t)VKA_ENV("DM_VK_PLACE_PART_MB", 1024) << 20; if (v < 0) v = 0; if (!v) VLOG("DM_VK_PLACE_PART_MB=0: every memory pool is placed whole"); } return (uint64_t)v; }
static uint64_t part_gran(void) {
    static int64_t v = -1;
    if (v < 0) { int64_t m = VKA_ENV("DM_VK_PLACE_GRANULE_MB", 16); if (m < 1) m = 1; v = m << 20; uintptr_t al = pg_align(); v = (v + (int64_t)al - 1) & ~(int64_t)(al - 1); }
    return (uint64_t)v;
}
static int can_place(uint64_t size) { return size && vka_ok() && placed_on() && (V.placed || V.fd_alias); }
// Place [mem, mem+size) (page aligned out): its segments into *l (which may list some already: they take no second reference). Caller holds g_pl. *lg: the placed total
// crossed a 256 MiB step since the last line (*tot). *mine: the bytes this call placed
static int place_pool(uintptr_t mem, uint64_t size, seglist *l, int *lg, uint64_t *tot, uint64_t *mine_out) {
    uintptr_t al = pg_align(), lo = mem & ~(al - 1), hi = (mem + size + al - 1) & ~(al - 1);
    if (lock_whole()) pthread_rwlock_wrlock(&g_rw);
    // The placement's own time and bytes: from here (not the wait for another placement), less the time cover waited for give-backs (pend_wait, which lets other
    // placements run: their bytes are theirs)
    uint64_t b0 = t_place_bytes, tw = mono_ns(); t_pend_ns = 0;
    int placed = cover(lo, hi, l); *tot = g_placed_bytes;
    uint64_t mine = t_place_bytes - b0, waitns = t_pend_ns;
    if (*tot >= g_placed_logged + (256ull << 20) || *tot + (256ull << 20) <= g_placed_logged) { g_placed_logged = *tot; *lg = 1; }
    if (lock_whole()) pthread_rwlock_unlock(&g_rw);
    if (mine) { atomic_fetch_add(&g_place_bytes, mine); atomic_fetch_add(&g_place_n, 1); }
    atomic_fetch_add(&g_place_ns, mono_ns() - tw - waitns);
    if (mine_out) *mine_out = mine;
    return placed;
}
static void placed_line(uint64_t tot) {
    pthread_mutex_lock(&g_pmu); int n = g_npool, nl = atomic_load(&g_nlazy), np = 0; uint64_t lb = g_lazy_bytes;
    for (int i = 0; i < g_npool; i++) if (g_pool[i].lazy && g_pool[i].pbytes) np++;
    pthread_mutex_unlock(&g_pmu);
    VLOG("%.2f GiB of memory pools is placed Vulkan memory (%d pools whole, %d by parts; %d pools, %.2f GiB not placed: no GPU address taken in them yet)", tot / 1073741824.0, n - nl, np, nl, lb / 1073741824.0);
}
static void maps_start(void);
void mtl_pool_add(const void *mem, uint64_t size) {
    vka_init(); swap_hooks(); maps_start();
    int placed = 0, lg = 0, lazy = 0, part = 0; uint64_t t0 = mono_ns(), tot = 0; seglist l = {0};
    if (can_place(size) && lazy_on()) { lazy = 1; part = vkp_by_parts(size, part_min(), part_gran()); }
    else if (can_place(size)) { pthread_mutex_lock(&g_pl); placed = place_pool((uintptr_t)mem, size, &l, &lg, &tot, NULL); pthread_mutex_unlock(&g_pl); }
    else placed = vka_addr((uintptr_t)mem, size, NULL, NULL, NULL);   // the selftest's mapped allocation
    pthread_mutex_lock(&g_pmu);
    if (g_npool == g_poolcap) { g_poolcap = g_poolcap ? g_poolcap * 2 : 64; g_pool = realloc(g_pool, (size_t)g_poolcap * sizeof *g_pool); }
    g_pool[g_npool++] = (pool_ent){(uintptr_t)mem, size, l, lazy, part, ++g_pool_id, 0, 0, 0, part ? calloc((size_t)(size / part_gran() / 64 + 1), 8) : NULL};
    if (lazy) { atomic_fetch_add(&g_nlazy, 1); g_lazy_bytes += size; atomic_fetch_add(&g_lzgen, 1); }
    static int nlog; int first = nlog++ < 8;
    pthread_mutex_unlock(&g_pmu);
    if (lazy) {
        static _Atomic int once, oncep;
        if (!atomic_exchange(&once, 1)) VLOG("memory pool %p+%#llx: placed when the GPU is first given an address in it (DM_VK_PLACE_LAZY)", mem, (unsigned long long)size);
        if (part && !atomic_exchange(&oncep, 1)) VLOG("memory pool %p+%#llx: placed by parts of %llu MiB, those the GPU is given an address in (pools of %llu MiB and more; DM_VK_PLACE_PART_MB, DM_VK_PLACE_GRANULE_MB)",
                                                      mem, (unsigned long long)size, (unsigned long long)(part_gran() >> 20), (unsigned long long)(part_min() >> 20));
        return;
    }
    VkDeviceAddress bda = 0; vka_addr((uintptr_t)mem, 1, NULL, NULL, &bda);
    if (!placed) { static _Atomic int nred; if (size && atomic_fetch_add(&nred, 1) < 8) VRED("memory pool %p+%#llx is not in placed memory: the GPU does not see it", mem, (unsigned long long)size); }
    else if (first) VLOG("memory pool %p+%#llx placed at device address %#llx (%.1f ms)", mem, (unsigned long long)size, (unsigned long long)bda, (mono_ns() - t0) / 1e6);
    if (lg) placed_line(tot);
}
// The GPU is given [addr, addr+len) (why: by what): every pool not placed yet that overlaps it is placed now, whole (its other addresses follow from this one), or, for a
// pool placed by parts, the granules [addr, addr+len) touches. Returns once they are placed, also when another thread is placing one of them
// Ranges a thread asked for with nothing left to place (no pool not placed yet overlaps them, or the parts they touch are placed), while g_lzgen stands (direct-mapped by
// 64 KiB block): the asks that keep missing one placed segment (a buffer past its pool's end, one across two segments, a buffer of a sparse virtual pool) skip the scan of every pool
enum { NNEG = 64, NDONE = 8 };
// [a, e) of pool p (placed by parts) lies in granules all placed already: nothing to place, and no wait for a placement of another part (caller holds g_pmu)
static int parts_placed(const pool_ent *p, uintptr_t a, uintptr_t e) {
    if (!p->gmap) return 0;
    uint64_t g = part_gran(), s = a > p->mem ? a - p->mem : 0, t = (e < p->mem + p->size ? e : p->mem + p->size) - p->mem;
    if (t <= s) return 1;
    for (uint64_t k = s / g; k <= (t - 1) / g; k++) if (!(p->gmap[k >> 6] >> (k & 63) & 1)) return 0;
    return 1;
}
static void parts_mark(pool_ent *p, uintptr_t lo, uintptr_t hi) {   // caller holds g_pmu
    if (!p->gmap || hi <= lo) return;
    uint64_t g = part_gran();
    for (uint64_t k = (lo - p->mem) / g; k <= (hi - 1 - p->mem) / g; k++) p->gmap[k >> 6] |= 1ull << (k & 63);
}
typedef struct { uint64_t gen; uintptr_t a, e; } negent;
static __thread negent t_neg[NNEG];
void mtl_pool_need(const void *addr, uint64_t len, const char *why) {
    if (!atomic_load_explicit(&g_nlazy, memory_order_relaxed)) return;
    uintptr_t a = (uintptr_t)addr, e = a + (len ? len : 1);
    if (e < a) e = UINTPTR_MAX;
    if (vka_addr(a, e - a, NULL, NULL, NULL)) return;   // placed already (the common case: one lookup, cached per thread)
    negent *ng = &t_neg[(a >> 16) & (NNEG - 1)];
    uint64_t gen = atomic_load_explicit(&g_lzgen, memory_order_acquire);
    if (ng->gen == gen && a >= ng->a && e <= ng->e) return;
    uint64_t done[NDONE]; int ndone = 0;   // pools placed by parts this call has placed its part of (each is still not wholly placed: the scan skips it)
    for (;;) {
        pthread_mutex_lock(&g_pmu);
        int k = -1, busy = 0;
        for (int i = 0; i < g_npool; i++) if (g_pool[i].lazy && g_pool[i].mem < e && a < g_pool[i].mem + (g_pool[i].size ? g_pool[i].size : 1)) {
            int skip = 0; for (int j = 0; j < ndone; j++) if (done[j] == g_pool[i].id) skip = 1;
            if (skip || parts_placed(&g_pool[i], a, e)) continue;
            if (g_pool[i].lazy == 2) busy = 1; else { k = i; break; }
        }
        if (k < 0 && busy) { pthread_cond_wait(&g_lzcv, &g_pmu); pthread_mutex_unlock(&g_pmu); continue; }
        if (k < 0) { *ng = (negent){gen, a, e}; pthread_mutex_unlock(&g_pmu); return; }   // (gen read before the scan: a pool added since bumped it)
        if (ndone == NDONE) { pthread_mutex_unlock(&g_pmu); static _Atomic int nr; if (!atomic_fetch_add(&nr, 1)) VRED("the GPU is given %#lx+%#lx across more than %d memory pools placed by parts: the rest is not placed", (unsigned long)a, (unsigned long)(e - a), NDONE); return; }
        pool_ent pe = g_pool[k]; g_pool[k].lazy = 2;
        pthread_mutex_unlock(&g_pmu);
        uint64_t plo = pe.mem, phi = pe.mem + pe.size;
        int whole = !(pe.part && vkp_part(pe.mem, pe.size, a, e, part_gran(), &plo, &phi));
        seglist l = pe.segs; int lg = 0, span = 0; uint64_t tot = 0, t0 = mono_ns(), mine = 0;   // (l: the parts it holds already)
        pthread_mutex_lock(&g_pl);
        int placed = place_pool(plo, phi - plo, &l, &lg, &tot, &mine);
        if (placed && !whole) { uintptr_t sa = a > pe.mem ? a : pe.mem, se = e < pe.mem + pe.size ? e : pe.mem + pe.size; span = !vka_addr(sa, se - sa, NULL, NULL, NULL); }
        pthread_mutex_unlock(&g_pl);
        pthread_mutex_lock(&g_pmu);   // (mtl_pool_del waits while the pool is being placed: it is still in the table)
        uint32_t nparts = 0, nspan = 0;
        for (int i = 0; i < g_npool; i++) if (g_pool[i].id == pe.id) {
            pool_ent *p = &g_pool[i]; p->segs = l;
            if (whole && (placed || !pe.part)) { g_lazy_bytes -= pe.size - (pe.pbytes < pe.size ? pe.pbytes : pe.size); p->lazy = 0; atomic_fetch_sub(&g_nlazy, 1); }
            else {   // a part, or a whole placement of a pool placed by parts that failed: it stays not wholly placed, what was placed counted
                uint64_t m = mine < pe.size - p->pbytes ? mine : pe.size - p->pbytes; p->pbytes += m; g_lazy_bytes -= m; p->lazy = 1;
                if (placed && !whole) parts_mark(p, plo, phi);
            }
            if (mine) p->nparts++;
            if (span) p->nspan++;
            nparts = p->nparts; nspan = span ? p->nspan : 0;
            break;
        }
        if ((!whole || pe.part) && ndone < NDONE) done[ndone++] = pe.id;
        pthread_cond_broadcast(&g_lzcv);
        static int nlog; int first = nlog++ < 8 || (whole && pe.pbytes);
        pthread_mutex_unlock(&g_pmu);
        if (!placed) { static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) VRED("memory pool %#lx+%#llx is not in placed memory: the GPU does not see it", (unsigned long)pe.mem, (unsigned long long)pe.size); }
        else if (whole && pe.pbytes) VLOG("memory pool %#lx+%#llx placed whole after %u parts (%.0f MiB) when the GPU was given %#lx+%#lx (%s; %.1f ms): ranges across the parts' ends lie in two segments",
                                         (unsigned long)pe.mem, (unsigned long long)pe.size, nparts, pe.pbytes / 1048576.0, (unsigned long)a, (unsigned long)(e - a), why, (mono_ns() - t0) / 1e6);
        else if (whole && first) VLOG("memory pool %#lx+%#llx placed when the GPU was given %#lx+%#lx (%s; %.1f ms)", (unsigned long)pe.mem, (unsigned long long)pe.size, (unsigned long)a, (unsigned long)(e - a), why, (mono_ns() - t0) / 1e6);
        else if (!whole && mine && (nparts <= 8 || !(nparts & 63)))
            VLOG("memory pool %#lx+%#llx: part %u, %#llx+%#llx (%.0f MiB), placed when the GPU was given %#lx+%#lx (%s; %.1f ms; %.0f MiB of the pool placed)", (unsigned long)pe.mem, (unsigned long long)pe.size, nparts,
                 (unsigned long long)plo, (unsigned long long)(phi - plo), mine / 1048576.0, (unsigned long)a, (unsigned long)(e - a), why, (mono_ns() - t0) / 1e6, (pe.pbytes + mine) / 1048576.0);
        if (span && nspan == 1) VRED("memory pool %#lx+%#llx (placed by parts): the GPU is given %#lx+%#lx (%s) across two of its parts: lookups over the whole range fail (draws that need it in one piece are skipped)",
                                     (unsigned long)pe.mem, (unsigned long long)pe.size, (unsigned long)a, (unsigned long)(e - a), why);
        if (lg) placed_line(tot);
    }
}
static void lazy_release_note(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_pmu);   // pools not placed yet (DM_VK_PLACE_LAZY) over the range: their storage leaves the guest too
    for (int i = 0; i < g_npool; i++) if (g_pool[i].lazy && g_pool[i].mem < hi && lo < g_pool[i].mem + g_pool[i].size) {
        static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) VRED("guest memory %#lx+%#lx is given back under live pool memory %#lx+%#llx (not placed): its textures read zeros there", (unsigned long)lo, (unsigned long)(hi - lo), (unsigned long)g_pool[i].mem, (unsigned long long)g_pool[i].size);
    }
    pthread_mutex_unlock(&g_pmu);
}
void mtl_pool_del(const void *mem) {
    pool_ent e = {0};
    pthread_mutex_lock(&g_pmu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == (uintptr_t)mem) {
        if (g_pool[i].lazy == 2) { pthread_cond_wait(&g_lzcv, &g_pmu); i = -1; continue; }   // being placed (mtl_pool_need): its segments are given back below once it is
        e = g_pool[i]; g_pool[i] = g_pool[--g_npool];
        if (e.lazy == 1) { atomic_fetch_sub(&g_nlazy, 1); g_lazy_bytes -= e.size - (e.pbytes < e.size ? e.pbytes : e.size); }   // (a pool placed by parts: its parts are in segs)
        break;
    }
    pthread_mutex_unlock(&g_pmu);
    free(e.gmap);
    if (!e.segs.n) { free(e.segs.lo); return; }
    segvec v = {0}; uint64_t t0 = mono_ns();
    pthread_mutex_lock(&g_pl); pthread_rwlock_wrlock(&g_rw); uncover(&e.segs, &v); pthread_rwlock_unlock(&g_rw); pthread_mutex_unlock(&g_pl);
    unplace_all(&v);
    atomic_fetch_add(&g_give_ns, mono_ns() - t0);
}
// The live memory pools (the frame capture's regions, vka_capture.c): at most max of them, the count returned
int vka_pools(uint64_t *lo, uint64_t *size, int max) {
    pthread_mutex_lock(&g_pmu);
    int n = g_npool; for (int i = 0; i < n && i < max; i++) { lo[i] = g_pool[i].mem; size[i] = g_pool[i].size; }
    pthread_mutex_unlock(&g_pmu);
    return n;
}
int vka_pool_bda(uint64_t addr, VkDeviceAddress *bda) { VkDeviceAddress b = 0; if (!vka_addr(addr, 1, NULL, NULL, &b) || !b) return 0; *bda = b; return 1; }
void mtl_alias_drop_pool(const void *pool) { vka_tex_drop_pool(pool); }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b; VkDeviceSize o;
    if (!vka_addr(addr, len, &b, &o, NULL)) { if (off) *off = 0; return 0; }
    if (buf) *buf = (void *)b; if (off) *off = o; return 1;
}
uint64_t vka_mem_placed_nolock(void) { return g_placed_bytes; }
uint64_t vka_mem_placed_bytes(uint64_t *peak) { pthread_rwlock_rdlock(&g_rw); uint64_t n = g_placed_bytes; if (peak) *peak = g_placed_peak; pthread_rwlock_unlock(&g_rw); return n; }

// ---------- The process's memory maps ----------
// Linux lets a process have vm.max_map_count mappings (65,530); past it every mmap, mprotect or munmap that needs one more fails with ENOMEM, in any thread: the alias
// region's MapPhysicalMemory (hle_svc.c: FATAL SVC 0x2c mprotect … errno=12), scudo, the garbage collector. The dirty-page tracker's write protections split placed
// memory, and the GPU driver's mappings (kgsl) never merge again: each split stays until the segment goes. Blocks (nvn_dirty.c, DM_DIRTY_BLOCK_KB) bound the splits
// to a placed pool's bytes / 64 KiB, which is still more than the limit for a game with gigabytes of placed texture pools that it writes over a long session (Mario Kart
// World: 34-36k maps at its title, the limit after about seven minutes of racing). So a thread reads the count every DM_VK_MAPS_PERIOD_S seconds (5), with the pieces of
// each placed segment (vka_mapbudget.h vkmb_line), and
// - makes the tracker coarser as the count climbs (vkmb_block: blocks from 40 % of the limit, 4 times larger at 50, 60 and 70 %): new splits land on fewer boundaries;
// - past DM_VK_MAPS_BUDGET percent (75) makes the CPU map of the segments with the most pieces again in one piece (compact), down to 55 %. That is a swap like a
//   placement's: the tracker is held over the segment (its protected pages released and stamped: their textures upload again), the CPU map is taken down and the
//   same memory mapped placed again at the same address, faulted in before any protection splits it (see prefault), the range's other protections put back. The
//   memory, its buffer and its GPU address stay: the GPU does not see it. Threads that touch the range meanwhile wait in the swap's fault hook. At most every 20 s
//   and DM_VK_MAPS_COMPACT_N segments (8) a round. DM_VK_MAPS_COMPACT=0 turns it off; DM_VK_MAPS=0 the whole thread.
// Open: between the unmap and the placed map (microseconds) a system call that reads or writes the range (not through the host's pre-write hook) fails with EFAULT
// instead of waiting, as during a give-back (unplace)
typedef struct { long total, guest, placed, out_kgsl; } mscan;
static uint64_t *g_ms_lo, *g_ms_hi; static int *g_ms_pc, g_ms_cap;   // the segments as the scan saw them, their pieces (the maps thread's own)
static vkmb_hist g_ms_hist;   // the maps outside the guest block by (name, permissions, size), when DM_VK_MAPS_HIST is on (the maps thread's own)
static int maps_scan(mscan *o, int nseg, uintptr_t glo, uintptr_t ghi, vkmb_hist *hist) {
    memset(o, 0, sizeof *o); if (nseg) memset(g_ms_pc, 0, (size_t)nseg * sizeof *g_ms_pc);
    if (hist) vkmb_hist_reset(hist);
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    static char b[131072]; size_t have = 0; ssize_t k = 0;
    for (;;) {
        if (have == sizeof b) { k = -1; break; }   // a line longer than the buffer: not the maps' text
        if ((k = read(fd, b + have, sizeof b - have)) <= 0) break;
        have += (size_t)k; size_t s = 0;
        for (size_t i = 0; i < have; i++) if (b[i] == '\n') {
            uint64_t a; int kg;
            if (vkmb_line(b + s, i - s, &a, &kg)) {
                o->total++;
                if (a >= glo && a < ghi) { o->guest++; int g = vkmb_seg(g_ms_lo, g_ms_hi, nseg, a); if (g >= 0) { o->placed++; g_ms_pc[g]++; } }
                else { o->out_kgsl += kg; if (hist) vkmb_hist_add(hist, b + s, i - s); }
            }
            s = i + 1;
        }
        memmove(b, b + s, have - s); have -= s;
    }
    close(fd);
    return k == 0;
}
// One segment's CPU map made again in one piece (see above). Caller holds g_pl. 1 = done, 0 = the segment is no longer there as the scan saw it
static int compact(uintptr_t lo, uintptr_t hi) {
    int k = sg_find(lo); if (k < 0 || g_sg[k].lo != lo || g_sg[k].hi != hi) return 0;   // given back (or placed again) since the scan
    VkDeviceMemory mem = g_sg[k].mem; size_t n = hi - lo;
    nvn_dirty_hold((const void *)lo, n);   // (as place: no protection of the tracker's in the range until the swap ends)
    prun pr[MAXRUN]; int npr = prot_runs(lo, hi, pr, MAXRUN), exact = prot_known(pr, &npr, lo, hi);
    swap_begin(lo, hi);
    unmap_placed(mem);   // (the bytes stay in the memory: nothing to copy, and a write before this is not lost)
    sw_hole_check(lo, n, 0, "compacting pool memory");
    int ok = 0; VkResult r = VK_SUCCESS;
    for (int t = 0; t < 2 && !ok; t++) {
        VkMemoryMapPlacedInfoEXT pi = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)lo};
        VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem, .offset = 0, .size = V.placed_range ? n : VK_WHOLE_SIZE};
        void *q = NULL;
        if (!(r = vk.vkMapMemory2KHR(V.dev, &mi, &q)) && q == (void *)lo) ok = 1;
        else if (!r) unmap_placed(mem);
    }
    // The memory was mapped at this address before; without its map the GPU and the CPU would see different bytes (the segment's lookups still name it)
    if (!ok) die("compacting pool memory %#lx+%#zx: the placed map at the same address failed (%d)", (unsigned long)lo, n, r);
    prefault(lo, hi);   // before any protection splits it (see prefault)
    prot_apply(pr, npr);
    swap_end_ok(exact);
    swap_end();
    nvn_dirty_unhold();
    return 1;
}
// The largest groups of maps outside the guest block (vka_mapbudget.h vkmb_hist), from DM_VK_MAPS_HIST percent of vm.max_map_count (40, the share where the dirty
// tracker starts coarsening; 0 off): when the count climbs past it, at each further 10 % step, and every 30 s while it stays over. The count falls and climbs again
// in cycles, so each climb past the share gets its own lines
static void maps_hist_log(const mscan *m, int pct, int step, int hist_pct, uint64_t now) {
    static int armed = 1, last_step = -1; static uint64_t last_ns;
    if (hist_pct <= 0) return;
    if (pct < hist_pct) { armed = 1; last_step = -1; return; }
    if (!armed && step <= last_step && now - last_ns < 30000000000ull) return;
    const vkmb_hist *h = &g_ms_hist; int idx[10], n = vkmb_hist_top(h, idx, 10), groups = 0;
    for (int i = 0; i < VKMB_HCAP; i++) groups += h->e[i].used;
    VLOG("memory maps outside the guest block: %ld of %ld in all (%d %% of the limit), %.0f MiB, in %d groups of (name, permissions, size)%s; the largest by maps (DM_VK_MAPS_HIST):",
         m->total - m->guest, m->total, pct, h->bytes / 1048576.0, groups, h->dropped ? ", some not sorted: the table is full" : "");
    for (int j = 0; j < n; j++) {
        const vkmb_hent *e = &h->e[idx[j]];
        VLOG("  maps outside #%d: %ld maps (%.1f %%), %.1f MiB: %s %s %s", j + 1, e->n, h->lines ? e->n * 100.0 / h->lines : 0.0, e->bytes / 1048576.0,
             e->perms, vkmb_bucket_name[e->bucket], e->name);
    }
    if (h->dropped) VLOG("  maps outside: %ld maps not sorted (more than %d groups)", h->dropped, VKMB_HCAP);
    void vka_pso_builds_log(void); vka_pso_builds_log();   // the compiles running now (the driver's compiler is the suspect for the scudo secondary maps)
    armed = 0; last_step = step; last_ns = now;
}
static void maps_tick(int *line_due) {
    static uint64_t last_compact_ns, last_line_ns; static int last_step = -1, nred_read, nred_cover;
    int mx = 0; plat_maps_count(&mx);   // (vm.max_map_count, read once)
    uint8_t *gb; size_t gs; void svc_guest_range(uint8_t **base, size_t *size); svc_guest_range(&gb, &gs);
    pthread_rwlock_rdlock(&g_rw);
    int nseg = g_nsg;
    if (nseg > g_ms_cap) { int c = nseg + nseg / 2 + 64; g_ms_lo = realloc(g_ms_lo, (size_t)c * 8); g_ms_hi = realloc(g_ms_hi, (size_t)c * 8); g_ms_pc = realloc(g_ms_pc, (size_t)c * sizeof *g_ms_pc); g_ms_cap = c; }
    for (int i = 0; i < nseg; i++) { g_ms_lo[i] = g_sg[i].lo; g_ms_hi[i] = g_sg[i].hi; }
    pthread_rwlock_unlock(&g_rw);
    int hist_pct = VKA_ENV("DM_VK_MAPS_HIST", 40);
    mscan m; uint64_t t0 = mono_ns();
    if (!maps_scan(&m, nseg, (uintptr_t)gb, (uintptr_t)gb + gs, hist_pct > 0 ? &g_ms_hist : NULL) || !m.total) { if (!nred_read++) VRED("memory maps: /proc/self/maps could not be read: the maps budget is off"); return; }
    double scan_ms = (mono_ns() - t0) / 1e6;
    atomic_store(&g_maps_n, (int)m.total); atomic_store(&g_maps_max, mx);
    int pct = (int)(m.total * 100 / mx), step = pct / 10, act = 0;
    size_t want = vkmb_block(m.total, mx, 64 << 10), had = nvn_dirty_block_bytes();
    if (want > had) { nvn_dirty_block_at_least(want); act = 1; }
    long shed = vkmb_shed(m.total, mx, VKA_ENV("DM_VK_MAPS_BUDGET", VKMB_BUDGET_PCT), VKMB_TARGET_PCT);
    int npieced = 0; long inseg = 0; for (int i = 0; i < nseg; i++) if (g_ms_pc[i] > 1) { npieced++; inseg += g_ms_pc[i] - 1; }
    if (shed > 0 && VKA_ENV("DM_VK_MAPS_COMPACT", 1) && mono_ns() - last_compact_ns >= 20000000000ull) {
        int maxn = VKA_ENV("DM_VK_MAPS_COMPACT_N", 8); if (maxn < 1) maxn = 1; if (maxn > 256) maxn = 256;
        int idx[256]; long covered = 0, shed_done = 0; int k = vkmb_pick(g_ms_pc, nseg, shed, idx, maxn, &covered), done = 0;
        uint64_t c0 = mono_ns(), bytes = 0;
        for (int j = 0; j < k; j++) {
            pthread_mutex_lock(&g_pl);
            if (compact((uintptr_t)g_ms_lo[idx[j]], (uintptr_t)g_ms_hi[idx[j]])) { done++; shed_done += g_ms_pc[idx[j]] - 1; bytes += g_ms_hi[idx[j]] - g_ms_lo[idx[j]]; }
            pthread_mutex_unlock(&g_pl);
        }
        last_compact_ns = mono_ns(); act = 1;
        VLOG("memory maps: %ld of %d (%d %%), over the budget: %d placed segments (%.0f MiB) made one piece each again, about %ld maps fewer, %.1f ms (their textures upload again)",
             m.total, mx, pct, done, bytes / 1048576.0, shed_done, (mono_ns() - c0) / 1e6);
        if (covered < shed && nred_cover < 4) { nred_cover++;
            VRED("memory maps: %ld of %d, %ld over the target: placed memory's pieces cover only %ld of them (%ld maps in placed memory, %ld more in the guest block, %ld outside it)",
                 m.total, mx, shed, covered, m.placed, m.guest - m.placed, m.total - m.guest); }
    }
    uint64_t now = mono_ns();
    if (*line_due || act || step != last_step || now - last_line_ns >= 60000000000ull) {
        VLOG("memory maps: %ld of %d (vm.max_map_count, %d %%): placed memory %ld in %d segments (%d split, %ld pieces more than one each), guest block anonymous %ld, outside the guest block %ld (%ld of them kgsl); dirty tracking by %s%zu KiB; read in %.1f ms (DM_VK_MAPS)",
             m.total, mx, pct, m.placed, nseg, npieced, inseg, m.guest - m.placed, m.total - m.guest, m.out_kgsl,
             nvn_dirty_block_bytes() ? "blocks of " : "pages of ", nvn_dirty_block_bytes() ? nvn_dirty_block_bytes() >> 10 : (size_t)getpagesize() >> 10, scan_ms);
        last_line_ns = now; last_step = step; *line_due = 0;
    }
    maps_hist_log(&m, pct, step, hist_pct, now);
}
static void *maps_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-maps");
    int line_due = 1;
    for (;;) {
        int s = VKA_ENV("DM_VK_MAPS_PERIOD_S", 5); if (s < 1) s = 1;
        sleep((unsigned)s);
        maps_tick(&line_due);
    }
    return NULL;
}
static void maps_start_once(void) {
    if (!VKA_ENV("DM_VK_MAPS", 1)) { VLOG("DM_VK_MAPS=0: the memory maps are not counted (no budget for the dirty tracker, no compaction)"); return; }
    pthread_t th; if (pthread_create(&th, NULL, maps_main, NULL)) { VRED("memory maps: the counting thread did not start"); return; }
    pthread_detach(th);
}
static void maps_start(void) { static pthread_once_t o = PTHREAD_ONCE_INIT; pthread_once(&o, maps_start_once); }

// The hook hle_svc.c calls when it gives guest memory back (weak no-op there for the Apple builds; its region and open hooks stay the weak no-ops)
int dm_gfx_guest_release(uintptr_t lo, uintptr_t hi) { return vka_mem_release(lo, hi); }
// A guest code address as module and offset (log lines; vka_draw.c's dispatch census): 0 = in no module
int vka_mod_off(uint64_t addr, const char **name, uint64_t *off) {
    module_t *m = addr ? module_of((const void *)(uintptr_t)addr) : NULL;
    if (!m) return 0;
    *name = m->name; *off = addr - (uint64_t)(uintptr_t)m->base; return 1;
}
#endif
