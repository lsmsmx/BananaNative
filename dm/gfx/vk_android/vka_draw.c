// Draws and dispatches of the Vulkan replay: nvn_mtl.m's draw path on Vulkan. vka_exec.c walks the records; state records land in the queue's vka_dstate (vka_state_op),
// and each draw resolves its program to shaders (vka_shader.c), opens the dynamic-rendering pass of the bound targets if none is open, takes a pipeline keyed on the state
// Vulkan cannot set dynamically here (vka_pso.c), writes set 0 from the binding records (vka_bind.c's walker: push descriptors when they fit, else a set from the submission
// slot's pool), sets the dynamic state and draws. The pass stays open across draws and closes at anything else (vka_rp_end), followed by a full barrier.
// What is not here yet, each logged once: SINT attachments and integer vertex formats (no variants), transform feedback, DrawTexture, line loops and quads through indirect draws,
// tessellation's patch size, texel buffers, viewport swizzles other than a y flip
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_mtl.h"
#include "../../host/plat/plat.h"
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static VkCommandBuffer g_cb; static unsigned g_slot; static int g_prof, g_dyn_filter = 1, g_pass_keep = 1, g_vb_filter = 1;   // g_prof: DM_VK_PROF
static int g_inst_on;   // DM_VK_INSTANCE (the instanced runs below)
uint64_t vka_n_rp;   // render passes the draws opened (vka_exec.c replay statistics)
static uint64_t g_draws, g_skipped, g_dispatches;
static uint32_t g_claim_words;   // the draw's claim words (an upper bound of its primitives): 0 = its geometry shader takes no claim
#define SKIP(...) do { g_skipped++; static _Atomic int n_; if (atomic_fetch_add(&n_, 1) < 4) VRED(__VA_ARGS__); return; } while (0)

// Whether draws and dispatches run. Without placed guest memory (an emulator's driver) the GPU sees no memory pool, so a game's draws would read nothing and only cost:
// pipeline compiles there take minutes each. Off there by default, on with placed memory; DM_VK_DRAW=0 / 1 overrides, the selftest turns them on for its own memory
static int g_force_draws;
void vka_draw_force(int on) { g_force_draws = on; }
static int draws_on(void) {
    int env = VKA_ENV("DM_VK_DRAW", -1); if (env > 0) env = 1;
    int on = g_force_draws || (env >= 0 ? env : V.placed);
    if (!on) VRED_ONCE("draws and dispatches are off (%s): their records are skipped; events, counters and fences still signal", env == 0 ? "DM_VK_DRAW=0" : "no placed guest memory, so the GPU would see no pool; DM_VK_DRAW=1 turns them on");
    return on;
}

// ---------- State ----------
void vka_dstate_reset(vka_dstate *d) {   // NVN's defaults (nvn_mtl.m state_reset)
    memset(d, 0, sizeof *d); d->inited = 1;
    for (int i = 0; i < 8; i++) d->blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    d->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; d->chmask.mask = 0xffffffffu;
    d->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    d->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    d->st_vmask[0] = d->st_vmask[1] = d->st_wmask[0] = d->st_wmask[1] = 0xff; d->dr[1] = 1; d->b.samples = 1;
}
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c: where a descriptor pool's IDs start in the host table
int vka_state_op(vka_dstate *d, uint32_t op, const void *a) {
    if (!d->inited) vka_dstate_reset(d);
    if (op != OP_BIND_UBO && op != OP_BIND_UBO_SMALL) { d->bind_gen++; }
    switch (op) {
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int tex = op == OP_SET_TEX_POOL, b = nvn_dpool_base(((const p_set_pool *)a)->pool, tex); if (tex) d->b.tex_base = b; else d->b.samp_base = b; break; }
    case OP_BIND_PROGRAM: { const p_bind_program *b = a;
        if (b->stages & 0x20) d->cprog = b->prog;
        for (int st = 0; st < VKA_NSTAGE; st++) if (st != 5 && (b->stages >> st & 1)) d->bst[st] = b->prog;   // NVN binds per stage (nvn_mtl.m bind_stages); 6 / 7: NVN2's task and mesh
        break; }
    case OP_BIND_STATE: {
        const p_bind_state *b = a; blend_st bs;
        switch (b->kind) {
        case 0: memcpy(&bs, b->bytes, sizeof bs); d->blend[bs.target] = bs; break;
        case 1: memcpy(&d->color, b->bytes, sizeof d->color); break;
        case 2: memcpy(&d->chmask, b->bytes, sizeof d->chmask); break;
        case 3: memcpy(&d->ms, b->bytes, sizeof d->ms); break;
        case 4: memcpy(&d->poly, b->bytes, sizeof d->poly); break;
        case 5: memcpy(&d->ds, b->bytes, sizeof d->ds); break;
        }
        break; }
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->first >= 0 && b->first + b->n <= 16) { memcpy(&d->attr[b->first], b->st, 4 * (size_t)b->n); if (b->first + b->n > d->nattr) d->nattr = b->first + b->n; } break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->first >= 0 && b->first + b->n <= 16) { memcpy(&d->stream[b->first], b->st, 8 * (size_t)b->n); if (b->first + b->n > d->nstream) d->nstream = b->first + b->n; } break; }
    case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); if (b->first >= 0 && b->first + b->n <= 16) memcpy(&d->vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
    case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->index >= 0 && b->index < 16) { d->ubo_dirty[b->stage] |= 1u << b->index; d->b.ubo[b->stage][b->index] = (vka_range){b->addr, b->size}; } break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->index >= 0 && b->index < 16) d->b.ssbo[b->stage][b->index] = (vka_range){b->addr, b->size}; break; }
    case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
        if (b->n < 0 || b->n > 32) { VRED_ONCE("texture bind record with count %d: overwritten after recording", b->n); break; }
        uint64_t h[32]; memcpy(h, b->h, 8 * (size_t)b->n);
        if (b->defer && b->src && b->n) {   // BindTexturesDeferred: the handles as memory holds them now (nvn_mtl.m bh_fresh); placed guest memory read directly, else word by word through the fault-safe read
            uint32_t w[32];
            if (vka_addr(b->src, 4u * (uint64_t)b->n, NULL, NULL, NULL)) { memcpy(w, (const void *)(uintptr_t)b->src, 4 * (size_t)b->n); for (int k = 0; k < b->n; k++) h[k] = w[k]; }
            else if (!plat_mem_read(b->src, w, 4 * (size_t)b->n)) for (int k = 0; k < b->n; k++) h[k] = w[k];   // one read for the run (they are in the game's heap; profile p2: 3.4 % in process_vm_readv)
            else for (int k = 0; k < b->n; k++) if (!plat_mem_read(b->src + 4u * k, &w[k], 4)) h[k] = w[k];   // a run across an unreadable page: word by word
        }
        if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->first + b->n <= VKA_NTEX) memcpy(&d->b.tex[b->stage][b->first], h, 8 * (size_t)b->n);
        break; }
    case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->n >= 0 && b->first + b->n <= 128) memcpy(&d->b.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->n >= 0 && b->first + b->n <= 32) memcpy(&d->b.img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_STENCIL: { const p_stencil *b = a;
        for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) d->st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) d->st_wmask[f] = (uint32_t)b->mask; else d->st_ref[f] = (uint32_t)b->ref; }
        break; }
    case OP_POLY_OFFSET: d->po = *(const p_poly_offset *)a; break;
    case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(d->vp[b->first + i], b->v[i], 16); d->has_vp = 1; break; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = a; d->dr[0] = b->n; d->dr[1] = b->f; break; }
    case OP_DEPTH_CLAMP: d->depth_clamp = ((const p_depth_clamp *)a)->on; break;
    case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(d->vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
    case OP_BLEND_COLOR: memcpy(d->blend_color, ((const p_blend_color *)a)->rgba, 16); break;
    case OP_ALPHA_REF: d->alpha_ref = ((const p_alpha_ref *)a)->ref; break;
    case OP_PUSH_CONST: { const p_push_const *b = a; if ((uint32_t)b->offset + b->size <= DMSH_PUSH_SIZE) memcpy(d->b.push[b->compute ? 1 : 0] + b->offset, b->data, b->size); break; }
    case OP_TF_BIND: break;
    case OP_TF_STATE: { const p_tf_state *b = a; d->tf_on = b->what == 0 || b->what == 3; break; }
    case OP_PRIM_RESTART: d->restart = *(const p_prim_restart *)a; break;
    case OP_RAST_DISCARD: d->rast_discard = ((const p_rast_discard *)a)->on; break;
    default: return 0;
    }
    return 1;
}

// ---------- Static buffers: zeros, a write sink, the support buffer ----------
static vka_span g_zero, g_sink, g_support;
static int statics(void) {
    static int ok = -1; if (ok >= 0) return ok;
    ok = vka_static(65536, V.ub_align > V.sb_align ? V.ub_align : V.sb_align, &g_zero) && vka_static(65536, V.sb_align, &g_sink) && vka_static(VKA_SUPPORT_SIZE, V.ub_align, &g_support);
    if (ok) { memset(g_zero.ptr, 0, 65536); memset(g_sink.ptr, 0, 65536); vka_support_fill(g_support.ptr, VKA_SUPPORT_SIZE); }
    return ok;
}

// ---------- Descriptor sets that do not fit a push (per submission slot) ----------
enum { NSLOT = 8, MAXPOOLS = 64 };
static VkDescriptorPool g_dp[NSLOT][MAXPOOLS]; static int g_ndp[NSLOT], g_cur_dp[NSLOT];
void vka_dyn_invalidate(void);
static void drv_memo_reset(void);
void vka_draw_sub_begin(VkCommandBuffer cb, unsigned slot) {
    g_cb = cb; g_slot = slot % NSLOT; vka_dyn_invalidate();
    { void vka_mem_rec_thread(void); vka_mem_rec_thread(); }   // the address lookups' cache outside TLS for the recording thread (vka_mem.c)
    drv_memo_reset();   // the last submission's scratch spans go with its fence
    g_prof = VKA_ENV("DM_VK_PROF", 0) != 0; g_dyn_filter = VKA_ENV("DM_VK_DYN_FILTER", 1); g_pass_keep = VKA_ENV("DM_VK_PASS_KEEP", 1); g_vb_filter = VKA_ENV("DM_VK_VB_FILTER", 1); g_inst_on = VKA_ENV("DM_VK_INSTANCE", 0);   // live: from this submission
    for (int i = 0; i < g_ndp[g_slot]; i++) vk.vkResetDescriptorPool(V.dev, g_dp[g_slot][i], 0);   // the slot's last submission is done (mtl_sub_begin waited for it)
    g_cur_dp[g_slot] = 0;
    vka_pso_save(0);
    static int ph; VkImageView placeholder(uint8_t dim, int storage);
    if (!V.null_desc && !ph && statics()) {   // no null descriptors: the placeholders are made here, outside any pass (their first use records a clear)
        ph = 1; static const uint8_t dims[] = {1, 2, 3, 1 | DMSH_VK_DIM_ARRAY, 3 | DMSH_VK_DIM_ARRAY};
        for (size_t i = 0; i < sizeof dims; i++) for (int c = 0; c < 3; c++) placeholder(dims[i] | (c == 1 ? DMSH_VK_DIM_SINT : c == 2 ? DMSH_VK_DIM_UINT : 0), 0);
    }
}
static VkDescriptorSet set_alloc(VkDescriptorSetLayout l) {
    for (;;) {
        int i = g_cur_dp[g_slot];
        if (i == g_ndp[g_slot]) {
            if (i == MAXPOOLS) { VRED_ONCE("descriptor pools exhausted in one submission"); return VK_NULL_HANDLE; }
            VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
            VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1024, .poolSizeCount = sizeof ps / sizeof *ps, .pPoolSizes = ps};
            if (vk.vkCreateDescriptorPool(V.dev, &pi, NULL, &g_dp[g_slot][i])) { VRED_ONCE("descriptor pool not made"); return VK_NULL_HANDLE; }
            g_ndp[g_slot]++;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dp[g_slot][i], .descriptorSetCount = 1, .pSetLayouts = &l};
        VkDescriptorSet s; VkResult r = vk.vkAllocateDescriptorSets(V.dev, &ai, &s);
        if (!r) return s;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) { VRED_ONCE("vkAllocateDescriptorSets %d", r); return VK_NULL_HANDLE; }
        g_cur_dp[g_slot]++;
    }
}

// ---------- The render pass ----------
static struct {
    int on, classic; VkRect2D area; uint32_t layers, samples;   // classic: begun as a VkRenderPass (DM_VK_CLASSIC_RP)
    uint32_t ncolor; VkFormat cf[8]; uint8_t ckind[8]; VkFormat df, sf;
    vka_img *cim[8]; uint32_t clv[8], cl0[8]; int nci;   // the colour attachments (DM_VK_NANSCAN)
    p_set_rt rt; int has_rt;                             // the targets it was opened for: a later SET_RT of the same targets keeps it open
    uint32_t cnl[8], dnl; VkExtent2D cext[8], dext;      // each attachment's layers and level size (clears in the pass must cover what they would outside it)
    int swrite;                                          // a draw of the pass wrote memory (storage buffers or images, raw global stores, a geometry shader's claim)
    int bar_pending;                                     // a game barrier came inside the pass and was taken there (vka_rp_game_barrier): the draws after it are checked (bar_hazard)
} R;
// Clears waiting for the pass of their targets (vka_rp_clear), in order: the next draw over the same targets opens its pass with them (loadOp CLEAR, or vkCmdClearAttachments),
// anything else does them outside a pass first (pc_flush), as before
enum { NPC = 16 };
static struct { int n; p_set_rt rt; vka_pclear c[NPC]; } PC;
static void pc_flush(void) { int n = PC.n; PC.n = 0; for (int i = 0; i < n; i++) vka_clear_outside(&PC.c[i]); }
// The same targets: the same textures and the same views of them (field by field: the records' padding is not compared)
static int view_same(const view_t *a, const view_t *b) {
    return a->set_levels == b->set_levels && a->set_layers == b->set_layers && a->set_format == b->set_format && (!a->set_levels || (a->base_level == b->base_level && a->nlevels == b->nlevels)) &&
           (!a->set_layers || (a->min_layer == b->min_layer && a->nlayers == b->nlayers)) && (!a->set_format || a->format == b->format);
}
static int rt_same(const p_set_rt *a, int ha, const p_set_rt *b, int hb) {
    if (!ha || !hb) return !ha && !hb;
    if (a->n != b->n || a->depth != b->depth || !!a->has_dview != !!b->has_dview || (a->has_dview && !view_same(&a->dview, &b->dview))) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (a->color[i] != b->color[i] || !!a->has_cview[i] != !!b->has_cview[i]) return 0;
        if (a->color[i] && a->has_cview[i] && !view_same(&a->cview[i], &b->cview[i])) return 0;
    }
    return 1;
}
// ---------- Classic render passes (DM_VK_CLASSIC_RP) ----------
// The game's passes as VkRenderPass objects begun with an imageless VkFramebuffer instead of dynamic rendering. Qualcomm's driver (Adreno 740, 512.863) spends 13 us of
// CPU in a vkCmdBeginRendering + vkCmdEndRendering pair at the least and ~40 us in Donkey Kong Bananza's level (a quarter of the encoder thread, with ~110 passes a frame);
// a render pass with an imageless framebuffer takes ~1.5 us (a Vulkan micro-benchmark on the same device). Render passes and framebuffers are made once per shape and kept (by targets' formats,
// loads and clears; by each attachment's image flags, usage, size and view formats); every attachment stays in GENERAL as before. Pipelines are made against a render
// pass of the same formats (vka_rp_compat: Vulkan's compatibility ignores loads and clears), without graphics pipeline libraries. Restart only
int vka_classic_rp(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_CLASSIC_RP"); on = e && atoi(e) != 0; if (on) VLOG("classic render passes (DM_VK_CLASSIC_RP)"); } return on; }
typedef struct { VkFormat cf[8]; uint32_t n; VkFormat ds; uint32_t d, s, samples, clear; } rpkey;   // clear: bit i colour i, bit 8 depth, bit 9 stencil
static struct { rpkey k; VkRenderPass rp; } g_rpc[256]; static int g_nrpc; static pthread_mutex_t g_rpmu = PTHREAD_MUTEX_INITIALIZER;
static VkRenderPass rp_obtain(const rpkey *k) {
    pthread_mutex_lock(&g_rpmu);
    for (int i = 0; i < g_nrpc; i++) if (!memcmp(&g_rpc[i].k, k, sizeof *k)) { VkRenderPass r = g_rpc[i].rp; pthread_mutex_unlock(&g_rpmu); return r; }
    VkAttachmentDescription at[9]; VkAttachmentReference cr[8], dr; uint32_t na = 0;
    VkSampleCountFlagBits sc = (VkSampleCountFlagBits)(k->samples ? k->samples : 1);
    for (uint32_t i = 0; i < k->n; i++) {
        if (k->cf[i] == VK_FORMAT_UNDEFINED) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; continue; }
        at[na] = (VkAttachmentDescription){0, k->cf[i], sc, k->clear >> i & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                           VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        cr[i] = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (k->ds) {
        at[na] = (VkAttachmentDescription){0, k->ds, sc, k->d && (k->clear >> 8 & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                           k->s && (k->clear >> 9 & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        dr = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, k->n, cr, NULL, k->ds ? &dr : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sd};
    VkRenderPass r = VK_NULL_HANDLE; VkResult e = vk.vkCreateRenderPass(V.dev, &ci, NULL, &r);
    if (e) { VRED_ONCE("vkCreateRenderPass %d", e); r = VK_NULL_HANDLE; }
    else if (g_nrpc < 256) { g_rpc[g_nrpc].k = *k; g_rpc[g_nrpc++].rp = r; }
    pthread_mutex_unlock(&g_rpmu);
    return r;
}
VkRenderPass vka_rp_compat(const VkFormat *cf, uint32_t n, VkFormat df, VkFormat sf, uint32_t samples) {
    rpkey k; memset(&k, 0, sizeof k);
    if (n > 8) n = 8; memcpy(k.cf, cf, n * sizeof *cf); k.n = n; k.ds = df ? df : sf; k.d = df != 0; k.s = sf != 0; k.samples = samples ? samples : 1;
    return rp_obtain(&k);
}
typedef struct { VkImageCreateFlags fl; VkImageUsageFlags u; uint32_t w, h, nl, nvf; VkFormat vf[6]; } fbatt;
typedef struct { VkRenderPass rp; uint32_t w, h, layers, na; fbatt a[9]; } fbkey;
enum { FBC_N = 1024 };
static struct { fbkey k; VkFramebuffer fb; int used; } g_fbc[FBC_N];
static VkFramebuffer fb_obtain(const fbkey *k) {   // (the recording thread only)
    uint64_t h = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)k; for (size_t i = 0; i < sizeof *k; i++) { h ^= p[i]; h *= 1099511628211ull; }
    for (uint32_t i = (uint32_t)h & (FBC_N - 1), m = 0; m < 32; m++, i = (i + 1) & (FBC_N - 1)) {
        if (g_fbc[i].used && !memcmp(&g_fbc[i].k, k, sizeof *k)) return g_fbc[i].fb;
        if (!g_fbc[i].used) {
            VkFramebufferAttachmentImageInfo ai[9];
            for (uint32_t a = 0; a < k->na; a++) ai[a] = (VkFramebufferAttachmentImageInfo){VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, NULL, k->a[a].fl, k->a[a].u, k->a[a].w, k->a[a].h, k->a[a].nl, k->a[a].nvf, k->a[a].vf};
            VkFramebufferAttachmentsCreateInfo fa = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, NULL, k->na, ai};
            VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .pNext = &fa, .flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, .renderPass = k->rp, .attachmentCount = k->na,
                                          .width = k->w, .height = k->h, .layers = k->layers};
            VkFramebuffer fb = VK_NULL_HANDLE; VkResult e = vk.vkCreateFramebuffer(V.dev, &ci, NULL, &fb);
            if (e) { VRED_ONCE("vkCreateFramebuffer %d", e); return VK_NULL_HANDLE; }
            g_fbc[i].k = *k; g_fbc[i].fb = fb; g_fbc[i].used = 1; return fb;
        }
    }
    VRED_ONCE("framebuffer cache full: passes fall back to dynamic rendering"); return VK_NULL_HANDLE;
}
static fbatt fb_att(const vka_img *im, uint32_t lv, uint32_t nl, VkFormat view) {
    fbatt a; memset(&a, 0, sizeof a);
    a.fl = im->cflags; a.u = im->usage; if (view != im->f.vk && (a.u & VK_IMAGE_USAGE_STORAGE_BIT)) a.u &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT;   // (as vka_view restricts an sRGB view)
    a.w = im->w >> lv ? im->w >> lv : 1; a.h = im->h >> lv ? im->h >> lv : 1; a.nl = nl;
    if (im->nvf) { a.nvf = im->nvf; memcpy(a.vf, im->vf, sizeof a.vf); } else { a.nvf = 1; a.vf[0] = view; }
    return a;
}
// Game barriers (OP_BARRIER) that came with a pass open: taken inside it (DM_VK_BAR_KEEP), ended it at once, ended it later at a draw that needed it (bar_hazard)
static uint64_t g_bar_in, g_bar_kept, g_bar_late;
void vka_rp_end(void) {
    if (PC.n) pc_flush();
    if (!R.on) return;
    if (R.classic) vk.vkCmdEndRenderPass(g_cb); else vk.vkCmdEndRendering(g_cb);
    vka_barrier(g_cb);   // the pass's writes before whatever reads them next (copies, the present, the next pass's samples)
    R.on = 0; vka_tex_feedback(NULL, NULL, 0);
    if (vka_nan_on()) vka_nan_pass(g_cb, R.cim, R.clv, R.cl0, R.nci);
}
static void rt_range(const vka_img *im, const view_t *v, int has_v, uint32_t *lv, uint32_t *l0, uint32_t *nl) {
    *lv = has_v && v->set_levels ? v->base_level : 0;
    *l0 = has_v && v->set_layers ? v->min_layer : 0;
    if (!vka_img_lv(im, lv)) { *lv = im->levels; *nl = 0; return; }   // a level the image leaves out (a sparse texture's unmapped levels)
    uint32_t all = vka_img_slices(im, *lv);
    *nl = has_v && v->set_layers && v->nlayers ? v->nlayers : all - (*l0 < all ? *l0 : 0);
    if (*l0 + *nl > all) *nl = *l0 < all ? all - *l0 : 0;
}
// Opens the pass over the bound targets: 0 = it cannot (no target can be an attachment), logged
static int rp_begin(const p_set_rt *rt, int has_rt, const vka_dstate *d) {
    if (R.on) { if (!g_pass_keep || rt_same(rt, has_rt, &R.rt, R.has_rt)) return 1; vka_rp_end(); }   // a new SET_RT of other targets ends the pass at its first draw (exec does not)
    if (PC.n && !rt_same(rt, has_rt, &PC.rt, 1)) pc_flush();                             // clears of other targets: done before
    VkRenderingAttachmentInfo ca[8], da = {0}, sa; uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0;
    vka_img *fb_im[9]; uint32_t fb_lv[9]; int nfb = 0;   // the attachments' images and levels (vka_tex_feedback)
    vka_img *s_im[9] = {0}; uint32_t s_lv[9] = {0}, s_nl[9] = {0};   // per colour slot and the depth / stencil target (8): for a classic pass's framebuffer
    memset(&R, 0, sizeof R);
    if (has_rt) R.rt = *rt; R.has_rt = has_rt;
    int n = has_rt ? (rt->n < 8 ? rt->n : 8) : 0;
    for (int i = 0; i < n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        R.cf[i] = VK_FORMAT_UNDEFINED;
        const tex_t *t = rt->color[i]; if (!t) continue;
        vka_img *im = vka_tex_get(t, g_cb); vka_fmt f;
        if (!im || !vka_fmt_of(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : t->d.format, &f)) continue;
        if (!(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || f.kind >= 3) { VRED_ONCE("colour target %d (format %#x) cannot be a Vulkan colour attachment: not drawn into", i, t->d.format); continue; }
        if (im->depth > 1 && !(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;   // its slices are drawn into through 2D-array views (img_create)
        uint32_t lv, l0, nl; rt_range(im, &rt->cview[i], rt->has_cview[i], &lv, &l0, &nl);
        if (lv >= im->levels || !nl) continue;
        if (samples && im->samples != samples) { VRED_ONCE("colour targets of different sample counts"); continue; }
        vka_tex_own(im, g_cb);   // (a zero-shared image gets memory of its own before it is drawn into: no pass is open here)
        VkImageView v = vka_view(im, f.view, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, l0, nl, NULL);
        if (!v) continue;
        ca[i].imageView = v; R.cf[i] = f.view; R.ckind[i] = f.kind; samples = im->samples; fb_im[nfb] = im; fb_lv[nfb++] = lv; s_im[i] = im; s_lv[i] = lv; s_nl[i] = nl;
        R.cim[R.nci] = im; R.clv[R.nci] = lv; R.cl0[R.nci++] = l0;
        uint32_t lw = im->w >> lv ? im->w >> lv : 1, lh = im->h >> lv ? im->h >> lv : 1;
        R.cnl[i] = nl; R.cext[i] = (VkExtent2D){lw, lh};
        if (lw < w) w = lw; if (lh < h) h = lh; if (nl < layers) layers = nl;
        vka_img_written(im);
    }
    R.ncolor = (uint32_t)n;
    VkRenderingAttachmentInfo *dp = NULL, *sp = NULL;
    if (has_rt && rt->depth) {
        vka_img *im = vka_tex_get(rt->depth, g_cb);
        if (im && im->f.kind >= 3 && (im->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && (!samples || im->samples == samples)) {
            uint32_t lv, l0, nl; rt_range(im, &rt->dview, rt->has_dview, &lv, &l0, &nl);
            if (lv < im->levels && nl) vka_tex_own(im, g_cb);
            VkImageView v = lv < im->levels && nl ? vka_view(im, im->f.vk, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, im->aspect, lv, 1, l0, nl, NULL) : VK_NULL_HANDLE;
            if (v) {
                da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
                if (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { dp = &da; R.df = im->f.vk; }
                if (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sp = &da; R.sf = im->f.vk; }
                samples = im->samples; fb_im[nfb] = im; fb_lv[nfb++] = lv; s_im[8] = im; s_lv[8] = lv; s_nl[8] = nl;
                uint32_t lw = im->w >> lv ? im->w >> lv : 1, lh = im->h >> lv ? im->h >> lv : 1;
                R.dnl = nl; R.dext = (VkExtent2D){lw, lh};
                if (lw < w) w = lw; if (lh < h) h = lh; if (nl < layers) layers = nl;
                vka_img_written(im);
            }
        } else if (im) VRED_ONCE("depth target (format %#x) cannot be a Vulkan depth attachment: drawn without depth", rt->depth->d.format);
    }
    if (w == UINT32_MAX) {   // no attachment at all (rasterizer discard, or targets Vulkan cannot take): the viewport's size, for side effects
        if (!d->has_vp) { VRED_ONCE("a draw with no render target and no viewport: skipped"); return 0; }
        w = (uint32_t)fabsf(d->vp[0][2]); h = (uint32_t)fabsf(d->vp[0][3]); if (!w) w = 1; if (!h) h = 1; layers = 1;
    }
    R.area = (VkRect2D){{0, 0}, {w, h}}; R.layers = layers; R.samples = samples ? samples : 1;
    // The pending clears of these targets (PC): loadOp CLEAR when one covers its attachment and the attachment is the render area, else vkCmdClearAttachments once the pass
    // begins when the rectangle is inside the render area and the attachment has the pass's layers, else outside before it begins
    int nlate = 0; vka_pclear *late[NPC];
    sa = da;
    for (int k = 0; k < PC.n; k++) {
        vka_pclear *c = &PC.c[k]; int ci = c->ci;
        int att = ci >= 0 ? ci < n && ca[ci].imageView && R.cnl[ci] == layers && R.cnl[ci] == c->nl
                          : R.dnl == layers && R.dnl == c->nl && (!(c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) || dp) && (!(c->asp & VK_IMAGE_ASPECT_STENCIL_BIT) || sp);
        VkExtent2D e = ci >= 0 ? (ci < 8 ? R.cext[ci] : (VkExtent2D){0, 0}) : R.dext;
        int whole = att && !c->rc.offset.x && !c->rc.offset.y && c->rc.extent.width == w && c->rc.extent.height == h && e.width == w && e.height == h;
        int inarea = att && (uint32_t)c->rc.offset.x + c->rc.extent.width <= w && (uint32_t)c->rc.offset.y + c->rc.extent.height <= h;
        if (whole && !nlate) {   // (after a late one it would come first: late too)
            if (ci >= 0) { ca[ci].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[ci].clearValue = c->in; }
            else { if (c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) { da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.clearValue.depthStencil.depth = c->in.depthStencil.depth; }
                   if (c->asp & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa.clearValue.depthStencil.stencil = c->in.depthStencil.stencil; } }
        } else if (inarea) late[nlate++] = c;
        else vka_clear_outside(c);
    }
    if (sp) sp = &sa;
    #undef INAREA
    #undef WHOLE
    vka_tex_feedback(fb_im, fb_lv, nfb);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = R.area, .layerCount = layers, .colorAttachmentCount = (uint32_t)n, .pColorAttachments = ca,
                          .pDepthAttachment = dp, .pStencilAttachment = sp};
    if (vka_gp_active) {   // DM_VK_GPUPROF: what the pass is, for the step it ends
        char b[96]; size_t bn = (size_t)snprintf(b, sizeof b, "%ux%ux%u", w, h, layers);
        for (int i = 0; i < n && bn < sizeof b; i++) if (ca[i].imageView) bn += (size_t)snprintf(b + bn, sizeof b - bn, " c%d:%d%s", i, R.cf[i], ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "C" : "");
        if (dp || sp) snprintf(b + bn, bn < sizeof b ? sizeof b - bn : 0, " d:%d%s", R.df ? R.df : R.sf, (dp ? dp : sp)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "C" : "");
        vka_gp_pass(b);
    }
    R.classic = 0;
    if (vka_classic_rp()) {   // DM_VK_CLASSIC_RP: the same pass as a VkRenderPass with an imageless framebuffer
        rpkey k; memset(&k, 0, sizeof k); fbkey fk; memset(&fk, 0, sizeof fk);
        VkImageView views[9]; VkClearValue cv[9]; uint32_t na = 0;
        k.n = (uint32_t)n; k.samples = R.samples;
        for (int i = 0; i < n; i++) {
            if (!ca[i].imageView) { k.cf[i] = VK_FORMAT_UNDEFINED; continue; }
            k.cf[i] = R.cf[i]; if (ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) k.clear |= 1u << i;
            fk.a[na] = fb_att(s_im[i], s_lv[i], s_nl[i], R.cf[i]); views[na] = ca[i].imageView; cv[na++] = ca[i].clearValue;
        }
        if (dp || sp) {
            k.ds = R.df ? R.df : R.sf; k.d = dp != NULL; k.s = sp != NULL;
            if (dp && dp->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) k.clear |= 1u << 8; if (sp && sp->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) k.clear |= 1u << 9;
            fk.a[na] = fb_att(s_im[8], s_lv[8], s_nl[8], k.ds); views[na] = da.imageView;
            cv[na].depthStencil.depth = da.clearValue.depthStencil.depth; cv[na++].depthStencil.stencil = sa.clearValue.depthStencil.stencil;
        }
        VkRenderPass rp = rp_obtain(&k);
        fk.rp = rp; fk.w = R.area.extent.width; fk.h = R.area.extent.height; fk.layers = layers; fk.na = na;
        VkFramebuffer fb = rp && s_im[0] != (vka_img *)1 ? fb_obtain(&fk) : VK_NULL_HANDLE;
        if (fb) {
            VkRenderPassAttachmentBeginInfo ab = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, NULL, na, views};
            VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .pNext = na ? &ab : NULL, .renderPass = rp, .framebuffer = fb, .renderArea = R.area, .clearValueCount = na, .pClearValues = cv};
            vk.vkCmdBeginRenderPass(g_cb, &rb, VK_SUBPASS_CONTENTS_INLINE); R.classic = 1;
        }
    }
    if (!R.classic) vk.vkCmdBeginRendering(g_cb, &ri);
    R.on = 1; vka_n_rp++; if (g_dyn_filter == 2) vka_dyn_invalidate();   // DM_VK_DYN_FILTER=2: the dynamic state set again at each pass (bisect)
    for (int k = 0; k < nlate; k++) { VkClearAttachment c = {late[k]->asp, late[k]->ci >= 0 ? (uint32_t)late[k]->ci : 0, late[k]->in}; VkClearRect cr = {late[k]->rc, 0, layers}; vk.vkCmdClearAttachments(g_cb, 1, &c, 1, &cr); }
    PC.n = 0;
    return 1;
}
int vka_pass_keep(void) { return g_pass_keep; }
// A game barrier (nvnCommandBufferBarrier) with a draw pass open. NVN orders the work before it against the work after it (ORDER_PRIMITIVES / ORDER_FRAGMENTS, the cache
// invalidations); the replay used to end the pass there and record a full barrier, so every barrier between draws into the same targets cost a pass end, a store and a
// load of every attachment on a tiler (the next draw reopens it with LOAD). Inside a pass Vulkan already keeps the attachment writes of draws in rasterization order, so
// what a barrier between two draws of one pass can order is: memory the earlier draws wrote with shader stores (the pass then ends at once, as nvn_mtl.m's OP_BARRIER
// does for a pass with shader writes), and an attachment of the pass that a later draw samples (a feedback read: the pass ends before that draw, bar_hazard). Everything
// else the draws read was written before the pass began, behind the barrier that ended what wrote it. DM_VK_BAR_KEEP=1 takes such barriers inside the pass (live;
// default 0 until phone-checked); the counts are in the replay statistics line either way
int vka_rp_game_barrier(uint32_t bits) {
    (void)bits;
    if (!R.on) return 0;
    g_bar_in++;
    if (R.swrite || !VKA_ENV("DM_VK_BAR_KEEP", 0)) return 0;
    R.bar_pending = 1; g_bar_kept++;
    return 1;
}
void vka_rp_bar_stats(char *b, size_t n, double frames) {
    static uint64_t i0, k0, l0;
    if (frames <= 0 || g_bar_in == i0) { if (n) b[0] = 0; i0 = g_bar_in; k0 = g_bar_kept; l0 = g_bar_late; return; }
    snprintf(b, n, "game barriers inside a draw pass per frame: %.1f, %.1f taken inside it (DM_VK_BAR_KEEP), %.1f of those ended it later at a draw sampling its targets or writing memory",
             (g_bar_in - i0) / frames, (g_bar_kept - k0) / frames, (g_bar_late - l0) / frames);
    i0 = g_bar_in; k0 = g_bar_kept; l0 = g_bar_late;
}
int vka_rp_is_open(void) { return R.on; }
int vka_rp_open_for(const p_set_rt *rt, int has_rt) { return R.on && rt_same(rt, has_rt, &R.rt, R.has_rt); }
// The draw about to come may start a new part of the submission: no pass is open over its targets and no clear waits for them (an open pass over other targets ends at it anyway)
int vka_rp_split_ok(const p_set_rt *rt, int has_rt) { return R.on ? !rt_same(rt, has_rt, &R.rt, R.has_rt) : PC.n ? has_rt && !rt_same(rt, has_rt, &PC.rt, 1) : 1; }
void vka_draw_cb_switch(VkCommandBuffer cb) { g_cb = cb; vka_dyn_invalidate(); }   // the submission goes on in another command buffer (vka_exec.c chunk): its bound state is gone
// Before a clear of these targets records anything (vka_tex_get may record a first upload): a pass over other targets ends, pending clears of other targets are done
void vka_rp_prep_clear(const p_set_rt *rt, int has_rt) {
    if (R.on) { if (!rt_same(rt, has_rt, &R.rt, R.has_rt)) vka_rp_end(); }
    else if (PC.n && !rt_same(rt, has_rt, &PC.rt, 1)) pc_flush();
}
// A clear of one of the bound targets (colour index ci, or depth / stencil when ci < 0) inside the render pass: an open pass over the same targets takes it as
// vkCmdClearAttachments (rasterization-ordered with its draws), a closed one opens over them with it (loadOp CLEAR). 0 = not here: the caller ends the pass and clears outside.
// rc is in the attachment's level (the scissored part), nl the layers the clear names: they must be the attachment's own, within the render area
int vka_rp_clear(const p_set_rt *rt, int has_rt, const vka_dstate *d, const vka_pclear *c) {
    (void)d;
    if (!g_cb || !has_rt || !draws_on()) return 0;
    if (R.on && !rt_same(rt, has_rt, &R.rt, R.has_rt)) return 0;
    if (!R.on) {
        if (PC.n && (!rt_same(rt, has_rt, &PC.rt, 1) || PC.n == NPC)) pc_flush();
        if (!PC.n) PC.rt = *rt;
        PC.c[PC.n++] = *c;
        return 1;
    }
    int ci = c->ci; uint32_t nl = c->nl; VkRect2D rc = c->rc;
    if (ci >= 0 ? ci >= 8 || !R.cf[ci] || R.cnl[ci] != nl || nl != R.layers : ((c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) && !R.df) || ((c->asp & VK_IMAGE_ASPECT_STENCIL_BIT) && !R.sf) || R.dnl != nl || nl != R.layers) return 0;
    if ((uint32_t)rc.offset.x + rc.extent.width > R.area.extent.width || (uint32_t)rc.offset.y + rc.extent.height > R.area.extent.height) return 0;
    VkClearAttachment ca = {c->asp, ci >= 0 ? (uint32_t)ci : 0, c->in}; VkClearRect cr = {rc, 0, nl};
    vk.vkCmdClearAttachments(g_cb, 1, &ca, 1, &cr);
    return 1;
}

// ---------- Formats and fixed-function mappings ----------
// NVN vertex formats 0x01-0x30: four rows (1-4 components) of {UNORM8, SNORM8, UINT8, SINT8, FLOAT16, UNORM16, SNORM16, UINT16, SINT16, FLOAT32, UINT32, SINT32}
#define VF(n, s) VK_FORMAT_##n##_##s
static const VkFormat VROW[4][12] = {
    {VF(R8, UNORM), VF(R8, SNORM), VF(R8, UINT), VF(R8, SINT), VF(R16, SFLOAT), VF(R16, UNORM), VF(R16, SNORM), VF(R16, UINT), VF(R16, SINT), VF(R32, SFLOAT), VF(R32, UINT), VF(R32, SINT)},
    {VF(R8G8, UNORM), VF(R8G8, SNORM), VF(R8G8, UINT), VF(R8G8, SINT), VF(R16G16, SFLOAT), VF(R16G16, UNORM), VF(R16G16, SNORM), VF(R16G16, UINT), VF(R16G16, SINT), VF(R32G32, SFLOAT), VF(R32G32, UINT), VF(R32G32, SINT)},
    {VF(R8G8B8, UNORM), VF(R8G8B8, SNORM), VF(R8G8B8, UINT), VF(R8G8B8, SINT), VF(R16G16B16, SFLOAT), VF(R16G16B16, UNORM), VF(R16G16B16, SNORM), VF(R16G16B16, UINT), VF(R16G16B16, SINT), VF(R32G32B32, SFLOAT), VF(R32G32B32, UINT), VF(R32G32B32, SINT)},
    {VF(R8G8B8A8, UNORM), VF(R8G8B8A8, SNORM), VF(R8G8B8A8, UINT), VF(R8G8B8A8, SINT), VF(R16G16B16A16, SFLOAT), VF(R16G16B16A16, UNORM), VF(R16G16B16A16, SNORM), VF(R16G16B16A16, UINT), VF(R16G16B16A16, SINT), VF(R32G32B32A32, SFLOAT), VF(R32G32B32A32, UINT), VF(R32G32B32A32, SINT)},
};
static const VkFormat VSCALED[4][2][2] = {   // [components][8 / 16 bits][unsigned / signed]
    {{VF(R8, USCALED), VF(R8, SSCALED)}, {VF(R16, USCALED), VF(R16, SSCALED)}}, {{VF(R8G8, USCALED), VF(R8G8, SSCALED)}, {VF(R16G16, USCALED), VF(R16G16, SSCALED)}},
    {{VF(R8G8B8, USCALED), VF(R8G8B8, SSCALED)}, {VF(R16G16B16, USCALED), VF(R16G16B16, SSCALED)}}, {{VF(R8G8B8A8, USCALED), VF(R8G8B8A8, SSCALED)}, {VF(R16G16B16A16, USCALED), VF(R16G16B16A16, SSCALED)}}};
#undef VF
// The Vulkan format for NVN vertex format f whose shader input has class want (DMSH_VK_TYPE_*): the format's own when its class matches, else the same layout in that class
// (Vulkan wants the classes equal). An integer format into a float input keeps its own format and *retype says the input's class must change (DMSH_VK_TYPE_SINT / UINT):
// Maxwell's fetch puts the integer's bits in the register the shader bit-casts, so the vertex module is rewritten to declare that input as an integer vector (vka_spvpatch.c).
// VK_FORMAT_UNDEFINED = not mapped
static VkFormat vin_format(int f, int want, int *changed, int *retype) {
    *changed = 0; *retype = 0;
    int n = 0, col = -1;   // row (components) and column of VROW, or a scaled / packed format
    if (f >= 0x01 && f <= 0x30) { n = (f - 1) / 12 + 1; col = (f - 1) % 12; }
    else if (f >= 0x51 && f <= 0x68) {   // integers read as floats (nvn_mtl.m vconv_i2f): USCALED / SSCALED, none for 32 bits
        int i = (f - 0x51) >> 1, sg = (f - 0x51) & 1, bits = 8 << (i % 3); n = i / 3 + 1;
        if (bits < 32 && want == DMSH_VK_TYPE_FLOAT) return VSCALED[n - 1][bits == 16][sg];
        col = bits == 8 ? 2 + sg : bits == 16 ? 7 + sg : 10 + sg; *changed = want == DMSH_VK_TYPE_FLOAT;
        if (want == DMSH_VK_TYPE_FLOAT) return VROW[n - 1][col];
    } else {
        VkFormat p = f == 0x3D ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : f == 0x69 ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : f == 0x3F ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : f == 0x9A ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
        if (p && want != DMSH_VK_TYPE_FLOAT) { *changed = 1; return want == DMSH_VK_TYPE_UINT ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SINT; }
        return p;
    }
    static const int8_t cls[12] = {0, 0, 2, 1, 0, 0, 0, 2, 1, 0, 2, 1};   // DMSH_VK_TYPE_* of each column
    if (cls[col] == want) return VROW[n - 1][col];
    if (want == DMSH_VK_TYPE_FLOAT && retype) { *retype = cls[col]; return VROW[n - 1][col]; }
    *changed = 1;
    int bits = col < 4 ? 8 : col < 9 ? 16 : 32, sg = col == 1 || col == 3 || col == 6 || col == 8 || col == 11;
    if (want == DMSH_VK_TYPE_FLOAT) return bits < 32 ? VSCALED[n - 1][bits == 16][sg] : VROW[n - 1][9];
    int base = bits == 8 ? 2 : bits == 16 ? 7 : 10;   // the integer of the same width, in the class asked
    return VROW[n - 1][base + (want == DMSH_VK_TYPE_SINT)];
}
static VkBlendFactor bfac(int f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp bop(int e) { return e >= 1 && e <= 5 ? (VkBlendOp)(e - 1) : VK_BLEND_OP_ADD; }
static int is_src1(int f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
static int zero_src1(int f) { return f == NVN_BLEND_FUNC_SRC1_COLOR || f == NVN_BLEND_FUNC_SRC1_ALPHA ? NVN_BLEND_FUNC_ZERO : f == NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR || f == NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA ? NVN_BLEND_FUNC_ONE : f; }
// An advanced blend mode as fixed-function factors where one exists (nvn_mtl.m adv_blend: the Porter-Duff modes with uncorrelated overlap)
static blend_st adv_blend(blend_st b) {
    if (!b.adv) return b;
    enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SA = NVN_BLEND_FUNC_SRC_ALPHA, ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, DA = NVN_BLEND_FUNC_DST_ALPHA, IDA = NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA };
    static const uint8_t pd[][4] = {
        [NVN_BLEND_ADVANCED_MODE_ZERO] = {Z, Z, Z, Z},        [NVN_BLEND_ADVANCED_MODE_SRC] = {O, Z, O, Z},           [NVN_BLEND_ADVANCED_MODE_DST] = {Z, O, Z, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_OVER] = {O, ISA, O, ISA}, [NVN_BLEND_ADVANCED_MODE_DST_OVER] = {IDA, O, IDA, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_IN] = {DA, Z, DA, Z},     [NVN_BLEND_ADVANCED_MODE_DST_IN] = {Z, SA, Z, SA},
        [NVN_BLEND_ADVANCED_MODE_SRC_OUT] = {IDA, Z, IDA, Z},  [NVN_BLEND_ADVANCED_MODE_DST_OUT] = {Z, ISA, Z, ISA},
        [NVN_BLEND_ADVANCED_MODE_SRC_ATOP] = {DA, ISA, Z, O},  [NVN_BLEND_ADVANCED_MODE_DST_ATOP] = {IDA, SA, O, Z},
        [NVN_BLEND_ADVANCED_MODE_XOR] = {IDA, ISA, IDA, ISA},  [NVN_BLEND_ADVANCED_MODE_PLUS] = {O, O, O, O},
        [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED] = {O, O, O, O}, [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA] = {O, O, O, O},
    };
    const uint8_t *f = b.adv < sizeof pd / sizeof *pd && pd[b.adv][0] ? pd[b.adv] : NULL;
    if (!f || b.adv_ovl || (b.adv_npm && f[0] != O && f[0] != Z)) { VRED_ONCE("advanced blend mode %#x has no fixed-function equivalent: the state's blend functions are used", (unsigned)b.adv); return b; }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0]; b.dst_rgb = f[1]; b.src_a = f[2]; b.dst_a = f[3];
    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
    return b;
}
// NVN primitive → Vulkan topology; *expand: 1 quads, 2 line loop (indices made on the CPU), 0 none; -1 = not drawable
static VkPrimitiveTopology topology(int prim, int *expand) {
    *expand = 0;
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: *expand = 2; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   // a quad strip rasterizes as the triangle strip of its vertices
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_QUADS: *expand = 1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    *expand = -1; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
}
static VkPrimitiveTopology topo_class(VkPrimitiveTopology t) {   // the pipeline's topology: one of each class, the draw sets its own dynamically
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return t;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return t;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// ---------- Guest memory as descriptors ----------
static int cpu_read(uint64_t addr, void *out, size_t n) {   // guest bytes as the CPU sees them now
    if (vka_addr(addr, n, NULL, NULL, NULL)) { memcpy(out, (const void *)(uintptr_t)addr, n); return 1; }
    return !plat_mem_read(addr, out, n);
}
static int rd_cb(void *ctx, uint64_t addr, void *out, size_t n) { (void)ctx; return cpu_read(addr, out, n); }
// A uniform buffer: bound in place when Vulkan's offset alignment allows, else a copy of what the CPU sees now (a GPU write earlier in this submission is missed: logged)
static int ubo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *bi) {
    if (!size || size > V.ub_range) size = size ? V.ub_range : 65536 < V.ub_range ? 65536 : V.ub_range;
    VkBuffer b; VkDeviceSize o;
    uint64_t run = vka_addr_run(addr, size, &b, &o);
    if (run && !(o % V.ub_align) && !(addr % V.ub_align)) { *bi = (VkDescriptorBufferInfo){b, o, run}; return 1; }
    vka_span s; if (!vka_scratch(size, V.ub_align, &s)) return 0;
    if (!cpu_read(addr, s.ptr, size)) { memset(s.ptr, 0, size); VRED_ONCE("uniform buffer at %#llx is not readable: it reads zeros", (unsigned long long)addr); }
    else if (run) VRED_ONCE("uniform buffer at %#llx is not %llu-byte aligned: bound as a copy of the CPU's bytes", (unsigned long long)addr, (unsigned long long)V.ub_align);
    else VRED_ONCE("uniform buffer at %#llx is not in placed guest RAM: bound as a copy of the CPU's bytes", (unsigned long long)addr);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, size}; return 1;
}
static int ssbo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *bi) {
    VkBuffer b; VkDeviceSize o;
    uint64_t run = vka_addr_run(addr, size ? size : V.sb_range, &b, &o);
    if (!run) { VRED_ONCE("storage buffer at %#llx is not in placed guest RAM: it is not bound", (unsigned long long)addr); return 0; }
    if (run > V.sb_range) run = V.sb_range;
    *bi = (VkDescriptorBufferInfo){b, o, run}; return 1;
}
// Driver segments (c0, c2, the push bank) and raw global memory's table, kept from the last draw of this submission that filled them: a draw whose segment holds the same
// bytes binds the same scratch span (DM_VK_DRV_MEMO, live, default 1). Each draw used to fill a fresh 2.5 KB span per segment it reads (memset and fill into scratch, under
// the scratch lock: ~10 KB of writes a draw, ~80 MB a frame in DKB's level), so no two draws' pushes were ever the same and the push dedupe (push_same) never matched them.
// Graphics stages only: an indirect dispatch's counts are copied into its c0 by the GPU (vka_dispatch). A span lives until the submission's fence (vka_scratch_retire at
// its commit), so the memo is forgotten at each submission's start (vka_draw_sub_begin)
typedef struct { VkBuffer buf; VkDeviceSize off; } c0_at;
static struct { int valid; vka_span s; uint8_t b[DMSH_DRV_SIZE]; } g_dm[VKA_NSTAGE][3];
static struct { int valid; uint64_t gen; vka_span s; } g_gm;
static struct { int valid; uint32_t ref; vka_span s; } g_sm_ref;   // the support buffer with the alpha test's reference (stage_writes)
static int g_drv_memo = 1; static uint64_t g_dm_hit, g_dm_miss;
static void drv_memo_reset(void) { for (int st = 0; st < VKA_NSTAGE; st++) for (int b = 0; b < 3; b++) g_dm[st][b].valid = 0; g_gm.valid = 0; g_sm_ref.valid = 0; g_drv_memo = VKA_ENV("DM_VK_DRV_MEMO", 1); }
static int drv_seg(const vka_dstate *d, int st, int bank, int pool, VkDescriptorBufferInfo *bi, c0_at *c0) {
    size_t sz = vka_drv_size(bank); vka_span s;
    // One memo slot per segment kind: c0, c2 and the push bank. Any other driver bank (an all-zero c17-style read of DMSH_DRV_SIZE bytes) takes the fill below: sharing
    // the push bank's slot, an all-zero push block matched it and the 0x100-byte span was bound with a 0xA00-byte range (other draws' scratch, or past the buffer)
    int bi_ = bank == 0 ? 0 : bank == 2 ? 1 : bank == DMSH_PUSH_BANK ? 2 : -1;
    if (g_drv_memo && bi_ >= 0 && st != 5 && sz <= DMSH_DRV_SIZE) {
        static uint8_t tmp[DMSH_DRV_SIZE];
        vka_drv_fill(&d->b, st, bank, pool, tmp);
        typeof(g_dm[0][0]) *m = &g_dm[st][bi_];
        if (m->valid && !memcmp(m->b, tmp, sz)) { g_dm_hit++; *bi = (VkDescriptorBufferInfo){m->s.buf, m->s.off, sz}; return 1; }
        if (!vka_scratch(sz, V.ub_align, &s)) return 0;
        memcpy(s.ptr, tmp, sz); memcpy(m->b, tmp, sz); m->s = s; m->valid = 1; g_dm_miss++;
        *bi = (VkDescriptorBufferInfo){s.buf, s.off, sz};
        return 1;
    }
    if (!vka_scratch(sz, V.ub_align, &s)) return 0;
    vka_drv_fill(&d->b, st, bank, pool, s.ptr);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, sz};
    if (bank == 0 && c0) { c0->buf = s.buf; c0->off = s.off; }
    return 1;
}
// Raw global memory's table (dmsh_abi.h DMSH_GMEM_MAP_*): entry 0 empty, then the placed chunks by address with delta = device address - guest address, unused entries at ~0
// (kept while no segment comes or goes: vka_mem_gen, and DM_VK_DRV_MEMO)
uint64_t vka_mem_gen(void);
static int gmem_map(VkDescriptorBufferInfo *bi) {
    size_t n = (size_t)DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY;
    uint64_t gen = vka_mem_gen();
    if (g_drv_memo && g_gm.valid && g_gm.gen == gen) { *bi = (VkDescriptorBufferInfo){g_gm.s.buf, g_gm.s.off, n}; return 1; }
    vka_span s; if (!vka_scratch(n, V.sb_align, &s)) return 0;
    memset(s.ptr, 0xff, n); memset(s.ptr, 0, DMSH_GMEM_MAP_ENTRY);
    static uint64_t lo[DMSH_GMEM_MAP_ENTRIES], hi[DMSH_GMEM_MAP_ENTRIES], bda[DMSH_GMEM_MAP_ENTRIES];
    int k = vka_mem_chunks(lo, hi, bda, DMSH_GMEM_MAP_ENTRIES - 1);
    for (int i = 0; i < k && i + 1 < DMSH_GMEM_MAP_ENTRIES; i++) { uint64_t e[4] = {lo[i], hi[i], bda[i] - lo[i], 0}; memcpy(s.ptr + (size_t)(i + 1) * DMSH_GMEM_MAP_ENTRY, e, sizeof e); }
    g_gm.valid = 1; g_gm.gen = gen; g_gm.s = s;   // (a segment that came between the read of gen and the table: the next draw sees a new gen and builds it again)
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, n}; return 1;
}
// A compute program's shared memory past the device's limit (DMSH_VK_KIND_SHARED_SPILL; Ryujinx StructuredIr/SharedSpillInfo.cs): one buffer for the process
// (DM_VK_SHARED_SPILL_MB, default 16 MiB, at least one slice), reused by every such dispatch: each dispatch ends with a barrier (vka_dispatch), so the next one finds it free.
// A dispatch binds slices for as many workgroups as it has, or as the buffer holds; workgroup w takes slice w mod the slices bound. A dispatch with more workgroups than
// that runs in parts (vkCmdDispatchBase, a barrier between them), each part's linear indices a contiguous run no longer than the slices, so no two workgroups that may run
// at once share a slice. An indirect dispatch's group count is known only on the GPU: it gets the whole buffer (open: correct while no two workgroups the buffer's slice
// count apart run at once, which in-order workgroup launch makes very likely but Vulkan does not promise)
static vka_span g_spill; static VkDeviceSize g_spill_cap, g_spill_range;
static uint32_t spill_bytes(const vka_shader *cs) {
    for (unsigned i = 0; i < cs->e->bind_count; i++) if (cs->b[i].kind == DMSH_VK_KIND_SHARED_SPILL) return cs->b[i].bytes;
    return 0;
}
static int spill_buf(uint32_t slice) {
    static int failed;
    if (!g_spill.buf && !failed) {   // made once, at the first such dispatch (one try: a failure is logged once and their dispatches are skipped)
        VkDeviceSize cap = (VkDeviceSize)VKA_ENV("DM_VK_SHARED_SPILL_MB", 16) << 20, lim = V.props.limits.maxStorageBufferRange;
        if (lim && cap > lim) cap = lim;
        if (cap < (VkDeviceSize)DMSH_VK_SHARED_MAX * 2) cap = (VkDeviceSize)DMSH_VK_SHARED_MAX * 2;   // at least one slice of any program the build moves (they declare under twice the limit)
        if ((lim && cap > lim) || !vka_static(cap, V.sb_align, &g_spill)) { failed = 1; g_spill.buf = VK_NULL_HANDLE; VRED("no shared memory spill buffer of %llu bytes", (unsigned long long)cap); return 0; }
        g_spill_cap = cap;
        VLOG("shared memory spill buffer: %llu KiB (%llu slices of the first program's %u bytes)", (unsigned long long)(cap >> 10), (unsigned long long)(cap / slice), slice);
    }
    return g_spill.buf && g_spill_cap >= slice;
}
static void spill_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t ns) {
    int first = 1;
    #define PART(bx, by, bz, cx, cy, cz) do { if (!first) vka_barrier(g_cb); first = 0; vk.vkCmdDispatchBase(g_cb, bx, by, bz, cx, cy, cz); } while (0)
    #define MIN(a, b) ((a) < (b) ? (a) : (b))
    if ((uint64_t)x * y <= ns) { uint32_t c = (uint32_t)(ns / ((uint64_t)x * y)); for (uint32_t z0 = 0; z0 < z; z0 += c) PART(0, 0, z0, x, y, MIN(c, z - z0)); }
    else if (x <= ns) { uint32_t c = (uint32_t)(ns / x); for (uint32_t zz = 0; zz < z; zz++) for (uint32_t y0 = 0; y0 < y; y0 += c) PART(0, y0, zz, x, MIN(c, y - y0), 1); }
    else for (uint32_t zz = 0; zz < z; zz++) for (uint32_t yy = 0; yy < y; yy++) for (uint32_t x0 = 0; x0 < x; x0 += (uint32_t)ns) PART(x0, yy, zz, MIN((uint32_t)ns, x - x0), 1, 1);
    #undef PART
    #undef MIN
}
static int gmem_log(VkDescriptorBufferInfo *bi) {
    if (vka_tex_gml_take(bi)) return 1;   // read when the submission is done: the textures over the pages its stores wrote are uploaded again
    size_t n = ((size_t)1 + (1u << DMSH_GMEM_LOG_LOG2)) * 4;
    vka_span s; if (!vka_scratch(n, V.sb_align, &s)) return 0;
    memset(s.ptr, 0, n);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, n}; return 1;
}

// Placeholders for unbound images when the device has no null descriptors: 1x1 images of the shapes a shader may declare, zero (float, sint and uint views)
VkImageView placeholder(uint8_t dim, int storage) {
    static vka_img *im2, *im3, *im1; static VkImageView cache[3][7][3];
    int cls = dim & DMSH_VK_DIM_SINT ? 1 : dim & DMSH_VK_DIM_UINT ? 2 : 0, d = dim & DMSH_VK_DIM_MASK, a = (dim & DMSH_VK_DIM_ARRAY) != 0;
    if ((dim & DMSH_VK_DIM_MS) || d == 0) return VK_NULL_HANDLE;   // 1D shapes: the replay makes no 1D images (vka_tex.c img_create) and the translator declares none
    VkImageViewType vt = d == 0 ? (a ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : d == 2 ? VK_IMAGE_VIEW_TYPE_3D : d == 3 ? (a ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : a ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (cache[storage][vt][cls]) return cache[storage][vt][cls];
    vka_img **pim = d == 2 ? &im3 : d == 0 ? &im1 : &im2;
    if (!*pim) {   // a texture of our own, made through the same path as the game's (zeroed at first use)
        static tex_t t[3]; tex_t *tt = &t[d == 2 ? 1 : d == 0 ? 2 : 0]; memset(tt, 0, sizeof *tt);
        tt->d.target = d == 2 ? NVN_TEXTURE_TARGET_3D : d == 0 ? NVN_TEXTURE_TARGET_1D_ARRAY : NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; tt->d.format = NVN_FORMAT_RGBA8;
        tt->d.width = 1; tt->d.height = 1; tt->d.depth = d == 2 ? 1 : 6; tt->d.levels = 1; tt->d.flags = NVN_TEXTURE_FLAGS_IMAGE;
        nvn_tex_layout(&tt->d, &tt->L); tt->serial = nvn_new_serial();
        *pim = vka_tex_get(tt, g_cb);
        if (!*pim) return VK_NULL_HANDLE;
    }
    static const VkFormat f[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT};
    uint32_t na = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? (*pim)->layers : 1;
    return cache[storage][vt][cls] = vka_view(*pim, f[cls], vt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, na, NULL);
}

// Where a draw's CPU time goes (DM_VK_PROF=1, logged with the replay statistics): the virtual counter at each phase boundary of vka_draw
static uint64_t g_pt_last, g_pt[9], g_ps[6]; static int g_pt_ph;
static uint64_t g_dtot, g_dn;   // DM_VK_PROF: cycles in the whole of vka_draw (every exit) and the calls that entered it, against the phases (which a draw that returns early does not reach)
static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
// ---------- Set 0 from the walk ----------
typedef struct { VkWriteDescriptorSet w[512]; VkDescriptorBufferInfo bi[512]; VkDescriptorImageInfo ii[512]; VkBufferView bv[512]; uint64_t ad[512]; uint32_t n; } writes_t;   // ad: a guest buffer's address (0 = other)
static writes_t g_w;
static vka_res g_res[512];   // the NaN scan's own walk (DM_VK_NANSCAN_FS)
// One stage's results as descriptor writes. 0 = the draw cannot run (logged)
static int stage_writes(const vka_dstate *d, int st, const vka_shader *sh, const vka_res *r, int n, int pool, c0_at *c0) {
    for (int i = 0; i < n; i++) {
        if (g_w.n == 512) { VRED_ONCE("more than 512 descriptors in one draw"); return 0; }
        uint32_t k = g_w.n;
        VkWriteDescriptorSet *w = &g_w.w[k]; VkDescriptorBufferInfo *bi = &g_w.bi[k]; VkDescriptorImageInfo *ii = &g_w.ii[k];
        g_w.ad[k] = r[i].what == VKA_R_BUF ? r[i].addr : 0;
        *w = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = r[i].binding, .dstArrayElement = r[i].elem, .descriptorCount = 1, .descriptorType = (VkDescriptorType)r[i].desc};
        int ok = 1;
        switch (r[i].desc) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
            int storage = r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            switch (r[i].what) {
            case VKA_R_BUF: ok = storage ? ssbo_desc(r[i].addr, r[i].size, bi) : ubo_desc(r[i].addr, r[i].size, bi);
                if (ok && storage && (r[i].flags & DMSH_VK_BF_RW)) vka_tex_gw_opaque(r[i].addr, bi->range);   // the shader may store into it
                break;
            case VKA_R_C1: if (sh->c1_len) *bi = (VkDescriptorBufferInfo){sh->c1.buf, sh->c1.off, (sh->c1_len + 15) & ~(VkDeviceSize)15}; else ok = 0; break;
            case VKA_R_SUPPORT:
                if (st == 1 && (sh->e->flags & DMSH_VK_F_ALPHA_TEST) && d->color.alpha_func && d->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) {   // word 0: the alpha test's reference
                    vka_span s;
                    if (g_drv_memo && g_sm_ref.valid && !memcmp(&g_sm_ref.ref, &d->alpha_ref, 4)) s = g_sm_ref.s;   // the same reference as the last alpha-tested draw: its copy (DM_VK_DRV_MEMO)
                    else { if (!vka_scratch(VKA_SUPPORT_SIZE, V.ub_align, &s)) return 0;
                           memcpy(s.ptr, g_support.ptr, VKA_SUPPORT_SIZE); memcpy(s.ptr, &d->alpha_ref, 4);
                           g_sm_ref.valid = 1; memcpy(&g_sm_ref.ref, &d->alpha_ref, 4); g_sm_ref.s = s; }
                    *bi = (VkDescriptorBufferInfo){s.buf, s.off, VKA_SUPPORT_SIZE};
                } else *bi = (VkDescriptorBufferInfo){g_support.buf, g_support.off, VKA_SUPPORT_SIZE};
                break;
            case VKA_R_DRV: { uint64_t td_ = g_prof ? cntvct() : 0;
                if (!drv_seg(d, st, r[i].bank, pool, bi, c0)) return 0;
                if (g_prof) g_ps[3] += cntvct() - td_;
                break; }
            case VKA_R_GMEM_MAP: ok = gmem_map(bi); break;
            case VKA_R_GMEM_LOG: ok = gmem_log(bi); break;
            case VKA_R_SHARED_SPILL: ok = st == 5 && g_spill_range && g_spill.buf; if (ok) *bi = (VkDescriptorBufferInfo){g_spill.buf, g_spill.off, g_spill_range}; break;
            default: ok = 0; break;
            }
            if (!ok) { if (V.null_desc) *bi = (VkDescriptorBufferInfo){VK_NULL_HANDLE, 0, VK_WHOLE_SIZE}; else *bi = (VkDescriptorBufferInfo){storage ? g_sink.buf : g_zero.buf, storage ? g_sink.off : g_zero.off, 65536}; }
            w->pBufferInfo = bi;
            break; }
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            int storage = r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; uint64_t ti_ = g_prof ? cntvct() : 0;
            VkImageView v = VK_NULL_HANDLE; VkSampler s = VK_NULL_HANDLE; VkImageLayout lay = VK_IMAGE_LAYOUT_GENERAL;
            if (r[i].desc != VK_DESCRIPTOR_TYPE_SAMPLER) {
                int tid = r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG ? r[i].tid : 0;
                if (tid) v = vka_tex_id_view_rec(tid, g_cb, r[i].dim, storage, &lay);
                // A 2D texture in a 3D or cube slot: what the game leaves bound where a shader's branch does not sample (DKB's materials keep a 1x1 2D texture in their volume slot 12).
                // It reads as unbound, zeros, as nvn_mtl.m's zero 3D / zero cube fallback does; noted once. Other mismatches are reported
                int dd = r[i].dim & DMSH_VK_DIM_MASK, tt = tid && !v ? vka_tex_id_target(tid) : -1;
                if (tid && !v && (dd == 2 || dd == 3) && (tt == NVN_TEXTURE_TARGET_2D || tt == NVN_TEXTURE_TARGET_2D_ARRAY)) {
                    static _Atomic int noted; if (!atomic_exchange(&noted, 1)) { char why[320]; vka_tex_id_desc(tid, why, sizeof why);
                        VLOG("%s binding fallback: shader %016llx binding %u, texture ID %d is a 2D texture (%s): it reads as zeros (the shader may leave it unused)", dd == 2 ? "3D" : "cube", (unsigned long long)sh->key, r[i].binding, tid, why); }
                } else if (tid && !v) { static _Atomic int nm; if (atomic_fetch_add(&nm, 1) < 8) { char why[320]; vka_tex_id_desc(tid, why, sizeof why);
                    VRED("shader %016llx binding %u: texture ID %d has no view of shape %#x%s: it reads as unbound (%s)", (unsigned long long)sh->key, r[i].binding, tid, r[i].dim, storage ? " (storage)" : "", why); } }
                if (!v && !V.null_desc) v = placeholder(r[i].dim, storage);
                if (!v && !V.null_desc) { VRED_ONCE("an unbound image of shape %#x and no null descriptors: the draw is skipped", r[i].dim); return 0; }
            }
            if (r[i].desc == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || r[i].desc == VK_DESCRIPTOR_TYPE_SAMPLER) {
                s = r[i].sid ? vka_samp_id_rec(r[i].sid) : VK_NULL_HANDLE;
                if (!s) s = vka_samp_default();
            }
            *ii = (VkDescriptorImageInfo){s, v, lay};
            w->pImageInfo = ii; if (g_prof) { g_ps[4] += cntvct() - ti_; g_ps[5]++; }
            break; }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (r[i].what != VKA_R_NULL) VRED_ONCE("texel buffers are not on Vulkan yet (shader %016llx binding %u): bound as null", (unsigned long long)sh->key, r[i].binding);
            if (!V.null_desc) { VRED_ONCE("a texel buffer binding and no null descriptors: the draw is skipped"); return 0; }
            g_w.bv[k] = VK_NULL_HANDLE; w->pTexelBufferView = &g_w.bv[k];
            break;
        default: VRED_ONCE("descriptor type %u in a binding record", r[i].desc); return 0;
        }
        g_w.n++;
    }
    return 1;
}
// Each stage's bindings, walked once a draw or dispatch (walk_all): the images first used are made from them (make_images), then set 0 is written from them (bind_sets)
static struct { int n[VKA_NSTAGE], pool[VKA_NSTAGE]; char why[VKA_NSTAGE][200]; vka_res r[VKA_NSTAGE][512]; } g_wk;
static void walk_all(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE]) {
    uint64_t t0_ = g_prof ? cntvct() : 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        vka_shader *s = sh[st]; g_wk.n[st] = 0; g_wk.pool[st] = 0; if (!s) continue;
        g_wk.n[st] = vka_walk(&d->b, st, s->e, s->b, s->lib->h->ssbo_align, V.sb_align, rd_cb, NULL, g_wk.r[st], 512, &g_wk.pool[st], g_wk.why[st], sizeof g_wk.why[st]);
    }
    if (g_prof) g_ps[0] += cntvct() - t0_;
}
// Writes set 0 from the walk (walk_all, just before) and binds it (and set 1 for shaders that read the pools). c0: where the compute stage's c0 went (an indirect dispatch copies its counts there)
// The last push descriptor set per bind point, as the words that decide it (layout, each write's binding, type and count, each descriptor's info): a draw whose writes
// are the same word for word skips its push (DM_VK_PUSH_DEDUP=0: every draw pushes). Pushed descriptors stay bound across draws and pipeline binds while the layout stays
// the same (Vulkan's pipeline layout compatibility); a new command buffer or a set bound in their place forgets them (vka_dyn_invalidate, bind_sets)
static struct { uint64_t *w; size_t n, cap; } g_pd[2];
static uint64_t g_pd_skip, g_pd_push, g_pd_skip_all, g_pd_push_all, g_pd_delta_w, g_pd_delta_all;
// The replay statistics' line: driver segments bound again (DM_VK_DRV_MEMO) and pushes skipped (DM_VK_PUSH_DEDUP) since the last line
void vka_drv_memo_stats(char *b, size_t n) {
    static uint64_t h0, m0, s0, p0;
    uint64_t h = g_dm_hit - h0, m = g_dm_miss - m0, sk = g_pd_skip_all - s0, pu = g_pd_push_all - p0;
    h0 = g_dm_hit; m0 = g_dm_miss; s0 = g_pd_skip_all; p0 = g_pd_push_all;
    if (!h && !m && !sk && !pu) { if (n) b[0] = 0; return; }
    snprintf(b, n, "descriptors: driver segments %.0f %% the same bytes as the draw before (bound again: DM_VK_DRV_MEMO); push descriptor sets %.0f %% skipped as the same (DM_VK_PUSH_DEDUP); "
             "partial pushes (DM_VK_PUSH_DELTA) wrote %.0f %% of their descriptors", h + m ? 100.0 * h / (double)(h + m) : 0.0, sk + pu ? 100.0 * sk / (double)(sk + pu) : 0.0,
             g_pd_delta_all ? 100.0 * g_pd_delta_w / (double)g_pd_delta_all : 0.0);
    g_pd_delta_w = g_pd_delta_all = 0;
}
static VkWriteDescriptorSet g_dw[512]; static uint32_t g_dwn;   // the writes that changed (push_same returns 2)
static int push_same(VkPipelineBindPoint bp, VkPipelineLayout layout) {
    static uint64_t *buf; static size_t cap;
    size_t need = 2 + (size_t)g_w.n * 5, k = 0;
    if (need > cap) { cap = need * 2; buf = realloc(buf, cap * sizeof *buf); if (!buf) { cap = 0; return 0; } }
    buf[k++] = (uint64_t)layout; buf[k++] = g_w.n;
    for (uint32_t i = 0; i < g_w.n; i++) {
        const VkWriteDescriptorSet *w = &g_w.w[i];
        buf[k++] = (uint64_t)w->dstBinding | (uint64_t)w->dstArrayElement << 16 | (uint64_t)w->descriptorType << 32 | (uint64_t)w->descriptorCount << 48;
        if (w->descriptorCount != 1) return (g_pd[bp == VK_PIPELINE_BIND_POINT_COMPUTE].n = 0), 0;   // (the walk writes one descriptor per write)
        if (w->pBufferInfo) { buf[k++] = (uint64_t)w->pBufferInfo->buffer; buf[k++] = w->pBufferInfo->offset; buf[k++] = w->pBufferInfo->range; }
        else if (w->pImageInfo) { buf[k++] = (uint64_t)w->pImageInfo->sampler; buf[k++] = (uint64_t)w->pImageInfo->imageView; buf[k++] = w->pImageInfo->imageLayout; }
        else if (w->pTexelBufferView) { buf[k++] = (uint64_t)*w->pTexelBufferView; buf[k++] = 0; buf[k++] = 0; }
        else { buf[k++] = 1; buf[k++] = 1; buf[k++] = 1; }
    }
    int c = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (VKA_ENV("DM_VK_PUSH_DEDUP", 1) && g_pd[c].n == k && !memcmp(g_pd[c].w, buf, k * sizeof *buf)) { g_pd_skip++; g_pd_skip_all++; return 1; }
    // DM_VK_PUSH_DELTA=1 (live, default 0 until phone-checked): the same layout and the same writes in the same order as the last push, some of them with other descriptors:
    // only those are pushed. Vulkan lets push descriptors be updated incrementally; the others stay as the last push left them (until the set is disturbed: a new command
    // buffer or part, a set bound in its place, which forget g_pd)
    g_dwn = 0;
    if (VKA_ENV("DM_VK_PUSH_DELTA", 0) && g_pd[c].n == k && g_pd[c].w[0] == buf[0] && g_pd[c].w[1] == buf[1]) {
        int ok = 1;
        for (uint32_t i = 0; i < g_w.n && ok; i++) {
            const uint64_t *a = g_pd[c].w + 2 + 4 * (size_t)i, *b = buf + 2 + 4 * (size_t)i;
            if (a[0] != b[0]) ok = 0; else if (memcmp(a + 1, b + 1, 3 * sizeof *a)) g_dw[g_dwn++] = g_w.w[i];
        }
        if (!ok) g_dwn = 0;
    }
    if (k > g_pd[c].cap) { g_pd[c].cap = k * 2; g_pd[c].w = realloc(g_pd[c].w, g_pd[c].cap * sizeof *buf); if (!g_pd[c].w) { g_pd[c].cap = g_pd[c].n = 0; return 0; } }
    memcpy(g_pd[c].w, buf, k * sizeof *buf); g_pd[c].n = k; g_pd_push++; g_pd_push_all++;
    if (g_dwn) { g_pd_delta_w += g_dwn; g_pd_delta_all += g_w.n; return 2; }
    return 0;
}
static int g_sets_dry, g_dry_pool;   // DM_VK_INSTANCE: bind_sets only builds the writes (g_w) when g_sets_dry is set, and sets_push binds them later
// The writes of g_w bound: pushed (only what changed since the last push) or as a set, and the texture pool set
static int sets_push(const vka_layout *lay, VkPipelineBindPoint bp, int any_pool) {
    uint64_t t2_ = g_prof ? cntvct() : 0;
    if (lay->push) { int ps = g_w.n ? push_same(bp, lay->layout) : 1;
                     if (ps == 0) vk.vkCmdPushDescriptorSetKHR(g_cb, bp, lay->layout, 0, g_w.n, g_w.w);
                     else if (ps == 2) vk.vkCmdPushDescriptorSetKHR(g_cb, bp, lay->layout, 0, g_dwn, g_dw); }
    else {
        g_pd[bp == VK_PIPELINE_BIND_POINT_COMPUTE].n = 0;   // set 0 bound as a set: the last push is no longer what the draws see
        VkDescriptorSet set = set_alloc(lay->set0); if (!set) return 0;
        for (uint32_t i = 0; i < g_w.n; i++) g_w.w[i].dstSet = set;
        if (g_w.n) vk.vkUpdateDescriptorSets(V.dev, g_w.n, g_w.w, 0, NULL);
        vk.vkCmdBindDescriptorSets(g_cb, bp, lay->layout, 0, 1, &set, 0, NULL);
    }
    if (g_prof) g_ps[2] += cntvct() - t2_;
    if (any_pool) { VkDescriptorSet ps = vka_bindless_set(); if (ps) vk.vkCmdBindDescriptorSets(g_cb, bp, lay->layout, 1, 1, &ps, 0, NULL); else VRED_ONCE("a shader reads the texture pool and there is no pool set"); }
    return 1;
}
static int bind_sets(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, VkPipelineBindPoint bp, c0_at *c0) {
    g_w.n = 0;
    int any_pool = 0;
    uint64_t t1_ = g_prof ? cntvct() : 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        vka_shader *s = sh[st]; if (!s) continue;
        if (st == 1 && d->rast_discard) continue;   // no fragments: its stage's records are not read (the layout keeps them unwritten: partially bound is not needed for push descriptors)
        int n = g_wk.n[st];
        if (n < 0) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 16) VRED("shader %016llx: %s; skipped", (unsigned long long)s->key, g_wk.why[st]); return 0; }
        any_pool |= g_wk.pool[st];
        if (!stage_writes(d, st, s, g_wk.r[st], n, g_wk.pool[st], st == 5 ? c0 : NULL)) return 0;
    }
    if (g_prof) g_ps[1] += cntvct() - t1_;
    if (sh[2] && sh[2]->gs_fx > 0 && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {   // the claim words, zeroed (vka_spv_gs_claim); bound also when the draw takes no claim (the layout has them)
        if (g_w.n == 512) { VRED_ONCE("more than 512 descriptors in one draw"); return 0; }
        VkDeviceSize n = 4 * (VkDeviceSize)(g_claim_words ? g_claim_words : 1); vka_span cs;
        if (!vka_scratch(n, V.sb_align, &cs)) return 0;
        memset(cs.ptr, 0, (size_t)n);
        uint32_t k = g_w.n++;
        g_w.bi[k] = (VkDescriptorBufferInfo){cs.buf, cs.off, n};
        g_w.w[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = VKA_GS_CLAIM_BINDING, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &g_w.bi[k]};
    }
    if (g_sets_dry) { g_dry_pool = any_pool; return 1; }   // DM_VK_INSTANCE: the writes only (sets_push binds them)
    return sets_push(lay, bp, any_pool);
}
// Images first used by a draw (from the walk, walk_all), or remade with storage use (vka_tex.c img_storage_upgrade): dry = only whether there is one
static int make_images(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], int dry) {
    (void)d; int any = 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        if (!sh[st]) continue;
        const vka_res *r = g_wk.r[st];
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG) && r[i].tid
                                                 && (vka_tex_id_pending_rec(r[i].tid, r[i].dim) || (r[i].what == VKA_R_IMG && vka_tex_id_storage_pending(r[i].tid)))) {   // (an image remade with storage use copies its content: outside the pass)
            any = 1; if (dry) return 1;
            vka_tex_id_view(r[i].tid, g_cb, r[i].dim, r[i].what == VKA_R_IMG, NULL);
        }
    }
    return any;
}

// The draw writes memory: a storage buffer or image it writes, raw global stores (their write log), a geometry shader's claimed writes (from the walk, walk_all)
static int draw_writes(vka_shader *const sh[VKA_NSTAGE], const vka_dstate *d) {
    if (sh[2] && sh[2]->gs_fx > 0) return 1;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        if (!sh[st] || (st == 1 && d->rast_discard)) continue;
        const vka_res *r = g_wk.r[st];
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].flags & DMSH_VK_BF_RW) || r[i].what == VKA_R_IMG || r[i].what == VKA_R_GMEM_LOG) return 1;
    }
    return 0;
}
// After a game barrier taken inside the pass (vka_rp_game_barrier): this draw needs the pass ended first when it writes memory (earlier draws of the pass may read what it
// overwrites), reads the texture pool directly (any texture, the pass's targets among them), or samples a texture whose image the pass draws into
static int bar_hazard(vka_shader *const sh[VKA_NSTAGE], const vka_dstate *d) {
    if (draw_writes(sh, d)) return 1;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        if (!sh[st] || (st == 1 && d->rast_discard)) continue;
        if (g_wk.pool[st] || g_wk.n[st] < 0) return 1;
        const vka_res *r = g_wk.r[st];
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].what == VKA_R_TEX || r[i].what == VKA_R_TBUF) && r[i].tid && vka_tex_id_in_pass(r[i].tid)) return 1;
    }
    return 0;
}

// ---------- Draws ----------
// A program's shaders by its serial (a program that gets new shaders gets a new serial, hle_nvn.c), remembered for the draws after: the shader table's lock and lookup
// were taken once per stage of every draw
static struct { uint64_t serial; vka_progsh ps; } g_pm[64];
static int resolve(const prog_t *p, int st, vka_shader **out, unsigned *bad) {
    *out = NULL; if (!p) return 1;
    vka_progsh ps;
    uint64_t sr = p->serial; typeof(g_pm[0]) *m = &g_pm[(sr ^ sr >> 6) & 63];
    if (sr && m->serial == sr) ps = m->ps;
    else { if (!vka_prog_shaders(p, &ps)) return 0; if (sr) { m->serial = sr; m->ps = ps; } }
    if (ps.bad & (1u << st | VKA_BAD_MISSING)) { *bad |= 1u << st; return 1; }
    *out = ps.sh[st]; return 1;
}
// Vertex input: an attribute per input location the vertex shader reads (Location = NVN attribute index), each from its stream; an input no attribute feeds reads a zero buffer.
// With VK_EXT_vertex_input_dynamic_state the descriptions are set per draw, else they go into the pipeline key
static struct { VkVertexInputBindingDescription2EXT b[33]; VkVertexInputAttributeDescription2EXT a[32]; uint32_t nb, na, used, zb; int zero; } VI;
static void vin_build(const vka_dstate *d, const vka_shader *vs, vka_gkey *k) {
    VI.nb = VI.na = VI.used = 0; VI.zero = 0;
    VI.zb = V.max_vin_bindings > 32 ? 31 : V.max_vin_bindings > 16 ? V.max_vin_bindings - 1 : 15;   // the zero buffer's binding: past the 16 streams when the device has room
    for (uint32_t loc = 0; loc < 32; loc++) {
        if (!(vs->e->vin_mask >> loc & 1)) continue;
        int want = (int)(vs->e->vin_types >> (2 * loc) & 3), changed = 0, retype = 0;
        const va_st *a = loc < 16 && (int)loc < d->nattr ? &d->attr[loc] : NULL;
        VkFormat f = a && a->fmt ? vin_format(a->fmt, want, &changed, &retype) : VK_FORMAT_UNDEFINED;
        if (retype == DMSH_VK_TYPE_UINT) k->vin_u |= 1u << loc; else if (retype == DMSH_VK_TYPE_SINT) k->vin_s |= 1u << loc;
        if (a && a->fmt && !f) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("vertex attribute format %#x is not mapped: the input reads zeros", a->fmt); }
        if (changed) { static uint64_t seen[4]; unsigned b = a->fmt & 255; if (!(seen[b >> 6] >> (b & 63) & 1)) { seen[b >> 6] |= 1ull << (b & 63);
            VRED("vertex attribute format %#x feeds a %s input: bound as VkFormat %d of that class (values may differ from Maxwell's fetch)", a->fmt, want == 0 ? "float" : want == 1 ? "sint" : "uint", f); } }
        if (!f) {
            f = want == DMSH_VK_TYPE_UINT ? VK_FORMAT_R32G32B32A32_UINT : want == DMSH_VK_TYPE_SINT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            VI.a[VI.na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, loc, VI.zb, f, 0};
            VI.zero = 1; continue;
        }
        uint32_t st = a->stream & 15;
        VI.a[VI.na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, loc, st, f, a->offset};
        VI.used |= 1u << st;
    }
    for (uint32_t st = 0; st < 16; st++) if (VI.used >> st & 1) {
        uint32_t div = d->stream[st].divisor;
        if (div > 1 && !V.divisor) { VRED_ONCE("instance divisor %u and no vertexAttributeInstanceRateDivisor: drawn with divisor 1", div); div = 1; }
        VI.b[VI.nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, st, d->stream[st].stride,
                                                               div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, div ? div : 1};
    }
    if (VI.zero) VI.b[VI.nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, VI.zb, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1};
    if (!V.vin_dyn) {
        k->vin_static = 1;
        for (uint32_t i = 0; i < VI.na; i++) { uint32_t l = VI.a[i].location; k->va[l].fmt = VI.a[i].format; k->va[l].offset = VI.a[i].offset; k->va[l].binding = (uint8_t)VI.a[i].binding; k->va[l].used = 1; }
        for (uint32_t i = 0; i < VI.nb; i++) { uint32_t b = VI.b[i].binding; k->vb[b].stride = VI.b[i].stride; k->vb[b].divisor = VI.b[i].inputRate ? VI.b[i].divisor : 0; k->vb[b].used = 1; }
    }
}
// The vertex buffers the command buffer holds (slot: buffer, offset, size): a draw binds only the slots that changed, consecutive ones in one call. Vulkan keeps vertex
// buffer bindings across pipeline binds and passes for the life of the command buffer; forgotten with the dynamic state (vka_dyn_invalidate: a new command buffer or
// part). Profile p2: vin_bind was 13 % of the replay thread, one vkCmdBindVertexBuffers2 a stream a draw. DM_VK_VB_FILTER=0 (live): every slot every draw
static struct { int valid, vin_stale; VkBuffer b[33]; VkDeviceSize o[33], sz[33]; uint32_t vnb, vna; VkVertexInputBindingDescription2EXT vb[33]; VkVertexInputAttributeDescription2EXT va[32]; } VB;
static void vb_set(uint32_t slot, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, uint32_t *n0, uint32_t *nn, VkBuffer *rb, VkDeviceSize *ro, VkDeviceSize *rs) {
    if (slot >= 33) return;
    if (VB.valid && g_vb_filter && VB.b[slot] == b && VB.o[slot] == o && VB.sz[slot] == sz) return;
    VB.b[slot] = b; VB.o[slot] = o; VB.sz[slot] = sz;
    if (*nn && *n0 + *nn != slot) { vk.vkCmdBindVertexBuffers2(g_cb, *n0, *nn, rb, ro, rs, NULL); *nn = 0; }   // a run ends: bound in one call
    if (!*nn) *n0 = slot;
    rb[*nn] = b; ro[*nn] = o; rs[*nn] = sz; (*nn)++;
}
static void vin_bind(const vka_dstate *d) {
    if (!VB.valid) memset(&VB, 0, sizeof VB);
    // The vertex input state too only when it changed (it was set every draw: up to 65 descriptions copied into the encoder's ring and the driver), and again after
    // every pipeline bind: Vulkan keeps dynamic vertex input across vkCmdBindPipeline, but drivers have lost it there before (hardening; the buffers stay filtered)
    if (V.vin_dyn) {
        int same = VB.valid && !VB.vin_stale && g_vb_filter && VB.vnb == VI.nb && VB.vna == VI.na && !memcmp(VB.vb, VI.b, VI.nb * sizeof *VI.b) && !memcmp(VB.va, VI.a, VI.na * sizeof *VI.a);
        if (!same) { vk.vkCmdSetVertexInputEXT(g_cb, VI.nb, VI.b, VI.na, VI.a); VB.vnb = VI.nb; VB.vna = VI.na; memcpy(VB.vb, VI.b, VI.nb * sizeof *VI.b); memcpy(VB.va, VI.a, VI.na * sizeof *VI.a); }
        VB.vin_stale = 0;
    }
    VkBuffer rb[33]; VkDeviceSize ro[33], rs[33]; uint32_t n0 = 0, nn = 0;
    for (uint32_t st = 0; st < 16; st++) if (VI.used >> st & 1) {   // each stream at its address, as far as it is bound and placed
        VkBuffer b = g_zero.buf; VkDeviceSize o = g_zero.off, sz = 65536;
        uint64_t addr = d->vbuf[st].address, size = d->vbuf[st].size;
        if (addr) { VkBuffer cb; VkDeviceSize co; uint64_t run = vka_addr_run(addr, size ? size : 1ull << 30, &cb, &co);
                    if (run) { b = cb; o = co; sz = run; } else VRED_ONCE("vertex stream at %#llx is not in placed guest RAM: it reads zeros", (unsigned long long)addr); }
        vb_set(st, b, o, sz, &n0, &nn, rb, ro, rs);
    }
    if (VI.zero) vb_set(VI.zb, g_zero.buf, g_zero.off, 65536, &n0, &nn, rb, ro, rs);
    if (nn) vk.vkCmdBindVertexBuffers2(g_cb, n0, nn, rb, ro, rs, NULL);
    VB.valid = 1;
}

// The guest indices of an indexed draw as 32-bit values (the CPU's view): for primitives Vulkan has no topology for and 8-bit indices on a device without them
static uint32_t *read_indices(uint64_t addr, int itype, uint32_t count) {
    size_t isz = itype == 0 ? 1 : itype == 1 ? 2 : 4; uint8_t *raw = malloc(isz * count + 4); uint32_t *o = malloc(4 * (size_t)count + 4);
    if (!raw || !o || !cpu_read(addr, raw, isz * count)) { free(raw); free(o); return NULL; }
    for (uint32_t i = 0; i < count; i++) o[i] = isz == 1 ? raw[i] : isz == 2 ? ((uint16_t *)raw)[i] : ((uint32_t *)raw)[i];
    free(raw); return o;
}

// The dynamic state the command buffer holds: each draw sets only what changed (the draw path set some 25 states a draw). Valid from the first draw of a submission until
// something else binds a graphics pipeline with static state (the masked stencil clear: vka_dyn_invalidate) or the submission ends; every graphics pipeline of the draws has
// the same dynamic states (vka_pso.c), so binding one keeps them, except a mesh pipeline, which has no topology or primitive restart (vka_draw resets DY.topo / DY.pr after one)
static struct { int valid; VkPipeline pipe; VkViewport vp; VkRect2D sc; uint32_t cull, ff, topo, dt, dw, dc, dbt, stt, dbe, rd, pr, sop[8], smask[6]; float db[3], bc[4]; } DY;
#define DY_SET(f, val, call) do { if (!DY.valid || !g_dyn_filter || memcmp(&DY.f, &(val), sizeof DY.f)) { memcpy(&DY.f, &(val), sizeof DY.f); call; } } while (0)
#define DY_SET1(f, val, call) do { uint32_t v_ = (val); if (!DY.valid || !g_dyn_filter || DY.f != v_) { DY.f = v_; call; } } while (0)
// The index buffer the command buffer holds: bound again only when it changes (DM_VK_VB_FILTER), forgotten with the dynamic state
static struct { int valid; VkBuffer b; VkDeviceSize o; VkIndexType t; } IB;
static void ib_bind(VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (IB.valid && g_vb_filter && IB.b == b && IB.o == o && IB.t == t) return;
    vk.vkCmdBindIndexBuffer(g_cb, b, o, t); IB.valid = 1; IB.b = b; IB.o = o; IB.t = t;
}
static void inst_reset(void);
void vka_dyn_invalidate(void) { DY.valid = 0; VB.valid = 0; IB.valid = 0; g_pd[0].n = g_pd[1].n = 0; inst_reset(); }
#define PT(i) do { if (g_prof) { uint64_t t_ = cntvct(); if ((i) > 0 && g_pt_ph == (i) - 1) g_pt[(i) > 0 ? (i) - 1 : 0] += t_ - g_pt_last; g_pt_last = t_; g_pt_ph = (i); } } while (0)
// ---------- Instanced runs (DM_VK_INSTANCE) ----------
// Donkey Kong Bananza draws many objects as runs of draws that repeat one mesh (the same pipeline, index range, base vertex and vertex streams) and differ only in where one
// 256-byte uniform block sits: the object's constants, read by the vertex shader and by the pixel shader through one binding each, moved by the same amount (a third of the
// level's draws, in runs of 32 on average). On an Adreno 740 (AYN Odin 2 Portal) every draw costs the GPU about 1-1.5 us however small, so such a run
// is drawn as one instanced draw: from its second draw on, the shaders are instanced variants (vka_spv_instance: the block read at data[i + d], d per instance
// from a uint vertex input at location 30, passed to the pixel shader as a flat input), the block is bound as a window from the run's first offset, and each further draw
// only writes its d into the run's table (an instance-rate vertex buffer) and raises the instance count of the run's indirect draw, in host-visible memory before the
// submission. A draw joins only when nothing was recorded since the run's last one (the encoder's call count): anything else closes the run and the draw is drawn alone.
// Variants are rewritten from the library's SPIR-V when a run first asks for them (a file <dir>/<shader key>.b<binding>.spv overrides one: DM_VK_INSTANCE_DIR, else
// "instance" beside the DM_ROMFS_OVERLAY folder); a pair without both is never instanced. DM_VK_INSTANCE=1 turns the runs on (live; off by default)
uint64_t vka_enc_total(void); int vka_enc_on(void);
enum { INST_LOC = 30, INST_BIND = 31, INST_CAP = 256 };
typedef struct { uint32_t binding, type; uint64_t a, b, c; } ikw;   // a write as compared: buffer, offset, range / sampler, view, layout
typedef struct {
    int valid; VkPipeline p; VkBuffer ib; VkDeviceSize io; VkIndexType it; uint32_t cnt; int32_t bv; uint64_t mark;
    uint32_t nw; ikw w[64]; uint64_t ad[64];
    uint32_t nb, na, used, zb; int zero; VkVertexInputBindingDescription2EXT b[33]; VkVertexInputAttributeDescription2EXT a[32]; uint64_t va[16], vs[16];
} isnap;
static isnap g_ia;   // the last direct indexed draw drawn alone that a run could start from
static struct { int open; isnap base; VkPipeline ipipe; uint32_t wv, wp; VkDeviceSize win_v, win_p; uint32_t *args, *table, n; uint64_t mark;
    // the fast join (inst_fast): the moved block's uniform slots and their addresses at the run's first draw, the state generation, the draw's own records
    int fast, iv, ip; vka_range uv, up; uint32_t gen; uint64_t ibuf; int32_t prim, itype; int has_rt, has_sc; p_set_rt rt; p_scissor sc; } RUN;
static uint64_t g_inst_runs, g_inst_joined, g_inst_falls, g_inst_fast;
static int inst_snap(isnap *s, VkPipeline p, VkBuffer ib, VkDeviceSize io, VkIndexType it, const p_draw_idx *di, const vka_dstate *d) {
    if (g_w.n > 64) return s->valid = 0;
    s->p = p; s->ib = ib; s->io = io; s->it = it; s->cnt = (uint32_t)di->count; s->bv = di->base_vertex; s->nw = g_w.n;
    for (uint32_t i = 0; i < g_w.n; i++) {
        const VkWriteDescriptorSet *w = &g_w.w[i]; ikw *k = &s->w[i]; k->binding = w->dstBinding | w->dstArrayElement << 16; k->type = w->descriptorType;
        if (w->pBufferInfo) { k->a = (uint64_t)w->pBufferInfo->buffer; k->b = w->pBufferInfo->offset; k->c = w->pBufferInfo->range; }
        else if (w->pImageInfo) { k->a = (uint64_t)w->pImageInfo->sampler; k->b = (uint64_t)w->pImageInfo->imageView; k->c = w->pImageInfo->imageLayout; }
        else { k->a = w->pTexelBufferView ? (uint64_t)*w->pTexelBufferView : 0; k->b = k->c = 0; }
        s->ad[i] = g_w.ad[i];
    }
    s->nb = VI.nb; s->na = VI.na; s->used = VI.used; s->zb = VI.zb; s->zero = VI.zero;
    memcpy(s->b, VI.b, VI.nb * sizeof *VI.b); memcpy(s->a, VI.a, VI.na * sizeof *VI.a);
    for (int st = 0; st < 16; st++) { s->va[st] = VI.used >> st & 1 ? d->vbuf[st].address : 0; s->vs[st] = VI.used >> st & 1 ? d->vbuf[st].size : 0; }
    return s->valid = 1;
}
// The same draw as snapshot s apart from its descriptors: index range, base vertex, vertex input and streams
static int inst_same_draw(const isnap *s, VkBuffer ib, VkDeviceSize io, VkIndexType it, const p_draw_idx *di, const vka_dstate *d) {
    if (!s->valid || s->ib != ib || s->io != io || s->it != it || s->cnt != (uint32_t)di->count || s->bv != di->base_vertex) return 0;
    if (s->nb != VI.nb || s->na != VI.na || s->used != VI.used || s->zb != VI.zb || s->zero != VI.zero) return 0;
    if (memcmp(s->b, VI.b, VI.nb * sizeof *VI.b) || memcmp(s->a, VI.a, VI.na * sizeof *VI.a)) return 0;
    for (int st = 0; st < 16; st++) if (VI.used >> st & 1 && (s->va[st] != d->vbuf[st].address || s->vs[st] != d->vbuf[st].size)) return 0;
    return 1;
}
// g_w against snapshot s: 2 = the same but for one vertex uniform write and one pixel uniform write moved by the same amount (*wv, *wp, *delta); 0 = otherwise.
// With want_v / want_p given (a run's moved writes) only those may differ
static int inst_cmp(const isnap *s, uint32_t *wv, uint32_t *wp, int64_t *delta, int fixed) {
    if (g_w.n != s->nw) return 0;
    int nd = 0; uint32_t m[2] = {0, 0};
    for (uint32_t i = 0; i < g_w.n; i++) {
        const VkWriteDescriptorSet *w = &g_w.w[i]; const ikw *k = &s->w[i];
        if ((w->dstBinding | w->dstArrayElement << 16) != k->binding || w->descriptorType != k->type) return 0;
        uint64_t a, b, c;
        if (w->pBufferInfo) { a = (uint64_t)w->pBufferInfo->buffer; b = w->pBufferInfo->offset; c = w->pBufferInfo->range; }
        else if (w->pImageInfo) { a = (uint64_t)w->pImageInfo->sampler; b = (uint64_t)w->pImageInfo->imageView; c = w->pImageInfo->imageLayout; }
        else { a = w->pTexelBufferView ? (uint64_t)*w->pTexelBufferView : 0; b = c = 0; }
        if (a == k->a && b == k->b && c == k->c) continue;
        if (w->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || a != k->a || c != k->c || nd == 2) return 0;
        m[nd++] = i;
    }
    if (nd == 0 && fixed) { *delta = 0; return 2; }   // the run's base again
    if (nd != 2) return 0;
    uint32_t v = m[0], f = m[1];
    if (g_w.w[v].dstBinding >= DMSH_VK_STAGE_BINDINGS || g_w.w[f].dstBinding < DMSH_VK_STAGE_FS * DMSH_VK_STAGE_BINDINGS || g_w.w[f].dstBinding >= (DMSH_VK_STAGE_FS + 1) * DMSH_VK_STAGE_BINDINGS) return 0;
    int64_t dv = (int64_t)g_w.bi[v].offset - (int64_t)s->w[v].b, dp = (int64_t)g_w.bi[f].offset - (int64_t)s->w[f].b;
    if (dv != dp) return 0;
    if (fixed && (v != *wv || f != *wp)) return 0;
    *wv = v; *wp = f; *delta = dv; return 2;
}
// The instanced variant of shader s with the block at binding moved (NULL = none: no file, or it does not load)
static vka_shader *inst_variant_make(vka_shader *s, uint32_t binding);
// The frame path and the prewarm threads (instanced variants in the prewarm list: vka_inst_variant) share the cache, so one variant is made once
static pthread_mutex_t g_ivmu = PTHREAD_MUTEX_INITIALIZER;
static vka_shader *inst_variant(vka_shader *s, uint32_t binding) { pthread_mutex_lock(&g_ivmu); vka_shader *v = inst_variant_make(s, binding); pthread_mutex_unlock(&g_ivmu); return v; }
vka_shader *vka_inst_variant(vka_shader *s, uint32_t binding) { return VKA_ENV("DM_VK_INSTANCE", 0) ? inst_variant(s, binding) : NULL; }
static vka_shader *inst_variant_make(vka_shader *s, uint32_t binding) {
    static struct { uint64_t key; uint32_t binding; vka_shader *v; } cache[128]; static int ncache;
    for (int i = 0; i < ncache; i++) if (cache[i].key == s->key && cache[i].binding == binding) return cache[i].v;
    if (ncache == 128) return NULL;
    static char dir[512]; static int dir_set;
    if (!dir_set) { dir_set = 1; const char *e = getenv("DM_VK_INSTANCE_DIR"), *o = getenv("DM_ROMFS_OVERLAY");
        if (e && *e) snprintf(dir, sizeof dir, "%s", e);
        else if (o && *o) { snprintf(dir, sizeof dir, "%s", o); char *sl = strrchr(dir, '/'); if (sl && !sl[1]) { *sl = 0; sl = strrchr(dir, '/'); } if (sl) snprintf(sl, sizeof dir - (size_t)(sl - dir), "/instance"); }
        VLOG("instanced runs: variants made from the library's SPIR-V; files in %s override them", dir[0] ? dir : "(no folder: DM_VK_INSTANCE_DIR or DM_ROMFS_OVERLAY)"); }
    vka_shader *v = NULL;
    char path[700]; snprintf(path, sizeof path, "%s/%016llx.b%u.spv", dir, (unsigned long long)s->key, binding);
    FILE *f = dir[0] ? fopen(path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint32_t *w = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
        if (w && fread(w, 1, (size_t)n, f) == (size_t)n && w[0] == 0x07230203) {
            VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = w}; VkShaderModule m = VK_NULL_HANDLE;
            if (!vk.vkCreateShaderModule(V.dev, &ci, NULL, &m) && (v = calloc(1, sizeof *v))) {
                *v = *s; memset(v->mods, 0, sizeof v->mods); v->mods[DMSH_VK_MOD_BASE] = m; v->vin = v->fout = NULL; v->gs_claim = VK_NULL_HANDLE; v->key = s->key ^ VKA_INST_KEY_XOR; v->inst_bind = binding + 1;
                VLOG("instanced runs: variant of %016llx (binding %u) loaded", (unsigned long long)s->key, binding);
            }
        }
        free(w); fclose(f);
    }
    if (!v) {   // no file (the usual case): made from the library's SPIR-V (vka_spv_instance); a file in the folder above overrides it
        VkShaderModule m = vka_shader_inst_module(s, binding, 30);
        if (m && (v = calloc(1, sizeof *v))) {
            *v = *s; memset(v->mods, 0, sizeof v->mods); v->mods[DMSH_VK_MOD_BASE] = m; v->vin = v->fout = NULL; v->gs_claim = VK_NULL_HANDLE; v->key = s->key ^ VKA_INST_KEY_XOR; v->inst_bind = binding + 1;
            static _Atomic int nm; if (atomic_fetch_add(&nm, 1) < 32) VLOG("instanced runs: variant of %016llx (binding %u) made", (unsigned long long)s->key, binding);
        } else if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    }
    cache[ncache].key = s->key; cache[ncache].binding = binding; cache[ncache++].v = v;
    return v;
}
static void inst_close(void) { RUN.open = 0; }
static void inst_reset(void) { RUN.open = 0; g_ia.valid = 0; }
// A draw that joins the open run without its walk: nothing but the run's two uniform slots was bound since the run's last draw, nothing was recorded, the same
// draw record, targets and scissor; the slots moved by one amount inside the windows. Every other draw takes the full path (which may still join)
static int inst_fast(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw_idx *di) {
    if (d->bind_gen != RUN.gen) return 0;
    if (RUN.n >= INST_CAP) return 0;
    if (vka_enc_total() != RUN.mark) return 0;
    if (di->ind || di->mdi_cnt || di->ibuf != RUN.ibuf || di->count != (int32_t)RUN.base.cnt || di->base_vertex != RUN.base.bv || di->itype != RUN.itype || di->prim != RUN.prim
        || di->instances > 1 || di->base_instance || d->b.base_instance) return 0;
    for (int st = 0; st < VKA_NSTAGE; st++) { uint32_t allow = st == 0 ? 1u << RUN.iv : st == 1 ? 1u << RUN.ip : 0; /* NVN stages: 0 vertex, 1 fragment */ if (d->ubo_dirty[st] & ~allow) return 0; }
    if (has_rt != RUN.has_rt || (has_rt && memcmp(rt, &RUN.rt, sizeof *rt)) || has_sc != RUN.has_sc || (has_sc && memcmp(sc, &RUN.sc, sizeof *sc))) return 0;
    vka_range v = d->b.ubo[0][RUN.iv], f = d->b.ubo[1][RUN.ip];
    int64_t dv = (int64_t)v.addr - (int64_t)RUN.uv.addr, dp = (int64_t)f.addr - (int64_t)RUN.up.addr;
    if (dv != dp || dv < 0 || (dv & 15) || v.size != RUN.uv.size || f.size != RUN.up.size) return 0;
    if ((uint64_t)dv + RUN.base.w[RUN.wv].c > RUN.win_v || (uint64_t)dv + RUN.base.w[RUN.wp].c > RUN.win_p) return 0;
    RUN.table[RUN.n++] = (uint32_t)(dv / 16); RUN.args[1] = RUN.n;
    memset(d->ubo_dirty, 0, sizeof d->ubo_dirty);
    g_inst_joined++; g_inst_fast++; g_draws++;
    return 1;
}
// After a run's first draw or a full-path join: what inst_fast compares against
static void inst_fast_arm(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw_idx *di, int first) {
    if (first) {
        RUN.fast = 0; RUN.iv = RUN.ip = -1;
        for (int i = 0; i < 16; i++) { if (RUN.iv < 0 && d->b.ubo[0][i].addr == g_w.ad[RUN.wv]) RUN.iv = i; if (RUN.ip < 0 && d->b.ubo[1][i].addr == g_w.ad[RUN.wp]) RUN.ip = i; }
        if (RUN.iv < 0 || RUN.ip < 0 || !g_w.ad[RUN.wv] || !g_w.ad[RUN.wp]) {
            static int n; if (n++ < 3) { char b[600]; size_t o = 0; for (int i = 0; i < 16; i++) o += (size_t)snprintf(b + o, sizeof b - o, " %llx/%llx", (unsigned long long)d->b.ubo[0][i].addr, (unsigned long long)d->b.ubo[1][i].addr);
                VLOG("fast join not armed: moved addresses %llx %llx (writes %u %u), slots vs/fs:%s", (unsigned long long)g_w.ad[RUN.wv], (unsigned long long)g_w.ad[RUN.wp], RUN.wv, RUN.wp, b); }
            return; }
        RUN.uv = d->b.ubo[0][RUN.iv]; RUN.up = d->b.ubo[1][RUN.ip];
        RUN.ibuf = di->ibuf; RUN.prim = di->prim; RUN.itype = di->itype;
        RUN.has_rt = has_rt; if (has_rt) RUN.rt = *rt; RUN.has_sc = has_sc; if (has_sc) RUN.sc = *sc;
        RUN.fast = VKA_ENV("DM_VK_INSTANCE_FAST", 1) != 0;
    }
    RUN.gen = d->bind_gen; memset(d->ubo_dirty, 0, sizeof d->ubo_dirty);
}
// The index buffer of a direct indexed draw as the draw would bind it; 0 = not one a run can take
static int inst_ib(const p_draw_idx *di, VkBuffer *ib, VkDeviceSize *io, VkIndexType *it) {
    *it = di->itype == 0 ? VK_INDEX_TYPE_UINT8 : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    size_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
    if (*it == VK_INDEX_TYPE_UINT8 && !V.idx8) return 0;
    return di->count > 0 && vka_addr(di->ibuf, isz * (uint64_t)di->count, ib, io, NULL) && !(*io % isz);
}
void vka_draw_prof(char *b, size_t n, double draws) {
    if (!g_prof || draws <= 0) { if (n) b[0] = 0; return; }
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); double us = 1e6 / (double)f / draws;
    static const char *nm[8] = {"resolve", "layout", "images+pass+key", "vertex input", "pipeline", "sets", "vertex buffers+dynamic", "draw"};
    size_t o = 0; for (int i = 0; i < 8 && o < n; i++) o += snprintf(b + o, n - o, "%s%s %.2f", i ? ", " : "", nm[i], g_pt[i] * us);
    if (o < n) o += snprintf(b + o, n - o, "; the whole of vka_draw %.2f us a draw over %llu calls", g_dtot * us, (unsigned long long)g_dn);
    if (o < n) o += snprintf(b + o, n - o, "; pushes %.0f%% skipped as the last one", g_pd_skip + g_pd_push ? 100.0 * g_pd_skip / (double)(g_pd_skip + g_pd_push) : 0.0);
    g_pd_skip = g_pd_push = 0;
    if (o < n) o += snprintf(b + o, n - o, "; sets: walk %.2f, writes %.2f (driver segments %.2f, %.1f images %.2f), push %.2f", g_ps[0] * us, g_ps[1] * us, g_ps[3] * us, g_ps[5] / draws, g_ps[4] * us, g_ps[2] * us);
    if (g_inst_on && o < n) o += snprintf(b + o, n - o, "; instanced runs %llu (%.1f draws joined each, %.0f%% of them without the walk), %llu fell back to single draws",
                                          (unsigned long long)g_inst_runs, g_inst_runs ? (double)g_inst_joined / (double)g_inst_runs : 0.0,
                                          g_inst_joined ? 100.0 * (double)g_inst_fast / (double)g_inst_joined : 0.0, (unsigned long long)g_inst_falls);
    g_inst_runs = g_inst_joined = g_inst_fast = g_inst_falls = 0;
    memset(g_ps, 0, sizeof g_ps);
    memset(g_pt, 0, sizeof g_pt);
    g_dtot = g_dn = 0;
}
static void vka_draw_(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di) {
    if (!g_cb || !draws_on() || !statics()) { g_skipped++; return; }
    if (RUN.open && RUN.fast && di && !dw && g_inst_on && inst_fast(d, rt, has_rt, sc, has_sc, di)) return;   // DM_VK_INSTANCE: the run's next object, no walk
    if (!d->inited) vka_dstate_reset(d);
    uint64_t mdi = dw ? dw->mdi_cnt : di->mdi_cnt; int mdi_i = dw ? dw->mdi_i : di->mdi_i;
    if (mdi) { uint32_t cnt = 0; if (!cpu_read(mdi, &cnt, 4) || (uint32_t)mdi_i >= cnt) return;   // past the multi-draw's count (read now: a count the GPU writes in this submission is not seen)
               VRED_ONCE("indirect-count multi-draws read their count on the CPU at replay"); }
    d->b.base_instance = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    vka_shader *sh[VKA_NSTAGE] = {0}; unsigned bad = 0;
    PT(0);
    // An NVN2 mesh draw (nvnCommandBufferDrawMeshTasks, hle_nvn.c): the task (optional) and mesh shaders of one program, the fragment stage's shader, in a mesh pipeline
    // (vka_pso.c pstate_fill) drawn with vkCmdDrawMeshTasksEXT: no vertex input, primitive type or index buffer
    const int mesh = dw && dw->prim == NVN_PRIM_MESH;
    if (mesh) {
        if (!V.mesh) SKIP("mesh draw (nvnCommandBufferDrawMeshTasks) on a device without VK_EXT_mesh_shader (or DM_VK_MESH=0): skipped");
        // The program whose task and mesh shaders run: the fragment stage's when it has a mesh shader (a program's stages are bound together), else the one bound at the
        // mesh stage bit. Task and mesh come from the same program (a program's task shader feeds its own mesh shader)
        const prog_t *mp = d->bst[7];
        { vka_shader *m = NULL; unsigned b2 = 0; if (d->bst[1] && d->bst[1] != mp && resolve(d->bst[1], 7, &m, &b2) && (m || (b2 & 1u << 7))) mp = d->bst[1]; }
        if (!resolve(mp, 6, &sh[6], &bad) || !resolve(mp, 7, &sh[7], &bad) || !resolve(d->bst[1], 1, &sh[1], &bad)) SKIP("mesh draw skipped: no shader library");
        if (bad) { g_skipped++; return; }   // a refused or missing shader (logged when it was made)
        if (!sh[7]) SKIP("mesh draw with no mesh shader bound: skipped");
        if (d->bst[6] && d->bst[6] != mp) VRED_ONCE("mesh draw: a program bound at the task stage bit apart from the mesh program; drawn with the mesh program's own task shader, if any");
        // A pixel shader with per-primitive inputs reads what its mesh shader writes per primitive (Vulkan matches only equally decorated interfaces). The other way round
        // (a mesh shader with per-primitive outputs, a pixel shader without the decoration: vk_android_build.py leaves a pixel shader also drawn with vertex shaders per
        // vertex) the flags cannot tell whether it reads those locations: a red line
        if (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM) && !(sh[7]->e->flags & DMSH_VK_F_MESH_PRIM))
            SKIP("mesh draw: pixel shader %016llx reads per-primitive inputs that mesh shader %016llx does not write: skipped", (unsigned long long)sh[1]->key, (unsigned long long)sh[7]->key);
        if (sh[1] && !(sh[1]->e->flags & DMSH_VK_F_MESH_PRIM) && (sh[7]->e->flags & DMSH_VK_F_MESH_PRIM))
            VRED_ONCE("mesh draw: mesh shader %016llx writes per-primitive outputs and pixel shader %016llx has no per-primitive inputs: any it reads there are undefined", (unsigned long long)sh[7]->key, (unsigned long long)sh[1]->key);
        if (dw->first) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("mesh draw with first workgroup %d (count %d): drawn from 0 (the translated task / mesh shaders read their workgroup index with no base; open)", dw->first, dw->count); }
        if (dw->count <= 0) return;
        if (d->tf_on) VRED_ONCE("transform feedback with a mesh draw: drawn without capture");
    } else {
        for (int st = 0; st < 5; st++) if (!resolve(d->bst[st], st, &sh[st], &bad)) SKIP("draw skipped: no shader library");
        if (bad) { g_skipped++; return; }   // a refused or missing shader (logged when it was made)
        if (!sh[0]) SKIP("draw with no vertex shader bound: skipped");
        if (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM)) SKIP("pixel shader %016llx reads per-primitive mesh outputs and is drawn with a vertex shader: skipped", (unsigned long long)sh[1]->key);
        if (d->rast_discard && d->tf_on) SKIP("transform feedback is not on Vulkan yet (F_TF): the draw is skipped");
        if (d->tf_on) VRED_ONCE("transform feedback is not on Vulkan yet: the draw is drawn without capture");
    }
    if (d->poly.cull == NVN_FACE_FRONT_AND_BACK && !d->rast_discard) return;
    int prim = dw ? dw->prim : di->prim, expand = 0;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // (a mesh pipeline's: every translated mesh shader outputs triangles)
    if (!mesh) {
        topo = topology(prim, &expand);
        if (expand < 0) SKIP("primitive type %d not on Vulkan: the draw is skipped", prim);
        if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) SKIP("tessellation patches are not on Vulkan yet (no patch size is recorded): the draw is skipped");
    }
    uint64_t ind = dw ? dw->ind : di->ind;
    // A geometry shader that writes memory makes those writes in the run that claims its primitive (vka_spv_gs_claim): a zeroed word per primitive at VKA_GS_CLAIM_BINDING.
    // Primitive IDs restart with each instance and an indirect draw's count is the GPU's, so those draws take the shader as it is (each run writing)
    g_claim_words = 0;
    if (sh[2] && sh[2]->gs_fx > 0) {
        int inst_n = dw ? dw->instances : di->instances;
        if (!ind && inst_n <= 1) { int32_t c = dw ? dw->count : di->count; g_claim_words = c > 0 ? (uint32_t)c : 1; }
        else VRED_ONCE("geometry shader %016llx writes memory in an %s draw: drawn without the claim, its writes repeated by the device's runs of each primitive", (unsigned long long)sh[2]->key, ind ? "indirect" : "instanced");
    }
    if (expand && ind) SKIP("an indirect draw of quads or a line loop is not on Vulkan yet: skipped");

    PT(1);
    // The last draw's shaders' layout (layouts live as long as the device): the layout table's lock and hash a draw
    static vka_shader *lsh[VKA_NSTAGE]; static vka_layout llay; static int lok;
    vka_layout lay;
    if (lok && !memcmp(lsh, sh, sizeof lsh)) lay = llay;
    else { lok = 0; if (!vka_layout_of(sh, 0, &lay)) SKIP("draw skipped: no pipeline layout"); memcpy(lsh, sh, sizeof lsh); llay = lay; lok = 1; }
    // Images first used by this draw are made (cleared or uploaded) outside the render pass, then the pass opens over the bound targets
    PT(2);
    walk_all(d, sh);
    if (make_images(d, sh, 1)) { vka_rp_end(); make_images(d, sh, 0); }
    if (R.on && R.bar_pending && bar_hazard(sh, d)) { g_bar_late++; vka_rp_end(); }   // the game barrier taken inside the pass is needed here: the pass ends (its full barrier) first
    if (vka_nan_on() >= 2 && sh[1] && !d->rast_discard && vka_nan_probe(sh[1]->key)) {   // DM_VK_NANSCAN_FS: this draw's sampled textures, scanned before it
        char why[8]; int n = vka_walk(&d->b, 1, sh[1]->e, sh[1]->b, sh[1]->lib->h->ssbo_align, 0, rd_cb, NULL, g_res, 512, NULL, why, sizeof why), m = 0;
        static vka_img *ims[64]; static int bs[64], ts[64];
        for (int i = 0; i < n && m < 64; i++) if (g_res[i].what == VKA_R_TEX && g_res[i].tid) { ims[m] = vka_tex_id_img(g_res[i].tid, g_cb); bs[m] = g_res[i].binding; ts[m++] = g_res[i].tid;
            static _Atomic int nd; if (atomic_fetch_add(&nd, 1) < 32) { char why[320]; vka_tex_id_desc(g_res[i].tid, why, sizeof why); vka_img *im = ims[m - 1];
                if (getenv("DM_VK_NANSCAN_DUMP") && im && im->d.format >= 0x79 && im->d.format <= 0x94) vka_tex_id_dump(g_res[i].tid);
                vka_tex_id_check(g_res[i].tid);
                VLOG("NaN scan: input binding %u dim %#x texture ID %d sampler %d: %s; image inited %d gpu_owned %d track %d", g_res[i].binding, g_res[i].dim, g_res[i].tid, g_res[i].sid, why, im ? im->inited : -1, im ? im->gpu_owned : -1, im ? im->track : -1); } }
        int on = R.on; vka_rp_end(); (void)on;
        vka_nan_inputs(g_cb, ims, bs, ts, m);
    }
    if (!rp_begin(rt, has_rt, d)) { g_skipped++; return; }
    if (!R.swrite && draw_writes(sh, d)) R.swrite = 1;   // (a game barrier in this pass from now on ends it: vka_rp_game_barrier)
    d->b.samples = R.samples;
    vka_gkey k; memset(&k, 0, V.vin_dyn ? offsetof(vka_gkey, va) : sizeof k);   // (va, vb: only without vertex input dynamic state, vin_build; the pipeline lookup reads no further then)
    k.ncolor = (uint8_t)R.ncolor; k.samples = (uint8_t)R.samples; k.dfmt = R.df; k.sfmt = R.sf;
    k.topo = mesh ? 0 : (uint8_t)topo_class(topo);
    k.poly_mode = (uint8_t)(d->poly.mode == NVN_POLYGON_MODE_LINE && V.fill_lines ? VK_POLYGON_MODE_LINE : d->poly.mode == NVN_POLYGON_MODE_POINT && V.fill_lines ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
    k.depth_clamp = d->depth_clamp && V.depth_clamp; k.a2c = d->ms.a2c != 0;
    if (d->color.logic_op != NVN_LOGIC_OP_COPY && V.logic_op) { k.logic_en = 1; k.logic_op = (uint8_t)d->color.logic_op; }
    k.gl_depth = !vka_depth_zero();
    const vka_shader *fs = d->rast_discard ? NULL : sh[1];
    // The pixel shader's module (android/SHADERS.md "Variants"): MOD_UINT when every target it writes is a UINT attachment, MOD_UINT_MIXED when the UINT ones are exactly its
    // uint_mask, MOD_DUAL when target 0 blends with a SRC1 factor; else the base. The alpha test is the base's specialization constant in every variant
    uint32_t umask = 0, amask = 0;
    for (uint32_t i = 0; i < R.ncolor; i++) if (R.cf[i]) { amask |= 1u << i; if (R.ckind[i] == 1) umask |= 1u << i; }
    int variant = DMSH_VK_MOD_BASE;
    if (fs) {
        uint32_t fm = fs->e->fout_mask;
        if ((umask & fm) && (umask & fm) == (amask & fm) && vka_shlib_module(fs->lib, fs->e, DMSH_VK_MOD_UINT)) variant = DMSH_VK_MOD_UINT;
        else if ((umask & fm) && fs->e->uint_mask == (umask & fm) && vka_shlib_module(fs->lib, fs->e, DMSH_VK_MOD_UINT_MIXED)) variant = DMSH_VK_MOD_UINT_MIXED;
        else if ((d->color.blend_en & 1) && V.dual_src && vka_shlib_module(fs->lib, fs->e, DMSH_VK_MOD_DUAL)) {
            blend_st b0 = adv_blend(d->blend[0]);
            if (is_src1(b0.src_rgb) || is_src1(b0.dst_rgb) || is_src1(b0.src_a) || is_src1(b0.dst_a)) variant = DMSH_VK_MOD_DUAL;
        }
        if ((fs->e->flags & DMSH_VK_F_ALPHA_TEST) && d->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) k.alpha_func = (uint8_t)d->color.alpha_func;
    }
    // No library variant for a float output into an integer attachment (the library has MOD_UINT only for outputs made of integer bits or constants): the base module
    // with those outputs retyped as the integer vectors of their bits (VKA_FS_RETYPE, vka_spv_fout_retype), which is what the hardware writes. DM_VK_FOUT_RETYPE=0: not written, as before
    // Only a module that was rewritten is used (one the rewrite refuses falls back to the base module, whose float outputs those targets then do not take: written = 0 below).
    // MOD_UINT_MIXED / MOD_DUAL pipelines keep their variant; a float output there into another integer target stays unwritten (open)
    if (fs && variant == DMSH_VK_MOD_BASE && VKA_ENV("DM_VK_FOUT_RETYPE", 1)) {
        uint32_t fu = 0, fsm = 0;
        for (uint32_t i = 0; i < R.ncolor; i++) if (R.cf[i] && (R.ckind[i] == 1 || R.ckind[i] == 2) && (fs->e->fout_mask >> i & 1) && (fs->e->fout_types >> (2 * i) & 3) == DMSH_VK_TYPE_FLOAT) { if (R.ckind[i] == 1) fu |= 1u << i; else fsm |= 1u << i; }
        if ((fu | fsm) && vka_shader_fout_retyped((vka_shader *)fs, fu, fsm)) variant = VKA_FS_RETYPE;
    }
    k.fs_variant = (uint8_t)variant; k.gs_fx = g_claim_words != 0;
    int src1_used = 0;
    for (uint32_t i = 0; i < R.ncolor; i++) {
        k.cfmt[i] = R.cf[i];
        if (!R.cf[i]) continue;
        uint32_t m = d->chmask.mask >> (4 * i) & 0xf;
        int written = fs && (fs->e->fout_mask >> i & 1);
        int out_cls = !fs ? 0 : variant == DMSH_VK_MOD_UINT || (variant == DMSH_VK_MOD_UINT_MIXED && (fs->e->uint_mask >> i & 1)) ? DMSH_VK_TYPE_UINT : (int)(fs->e->fout_types >> (2 * i) & 3);
        int att_cls = R.ckind[i] == 1 ? DMSH_VK_TYPE_UINT : R.ckind[i] == 2 ? DMSH_VK_TYPE_SINT : DMSH_VK_TYPE_FLOAT;
        if (variant == VKA_FS_RETYPE && out_cls == DMSH_VK_TYPE_FLOAT && att_cls != DMSH_VK_TYPE_FLOAT) out_cls = att_cls;   // (retyped in the module: vka_pso.c fout_masks)
        if (written && out_cls != att_cls) {   // Vulkan leaves a class mismatch undefined: the target is not written (SINT attachments have no variant yet)
            static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("fragment shader %016llx writes a %s output to a %s target %u: that target is not written", (unsigned long long)fs->key,
                                                                       out_cls == 0 ? "float" : out_cls == 1 ? "sint" : "uint", att_cls == 0 ? "float" : att_cls == 1 ? "sint" : "uint", i);
            written = 0;
        }
        k.att[i].mask = written ? (uint8_t)m : 0;   // outputs the shader does not write leave the target as it was (Maxwell writes only the shader's outputs)
        if (written && (d->color.blend_en >> i & 1) && att_cls == DMSH_VK_TYPE_FLOAT) {
            blend_st b = adv_blend(d->blend[i]);
            if (variant != DMSH_VK_MOD_DUAL && (is_src1(b.src_rgb) || is_src1(b.dst_rgb) || is_src1(b.src_a) || is_src1(b.dst_a))) {   // no second source: it reads as zero
                src1_used = 1; b.src_rgb = zero_src1(b.src_rgb); b.dst_rgb = zero_src1(b.dst_rgb); b.src_a = zero_src1(b.src_a); b.dst_a = zero_src1(b.dst_a); }
            k.att[i] = (typeof(k.att[i])){1, (uint8_t)bfac(b.src_rgb), (uint8_t)bfac(b.dst_rgb), (uint8_t)bop(b.eq_rgb), (uint8_t)bfac(b.src_a), (uint8_t)bfac(b.dst_a), (uint8_t)bop(b.eq_a), (uint8_t)m};
        }
    }
    if (src1_used) VRED_ONCE("a SRC1 blend factor with no dual-source module (MOD_DUAL; the profile's dual_want.tsv) or no dualSrcBlend: the second source reads as zero");

    PT(3);
    if (!mesh) vin_build(d, sh[0], &k);
    PT(4);
    VkPipeline p = vka_gfx_pipeline(sh, &lay, &k);
    if (!p) { g_skipped++; return; }
    // DM_VK_INSTANCE: a direct indexed draw of one instance with only vertex and pixel shaders, their base modules and no retyped inputs may join the open run (its
    // instanced pipeline stays bound) or start one (decided at the sets, below)
    VkBuffer i_ib = VK_NULL_HANDLE; VkDeviceSize i_io = 0; VkIndexType i_it = VK_INDEX_TYPE_UINT16;
    int icand = g_inst_on && !mesh && di && !di->ind && !expand && di->instances <= 1 && d->b.base_instance == 0 && sh[1] && !sh[2] && !sh[3] && !sh[4] && !d->rast_discard
                && !(k.vin_u | k.vin_s) && k.fs_variant == DMSH_VK_MOD_BASE && lay.push && vka_enc_on() && inst_ib(di, &i_ib, &i_io, &i_it);
    int ijoin = icand && RUN.open && p == RUN.base.p && DY.valid && DY.pipe == RUN.ipipe && vka_enc_total() == RUN.mark && inst_same_draw(&RUN.base, i_ib, i_io, i_it, di, d);
    if (!ijoin) {
        if (RUN.open) inst_close();
        if (!DY.valid || !g_dyn_filter || DY.pipe != p) { vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); DY.pipe = p; VB.vin_stale = 1; }
    }
    if (vka_nan_on()) vka_nan_draw(mesh ? sh[7]->key : sh[0]->key, fs ? fs->key : 0);
    PT(5);
    int istart = 0; uint32_t i_wv = 0, i_wp = 0; int64_t i_d = 0; vka_span i_ta = {0}, i_aa = {0};
    if (ijoin || (icand && g_ia.valid)) {
        g_sets_dry = 1; int ok = bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_GRAPHICS, NULL); g_sets_dry = 0;
        if (!ok) { if (ijoin) inst_close(); g_skipped++; return; }
        if (ijoin) {
            i_wv = RUN.wv; i_wp = RUN.wp;
            ijoin = !g_dry_pool && inst_cmp(&RUN.base, &i_wv, &i_wp, &i_d, 1) == 2 && i_d >= 0 && !(i_d & 15) && RUN.n < INST_CAP
                    && (VkDeviceSize)i_d + g_w.bi[i_wv].range <= RUN.win_v && (VkDeviceSize)i_d + g_w.bi[i_wp].range <= RUN.win_p;
            if (!ijoin) {   // not the run's draw after all: drawn alone with its own pipeline
                inst_close(); g_inst_falls++;
                vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); DY.pipe = p; VB.valid = 0; g_pd[0].n = 0;
                if (!sets_push(&lay, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dry_pool)) { g_skipped++; return; }
            }
        } else {   // a run from the last draw drawn alone: the same draw with the object block moved
            istart = !g_dry_pool && p == g_ia.p && DY.pipe == p && vka_enc_total() == g_ia.mark && inst_same_draw(&g_ia, i_ib, i_io, i_it, di, d)
                     && inst_cmp(&g_ia, &i_wv, &i_wp, &i_d, 0) == 2 && !(VI.zero && VI.zb == INST_BIND) && VI.nb < 33 && VI.na < 32;
            vka_shader *vv = istart ? inst_variant(sh[0], g_w.w[i_wv].dstBinding) : NULL, *vp = vv ? inst_variant(sh[1], g_w.w[i_wp].dstBinding) : NULL;
            VkPipeline ip = VK_NULL_HANDLE; VkDeviceSize wv = 0, wp = 0;
            if (vp) { vka_shader *ishv[VKA_NSTAGE]; memcpy(ishv, sh, sizeof ishv); ishv[0] = vv; ishv[1] = vp; ip = vka_gfx_pipeline(ishv, &lay, &k); }
            if (ip) {   // the windows: the block's buffer from the run's first offset, up to 64 KB (the translator's data[4096])
                VkBuffer b; VkDeviceSize o; uint64_t r;
                if (g_w.ad[i_wv] && (r = vka_addr_run(g_w.ad[i_wv], 65536, &b, &o)) && b == g_w.bi[i_wv].buffer && o == g_w.bi[i_wv].offset) wv = (r > 65536 ? 65536 : r) & ~(VkDeviceSize)15;
                if (g_w.ad[i_wp] && (r = vka_addr_run(g_w.ad[i_wp], 65536, &b, &o)) && b == g_w.bi[i_wp].buffer && o == g_w.bi[i_wp].offset) wp = (r > 65536 ? 65536 : r) & ~(VkDeviceSize)15;
                if (wv > V.ub_range) wv = V.ub_range & ~(VkDeviceSize)15; if (wp > V.ub_range) wp = V.ub_range & ~(VkDeviceSize)15;
            }
            istart = ip && wv >= g_w.bi[i_wv].range && wp >= g_w.bi[i_wp].range && inst_snap(&RUN.base, p, i_ib, i_io, i_it, di, d)
                     && vka_scratch(4 * INST_CAP, 16, &i_ta) && vka_scratch(20, 4, &i_aa);
            if (istart) {
                vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ip); DY.pipe = ip; VB.valid = 0;
                g_w.bi[i_wv].range = wv; g_w.bi[i_wp].range = wp;
                vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay.layout, 0, g_w.n, g_w.w); g_pd[0].n = 0;
                RUN.open = 1; RUN.ipipe = ip; RUN.wv = i_wv; RUN.wp = i_wp; RUN.win_v = wv; RUN.win_p = wp; RUN.n = 1;
                RUN.table = (uint32_t *)i_ta.ptr; RUN.args = (uint32_t *)i_aa.ptr; RUN.table[0] = 0;
            } else if (!sets_push(&lay, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dry_pool)) { g_skipped++; return; }
        }
    } else if (!bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_GRAPHICS, NULL)) { g_skipped++; return; }
    PT(6);
    if (!mesh && !ijoin) {
        if (istart) {   // the run's table as an instance-rate input at INST_LOC
            VI.b[VI.nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, INST_BIND, 4, VK_VERTEX_INPUT_RATE_INSTANCE, 1};
            VI.a[VI.na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, INST_LOC, INST_BIND, VK_FORMAT_R32_UINT, 0};
            vin_bind(d);
            VI.nb--; VI.na--;
            VkDeviceSize tsz = 4 * INST_CAP; vk.vkCmdBindVertexBuffers2(g_cb, INST_BIND, 1, &i_ta.buf, &i_ta.off, &tsz, NULL);
        } else vin_bind(d);
    }

    // Viewport and scissor. The translator writes clip space for Metal (y up, origin upper left): Metal's viewport, flipped for a lower-left window origin as on Metal, then given
    // a negative height, maps it as Metal does (android/SHADERS.md "Depth and Y")
    float vx = d->has_vp ? d->vp[0][0] : 0, vy = d->has_vp ? d->vp[0][1] : 0, vw = d->has_vp ? d->vp[0][2] : (float)R.area.extent.width, vh = d->has_vp ? d->vp[0][3] : (float)R.area.extent.height;
    if (vka_origin_ll()) { vy += vh; vh = -vh; }
    { uint32_t sw; memcpy(&sw, d->vp_sw[0], 4);
      if (sw == 0x100) { vy += vh; vh = -vh; }
      else if (sw) VRED_ONCE("viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", d->vp_sw[0][0], d->vp_sw[0][1] ^ 2, d->vp_sw[0][2] ^ 4, d->vp_sw[0][3] ^ 6); }
    float n0 = d->dr[0] < 0 ? 0 : d->dr[0] > 1 ? 1 : d->dr[0], n1 = d->dr[1] < 0 ? 0 : d->dr[1] > 1 ? 1 : d->dr[1];
    if (!vw) vw = 1; if (!vh) vh = 1;
    VkViewport vp = {vx, vy + vh, vw, -vh, n0, n1};
    DY_SET(vp, vp, vk.vkCmdSetViewport(g_cb, 0, 1, &vp));
    VkRect2D scr = R.area;
    if (has_sc && !d->rast_discard) {
        int64_t x0 = sc->x < 0 ? 0 : sc->x, y0 = sc->y < 0 ? 0 : sc->y, x1 = (int64_t)sc->x + sc->w, y1 = (int64_t)sc->y + sc->h;
        if (x1 > R.area.extent.width) x1 = R.area.extent.width; if (y1 > R.area.extent.height) y1 = R.area.extent.height;
        if (x1 <= x0 || y1 <= y0) return;   // an empty scissor: nothing drawn
        scr = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    { int xs = VKA_ENV("DM_VK_XP_SCISSOR", 0);   // experiment (wrong picture): every draw's scissor cut to xs x xs pixels, to split a pass's GPU time into per-pixel and per-draw work
      if (xs > 0) { if (scr.extent.width > (uint32_t)xs) scr.extent.width = (uint32_t)xs; if (scr.extent.height > (uint32_t)xs) scr.extent.height = (uint32_t)xs; } }
    DY_SET(sc, scr, vk.vkCmdSetScissor(g_cb, 0, 1, &scr));
    // Rasterizer and depth / stencil
    DY_SET1(cull, (uint32_t)(d->poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : d->poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : d->poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE), vk.vkCmdSetCullMode(g_cb, DY.cull));
    DY_SET1(ff, (uint32_t)((d->poly.front == NVN_FRONT_FACE_CCW) != vka_queue_origin_ll() ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE), vk.vkCmdSetFrontFace(g_cb, DY.ff));   // as nvn_mtl.m's winding
    // A mesh pipeline has no input assembly: the topology and primitive restart are not set for it, and set again at the next draw that has them (its pipeline bind may
    // have left them undefined)
    if (mesh) DY.topo = DY.pr = ~0u;
    else DY_SET1(topo, (uint32_t)(topo), vk.vkCmdSetPrimitiveTopology(g_cb, DY.topo));
    int hd = R.df != VK_FORMAT_UNDEFINED, hs = R.sf != VK_FORMAT_UNDEFINED;
    DY_SET1(dt, (uint32_t)(hd && d->ds.dt), vk.vkCmdSetDepthTestEnable(g_cb, DY.dt));
    DY_SET1(dw, (uint32_t)(hd && d->ds.dt && d->ds.dw), vk.vkCmdSetDepthWriteEnable(g_cb, DY.dw));
    DY_SET1(dc, (uint32_t)(d->ds.df >= 1 && d->ds.df <= 8 ? (VkCompareOp)(d->ds.df - 1) : VK_COMPARE_OP_ALWAYS), vk.vkCmdSetDepthCompareOp(g_cb, DY.dc));
    DY_SET1(dbt, (uint32_t)(VK_FALSE), vk.vkCmdSetDepthBoundsTestEnable(g_cb, DY.dbt));
    DY_SET1(stt, (uint32_t)(hs && d->ds.st), vk.vkCmdSetStencilTestEnable(g_cb, DY.stt));
    #define SOP(x) ((x) >= 1 && (x) <= 8 ? (VkStencilOp)((x) - 1) : VK_STENCIL_OP_KEEP)
    #define SCMP(x) ((x) >= 1 && (x) <= 8 ? (VkCompareOp)((x) - 1) : VK_COMPARE_OP_ALWAYS)
    { uint32_t so[8] = {SOP(d->ds.fail_f), SOP(d->ds.zpass_f), SOP(d->ds.zfail_f), SCMP(d->ds.sf_f), SOP(d->ds.fail_b), SOP(d->ds.zpass_b), SOP(d->ds.zfail_b), SCMP(d->ds.sf_b)};
      DY_SET(sop, so, (vk.vkCmdSetStencilOp(g_cb, VK_STENCIL_FACE_FRONT_BIT, so[0], so[1], so[2], so[3]), vk.vkCmdSetStencilOp(g_cb, VK_STENCIL_FACE_BACK_BIT, so[4], so[5], so[6], so[7]))); }
    #undef SOP
    #undef SCMP
    { uint32_t sm[6] = {d->st_vmask[0], d->st_vmask[1], d->st_wmask[0], d->st_wmask[1], d->st_ref[0], d->st_ref[1]};
      DY_SET(smask, sm, (vk.vkCmdSetStencilCompareMask(g_cb, VK_STENCIL_FACE_FRONT_BIT, sm[0]), vk.vkCmdSetStencilCompareMask(g_cb, VK_STENCIL_FACE_BACK_BIT, sm[1]),
                         vk.vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_FRONT_BIT, sm[2]), vk.vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_BACK_BIT, sm[3]),
                         vk.vkCmdSetStencilReference(g_cb, VK_STENCIL_FACE_FRONT_BIT, sm[4]), vk.vkCmdSetStencilReference(g_cb, VK_STENCIL_FACE_BACK_BIT, sm[5]))); }
    int bias_bit = d->poly.mode == NVN_POLYGON_MODE_POINT ? NVN_POLYGON_OFFSET_ENABLE_POINT : d->poly.mode == NVN_POLYGON_MODE_LINE ? NVN_POLYGON_OFFSET_ENABLE_LINE : NVN_POLYGON_OFFSET_ENABLE_FILL;
    int bias = (d->poly.offset_en & bias_bit) != 0;
    DY_SET1(dbe, (uint32_t)(bias), vk.vkCmdSetDepthBiasEnable(g_cb, DY.dbe));
    { float db[3] = {bias ? d->po.units : 0, bias && V.depth_bias_clamp ? d->po.clamp : 0, bias ? d->po.factor : 0}; DY_SET(db, db, vk.vkCmdSetDepthBias(g_cb, db[0], db[1], db[2])); }
    DY_SET1(rd, (uint32_t)(d->rast_discard != 0), vk.vkCmdSetRasterizerDiscardEnable(g_cb, DY.rd));
    DY_SET(bc, d->blend_color, vk.vkCmdSetBlendConstants(g_cb, d->blend_color));
    if (!DY.valid) vk.vkCmdSetLineWidth(g_cb, 1.0f);
    int restart = di && d->restart.on;
    if (restart) { uint32_t all = di->itype == 0 ? 0xff : di->itype == 1 ? 0xffff : 0xffffffffu; if (d->restart.index != all) VRED_ONCE("primitive restart index %#x is not the all-ones index Vulkan restarts at", d->restart.index); }
    if (!mesh) DY_SET1(pr, (uint32_t)(restart), vk.vkCmdSetPrimitiveRestartEnable(g_cb, DY.pr));
    DY.valid = 1;

    PT(7);
    // The draw
    uint32_t inst = (uint32_t)((dw ? dw->instances : di->instances) > 0 ? (dw ? dw->instances : di->instances) : 1), binst = d->b.base_instance;
    if (mesh) {   // count workgroups of the task shader, or of the mesh shader without one (the device's limit per dimension: maxTaskWorkGroupCount / maxMeshWorkGroupCount, 65535 on the Adreno 840)
        static uint32_t largest; if ((uint32_t)dw->count > largest) { largest = (uint32_t)dw->count; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("mesh draw: largest so far %u workgroups (task %016llx mesh %016llx)", largest, sh[6] ? (unsigned long long)sh[6]->key : 0ull, (unsigned long long)sh[7]->key); }
        uint32_t maxg = sh[6] ? V.task_groups : V.mesh_groups;
        if ((uint32_t)dw->count > maxg) SKIP("mesh draw of %d workgroups: more than the device's %u in one dimension; skipped", dw->count, maxg);
        vk.vkCmdDrawMeshTasksEXT(g_cb, (uint32_t)dw->count, 1, 1);
    } else if (expand) {   // quads or a line loop: the indices made here, 32-bit, in scratch memory
        uint32_t count = (uint32_t)(dw ? dw->count : di->count), *src = NULL;
        if (di && !(src = read_indices(di->ibuf, di->itype, count))) SKIP("indices at %#llx are not readable: the draw is skipped", (unsigned long long)di->ibuf);
        uint32_t nout = expand == 1 ? count / 4 * 6 : count + 1; vka_span s;
        if (!nout || !vka_scratch(4 * (VkDeviceSize)nout, 4, &s)) { free(src); return; }
        uint32_t *o = (uint32_t *)s.ptr, base = dw ? (uint32_t)dw->first : 0;
        #define IX(i) (src ? src[i] : base + (i))
        if (expand == 1) for (uint32_t q = 0; q < count / 4; q++) { static const uint8_t t[6] = {0, 1, 2, 0, 2, 3}; for (int j = 0; j < 6; j++) o[6 * q + j] = IX(4 * q + t[j]); }
        else { for (uint32_t i = 0; i < count; i++) o[i] = IX(i); o[count] = IX(0); }
        #undef IX
        free(src);
        ib_bind(s.buf, s.off, VK_INDEX_TYPE_UINT32);
        vk.vkCmdDrawIndexed(g_cb, nout, inst, 0, di ? di->base_vertex : 0, binst);
    } else if (dw) {
        if (ind) { VkBuffer b; VkDeviceSize o; if (!vka_addr(ind, 16, &b, &o, NULL) || (o & 3)) SKIP("indirect draw arguments at %#llx are not in placed guest RAM: skipped", (unsigned long long)ind);
                   VRED_ONCE("indirect draws: outside vertex shaders, the driver segment's base instance (c0 + 0x644) reads the record's, not the GPU's"); vk.vkCmdDrawIndirect(g_cb, b, o, 1, 16); }
        else vk.vkCmdDraw(g_cb, (uint32_t)dw->count, inst, (uint32_t)dw->first, binst);
    } else {
        VkBuffer ib; VkDeviceSize io; VkIndexType it = di->itype == 0 ? VK_INDEX_TYPE_UINT8 : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        size_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (it == VK_INDEX_TYPE_UINT8 && !V.idx8) {   // 8-bit indices on a device without them: widened on the CPU
            if (ind) SKIP("an indirect draw of 8-bit indices on a device without them: skipped");
            uint32_t *src = read_indices(di->ibuf, 0, (uint32_t)di->count); vka_span s;
            if (!src || !vka_scratch(2 * (VkDeviceSize)di->count + 4, 4, &s)) { free(src); SKIP("8-bit indices at %#llx not readable: skipped", (unsigned long long)di->ibuf); }
            for (int i = 0; i < di->count; i++) ((uint16_t *)s.ptr)[i] = (uint16_t)src[i];
            free(src); ib = s.buf; io = s.off; it = VK_INDEX_TYPE_UINT16;
            if (restart) VRED_ONCE("primitive restart with widened 8-bit indices");
        } else if (ijoin) {   // the run's next object: its offset in the table, one more instance; nothing recorded unless something was since the run's last draw
            if (vka_enc_total() == RUN.mark) { RUN.table[RUN.n++] = (uint32_t)(i_d / 16); RUN.args[1] = RUN.n; g_inst_joined++; g_draws++; inst_fast_arm(d, rt, has_rt, sc, has_sc, di, 0); PT(8); return; }
            inst_close(); g_inst_falls++;   // (a dynamic state was set for it): drawn alone after all
            vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); DY.pipe = p; VB.valid = 0; g_pd[0].n = 0;
            vin_bind(d);
            if (!sets_push(&lay, VK_PIPELINE_BIND_POINT_GRAPHICS, 0)) { g_skipped++; return; }
            ijoin = 0; ib = i_ib; io = i_io; it = i_it;
        } else if (!vka_addr(di->ibuf, ind ? isz : isz * (uint64_t)(di->count > 0 ? di->count : 1), &ib, &io, NULL) || io % isz) SKIP("index buffer at %#llx is not in placed guest RAM: skipped", (unsigned long long)di->ibuf);
        ib_bind(ib, io, it);
        if (ind) { VkBuffer b; VkDeviceSize o; if (!vka_addr(ind, 20, &b, &o, NULL) || (o & 3)) SKIP("indirect draw arguments at %#llx are not in placed guest RAM: skipped", (unsigned long long)ind);
                   vk.vkCmdDrawIndexedIndirect(g_cb, b, o, 1, 20); }
        else if (istart) {   // the run's first draw: indirect, its instance count raised by the draws that join
            RUN.args[0] = (uint32_t)di->count; RUN.args[1] = 1; RUN.args[2] = 0; RUN.args[3] = (uint32_t)di->base_vertex; RUN.args[4] = 0;
            vk.vkCmdDrawIndexedIndirect(g_cb, i_aa.buf, i_aa.off, 1, 20); RUN.mark = vka_enc_total(); g_inst_runs++;
            inst_fast_arm(d, rt, has_rt, sc, has_sc, di, 1);
        } else vk.vkCmdDrawIndexed(g_cb, (uint32_t)di->count, inst, 0, di->base_vertex, binst);
    }
    g_draws++;
    if (icand && !istart && !RUN.open) { if (inst_snap(&g_ia, p, i_ib, i_io, i_it, di, d)) g_ia.mark = vka_enc_total(); } else g_ia.valid = 0;   // a run may start from this draw
    PT(8);
    if (vka_nan_on() >= 2 && vka_nan_split(R.cim, R.nci)) vka_rp_end();   // DM_VK_NANSCAN=2: a scan inside the game's pass
}
// The whole of a draw, every exit included (the instanced fast path, a skipped one, one whose indirect count is past): its total per draw against the recording's,
// so what the phases leave out is named instead of guessed (DM_VK_PROF)
void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di) {
    if (!g_prof) { vka_draw_(d, rt, has_rt, sc, has_sc, dw, di); return; }
    uint64_t t0 = cntvct();
    vka_draw_(d, rt, has_rt, sc, has_sc, dw, di);
    g_dtot += cntvct() - t0; g_dn++;
}

int dm_sdk_ounce(void);
static int g_nvn2 = -1;
void vka_set_nvn2(int on) { g_nvn2 = on != 0; }
int vka_nvn2(void) { if (g_nvn2 < 0) g_nvn2 = dm_sdk_ounce(); return g_nvn2; }   // first called at a dispatch or a capture, after the modules are loaded
// ---------- FSR 1 in place of a game's neural upscaler (DM_VK_NDSR=2) ----------
// AMD's EASU (and RCAS, DM_VK_NDSR_SHARP) as compute passes from the upscaler's input texture to its output image (vka_ndsr_fsr.h, dm/gfx/vk_android/fsr/): edge-adaptive
// instead of the stretch's bilinear blit, for a fraction of the emulated network's GPU time. With RCAS the EASU pass writes the replay's own intermediate image of the output's
// size and RCAS writes the output. 0 = not done (no push descriptors, another output format than B10G11R11, no view): the caller blits
#include "vka_ndsr_fsr.h"
static struct { int state; VkDescriptorSetLayout dsl; VkPipelineLayout lay; VkPipeline easu, rcas; VkSampler samp; VkImage mid; VkDeviceMemory mem; VkImageView mv; uint32_t mw, mh; } g_nf;
static void nf_mid_free(void *p) { VkImage *a = p; vk.vkDestroyImageView(V.dev, (VkImageView)(uintptr_t)a[1], NULL); vk.vkDestroyImage(V.dev, a[0], NULL); vk.vkFreeMemory(V.dev, (VkDeviceMemory)(uintptr_t)a[2], NULL); free(p); }
static int nf_ready(void) {
    if (g_nf.state) return g_nf.state > 0;
    g_nf.state = -1;
    if (!V.push_desc) { VRED("NDSR FSR: no push descriptors: the stretch instead"); return 0; }
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 72};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_nf.dsl);
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_nf.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (!r) r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_nf.lay);
    if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_nf.samp);
    const uint32_t *code[2] = {vka_ndsr_easu, vka_ndsr_rcas}; size_t size[2] = {sizeof vka_ndsr_easu, sizeof vka_ndsr_rcas}; VkPipeline *out[2] = {&g_nf.easu, &g_nf.rcas};
    for (int k = 0; k < 2 && !r; k++) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[k], .pCode = code[k]}; VkShaderModule m = VK_NULL_HANDLE;
        r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_nf.lay};
        if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, out[k]);
        if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    }
    if (r) { VRED("NDSR FSR: pipelines %d: the stretch instead", r); return 0; }
    g_nf.state = 1; return 1;
}
static int nf_mid(uint32_t w, uint32_t h) {   // the intermediate image for RCAS (B10G11R11, storage and sampled, kept in GENERAL); the old one goes once the GPU is past it
    if (g_nf.mid && g_nf.mw == w && g_nf.mh == h) return 1;
    if (g_nf.mid) { VkImage *a = malloc(3 * sizeof *a); if (a) { a[0] = g_nf.mid; a[1] = (VkImage)(uintptr_t)g_nf.mv; a[2] = (VkImage)(uintptr_t)g_nf.mem; vka_after(vka_fence_last(), nf_mid_free, a); } }
    g_nf.mid = VK_NULL_HANDLE; g_nf.mv = VK_NULL_HANDLE; g_nf.mem = VK_NULL_HANDLE; g_nf.mw = g_nf.mh = 0;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &g_nf.mid);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, g_nf.mid, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &g_nf.mem);
    if (!r) r = vk.vkBindImageMemory(V.dev, g_nf.mid, g_nf.mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_nf.mid, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, &g_nf.mv);
    if (r) { VRED_ONCE("NDSR FSR: intermediate image %ux%u: %d; sharpening off", w, h, r); return 0; }
    vka_img_barrier(g_cb, g_nf.mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    g_nf.mw = w; g_nf.mh = h; return 1;
}
static uint32_t nf_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void nf_pass(VkPipeline pipe, VkImageView src, VkImageLayout ls, VkImageView dst, VkImageLayout ld, const uint32_t pc[18], uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[2] = {{g_nf.samp, src, ls}, {VK_NULL_HANDLE, dst, ld}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1]}};
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_nf.lay, 0, 2, wr);
    vk.vkCmdPushConstants(g_cb, g_nf.lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, 72, pc);
    vk.vkCmdDispatch(g_cb, (w + 7) / 8, (h + 7) / 8, 1);
}
static int ndsr_fsr(int tin, int tout, vka_img *a, vka_img *b) {
    if (!nf_ready() || b->f.vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return 0;
    static int st_ok = -1;
    if (st_ok < 0) { VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, b->f.vk, &fp); st_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
                     if (!st_ok) VRED("NDSR FSR: B10G11R11 is no storage image format here: the stretch instead"); }
    if (!st_ok || !(b->usage & VK_IMAGE_USAGE_STORAGE_BIT) || !(a->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return 0;
    VkImageLayout la = VK_IMAGE_LAYOUT_GENERAL, lb = VK_IMAGE_LAYOUT_GENERAL;
    VkImageView va = vka_tex_id_view_rec(tin, g_cb, 1, 0, &la), vb = vka_tex_id_view_rec(tout, g_cb, 1, 1, &lb);   // dim 1: 2D; sampled / storage
    if (!va || !vb) return 0;
    float sharp = VKA_ENVF("DM_VK_NDSR_SHARP", 0.25);   // RCAS's sharpness in stops (0 = the most, AMD's default 0.25); below 0: no RCAS
    int rcas = sharp >= 0 && nf_mid(b->w, b->h);
    float sx = (float)a->w / b->w, sy = (float)a->h / b->h, rw = 1.0f / a->w, rh = 1.0f / a->h;
    float easu[16] = {sx, sy, 0.5f * sx - 0.5f, 0.5f * sy - 0.5f, rw, rh, rw, -rh, -rw, 2 * rh, rw, 2 * rh, 0, 4 * rh, 0, 0};   // AMD's FsrEasuCon for the whole image
    uint32_t pc[18] = {0}; for (int i = 0; i < 16; i++) pc[i] = nf_bits(easu[i]); pc[16] = b->w; pc[17] = b->h;
    vka_barrier(g_cb);
    nf_pass(g_nf.easu, va, la, rcas ? g_nf.mv : vb, rcas ? VK_IMAGE_LAYOUT_GENERAL : lb, pc, b->w, b->h);
    if (rcas) {
        vka_barrier(g_cb);
        memset(pc, 0, 64); pc[0] = nf_bits(exp2f(-sharp));   // FsrRcasCon's con0.x (the packed half at word 1 is for the 16-bit path)
        nf_pass(g_nf.rcas, g_nf.mv, VK_IMAGE_LAYOUT_GENERAL, vb, lb, pc, b->w, b->h);
    }
    vka_barrier(g_cb);
    static int noted; if (!noted++) VLOG("NDSR upscaler replaced by FSR 1 (EASU%s): %ux%u -> %ux%u (DM_VK_NDSR: 0 the game's network, 1 the stretch, 2 FSR 1, 3 nearest)", rcas ? " + RCAS" : "", a->w, a->h, b->w, b->h);
    return 1;
}
void vka_dispatch(vka_dstate *d, const p_dispatch *dp) {
    if (!g_cb || !draws_on() || !statics()) { g_skipped++; return; }
    if (!d->inited) vka_dstate_reset(d);
    vka_rp_end();
    vka_shader *sh[VKA_NSTAGE] = {0}; unsigned bad = 0;
    if (!resolve(d->cprog, 5, &sh[5], &bad)) SKIP("dispatch skipped: no shader library");
    if (bad) { g_skipped++; return; }
    if (!sh[5]) SKIP("dispatch with no compute shader bound: skipped");
    vka_layout lay; if (!vka_layout_of(sh, 1, &lay)) SKIP("dispatch skipped: no pipeline layout");
    VkPipeline p = vka_cs_pipeline(sh[5], &lay);
    if (vka_nan_on()) vka_tex_watch_dispatch(sh[5]->key);
    if (!p) { g_skipped++; return; }
    d->b.dispatch[0] = (uint32_t)dp->x; d->b.dispatch[1] = (uint32_t)dp->y; d->b.dispatch[2] = (uint32_t)dp->z; d->b.nvn2 = (uint32_t)vka_nvn2();
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0;
    if (dp->ind && (!vka_addr(dp->ind, 12, &ib, &io, NULL) || (io & 3))) SKIP("indirect dispatch arguments at %#llx are not in placed guest RAM: skipped", (unsigned long long)dp->ind);
    uint32_t sb = spill_bytes(sh[5]); uint64_t ns = 0;   // shared memory in the spill buffer: the slices this dispatch binds (spill_buf)
    if (sb) {
        if (!spill_buf(sb)) SKIP("dispatch of %016llx skipped: no shared memory spill buffer of %u bytes", (unsigned long long)sh[5]->key, sb);
        uint64_t cap = g_spill_cap / sb, total = dp->ind ? cap : (uint64_t)(dp->x > 0 ? dp->x : 0) * (uint64_t)(dp->y > 0 ? dp->y : 0) * (uint64_t)(dp->z > 0 ? dp->z : 0);
        ns = total < cap ? total : cap; if (!ns) ns = 1;
        g_spill_range = ns * sb;
        if (dp->ind) VRED_ONCE("indirect dispatch of a program with its shared memory in the spill buffer (%016llx): its %llu slices are shared by workgroups that many apart", (unsigned long long)sh[5]->key, (unsigned long long)ns);
    }
    c0_at c0 = {0};
    walk_all(d, sh);
    // A game's NDSR upscaler (agl's neural super resolution, used by Donkey Kong Bananza: the scene's colour to the upscaler's target, both B10G11R11): its tensor
    // (HMMA) arithmetic has no Adreno equivalent and runs emulated, 10-27 ms of GPU time a frame on an Adreno 740, more the larger its output. DM_VK_NDSR (live) picks
    // what runs instead: 0 = the game's network, 1 = a linear blit of its input onto its output, 2 = FSR 1 (the default; the linear blit where it cannot run),
    // 3 = a nearest-neighbour blit. Anything that does not fit runs the network
    { static int fmt_ok = -1, noted; int mode = VKA_ENV("DM_VK_NDSR", 2), on = mode > 0;
      // Every variant of the network: the exact 2x one (14a8aaf3e0a9edbd) and the others the game picks at other ratios (9e5597e2fcfc3c2f at 1.833: 27 ms of the GPU at
      // 1080p, a hitch at every step of the dynamic resolution, 2026-11-05), known by their shape: 128-wide workgroups in x, 8-12 KB of shared memory, flags 0x84
      const dmsh_vk_entry *ce = sh[5]->e;
      // (seen: 14a8aaf3e0a9edbd 2x, 9e5597e2fcfc3c2f, a9fed0568668bf97, 7629d857a0a93833 at 67 %, 3b716012fcd10575 at 60 %: 9.6-10.9 KB of shared memory)
      int ndsr = sh[5]->key == 0x14a8aaf3e0a9edbdull || sh[5]->key == 0x9e5597e2fcfc3c2full || sh[5]->key == 0x7629d857a0a93833ull || sh[5]->key == 0x3b716012fcd10575ull
                 || (ce->local[0] == 128 && ce->local[1] == 1 && ce->local[2] == 1 && ce->shared >= 8000 && ce->shared <= 12000 && (ce->flags & 0x84) == 0x84);
      if (on && ndsr) {
        int tin = 0, tout = 0;
        for (int i = 0; i < g_wk.n[5]; i++) { const vka_res *r = &g_wk.r[5][i];
          if (r->desc == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && r->tid) tin = r->tid; else if (r->desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && r->tid) tout = r->tid; }
        vka_img *a = tin ? vka_tex_id_img(tin, g_cb) : NULL, *b = tout ? vka_tex_id_img(tout, g_cb) : NULL;
        if (a && b && fmt_ok < 0) {
          VkFormatProperties fa, fb; vk.vkGetPhysicalDeviceFormatProperties(V.pd, a->f.vk, &fa); vk.vkGetPhysicalDeviceFormatProperties(V.pd, b->f.vk, &fb);
          fmt_ok = (fa.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (fa.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                && (fb.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
          if (!fmt_ok) VRED("NDSR blit: format %d -> %d cannot be blitted with a linear filter on this device: the network runs (DM_VK_NDSR)", a->f.vk, b->f.vk);
        }
        if (a && b && mode == 2 && a->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && a->samples <= 1 && b->samples <= 1 && ndsr_fsr(tin, tout, a, b)) {
            g_dispatches++; return; }
        if (a && b && fmt_ok > 0 && (a->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (b->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            && a->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && a->samples <= 1 && b->samples <= 1) {
          VkImageLayout la = VK_IMAGE_LAYOUT_GENERAL, lb = VK_IMAGE_LAYOUT_GENERAL;
          vka_tex_id_view_rec(tin, g_cb, 1, 0, &la); vka_tex_id_view_rec(tout, g_cb, 1, 1, &lb);   // dim 1: 2D   // the layouts the images are kept in
          vka_barrier(g_cb);
          VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)a->w, (int32_t)a->h, 1}},
                            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)b->w, (int32_t)b->h, 1}}};
          vk.vkCmdBlitImage(g_cb, a->img, la, b->img, lb, 1, &bl, mode == 3 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
          vka_barrier(g_cb);
          static uint32_t lw, lh, lo; if (a->w != lw || a->h != lh || b->w != lo) { lw = a->w; lh = a->h; lo = b->w; noted = 0; }   // logged again at every size change
          { static uint64_t seen[16]; int k = 0; for (; k < 16 && seen[k] && seen[k] != sh[5]->key; k++) {} if (k < 16 && !seen[k]) { seen[k] = sh[5]->key; VLOG("NDSR network %016llx replaced by a linear blit (%ux%u -> %ux%u)", (unsigned long long)sh[5]->key, a->w, a->h, b->w, b->h); } }
          if (noted++ < 1) VLOG("NDSR upscaler replaced by a linear blit: texture %d (%ux%u) -> image %d (%ux%u), layouts %d -> %d (DM_VK_NDSR=0: the network)", tin, a->w, a->h, tout, b->w, b->h, la, lb);
          g_dispatches++;
          return;
        }
      } }
    if (!bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_COMPUTE, &c0)) { g_skipped++; return; }
    if (dp->ind && c0.buf) {   // the group counts the GPU wrote, into c0's dispatch words before the dispatch reads them (nvn_mtl.m g_c0_dim_buf); NVN2 reads them at c0 + 4 too
        VkBufferCopy c[2] = {{io, c0.off + DMSH_C0_DISPATCH, 12}, {io, c0.off + DMSH_C0_DISPATCH_NVN2, 12}};
        vk.vkCmdCopyBuffer(g_cb, ib, c0.buf, d->b.nvn2 ? 2 : 1, c);
        vka_barrier(g_cb);
    }
    // DM_VK_DISPATCH_LOG=1: one line per dispatch (its index in the submission's run of dispatches since the last log reset, key, groups, workgroup, shared memory, flags);
    // DM_VK_DISPATCH_SKIP=<from>[,<to>]: dispatches from..to (default: to the end) are recorded without their vkCmdDispatch, to find the one that writes a wrong picture
    uint64_t vka_frame(void); static unsigned long long nd; static uint64_t fr = ~0ull; static int lg; if (vka_frame() != fr) { fr = vka_frame(); lg = (int)VKA_ENV("DM_VK_DISPATCH_LOG", 0); nd = 0; }
    unsigned long long di = nd++; const char *sk = getenv("DM_VK_DISPATCH_SKIP"); int skip = 0;
    if (sk && *sk) { char *e; unsigned long long a = strtoull(sk, &e, 10), b = *e == ',' ? strtoull(e + 1, NULL, 10) : ~0ull; skip = di >= a && di <= b; }
    if (lg) VLOG("dispatch %llu: %016llx groups %d,%d,%d%s local %u,%u,%u shared %u flags %#x%s", di, (unsigned long long)sh[5]->key, dp->x, dp->y, dp->z, dp->ind ? " (indirect)" : "",
                 sh[5]->e->local[0], sh[5]->e->local[1], sh[5]->e->local[2], sh[5]->e->shared, sh[5]->e->flags, skip ? " SKIPPED" : "");
    if (skip) { vka_barrier(g_cb); g_dispatches++; return; }
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (dp->ind) vk.vkCmdDispatchIndirect(g_cb, ib, io);
    else if (dp->x > 0 && dp->y > 0 && dp->z > 0) {
        if (sb && (uint64_t)dp->x * dp->y * dp->z > ns) spill_dispatch((uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z, ns);
        else vk.vkCmdDispatch(g_cb, (uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z);
    }
    vka_barrier(g_cb);   // (also frees the shared memory spill buffer for the next dispatch)
    g_dispatches++;
}
int vka_prof_on(void) { return g_prof; }   // DM_VK_PROF: the recording loop (vka_exec.c) times its records by the same switch
void vka_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *dispatches, uint64_t *pipelines) {
    if (draws) *draws = g_draws; if (skipped) *skipped = g_skipped; if (dispatches) *dispatches = g_dispatches; if (pipelines) *pipelines = 0;
}
#endif
