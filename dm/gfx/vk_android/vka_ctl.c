// Live control of the replay while the game runs (android/TESTING.md "Live control"): the host's DM_CTL reader (hle_hid.c ctl_main, a thread polling the file every 100 ms;
// the launcher points DM_CTL at <app files>/ctl.txt) hands the Android-only lines to dm_gfx_ctl / dm_gfx_ctl_env here:
//   "env KEY=VALUE"   queued, applied at the next frame boundary: the first submission after the next present (vka_ctl_sub, under the host's submit lock, so no submission is
//                     being recorded). It sets the value for VKA_ENV readers (an override table, read before the process environment) and in the environment (setenv, for host
//                     switches read at use), then bumps vka_env_gen: every VKA_ENV call site reads its switch again on its next call. "env KEY=" clears an override
//                     (the switch reads as unset). Switches that need a restart are named in the log line (g_restart)
//   "capture <n>"     the next n frames, one log line each: draws, skipped draws, render passes, full barriers, clears / copies, submissions, recording ms, GPU ms
//   "screenshot [p]"  the next presented game frame (the window texture, before the present's scaling) as a PNG, by default ctl-shot-<frame>.png beside the control file
//   "stats"           one log line now: the last 60 frames' averages, the frame rate, placed memory, pipelines, the switches set live
// Every answer is a log line starting "[vk] ctl:" (android/tools/nxctl.sh waits for them)
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <math.h>
#include <sys/stat.h>

_Atomic uint32_t vka_env_gen = 1;   // 1: call sites' zeroed caches read their switch on the first call
// The launcher's environment set (nxlaunch.c, after its setenv lines): a switch read before it (the present thread starts with the window, before the game's environment:
// DM_VK_FSR was read as unset and FSR stayed off until a live change) is read again at its next call
__attribute__((visibility("default"))) void nxhost_env_changed(void) { atomic_fetch_add(&vka_env_gen, 1); }

// ---------- Switches ----------
enum { NOVR = 64 };
static struct { char key[64]; char val[256]; int set; } g_ovr[NOVR];   // the switches set live; set 0 = cleared (reads as unset)
static int g_novr;
static pthread_mutex_t g_emu = PTHREAD_MUTEX_INITIALIZER;
static struct { char key[64]; char val[256]; } g_pend[NOVR];   // queued by the control thread, applied at the frame boundary
static int g_npend;
static _Atomic int g_pending; static _Atomic uint64_t g_pend_frame;
uint64_t vka_frame(void);

static const char *ovr_find(const char *key, int *found) {   // under g_emu
    for (int i = 0; i < g_novr; i++) if (!strcmp(g_ovr[i].key, key)) { *found = 1; return g_ovr[i].set ? g_ovr[i].val : NULL; }
    *found = 0; return NULL;
}
static int str_get(const char *key, char *out, size_t n) {   // 1 = set (out holds the value)
    pthread_mutex_lock(&g_emu);
    int found; const char *v = ovr_find(key, &found);
    if (!found) v = getenv(key);
    int r = v && *v; if (r) snprintf(out, n, "%s", v);
    pthread_mutex_unlock(&g_emu);
    return r;
}
int vka_env_str(const char *key, char *out, size_t n) { return str_get(key, out, n); }
double vka_env_slow(vka_envc *c, const char *key, double def) {
    uint32_t g = atomic_load(&vka_env_gen);   // before the value: a change applied meanwhile makes the next call read again
    char b[256]; double v = str_get(key, b, sizeof b) ? strtod(b, NULL) : def;
    // Refresh the cache only to a newer generation, one thread at a time (gen UINT32_MAX while v is written: readers take this slow path meanwhile); a thread whose g is
    // older than the cache's, or that loses the race, returns its value uncached
    uint32_t cur = atomic_load_explicit(&c->gen, memory_order_relaxed);
    if (cur != UINT32_MAX && (int32_t)(g - cur) > 0 && atomic_compare_exchange_strong_explicit(&c->gen, &cur, UINT32_MAX, memory_order_acquire, memory_order_relaxed)) {
        atomic_store_explicit(&c->v, v, memory_order_relaxed); atomic_store_explicit(&c->gen, g, memory_order_release);
    }
    return v;
}

// Switches read once (at start, or when a device object is made): a live change is logged as taking effect at the next start
static const char *const g_restart[] = {"DM_VK_VALIDATE", "DM_VK_DRIVER", "DM_VK_HOOKDIR", "DM_VK_PLACED", "DM_VK_PREFAULT", "DM_VK_PCACHE", "DM_VK_GUEST_FPCR", "DM_VK_SHLIB", "DM_VK_SHLIB_CHECK", "DM_VK_GS_CLAIM", "DM_VK_SUBGROUP_PIN", "DM_VK_GPL", "DM_VK_MESH", "DM_VK_SUBGROUP_GS", "DM_VK_SHUFFLE_LDS", "DM_VK_ADPF", "DM_VK_ADPF_PIPELINE", "DM_VK_NANSCAN",
                                        "DM_VK_NANSCAN_STEP", "DM_VK_NANSCAN_DRAWS", "DM_VK_NANSCAN_SERIAL", "DM_VK_NANSCAN_FS", "DM_VK_WORKER",
                                        "DM_VK_WORKER_FRAMES", "DM_VK_TEST_HANG", "DM_LSFG_DLL", "DM_FPS", "NXHOST_VK_SELFTEST", "DM_CTL", "DM_VK_PREWARM", "DM_VK_PREWARM_MAX", "DM_VK_PREWARM_FILE", "DM_VK_LC_GLOBAL",
                                        "DM_VK_PSO_ASYNC", NULL};
// Switches the replay reads again after a change (VKA_ENV, or at use); a table, so the log line can say which kind a key is
static const char *const g_live[] = {"DM_VK_DRAW", "DM_VK_STATS", "DM_VK_HANG_S", "DM_VK_HANG_EXIT", "DM_VK_FRAME_RATE", "DM_VK_FRAME_RATE_HOLD", "DM_VK_PCACHE_SAVE_S", "DM_VK_GPL_GIVEUP", "DM_VK_GPL_PROBE", "DM_VK_PREWARM_KEEP", "DM_VK_PRETRANSFORM", "DM_VK_FSR", "DM_VK_FSR_SHARP", "DM_VK_TEX_TRACK",
                                     "DM_VK_CHUNK", "DM_VK_PASS_CLEARS", "DM_VK_PASS_KEEP", "DM_VK_DYN_FILTER", "DM_VK_PROF", "DM_VK_ROWS", "DM_VK_SYNCLOG", "DM_VK_UBWC", "DM_VK_IMG_UPGRADE", "DM_VK_DONE_LAST", "DM_VK_AFF_REPLAY", "DM_VK_AFF_ALL",
                                     "DM_VK_NANSCAN_DUMP", "DM_VK_TEX_STATS", "DM_VK_TEX_STATS_LINES", "DM_VK_TEX_STATS_DRAWS", "DM_VK_TEX_STATS_TABLE", "DM_VK_RT_LOG", "DM_VK_RT_LOG_LINES", "DM_VK_VB_FILTER", "DM_VK_ENC_BATCH", "DM_VK_ENC_SPIN", "DM_VK_BAR_KEEP", "DM_VK_DRV_MEMO", "DM_VK_PUSH_DELTA", "DM_VK_UPLOAD_MT", "DM_VK_UPLOAD_MT_MIN",
                                     "DM_LSFG", "DM_LSFG_MULT", "DM_LSFG_TARGET", "DM_LSFG_FLOW", "DM_VK_DISPATCH_ORDER", NULL};
static int in(const char *const *t, const char *k) { for (; *t; t++) if (!strcmp(*t, k)) return 1; return 0; }

void dm_gfx_ctl_env(const char *key, const char *val) {   // the control thread
    if (!*key || strlen(key) >= sizeof g_pend[0].key || strlen(val) >= sizeof g_pend[0].val) { VLOG("ctl: env %s: key or value too long", key); return; }
    pthread_mutex_lock(&g_emu);
    int i = 0; while (i < g_npend && strcmp(g_pend[i].key, key)) i++;
    if (i == g_npend && g_npend < NOVR) g_npend++;
    if (i < NOVR) { snprintf(g_pend[i].key, sizeof g_pend[i].key, "%s", key); snprintf(g_pend[i].val, sizeof g_pend[i].val, "%s", val); }
    pthread_mutex_unlock(&g_emu);
    atomic_store(&g_pend_frame, vka_frame()); atomic_store(&g_pending, 1);
    VLOG("ctl: env %s=%s queued for the next frame (frame %llu now)", key, val, (unsigned long long)vka_frame());
}
static int lsfg_int(const char *k, int def) { char b[64]; return str_get(k, b, sizeof b) ? atoi(b) : def; }
static void apply(void) {
    int lsfg = 0, pre = 0; char done[1024]; size_t dn = 0; done[0] = 0;
    pthread_mutex_lock(&g_emu);
    static typeof(g_pend[0]) p[NOVR];   // apply runs under the submit lock: one caller at a time
    int n = g_npend; memcpy(p, g_pend, (size_t)n * sizeof p[0]); g_npend = 0;
    for (int i = 0; i < n; i++) {
        int j = 0; while (j < g_novr && strcmp(g_ovr[j].key, p[i].key)) j++;
        if (j == g_novr) { if (g_novr == NOVR) { VRED("ctl: more than %d live switches: %s dropped", NOVR, p[i].key); continue; } g_novr++; }
        snprintf(g_ovr[j].key, sizeof g_ovr[j].key, "%s", p[i].key); snprintf(g_ovr[j].val, sizeof g_ovr[j].val, "%s", p[i].val); g_ovr[j].set = p[i].val[0] != 0;
        if (p[i].val[0]) setenv(p[i].key, p[i].val, 1); else unsetenv(p[i].key);
        lsfg |= !strncmp(p[i].key, "DM_LSFG", 7) && strcmp(p[i].key, "DM_LSFG_DLL"); pre |= !strcmp(p[i].key, "DM_VK_PRETRANSFORM");
        const char *kind = in(g_restart, p[i].key) ? "restart only: takes effect at the next start" : in(g_live, p[i].key) ? "live"
                         : !strncmp(p[i].key, "DM_VK_", 6) ? "not in the live table: live where its reader uses VKA_ENV" : "set in the environment (live where the host reads it at use)";
        VLOG("ctl: env %s=%s applied at frame %llu (%s)", p[i].key, p[i].val, (unsigned long long)vka_frame(), kind);
        dn += (size_t)snprintf(done + dn, dn < sizeof done ? sizeof done - dn : 0, "%s%s", dn ? " " : "", p[i].key); if (dn >= sizeof done) dn = sizeof done - 1;
    }
    pthread_mutex_unlock(&g_emu);
    atomic_fetch_add(&vka_env_gen, 1);
    (void)pre;   // the present reads DM_VK_PRETRANSFORM through VKA_ENV and makes a new swapchain when it changed (vka_present.c)
    if (lsfg) nxhost_framegen_set(lsfg_int("DM_LSFG", 0) != 0, lsfg_int("DM_LSFG_MULT", 2), lsfg_int("DM_LSFG_TARGET", 0), lsfg_int("DM_LSFG_FLOW", 75));
}
void vka_ctl_sub(void) {   // mtl_sub_begin: the submit lock is held, no submission is being recorded
    if (!atomic_load_explicit(&g_pending, memory_order_relaxed) || vka_frame() == atomic_load(&g_pend_frame)) return;
    atomic_store(&g_pending, 0);
    apply();
}

// ---------- Frame statistics: capture and stats ----------
extern uint64_t vka_n_rp, vka_n_bar;   // vka_draw.c, vka_tex.c
void vka_exec_counts(uint64_t *ops, uint64_t *subs, uint64_t *rec_ns);   // vka_exec.c
double mtl_gpu_ms(uint64_t f);
enum { NREC = 128 };
typedef struct { uint64_t frame, t, draws, skipped, disp, rp, bar, ops, subs, rec_ns; } frec;   // counters at the start of frame
static frec g_rec[NREC];
static _Atomic long g_capture; static uint64_t g_cap_from, g_cap_to; static _Atomic int g_stats;
static void snap(frec *r, uint64_t f) {
    r->frame = f; r->t = now_ns(); uint64_t pipes; vka_draw_stats(&r->draws, &r->skipped, &r->disp, &pipes);
    r->rp = vka_n_rp; r->bar = vka_n_bar; vka_exec_counts(&r->ops, &r->subs, &r->rec_ns);
}
static void cap_line(uint64_t f) {   // frame f, once its GPU time is in (8 frames on)
    const frec *a = &g_rec[f % NREC], *b = &g_rec[(f + 1) % NREC];
    if (a->frame != f || b->frame != f + 1) return;
    VLOG("ctl: capture frame %llu: %.2f ms, %llu draws (%llu skipped), %llu dispatches, %llu render passes, %llu full barriers, %llu clears / copies, %llu submissions; recording %.2f ms, GPU %.2f ms",
         (unsigned long long)f, (b->t - a->t) / 1e6, (unsigned long long)(b->draws - a->draws), (unsigned long long)(b->skipped - a->skipped), (unsigned long long)(b->disp - a->disp),
         (unsigned long long)(b->rp - a->rp), (unsigned long long)(b->bar - a->bar), (unsigned long long)(b->ops - a->ops), (unsigned long long)(b->subs - a->subs),
         (b->rec_ns - a->rec_ns) / 1e6, mtl_gpu_ms(f));
}
static void stats_line(uint64_t f) {
    int n = 60; while (n > 1 && g_rec[(f - (uint64_t)n) % NREC].frame != f - (uint64_t)n) n--;
    const frec *a = &g_rec[(f - (uint64_t)n) % NREC], *b = &g_rec[f % NREC];
    double k = n, gpu = 0; int ng = 0;
    for (uint64_t i = f - (uint64_t)n; i < f; i++) { double g = mtl_gpu_ms(i); if (g >= 0) { gpu += g; ng++; } }
    float fps[5] = {0}; nxhost_fps(fps, 5); uint64_t peak, placed = vka_mem_placed_bytes(&peak), img_gw; int img_n; uint64_t img = vka_tex_mem(&img_n, &img_gw);
    char sw[600] = ""; size_t sn = 0;
    pthread_mutex_lock(&g_emu);
    for (int i = 0; i < g_novr && sn < sizeof sw; i++) sn += (size_t)snprintf(sw + sn, sizeof sw - sn, " %s=%s", g_ovr[i].key, g_ovr[i].set ? g_ovr[i].val : "(unset)");
    pthread_mutex_unlock(&g_emu);
    VLOG("ctl: stats at frame %llu, last %d frames: %.1f fps (%.1f presented), %.2f ms a frame; per frame %.0f draws (%.0f skipped), %.0f dispatches, %.0f render passes, %.0f full barriers, "
         "%.0f clears / copies, %.1f submissions, recording %.2f ms, GPU %.2f ms (%d frames); placed memory %.0f MB (peak %.0f); images %.0f MB (%d; GPU-written %.0f MB, from storage %.0f MB); switches set live:%s",
         (unsigned long long)f, n, fps[0], fps[1], (b->t - a->t) / 1e6 / k, (b->draws - a->draws) / k, (b->skipped - a->skipped) / k, (b->disp - a->disp) / k, (b->rp - a->rp) / k,
         (b->bar - a->bar) / k, (b->ops - a->ops) / k, (b->subs - a->subs) / k, (b->rec_ns - a->rec_ns) / 1e6 / k, ng ? gpu / ng : -1.0, ng, placed / 1e6, peak / 1e6, img / 1e6, img_n, img_gw / 1e6, (img - img_gw) / 1e6, sn ? sw : " none");
    { char ab[400]; vka_tex_alias_stats(ab, sizeof ab); VLOG("ctl: %s", ab); }
    { char im[600]; vka_imem_stats(im, sizeof im); VLOG("ctl: %s", im); }
    { char bs[1000]; vka_tex_big_stats(bs, sizeof bs); if (bs[0]) VLOG("ctl: %s", bs); }
    { char ss[300]; vka_tex_soft_stats(ss, sizeof ss); VLOG("ctl: %s", ss); }
}
void vka_ctl_frame(uint64_t f) {   // mtl_set_frame: frame f begins
    snap(&g_rec[f % NREC], f);
    long c = atomic_load_explicit(&g_capture, memory_order_relaxed);
    if (c > 0) { if (!g_cap_from) { g_cap_from = f; g_cap_to = f + (uint64_t)c; VLOG("ctl: capture of frames %llu-%llu begins", (unsigned long long)f, (unsigned long long)(g_cap_to - 1)); } atomic_store(&g_capture, 0); }
    if (g_cap_from && f >= 9 && f - 9 >= g_cap_from) {   // GPU time is in 8 frames late
        cap_line(f - 9);
        if (f - 9 + 1 >= g_cap_to) { VLOG("ctl: capture done"); g_cap_from = g_cap_to = 0; }
    }
    if (atomic_exchange_explicit(&g_stats, 0, memory_order_relaxed)) stats_line(f);
}

// ---------- Screenshot ----------
// The present thread copies the window texture's crop into a host-visible buffer in the present's command buffer (vka_ctl_shot_record), waits for that submission and hands the
// texels to a thread that writes the PNG (zlib, the NDK's)
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static char g_shot_path[1024]; static _Atomic int g_shot_want;
static VkBuffer g_sbuf; static VkDeviceMemory g_smem; static void *g_smap; static VkDeviceSize g_ssize;
static struct { int on, w, h, fmt, flip; uint64_t frame; } g_rec_shot;
static char g_ctl_dir[1024];
static void ctl_dir(void) {
    if (g_ctl_dir[0]) return;
    const char *c = getenv("DM_CTL"); snprintf(g_ctl_dir, sizeof g_ctl_dir, "%s", c && *c ? c : getenv("HOME") ? getenv("HOME") : "/data/local/tmp/x");
    char *s = strrchr(g_ctl_dir, '/'); if (s) *s = 0;
}
void dm_gfx_shot_at(long k, const char *path) {   // the Mac's "shot <path>" too (k: frames ahead; the next frame here)
    (void)k; pthread_mutex_lock(&g_smu); snprintf(g_shot_path, sizeof g_shot_path, "%s", path ? path : ""); pthread_mutex_unlock(&g_smu); atomic_store(&g_shot_want, 1);
}
static int shot_buf(VkDeviceSize n) {
    if (g_ssize >= n) return 1;
    if (g_sbuf) { vk.vkDestroyBuffer(V.dev, g_sbuf, NULL); vk.vkFreeMemory(V.dev, g_smem, NULL); g_sbuf = VK_NULL_HANDLE; g_smem = VK_NULL_HANDLE; g_ssize = 0; }
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &g_sbuf)) return 0;
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, g_sbuf, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &g_smem) || vk.vkBindBufferMemory(V.dev, g_sbuf, g_smem, 0) || vk.vkMapMemory(V.dev, g_smem, 0, VK_WHOLE_SIZE, 0, &g_smap)) {
        if (g_smem) vk.vkFreeMemory(V.dev, g_smem, NULL);
        vk.vkDestroyBuffer(V.dev, g_sbuf, NULL); g_sbuf = VK_NULL_HANDLE; g_smem = VK_NULL_HANDLE; return 0;
    }
    g_ssize = n; return 1;
}
static int shot_bpp(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
    }
}
void vka_ctl_shot_record(VkCommandBuffer cb, vka_img *im, int cx, int cy, int cw, int ch) {   // present thread, before vkEndCommandBuffer
    g_rec_shot.on = 0;
    if (!atomic_load_explicit(&g_shot_want, memory_order_relaxed) || !im || !im->inited) return;
    atomic_store(&g_shot_want, 0);
    int bpp = shot_bpp(im->f.vk);
    if (!bpp || im->samples != 1 || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { VLOG("ctl: screenshot: the window texture's format %d is not one the writer takes (RGBA8, BGRA8, RGB10A2, RGBA16F)", im->f.vk); return; }
    if (cx < 0 || cy < 0 || cw <= 0 || ch <= 0 || (uint32_t)(cx + cw) > im->w || (uint32_t)(cy + ch) > im->h) { cx = cy = 0; cw = (int)im->w; ch = (int)im->h; }
    if (!shot_buf((VkDeviceSize)cw * ch * bpp)) { VLOG("ctl: screenshot: no host-visible buffer of %d x %d", cw, ch); return; }
    vka_barrier(cb);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {cx, cy, 0}, {(uint32_t)cw, (uint32_t)ch, 1}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g_sbuf, 1, &r);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    g_rec_shot.on = 1; g_rec_shot.w = cw; g_rec_shot.h = ch; g_rec_shot.fmt = im->f.vk; g_rec_shot.flip = vka_origin_ll(); g_rec_shot.frame = vka_frame();
}
typedef struct { uint8_t *rgb; int w, h; char path[1100]; uint64_t frame; } shot_job;
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; put32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4); if (n) c = crc32(c, d, n); uint8_t cc[4]; put32(cc, (uint32_t)c); fwrite(cc, 1, 4, f);
}
static void *shot_main(void *arg) {
    shot_job *j = arg; pthread_setname_np(pthread_self(), "vk-shot");
    size_t row = (size_t)j->w * 3 + 1, raw = row * (size_t)j->h; uLongf zn = compressBound((uLong)raw);
    uint8_t *flt = malloc(raw), *z = malloc(zn); FILE *f = NULL;
    if (flt && z) {
        for (int y = 0; y < j->h; y++) { flt[row * y] = 0; memcpy(flt + row * y + 1, j->rgb + (size_t)y * j->w * 3, (size_t)j->w * 3); }
        if (compress2(z, &zn, flt, (uLong)raw, 3) == Z_OK && (f = fopen(j->path, "wb"))) {
            static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}; fwrite(sig, 1, 8, f);
            uint8_t ih[13]; put32(ih, (uint32_t)j->w); put32(ih + 4, (uint32_t)j->h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
            chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)zn); chunk(f, "IEND", NULL, 0);
            fclose(f); chmod(j->path, 0644);
            VLOG("ctl: screenshot of frame %llu, %dx%d → %s", (unsigned long long)j->frame, j->w, j->h, j->path);
        } else VLOG("ctl: screenshot: cannot write %s", j->path);
    }
    free(flt); free(z); free(j->rgb); free(j); return NULL;
}
static uint8_t half8(uint16_t h) {   // an RGBA16F channel, clamped to [0, 1] and sRGB-encoded roughly (gamma 1/2.2)
    uint32_t e = h >> 10 & 31, m = h & 1023; if (h & 0x8000) return 0;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 1.0f : (1.0f + m / 1024.0f) * (float)(1u << e) / 32768.0f;
    if (!(v > 0)) return 0; if (v >= 1) return 255;
    return (uint8_t)(powf(v, 1 / 2.2f) * 255 + 0.5f);
}
void vka_ctl_shot_done(VkFence fence) {   // present thread, after the present's submission
    if (!g_rec_shot.on) return;
    g_rec_shot.on = 0;
    if (vk.vkWaitForFences(V.dev, 1, &fence, VK_TRUE, 2000000000ull)) { VLOG("ctl: screenshot: the present's copy did not finish in 2 s"); return; }
    int w = g_rec_shot.w, h = g_rec_shot.h, fmt = g_rec_shot.fmt, swap_rb = vka_swap_rb();
    shot_job *j = calloc(1, sizeof *j); uint8_t *rgb = j ? malloc((size_t)w * h * 3) : NULL;
    if (!rgb) { free(j); return; }
    for (int y = 0; y < h; y++) {
        int sy = g_rec_shot.flip ? h - 1 - y : y; uint8_t *d = rgb + (size_t)y * w * 3;
        if (fmt == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const uint16_t *s = (const uint16_t *)g_smap + (size_t)sy * w * 4;
            for (int x = 0; x < w; x++) {
                uint8_t r = half8(s[4 * x]), g = half8(s[4 * x + 1]), b = half8(s[4 * x + 2]);
                d[3 * x] = swap_rb ? b : r; d[3 * x + 1] = g; d[3 * x + 2] = swap_rb ? r : b;
            }
        } else {
            const uint8_t *s = (const uint8_t *)g_smap + (size_t)sy * w * 4;
            for (int x = 0; x < w; x++, s += 4) {
                if (fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                    uint32_t p; memcpy(&p, s, 4);
                    uint8_t r = (uint8_t)((p & 1023) >> 2), g = (uint8_t)((p >> 10 & 1023) >> 2), b = (uint8_t)((p >> 20 & 1023) >> 2);
                    d[3 * x] = swap_rb ? b : r; d[3 * x + 1] = g; d[3 * x + 2] = swap_rb ? r : b;
                } else if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) {
                    d[3 * x] = swap_rb ? s[0] : s[2]; d[3 * x + 1] = s[1]; d[3 * x + 2] = swap_rb ? s[2] : s[0];
                } else {
                    d[3 * x] = swap_rb ? s[2] : s[0]; d[3 * x + 1] = s[1]; d[3 * x + 2] = swap_rb ? s[0] : s[2];
                }
            }
        }
    }
    j->rgb = rgb; j->w = w; j->h = h; j->frame = g_rec_shot.frame;
    pthread_mutex_lock(&g_smu);
    if (g_shot_path[0]) snprintf(j->path, sizeof j->path, "%s", g_shot_path);
    else { ctl_dir(); snprintf(j->path, sizeof j->path, "%s/ctl-shot-%llu.png", g_ctl_dir, (unsigned long long)j->frame); }
    pthread_mutex_unlock(&g_smu);
    pthread_t th; if (pthread_create(&th, NULL, shot_main, j)) { free(rgb); free(j); return; }
    pthread_detach(th);
}

// ---------- Commands ----------
int dm_gfx_ctl(const char *line) {   // the control thread (hle_hid.c ctl_main): 1 = a replay command, taken
    if (!strncmp(line, "capture", 7) && (!line[7] || line[7] == ' ')) {
        long n = line[7] ? atol(line + 8) : 60; if (n <= 0) n = 60; if (n > NREC - 16) n = NREC - 16;
        if (g_cap_from) { VLOG("ctl: capture: one is running (frames %llu-%llu)", (unsigned long long)g_cap_from, (unsigned long long)(g_cap_to - 1)); return 1; }
        atomic_store(&g_capture, n); VLOG("ctl: capture of the next %ld frames queued", n); return 1;
    }
    if (!strncmp(line, "screenshot", 10) && (!line[10] || line[10] == ' ')) {
        const char *p = line[10] ? line + 11 : ""; while (*p == ' ') p++;
        dm_gfx_shot_at(1, p); VLOG("ctl: screenshot of the next frame queued%s%s", *p ? " → " : "", p); return 1;
    }
    if (!strcmp(line, "stats")) { atomic_store(&g_stats, 1); return 1; }
    return 0;
}
#endif
