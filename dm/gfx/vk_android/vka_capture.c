// DM_NVN_CAPTURE on Android: one frame captured at the replay boundary (nvnQueueSubmitCommands → mtl_exec) into one file, for the replayer on the phone (vka_replay.c,
// nxhost_replay; android/TESTING.md "Frame capture and replay") to replay against the Vulkan replay without the game. It is dm/gfx/nvn/nvn_capture.inc (the Mac's) with the Vulkan
// replay's tables: the pools (vka_mem.c), the texture and sampler registries and the images by serial (vka_tex.c), and the shader library's binding records for the interface hash.
// The file format is nvn_capture.h's, written uncompressed (no LZFSE here). What a frame holds, and how each later submit adds what changed, is described in nvn_capture.inc.
// Images whose content only the GPU has (render targets, copy and clear destinations, and images filled once from storage that may have changed since) are read back; images that
// follow their storage (tracked) are made again from the pools at replay. Each capture step waits for the GPU first; that changes the frame's timing, not what it renders.
//   DM_NVN_CAPTURE=<frame>:1:<path>      the frame-th present (counted from the first, as DM_NVN_SHOT counts them)
//   DM_NVN_CAPTURE=@<seconds>:1:<path>   the first frame that starts after that many seconds of the host's run (scripted runs on the phone, whose frame rate varies)
// The path's folder is made when missing. A capture is game data: it stays on the phone or in the game's work folder, never in the repository
#if defined(__ANDROID__)
#include "vka_capture.h"
#include "../nvn/nvn_hmap.h"
#include "../../host/plat/plat.h"
#include <errno.h>
#include <math.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define CLOG(...) LOG("DM_NVN_CAPTURE: " __VA_ARGS__)
#define CRED(...) LOG("[RED] DM_NVN_CAPTURE: " __VA_ARGS__)

// ---------- One-off GPU work ----------
static VkCommandPool g_ocp; static VkCommandBuffer g_ocb; static pthread_mutex_t g_omu = PTHREAD_MUTEX_INITIALIZER;
VkCommandBuffer vcap_cb_begin(void) {
    if (!vka_init()) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_omu);
    if (!g_ocp) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf};
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vk.vkCreateCommandPool(V.dev, &pi, NULL, &g_ocp) || ((ai.commandPool = g_ocp), vk.vkAllocateCommandBuffers(V.dev, &ai, &g_ocb))) { g_ocp = VK_NULL_HANDLE; pthread_mutex_unlock(&g_omu); return VK_NULL_HANDLE; }
    }
    vk.vkResetCommandBuffer(g_ocb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(g_ocb, &bi);
    vka_barrier(g_ocb);   // after everything submitted before it
    return g_ocb;         // g_omu stays held until vcap_cb_run
}
int vcap_cb_run(VkCommandBuffer cb) {
    vka_barrier(cb);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    int ok = 0; VkFence f = VK_NULL_HANDLE; VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!vk.vkEndCommandBuffer(cb) && !vk.vkCreateFence(V.dev, &fi, NULL, &f)) {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        pthread_mutex_lock(&V.qmu); VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, f); pthread_mutex_unlock(&V.qmu);
        ok = !r && !vk.vkWaitForFences(V.dev, 1, &f, VK_TRUE, 30000000000ull);
        if (!ok) LOG("[RED] vk: capture / replay copy: submit %d, fence not reached in 30 s", r);
        if (ok || r) vk.vkDestroyFence(V.dev, f, NULL);   // a fence the GPU may still signal is left alone
    }
    pthread_mutex_unlock(&g_omu);
    return ok;
}
int vcap_hbuf_make(vcap_hbuf *h, VkDeviceSize n) {
    memset(h, 0, sizeof *h); if (!n) n = 16;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &h->b)) return 0;
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, h->b, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    void *p = NULL;
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &h->m) || vk.vkBindBufferMemory(V.dev, h->b, h->m, 0) || vk.vkMapMemory(V.dev, h->m, 0, VK_WHOLE_SIZE, 0, &p)) { vcap_hbuf_free(h); return 0; }
    h->p = p; h->n = n; return 1;
}
void vcap_hbuf_free(vcap_hbuf *h) { if (h->b) vk.vkDestroyBuffer(V.dev, h->b, NULL); if (h->m) vk.vkFreeMemory(V.dev, h->m, NULL); memset(h, 0, sizeof *h); }

// ---------- Image contents ----------
static int plane_block(const vka_img *im, int pl, uint32_t *bw, uint32_t *bh, VkImageAspectFlags *asp) {   // bytes per block of a plane (0 = unknown)
    *bw = *bh = 1;
    switch (im->f.kind) {
    case 3: *asp = VK_IMAGE_ASPECT_DEPTH_BIT; return pl ? 0 : im->f.vk == VK_FORMAT_D16_UNORM ? 2 : 4;
    case 4: *asp = VK_IMAGE_ASPECT_STENCIL_BIT; return pl ? 0 : 1;
    case 5: *asp = pl ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT; return pl ? 1 : 4;   // the depth aspect of D24S8 and D32S8 copies as 4 bytes a texel
    default: *asp = VK_IMAGE_ASPECT_COLOR_BIT; if (pl) return 0; *bw = im->f.bw ? im->f.bw : 1; *bh = im->f.bh ? im->f.bh : 1; return im->f.bpb;
    }
}
uint32_t vcap_subs(const vka_img *im, cap_tsub **subs, uint64_t *total) {
    *subs = NULL; *total = 0;
    if (!im || im->samples > 1 || !im->img) return 0;
    uint32_t np = im->f.kind == 5 ? 2 : 1, n = 0, layers = im->layers ? im->layers : 1, levels = im->levels ? im->levels : 1;
    cap_tsub *s = calloc((size_t)np * levels * layers, sizeof *s);
    for (uint32_t pl = 0; pl < np; pl++) for (uint32_t lv = 0; lv < levels; lv++) for (uint32_t ly = 0; ly < layers; ly++) {
        uint32_t w = MAX(im->w >> lv, 1u), h = MAX(im->h >> lv, 1u), d = im->d.target == NVN_TEXTURE_TARGET_3D ? MAX(im->depth >> lv, 1u) : 1, bw, bh; VkImageAspectFlags a;
        uint64_t bpb = (uint64_t)plane_block(im, (int)pl, &bw, &bh, &a);
        if (!bpb) { free(s); return 0; }
        uint64_t row = (uint64_t)((w + bw - 1) / bw) * bpb, img = row * ((h + bh - 1) / bh);
        s[n++] = (cap_tsub){lv, ly, w, h, d, pl, row, img, img * d}; *total += img * d;
    }
    *subs = s; return n;
}
VkDeviceSize vcap_sub_off(const cap_tsub *subs, uint32_t i) { VkDeviceSize o = 0; for (uint32_t j = 0; j < i; j++) o += (subs[j].bytes + 15) & ~15ull; return o; }
void vcap_img_copy(VkCommandBuffer cb, vka_img *im, const cap_tsub *subs, uint32_t nsub, VkBuffer b, int to_image) {
    VkBufferImageCopy *c = calloc(nsub ? nsub : 1, sizeof *c); VkDeviceSize off = 0;
    for (uint32_t i = 0; i < nsub; i++) {
        uint32_t bw, bh; VkImageAspectFlags a; plane_block(im, (int)subs[i].plane, &bw, &bh, &a);
        c[i] = (VkBufferImageCopy){off, 0, 0, {a, subs[i].level, subs[i].slice, 1}, {0, 0, 0}, {subs[i].w, subs[i].h, subs[i].d}};
        off += (subs[i].bytes + 15) & ~15ull;
    }
    if (to_image) vka_tex_own(im, cb);   // (a zero-shared image gets memory of its own first)
    vka_barrier(cb);
    if (to_image) vk.vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, nsub, c);
    else vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, nsub, c);
    vka_barrier(cb);
    free(c);
}
uint8_t *vcap_img_level0(vka_img *im, size_t *n) {
    cap_tsub *all; uint64_t total; uint32_t ns = vcap_subs(im, &all, &total);
    if (!ns || !im->inited) { free(all); return NULL; }
    cap_tsub s = all[0]; free(all);   // plane 0, level 0, layer 0
    vcap_hbuf h; if (!vcap_hbuf_make(&h, s.bytes)) return NULL;
    VkCommandBuffer cb = vcap_cb_begin(); uint8_t *out = NULL;
    if (cb) { vcap_img_copy(cb, im, &s, 1, h.b, 0); if (vcap_cb_run(cb) && (out = malloc((size_t)s.bytes))) { memcpy(out, h.p, (size_t)s.bytes); *n = (size_t)s.bytes; } }
    vcap_hbuf_free(&h);
    return out;
}

// ---------- PNG ----------
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, b;
    if (!e) { if (!m) b = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } b = s | e << 23 | (m & 1023) << 13; } }
    else if (e == 31) b = s | 0x7f800000u | m << 13; else b = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &b, 4); return f;
}
static float uf(uint32_t v, int mb) { uint32_t e = v >> mb, m = v & ((1u << mb) - 1); if (e == 31) return 65504.f; return e ? ldexpf(1.f + m / (float)(1u << mb), (int)e - 15) : ldexpf(m / (float)(1u << mb), -14); }
static uint8_t lin8(float x) { if (!(x > 0)) return 0; if (x >= 1) return 255; float s = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f; return (uint8_t)(s * 255 + 0.5f); }
static uint8_t un8(float x) { return !(x > 0) ? 0 : x >= 1 ? 255 : (uint8_t)(x * 255 + 0.5f); }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, h + 4, 4); if (n) c = crc32(c, d, n); uint8_t cc[4]; be32(cc, (uint32_t)c); fwrite(cc, 1, 4, f);
}
int vcap_png(const char *path, const uint8_t *px, uint32_t w, uint32_t h, VkFormat fmt, int flip) {
    size_t rowb = (size_t)w * 4 + 1; uint8_t *raw = malloc(rowb * h); if (!raw) return 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *o = raw + (size_t)y * rowb; *o++ = 0; uint32_t sy = flip ? h - 1 - y : y;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            size_t i = (size_t)sy * w + x; uint32_t v; uint8_t r = 0, g = 0, b = 0;
            switch (fmt) {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UINT: r = px[4 * i]; g = px[4 * i + 1]; b = px[4 * i + 2]; break;
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: b = px[4 * i]; g = px[4 * i + 1]; r = px[4 * i + 2]; break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: memcpy(&v, px + 4 * i, 4); r = (uint8_t)((v & 1023) >> 2); g = (uint8_t)((v >> 10 & 1023) >> 2); b = (uint8_t)((v >> 20 & 1023) >> 2); break;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: memcpy(&v, px + 4 * i, 4); r = lin8(uf(v & 2047, 6)); g = lin8(uf(v >> 11 & 2047, 6)); b = lin8(uf(v >> 22, 5)); break;
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: { memcpy(&v, px + 4 * i, 4); float sc = ldexpf(1.0f, (int)(v >> 27) - 24); r = lin8((float)(v & 511) * sc); g = lin8((float)(v >> 9 & 511) * sc); b = lin8((float)(v >> 18 & 511) * sc); break; }
            case VK_FORMAT_R16G16B16A16_SFLOAT: { uint16_t c[4]; memcpy(c, px + 8 * i, 8); r = lin8(half_f(c[0])); g = lin8(half_f(c[1])); b = lin8(half_f(c[2])); break; }
            case VK_FORMAT_R16G16_SFLOAT: { uint16_t c[2]; memcpy(c, px + 4 * i, 4); r = lin8(half_f(c[0])); g = lin8(half_f(c[1])); break; }
            case VK_FORMAT_R16_SFLOAT: { uint16_t c; memcpy(&c, px + 2 * i, 2); r = g = b = lin8(half_f(c)); break; }
            case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: { float c; memcpy(&c, px + 4 * i, 4); r = g = b = un8(c); break; }
            case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: memcpy(&v, px + 4 * i, 4); r = g = b = (uint8_t)((v & 0xffffff) >> 16); break;
            case VK_FORMAT_D16_UNORM: case VK_FORMAT_R16_UNORM: { uint16_t c; memcpy(&c, px + 2 * i, 2); r = g = b = (uint8_t)(c >> 8); break; }
            case VK_FORMAT_R8_UNORM: case VK_FORMAT_S8_UINT: r = g = b = px[i]; break;
            case VK_FORMAT_R8G8_UNORM: r = px[2 * i]; g = px[2 * i + 1]; break;
            default: free(raw); return 0;
            }
            o[0] = r; o[1] = g; o[2] = b; o[3] = 255;   // opaque: a render target's alpha is often 0
        }
    }
    uLongf zn = compressBound((uLong)(rowb * h)); uint8_t *z = malloc(zn); int ok = 0;
    FILE *f = z ? fopen(path, "wb") : NULL;
    if (f && compress2(z, &zn, raw, (uLong)(rowb * h), 1) == Z_OK) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}; uint8_t ih[13];
        be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
        fwrite(sig, 1, 8, f); png_chunk(f, "IHDR", ih, 13); png_chunk(f, "IDAT", z, (uint32_t)zn); png_chunk(f, "IEND", NULL, 0);
        ok = !ferror(f);
    }
    if (f) ok &= !fclose(f);
    free(z); free(raw);
    return ok;
}

int vcap_digest(const tex_t *t, uint8_t md[32], uint64_t *bytes, uint32_t *w, uint32_t *hh, uint32_t *fmt, const char *png, int flip, cap_buf *pix) {
    mtl_finish();
    vka_img *im = t && t->serial ? vka_tex_acquire(t->serial) : NULL; if (!im) return 0;
    size_t n = 0; uint8_t *px = vcap_img_level0(im, &n);
    if (px) {
        plat_sha256(px, n, md); *bytes = n; *w = im->w; *hh = im->h; *fmt = (uint32_t)im->f.vk;
        if (pix) cap_buf_put(pix, px, n);
        if (png && *png && !vcap_png(png, px, im->w, im->h, im->f.vk, flip)) LOG("[vk] %s: no picture of VkFormat %d", png, im->f.vk);
        free(px);
    }
    vka_tex_drop(im);
    return px != NULL;
}

void vcap_iface(const prog_t *p, uint64_t out[6]) {
    memset(out, 0, 6 * sizeof *out);
    vka_progsh ps; if (!p || !vka_prog_shaders(p, &ps)) return;
    for (int st = 0; st < 6; st++) {
        const vka_shader *sh = ps.sh[st]; if (!sh || !sh->e) continue;
        plat_sha256_t c; uint8_t md[32]; plat_sha256_init(&c);
        uint64_t hd[3] = {sh->key, sh->e->bind_count, sh->e->vin_types}; plat_sha256_update(&c, hd, sizeof hd);
        plat_sha256_update(&c, &sh->e->vin_mask, sizeof sh->e->vin_mask);
        if (sh->b && sh->e->bind_count) plat_sha256_update(&c, sh->b, (size_t)sh->e->bind_count * sizeof *sh->b);
        plat_sha256_final(md, &c); memcpy(&out[st], md, 8); out[st] |= 1;
    }
}
void vcap_shlib_path(char *out, size_t n) {
    const char *e = getenv("DM_VK_SHLIB"), *l = vka_shader_lib_path();
    if (l && *l) snprintf(out, n, "%s", l);   // the one loaded: an older library of the same ABI while the update runs
    else if (e && *e) snprintf(out, n, "%s", e);
    else { const char *dir = dm_module_dir(); snprintf(out, n, "%s/shaders/" DMSH_VK_DIR_PREFIX "%s/" DMSH_VK_LIB_FILE, dir && *dir ? dir : ".", DMSH_VK_CHAIN); }
}

// ---------- The capture ----------
static int g_cap_frame = -1;            // -1: not parsed yet, 0: off (or not chosen yet with @seconds)
static double g_cap_at_s;               // @seconds: the frame is chosen at the first present after that time
static char g_cap_path[1024];
static int g_cap_active, g_cap_done, g_cap_failed;
static uint32_t g_cap_submit, g_cap_cur_submit;
static uint64_t g_cap_presents;
static cap_writer g_cap_w;
static uint64_t g_cap_gone, g_cap_nostore, g_cap_ns, g_cap_unmapped, g_cap_segs, g_cap_tex_bytes, g_cap_tex_n, g_cap_tex_skip;
typedef struct { uint64_t lo, size; uint32_t kind, pending; uint64_t *hash; } cap_region;
static cap_region *g_cap_reg; static uint32_t g_cap_nreg;
static uint32_t *g_cap_rid; static uint32_t g_cap_nrid;
static nvn_hmap g_cap_tex = {.klen = sizeof(tex_t)}, g_cap_prog = {.klen = 16}, g_cap_dpol = {.klen = 8 + sizeof(dpool_t)}, g_cap_sync = {.klen = 8}, g_cap_pool = {.klen = 8 + sizeof(pool_t)};
static nvn_hmap g_cap_seg = {.klen = 8}, g_cap_read = {.klen = 8}, g_cap_img = {.klen = 8};
static int g_cap_no_read;
static uint32_t g_cap_ntex, g_cap_nprog, g_cap_ndpol, g_cap_nsync, g_cap_npool;
static cap_buf g_cap_tobj, g_cap_pobj, g_cap_progb, g_cap_dpolb, g_cap_kind;
typedef struct { uint8_t ok, has_v, img; tex_t t; view_t v; } treg_seen;
static treg_seen *g_cap_treg0; static int g_cap_ntreg0; static samp_par *g_cap_sreg0; static uint8_t *g_cap_sreg0_ok; static int g_cap_nsreg0;
static char g_cap_where[96]; static const uint8_t *g_cap_where_base, *g_cap_orig;
#define CAP_WHERE(base, ...) (snprintf(g_cap_where, sizeof g_cap_where, __VA_ARGS__), g_cap_where_base = (const uint8_t *)(base))

static int nvn_cap_enabled(void) {
    if (g_cap_frame >= 0) return (g_cap_frame > 0 || g_cap_at_s > 0) && !g_cap_done;
    g_cap_frame = 0;
    const char *e = getenv("DM_NVN_CAPTURE"); int f = 0, n = 0, used = 0; double s = 0;
    if (!e || !*e) return 0;
    if (e[0] == '@') { if (sscanf(e + 1, "%lf:%d:%n", &s, &n, &used) >= 2 && used && s > 0) used++; else used = 0; }
    else if (sscanf(e, "%d:%d:%n", &f, &n, &used) < 2 || f < 1) used = 0;
    if (!used || !e[used]) { CRED("%s: expected <frame>:1:<path> or @<seconds>:1:<path>; capture off", e); return 0; }
    if (n != 1) CRED("%d frames asked, one is captured", n);
    strlcpy(g_cap_path, e + used, sizeof g_cap_path); g_cap_frame = f; g_cap_at_s = s;
    if (f) CLOG("capturing frame %d into %s", f, g_cap_path); else CLOG("capturing the first frame after %.1f s into %s", s, g_cap_path);
    return 1;
}
static uint64_t cap_hash_block(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, v; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void cap_fail(const char *why) { if (!g_cap_failed++) CRED("%s: the capture is incomplete", why); }

// ---------- Regions ----------
static int cap_reg_find(uint64_t a) {
    int lo = 0, hi = (int)g_cap_nreg - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < g_cap_reg[m].lo) hi = m - 1; else if (a >= g_cap_reg[m].lo + g_cap_reg[m].size) lo = m + 1; else return m; }
    return -1;
}
static void cap_reg_hash(cap_region *r, const uint8_t *p) {
    size_t nb = (size_t)((r->size + 4095) / 4096); r->hash = realloc(r->hash, nb * 8);
    for (size_t b = 0; b < nb; b++) { uint64_t o = (uint64_t)b * 4096; r->hash[b] = cap_hash_block(p + o, (size_t)MIN(4096, r->size - o)); }
}
static int cap_reg_add(uint64_t lo, uint64_t size, uint32_t kind) {
    int pos = 0; while (pos < (int)g_cap_nreg && g_cap_reg[pos].lo < lo) pos++;
    g_cap_reg = realloc(g_cap_reg, (g_cap_nreg + 1) * sizeof *g_cap_reg); g_cap_rid = realloc(g_cap_rid, (g_cap_nreg + 1) * sizeof *g_cap_rid);
    memmove(g_cap_reg + pos + 1, g_cap_reg + pos, (g_cap_nreg - pos) * sizeof *g_cap_reg); memmove(g_cap_rid + pos + 1, g_cap_rid + pos, (g_cap_nreg - pos) * sizeof *g_cap_rid);
    g_cap_reg[pos] = (cap_region){lo, size, kind, 1, NULL}; g_cap_rid[pos] = g_cap_nrid++; g_cap_nreg++;
    cap_reg_hash(&g_cap_reg[pos], (const uint8_t *)(uintptr_t)lo);
    return pos;
}
static const char *cap_where(const uint64_t *field) {
    static char b[128]; uint64_t off = (uint64_t)((const uint8_t *)field - g_cap_where_base);
    if (g_cap_where_base && off < (64u << 20)) snprintf(b, sizeof b, "%s, field +%llu", g_cap_where, (unsigned long long)off); else strlcpy(b, g_cap_where, sizeof b);
    return b;
}
static int cap_enc_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable;
    uint64_t v = *field; if (!v) return 1;
    int i = cap_reg_find(v);
    if (i < 0 && len && len <= (1u << 20) && nvn_host_readable(v, len)) {
        for (uint32_t k = 0; k < g_cap_nreg; k++) if (g_cap_reg[k].lo < v + len && v < g_cap_reg[k].lo + g_cap_reg[k].size) { cap_fail("a record's memory straddles a region");
            CRED("%#llx (%llu bytes) straddles region %#llx (%llu bytes): %s", (unsigned long long)v, (unsigned long long)len, (unsigned long long)g_cap_reg[k].lo, (unsigned long long)g_cap_reg[k].size, cap_where(field)); return 1; }
        i = cap_reg_add(v, len, 1);
    }
    if (i < 0) { if (g_cap_unmapped++ < 8) CRED("a record names %#llx (%llu bytes), outside every pool: kept as it is: %s", (unsigned long long)v, (unsigned long long)len, cap_where(field)); return 1; }
    *field = cap_addr(g_cap_rid[i], v - g_cap_reg[i].lo);
    return 1;
}

// ---------- Objects ----------
static int cap_enc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops g_cap_enc = {cap_enc_addr, cap_enc_obj};
static void cap_tex_read(const tex_t *t, int rec);
static uint64_t cap_pool_obj(const pool_t *p) {
    if (!p) return 0;
    uint8_t k[8 + sizeof(pool_t)]; memcpy(k, &p, 8); memcpy(k + 8, p, sizeof *p);
    void *have = nvn_hm_get(&g_cap_pool, k); if (have) return cap_obj(CAP_K_POOL, (uint64_t)(uintptr_t)have - 1);
    uint32_t idx = g_cap_npool++; nvn_hm_put(&g_cap_pool, k, (void *)(uintptr_t)(idx + 1));
    cap_pobj o = {idx, p->flags, (uint64_t)(uintptr_t)p->mem, p->size, p->vmode, 0};
    if (p->vmode == 2) cap_fail("a sparse virtual pool (page tables are not captured)");
    cap_enc_addr(NULL, &o.mem, 0, 0); cap_buf_put(&g_cap_pobj, &o, sizeof o);
    return cap_obj(CAP_K_POOL, idx);
}
// A texture's storage outside every live pool (a pool finalized while the texture stays registered, a render target whose storage was given back): written as none. Its image is
// found by serial and its content, the GPU's, is read back like any other; the storage address would name memory the replayer does not have (and the game may have reused)
static int cap_in_pool(const tex_t *t) {
    if (!t->mem) return 1;
    int r = cap_reg_find((uint64_t)(uintptr_t)t->mem);
    return r >= 0 && !g_cap_reg[r].kind && (uint64_t)(uintptr_t)t->mem + t->L.size <= g_cap_reg[r].lo + g_cap_reg[r].size;
}
static void cap_tex_enc(tex_t *t, const char *what) {
    if (!cap_in_pool(t)) { t->mem = NULL; t->pool = NULL; g_cap_nostore++; }
    char sv[sizeof g_cap_where]; const uint8_t *sb = g_cap_where_base; memcpy(sv, g_cap_where, sizeof sv);
    CAP_WHERE(t, "%s ser=%llu%s%s", what, (unsigned long long)t->serial, sb ? " in " : "", sb ? sv : "");
    cap_tex_fields(t, &g_cap_enc, NULL);
    memcpy(g_cap_where, sv, sizeof sv); g_cap_where_base = sb;
}
static uint64_t cap_tex_obj(const tex_t *src) {
    tex_t t = *src;
    void *have = nvn_hm_get(&g_cap_tex, &t); if (have) return cap_obj(CAP_K_TEX, (uint64_t)(uintptr_t)have - 1);
    uint32_t idx = g_cap_ntex++; nvn_hm_put(&g_cap_tex, &t, (void *)(uintptr_t)(idx + 1));
    cap_tex_read(&t, 1);
    cap_tex_enc(&t, "texture object");
    uint32_t hd[2] = {idx, 0}; cap_buf_put(&g_cap_tobj, hd, 8); cap_buf_put(&g_cap_tobj, &t, sizeof t);
    return cap_obj(CAP_K_TEX, idx);
}
static uint64_t cap_seg_get(uint64_t addr);
static int cap_enc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *field; if (!v && kind != CAP_K_SEG) return 1;
    switch (kind) {
        case CAP_K_SEG: *field = cap_obj(CAP_K_SEG, cap_seg_get(v)); return 1;
        case CAP_K_POOL: *field = cap_pool_obj((const pool_t *)(uintptr_t)v); return 1;
        case CAP_K_TEX:
            if (g_cap_orig && v >= (uint64_t)(uintptr_t)g_cap_orig && v + sizeof(tex_t) <= (uint64_t)(uintptr_t)g_cap_orig + size) {
                uint64_t off = v - (uint64_t)(uintptr_t)g_cap_orig; tex_t *in = (tex_t *)(payload + off);
                cap_tex_read(in, 1); cap_tex_enc(in, "texture snapshot"); *field = cap_obj(CAP_K_TEXIN, off); return 1;
            }
            *field = cap_tex_obj((const tex_t *)(uintptr_t)v); return 1;
        case CAP_K_PROG: {
            const prog_t *p = (const prog_t *)(uintptr_t)v; uint64_t k[2] = {v, p->serial};
            void *have = nvn_hm_get(&g_cap_prog, k); if (have) { *field = cap_obj(CAP_K_PROG, (uint64_t)(uintptr_t)have - 1); return 1; }
            uint32_t idx = g_cap_nprog++; nvn_hm_put(&g_cap_prog, k, (void *)(uintptr_t)(idx + 1));
            cap_prog o = {idx, p->nstage, p->serial, {{0}}, 0, {0}, {0}};
            for (unsigned s = 0; s < p->nstage && s < 6; s++) if (prog_key(p, s, o.key[s])) o.ok |= (uint8_t)(1u << s);
            vcap_iface(p, o.iface);
            cap_buf_put(&g_cap_progb, &o, sizeof o); *field = cap_obj(CAP_K_PROG, idx); return 1;
        }
        case CAP_K_DPOOL: {
            const dpool_t *d = (const dpool_t *)(uintptr_t)v; uint8_t k[8 + sizeof(dpool_t)]; memcpy(k, &v, 8); memcpy(k + 8, d, sizeof *d);
            void *have = nvn_hm_get(&g_cap_dpol, k); if (have) { *field = cap_obj(CAP_K_DPOOL, (uint64_t)(uintptr_t)have - 1); return 1; }
            uint32_t idx = g_cap_ndpol++; nvn_hm_put(&g_cap_dpol, k, (void *)(uintptr_t)(idx + 1));
            cap_dpol o = {idx, d->n, cap_pool_obj(d->pool), d->off, nvn_dpool_base(d, 1), nvn_dpool_base(d, 0)};
            cap_buf_put(&g_cap_dpolb, &o, sizeof o); *field = cap_obj(CAP_K_DPOOL, idx); return 1;
        }
        case CAP_K_SYNC: {
            void *have = nvn_hm_get(&g_cap_sync, &v); uint32_t idx = have ? (uint32_t)(uintptr_t)have - 1 : g_cap_nsync++;
            if (!have) nvn_hm_put(&g_cap_sync, &v, (void *)(uintptr_t)(idx + 1));
            *field = cap_obj(CAP_K_SYNC, idx); return 1;
        }
    }
    return 0;
}

// ---------- Segments ----------
static uint64_t *g_cap_segq; static uint32_t g_cap_nseg, g_cap_segq_cap;
static uint64_t cap_seg_get(uint64_t addr) {
    void *have = nvn_hm_get(&g_cap_seg, &addr); if (have) return (uint64_t)(uintptr_t)have - 1;
    uint32_t idx = g_cap_nseg++; nvn_hm_put(&g_cap_seg, &addr, (void *)(uintptr_t)(idx + 1));
    if (g_cap_nseg > g_cap_segq_cap) { g_cap_segq_cap = g_cap_segq_cap ? g_cap_segq_cap * 2 : 256; g_cap_segq = realloc(g_cap_segq, g_cap_segq_cap * 8); }
    g_cap_segq[idx] = addr; return idx;
}
static void cap_seg_write(uint32_t idx) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)g_cap_segq[idx]; size_t n = 0;
    for (;;) {
        op_hdr h; memcpy(&h, p + n, 8);
        if (h.size < 8 || (h.size & 7) || h.size > (64u << 20) || h.op >= OP_COUNT_) { cap_fail("a corrupt record in a submitted segment"); CRED("segment %p offset %zu: op %u size %u", p, n, h.op, h.size); break; }
        n += h.size;
        if (h.op == OP_END || h.op == OP_JUMP) break;
        if (n > (256u << 20)) { cap_fail("a segment longer than 256 MiB without END"); break; }
    }
    uint8_t *b = malloc(8 + n); uint32_t hd[2] = {idx, (uint32_t)n}; memcpy(b, hd, 8); memcpy(b + 8, p, n);
    for (size_t o = 0; o < n;) {
        op_hdr h; memcpy(&h, b + 8 + o, 8);
        g_cap_orig = p + o + 8; CAP_WHERE(b + 8 + o + 8, "op %u (%u bytes) at +%zu of segment %u", h.op, h.size, o, idx);
        if (!cap_rec_fields(h.op, b + 8 + o + 8, h.size, &g_cap_enc, NULL)) { cap_fail("a record the capture walk does not know"); CRED("op %u size %u not walked", h.op, h.size); }
        o += h.size;
    }
    g_cap_orig = NULL; CAP_WHERE(NULL, "-");
    cap_put(&g_cap_w, CAP_SEGM, g_cap_cur_submit, b, 8 + n, 0); free(b); g_cap_segs++;
}

// ---------- Images whose content only the GPU has ----------
// The images of textures the frame can reach (registered, or named by a record: rec). An image that follows its storage (tracked, not written by the GPU) is made again from
// the pools at replay, as the live run made it; one the CPU wrote is marked so (CAP_TK_CPU) to take the same path. Every other image's content is read back now (CAP_TPIX,
// pixfmt a VkFormat, pad bit 0 = the GPU wrote it): render targets, copy destinations, images filled once from storage. A texture without an image that would share a live
// one's (same storage and shape, vka_tex_get) takes that image's content
static void cap_tex_read(const tex_t *t, int rec) {
    if (g_cap_no_read || !t || !t->serial || t->magic != MAGIC('T', 'E', 'X', 0)) return;
    uintptr_t seen = (uintptr_t)nvn_hm_get(&g_cap_read, &t->serial);
    if (seen == 1 || (seen == 2 && !rec)) return;
    vka_img *im = vka_tex_acquire(t->serial); int shared = 0;
    if (!im && rec) { im = vka_cap_shared(t); shared = im != NULL; }
    nvn_hm_put(&g_cap_read, &t->serial, (void *)(uintptr_t)(!im && !rec ? 2 : 1));
    if (!seen) {
        int cpu = vka_cap_cpuw(t->serial) || (im && !shared && im->track && !im->gpu_owned);
        cap_tknd k = {*t, cpu ? CAP_TK_CPU : 0, 0};
        if (k.kind) { cap_tex_enc(&k.t, "texture kind"); cap_buf_put(&g_cap_kind, &k, sizeof k); }
    }
    if (!im) return;
    if (!im->inited || (im->track && !im->gpu_owned)) { vka_tex_drop(im); return; }
    uint64_t key = (uint64_t)(uintptr_t)im;
    if (nvn_hm_get(&g_cap_img, &key)) { vka_tex_drop(im); return; }
    nvn_hm_put(&g_cap_img, &key, (void *)1);
    cap_tsub *subs; uint64_t total; uint32_t nsub = vcap_subs(im, &subs, &total);
    if (!nsub) {
        if (g_cap_tex_skip++ < 8) CRED("texture ser=%llu %ux%u VkFormat %d samples %u: content not captured (%s)", (unsigned long long)t->serial, im->w, im->h, im->f.vk, im->samples, im->samples > 1 ? "multisampled" : "a format the capture cannot read back");
        vka_tex_drop(im); return;
    }
    vcap_hbuf hb; VkCommandBuffer cb;
    if (!vcap_hbuf_make(&hb, vcap_sub_off(subs, nsub) + 16)) { cap_fail("no readback buffer for a texture"); free(subs); vka_tex_drop(im); return; }
    if (!(cb = vcap_cb_begin())) { cap_fail("no command buffer for a texture readback"); vcap_hbuf_free(&hb); free(subs); vka_tex_drop(im); return; }
    vcap_img_copy(cb, im, subs, nsub, hb.b, 0);
    if (!vcap_cb_run(cb)) { cap_fail("a texture readback did not finish"); vcap_hbuf_free(&hb); free(subs); vka_tex_drop(im); return; }
    cap_tpix h = {*t, (uint32_t)im->f.vk, im->d.target, im->w, im->h, im->depth, im->levels, im->layers, im->samples, nsub, im->gpu_owned ? 1u : 0u};
    cap_tex_enc(&h.t, "texture contents");
    cap_buf c = {0}; cap_buf_put(&c, &h, sizeof h); cap_buf_put(&c, subs, nsub * sizeof *subs);
    VkDeviceSize off = 0; for (uint32_t i = 0; i < nsub; i++) { cap_buf_put(&c, hb.p + off, (size_t)subs[i].bytes); off += (subs[i].bytes + 15) & ~15ull; }
    cap_put(&g_cap_w, CAP_TPIX, g_cap_cur_submit, c.p, c.n, 0);
    g_cap_tex_bytes += total; g_cap_tex_n++;
    cap_buf_free(&c); free(subs); vcap_hbuf_free(&hb); vka_tex_drop(im);
}

// ---------- Registries ----------
static void cap_registries(uint32_t submit) {
    cap_buf tb = {0}, sb = {0}; tex_t *reads = NULL; int nreads = 0, nt = 0, ns = 0;
    tex_t t; view_t v; uint8_t has_v, img; samp_par sp;
    vka_cap_treg(-1, &t, &v, &has_v, &img, &nt); vka_cap_sreg(-1, &sp, &ns);
    if (g_cap_ntreg0 < nt) { g_cap_treg0 = realloc(g_cap_treg0, (size_t)nt * sizeof *g_cap_treg0); memset(g_cap_treg0 + g_cap_ntreg0, 0, (size_t)(nt - g_cap_ntreg0) * sizeof *g_cap_treg0); g_cap_ntreg0 = nt; }
    reads = malloc((size_t)MAX(nt, 1) * sizeof *reads);
    for (int i = 0; i < nt; i++) {
        memset(&t, 0, sizeof t); memset(&v, 0, sizeof v); has_v = img = 0;
        int ok = vka_cap_treg(i, &t, &v, &has_v, &img, NULL); treg_seen *o = &g_cap_treg0[i];
        if (ok == o->ok && (!ok || (has_v == o->has_v && img == o->img && !memcmp(&t, &o->t, sizeof t) && (!has_v || !memcmp(&v, &o->v, sizeof v))))) continue;
        *o = (treg_seen){(uint8_t)ok, has_v, img, t, v};
        cap_treg e = {i, (uint8_t)ok, has_v, 0, img, t, v};   // pad: 1 = RegisterImage
        // Storage in no live pool: the ID was registered before its pool was finalized (the Vulkan replay keeps such entries and their images, where nvn_mtl.m drops their views:
        // treg_drop_pool_views). gone = 1 says so; the entry stays registered at replay, without storage
        if (ok && !cap_in_pool(&t)) { e.gone = 1; g_cap_gone++; }   // kept registered (its image's content is captured by serial), without storage (cap_tex_enc)
        if (ok) reads[nreads++] = t;
        cap_buf_put(&tb, &e, sizeof e);
    }
    if (g_cap_nsreg0 < ns) { g_cap_sreg0 = realloc(g_cap_sreg0, (size_t)ns * sizeof *g_cap_sreg0); g_cap_sreg0_ok = realloc(g_cap_sreg0_ok, (size_t)ns); memset(g_cap_sreg0_ok + g_cap_nsreg0, 0, (size_t)(ns - g_cap_nsreg0)); g_cap_nsreg0 = ns; }
    for (int i = 0; i < ns; i++) {
        if (!vka_cap_sreg(i, &sp, NULL) || (g_cap_sreg0_ok[i] && !memcmp(&sp, &g_cap_sreg0[i], sizeof sp))) continue;
        cap_sreg e = {i, 0, sp}; g_cap_sreg0[i] = sp; g_cap_sreg0_ok[i] = 1; cap_buf_put(&sb, &e, sizeof e);
    }
    for (size_t o = 0; o < tb.n; o += sizeof(cap_treg)) { cap_treg *e = (cap_treg *)(tb.p + o); if (e->ok) { char w[32]; snprintf(w, sizeof w, "texture registry entry %d", e->tid); cap_tex_enc(&e->t, w); } }
    for (int i = 0; i < nreads; i++) cap_tex_read(&reads[i], 0);
    free(reads);
    if (tb.n) cap_put(&g_cap_w, CAP_TREG, submit, tb.p, tb.n, 0);
    if (sb.n) cap_put(&g_cap_w, CAP_SREG, submit, sb.p, sb.n, 0);
    cap_buf_free(&tb); cap_buf_free(&sb);
}

// ---------- The frame ----------
extern char **environ;
static void cap_head(void) {
    cap_buf h = {0}; char line[2048], lib[1024];
#define KV(...) do { int n_ = snprintf(line, sizeof line, __VA_ARGS__); cap_buf_put(&h, line, (size_t)MIN(n_, (int)sizeof line - 1)); } while (0)
    const char *commit = getenv("DM_BUILD_COMMIT"), *tmp = getenv("DM_TEMP"), *pc = getenv("DM_VK_PCACHE");
    vcap_shlib_path(lib, sizeof lib);
    KV("format\t%u\n", CAP_VERSION); KV("backend\tvulkan\n"); KV("iface\t%d\n", VCAP_IFACE); KV("chain\t%s\n", DMSH_CHAIN); KV("vk_chain\t%s\n", DMSH_VK_CHAIN);
    KV("commit\t%s\n", commit && *commit ? commit : "unknown"); KV("title\t%s\n", DM_GAME_ID); KV("build\t%s\n", dm_main_build());
    KV("frame\t%d\n", g_cap_frame); KV("frames\t1\n"); KV("mtl_frame\t%llu\n", (unsigned long long)vka_frame()); KV("vk_shlib\t%s\n", lib);
    if (pc && *pc) KV("vk_pcache\t%s\n", pc); else if (tmp && *tmp) { char dp[1100]; vka_pcache_name(dp, sizeof dp, tmp, vka_driver_tag(), VKA_PCACHE_GEN); KV("vk_pcache\t%s\n", dp); }   // (vka_pso.c's default)
    KV("device\t%s\n", V.props.deviceName);
    for (char **e = environ; e && *e; e++) if (!strncmp(*e, "DM_", 3) && strncmp(*e, "DM_NVN_CAPTURE=", 15)) KV("env\t%s\n", *e);
#undef KV
    cap_put(&g_cap_w, CAP_HEAD, 0, h.p, h.n, 0); cap_buf_free(&h);
    const char *lay = cap_layout(); cap_put(&g_cap_w, CAP_LAYT, 0, lay, strlen(lay), 0);
    snprintf(line, sizeof line, "tex_pool_n\t0\norigin_ll\t%d\nqueue_origin_ll\t%d\ndepth_zero\t%d\nnvn2\t%d\n", vka_origin_ll(), vka_queue_origin_ll(), vka_depth_zero(), vka_nvn2());
    cap_put(&g_cap_w, CAP_STAT, 0, line, strlen(line), 0);
}
typedef struct { uint64_t lo, hi; } pent;
static int pent_cmp(const void *a, const void *b) { uint64_t x = ((const pent *)a)->lo, y = ((const pent *)b)->lo; return x < y ? -1 : x > y; }
static void cap_pools(uint32_t submit) {
    int n = vka_pools(NULL, NULL, 0), m = n + 64;
    uint64_t *lo = malloc((size_t)m * 8), *sz = malloc((size_t)m * 8);
    n = MIN(vka_pools(lo, sz, m), m);
    pent *ps = malloc((size_t)MAX(n, 1) * sizeof *ps); int k = 0;
    for (int i = 0; i < n; i++) if (sz[i]) ps[k++] = (pent){lo[i], lo[i] + sz[i]};
    n = k; free(lo); free(sz);
    qsort(ps, (size_t)n, sizeof *ps, pent_cmp);
    for (int i = 0; i < n;) {
        uint64_t a = ps[i].lo, b = ps[i].hi; int j = i + 1;
        while (j < n && ps[j].lo < b) { b = MAX(b, ps[j].hi); j++; }
        int r = cap_reg_find(a);
        if (r < 0 || g_cap_reg[r].lo + g_cap_reg[r].size < b) {
            if (r >= 0 || cap_reg_find(b - 1) >= 0) cap_fail("a pool grew over an existing region during the frame");
            else cap_reg_add(a, b - a, 0);
        }
        i = j;
    }
    cap_buf bb = {0};
    for (int i = 0; i < n; i++) { cap_padd a = {ps[i].lo, ps[i].hi - ps[i].lo}; cap_enc_addr(NULL, &a.mem, 0, 0); cap_buf_put(&bb, &a, sizeof a); }
    cap_put(&g_cap_w, CAP_PADD, submit, bb.p, bb.n, 0); cap_buf_free(&bb); free(ps);
}
// A region's bytes go straight from guest memory to the file, after their hashes are taken: a write after the hash shows as a change at the next submit (no copy of up to
// a whole pool in memory, which a phone running the game cannot spare)
static void cap_put_region(uint32_t submit, const cap_regn *hh, const void *bytes) {
    cap_chunk c = {CAP_REGN, CAP_RAW, sizeof *hh + hh->size, sizeof *hh + hh->size, submit, 0};
    if (fwrite(&c, sizeof c, 1, g_cap_w.f) != 1 || fwrite(hh, sizeof *hh, 1, g_cap_w.f) != 1 || fwrite(bytes, 1, (size_t)hh->size, g_cap_w.f) != hh->size) g_cap_w.err = 1;
    g_cap_w.raw += c.raw; g_cap_w.stored += c.stored + sizeof c;
}
static void cap_reg_write(uint32_t submit) {
    for (uint32_t i = 0; i < g_cap_nreg; i++) {
        cap_region *r = &g_cap_reg[i]; if (!r->pending) continue;
        r->pending = 0;
        cap_reg_hash(r, (const uint8_t *)(uintptr_t)r->lo);
        cap_regn hh = {g_cap_rid[i], r->kind, r->lo, r->size};
        cap_put_region(submit, &hh, (const void *)(uintptr_t)r->lo);
    }
}
static void cap_mem(uint32_t submit) {
    cap_reg_write(submit);
    cap_buf b = {0};
    for (uint32_t i = 0; i < g_cap_nreg; i++) {
        cap_region *r = &g_cap_reg[i]; size_t nb = (size_t)((r->size + 4095) / 4096);
        for (size_t k = 0; k < nb;) {
            uint64_t o = (uint64_t)k * 4096; size_t len = (size_t)MIN(4096, r->size - o);
            uint64_t h = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, len);
            if (h == r->hash[k]) { k++; continue; }
            size_t k1 = k; uint64_t end = o;
            while (k1 < nb) { uint64_t o1 = (uint64_t)k1 * 4096; size_t l1 = (size_t)MIN(4096, r->size - o1); uint64_t h1 = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o1, l1);
                if (h1 == r->hash[k1]) break; r->hash[k1] = h1; end = o1 + l1; k1++; }
            struct { uint32_t region, pad; uint64_t off, len; } d = {g_cap_rid[i], 0, o, end - o};
            cap_buf_put(&b, &d, sizeof d); cap_buf_put(&b, (const uint8_t *)(uintptr_t)r->lo + o, (size_t)(end - o));
            k = k1;
        }
    }
    if (b.n) cap_put(&g_cap_w, CAP_PDIF, submit, b.p, b.n, 0);
    cap_buf_free(&b);
}
static void cap_flush_objs(uint32_t submit) {
    if (g_cap_pobj.n) cap_put(&g_cap_w, CAP_POBJ, submit, g_cap_pobj.p, g_cap_pobj.n, 0);
    if (g_cap_tobj.n) cap_put(&g_cap_w, CAP_TOBJ, submit, g_cap_tobj.p, g_cap_tobj.n, 0);
    if (g_cap_progb.n) cap_put(&g_cap_w, CAP_PROG, submit, g_cap_progb.p, g_cap_progb.n, 0);
    if (g_cap_dpolb.n) cap_put(&g_cap_w, CAP_DPOL, submit, g_cap_dpolb.p, g_cap_dpolb.n, 0);
    if (g_cap_kind.n) cap_put(&g_cap_w, CAP_TKND, submit, g_cap_kind.p, g_cap_kind.n, 0);
    g_cap_pobj.n = g_cap_tobj.n = g_cap_progb.n = g_cap_dpolb.n = g_cap_kind.n = 0;
}
static void mkdir_p(const char *path) {   // the capture's folder
    char d[1024]; strlcpy(d, path, sizeof d); char *s = strrchr(d, '/'); if (!s || s == d) return; *s = 0;
    for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0777); *p = '/'; }
    mkdir(d, 0777);
}

// Hook: nvnQueueSubmitCommands, under the submit lock, before the submission is replayed
void nvn_cap_submit(int n, const uint64_t *h) {
    if (!nvn_cap_enabled() || !g_cap_frame || g_cap_presents + 1 != (uint64_t)g_cap_frame) return;
    uint64_t t0 = now_ns();
    mtl_finish();
    g_cap_cur_submit = ++g_cap_submit;
    if (!g_cap_active) {
        mkdir_p(g_cap_path);
        if (!cap_open(&g_cap_w, g_cap_path)) { CRED("cannot write %s (errno %d): capture off", g_cap_path, errno); g_cap_done = 1; return; }
        setvbuf(g_cap_w.f, NULL, _IOFBF, 8u << 20);
        g_cap_active = 1;
        CLOG("frame %d: capture starts (%.1f s into the run)", g_cap_frame, (t0 - g_t0_ns) / 1e9);
        cap_head();
    }
    cap_pools(g_cap_cur_submit);
    uint64_t t1 = now_ns(); cap_registries(g_cap_cur_submit);
    uint64_t t2 = now_ns(); nvn_hm_clear(&g_cap_seg, NULL); g_cap_nseg = 0;
    uint32_t *top = malloc((size_t)MAX(n, 1) * 4);
    for (int i = 0; i < n; i++) top[i] = (uint32_t)cap_seg_get(h[i]);
    for (uint32_t i = 0; i < g_cap_nseg; i++) cap_seg_write(i);
    cap_flush_objs(g_cap_cur_submit);
    uint64_t t3 = now_ns(); cap_mem(g_cap_cur_submit);
    CLOG("submit %u: registries and their textures %.1f s, records and their textures %.1f s, pool bytes %.1f s", g_cap_cur_submit, (t2 - t1) / 1e9, (t3 - t2) / 1e9, (now_ns() - t3) / 1e9);
    if (g_cap_nsync) { uint32_t ns = g_cap_nsync; cap_put(&g_cap_w, CAP_SYNC, g_cap_cur_submit, &ns, 4, 0); }
    cap_buf s = {0}; uint32_t nn = (uint32_t)n; cap_buf_put(&s, &nn, 4); cap_buf_put(&s, top, (size_t)n * 4);
    cap_put(&g_cap_w, CAP_SUBM, g_cap_cur_submit, s.p, s.n, 0); cap_buf_free(&s); free(top);
    g_cap_ns += now_ns() - t0;
}

// Hook: nvnQueuePresentTexture, before the present is queued
void nvn_cap_present(const tex_t *t, const int crop[4]) {
    if (!nvn_cap_enabled()) return;
    g_cap_presents++;
    if (!g_cap_frame) {   // @seconds: the next frame is the one
        if ((now_ns() - g_t0_ns) / 1e9 >= g_cap_at_s) { g_cap_frame = (int)g_cap_presents + 1; CLOG("%.1f s: capturing frame %d (the next one)", (now_ns() - g_t0_ns) / 1e9, g_cap_frame); }
        return;
    }
    if (!g_cap_active || g_cap_presents != (uint64_t)g_cap_frame) {
        if (g_cap_presents >= (uint64_t)g_cap_frame && !g_cap_active) { CRED("frame %d presented without a submit: nothing captured", g_cap_frame); g_cap_done = 1; }
        return;
    }
    uint64_t t0 = now_ns();
    cap_pres p = {0}; cap_buf px = {0}; char png[1100]; snprintf(png, sizeof png, "%s.live.png", g_cap_path);
    g_cap_cur_submit = g_cap_submit + 1; g_cap_no_read = 1;
    if (t) { p.tex = cap_tex_obj(t); cap_flush_objs(g_cap_cur_submit); cap_reg_write(g_cap_cur_submit); }
    if (crop) memcpy(p.crop, crop, sizeof p.crop);
    p.frame = (uint32_t)g_cap_frame;
    if (!t || !vcap_digest(t, p.sha256, &p.bytes, &p.width, &p.height, &p.pixfmt, png, vka_origin_ll(), &px)) cap_fail("the presented texture could not be read back");
    g_cap_ns += now_ns() - t0; p.capture_ms = g_cap_ns / 1e6;
    cap_put(&g_cap_w, CAP_PRES, g_cap_cur_submit, &p, sizeof p, 0);
    if (px.n) cap_put(&g_cap_w, CAP_LPIX, g_cap_cur_submit, px.p, px.n, 0);
    cap_buf_free(&px);
    {
        cap_buf hb = {0};
        for (uint32_t i = 0; i < g_cap_nreg; i++) {
            cap_region *r = &g_cap_reg[i]; uint32_t nb = (uint32_t)((r->size + 4095) / 4096), hd[2] = {g_cap_rid[i], nb}; cap_buf_put(&hb, hd, 8);
            for (uint32_t k = 0; k < nb; k++) { uint64_t o = (uint64_t)k * 4096, hv = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, (size_t)MIN(4096, r->size - o)); cap_buf_put(&hb, &hv, 8); }
        }
        cap_put(&g_cap_w, CAP_PHSH, g_cap_cur_submit, hb.p, hb.n, 0); cap_buf_free(&hb);
    }
    uint64_t raw = g_cap_w.raw;
    int ok = cap_close(&g_cap_w); char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", p.sha256[i]);
    LOG("%sDM_NVN_CAPTURE: frame %d: %u submits, %llu segments, %u regions, %u textures (%llu read back, %.1f MB; %llu not captured), %llu addresses outside the pools, %llu registered textures outside them (%llu texture records written without storage); "
        "%.1f MB in %.0f ms → %s%s; live digest %s (%ux%u VkFormat %u)",
        g_cap_failed || !ok ? "[RED] " : "", g_cap_frame, g_cap_submit, (unsigned long long)g_cap_segs, g_cap_nreg, g_cap_ntex, (unsigned long long)g_cap_tex_n, g_cap_tex_bytes / 1048576.0,
        (unsigned long long)g_cap_tex_skip, (unsigned long long)g_cap_unmapped, (unsigned long long)g_cap_gone, (unsigned long long)g_cap_nostore, raw / 1048576.0, p.capture_ms, g_cap_path, ok ? "" : " (write failed)", hex, p.width, p.height, p.pixfmt);
    g_cap_active = 0; g_cap_done = 1;
}
#endif
