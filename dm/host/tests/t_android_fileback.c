// From Anka Native (https://github.com/EddizEge/AnkaNative, v0.2.2), Copyright (c) 2026 EddizEge, MIT licence (LICENSE; credited in android/NOTICE.md)
// File-backed guest ranges (dm/gfx/vk_android/vka_fileback.h; vka_mem.c "File-backed pools", DM_MEM_FILEBACK) on a real file in TMPDIR: an anonymous range moves to a
// MAP_SHARED mapping of an unlinked sparse file with its bytes (untouched and all-zero pages stay holes), a writer that faults on the read-only range during the move
// waits in the fault handler and its write lands in the new mapping, a write-protected page of the shared mapping (the dirty tracker's mprotect) faults and is written
// after the handler opens it, a part taken by a placement is cut out and its file blocks punched, and the rest goes back to anonymous memory with its bytes
#define _GNU_SOURCE 1
#include "../../gfx/vk_android/vka_fileback.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
enum { NP = 64, PG = VKFB_PG };
static uint8_t *g_lo;
static _Atomic int g_swap;                 // the move runs: a fault in the range waits (vka_mem.c swap_fault)
static _Atomic uintptr_t g_track;          // a page write-protected by the "dirty tracker": its fault opens it
static _Atomic int g_faults, g_track_faults;
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc; uintptr_t a = (uintptr_t)si->si_addr;
    if (a >= (uintptr_t)g_lo && a < (uintptr_t)g_lo + NP * PG) {
        uintptr_t t = atomic_load(&g_track);
        if (t && a >= t && a < t + PG) { mprotect((void *)t, PG, PROT_READ | PROT_WRITE); atomic_store(&g_track, 0); atomic_fetch_add(&g_track_faults, 1); return; }
        atomic_fetch_add(&g_faults, 1);
        while (atomic_load(&g_swap)) sched_yield();
        return;   // the write runs again on the range as the move left it
    }
    signal(SIGSEGV, SIG_DFL);
}
static uint8_t pat(int page, int i) { return (uint8_t)(page * 31 + i * 7 + 1); }
static void *writer(void *arg) { (void)arg; while (!atomic_load(&g_swap)) sched_yield(); g_lo[5 * PG + 100] = 0xAB; return NULL; }

int main(void) {
    const char *dir = getenv("TMPDIR"); if (!dir || !*dir) dir = "/tmp";
    struct sigaction sa = {0}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    g_lo = mmap(NULL, NP * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(g_lo != MAP_FAILED, "map"); if (g_lo == MAP_FAILED) return 1;
    // pages: data in 0-9 and 20-29 and 40-49, a written page of zeros at 30, the rest never touched
    for (int p = 0; p < NP; p++) if (p < 10 || (p >= 20 && p < 30) || (p >= 40 && p < 50)) for (int i = 0; i < PG; i++) g_lo[p * PG + i] = pat(p, i);
    memset(g_lo + 30 * PG, 0, PG);
    int err = 0, fd = vkfb_open(dir, &err);
    CHECK(fd >= 0, "unlinked file in %s: %s", dir, strerror(err)); if (fd < 0) return 1;
    uint64_t next = 0; int64_t off0 = vkfb_alloc(fd, &next, 3 * PG), off = vkfb_alloc(fd, &next, NP * PG);   // (a piece before it: offsets are the file's)
    CHECK(off0 == 0 && off == 3 * PG && next == (3 + NP) * PG, "alloc %lld %lld", (long long)off0, (long long)off);

    // to the file, with a writer that faults during the move
    pthread_t th; pthread_create(&th, NULL, writer, NULL);
    mprotect(g_lo, NP * PG, PROT_READ);
    atomic_store(&g_swap, 1);
    for (int i = 0; i < 2000 && !atomic_load(&g_faults); i++) usleep(100);   // (the writer is held in the handler)
    vkfb_run run = {(uintptr_t)g_lo, (uintptr_t)g_lo + NP * PG};
    uint64_t copied = 0; int lost = 0;
    int r = vkfb_to_file(fd, (uint64_t)off, (uintptr_t)g_lo, NP * PG, &run, 1, pm, &copied, &lost);
    CHECK(r == 0 && !lost, "to file: %d lost %d", r, lost);
    CHECK(copied == 30 * PG, "copied %llu bytes, want 30 pages (zero and untouched pages skipped)", (unsigned long long)copied);
    mprotect(g_lo, NP * PG, PROT_READ | PROT_WRITE);   // (the moved mapping is read-write already; the caller puts the range's protections back)
    atomic_store(&g_swap, 0);
    pthread_join(th, NULL);
    CHECK(atomic_load(&g_faults) >= 1, "the writer faulted during the move");
    CHECK(g_lo[5 * PG + 100] == 0xAB, "the held write landed in the new mapping");
    int same = 1; for (int p = 0; p < NP; p++) for (int i = 0; i < PG; i++) {
        uint8_t want = p < 10 || (p >= 20 && p < 30) || (p >= 40 && p < 50) ? pat(p, i) : 0; if (p == 5 && i == 100) want = 0xAB;
        if (g_lo[p * PG + i] != want) { same = 0; break; } }
    CHECK(same, "bytes after the move");
    uint8_t b = 0; CHECK(pread(fd, &b, 1, off + 5 * PG + 100) == 1 && b == 0xAB, "the range is the file's: its write is in the file (%#x)", b);
    int64_t disk = vkfb_disk(fd);
    CHECK(disk >= 30 * PG, "file blocks %lld for 30 data pages (holes for the rest, as far as the file system allocates by page: ext4 39 pages, tmpfs with huge pages all)", (long long)disk);
    int64_t disk1 = disk;
    // a later write to a page never touched before goes to the file too (the game filling the pool)
    g_lo[60 * PG] = 0x5A; CHECK(pread(fd, &b, 1, off + 60 * PG) == 1 && b == 0x5A, "a later write is in the file");

    // the dirty tracker's protection on the shared mapping: the write faults, the handler opens the page, the write lands
    mprotect(g_lo + 21 * PG, PG, PROT_READ); atomic_store(&g_track, (uintptr_t)(g_lo + 21 * PG));
    g_lo[21 * PG + 7] = 0x77;
    CHECK(atomic_load(&g_track_faults) == 1 && g_lo[21 * PG + 7] == 0x77, "write-protected page of the shared mapping: faulted %d", atomic_load(&g_track_faults));
    CHECK(pread(fd, &b, 1, off + 21 * PG + 7) == 1 && b == 0x77, "and the write is in the file");

    // a placement over pages 20-29: new memory over the range (here anonymous) with the bytes, then the piece cut out and its file range punched
    vkfb_tab tab = {0}; CHECK(vkfb_tab_add(&tab, (uintptr_t)g_lo, (uintptr_t)g_lo + NP * PG, (uint64_t)off), "table");
    uint8_t *keep = malloc(10 * PG); memcpy(keep, g_lo + 20 * PG, 10 * PG);
    CHECK(mmap(g_lo + 20 * PG, 10 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == g_lo + 20 * PG, "placed stand-in");
    memcpy(g_lo + 20 * PG, keep, 10 * PG); free(keep);
    vkfb_piece cut[4]; int nc = vkfb_cut(&tab, (uintptr_t)g_lo + 20 * PG, (uintptr_t)g_lo + 30 * PG, cut, 4);
    CHECK(nc == 1 && cut[0].off == (uint64_t)off + 20 * PG && cut[0].hi - cut[0].lo == 10 * PG, "cut: %d", nc);
    for (int i = 0; i < nc; i++) CHECK(vkfb_punch(fd, cut[i].off, cut[i].hi - cut[i].lo) == 0, "punch");
    CHECK(tab.n == 2 && tab.p[0].hi == (uintptr_t)g_lo + 20 * PG && tab.p[1].lo == (uintptr_t)g_lo + 30 * PG && tab.p[1].off == (uint64_t)off + 30 * PG, "two pieces left");
    CHECK(vkfb_tab_bytes(&tab) == (NP - 10) * PG, "pieces' bytes");
    disk = vkfb_disk(fd); CHECK(disk >= 21 * PG && disk <= disk1, "file blocks %lld after the punch (20 data pages + page 60; %lld before)", (long long)disk, (long long)disk1);

    // back to anonymous memory, piece by piece (Finalize)
    uint64_t back = 0;
    for (int i = 0; i < tab.n; i++) {
        vkfb_run rr = {tab.p[i].lo, tab.p[i].hi}; uint64_t c = 0; int l = 0;
        int rb = vkfb_to_anon(fd, tab.p[i].off, tab.p[i].lo, tab.p[i].hi - tab.p[i].lo, &rr, 1, &c, &l);
        CHECK(rb == 0 && !l, "to anonymous: %d", rb); back += c;
        CHECK(vkfb_punch(fd, tab.p[i].off, tab.p[i].hi - tab.p[i].lo) == 0, "punch after");
    }
    CHECK(back == 21 * PG, "read back %llu bytes, want 21 pages (the file's data, page 60 among them)", (unsigned long long)back);
    same = 1; for (int p = 0; p < NP; p++) for (int i = 0; i < PG; i++) {
        uint8_t want = p < 10 || (p >= 20 && p < 30) || (p >= 40 && p < 50) ? pat(p, i) : 0;
        if (p == 5 && i == 100) want = 0xAB; if (p == 60 && i == 0) want = 0x5A; if (p == 21 && i == 7) want = 0x77;
        if (g_lo[p * PG + i] != want) { same = 0; printf("  page %d byte %d: %#x want %#x\n", p, i, g_lo[p * PG + i], want); break; } }
    CHECK(same, "bytes after the way back");
    g_lo[5 * PG + 100] = 0x11; CHECK(pread(fd, &b, 1, off + 5 * PG + 100) == 1 && b == 0, "anonymous again: a write no longer reaches the file (%#x)", b);
    disk = vkfb_disk(fd); CHECK(disk >= 0 && disk <= 5 * PG, "file blocks %lld after the way back (the 3-page piece before it stays reserved)", (long long)disk);

    // a move that fails (the target not page aligned): the range keeps its mapping and bytes
    uint8_t *t = vkfb_mmap(NULL, PG, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); int l2 = 0;
    CHECK(vkfb_move(t, (uintptr_t)g_lo + 1, PG, &l2) < 0 && !l2 && g_lo[0] == pat(0, 0), "failed move leaves the range");

    // the table's cut: an edge, the middle, a cut over two pieces, the whole
    vkfb_tab u = {0}; vkfb_tab_add(&u, 0x10000, 0x20000, 0); vkfb_tab_add(&u, 0x30000, 0x40000, 0x100000);
    vkfb_piece o[4];
    CHECK(vkfb_cut(&u, 0x18000, 0x34000, o, 4) == 2 && o[0].lo == 0x18000 && o[0].off == 0x8000 && o[1].lo == 0x30000 && o[1].hi == 0x34000 && o[1].off == 0x100000, "cut over two pieces");
    CHECK(u.n == 2 && u.p[0].hi == 0x18000 && u.p[1].lo == 0x34000 && u.p[1].off == 0x104000, "their rests");
    CHECK(vkfb_cut(&u, 0x12000, 0x13000, o, 4) == 1 && u.n == 3 && u.p[1].lo == 0x13000 && u.p[1].off == 0x3000, "a middle cut splits: n %d", u.n);
    CHECK(vkfb_cut(&u, 0, 0x100000, o, 4) == 3 && u.n == 0, "everything");
    free(u.p); free(tab.p);
    close(fd); if (pm >= 0) close(pm);
    printf("t_android_fileback: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
