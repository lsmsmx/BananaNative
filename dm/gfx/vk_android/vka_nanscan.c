// DM_VK_NANSCAN=1: a diagnostic that finds the pass that first writes NaN or infinite texels. After every render pass, each float colour attachment's level / layer is
// scanned by a compute shader (selftest/nanscan.comp) that counts NaN and infinite texels and keeps the first one's position; when the submission is done the completion
// thread logs each pass with any, with its frame, its place in the submission, the attachment (serial, format, size) and the vertex / fragment shaders of its draws.
// DM_VK_NANSCAN=2 also splits long passes to find the draw: the Vulkan pass is ended (and reopened by the next draw, its attachments loaded) after every
// DM_VK_NANSCAN_STEP draws (default 64), in passes into the image DM_VK_NANSCAN_SERIAL (default any), of the game's pass whose index is in DM_VK_NANSCAN_DRAWS=<from>-<to> (default all), at most MAXSPLIT times a submission,
// and only the scans after which an attachment has more NaN / infinite texels than at its previous scan in the submission are logged, with the pass's draw range.
// Bisect with a coarse step over the whole pass, then step 1 over the range it names. Costs a dispatch per attachment per pass (per split with 2): for finding a
// cause, not for play.
// DM_VK_TEX_STATS=<fragment shader key>[,…] (live, needs no DM_VK_NANSCAN): the first draw of each frame with one of those fragment shaders has level 0 of each sampled texture
// (each layer up to 6, a 3D texture whole) scanned before it with the same dispatch, which also counts per channel the texels with |c| > 1/1024 and keeps the largest |c|;
// the completion thread logs one `tex stats:` line per texture and layer: to see which input of a lit material is zero (missing light) without a capture. The encoder thread
// is off while it is set (the scans record on the replay thread). DM_VK_TEX_STATS_LINES (live, default 200) bounds the value lines; each texture's description line (with the fragment key) is logged once a value; a new value starts again
// DM_VK_TEX_STATS_DRAWS=<n> (live, default 1): the first n draws of each frame with such a fragment shader are scanned, not only the first.
// DM_VK_TEX_STATS_TABLE=<sb>,<base ub>:<word>,<bias ub>:<word>,<index ub>:<word>[/<word>…],<offset>[/<offset>…] (live): also the bindless textures the draw reads through
// a record table in a storage buffer, as agl / gsys lit materials do (Zelda: Ocarina of Time's 1e4d2630d24846d0: its lightmap and baked-occlusion handles). Per index word v
// (a fragment-stage uniform word: a guest uniform buffer, or a driver segment such as c0, read as vka_drv_fill writes it): the record is ((v & 0xFFFFF) + bias) << 5 bytes into the table when (v & 0xC0000000) == 0x80000000, else record 0 (the shader's
// fallback); the table starts at the storage binding's address plus (base & 63), base being the uniform word that holds the table's address (c0[84].x there); each
// offset's word is a combined handle: texture ID = low 20 bits, sampler ID = the rest. One `tex stats table:` line per draw and index (the record's eight words and the
// handles), then each texture not yet scanned this frame is scanned as an input, logged with binding 1000 + 100 × the index's place + the offset
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_selftest_spv.h"
#include <stdlib.h>
#include <string.h>

enum { NBATCH = 16, MAXR = 1024, NKEY = 8, MAXSPLIT = 256 };
typedef struct { uint32_t nan, inf, first, pad, nz[4], mx[4]; } res_t;   // nz, mx: per channel, texels with |c| > 1/1024 and the largest |c| (float bits)
typedef struct { uint64_t frame, serial; uint32_t pass, fmt, w, h, lv, layer, d0, ndraws, nkeys; uint64_t vs[NKEY], fs[NKEY]; int bind, tid; uint32_t dz; uint64_t stats; } rec_t;   // stats: DM_VK_TEX_STATS's fragment shader (0: a NaN scan)
typedef struct { rec_t r[MAXR]; int n; res_t *res; VkDeviceSize off; } batch_t;
static batch_t g_b[NBATCH]; static unsigned g_bi; static vka_span g_res;
static VkPipeline g_pipe, g_pipe3, g_fetch; static VkPipelineLayout g_lay; static VkDescriptorSetLayout g_dsl;
static uint32_t g_pass; static uint64_t g_pvs[NKEY], g_pfs[NKEY]; static uint32_t g_pn, g_pdraws, g_d0;   // g_pdraws: draws of the game's pass so far, g_d0: the segment's first
static uint64_t g_probe, g_from_frame; static uint32_t g_big; static int g_split, g_nsplit; static uint32_t g_step = 64, g_from, g_to = UINT32_MAX; static uint64_t g_only;
static _Atomic int g_lines; static int g_maxl = 200;
static uint32_t g_ts_gen = UINT32_MAX; static uint64_t g_ts_keys[NKEY], g_ts_last[NKEY]; static int g_ts_n; static _Atomic int g_ts_lines;   // DM_VK_TEX_STATS (replay thread)
static struct { uint64_t fs; int bind, tid; } g_ts_seen[512]; static int g_ts_nseen;   // DM_VK_TEX_STATS: descriptions logged

int vka_nan_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_VK_NANSCAN"); v = e && atoi(e) > 0 && V.push_desc ? atoi(e) : 0;
    if (v >= 2) {
        const char *s = getenv("DM_VK_NANSCAN_STEP"), *r = getenv("DM_VK_NANSCAN_DRAWS"); if (s && atoi(s) > 0) g_step = (uint32_t)atoi(s);
        const char *o = getenv("DM_VK_NANSCAN_SERIAL"); if (o) g_only = strtoull(o, NULL, 10);
        if (r) { char *x; g_from = (uint32_t)strtoul(r, &x, 10); if (*x == '-') g_to = (uint32_t)strtoul(x + 1, NULL, 10); }
        VLOG("DM_VK_NANSCAN=2: passes split every %u draws in draws %u-%u of a pass%s, at most %d splits a submission", g_step, g_from, g_to, g_only ? " into DM_VK_NANSCAN_SERIAL" : "", MAXSPLIT);
    } else if (v) VLOG("DM_VK_NANSCAN: every pass's float attachments are scanned for NaN / infinite texels");
    const char *bg = getenv("DM_VK_NANSCAN_BIG"); float bf = bg ? strtof(bg, NULL) : 0;   // finite texels above this count as infinite: values out of a shader's range
    if (v && bf > 0) { memcpy(&g_big, &bf, 4); VLOG("DM_VK_NANSCAN_BIG: texels above %g count as infinite", bf); }
    const char *ml = getenv("DM_VK_NANSCAN_LINES"); if (ml && atoi(ml) > 0) g_maxl = atoi(ml);   // the lines logged in all (default 200)
    const char *ff = getenv("DM_VK_NANSCAN_FROM"); if (v && ff) { g_from_frame = strtoull(ff, NULL, 10); VLOG("DM_VK_NANSCAN_FROM: scans from frame %llu", (unsigned long long)g_from_frame); }
    return v;
}
// DM_VK_NANSCAN=2, after a draw: 1 = end the Vulkan pass now for a scan (the game's pass goes on)
int vka_nan_split(vka_img *const *ims, int n) {
    uint32_t i = g_pdraws;   // the draws of the game's pass so far
    if (i <= g_from || i > g_to || (i - g_from) % g_step) return 0;
    if (g_nsplit >= MAXSPLIT) { static _Atomic int n; if (!atomic_exchange(&n, 1)) VLOG("NaN scan: %d splits in one submission, no more in it (at draw %u of pass %u)", MAXSPLIT, i, g_pass); return 0; }
    if (g_only) { int k = 0; while (k < n && !(ims[k] && ims[k]->serial == g_only)) k++; if (k == n) return 0; }   // DM_VK_NANSCAN_SERIAL: only passes into that image
    g_nsplit++; g_split = 1; return 1;
}
static int ready(void) {
    static int st; if (st) return st > 0;
    st = -1;
    if (!vka_static((VkDeviceSize)NBATCH * MAXR * sizeof(res_t), V.sb_align, &g_res)) return 0;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof SPV_NANSCAN_COMP, .pCode = SPV_NANSCAN_COMP};
    VkResult r;
    if ((r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_dsl)) || (r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_lay))) { VRED("NaN scan: %d", r); return 0; }
    for (int i = 0; i < 3; i++) {   // 2D-array views (attachments, 2D / cube inputs), 3D inputs, the ASTC check's fetch
        VkShaderModule m = VK_NULL_HANDLE;
        if (i == 1) { mi.codeSize = sizeof SPV_NANSCAN3_COMP; mi.pCode = SPV_NANSCAN3_COMP; }
        if (i == 2) { mi.codeSize = sizeof SPV_FETCH_COMP; mi.pCode = SPV_FETCH_COMP; }
        if ((r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m))) { VRED("NaN scan: %d", r); return 0; }
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_lay};
        r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, i == 2 ? &g_fetch : i ? &g_pipe3 : &g_pipe);
        vk.vkDestroyShaderModule(V.dev, m, NULL);
        if (r) { VRED("NaN scan pipeline: %d", r); return 0; }
    }
    for (int i = 0; i < NBATCH; i++) { g_b[i].res = (res_t *)(g_res.ptr + (size_t)i * MAXR * sizeof(res_t)); g_b[i].off = g_res.off + (VkDeviceSize)i * MAXR * sizeof(res_t); }
    st = 1; return 1;
}
void vka_nan_draw(uint64_t vs, uint64_t fs) {
    g_pdraws++;   // counted before the draw records: a draw skipped after this still has its index
    for (uint32_t i = 0; i < g_pn; i++) if (g_pvs[i] == vs && g_pfs[i] == fs) return;
    if (g_pn < NKEY) { g_pvs[g_pn] = vs; g_pfs[g_pn] = fs; g_pn++; }
}
uint64_t vka_frame(void);
// The ASTC check (once per run with DM_VK_NANSCAN): an 8x8 ASTC 4x4 texture of four void-extent (constant colour) blocks, sampled by the device and logged. A void-extent block's
// colour is UNORM16 when its bit 9 (dynamic range) is 0 and FP16 when it is 1 (ASTC specification, "Void-Extent Blocks")
static const uint16_t g_vcol[4][4] = {{0x1212, 0x1212, 0x1212, 0xffff}, {0x8000, 0x4000, 0xc000, 0x8000}, {0x3c00, 0x3800, 0x3400, 0x3c00}, {0xffff, 0x0000, 0xffff, 0xffff}};
static const int g_vhdr[4] = {0, 0, 1, 0};
static vka_span g_fetch_out; static int g_astc_state;   // 1 = recorded, report at the next commit
static void astc_report(void *arg) {
    (void)arg; const float *t = (const float *)g_fetch_out.ptr;
    for (int b = 0; b < 4; b++) {
        const float *v = t + 4 * ((b / 2) * 4 * 8 + (b % 2) * 4);   // the block's first texel in the 8x8 texture
        VLOG("NaN scan: ASTC check: void-extent block %d (%s, colour %04x %04x %04x %04x) samples as (%g, %g, %g, %g)", b, g_vhdr[b] ? "HDR: FP16" : "LDR: UNORM16",
             g_vcol[b][0], g_vcol[b][1], g_vcol[b][2], g_vcol[b][3], v[0], v[1], v[2], v[3]);
    }
}
static void astc_check(VkCommandBuffer cb) {
    static uint8_t blocks[4 * 16]; static tex_t t;
    for (int b = 0; b < 4; b++) {
        uint64_t lo = 0x1fc | (uint64_t)g_vhdr[b] << 9 | 3ull << 10 | 0xfffffffffffffull << 12, hi = 0;   // extent all ones: the whole texture
        for (int c = 0; c < 4; c++) hi |= (uint64_t)g_vcol[b][c] << (16 * c);
        memcpy(blocks + 16 * b, &lo, 8); memcpy(blocks + 16 * b + 8, &hi, 8);
    }
    memset(&t, 0, sizeof t);
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = 0x79; t.d.width = 8; t.d.height = 8; t.d.depth = 1; t.d.levels = 1; t.d.flags = NVN_TEXTURE_FLAGS_LINEAR; t.d.stride = 32;
    nvn_tex_layout(&t.d, &t.L); t.mem = blocks; t.serial = nvn_new_serial();
    vka_img *im = vka_tex_get(&t, cb);
    if (!im || !vka_static(64 * 16, V.sb_align, &g_fetch_out)) { VLOG("NaN scan: ASTC check: no test texture"); return; }
    memset(g_fetch_out.ptr, 0xff, 64 * 16);
    VkImageView v = vka_view(im, im->f.view, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, NULL);
    if (!v) return;
    VkDescriptorImageInfo ii = {vka_samp_default(), v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {g_fetch_out.buf, g_fetch_out.off, 64 * 16};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    uint32_t pc[4] = {8, 8, 0, 0};
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_fetch);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_lay, 0, 2, wr); vka_dyn_invalidate();   // the draws' last push is no longer bound (vka_draw.c push_same)
    vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(cb, 1, 1, 1);
    vka_barrier(cb);
    g_astc_state = 1;
}
// One dispatch: level lv, layer l0 of im into the batch's next result
static int scan_one(VkCommandBuffer cb, batch_t *b, vka_img *im, uint32_t lv, uint32_t l0, uint32_t pass, int bind, int tid, uint64_t stats) {
    if (b->n == MAXR) return 0;
    int v3 = im->d.target == NVN_TEXTURE_TARGET_3D;   // a whole level of a 3D texture (inputs only)
    VkImageView v = vka_view(im, im->f.view, v3 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, v3 ? 0 : l0, 1, NULL);
    if (!v) return 0;
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, dz = v3 ? (im->depth >> lv ? im->depth >> lv : 1) : 1;
    int k = b->n++;
    b->res[k] = (res_t){0, 0, 0xffffffffu, 0, {0}, {0}};
    b->r[k] = (rec_t){vka_frame(), im->serial, pass, (uint32_t)im->d.format, w, h, lv, l0, g_d0, g_pdraws, g_pn, {0}, {0}, bind, tid, dz, stats};
    memcpy(b->r[k].vs, g_pvs, sizeof g_pvs); memcpy(b->r[k].fs, g_pfs, sizeof g_pfs);
    VkDescriptorImageInfo ii = {vka_samp_default(), v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {g_res.buf, b->off + (VkDeviceSize)k * sizeof(res_t), sizeof(res_t)};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    uint32_t pc[4] = {w, h, v3 ? dz : 0, g_big};   // 2D: layer 0 of the 2D-array view; g_big: DM_VK_NANSCAN_BIG
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v3 ? g_pipe3 : g_pipe);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_lay, 0, 2, wr); vka_dyn_invalidate();   // the draws' last push is no longer bound (vka_draw.c push_same)
    vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, dz);
    return 1;
}
// DM_VK_NANSCAN_FS=<fragment shader key> (with DM_VK_NANSCAN=2): the first draw of each frame with that fragment shader has its sampled float textures scanned before it
// (every level and layer, outside the pass): which input holds NaN / infinite texels. 1 = this draw is one (the caller ends the pass and calls vka_nan_inputs)
int vka_nan_probe(uint64_t fs) {
    static uint64_t last = ~0ull; static int init; uint64_t key = g_probe;
    if (!init) { init = 1; const char *e = getenv("DM_VK_NANSCAN_FS"); if (e) g_probe = key = strtoull(e, NULL, 16); }
    if (!key || fs != key || last == vka_frame()) return 0;
    last = vka_frame(); return 1;
}
// DM_VK_TEX_STATS: 1 = set (its keys parsed again after each live change)
int vka_texstats_on(void) {
    uint32_t g = atomic_load_explicit(&vka_env_gen, memory_order_relaxed);
    if (g != g_ts_gen) {
        g_ts_gen = g; char b[256]; int n = 0;
        if (vka_env_str("DM_VK_TEX_STATS", b, sizeof b)) for (const char *q = b; *q && n < NKEY;) { char *e; unsigned long long k = strtoull(q, &e, 16); if (e == q) break; g_ts_keys[n++] = k; q = *e == ',' ? e + 1 : e; }
        static uint64_t prev[NKEY]; static int nprev = -1; int changed = n != nprev; for (int i = 0; !changed && i < n; i++) changed = prev[i] != g_ts_keys[i];
        if (changed) {
            memcpy(prev, g_ts_keys, sizeof prev); nprev = n; atomic_store(&g_ts_lines, 0); memset(g_ts_last, 0xff, sizeof g_ts_last); g_ts_nseen = 0;
            if (n) VLOG("tex stats: from frame %llu, the inputs of %d fragment shader(s) (first %016llx) are scanned before their first draw of each frame (DM_VK_TEX_STATS)", (unsigned long long)vka_frame(), n, (unsigned long long)g_ts_keys[0]);
        }
        g_ts_n = n;
    }
    return g_ts_n > 0 && V.push_desc;
}
// DM_VK_TEX_STATS: 1 = this draw's fragment shader is one, its first draw this frame, and lines are left (the caller ends the pass and calls vka_nan_inputs)
int vka_texstats_probe(uint64_t fs) {
    if (!vka_texstats_on() || vka_enc_on() || atomic_load(&g_ts_lines) >= VKA_ENV("DM_VK_TEX_STATS_LINES", 200)) return 0;
    static uint32_t ndr[NKEY];   // DM_VK_TEX_STATS_DRAWS (live, default 1): draws of this fragment shader scanned this frame so far
    for (int i = 0; i < g_ts_n; i++) if (g_ts_keys[i] == fs) {
        if (g_ts_last[i] != vka_frame()) { g_ts_last[i] = vka_frame(); ndr[i] = 0; }
        if (ndr[i] >= (uint32_t)VKA_ENV("DM_VK_TEX_STATS_DRAWS", 1)) return 0;
        ndr[i]++; return 1;
    }
    return 0;
}
int vka_texstats_room(void) {   // counts a line only while below the limit, so a larger DM_VK_TEX_STATS_LINES set later gives room again
    int lim = VKA_ENV("DM_VK_TEX_STATS_LINES", 200), n = atomic_load(&g_ts_lines);
    while (n < lim) if (atomic_compare_exchange_weak(&g_ts_lines, &n, n + 1)) return 1;
    return 0;
}
// DM_VK_TEX_STATS: 1 = texture ID tid has not been scanned yet this frame (at most 256 a frame), noted now
int vka_texstats_table_new(int tid) {
    static uint64_t fr = ~0ull; static int seen[256], n;
    if (fr != vka_frame()) { fr = vka_frame(); n = 0; }
    for (int i = 0; i < n; i++) if (seen[i] == tid) return 0;
    if (n == 256) return 0;
    seen[n++] = tid; return 1;
}
// DM_VK_TEX_STATS: 1 = the first time this (fragment shader, binding, texture ID) is seen since the switch was set: its description line is logged once (no line budget;
// at most 512 a value)
int vka_texstats_desc_new(uint64_t fs, int bind, int tid) {
    for (int i = 0; i < g_ts_nseen; i++) if (g_ts_seen[i].fs == fs && g_ts_seen[i].bind == bind && g_ts_seen[i].tid == tid) return 0;
    if (g_ts_nseen == 512) return 0;
    g_ts_seen[g_ts_nseen].fs = fs; g_ts_seen[g_ts_nseen].bind = bind; g_ts_seen[g_ts_nseen].tid = tid; g_ts_nseen++; return 1;
}
void vka_nan_inputs(VkCommandBuffer cb, vka_img *const *ims, const int *binds, const int *tids, int n, uint64_t stats) {
    batch_t *b = &g_b[g_bi % NBATCH];
    if (!ready()) return;
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->f.kind != 0 || im->samples > 1 || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) continue;
        uint32_t nl = im->d.target == NVN_TEXTURE_TARGET_3D ? 1 : im->layers;
        uint32_t nlv = stats ? 1 : im->levels;   // DM_VK_TEX_STATS: level 0 only
        for (uint32_t lv = 0; lv < nlv && lv < 14; lv++) for (uint32_t l = 0; l < nl && l < 6; l++) any |= scan_one(cb, b, im, lv, l, g_pass, binds[i], tids[i], stats);
    }
    if (any) vka_barrier(cb);
    if (stats) return;
    static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VLOG("NaN scan: frame %llu: %d inputs of fragment shader %016llx scanned before its first draw (only those with NaN / infinite texels are logged)", (unsigned long long)vka_frame(), n, (unsigned long long)g_probe);
}
// After a pass ends (its writes made visible by the caller's barrier): one dispatch per float colour attachment
void vka_nan_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n) {
    batch_t *b = &g_b[g_bi % NBATCH];
    int split = g_split; g_split = 0;
    uint32_t pass = split ? g_pass : g_pass++;
    if (vka_frame() < g_from_frame) { g_pn = 0; g_pdraws = 0; g_d0 = 0; return; }   // DM_VK_NANSCAN_FROM: not yet
    if (!ready()) return;
    if (!g_astc_state) { g_astc_state = -1; astc_check(cb); }
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->f.kind != 0 || im->samples > 1 || im->d.target == NVN_TEXTURE_TARGET_3D || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || b->n == MAXR) continue;
        any |= scan_one(cb, b, im, lvs[i], l0s[i], pass, -1, 0, 0);
    }
    if (any) vka_barrier(cb);
    g_pn = 0;
    if (split) g_d0 = g_pdraws; else { g_pdraws = 0; g_d0 = 0; }
}
// After a dispatch (DM_VK_NANSCAN): its float storage images, level 0 (each layer up to 6, a 3D one whole), logged as a pass whose only shader is the compute shader (vertex 0)
void vka_nan_dispatch(VkCommandBuffer cb, uint64_t key, vka_img *const *ims, int n) {
    batch_t *b = &g_b[g_bi % NBATCH];
    if (vka_frame() < g_from_frame || !ready()) return;
    uint64_t pvs[NKEY], pfs[NKEY]; uint32_t pn = g_pn, pd = g_pdraws, d0 = g_d0; memcpy(pvs, g_pvs, sizeof pvs); memcpy(pfs, g_pfs, sizeof pfs);   // (an open game pass's draws: kept)
    g_pvs[0] = 0; g_pfs[0] = key; g_pn = 1; g_pdraws = 1; g_d0 = 0;
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i]; int dup = 0;
        for (int j = 0; j < i; j++) dup |= ims[j] == im;
        if (!im || dup || im->f.kind != 0 || im->samples > 1 || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) continue;
        uint32_t nl = im->d.target == NVN_TEXTURE_TARGET_3D ? 1 : im->layers;
        for (uint32_t l = 0; l < nl && l < 6; l++) any |= scan_one(cb, b, im, 0, l, g_pass, -1, 0, 0);
    }
    if (any) vka_barrier(cb);
    memcpy(g_pvs, pvs, sizeof pvs); memcpy(g_pfs, pfs, sizeof pfs); g_pn = pn; g_pdraws = pd; g_d0 = d0;
}
static void report(void *arg) {
    batch_t *b = arg;
    for (int k = 0; k < b->n; k++) {
        const res_t *r = &b->res[k]; const rec_t *c = &b->r[k];
        if (c->stats) {   // DM_VK_TEX_STATS: every texture and layer, NaN or not
            if (!vka_texstats_room()) return;
            uint64_t nt = (uint64_t)c->w * c->h * (c->dz ? c->dz : 1); float mx[4]; memcpy(mx, r->mx, sizeof mx);
            char dims[32]; snprintf(dims, sizeof dims, c->dz ? "%ux%ux%u" : "%ux%u", c->w, c->h, c->dz);
            VLOG("tex stats: frame %llu fragment %016llx binding %d texture ID %d serial %llu (format %#x %s level 0 layer %u): %llu texels; above 1/1024 x %.1f%% y %.1f%% z %.1f%% w %.1f%%; max x %g y %g z %g w %g; %u NaN + %u infinite",
                 (unsigned long long)c->frame, (unsigned long long)c->stats, c->bind, c->tid, (unsigned long long)c->serial, c->fmt, dims, c->layer, (unsigned long long)nt,
                 100.0 * r->nz[0] / (double)nt, 100.0 * r->nz[1] / (double)nt, 100.0 * r->nz[2] / (double)nt, 100.0 * r->nz[3] / (double)nt, mx[0], mx[1], mx[2], mx[3], r->nan, r->inf);
            continue;
        }
        if (!r->nan && !r->inf) continue;
        if (c->bind >= 0) {
            if (atomic_fetch_add(&g_lines, 1) >= g_maxl) return;
            VLOG("NaN scan: frame %llu pass %u (fragment %016llx): input binding %d texture ID %d serial %llu (format %#x %ux%u level %u layer %u): %u NaN + %u infinite texels, first at (%u, %u)",
                 (unsigned long long)c->frame, c->pass, (unsigned long long)g_probe, c->bind, c->tid, (unsigned long long)c->serial, c->fmt, c->w, c->h, c->lv, c->layer, r->nan, r->inf, r->first & 0xffff, r->first >> 16);
            continue;
        }
        if (vka_nan_on() >= 2) {   // only where the count of this attachment grew since its previous scan in this submission
            int j = k - 1; while (j >= 0 && (b->r[j].serial != c->serial || b->r[j].lv != c->lv || b->r[j].layer != c->layer)) j--;
            if (j >= 0 && b->res[j].nan + b->res[j].inf >= r->nan + r->inf) continue;
        }
        if (atomic_fetch_add(&g_lines, 1) >= g_maxl) return;
        char keys[400] = ""; size_t o = 0;
        for (uint32_t i = 0; i < c->nkeys && o < sizeof keys - 40; i++) o += (size_t)snprintf(keys + o, sizeof keys - o, " %016llx/%016llx", (unsigned long long)c->vs[i], (unsigned long long)c->fs[i]);
        VLOG("NaN scan: frame %llu pass %u: %u NaN + %u infinite texels in serial %llu (format %#x %ux%u level %u layer %u), first at (%u, %u); draws %u-%u of the pass, vertex/fragment%s%s",
             (unsigned long long)c->frame, c->pass, r->nan, r->inf, (unsigned long long)c->serial, c->fmt, c->w, c->h, c->lv, c->layer, r->first & 0xffff, r->first >> 16, c->d0, c->ndraws, keys, c->nkeys == NKEY ? " …" : "");
    }
}
void vka_nan_commit(uint64_t fence) {   // the submission's batch is read when fence is done; the next submission starts a new batch
    batch_t *b = &g_b[g_bi % NBATCH];
    if (b->n) vka_after(fence, report, b);
    if (g_astc_state == 1) { g_astc_state = 2; vka_after(fence, astc_report, NULL); }
    g_bi++; g_b[g_bi % NBATCH].n = 0; g_pass = 0; g_nsplit = 0;
}
#endif
