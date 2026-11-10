// Save write and save transaction unit test (takes seconds): includes hle_fs.c directly and calls its static implementation; pwrite is replaced by a scriptable fake.
// All expected values are hand-written from the semantics, not computed by calling the implementation under test:
//   (1) pwrite returns 0 (can happen when the disk is full) -> WriteFile returns failure after one call (it used to loop forever here);
//   (2) pwrite writes in pieces (3 bytes first, then the rest) -> success and the file content is complete;
//   (3) transaction: the failure in (1) causes this transaction's Commit to be rejected and the working directory to return to the committed version; the next transaction writes in the working directory, the committed directory is still the old version before Commit and the new version after Commit;
//   (4) DM_TEST_FS_FAIL=write:N:errno makes only the Nth save write fail, with writes before and after proceeding normally; that transaction's Commit is rejected, the committed version is unchanged, and the front end receives one notification;
//      the next transaction commits normally, and the final save = the content of the next transaction;
//   (5) remounting discards uncommitted content;
//   (8) a save folder that cannot clone (ENOTSUP: HFS+, exFAT) mounts read-only and tells the front end; recursive deletes unlink symlinks and never empty their targets.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/clonefile.h>                // declared before the define, so only hle_fs.c's calls go to the fake
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off);
static int fake_clonefile(const char *src, const char *dst, uint32_t flags);
#define pwrite fake_pwrite
#define clonefile fake_clonefile
#include "../hle_fs.c"
#undef pwrite
#undef clonefile
ssize_t pwrite(int, const void *, size_t, off_t);
static int g_clone_enotsup;                   // 1: clonefile fails as on HFS+ and exFAT (ENOTSUP), checked on disk images of both
static int fake_clonefile(const char *src, const char *dst, uint32_t flags) {
    if (g_clone_enotsup) { errno = ENOTSUP; return -1; }
    return clonefile(src, dst, flags);
}

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }   // stubs for log timestamps and the read path (not used by the write path)
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;                                        // front-end notification count (the strong implementation in t_fs_stub.c replaces the weak one in hle_fs.c)
static int g_mode, g_calls;                   // 0 write normally; 1 always return 0; 2 write only 3 bytes the first time
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off) {
    g_calls++;
    if (g_mode == 1) { if (g_calls > 1000) { printf("  ✗ pwrite retried over 1000 times after returning 0 (endless loop)\n"); exit(1); } return 0; }
    if (g_mode == 2 && g_calls == 1 && n > 3) n = 3;
    return pwrite(fd, buf, n, off);
}
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static Result write_save(const char *path, const char *s) {           // open -> set size -> write -> close
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}
static int mnt_find(const char *name) { for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) return 1; return 0; }
static int read_fs(const char *path, char *out, size_t n) { uint64_t h; size_t got = 0; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0; Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0; return !r; }

// ---------- NUP container (nup_image.h, android/NUP.md) ----------
// A .nup written here from SPEC.md's layout, independently of dm/tools/nup_pack.py: four packages (base, two updates, an add-on), raw, empty and
// zstd files, and one extent shared by two files (dedup)
#include "nup_fixture_zst.h"
static void nup_fixture_text(char *out) { for (int i = 0; i < 3000; i++) out += sprintf(out, "line %05d of the compressed file\n", i); }
enum { NUP_ND = 10, NUP_NF = 8 };
static size_t nup_fixture(uint8_t *o) {   // returns the file's size; o has room for 8 KiB
    static const struct { const char *name; int kids[3], nk; } D[NUP_ND] = {
        {"", {1}, 1}, {"romfs", {2, 10 + 0}, 2}, {"data", {10 + 1, 10 + 2, 10 + 3}, 3},          // base: romfs/{data/{a,e,z}, top.txt}
        {"", {4}, 1}, {"romfs", {5}, 1}, {"data", {10 + 4, 10 + 5}, 2},                          // update 65536: romfs/data/{a,z}
        {"", {7}, 1}, {"romfs", {10 + 6}, 1},                                                     // update 131072: romfs/v2.txt
        {"", {9}, 1}, {"romfs", {10 + 7}, 1}};                                                    // add-on 040074a01bf13001: romfs/d.txt
    static const struct { const char *name, *data; int zstd, share; } F[NUP_NF] = {
        {"top.txt", "base-top"}, {"a", "hello"}, {"e", ""}, {"z", NULL, 1}, {"a", "HELLO"}, {"z", NULL, 1, 3}, {"v2.txt", "two"}, {"d.txt", "dlc"}};
    static const struct { uint64_t tid; uint32_t root, romfs, ver; uint8_t kind; } P[4] = {
        {0x040074A01BF12000ull, 0, 1, 0, 0}, {0x040074A01BF12000ull, 3, 4, 65536, 1}, {0x040074A01BF12000ull, 6, 7, 131072, 1}, {0x040074a01bf13001ull, 8, 9, 0, 2}};
    uint8_t toc[4096] = {0}; char pool[256]; size_t pl = 0, no[NUP_ND + NUP_NF];
    for (int i = 0; i < NUP_ND + NUP_NF; i++) { const char *n = i < NUP_ND ? D[i].name : F[i - NUP_ND].name; no[i] = pl; memcpy(pool + pl, n, strlen(n)); pl += strlen(n); }
    size_t o_pk = 0x30, o_dir = o_pk + 4 * 0x40, o_file = o_dir + NUP_ND * 0x18, o_kids = o_file + NUP_NF * 0x50, nkids = 0;
    for (int i = 0; i < NUP_ND; i++) nkids += D[i].nk;
    size_t o_pool = o_kids + 4 * nkids, toc_size = o_pool + pl, blob = (0x80 + toc_size + 15) & ~(size_t)15, at = blob;
    #define W32(b, off, v) do { uint32_t _v = (uint32_t)(v); memcpy((b) + (off), &_v, 4); } while (0)
    #define W64(b, off, v) do { uint64_t _v = (uint64_t)(v); memcpy((b) + (off), &_v, 8); } while (0)
    memcpy(toc, "TOC\x01", 4); W32(toc, 4, 4); W64(toc, 8, NUP_ND); W64(toc, 0x10, NUP_NF); W64(toc, 0x18, o_pool); W64(toc, 0x20, pl);
    for (int i = 0; i < 4; i++) { uint8_t *p = toc + o_pk + 0x40 * i; W64(p, 0, P[i].tid); W32(p, 8, P[i].root); W32(p, 0xC, NUP_NONE); W32(p, 0x10, P[i].romfs); W32(p, 0x14, P[i].ver); p[0x18] = P[i].kind; }
    size_t slot = 0;
    for (int i = 0; i < NUP_ND; i++) { uint8_t *d = toc + o_dir + 0x18 * i; W32(d, 0, no[i]); W32(d, 4, strlen(D[i].name)); W32(d, 8, slot); W32(d, 12, D[i].nk);
        for (int j = 0; j < D[i].nk; j++) W32(toc, o_kids + 4 * slot++, D[i].kids[j]); }
    uint64_t xo[NUP_NF] = {0};
    for (int i = 0; i < NUP_NF; i++) {
        uint8_t *r = toc + o_file + 0x50 * i; size_t size = F[i].zstd ? 102000 : strlen(F[i].data), stored = F[i].zstd ? nup_zst_len : size;
        if (F[i].share) xo[i] = xo[F[i].share];
        else if (stored) { xo[i] = at; memcpy(o + at, F[i].zstd ? (const char *)nup_zst : F[i].data, stored); at = (at + stored + 15) & ~(size_t)15; }
        W32(r, 0, no[NUP_ND + i]); W32(r, 4, strlen(F[i].name)); W64(r, 8, size); W64(r, 0x10, stored ? xo[i] : 0); W64(r, 0x18, stored); r[0x40] = (uint8_t)F[i].zstd;
    }
    memcpy(toc + o_pool, pool, pl);
    size_t fsize = at;   // the last extent's padding is part of the file here (SPEC: gaps are zero padding)
    uint8_t *s = o; memset(s, 0, 0x80); memcpy(s, "NUP\x01", 4); s[4] = 0x80; s[6] = 1; W64(s, 8, 0x80); W64(s, 0x10, toc_size);
    plat_sha256(toc, toc_size, s + 0x18); W64(s, 0x38, blob); W64(s, 0x40, fsize); W32(s, 0x48, 4);
    memcpy(o + 0x80, toc, toc_size);
    #undef W32
    #undef W64
    return fsize;
}
static void nup_write(const char *p, const uint8_t *b, size_t n) { FILE *f = fopen(p, "wb"); fwrite(b, 1, n, f); fclose(f); }
static void nup_tests(const char *root) {
    printf("NUP container\n");
    static uint8_t b[8192]; static char want[102001], got[102001];
    size_t n = nup_fixture(b); nup_fixture_text(want);
    char np[1100], spec[1200]; snprintf(np, sizeof np, "%s/game.nup", root); nup_write(np, b, n);
    unsetenv("DM_ROMFS_OVERLAY"); unsetenv("DM_NUP_CACHE_MB"); g_nmnt = 0;
    setenv("DM_ROMFS", np, 1);
    CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK && g_mnt[0].img, "a .nup with no selector mounts");
    CHECK(read_fs("rom:/v2.txt", got, sizeof got) && !strcmp(got, "two"), "with no selector the RomFS is the newest update's (131072)");
    { uint64_t h; CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_READ) == RES_PATH_NOT_FOUND, "the older update's files are not in it"); }
    g_nmnt = 0;
    snprintf(spec, sizeof spec, "%s:update/65536/romfs", np); setenv("DM_ROMFS", spec, 1);
    CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK && read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "HELLO"), "update/65536/romfs selects that update");
    {   uint64_t h; int64_t sz = 0; size_t r1 = 0, r2 = 0, r3 = 0;
        CHECK(h_fs_OpenFile(&h, "rom:/data/z", MODE_READ) == RES_OK && h_fs_GetFileSize(&sz, h) == RES_OK && sz == 102000, "a zstd file opens with its uncompressed size (%lld)", (long long)sz);
        CHECK(h_fs_ReadFile(&r1, h, 0, got, 40000) == RES_OK && h_fs_ReadFile(&r2, h, 40000, got + 40000, 70000) == RES_OK && r1 == 40000 && r2 == 62000 && !memcmp(got, want, 102000),
              "a zstd file reads in pieces, the last one cut at its end (%zu + %zu)", r1, r2);
        CHECK(h_fs_ReadFile(&r3, h, 102000, got, 10) == RES_OK && r3 == 0, "a read at the end gives 0 bytes");
        nup_aux *a = g_mnt[0].img->aux;
        CHECK(a->nz == 1 && a->z[0].refs == 1 && a->held == 102000, "the open holds the decompressed bytes (refs %u, held %llu)", a->z[0].refs, (unsigned long long)a->held);
        h_fs_CloseFile(h);
        CHECK(a->z[0].refs == 0 && a->z[0].buf, "closed, the bytes stay cached within the budget");
    }
    g_nmnt = 0; setenv("DM_NUP_CACHE_MB", "0", 1);
    snprintf(spec, sizeof spec, "%s:base", np); setenv("DM_ROMFS", spec, 1);
    CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK && read_fs("rom:/top.txt", got, sizeof got) && !strcmp(got, "base-top"), ":base selects the base");
    CHECK(read_fs("rom:/data/e", got, sizeof got) && !got[0], "an empty file reads as empty");
    CHECK(read_fs("rom:/data/z", got, sizeof got) && !memcmp(got, want, 102000), "the file sharing the zstd extent reads the same bytes");
    { nup_aux *a = g_mnt[0].img->aux; CHECK(!a->z[0].buf && !a->held, "with DM_NUP_CACHE_MB=0 nothing stays cached after close"); }
    { int t = -1; CHECK(h_fs_GetEntryType(&t, "rom:/data") == RES_OK && t == ENT_DIR, "a directory is a directory"); }
    unsetenv("DM_NUP_CACHE_MB");
    // a large compressed file (here: DM_NUP_DISK_MB of 0.05 MiB, the 102,000-byte file is "large") is decompressed into a temporary file and mapped: the same bytes,
    // not in the cache's budget, kept after close
    g_nmnt = 0; setenv("DM_NUP_DISK_MB", "0.05", 1); setenv("TMPDIR", root, 1);
    snprintf(spec, sizeof spec, "%s:update/65536/romfs", np); setenv("DM_ROMFS", spec, 1);
    {   uint64_t h; size_t r1 = 0, r2 = 0; memset(got, 0, sizeof got);
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK && h_fs_OpenFile(&h, "rom:/data/z", MODE_READ) == RES_OK, "a large zstd file opens");
        CHECK(h_fs_ReadFile(&r1, h, 0, got, 50000) == RES_OK && h_fs_ReadFile(&r2, h, 50000, got + 50000, 60000) == RES_OK && r1 == 50000 && r2 == 52000 && !memcmp(got, want, 102000),
              "a large zstd file reads its bytes from the temporary file (%zu + %zu)", r1, r2);
        nup_aux *a = g_mnt[0].img->aux;
        CHECK(a->z[0].mapped && a->held == 0, "it is mapped from a temporary file, outside the cache's budget (mapped %d, held %llu)", a->z[0].mapped, (unsigned long long)a->held);
        h_fs_CloseFile(h);
        CHECK(a->z[0].buf && a->ndec == 1, "closed, the map stays: no second decompression at the next open");
        char tl[1200]; snprintf(tl, sizeof tl, "%s/nup-", root); DIR *d = opendir(root); int left = 0; struct dirent *de;
        while (d && (de = readdir(d))) if (!strncmp(de->d_name, "nup-", 4)) left++;
        if (d) closedir(d);
        CHECK(!left, "the temporary file is unlinked (%d left in %s)", left, root); (void)tl;
    }
    g_nmnt = 0; unsetenv("DM_NUP_DISK_MB"); unsetenv("TMPDIR");
    {   char aocd[1100], ln[1200]; size_t cache = 0; snprintf(aocd, sizeof aocd, "%s/aocnup", root); mkdir(aocd, 0755);
        snprintf(ln, sizeof ln, "%s/1.nup", aocd); unlink(ln); CHECK(!symlink(np, ln), "link 1.nup to the .nup"); setenv("DM_AOC_DIR", aocd, 1);
        CHECK(h_fs_QueryAocCache(&cache, 1) == RES_OK && cache > 0x30, "the add-on reports a table size (%zu)", cache);
        CHECK(h_fs_MountAoc("aocn", 1, NULL, 0) == RES_OK && read_fs("aocn:/d.txt", got, sizeof got) && !strcmp(got, "dlc"), "add-on 1 mounts from the addon package whose title ID ends in 001");
        CHECK(h_fs_MountAoc("aocm", 2, NULL, 0) == RES_TARGET_NOT_FOUND, "an index with no link is not found");
        // Forgejo #30: a link to a .nup that has no add-on of that index (the launcher linked the game's own .nup for a pack's aoc row), a link to nothing and a cut-off
        // image are not usable (aoc_load leaves them out with a [RED] line), and mounting one anyway answers "target not found" instead of a FATAL
        CHECK(dm_aoc_usable(1), "add-on 1 in the .nup is usable");
        snprintf(ln, sizeof ln, "%s/2.nup", aocd); unlink(ln); CHECK(!symlink(np, ln), "link 2.nup to the .nup, which has no add-on 2");
        CHECK(!dm_aoc_usable(2), "a .nup without that add-on is not usable");
        CHECK(h_fs_MountAoc("aocx", 2, NULL, 0) == RES_TARGET_NOT_FOUND && !mnt_find("aocx"), "mounting it answers target not found, no FATAL, no mount");
        snprintf(ln, sizeof ln, "%s/3.nup", aocd); unlink(ln); CHECK(!symlink("/nonexistent/Game.nup", ln), "link 3.nup to nothing");
        CHECK(!dm_aoc_usable(3) && h_fs_MountAoc("aocy", 3, NULL, 0) == RES_TARGET_NOT_FOUND, "a link to nothing is not usable, and not found");
        snprintf(ln, sizeof ln, "%s/4.img", aocd); { FILE *f = fopen(ln, "wb"); fwrite("DMROMFS1", 1, 8, f); fclose(f); }
        CHECK(!dm_aoc_usable(4) && h_fs_MountAoc("aocz", 4, NULL, 0) == RES_TARGET_NOT_FOUND, "a cut-off image is not usable, and its mount is not found");
        CHECK(h_fs_MountAoc("aocn", 1, NULL, 0) == RES_OK && read_fs("aocn:/d.txt", got, sizeof got) && !strcmp(got, "dlc"), "the readable add-on still reads");
        for (int i = 2; i <= 4; i++) { snprintf(ln, sizeof ln, "%s/%d.%s", aocd, i, i == 4 ? "img" : "nup"); unlink(ln); }
        unsetenv("DM_AOC_DIR");
    }
    // fd:<n>/<name> in place of the path (a .nup picked with Android's file picker: the launcher's descriptor, nup_image.h nup_fd_spec)
    {   int fd = open(np, O_RDONLY); CHECK(fd >= 0, "open the .nup for its descriptor");
        snprintf(spec, sizeof spec, "fd:%d/Game Name.nup:update/65536/romfs", fd); setenv("DM_ROMFS", spec, 1); g_nmnt = 0;
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK && read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "HELLO"), "fd:<n>/<name>.nup:update/65536/romfs mounts that update");
        CHECK(read_fs("rom:/data/z", got, sizeof got) && !memcmp(got, want, 102000), "a zstd file reads through the descriptor");
        CHECK(lseek(fd, 0, SEEK_CUR) == 0, "the descriptor's own position is not moved (pread of a duplicate)");
        g_nmnt = 0;
        {   char aocd[1100], ln[1200], tgt[64]; snprintf(aocd, sizeof aocd, "%s/aocfd", root); mkdir(aocd, 0755);
            snprintf(ln, sizeof ln, "%s/1.nup", aocd); unlink(ln); snprintf(tgt, sizeof tgt, "fd:%d/Game Name.nup", fd);
            CHECK(!symlink(tgt, ln), "link 1.nup to fd:<n>/<name>"); setenv("DM_AOC_DIR", aocd, 1);
            CHECK(h_fs_MountAoc("aocf", 1, NULL, 0) == RES_OK && read_fs("aocf:/d.txt", got, sizeof got) && !strcmp(got, "dlc"), "add-on 1 mounts through a link to the descriptor");
            unsetenv("DM_AOC_DIR");
        }
        close(fd);
        snprintf(spec, sizeof spec, "fd:%d/Game Name.nup:base", fd); CHECK(!img_open(spec), "a descriptor that is not open is refused");
        g_nmnt = 0;
    }
    snprintf(spec, sizeof spec, "%s:addon/040074a01bf13001", np); { romfs_img *m = img_open(spec); CHECK(m && m->n == 2, "addon/<title ID> selects the add-on"); romfs_image_close(m); }
    snprintf(spec, sizeof spec, "%s:update/1", np); CHECK(!img_open(spec), "an update that is not there is refused");
    snprintf(spec, sizeof spec, "%s:patch", np); CHECK(!img_open(spec), "an unknown selector is refused");
    {   char bp[1100]; snprintf(bp, sizeof bp, "%s/bad.nup", root);
        uint8_t c[8192]; memcpy(c, b, n); c[0x80 + 0x40] ^= 1; nup_write(bp, c, n); CHECK(!img_open(bp), "a changed TOC byte fails its SHA-256");
        nup_write(bp, b, n - 16); CHECK(!img_open(bp), "a cut-off .nup is refused (file size)");
        memcpy(c, b, n); c[0x4C] = 1; nup_write(bp, c, n); CHECK(!img_open(bp), "unknown superblock flags are refused");
    }
    g_nmnt = 0; unsetenv("DM_ROMFS");
}

int main(void) {
    char root[1024], save[1100], work[1200], p[1200];
    const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_fs.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char rom[1100], overlay[1100], rdata[1200], odata[1200], fn[1300], got[64];
    snprintf(rom, sizeof rom, "%s/rom", root); snprintf(overlay, sizeof overlay, "%s/overlay", root); snprintf(rdata, sizeof rdata, "%s/data", rom); snprintf(odata, sizeof odata, "%s/data", overlay);
    mkdir(rom, 0755); mkdir(overlay, 0755); mkdir(rdata, 0755); mkdir(odata, 0755);
    snprintf(fn, sizeof fn, "%s/same", rdata); put(fn, "base"); snprintf(fn, sizeof fn, "%s/same", odata); put(fn, "overlay"); snprintf(fn, sizeof fn, "%s/base", rdata); put(fn, "only-base"); snprintf(fn, sizeof fn, "%s/over", odata); put(fn, "only-overlay");
    setenv("DM_ROMFS", rom, 1); setenv("DM_ROMFS_OVERLAY", overlay, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount RomFS with two layers (base + overlay)");
    CHECK(read_fs("rom:/data/same", got, sizeof got) && !strcmp(got, "overlay"), "the overlay file wins"); CHECK(read_fs("rom:/data/base", got, sizeof got) && !strcmp(got, "only-base"), "falls back to the base when the overlay lacks the file");
    uint64_t rh; CHECK(h_fs_OpenFile(&rh, "rom:/data/same", MODE_WRITE) == RES_FAIL, "the two-layer mount refuses writes");
    CHECK(read_fs("rom:\\data\\over", got, sizeof got) && !strcmp(got, "only-overlay") && read_fs("rom:\\data\\base", got, sizeof got) && !strcmp(got, "only-base"), "backslashes separate as slashes do, in both layers");
    CHECK(h_fs_OpenFile(&rh, "rom:/data/base", MODE_READ) == RES_OK && rh && !(rh & 15), "a folder file's handle is 16-byte aligned, as a Horizon FileAccessor pointer is (%#llx)", (unsigned long long)rh); h_fs_CloseFile(rh);
    g_nmnt = 0;
    unsetenv("DM_ROMFS_OVERLAY");
    {   // game files that are neither a folder nor a file (a stale path, a fifo) stop the rom mount with a FATAL line, in a child so the abort does not end this test
        char gone[1200], fifo[1200], out[1200]; snprintf(gone, sizeof gone, "%s/RomFS", root); snprintf(fifo, sizeof fifo, "%s/fifo", root); snprintf(out, sizeof out, "%s/fatal.txt", root);
        mkfifo(fifo, 0644);
        const char *paths[2] = {gone, fifo};
        for (int i = 0; i < 2; i++) {
            fflush(NULL); pid_t pid = fork();
            if (!pid) { freopen(out, "w", stderr); setenv("DM_ROMFS", paths[i], 1); h_fs_MountRom("rom", NULL, 0); _exit(0); }
            int st = 0; waitpid(pid, &st, 0);
            char line[512] = {0}; FILE *f = fopen(out, "r"); if (f) { fread(line, 1, sizeof line - 1, f); fclose(f); }
            CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strstr(line, "FATAL the game files") && strstr(line, paths[i]), "%s game files stop the rom mount", i ? "a fifo as the" : "missing");
        }
    }

    printf("RomFS image\n");   // written by hand in romfs_image.py's layout: /data/a = "hello", /data/b empty directory, /data/c = "xy"
    {   uint8_t img[32 + 7 + 5 * 24 + 7] = {0}; uint64_t hd[3] = {39, 5 * 24 + 7, 5};
        img_ent e[5] = {{1, 1, 0, 0, 0, 0}, {2, 3, 0, 4, 0, 0}, {32, 5, 4, 1, 1, 0}, {5, 0, 5, 1, 0, 0}, {37, 2, 6, 1, 1, 0}};
        memcpy(img, "DMROMFS1", 8); memcpy(img + 8, hd, sizeof hd); memcpy(img + 32, "helloxy", 7); memcpy(img + 39, e, sizeof e); memcpy(img + 39 + sizeof e, "dataabc", 7);
        char ip[1100]; snprintf(ip, sizeof ip, "%s/rom.img", root); FILE *f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
        setenv("DM_ROMFS", ip, 1); unsetenv("DM_ROMFS_OVERLAY"); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image");
        CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "hello"), "reads a file"); CHECK(read_fs("rom:/data/../data/c", got, sizeof got) && !strcmp(got, "xy"), "reads the next file, through ..");
        uint64_t h; size_t n = 0; int64_t sz = 0; char b[16] = {0};
        CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_READ) == RES_OK && h_fs_ReadFile(&n, h, 3, b, 10) == RES_OK && n == 2 && !memcmp(b, "lo", 2), "a read stops at the end of the file (got %zu)", n);
        CHECK(h_fs_GetFileSize(&sz, h) == RES_OK && sz == 5 && h_fs_GetFileOpenMode(h) == MODE_READ, "size 5, open for reading"); h_fs_CloseFile(h);
        // entry 4: its index has low bits set, which Metroid Prime 4's file device masks off (handle & ~7) as it may of a Horizon pointer
        CHECK(h_fs_OpenFile(&h, "rom:/data/c", MODE_READ) == RES_OK && !(h & 15) && h_fs_ReadFile(&n, h & ~7ull, 0, b, 10) == RES_OK && n == 2 && !memcmp(b, "xy", 2), "entry 4 still reads after its handle's low bits are masked off (%#llx)", (unsigned long long)h);
        CHECK(h_fs_GetFileSize(&sz, h & ~7ull) == RES_OK && sz == 2, "and is 2 bytes"); h_fs_CloseFile(h & ~7ull);
        CHECK(read_fs("rom:\\data\\c", got, sizeof got) && !strcmp(got, "xy") && read_fs("rom:\\data\\..\\data\\a", got, sizeof got) && !strcmp(got, "hello"), "backslash paths, also through ..");
        CHECK(h_fs_OpenFile(&h, "rom:/data/x", MODE_READ) == RES_PATH_NOT_FOUND, "a missing file is not found");
        CHECK(h_fs_OpenFile(&h, "rom:/data/A", MODE_READ) == RES_PATH_NOT_FOUND, "names are case-sensitive");
        CHECK(h_fs_OpenFile(&h, "rom:/data/b", MODE_READ) == RES_PATH_NOT_FOUND, "a directory does not open as a file");
        CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_WRITE) == RES_FAIL && h_fs_CreateFile("rom:/data/a", 1) == RES_PATH_EXISTS, "refuses writes; create answers \"exists\"");
        int32_t t = -1; CHECK(h_fs_GetEntryType(&t, "rom:/data/b") == RES_OK && t == ENT_DIR && h_fs_GetEntryType(&t, "rom:/data/c") == RES_OK && t == ENT_FILE, "entry types");
        DirectoryEntry de[4]; int64_t cnt = 0;
        CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 3, "a directory of 3 entries (%lld)", (long long)cnt);
        CHECK(h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 3 && !strcmp(de[0].name, "a") && de[0].size == 5 && de[1].type == ENT_DIR && !strcmp(de[2].name, "c"), "lists a, b, c"); h_fs_CloseDirectory(h);
        CHECK(h_fs_OpenDirectory(&h, "rom:/", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 0, "the file filter hides directories"); h_fs_CloseDirectory(h);
        CHECK(h_fs_OpenDirectory(&h, "rom:/data/b", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 0, "an empty directory"); h_fs_CloseDirectory(h);
        g_nmnt = 0;
        char ov[1100], ovd[1200]; snprintf(ov, sizeof ov, "%s/imgov", root); snprintf(ovd, sizeof ovd, "%s/data", ov); mkdir(ov, 0755); mkdir(ovd, 0755);
        snprintf(fn, sizeof fn, "%s/a", ovd); put(fn, "mod"); snprintf(fn, sizeof fn, "%s/new", ovd); put(fn, "added");
        setenv("DM_ROMFS_OVERLAY", ov, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image with an overlay folder");
        CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "mod"), "the overlay file wins over the image");
        CHECK(read_fs("rom:/data/c", got, sizeof got) && !strcmp(got, "xy") && read_fs("rom:/data/new", got, sizeof got) && !strcmp(got, "added"), "image files and overlay-only files both read");
        CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 4, "the listing merges both, once each (%lld)", (long long)cnt);
        int seen = 0; for (int i = 0; i < cnt; i++) { if (!strcmp(de[i].name, "a")) seen += de[i].size == 3; if (!strcmp(de[i].name, "b")) seen += de[i].type == ENT_DIR; if (!strcmp(de[i].name, "c")) seen += de[i].size == 2; if (!strcmp(de[i].name, "new")) seen += de[i].size == 5; }
        CHECK(seen == 4, "with the right types and sizes"); h_fs_CloseDirectory(h);
        CHECK(read_fs("rom:\\data\\a", got, sizeof got) && !strcmp(got, "mod") && read_fs("rom:\\data\\c", got, sizeof got) && !strcmp(got, "xy"), "backslash paths reach the overlay folder and the image");
        h = 0; cnt = -1; CHECK(h_fs_OpenDirectory(&h, "rom:\\data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 4, "and list the merged directory (%lld)", (long long)cnt); h_fs_CloseDirectory(h);
        unsetenv("DM_ROMFS_OVERLAY"); g_nmnt = 0;
        snprintf(fn, sizeof fn, "%s/2.img", root); CHECK(!link(ip, fn), "stage a DLC image"); setenv("DM_AOC_DIR", root, 1);
        size_t cache = 0;
        CHECK(h_fs_QueryAocCache(&cache, 2) == RES_OK && cache == 176, "DLC image reports Horizon table size (176, got %zu)", cache);
        CHECK(h_fs_MountAoc("aoc", 2, NULL, 0) == RES_OK && read_fs("aoc:/data/a", got, sizeof got) && !strcmp(got, "hello"), "DLC mounts and reads directly from its image");
        unsetenv("DM_AOC_DIR"); g_nmnt = 0;
        {   // a file the image marks damaged (IMG_ENT_DAMAGED) still reads, and the mount counts it and the first open names it once
            romfs_img *clean = img_open(ip); CHECK(clean && !clean->damaged, "a clean image has no damaged files"); romfs_image_close(clean);
            e[2].flags = IMG_ENT_DAMAGED; memcpy(img + 39, e, sizeof e);
            f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
            CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK && g_mnt[0].img && g_mnt[0].img->damaged == 1, "a damaged file mounts, and is counted");
            CHECK(g_mnt[0].img->e[2].flags == IMG_ENT_DAMAGED, "not named before its first open");
            CHECK(read_fs("rom:/data/c", got, sizeof got) && !strcmp(got, "xy") && g_mnt[0].img->e[4].flags == 0, "opening a clean file names nothing");
            CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "hello") && g_mnt[0].img->e[2].flags == (IMG_ENT_DAMAGED | IMG_ENT_REPORTED), "the damaged file reads as written, and is named");
            CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "hello"), "and reads again");
            g_nmnt = 0;
            e[2].flags = 2; memcpy(img + 39, e, sizeof e); f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
            CHECK(!img_open(ip), "an unknown flag on a file is refused");
            e[2].flags = 0; e[1].flags = IMG_ENT_DAMAGED; memcpy(img + 39, e, sizeof e); f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
            CHECK(!img_open(ip), "a damaged flag on a directory is refused");
            e[1].flags = 0; memcpy(img + 39, e, sizeof e);
        }
        f = fopen(ip, "wb"); fwrite(img, 1, 39 + 50, f); fclose(f);   // cut off inside the index, as a copy stopped partway
        CHECK(!img_open(ip), "a cut-off image is refused");
    }
    nup_tests(root);
    snprintf(save, sizeof save, "%s/save", root); snprintf(work, sizeof work, "%s.work", save);
    mkdir(save, 0755); snprintf(p, sizeof p, "%s/a", save); put(p, "old");
    setenv("DM_SAVE", save, 1); setenv("DM_TEST_FS_FAIL", "write:5:28", 1);   // the 5th save write fails, errno 28 = ENOSPC
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "mount the save");

    // Save write count: (1)=1st (2)=2nd (3)=3rd (4)=4th/5th/6th/7th (5)=8th
    printf("① pwrite returns 0\n");
    g_mode = 1; g_calls = 0;
    CHECK(write_save("save:/a", "xyz") == RES_FAIL, "WriteFile returns failure 0x2A2");
    CHECK(g_calls == 1, "pwrite called only once (actually %d times; it used to retry forever)", g_calls);

    printf("② pwrite writes in pieces\n");
    g_mode = 2; g_calls = 0;
    CHECK(write_save("save:/a", "abcdefg") == RES_OK, "WriteFile succeeds");
    CHECK(g_calls == 2, "pwrite called twice (3 bytes + 4 bytes; actually %d)", g_calls);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "abcdefg"), "the work directory holds the complete abcdefg");
    g_mode = 0;

    printf("③ transaction\n");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 1 && g_lasterr == ENOSPC, "the Commit of the transaction holding ①② is refused, front end notified once errno=ENOSPC (actually %d times errno=%d)", g_nfail, g_lasterr);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "old"), "work directory discarded, back to the committed version old");
    CHECK(write_save("save:/a", "new") == RES_OK, "next transaction: write new");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "old"), "before Commit the committed directory is still old");
    CHECK(h_fs_Commit("save") == RES_OK, "Commit");
    CHECK(same(p, "new"), "after Commit the committed directory is new");
    snprintf(p, sizeof p, "%s.new", save); CHECK(access(p, F_OK) != 0, "the swapped-out old version is deleted (no .new left)");

    printf("④ DM_TEST_FS_FAIL=write:5:28\n");
    CHECK(write_save("save:/b", "4") == RES_PATH_NOT_FOUND, "(b does not exist: open fails, not counted as a write)");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "4") == RES_OK, "4th write succeeds");
    errno = 0; CHECK(write_save("save:/b", "5") == RES_FAIL && errno == 28, "5th write fails errno=28 (actually errno=%d)", errno);
    CHECK(write_save("save:/b", "6") == RES_OK, "6th write succeeds");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 2 && g_lasterr == 28, "that transaction's Commit is refused, front end notified errno=28 (%d times in total)", g_nfail);
    snprintf(p, sizeof p, "%s/b", save); CHECK(access(p, F_OK) != 0, "committed directory unchanged (no b)");
    snprintf(p, sizeof p, "%s/b", work); CHECK(access(p, F_OK) != 0, "work directory discarded (no b)");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "committed directory a is still new");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "7") == RES_OK && h_fs_Commit("save") == RES_OK, "next transaction: write b=7, Commit succeeds");
    snprintf(p, sizeof p, "%s/b", save); CHECK(same(p, "7"), "final committed b = 7 (the next transaction's content)");
    CHECK(g_nfail == 2, "a successful transaction sends no notification (still %d times in total)", g_nfail);

    printf("⑤ remounting discards uncommitted content\n");
    CHECK(write_save("save:/a", "v9") == RES_OK, "write a=v9 without committing");
    g_nmnt = 0;                                                        // simulate a process restart: clear the mount table
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "work directory a is back to the committed version new");
    snprintf(p, sizeof p, "%s/b", work); CHECK(same(p, "7"), "work directory b = the committed 7");

    printf("⑥ failed writes are handled without exiting (DM_TEST_FS_FAILAT)\n");
    unsetenv("DM_TEST_FS_FAIL");
    // 6a Recloning after a refused commit fails: do not exit, the work directory is untouched and the committed version unchanged; the next commit is refused again and the reclone succeeds; the transaction after that commits normally
    setenv("DM_TEST_FS_FAILAT", "reclone:1", 1);
    CHECK(write_save("save:/a", "bad") == RES_OK, "write a=bad");
    save_poison(ENOSPC); int nf0 = g_nfail;   // a save write of this transaction failed
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "this transaction is refused (injected reclone failure), the process is still alive, front end notified once");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "committed directory a is still new");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "bad"), "work directory not deleted (a is still bad)");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 2, "still tainted: the next commit is refused too");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "this time the reclone works: work directory a is back to new");
    snprintf(p, sizeof p, "%s.re", work); CHECK(access(p, F_OK) != 0, "no .re left");
    CHECK(write_save("save:/a", "good") == RES_OK && h_fs_Commit("save") == RES_OK, "next transaction writes a=good, commit succeeds");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "committed a = good");
    // 6b Writing the guest name after a rename fails and the rollback fails too: do not exit; the file is in a transactional save, so this transaction's commit is refused and the committed version is unchanged; after recloning everything works
    setenv("DM_TEST_FS_FAILAT", "guestname:1,renameback:1", 1);
    h_fs_CreateFile("save:/c", 0); CHECK(write_save("save:/c", "c") == RES_OK, "write c");
    CHECK(h_fs_RenameFile("save:/c", "save:/d") != RES_OK, "rename c → d returns failure (both steps injected to fail), the process is still alive");
    nf0 = g_nfail; CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "this transaction's commit is refused");
    snprintf(p, sizeof p, "%s/c", save); CHECK(access(p, F_OK) != 0, "committed directory has no c");
    snprintf(p, sizeof p, "%s/d", save); CHECK(access(p, F_OK) != 0, "committed directory has no d");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "committed a is still good");
    CHECK(write_save("save:/a", "ok2") == RES_OK && h_fs_Commit("save") == RES_OK, "the next transaction commits as usual");
    // 6c Cloning the work directory at mount fails: do not exit; mount the committed directory read-only instead. Reads work, writes fail, commits are no-ops, the committed version is unchanged, and the front end is notified once; the next normal mount works as before
    setenv("DM_TEST_FS_FAILAT", "mountclone:1", 1); g_nmnt = 0; nf0 = g_nfail;
    snprintf(p, sizeof p, "rm -rf '%s'", work); system(p);
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && g_nfail == nf0 + 1, "mount returns success, front end notified once (actually %d times)", g_nfail - nf0);
    CHECK(read_fs("save:/a", got, sizeof got) && !strcmp(got, "ok2"), "the read-only mount reads the committed a=ok2 (read %s)", got);
    CHECK(write_save("save:/a", "zzz") != RES_OK, "writes fail");
    CHECK(h_fs_Commit("save") == RES_OK, "commit is a no-op");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok2"), "committed a is still ok2");
    unsetenv("DM_TEST_FS_FAILAT"); g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && write_save("save:/a", "ok3") == RES_OK && h_fs_Commit("save") == RES_OK, "remount without injection: transactional as usual, writing a=ok3 and committing succeed");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok3"), "committed a = ok3");

    printf("⑦ backslash paths on a save\n");
    {   uint64_t h; DirectoryEntry de[4]; int64_t cnt = 0; int32_t t = -1;
        CHECK(h_fs_CreateDirectory("save:\\dir") == RES_OK && h_fs_CreateFile("save:\\dir\\f", 0) == RES_OK && write_save("save:\\dir\\f", "bs") == RES_OK, "create and write save:\\dir\\f");
        CHECK(read_fs("save:/dir/f", got, sizeof got) && !strcmp(got, "bs"), "it is save:/dir/f");
        CHECK(h_fs_RenameFile("save:\\dir\\f", "save:\\dir\\g") == RES_OK && h_fs_GetEntryType(&t, "save:/dir/g") == RES_OK && t == ENT_FILE, "rename by backslash path");
        h = 0; CHECK(h_fs_OpenDirectory(&h, "save:\\dir", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 1 && !strcmp(de[0].name, "g"), "the directory lists g, not a name with backslashes (%s)", cnt ? de[0].name : "-"); h_fs_CloseDirectory(h);
    }

    printf("⑧ a save folder that cannot clone, and symlinks in recursive deletes\n");
    {   g_nmnt = 0; g_clone_enotsup = 1; int nf1 = g_nfail;
        CHECK(h_fs_MountSave("save", NULL) == RES_OK && g_mnt[0].ro && g_nfail == nf1 + 1, "clonefile ENOTSUP: the committed save mounts read-only and the front end is told once");
        CHECK(read_fs("save:/a", got, sizeof got) && !strcmp(got, "ok3"), "the committed save still reads (a = %s)", got);
        CHECK(write_save("save:/a", "lost") != RES_OK, "a write is refused");
        snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok3"), "committed a is still ok3");
        g_clone_enotsup = 0; g_nmnt = 0;
        CHECK(h_fs_MountSave("save", NULL) == RES_OK && !g_mnt[0].ro, "with cloning back, the save mounts writable");
        char victim[1100], vf[1200], lnk[1200];
        snprintf(victim, sizeof victim, "%s/victim", root); mkdir(victim, 0755); snprintf(vf, sizeof vf, "%s/keep", victim); put(vf, "keep");
        CHECK(h_fs_CreateDirectory("save:/d") == RES_OK, "create save:/d");
        snprintf(lnk, sizeof lnk, "%s/d/lnk", work); CHECK(symlink(victim, lnk) == 0, "a symlink to a directory outside the save inside save:/d");
        CHECK(h_fs_DeleteDirRec("save:/d") == RES_OK && access(lnk, F_OK) && same(vf, "keep"), "DeleteDirectoryRecursively removes the symlink, not its target's files");
        CHECK(h_fs_CreateDirectory("save:/d") == RES_OK && symlink(victim, lnk) == 0, "the same again in a new save:/d");
        CHECK(h_fs_CleanDirRec("save:/d") == RES_OK && access(lnk, F_OK) && same(vf, "keep"), "CleanDirectoryRecursively removes the symlink, not its target's files");
        snprintf(lnk, sizeof lnk, "%s/lnk2", root); symlink(victim, lnk); errno = 0;
        CHECK(rm_tree(lnk) == -1 && errno == ENOTDIR && same(vf, "keep"), "rm_tree of a symlink to a directory refuses it (ENOTDIR) and leaves the target alone");
    }

    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); system(cmd);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
