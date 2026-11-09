// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __ANDROID__
#define DIRTY_BLOCK_KB 64
#else
#define DIRTY_BLOCK_KB 0
#endif

// Order of page state and mprotect: the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY.
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, one whose state is PROT is always read-only. Setting read-only used to be "record PROT, then mprotect"; in between, another thread's unprotect-before-write
// (the movie decoder thread writing a frame into texture storage, a disk read into a guest buffer) could set the state back to NONE, then the mprotect landed => the page read-only with state NONE: a game thread writing it got only
// "re-execute" from the fault handler and spun forever; a host system call reading into it saw NONE in prep_write, skipped, and failed with EFAULT (reproduced by t_dirty_race).
// BUSY lasts at most one mprotect (plus a few atomic writes when setting read-only). The wait is capped at BUSY_WAIT_MS and reports red beyond it: in the fault handler it then returns "not our page" so the crash handler reports truthfully
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
// The table grows in blocks of 1,024 slots, up to 262,144; a block is never moved or freed, so the signal handler reads it without locks. A pool past the end goes untracked: its textures count
// as dirty and are re-uploaded at every use. The table was a fixed 256 slots (Zelda has about 300 pools at the title), then a fixed 4,096, and Breath of the Wild reaches 4,006 pools in its first
// shrine. The scans only cover slots in use, so the capacity itself costs nothing
enum { BLK_SHIFT = 10, BLK = 1 << BLK_SHIFT, NBLK = 256, MAX_RANGE = BLK * NBLK };
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; uint8_t *ever; size_t cap; _Atomic int live; _Atomic uint64_t gmax; _Atomic uintptr_t last; } range_t;   // gmax: the newest page generation in the range, so a query can say "nothing written since" without walking; last: that page's index, so it can say "something was" without walking too
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
// Protection changes in blocks of this many bytes, aligned to it, less one (DM_DIRTY_BLOCK_KB; 0 = by the page). Android's GPU memory is mapped by the kernel driver, and such maps never merge
// again once mprotect has split them: page-by-page tracking left a mapping per page ever protected, and Mario Kart World reached the system's limit of 65,530 mappings a process
// (39,726 of them single pages of /kgsl-3d0), after which any allocation in the process fails. In blocks, splits only land on block boundaries. The cost is on the safe side: a write
// stamps the whole block, so a texture beside the written one may upload again, so it starts only under pressure: when DM_DIRTY_BLOCK_AT separate regions have been protected for the first time. Regions, not pages:
// a large texture is many pages and one split, and Donkey Kong Bananza passes 16,384 pages at its title with 4,000 textures. Games that never get there stay page by page, with no extra uploads
static _Atomic uintptr_t g_bmask;
static uintptr_t g_bwant; static uint64_t g_ever, g_block_at;   // the block mask to use; regions first protected (under g_slot_mu), and the count where blocks start
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation, retirement and protection serialize descriptor lifetime; the page-fault path does not touch this mutex
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static _Atomic uint64_t g_mp_fail; static _Atomic int g_mp_errno;
static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = plat_abs_time(); if (mprotect((void *)a, len, prot)) { atomic_fetch_add(&g_mp_fail, 1); atomic_store(&g_mp_errno, errno); }
    atomic_fetch_add(&g_mp_ticks, plat_abs_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static plat_timebase_t g_tb;   // obtained when the first pool is registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = plat_abs_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("[RED] dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not finished), no longer waiting", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a + len <= r->end) return r; }
    return NULL;
}
// Pages p..q widened to their blocks, within the range
static void block_bounds(const range_t *r, uintptr_t *p, uintptr_t *q) {
    uintptr_t a = (r->base + (*p << g_shift)) & ~g_bmask, b = (r->base + (*q << g_shift)) | g_bmask;
    if (a < r->base) a = r->base;
    if (b >= r->end) b = r->end - 1;
    *p = (a - r->base) >> g_shift; *q = (b - r->base) >> g_shift;
}
static void stamp_page(range_t *r, uintptr_t p) {
    { uint64_t g = atomic_fetch_add(&g_now, 1) + 1; atomic_store(&r->gen[p], g);
      uint64_t cur = atomic_load(&r->gmax);   // (a max, not a store: two writers may reach the two stores out of order)
      while (g > cur && !atomic_compare_exchange_weak(&r->gmax, &cur, g)) {}
      atomic_store_explicit(&r->last, p, memory_order_relaxed); }   // the newest write's page, as the query's hint (a stale read only costs the walk, never an answer)
    // Publish completion after the generation: a checker may have cached "clean" using the incremented clock before the generation store. Both fault and host-write paths
    // must invalidate that cached clock, as the page stays writable until the next upload protects it again.
    atomic_fetch_add(&g_now, 1);
}
static void unprotect_page(range_t *r, uintptr_t p) {   // The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    stamp_page(r, p);
    atomic_store(&r->state[p], PG_NONE);
}
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page is managed by this module; 0 = it is not, or the wait on BUSY timed out
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = PG_PROT;
        if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); if (count) atomic_fetch_add(count, 1); return 1; }
        if (st != PG_BUSY) return atomic_load(&r->gen[p]) != 0;   // already NONE: just unprotected by another thread (the fault happened before the unprotect) -> re-execute; pages never protected are not ours
        if (!busy_wait(r, p)) return 0;
    }
}
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count);
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr; int ours = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)   // Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end) {
            uintptr_t p = (a - r->base) >> g_shift, q = p; int mine = release_page(r, p, &g_faults); ours |= mine;
            if (mine && g_bmask) { block_bounds(r, &p, &q); release_pages(r, p, q, &g_faults); } } }
    return ours;
}
// Unprotect pages p..q of a range with one mprotect per run of protected pages (a host write over a whole texture, e.g. WriteTexels, used to make one system call per 16 KB page:
// Link's Awakening, about 275 a frame). Pages another thread is changing are handled one by one after the run so far is finished, so no BUSY page is held while waiting
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count) {
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_PROT; int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took) { if (!open) { run = i; open = 1; } continue; }
        if (open) {
            mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE);
            for (uintptr_t k = run; k < i; k++) { stamp_page(r, k); atomic_store(&r->state[k], PG_NONE); }
            if (count) atomic_fetch_add(count, i - run);
            open = 0;
        }
        if (i <= q && st == PG_BUSY) release_page(r, i, count);   // another thread is changing it: wait, then unprotect it if it ended up protected
    }
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len; int n = atomic_load(&g_nr);
    if (!len) return;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        uintptr_t p = (lo - r->base) >> g_shift, q = (hi - 1 - r->base) >> g_shift;
        if (g_bmask) block_bounds(r, &p, &q);
        release_pages(r, p, q, &g_preps);
    }
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); plat_timebase(&g_tb);
        { const char *e = getenv("DM_DIRTY_BLOCK_KB"); size_t kb = e ? strtoul(e, NULL, 0) : DIRTY_BLOCK_KB; size_t b = kb << 10;   // a power of two of at least a page, else by the page
          g_bwant = b > g_pg && !(b & (b - 1)) ? b - 1 : 0;
          e = getenv("DM_DIRTY_BLOCK_AT"); g_block_at = e ? strtoull(e, NULL, 0) : 8192;
          if (!g_block_at) g_bmask = g_bwant; } dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!live_at(i)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[RED] dirty-page tracking ranges are full (%d), pool %p not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
        if (!atomic_load_explicit(&g_blk[n >> BLK_SHIFT], memory_order_acquire)) {   // published before g_nr counts the slot (the scans read g_nr first)
            range_t *b = calloc(BLK, sizeof *b); if (!b) die("dirty-page tracking range table allocation failed (block %d)", n >> BLK_SHIFT);
            atomic_store_explicit(&g_blk[n >> BLK_SHIFT], b, memory_order_release);
        }
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = rg(slot);
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    uint8_t *ever = r->ever;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); ever = calloc(np, 1);
        if (!state || !gen || !ever) die("dirty-page table allocation failed (%zu pages)", np);
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset(ever, 0, np);
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen; r->ever = ever; atomic_store(&r->gmax, 0); atomic_store(&r->last, ~(uintptr_t)0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);   // Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// The tracked range holding [addr, addr+len), as a handle a caller keeps (packaged textures: the scan over every range ran twice per texture lookup). NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    // Keep this descriptor live through the last mprotect. Retirement restores
    // RW under this same mutex; it must not finish between lookup and PG_BUSY.
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) { pthread_mutex_unlock(&g_slot_mu); return; }
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    if (g_bmask) block_bounds(r, &p, &q);
    // Consecutive pages are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is changing.
    // A BUSY page is waited out (the run so far is protected first, so none of ours is BUSY meanwhile) and looked at again: an unprotect in progress leaves the page writable
    // with a generation the caller's stamp may cover, so skipped, its later writes never faulted and the texture stayed stale (MacNativeGames a166602)
    uintptr_t run = 0; int open = 0, fresh = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !r->ever[i]) { r->ever[i] = 1; if (!fresh) { fresh = 1; if (++g_ever == g_block_at && g_bwant && g_bmask != g_bwant) { g_bmask = g_bwant; LOG("dirty-page tracking: %llu regions protected so far, from now on in blocks of %zu KiB", (unsigned long long)g_ever, (size_t)((g_bwant + 1) >> 10)); } } }
        else fresh = 0;
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT); open = 0; }
        if (!took && i <= q && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
    pthread_mutex_unlock(&g_slot_mu);
}
// A swap of guest pages under the tracker (the Vulkan replay placing a pool whose textures it already tracks, vka_mem.c place): the swap reads the range's protections,
// copies the pages and puts the protections back on the new ones, so a page this module protected or unprotected meanwhile would come out with the wrong protection for
// its state (read-only and NONE: a writer spins forever; writable and PROT: its writes are missed). So the swap holds the tracker: every protected page of the range is
// released first, as a host write would (stamped: its textures upload again), and g_slot_mu stays held, so nvn_dirty_protect waits until the swap ends. With no page of
// the range protected, the fault and pre-write paths change nothing there. The holder must not wait for anything that waits on nvn_dirty_protect's callers
// Coarser blocks from now on (the Vulkan replay's memory maps budget, vka_mem.c maps_tick): a power of two of at least a page; the block size only grows. Blocks start at
// once whatever DM_DIRTY_BLOCK_AT says. A boundary of the coarser block is also one of every finer block, so no split made before lies where the new blocks would not
// have made one: from here on new splits land only on the coarse boundaries. Pages already protected stay as they are; the next protection or release of a block takes
// the whole coarse block. Returns the block size in force (0 = by the page: the tracker has not started)
size_t nvn_dirty_block_at_least(size_t bytes) {
    pthread_mutex_lock(&g_slot_mu);
    size_t cur = g_bmask ? (size_t)g_bmask + 1 : 0;
    if (g_pg && bytes > cur && bytes > g_pg && !(bytes & (bytes - 1))) {
        g_bwant = bytes - 1; atomic_store(&g_bmask, (uintptr_t)(bytes - 1)); cur = bytes;
        LOG("dirty-page tracking: in blocks of %zu KiB from now on (%llu regions protected so far)", bytes >> 10, (unsigned long long)g_ever);
    }
    pthread_mutex_unlock(&g_slot_mu);
    return cur;
}
size_t nvn_dirty_block_bytes(void) { uintptr_t m = atomic_load(&g_bmask); return m ? (size_t)m + 1 : 0; }
void nvn_dirty_hold(const void *addr, size_t len) { pthread_mutex_lock(&g_slot_mu); if (g_pg) prep_write((void *)addr, len); }
void nvn_dirty_unhold(void) { pthread_mutex_unlock(&g_slot_mu); }
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a0 = (uintptr_t)addr;   // the kept handle still holds the span (a slot reused by another pool over the same memory is as good): no scan
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a0 < r->base || a0 + len > r->end) r = find_span(a0, len);
    if (!r) return 1;   // When untracked, conservatively treat as dirty: better to upload more than to show stale content
    if (atomic_load_explicit(&r->gmax, memory_order_acquire) <= stamp) return 0;   // (no page of it written since: the walk below was 65k loads for an 8192² image)
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
// The written part of [addr, addr+len) since stamp, by pages: [*lo, *hi) as offsets from addr (clipped to the span). 0 = nothing written; untracked = all of it
int nvn_dirty_bounds_since(const void *span, const void *addr, size_t len, uint64_t stamp, size_t *lo, size_t *hi) {
    *lo = 0; *hi = len;
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a = (uintptr_t)addr;
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a < r->base || a + len > r->end) r = find_span(a, len);
    if (!r) return 1;
    if (atomic_load_explicit(&r->gmax, memory_order_acquire) <= stamp) return 0;   // (nothing in it written since: see nvn_dirty_since_span)
    uintptr_t p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift, f = UINTPTR_MAX, l = 0;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) { if (f == UINTPTR_MAX) f = i; l = i; }
    if (f == UINTPTR_MAX) return 0;
    uintptr_t b0 = r->base + (f << g_shift), b1 = r->base + ((l + 1) << g_shift);
    *lo = b0 > a ? b0 - a : 0; *hi = b1 - a < len ? b1 - a : len;
    return 1;
}
void nvn_dirty_debug(const void *span, const void *addr, size_t len, uint64_t stamp, char *out, size_t n) {   // diagnostics (DM_VK_NANSCAN): the kept range, the range now, page states and generations
    range_t *k = (range_t *)span, *r = find_span((uintptr_t)addr, len);
    int live = k && atomic_load(&k->live), np = 0, nn = 0, nb = 0, nl = 0; uint64_t mg = 0;
    if (r) { uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
        for (uintptr_t i = p; i <= q; i++) { uint8_t st = atomic_load(&r->state[i]); np += st == PG_PROT; nn += st == PG_NONE; nb += st == PG_BUSY; uint64_t g = atomic_load(&r->gen[i]); if (g > mg) mg = g; nl++; } }
    size_t o0 = (size_t)snprintf(out, n, "mprotect failures %llu (errno %d); ", (unsigned long long)atomic_load(&g_mp_fail), atomic_load(&g_mp_errno)); out += o0; n -= o0;
    snprintf(out, n, "kept range %p (%s, %#lx-%#lx), range now %p (%#lx-%#lx, base mem %#lx); %d pages: %d protected, %d writable, %d busy; newest page generation %llu, stamp %llu, clock %llu",
             span, live ? "live" : "dead", k ? (unsigned long)k->base : 0ul, k ? (unsigned long)k->end : 0ul, (void *)r, r ? (unsigned long)r->base : 0ul, r ? (unsigned long)r->end : 0ul, r ? (unsigned long)r->mem : 0ul,
             nl, np, nn, nb, (unsigned long long)mg, (unsigned long long)stamp, (unsigned long long)atomic_load(&g_now));
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += live_at(i) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static plat_timebase_t tb; if (!tb.denom) plat_timebase(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
