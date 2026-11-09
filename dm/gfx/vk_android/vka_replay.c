// The frame replayer on the phone: replays a frame captured with DM_NVN_CAPTURE on Android (vka_capture.c) against this build's Vulkan replay, without the game, and compares the
// presented frame with the live one. It is dm/gfx/tests/nvn_replay.m (the Mac's) on the Vulkan replay: the same chunk walk and record relocation (nvn_capture.c), the replay's own
// mtl_* entry points (vka_*.c) and hle_nvn.c's side tables through two setters. It lives in libnxhost.so (nxhost_replay); android/host/nxreplay.c is the executable that runs it
// from adb shell (android/TESTING.md "Frame capture and replay"):
//   nxreplay <capture> [--warm n] [--repeat n] [--max-pixels n] [--png path | --no-png] [--list] [--dump serial,...] [--dump-rt] [--dump-gpu] [--out dir] [--no-flip] [--sync]
// --warm n: n replays in one process, each after the one before (pipelines and images made by the first are kept: the steady-state time); --repeat n: n fresh processes.
// Each replay prints the submissions' CPU time (recording: mtl_sub_begin to vkQueueSubmit), the frame's GPU time (timestamps), the wall time to the last fence and the draws.
// --list: the textures the frame's records name, with their images; --dump: PNGs of those serials' level 0; --dump-rt: of every colour image the GPU wrote among them;
// --dump-gpu: of every colour image the GPU wrote, named by the records or not (intermediate targets: a scene before its upscaler).
// Exit status: 0 the frame equals the live one (within --max-pixels), 1 it differs, 2 the capture cannot be replayed by this build ([RED] line)
#if defined(__ANDROID__)
#include "vka_capture.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_dirty.h"
#include "../nvn/nvn_hmap.h"
void nvn_pool_note(const void *pool);   // hle_nvn.c: the replay reads pools only through the live pool table
#include "../nvn/dmsh_chain.h"
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
void crash_install(void);
#define RFAIL(...) do { fprintf(stdout, "[RED] nxreplay: " __VA_ARGS__); fputc('\n', stdout); fflush(stdout); _exit(2); } while (0)

typedef struct { cap_chunk c; uint8_t *d; } chunk;
static chunk *rp_ch; static size_t rp_nch;
typedef struct { uint64_t old_lo, size; uint32_t kind; uint8_t *mem; const uint8_t *orig; } region;
static region *rp_rg; static uint32_t rp_nrg;
static pool_t *rp_po; static uint32_t rp_npo;
static tex_t *rp_to; static uint32_t rp_nto;
static prog_t *rp_pr; static cap_prog *rp_prc; static uint32_t rp_npr;
static dpool_t *rp_dp; static uint32_t rp_ndp;
static sync_t *rp_sy; static uint32_t rp_nsy;
static uint8_t **rp_seg; static uint32_t rp_nseg;
static uint64_t rp_bad;

static int dec_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable;
    uint64_t v = *field; if (!cap_is_addr(v)) return 1;
    uint32_t r = cap_addr_region(v); uint64_t off = cap_addr_off(v);
    if (r >= rp_nrg || !rp_rg[r].mem || off > rp_rg[r].size || (len && off + len > rp_rg[r].size && rp_rg[r].kind)) {
        if (rp_bad++ < 4) printf("[RED] nxreplay: region %u offset %#llx length %#llx: %s\n", r, (unsigned long long)off, (unsigned long long)len, r >= rp_nrg || !rp_rg[r].mem ? "no such region in the capture" : "past the region's end");
        *field = 0; return 0; }
    *field = (uint64_t)(uintptr_t)rp_rg[r].mem + off; return 1;
}
static int dec_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops rp_dec = {dec_addr, dec_obj};
static int dec_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *field; if (!v) return 1;
    int k = cap_obj_kind(v); uint64_t i = cap_obj_index(v);
    if (k != kind && !(kind == CAP_K_TEX && k == CAP_K_TEXIN)) { rp_bad++; *field = 0; return 0; }
    void *p = NULL;
    switch (k) {
        case CAP_K_TEX: p = i < rp_nto ? &rp_to[i] : NULL; break;
        case CAP_K_TEXIN: if (payload && i + sizeof(tex_t) <= size) { p = payload + i; cap_tex_fields(p, &rp_dec, NULL); } break;
        case CAP_K_PROG: p = i < rp_npr ? &rp_pr[i] : NULL; break;
        case CAP_K_DPOOL: p = i < rp_ndp ? &rp_dp[i] : NULL; break;
        case CAP_K_SYNC: p = i < rp_nsy ? &rp_sy[i] : NULL; break;
        case CAP_K_POOL: p = i < rp_npo ? &rp_po[i] : NULL; break;
        case CAP_K_SEG: p = i < rp_nseg ? rp_seg[i] : NULL; break;
    }
    if (!p) { if (rp_bad++ < 4) printf("[RED] nxreplay: object kind %d index %llu not in the capture\n", k, (unsigned long long)i); *field = 0; return 0; }
    *field = (uint64_t)(uintptr_t)p; return 1;
}

static const char *head_get(const char *head, size_t n, const char *key, char *out, size_t on) {
    size_t kl = strlen(key);
    for (size_t i = 0; i < n;) {
        const char *e = memchr(head + i, '\n', n - i); size_t ln = e ? (size_t)(e - head - i) : n - i;
        if (ln > kl && !memcmp(head + i, key, kl) && head[i + kl] == '\t') { size_t vl = MIN(ln - kl - 1, on - 1); memcpy(out, head + i + kl + 1, vl); out[vl] = 0; return out; }
        i += ln + 1;
    }
    return NULL;
}
// The live run's DM_* settings, so the replay takes the same paths; a setting given to the replayer wins. The app's own folders, input and watchdogs stay out
static void apply_env(const char *head, size_t n) {
    static const char *const skip[] = {"DM_NVN_SHOT", "DM_NVN_TSV", "DM_KEYS", "DM_CTL", "DM_WINDOW", "DM_TEMP", "DM_SAVE", "DM_ROOT", "DM_ROMFS", "DM_AOC_DIR", "DM_WATCH",
                                       "DM_VK_PCACHE", "DM_VK_SHLIB", "DM_BUILD_COMMIT", "DM_REC", "DM_OVERLAY", "DM_VK_FG", "DM_LSFG"};
    for (size_t i = 0; i < n;) {
        const char *e = memchr(head + i, '\n', n - i); size_t ln = e ? (size_t)(e - head - i) : n - i;
        if (ln > 4 && !memcmp(head + i, "env\t", 4)) {
            char kv[4096]; size_t l = MIN(ln - 4, sizeof kv - 1); memcpy(kv, head + i + 4, l); kv[l] = 0;
            char *eq = strchr(kv, '='); if (eq) { *eq = 0; int sk = 0; for (unsigned k = 0; k < sizeof skip / sizeof *skip; k++) sk |= !strncmp(kv, skip[k], strlen(skip[k]));
                if (!sk) setenv(kv, eq + 1, 0); }
        }
        i += ln + 1;
    }
}

// A region goes at the live run's own address when that is free here (the guest block's range, 0x4000000000 and up, is free in the replayer's process): the game keeps GPU addresses
// inside pool memory (pointers to buffers its shaders follow: docs/reference/shaders.md, global memory at an address loaded from memory), which a region moved elsewhere would leave
// pointing at nothing; at a fresh address those reads go astray (the first replays of DKB's tunnel drew NaN blobs and black walls). Otherwise a fresh mapping at the same offset
// within 64 KiB as the live one
static uint64_t rp_moved;
static void map_region(uint32_t idx, const cap_regn *h, const uint8_t *bytes) {
    if (idx >= rp_nrg) { rp_rg = realloc(rp_rg, (idx + 1) * sizeof *rp_rg); memset(rp_rg + rp_nrg, 0, (idx + 1 - rp_nrg) * sizeof *rp_rg); rp_nrg = idx + 1; }
    uintptr_t pg = 4096, lo = (uintptr_t)h->old_lo & ~(pg - 1), hi = ((uintptr_t)h->old_lo + (uintptr_t)h->size + pg - 1) & ~(pg - 1);
    uint8_t *base = NULL;
    if (lo) {
        uint8_t *m = mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m == (uint8_t *)lo) base = (uint8_t *)(uintptr_t)h->old_lo;
        else if (m != MAP_FAILED) munmap(m, hi - lo);
        else {   // a page shared with a region placed earlier (a record's small read beside a pool): the pages not mapped yet, if every other one is a region's
            int ok = 1;
            for (uintptr_t a = lo; a < hi && ok; a += pg) {
                uint8_t *q = mmap((void *)a, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (q == (uint8_t *)a) continue;
                if (q != MAP_FAILED) { munmap(q, pg); ok = 0; break; }
                ok = 0; for (uint32_t r = 0; r < rp_nrg && !ok; r++) if (rp_rg[r].mem && (uintptr_t)rp_rg[r].mem == (uintptr_t)rp_rg[r].old_lo && a + pg > (uintptr_t)rp_rg[r].old_lo && a < (uintptr_t)rp_rg[r].old_lo + rp_rg[r].size) ok = 1;
            }
            if (ok) base = (uint8_t *)(uintptr_t)h->old_lo;
        }
    }
    if (!base) {   // taken here (or the region overlaps one mapped already): moved
        size_t lead = (size_t)(h->old_lo & 0xffff), len = (size_t)h->size + lead + 0x30000;
        uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) RFAIL("cannot map region %u (%llu bytes)", idx, (unsigned long long)h->size);
        base = (uint8_t *)(((uintptr_t)m + 0xffff) & ~(uintptr_t)0xffff) + lead;
        if (rp_moved++ < 4) printf("[RED] nxreplay: region %u (%#llx, %llu bytes) is not at its live address: GPU addresses the game keeps in pool memory do not follow it\n", idx, (unsigned long long)h->old_lo, (unsigned long long)h->size);
    }
    memcpy(base, bytes, (size_t)h->size);
    rp_rg[idx] = (region){h->old_lo, h->size, h->kind, base, bytes};
}
static int rp_pools_added;
static nvn_hmap g_seen_pool = {.klen = 8};
static void add_pools(const uint8_t *d, size_t n) {   // the replay's pool list, as hle_nvn.c registers a pool (MemoryPoolInitialize)
    for (size_t o = 0; o + sizeof(cap_padd) <= n; o += sizeof(cap_padd)) {
        cap_padd a; memcpy(&a, d + o, sizeof a);
        if (!dec_addr(NULL, &a.mem, 0, 0) || !a.mem) RFAIL("a pool lies in no captured region");
        if (nvn_hm_get(&g_seen_pool, &a.mem)) continue; nvn_hm_put(&g_seen_pool, &a.mem, (void *)1);
        mtl_pool_add((void *)(uintptr_t)a.mem, a.size); nvn_dirty_add_range((void *)(uintptr_t)a.mem, a.size); rp_pools_added++;
    }
}
// A TPIX chunk: the image made as the replay makes it (vka_tex_get), then its levels and slices copied in from the captured bytes
static uint64_t g_fill_n, g_fill_bytes;
static void fill_tex(const uint8_t *d, size_t n) {
    cap_tpix h; if (n < sizeof h) RFAIL("short texture chunk");
    memcpy(&h, d, sizeof h); cap_tex_fields(&h.t, &rp_dec, NULL);
    const uint8_t *cs = d + sizeof h, *px = cs + (size_t)h.nsub * sizeof(cap_tsub);
    VkCommandBuffer cb = vcap_cb_begin(); if (!cb) RFAIL("no command buffer");
    vka_img *im = vka_tex_get(&h.t, cb);
    if (!im) { vcap_cb_run(cb); RFAIL("texture ser=%llu %ux%u: the replay made no image for it", (unsigned long long)h.t.serial, h.width, h.height); }
    cap_tsub *subs; uint64_t total; uint32_t ns = vcap_subs(im, &subs, &total);
    // Another format of the same texel size: live, the texture shared an image another texture of that memory made first (vka_tex.c same_shape, learned view formats);
    // here the captured textures are made in another order. The bytes are the same texels (each subresource's size is checked below), so they fill its own image
    if ((uint32_t)im->f.vk != h.pixfmt) { static int n; if (n++ < 4) VLOG("nxreplay: texture ser=%llu captured as VkFormat %u, made as VkFormat %d: filled with the same bytes", (unsigned long long)h.t.serial, h.pixfmt, im->f.vk); }
    if (im->w != h.width || im->h != h.height || im->levels != h.levels || im->layers != h.slices || ns != h.nsub) {
        vcap_cb_run(cb);
        RFAIL("texture ser=%llu: captured as %ux%u VkFormat %u, %u levels, %u layers; this build makes it %ux%u VkFormat %d, %u levels, %u layers (the replay's image creation changed)",
              (unsigned long long)h.t.serial, h.width, h.height, h.pixfmt, h.levels, h.slices, im->w, im->h, im->f.vk, im->levels, im->layers);
    }
    vcap_hbuf hb; if (!vcap_hbuf_make(&hb, vcap_sub_off(subs, ns) + 16)) { vcap_cb_run(cb); RFAIL("no staging buffer for texture ser=%llu", (unsigned long long)h.t.serial); }
    VkDeviceSize off = 0; const uint8_t *src = px;
    for (uint32_t i = 0; i < ns; i++) {
        cap_tsub c; memcpy(&c, cs + (size_t)i * sizeof c, sizeof c);
        if (c.bytes != subs[i].bytes || src + c.bytes > d + n) { vcap_cb_run(cb); RFAIL("texture ser=%llu: subresource %u differs from the capture's", (unsigned long long)h.t.serial, i); }
        memcpy(hb.p + off, src, (size_t)c.bytes); src += c.bytes; off += (c.bytes + 15) & ~15ull;
    }
    vcap_img_copy(cb, im, subs, ns, hb.b, 1);
    if (!vcap_cb_run(cb)) RFAIL("texture ser=%llu: the upload did not finish", (unsigned long long)h.t.serial);
    im->track = 0; if (h.pad & 1) im->gpu_owned = 1;   // as the live image: not following its storage
    vcap_hbuf_free(&hb); free(subs);
    g_fill_n++; g_fill_bytes += total;
}
static void check_programs(const char *cap_chain) {
    int missing = 0;
    for (uint32_t i = 0; i < rp_npr; i++) {
        uint64_t now[6]; vcap_iface(&rp_pr[i], now);
        for (int st = 0; st < 6; st++) {
            if (now[st] == rp_prc[i].iface[st]) continue;
            if (!now[st] || !rp_prc[i].iface[st]) { missing++; printf("[RED] nxreplay: program #%u stage %d: %s in this build's library than at capture\n", i, st, now[st] ? "a shader now, none" : "no shader now, one"); continue; }
            RFAIL("program #%u stage %d: the library's binding records differ from the ones the records were captured against (capture chain %s, this build %s): not replaying", i, st, cap_chain, DMSH_CHAIN);
        }
    }
    if (missing) RFAIL("%d program stages resolve differently than at capture: the same shader library is needed (DM_VK_SHLIB)", missing);
}
static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"); if (!a) return 0;
    FILE *b = fopen(to, "wb"); if (!b) { fclose(a); return 0; }
    char buf[1 << 16]; size_t k; int ok = 1;
    while ((k = fread(buf, 1, sizeof buf, a))) ok &= fwrite(buf, 1, k, b) == k;
    fclose(a); ok &= !fclose(b); return ok;
}
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
extern uint64_t vka_n_rp;

__attribute__((visibility("default"))) int nxhost_replay(int argc, char **argv) {
    const char *path = NULL, *png = NULL, *maxpx_arg = "0", *dump = NULL, *outdir = NULL; int repeat = 1, warm = 1, index = 1, nopng = 0, list = 0, dump_rt = 0, dump_gpu = 0, flip = -1, sync = 0;
    setvbuf(stdout, NULL, _IOLBF, 0); crash_install();
#define USAGE "usage: %s <capture> [--warm n] [--repeat n] [--max-pixels n] [--png path | --no-png] [--list] [--dump serial,...] [--dump-rt] [--dump-gpu] [--out dir] [--no-flip] [--sync]\n"
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--repeat") && i + 1 < argc) { repeat = atoi(argv[++i]); if (repeat < 1) repeat = 1; }
        else if (!strcmp(argv[i], "--warm") && i + 1 < argc) { warm = atoi(argv[++i]); if (warm < 1) warm = 1; }
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--no-png")) nopng = 1;
        else if (!strcmp(argv[i], "--max-pixels") && i + 1 < argc) maxpx_arg = argv[++i];
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) { index = atoi(argv[++i]); if (index < 1) index = 1; }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--dump-rt")) dump_rt = 1;
        else if (!strcmp(argv[i], "--dump-gpu")) dump_gpu = 1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--no-flip")) flip = 0;
        else if (!strcmp(argv[i], "--sync")) sync = 1;   // wait for the GPU after every submission (as the capture did), not only before captured changes
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { fprintf(stderr, USAGE, argv[0]); return 2; }
    }
    if (!path) { fprintf(stderr, USAGE, argv[0]); return 2; }
    uint64_t maxpx = strtoull(maxpx_arg, NULL, 10);
    char outb[1100]; if (!outdir) { snprintf(outb, sizeof outb, "%s", path); char *s = strrchr(outb, '/'); if (s) *s = 0; else strcpy(outb, "."); outdir = outb; }
    const char *cname = strrchr(path, '/'); cname = cname ? cname + 1 : path;
    if (repeat > 1) {   // each replay in a fresh process
        int ok = 0, worst = 0; char self[1024]; ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1); if (sl <= 0) RFAIL("no /proc/self/exe"); self[sl] = 0;
        for (int k = 1; k <= repeat; k++) {
            char idx[16], wn[16]; snprintf(idx, sizeof idx, "%d", k); snprintf(wn, sizeof wn, "%d", warm);
            const char *a[32]; int na = 0;
            a[na++] = self; a[na++] = path; a[na++] = "--index"; a[na++] = idx; a[na++] = "--max-pixels"; a[na++] = maxpx_arg; a[na++] = "--warm"; a[na++] = wn; a[na++] = "--out"; a[na++] = outdir;
            if (png) { a[na++] = "--png"; a[na++] = png; } if (nopng) a[na++] = "--no-png"; if (flip == 0) a[na++] = "--no-flip"; if (sync) a[na++] = "--sync";
            a[na] = NULL;
            extern char **environ; pid_t pid; int st = 0;
            if (posix_spawn(&pid, self, NULL, NULL, (char *const *)a, environ) || waitpid(pid, &st, 0) < 0) RFAIL("cannot run %s", self);
            int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 2; ok += rc == 0; worst = MAX(worst, rc);
        }
        printf("%d of %d fresh replays match the live frame\n", ok, repeat);
        return worst;
    }
    uint64_t tl0 = mono();
    nvn_serial_floor(1ull << 61);   // the replay's own objects take serials above the capture's (docs/reference/nvn-objects.md)
    // The file is mapped (private: decoding writes into the chunks); chunks of up to 16 MB are copied out, aligned, the large ones (pool bytes, texture contents) are read in place
    int fd = open(path, O_RDONLY); if (fd < 0) RFAIL("%s: cannot read", path);
    struct stat sb; fstat(fd, &sb); size_t fn = (size_t)sb.st_size;
    uint8_t *fm = fn >= 16 ? mmap(NULL, fn, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) : MAP_FAILED; close(fd);
    if (fm == MAP_FAILED) RFAIL("%s: cannot map", path);
    uint32_t ver; memcpy(&ver, fm + 8, 4);
    if (memcmp(fm, CAP_MAGIC, 8) || ver != CAP_VERSION) RFAIL("%s: not a capture of format %u", path, CAP_VERSION);
    madvise(fm, fn, MADV_SEQUENTIAL);
    for (size_t pos = 16;;) {
        cap_chunk c; if (pos + sizeof c > fn) RFAIL("%s: truncated at byte %zu (the capture did not finish)", path, pos);
        memcpy(&c, fm + pos, sizeof c); pos += sizeof c;
        if (c.tag == CAP_END_) break;
        if (c.stored > fn - pos) RFAIL("%s: corrupt chunk at byte %zu", path, pos);
        if (c.codec != CAP_RAW || c.stored != c.raw) RFAIL("%s: a compressed chunk (a Mac capture?): this replayer reads Android captures", path);
        uint8_t *d = fm + pos;
        if (c.raw <= (16u << 20)) { uint8_t *k = malloc(c.raw ? (size_t)c.raw : 1); memcpy(k, d, (size_t)c.raw); d = k; }
        rp_ch = realloc(rp_ch, (rp_nch + 1) * sizeof *rp_ch); rp_ch[rp_nch++] = (chunk){c, d};
        pos += c.stored;
    }
    char *head = NULL; size_t hn = 0; const char *lay = NULL; size_t ln = 0; const char *stat = ""; size_t sn = 0;
    for (size_t i = 0; i < rp_nch; i++) { if (rp_ch[i].c.tag == CAP_HEAD) { head = (char *)rp_ch[i].d; hn = rp_ch[i].c.raw; } else if (rp_ch[i].c.tag == CAP_LAYT) { lay = (const char *)rp_ch[i].d; ln = rp_ch[i].c.raw; }
        else if (rp_ch[i].c.tag == CAP_STAT) { stat = (const char *)rp_ch[i].d; sn = rp_ch[i].c.raw; } }
    if (!head || !lay) RFAIL("%s: no header", path);
    char title[64] = "", chain[64] = "?", commit[128] = "?", frame[32] = "?", build[64] = "?", backend[32] = "metal", v[1024];
    head_get(head, hn, "title", title, sizeof title); head_get(head, hn, "chain", chain, sizeof chain); head_get(head, hn, "commit", commit, sizeof commit);
    head_get(head, hn, "frame", frame, sizeof frame); head_get(head, hn, "build", build, sizeof build); head_get(head, hn, "backend", backend, sizeof backend);
    if (strcmp(backend, "vulkan")) RFAIL("the capture is of the %s replay: this replayer replays Android captures (DM_NVN_CAPTURE on the phone)", backend);
    if (strcmp(title, DM_GAME_ID)) RFAIL("the capture is of %s, this replayer is built for %s", title, DM_GAME_ID);
    if (ln != strlen(cap_layout()) || memcmp(lay, cap_layout(), ln)) RFAIL("the capture's record layout differs from this build's (nvn_obj.h changed since the capture): capture again");
    if (!head_get(head, hn, "iface", v, sizeof v) || atoi(v) != VCAP_IFACE) RFAIL("the capture's shader interface hashes are of another kind: capture again");
    printf("capture %s: %s frame %s, build %s, host %s, chain %s (this build: chain %s)\n", path, title, frame, build, commit, chain, DMSH_CHAIN);
    apply_env(head, hn);
    setenv("DM_VK_STATS", "0", 0);   // frame numbers jump between replays: no every-300-frames lines
    setenv("DM_VK_PREWARM", "0", 0);   // no background pipeline making beside the replay's timings (vka_pso.c prewarm list)
    if (!getenv("DM_VK_SHLIB") && head_get(head, hn, "vk_shlib", v, sizeof v)) setenv("DM_VK_SHLIB", v, 1);
    if (!getenv("DM_VK_PCACHE")) {   // a cache of this replayer's own beside it, seeded once from the live run's (read only: the app's file is left alone)
        char self[1024], pc[1100]; ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
        if (sl > 0) { self[sl] = 0; char *s = strrchr(self, '/'); if (s) *s = 0; snprintf(pc, sizeof pc, "%s/vk_pipelines" VKA_PCACHE_GEN ".bin", self); setenv("DM_VK_PCACHE", pc, 1);
            // (a live cache of the generation before, compiled with the game's FPCR (vka_fpenv.h), seeds nothing)
            if (access(pc, R_OK) && head_get(head, hn, "vk_pcache", v, sizeof v) && !strstr(v, VKA_PCACHE_GEN ".bin")) printf("pipeline cache not seeded: the live run's %s is not of this generation (" VKA_PCACHE_GEN ")\n", v);
            else if (access(pc, R_OK) && head_get(head, hn, "vk_pcache", v, sizeof v) && copy_file(v, pc)) {
                printf("pipeline cache seeded from the live run's %s\n", v);
                char sv[1100], sp[1100]; snprintf(sv, sizeof sv, "%s.lib", v); snprintf(sp, sizeof sp, "%s.lib", pc); copy_file(sv, sp);   // its library stamp (vka_pso.c)
            } }
    }
    if (!vka_init()) RFAIL("no Vulkan device");
    int origin_ll = head_get(stat, sn, "origin_ll", v, sizeof v) ? atoi(v) : 1;
    mtl_set_origin_lower_left(origin_ll);
    if (head_get(stat, sn, "depth_zero", v, sizeof v)) mtl_set_depth_zero(atoi(v));
    if (head_get(stat, sn, "nvn2", v, sizeof v)) vka_set_nvn2(atoi(v));   // the replayer loads no sdk module, so dm_sdk_ounce() cannot tell
    int qll = head_get(stat, sn, "queue_origin_ll", v, sizeof v) ? atoi(v) : 1;
    if (flip < 0) flip = origin_ll;
    // regions first, then the object tables, sized before any is filled
    uint32_t nsub = 0; const cap_pres *pres = NULL;
#define GROW(n, idx) do { if ((idx) >= (n)) n = (idx) + 1; } while (0)
    for (size_t i = 0; i < rp_nch; i++) {
        cap_chunk *c = &rp_ch[i].c; const uint8_t *d = rp_ch[i].d; uint32_t idx;
        if (c->tag == CAP_REGN) { cap_regn h; if (c->raw < sizeof h) RFAIL("short region chunk"); memcpy(&h, d, sizeof h); if (h.size != c->raw - sizeof h) RFAIL("region %u: size mismatch", h.index); map_region(h.index, &h, d + sizeof h); }
        if (c->tag == CAP_SUBM) nsub = MAX(nsub, c->submit);
        if (c->tag == CAP_PRES && c->raw == sizeof *pres) pres = (const cap_pres *)d;
        if (c->tag == CAP_POBJ) for (size_t o = 0; o + sizeof(cap_pobj) <= c->raw; o += sizeof(cap_pobj)) { memcpy(&idx, d + o, 4); GROW(rp_npo, idx); }
        if (c->tag == CAP_TOBJ) for (size_t o = 0; o + 8 + sizeof(tex_t) <= c->raw; o += 8 + sizeof(tex_t)) { memcpy(&idx, d + o, 4); GROW(rp_nto, idx); }
        if (c->tag == CAP_PROG) for (size_t o = 0; o + sizeof(cap_prog) <= c->raw; o += sizeof(cap_prog)) { memcpy(&idx, d + o, 4); GROW(rp_npr, idx); }
        if (c->tag == CAP_DPOL) for (size_t o = 0; o + sizeof(cap_dpol) <= c->raw; o += sizeof(cap_dpol)) { memcpy(&idx, d + o, 4); GROW(rp_ndp, idx); }
        if (c->tag == CAP_SYNC && c->raw >= 4) { memcpy(&idx, d, 4); rp_nsy = MAX(rp_nsy, idx); }
    }
#undef GROW
    if (!pres) RFAIL("%s: no present (the capture did not finish)", path);
    rp_po = calloc(rp_npo + 1, sizeof *rp_po); rp_to = calloc(rp_nto + 1, sizeof *rp_to); rp_pr = calloc(rp_npr + 1, sizeof *rp_pr); rp_prc = calloc(rp_npr + 1, sizeof *rp_prc);
    rp_dp = calloc(rp_ndp + 1, sizeof *rp_dp); rp_sy = calloc(rp_nsy + 1, sizeof *rp_sy);
    for (size_t i = 0; i < rp_nch; i++) {
        cap_chunk *c = &rp_ch[i].c; const uint8_t *d = rp_ch[i].d; if (c->tag != CAP_POBJ) continue;
        for (size_t o = 0; o + sizeof(cap_pobj) <= c->raw; o += sizeof(cap_pobj)) { cap_pobj p; memcpy(&p, d + o, sizeof p);
            uint64_t m = p.mem, b0 = rp_bad; dec_addr(NULL, &m, 0, 0);
            if (rp_bad != b0) { printf("pool object #%u: its memory is in no captured region (a finalized pool a stale texture names): replayed without memory\n", p.index); rp_bad = b0; m = 0; }
            rp_po[p.index] = (pool_t){MAGIC('P', 'O', 'O', 'L'), p.flags, (uint8_t *)(uintptr_t)m, p.size, NULL, p.vmode}; nvn_pool_note(&rp_po[p.index]); }
    }
    for (size_t i = 0; i < rp_nch; i++) {
        cap_chunk *c = &rp_ch[i].c; const uint8_t *d = rp_ch[i].d;
        if (c->tag == CAP_TOBJ) for (size_t o = 0; o + 8 + sizeof(tex_t) <= c->raw; o += 8 + sizeof(tex_t)) { uint32_t idx; memcpy(&idx, d + o, 4); memcpy(&rp_to[idx], d + o + 8, sizeof(tex_t)); }
        if (c->tag == CAP_PROG) for (size_t o = 0; o + sizeof(cap_prog) <= c->raw; o += sizeof(cap_prog)) { cap_prog p; memcpy(&p, d + o, sizeof p);
            rp_prc[p.index] = p; prog_t *pr = &rp_pr[p.index]; pr->magic = MAGIC('P', 'R', 'O', 'G'); pr->nstage = p.nstage; pr->serial = p.serial; }
        if (c->tag == CAP_DPOL) for (size_t o = 0; o + sizeof(cap_dpol) <= c->raw; o += sizeof(cap_dpol)) { cap_dpol p; memcpy(&p, d + o, sizeof p);
            uint64_t po = p.pool; dec_obj(NULL, CAP_K_POOL, &po, NULL, 0); rp_dp[p.index] = (dpool_t){(const pool_t *)(uintptr_t)po, p.off, p.n}; }
    }
    for (uint32_t i = 0; i < rp_nto; i++) {   // a texture whose storage field names no captured memory (the window's textures may hold none the capture knows): kept without storage
        uint64_t b0 = rp_bad; cap_tex_fields(&rp_to[i], &rp_dec, NULL);
        if (rp_bad != b0) { printf("texture object #%u (ser=%llu, %ux%u format %#x): its storage is in no captured region: replayed without storage\n", i, (unsigned long long)rp_to[i].serial,
                                   rp_to[i].d.width, rp_to[i].d.height, rp_to[i].d.format); rp_to[i].mem = NULL; rp_to[i].pool = NULL; rp_bad = b0; }
    }
    for (uint32_t i = 0; i < rp_npr; i++) nvn_prog_keys_set(&rp_pr[i], (const uint8_t (*)[8])rp_prc[i].key, rp_prc[i].ok);
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_DPOL) for (size_t o = 0; o + sizeof(cap_dpol) <= rp_ch[i].c.raw; o += sizeof(cap_dpol)) { cap_dpol p; memcpy(&p, rp_ch[i].d + o, sizeof p);
        nvn_dpool_base_set(&rp_dp[p.index], p.tex_base, p.samp_base); }
    uint64_t ncpu = 0, ntreg_bad = 0;
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_TKND) for (size_t o = 0; o + sizeof(cap_tknd) <= rp_ch[i].c.raw; o += sizeof(cap_tknd)) {
        cap_tknd *k = (cap_tknd *)(rp_ch[i].d + o); uint64_t b0 = rp_bad; cap_tex_fields(&k->t, &rp_dec, NULL); if (rp_bad != b0) { rp_bad = b0; continue; } if (k->kind & CAP_TK_CPU) { mtl_tex_cpu_written(&k->t); ncpu++; } }
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_TREG) for (size_t o = 0; o + sizeof(cap_treg) <= rp_ch[i].c.raw; o += sizeof(cap_treg)) {
        cap_treg *e = (cap_treg *)(rp_ch[i].d + o); if (!e->ok) continue;
        uint64_t b0 = rp_bad, m0 = (uint64_t)(uintptr_t)e->t.mem; cap_tex_fields(&e->t, &rp_dec, NULL);
        if (m0 && !cap_is_addr(m0)) rp_bad++;   // a live address the capture could not place (outside every pool)
        if (rp_bad != b0) { if (ntreg_bad++ < 4) printf("[RED] nxreplay: texture registry entry %d (ser=%llu): its storage is in no captured region: left unregistered\n", e->tid, (unsigned long long)e->t.serial); e->ok = 0; rp_bad = b0; } }
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_PADD && rp_ch[i].c.submit <= 1) add_pools(rp_ch[i].d, rp_ch[i].c.raw);
    if (rp_bad) RFAIL("%llu captured pointers (pools, descriptor pools) name no captured object or region", (unsigned long long)rp_bad);
    if (ntreg_bad) printf("%llu texture registry entries left unregistered (storage in no captured region)\n", (unsigned long long)ntreg_bad);
    check_programs(chain);
    uint64_t rbytes = 0; for (uint32_t r = 0; r < rp_nrg; r++) rbytes += rp_rg[r].size;
    printf("loaded in %.0f ms: %u regions (%.0f MB, %llu not at their live address), %d pools, %u textures, %u programs, %u descriptor pools, %u submits; %llu CPU-written textures marked\n",
           (mono() - tl0) / 1e6, rp_nrg, rbytes / 1048576.0, (unsigned long long)rp_moved, rp_pools_added, rp_nto, rp_npr, rp_ndp, nsub, (unsigned long long)ncpu);

    // what each submit takes before it: if nothing, the replay need not wait for the GPU in between
    uint8_t *has_state = calloc(nsub + 2, 1);
    for (size_t i = 0; i < rp_nch; i++) { uint32_t t = rp_ch[i].c.tag; if (t == CAP_PADD || t == CAP_PDIF || t == CAP_TREG || t == CAP_SREG || t == CAP_TPIX) has_state[MIN(rp_ch[i].c.submit, nsub + 1)] = 1; }
    const uint8_t *lpix = NULL; uint64_t lpixn = 0;
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_LPIX) { lpix = rp_ch[i].d; lpixn = rp_ch[i].c.raw; }
    char live[65]; for (int i = 0; i < 32; i++) snprintf(live + 2 * i, 3, "%02x", pres->sha256[i]);
    int same_all = 1, ok_all = 1; uint64_t worst_px = 0; double best_cpu = 1e30, best_gpu = 1e30, best_wall = 1e30;
    for (int rep = 0; rep < warm; rep++) {
        uint64_t fno = 1000000 + 4 * (uint64_t)(index * 1000 + rep), exec_ns = 0, setup_ns = 0, d0, s0, p0, n0, rp0 = vka_n_rp, dd, ds;
        uint64_t restored = 0, tr0 = mono();
        if (rep) for (uint32_t r = 0; r < rp_nrg; r++) if (rp_rg[r].mem)   // back to the state at the first submit: runs of the blocks that changed, their pages unprotected first (the dirty tracker's)
            for (uint64_t o = 0; o < rp_rg[r].size;) {
                uint64_t e = o; while (e < rp_rg[r].size) { size_t l = (size_t)MIN(4096, rp_rg[r].size - e); if (!memcmp(rp_rg[r].mem + e, rp_rg[r].orig + e, l)) break; e += l; }
                if (e > o) { dm_guest_write_prepare(rp_rg[r].mem + o, (size_t)(e - o)); memcpy(rp_rg[r].mem + o, rp_rg[r].orig + o, (size_t)(e - o)); restored += e - o; o = e; }
                else o += 4096;
            }
        if (rep) printf("replay %d: %.1f MB of pool memory restored to the capture's in %.0f ms\n", index + rep, restored / 1048576.0, (mono() - tr0) / 1e6);
        uint64_t fill0 = g_fill_n, prep_ns = 0, tw0 = 0;
        for (uint32_t s = 1; s <= nsub; s++) {
            uint64_t t0 = mono();
            for (size_t i = 0; i < rp_nch; i++) {
                cap_chunk *c = &rp_ch[i].c; uint8_t *d = rp_ch[i].d; if (c->submit != s) continue;
                if (c->tag == CAP_PADD && s > 1) add_pools(d, c->raw);
                else if (c->tag == CAP_PDIF) for (size_t o = 0; o + 24 <= c->raw;) { uint32_t r; uint64_t off, len; memcpy(&r, d + o, 4); memcpy(&off, d + o + 8, 8); memcpy(&len, d + o + 16, 8);
                    if (r >= rp_nrg || off + len > rp_rg[r].size || o + 24 + len > c->raw) RFAIL("a pool change outside its region"); dm_guest_write_prepare(rp_rg[r].mem + off, (size_t)len); memcpy(rp_rg[r].mem + off, d + o + 24, (size_t)len); o += 24 + (size_t)len; }
                else if (c->tag == CAP_TREG) for (size_t o = 0; o + sizeof(cap_treg) <= c->raw; o += sizeof(cap_treg)) { cap_treg *e = (cap_treg *)(d + o); if (!e->ok) continue;
                    if (e->pad) mtl_reg_image(e->tid, &e->t, e->has_v ? &e->v : NULL); else mtl_reg_tex(e->tid, &e->t, e->has_v ? &e->v : NULL); }
                else if (c->tag == CAP_SREG) for (size_t o = 0; o + sizeof(cap_sreg) <= c->raw; o += sizeof(cap_sreg)) { cap_sreg e; memcpy(&e, d + o, sizeof e); mtl_reg_samp(e.sid, &e.p); }
                else if (c->tag == CAP_TPIX) fill_tex(d, c->raw);
            }
            uint32_t nh = 0; const uint32_t *top = NULL; rp_nseg = 0;
            for (size_t i = 0; i < rp_nch; i++) { cap_chunk *c = &rp_ch[i].c; if (c->submit != s) continue;
                if (c->tag == CAP_SEGM) { uint32_t idx, len; memcpy(&idx, rp_ch[i].d, 4); memcpy(&len, rp_ch[i].d + 4, 4); if (idx >= rp_nseg) { rp_seg = realloc(rp_seg, (idx + 1) * sizeof *rp_seg); memset(rp_seg + rp_nseg, 0, (idx + 1 - rp_nseg) * sizeof *rp_seg); rp_nseg = idx + 1; }
                    rp_seg[idx] = malloc(len + 8); memcpy(rp_seg[idx], rp_ch[i].d + 8, len); memset(rp_seg[idx] + len, 0, 8); }
                if (c->tag == CAP_SUBM) { memcpy(&nh, rp_ch[i].d, 4); top = (const uint32_t *)(rp_ch[i].d + 4); } }
            for (size_t i = 0; i < rp_nch; i++) { cap_chunk *c = &rp_ch[i].c; if (c->submit != s || c->tag != CAP_SEGM) continue;
                uint32_t idx, len; memcpy(&idx, rp_ch[i].d, 4); memcpy(&len, rp_ch[i].d + 4, 4);
                for (uint32_t o = 0; o < len;) { op_hdr h; memcpy(&h, rp_seg[idx] + o, 8); if (h.size < 8 || o + h.size > len) RFAIL("submit %u segment %u: corrupt record at %u", s, idx, o);
                    if (!cap_rec_fields(h.op, rp_seg[idx] + o + 8, h.size, &rp_dec, NULL) || rp_bad) RFAIL("submit %u segment %u: op %u at %u does not decode", s, idx, h.op, o); o += h.size; } }
            uint64_t *hs = malloc((size_t)MAX(nh, 1) * 8); for (uint32_t i = 0; i < nh; i++) { if (top[i] >= rp_nseg || !rp_seg[top[i]]) RFAIL("submit %u: handle %u names no segment", s, i); hs[i] = (uint64_t)(uintptr_t)rp_seg[top[i]]; }
            setup_ns += mono() - t0;
            if (s == 1) {   // untimed: what the captured state makes the replay do before it records (images of the registered textures made and uploaded, descriptors written)
                uint64_t tp = mono();
                mtl_set_frame(fno - 1); mtl_sub_begin(); mtl_sub_commit(); mtl_finish();
                prep_ns = mono() - tp;
                vka_draw_stats(&d0, &s0, &p0, &n0); tw0 = mono();
            }
            uint64_t t1 = mono();
            mtl_set_frame(fno); mtl_set_queue_origin_lower_left(qll);
            mtl_sub_begin(); nvn_ft_poll(); mtl_sub_index(hs, (int)nh);
            for (uint32_t i = 0; i < nh; i++) { mtl_exec_rest(hs + i + 1, (int)(nh - i - 1)); mtl_exec((const uint8_t *)(uintptr_t)hs[i]); }
            mtl_exec_rest(NULL, 0); mtl_sub_commit();
            exec_ns += mono() - t1;
            if (sync || s == nsub || has_state[s + 1]) mtl_finish();   // the capture read the next submit's inputs after the GPU finished this one
            free(hs); for (uint32_t i = 0; i < rp_nseg; i++) { free(rp_seg[i]); rp_seg[i] = NULL; }
        }
        double wall = (mono() - tw0) / 1e6;
        uint64_t d1, s1, p1, n1; vka_draw_stats(&d1, &s1, &p1, &n1); dd = d1 - d0; ds = s1 - s0;
        mtl_set_frame(fno + 1); mtl_sub_begin(); mtl_sub_commit(); mtl_finish();   // collects the frame's GPU timestamps
        double gpu = mtl_gpu_ms(fno), cpu = exec_ns / 1e6;
        tex_t pt; uint64_t pv = pres->tex; if (!dec_obj(NULL, CAP_K_TEX, &pv, NULL, 0)) RFAIL("the presented texture is not in the capture"); pt = *(const tex_t *)(uintptr_t)pv;
        uint8_t md[32]; uint64_t bytes; uint32_t w, h, pf; char out[65], pngp[1200]; cap_buf mine = {0};
        if (png) snprintf(pngp, sizeof pngp, "%s", png); else if (nopng) pngp[0] = 0; else snprintf(pngp, sizeof pngp, "%s/%s.replay%d.png", outdir, cname, index + rep);
        if (!vcap_digest(&pt, md, &bytes, &w, &h, &pf, pngp, flip, &mine)) RFAIL("the presented texture cannot be read back");
        for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", md[i]);
        int same = !memcmp(md, pres->sha256, 32); same_all &= same;
        uint64_t npx = 0; uint32_t big = 0, x0 = UINT32_MAX, x1 = 0, y0 = UINT32_MAX, y1 = 0; int have_px = !same && lpix && lpixn == mine.n && w == pres->width && h == pres->height && pf == pres->pixfmt && w && h;
        if (have_px) { size_t bpp = (size_t)(mine.n / ((uint64_t)w * h));
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { const uint8_t *a = lpix + ((size_t)y * w + x) * bpp, *b = mine.p + ((size_t)y * w + x) * bpp; uint32_t dv = 0;
                for (size_t k = 0; k < bpp; k++) dv = MAX(dv, (uint32_t)abs((int)a[k] - (int)b[k]));
                if (dv) { npx++; big = MAX(big, dv); x0 = MIN(x0, x); x1 = MAX(x1, x); y0 = MIN(y0, y); y1 = MAX(y1, y); } } }
        int ok = same || (have_px && npx <= maxpx); ok_all &= ok; if (have_px) worst_px = MAX(worst_px, npx);
        cap_buf_free(&mine);
        if (have_px) printf("replay %d: %llu pixels differ from the live frame (largest byte difference %u, x %u-%u y %u-%u)\n", index + rep, (unsigned long long)npx, big, x0, x1, y0, y1);
        printf("replay %d: digest %s %s live %s (%ux%u VkFormat %u)%s%s\n", index + rep, out, same ? "==" : "!=", live, w, h, pf, pngp[0] ? " → " : "", pngp);
        printf("replay %d: submits CPU %.2f ms (recording, %u submits), GPU %.2f ms, wall %.2f ms to the last fence; %llu draws (%llu skipped), %llu dispatches, %llu render passes, %llu pipelines made; "
               "before the frame: %.1f ms applying the captured state (%llu images filled), %.1f ms preparing\n", index + rep, cpu, nsub, gpu, wall, (unsigned long long)dd, (unsigned long long)ds,
               (unsigned long long)(p1 - p0), (unsigned long long)(vka_n_rp - rp0), (unsigned long long)(n1 - n0), setup_ns / 1e6, (unsigned long long)(g_fill_n - fill0), prep_ns / 1e6);
        if (rep || warm == 1) { best_cpu = MIN(best_cpu, cpu); if (gpu >= 0) best_gpu = MIN(best_gpu, gpu); best_wall = MIN(best_wall, wall); }
        if (rep == 0) {   // pool blocks after the frame against the live run's
            uint64_t live_only = 0, rep_only = 0, both = 0, blocks = 0;
            for (size_t i = 0; i < rp_nch; i++) { if (rp_ch[i].c.tag != CAP_PHSH) continue; const uint8_t *d = rp_ch[i].d;
                for (size_t o = 0; o + 8 <= rp_ch[i].c.raw;) { uint32_t r, nb; memcpy(&r, d + o, 4); memcpy(&nb, d + o + 4, 4); o += 8;
                    for (uint32_t k = 0; k < nb && o + 8 <= rp_ch[i].c.raw; k++, o += 8) {
                        if (r >= rp_nrg || !rp_rg[r].mem) continue; uint64_t off = (uint64_t)k * 4096; size_t len = (size_t)MIN(4096, rp_rg[r].size - off); uint64_t lh; memcpy(&lh, d + o, 8);
                        uint64_t h0 = 0x9E3779B97F4A7C15ull ^ len, h1 = h0, vv; const uint8_t *a = rp_rg[r].orig + off, *b = rp_rg[r].mem + off; size_t q = 0;
                        for (; q + 8 <= len; q += 8) { memcpy(&vv, a + q, 8); h0 = (h0 ^ vv) * 0xff51afd7ed558ccdull; h0 ^= h0 >> 32; memcpy(&vv, b + q, 8); h1 = (h1 ^ vv) * 0xff51afd7ed558ccdull; h1 ^= h1 >> 32; }
                        for (; q < len; q++) { h0 = (h0 ^ a[q]) * 0x100000001b3ull; h1 = (h1 ^ b[q]) * 0x100000001b3ull; }
                        blocks++; if (h1 == lh) continue;
                        if (lh != h0 && h1 == h0) live_only++; else if (lh == h0) rep_only++; else both++; } } }
            printf("pool blocks after the frame: %llu of %llu differ from the live run (%llu written only live, %llu only in the replay, %llu in both with other bytes)\n",
                   (unsigned long long)(live_only + rep_only + both), (unsigned long long)blocks, (unsigned long long)live_only, (unsigned long long)rep_only, (unsigned long long)both);
        }
        if (rep == warm - 1 && (list || dump || dump_rt)) {   // the textures the records name, after the last replay
            nvn_hmap seen = {.klen = 8};
            for (uint32_t i = 0; i < rp_nto; i++) {
                const tex_t *t = &rp_to[i]; if (!t->serial) continue;
                vka_img *im = vka_tex_acquire(t->serial); if (!im) { if (list) printf("  ser=%llu: no image\n", (unsigned long long)t->serial); continue; }
                uint64_t key = (uint64_t)(uintptr_t)im; int again = nvn_hm_get(&seen, &key) != NULL; nvn_hm_put(&seen, &key, (void *)1);
                if (list) printf("  ser=%llu %ux%ux%u layers %u levels %u VkFormat %d NVN format %#x%s%s%s\n", (unsigned long long)t->serial, im->w, im->h, im->depth, im->layers, im->levels, im->f.vk, im->d.format,
                                 im->gpu_owned ? " gpu-written" : "", im->track ? " tracked" : "", again ? " (same image as above)" : "");
                char sv[32]; snprintf(sv, sizeof sv, "%llu", (unsigned long long)t->serial); int want = 0;
                if (dump) { const char *p = dump; size_t l = strlen(sv); while ((p = strstr(p, sv))) { if ((p == dump || p[-1] == ',') && (p[l] == 0 || p[l] == ',')) { want = 1; break; } p += l; } }
                if (dump_rt && im->gpu_owned && im->f.kind < 3 && !again) want = 1;
                if (want) { size_t n = 0; uint8_t *px = vcap_img_level0(im, &n); char pp[1300]; snprintf(pp, sizeof pp, "%s/%s.r%d_%s.png", outdir, cname, index + rep, sv);
                    if (px && vcap_png(pp, px, im->w, im->h, im->f.vk, flip)) printf("  ser=%s → %s\n", sv, pp); else printf("  ser=%s: no picture (VkFormat %d)\n", sv, im->f.vk); free(px); }
                vka_tex_drop(im);
            }
            nvn_hm_clear(&seen, NULL);
        }
        if (rep == warm - 1 && (dump_gpu || dump)) {   // every GPU-written colour image alive after the last replay, and the --dump serials among them (an image no record names: one only texture IDs bind)
            enum { MAXI = 16384 }; vka_img **ims = malloc(MAXI * sizeof *ims); int n = ims ? vka_tex_live_refs(ims, MAXI) : 0;
            for (int i = 0; i < n; i++) { vka_img *im = ims[i];
                int named = 0; if (dump) { char sv[32]; snprintf(sv, sizeof sv, "%llu", (unsigned long long)im->serial); size_t l = strlen(sv);
                    for (const char *p = dump; (p = strstr(p, sv)); p += l) if ((p == dump || p[-1] == ',') && (p[l] == 0 || p[l] == ',')) { named = 1; break; } }
                if (((dump_gpu && im->gpu_owned) || named) && im->f.kind < 3 && im->w >= 64 && im->h >= 64) { size_t nb = 0; uint8_t *px = vcap_img_level0(im, &nb); char pp[1300];
                    snprintf(pp, sizeof pp, "%s/%s.r%d_g%llu_%ux%u_f%d.png", outdir, cname, index + rep, (unsigned long long)im->serial, im->w, im->h, im->f.vk);
                    if (px && vcap_png(pp, px, im->w, im->h, im->f.vk, flip)) printf("  gpu ser=%llu %ux%u VkFormat %d → %s\n", (unsigned long long)im->serial, im->w, im->h, im->f.vk, pp);
                    else printf("  gpu ser=%llu %ux%u: no picture (VkFormat %d)\n", (unsigned long long)im->serial, im->w, im->h, im->f.vk); free(px); }
                vka_tex_drop(im); }
            free(ims);
            for (const char *p = dump; p && *p; ) {   // --dump serials by the serial map (a serial whose image another serial made: im->serial is that one's)
                char *e; unsigned long long sv = strtoull(p, &e, 10); if (e == p) break; p = *e == ',' ? e + 1 : e;
                vka_img *im = vka_tex_acquire(sv); if (!im) { printf("  ser=%llu: no image\n", sv); continue; }
                size_t nb = 0; uint8_t *px = im->f.kind < 3 ? vcap_img_level0(im, &nb) : NULL; char pp[1300];
                snprintf(pp, sizeof pp, "%s/%s.r%d_s%llu_%ux%u_f%d.png", outdir, cname, index + rep, sv, im->w, im->h, im->f.vk);
                if (px && vcap_png(pp, px, im->w, im->h, im->f.vk, flip)) printf("  ser=%llu (image of serial %llu%s) %ux%u VkFormat %d → %s\n", sv, (unsigned long long)im->serial, im->gpu_owned ? ", gpu-written" : "", im->w, im->h, im->f.vk, pp);
                else printf("  ser=%llu: no picture (VkFormat %d)\n", sv, im->f.vk);
                if (px && nb) { snprintf(pp, sizeof pp, "%s/%s.r%d_s%llu_%ux%u_f%d.raw", outdir, cname, index + rep, sv, im->w, im->h, im->f.vk); FILE *rf = fopen(pp, "wb"); if (rf) { fwrite(px, 1, nb, rf); fclose(rf); printf("  ser=%llu raw level 0 (%zu bytes) → %s\n", sv, nb, pp); } }
                free(px); vka_tex_drop(im);
            }
        }
    }
    vka_pso_save(1);
    if (warm > 1) printf("best of the warm replays: submits CPU %.2f ms, GPU %.2f ms, wall %.2f ms\n", best_cpu, best_gpu < 1e29 ? best_gpu : -1.0, best_wall);
    if (same_all) printf("MATCH: the replayed frame equals the live frame\n");
    else if (ok_all) printf("MATCH within --max-pixels %llu: at most %llu pixels differ\n", (unsigned long long)maxpx, (unsigned long long)worst_px);
    else printf("[RED] MISMATCH: the replayed frame differs from the live frame%s\n", lpix ? "" : " (compared by digest only)");
    fflush(stdout);
    _exit(ok_all ? 0 : 1);   // no destructors: the device and placed memory go with the process
}
#endif
