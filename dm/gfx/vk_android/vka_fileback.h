// From Anka Native (https://github.com/EddizEge/AnkaNative, v0.2.2), Copyright (c) 2026 EddizEge, MIT licence (LICENSE; credited in android/NOTICE.md)
// File-backed guest ranges (vka_mem.c "File-backed pools", DM_MEM_FILEBACK): the moves of one page-aligned range of guest memory between private anonymous pages and a
// MAP_SHARED mapping of an unlinked sparse file, and the file's bookkeeping. Plain C over the system calls, no Vulkan and no locks: vka_mem.c runs them inside its swap
// (the range read-only, writers held in the fault hook and the pre-write hook) and the Linux unit test (dm/host/tests/t_android_fileback.c) runs them the same way.
// Why: a pool the GPU is never given (a shader-code pool, a texture pool the host reads on the CPU) is written once and then read rarely; as anonymous memory under
// pressure it goes to zram, which keeps ~0.6-0.7 of it in RAM (foot/NEXT.md, 1.43:1). As clean file pages the kernel drops it outright and reads it back from flash.
// To the file: map the file's [off, off + n) shared elsewhere, copy the pages of the range that hold data (present or swapped in /proc/self/pagemap, readable, not all
// zero: an untouched page is never written; its block is reserved by vkfb_alloc, unwritten, so a later write cannot meet a full disk), then mremap that mapping over the range in one call (readers see the old pages or the new ones,
// both with the bytes). Back: the same with fresh anonymous pages, copying only the file's data extents (SEEK_DATA / SEEK_HOLE) that are not all zero, then the file's
// range is punched out. Pages of the range that are not readable (PROT_NONE) are not copied: they read zeros after the move, as vka_mem.c's place does with them.
// A failed mremap may have unmapped the range already (MREMAP_FIXED unmaps its target first): the range is then mapped again as anonymous memory and filled from the
// copy, so no byte is lost; the caller is told (*lost_map) and says so
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif
#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

enum { VKFB_PG = 4096 };
typedef struct { uintptr_t lo, hi; } vkfb_run;   // a readable run of the range

// An unlinked file in dir: O_TMPFILE, else a named one unlinked at once. -1 with *err set when neither works
static inline int vkfb_open(const char *dir, int *err) {
    int fd = open(dir, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0) return fd;
    char p[512]; snprintf(p, sizeof p, "%s/dm_fileback_XXXXXX", dir);
    fd = mkostemp(p, O_CLOEXEC);
    if (fd < 0) { *err = errno; return -1; }
    unlink(p);
    return fd;
}
// Room for n bytes at the file's end, its blocks reserved (fallocate: unwritten extents, no data written). A write through a MAP_SHARED mapping into a hole needs a block
// at fault time, and with the disk full that fault is SIGBUS in whichever thread wrote (the copy below, or the game filling its pool later): so the blocks are taken here,
// where a full disk is an error (the pool stays anonymous). *next: the file's size so far. Returns the offset, or -errno
static inline int64_t vkfb_alloc(int fd, uint64_t *next, uint64_t n) {
    uint64_t off = *next, e = off + ((n + VKFB_PG - 1) & ~(uint64_t)(VKFB_PG - 1));
    if (fallocate(fd, 0, (off_t)off, (off_t)(e - off))) { int r = errno; if (ftruncate(fd, (off_t)off)) {} return -r; }   // (a partial reservation given back)
    *next = e; return (int64_t)off;
}
static inline int vkfb_punch(int fd, uint64_t off, uint64_t n) { return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)n) ? -errno : 0; }
static inline int vkfb_zero_page(const void *p) { const uint64_t *w = (const uint64_t *)p; uint64_t o = 0; for (int i = 0; i < VKFB_PG / 8; i++) o |= w[i]; return !o; }
// Pages of [a, a + 4096 k) with data: pagemap bits 63 (present) or 62 (swapped). pm < 0, or a read that fails: every page counts (the zero check still skips empty ones)
static inline void vkfb_touched(int pm, uintptr_t a, size_t k, uint8_t *t) {
    uint64_t e[512];
    for (size_t i = 0; i < k;) {
        size_t m = k - i < 512 ? k - i : 512;
        ssize_t r = pm >= 0 ? pread(pm, e, m * 8, (off_t)((a / VKFB_PG + i) * 8)) : -1;
        for (size_t j = 0; j < m; j++) t[i + j] = r != (ssize_t)(m * 8) || (e[j] >> 62) != 0;
        i += m;
    }
}
// The raw system calls (vka_mem.c defines mmap / mremap over them for its protection snapshot: the swap's own calls are not marked)
static inline void *vkfb_mremap_to(void *from, size_t n, uintptr_t to) { long r = syscall(__NR_mremap, from, n, n, MREMAP_MAYMOVE | MREMAP_FIXED, to); return r == -1 ? MAP_FAILED : (void *)r; }
static inline void *vkfb_mmap(void *a, size_t n, int flags, int fd, uint64_t off) {
    long r = syscall(__NR_mmap, a, n, PROT_READ | PROT_WRITE, flags, fd, (off_t)off); return r == -1 ? MAP_FAILED : (void *)r; }
static inline int vkfb_mapped(uintptr_t lo, size_t n) { uintptr_t a = lo & ~(uintptr_t)(VKFB_PG - 1); return msync((void *)a, n + (lo - a), MS_ASYNC) == 0 || errno != ENOMEM; }   // (ENOMEM: a page of the range is not mapped)
// Moves the mapping t (n bytes) over [lo, lo + n). On a failed mremap that took the range down, anonymous pages with t's bytes go there (*lost_map = 1). 0 or -errno
static inline int vkfb_move(void *t, uintptr_t lo, size_t n, int *lost_map) {
    *lost_map = 0;
    if (vkfb_mremap_to(t, n, lo) != MAP_FAILED) return 0;
    int e = errno;
    if (!vkfb_mapped(lo, n)) {
        *lost_map = 1;
        if (vkfb_mmap((void *)lo, n, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)lo) { int e2 = errno; munmap(t, n); return -e2; }
        memcpy((void *)lo, t, n);
    }
    munmap(t, n);
    return -e;
}
// Anonymous [lo, lo + n) to the file's [off, off + n): runs: the readable runs of the range (the caller's protection read). *copied: bytes written into the file.
// 0 = the range is the file's now; else -errno and the range holds its bytes (anonymous, its old pages unless *lost_map)
static inline int vkfb_to_file(int fd, uint64_t off, uintptr_t lo, size_t n, const vkfb_run *runs, int nruns, int pm, uint64_t *copied, int *lost_map) {
    *copied = 0; *lost_map = 0;
    uint8_t *t = vkfb_mmap(NULL, n, MAP_SHARED, fd, off);
    if (t == MAP_FAILED) return -errno;
    uint8_t tb[512];
    for (int i = 0; i < nruns; i++) {
        uintptr_t a = runs[i].lo < lo ? lo : runs[i].lo, e = runs[i].hi > lo + n ? lo + n : runs[i].hi;
        for (; a < e;) {
            size_t k = (e - a) / VKFB_PG < 512 ? (e - a) / VKFB_PG : 512; if (!k) break;
            vkfb_touched(pm, a, k, tb);
            for (size_t j = 0; j < k; j++) { const uint8_t *s = (const uint8_t *)(a + j * VKFB_PG); if (tb[j] && !vkfb_zero_page(s)) { memcpy(t + (a - lo) + j * VKFB_PG, s, VKFB_PG); *copied += VKFB_PG; } }
            a += k * VKFB_PG;
        }
    }
    return vkfb_move(t, lo, n, lost_map);
}
// The file's [off, off + n), mapped at [lo, lo + n), back to anonymous pages: the data extents' pages that are readable and not all zero. *copied: bytes copied.
// 0 = anonymous now (the file's range is still allocated: the caller punches it); else -errno and the range stays the file's (unless *lost_map: anonymous with the bytes)
static inline int vkfb_to_anon(int fd, uint64_t off, uintptr_t lo, size_t n, const vkfb_run *runs, int nruns, uint64_t *copied, int *lost_map) {
    *copied = 0; *lost_map = 0;
    uint8_t *t = vkfb_mmap(NULL, n, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return -errno;
    for (uint64_t o = off; o < off + n;) {
        off_t d = lseek(fd, (off_t)o, SEEK_DATA);
        if (d < 0 || (uint64_t)d >= off + n) break;   // (ENXIO: no data past o)
        off_t h = lseek(fd, d, SEEK_HOLE); uint64_t he = h < 0 || (uint64_t)h > off + n ? off + n : (uint64_t)h;
        for (uint64_t p = (uint64_t)d & ~(uint64_t)(VKFB_PG - 1); p < he; p += VKFB_PG) {
            uintptr_t a = lo + (p - off); int rd = 0;
            for (int i = 0; i < nruns && !rd; i++) rd = a >= runs[i].lo && a + VKFB_PG <= runs[i].hi;
            if (rd && !vkfb_zero_page((const void *)a)) { memcpy(t + (p - off), (const void *)a, VKFB_PG); *copied += VKFB_PG; }
        }
        o = he;
    }
    return vkfb_move(t, lo, n, lost_map);
}
// Bytes the file holds on disk (its allocated blocks); -1 unknown
static inline int64_t vkfb_disk(int fd) { struct stat s; return fstat(fd, &s) ? -1 : (int64_t)s.st_blocks * 512; }
// The pieces still mapped from the file, kept sorted by lo and disjoint. vkfb_cut takes [lo, hi) out of them (a placement or a release over it), writing the
// file ranges it frees to out (at most max; the count returned, -1 when the table could not grow)
typedef struct { uintptr_t lo, hi; uint64_t off; } vkfb_piece;
typedef struct { vkfb_piece *p; int n, cap; } vkfb_tab;
static inline int vkfb_tab_add(vkfb_tab *t, uintptr_t lo, uintptr_t hi, uint64_t off) {
    if (t->n == t->cap) { int c = t->cap ? 2 * t->cap : 16; void *q = realloc(t->p, (size_t)c * sizeof *t->p); if (!q) return 0; t->p = q; t->cap = c; }
    int i = t->n; while (i > 0 && t->p[i - 1].lo > lo) { t->p[i] = t->p[i - 1]; i--; }
    t->p[i] = (vkfb_piece){lo, hi, off}; t->n++; return 1;
}
static inline int vkfb_cut(vkfb_tab *t, uintptr_t lo, uintptr_t hi, vkfb_piece *out, int max) {
    int k = 0;
    for (int i = 0; i < t->n; i++) {
        vkfb_piece p = t->p[i]; if (p.hi <= lo || p.lo >= hi) continue;
        uintptr_t a = p.lo > lo ? p.lo : lo, b = p.hi < hi ? p.hi : hi;
        if (k < max) out[k] = (vkfb_piece){a, b, p.off + (a - p.lo)}; k++;
        if (p.lo < a && p.hi > b) {   // a hole in the middle: two pieces
            t->p[i].hi = a;
            if (!vkfb_tab_add(t, b, p.hi, p.off + (b - p.lo))) return -1;
        } else if (p.lo < a) t->p[i].hi = a;
        else if (p.hi > b) { t->p[i].lo = b; t->p[i].off = p.off + (b - p.lo); }
        else { memmove(&t->p[i], &t->p[i + 1], (size_t)(t->n - i - 1) * sizeof *t->p); t->n--; i--; }
    }
    return k;
}
static inline uint64_t vkfb_tab_bytes(const vkfb_tab *t) { uint64_t b = 0; for (int i = 0; i < t->n; i++) b += t->p[i].hi - t->p[i].lo; return b; }
