// Draws and dispatches of the Vulkan replay: nvn_mtl.m's draw path on Vulkan. vka_exec.c walks the records; state records land in the queue's vka_dstate (vka_state_op),
// and each draw resolves its program to shaders (vka_shader.c), opens the dynamic-rendering pass of the bound targets if none is open, takes a pipeline keyed on the state
// Vulkan cannot set dynamically here (vka_pso.c), writes set 0 from the binding records (vka_bind.c's walker: push descriptors when they fit, else a set from the submission
// slot's pool), sets the dynamic state and draws. The pass stays open across draws and closes at anything else (vka_rp_end), followed by a full barrier.
// What is not here yet, each logged once: SINT attachments and integer vertex formats (no variants), transform feedback, DrawTexture, line loops and quads through indirect draws,
// tessellation's patch size, texel buffers, viewport swizzles other than a y flip
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_gmem.h"
#include "../nvn/nvn_mtl.h"
#include "../../host/plat/plat.h"
#include "vka_gml.h"
#include "game_profile.h"
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static VkCommandBuffer g_cb; static unsigned g_slot; static int g_prof, g_dyn_filter = 1, g_pass_keep = 1, g_vb_filter = 1;   // g_prof: this draw is timed (vka_draw)
static int g_prof_all, g_prof_every;   // DM_VK_PROF: every draw timed; the pace line's sample (DM_VK_PACE_PROF): one draw in g_prof_every
// DM_VK_PROF_IMG (diagnostic, default 0): the image descriptors' own split in the pace line (how much of them is the ID's view lookup and how much everything after it).
// It costs two counter reads per image descriptor -- six or so a draw -- which was a tenth of a draw's recording time, so it is off unless asked for
static int g_prof_img;
// The two per-descriptor diagnostics are off unless asked for: class_note walks a 4096-entry table for every image descriptor of every draw to log at most 64 lines in a
// run, and tex_peek re-read the switch's list each time. DM_VK_CLASS_NOTE=1 brings the first back (it explains "reads as unbound" lines); DM_VK_TEX_PEEK its own
static int g_class_note, g_tex_peek_any;
static int g_vin_skip = 1;             // DM_VK_VIN_SKIP (vin_build_kept)
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
    int on = g_force_draws || (env >= 0 ? env : (V.placed || V.fd_alias));
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
    d->s_vfmt = vka_stamp(); d->s_vbuf = vka_stamp(); d->s_push = vka_stamp();
    for (int st = 0; st < VKA_NSTAGE; st++) { d->s_ssbo[st] = vka_stamp(); d->s_tex[st] = vka_stamp(); }
}
static _Atomic uint64_t g_stamp;
uint64_t vka_stamp(void) { return atomic_fetch_add_explicit(&g_stamp, 1, memory_order_relaxed) + 1; }
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
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; d->s_vfmt = vka_stamp(); if (b->first >= 0 && b->first + b->n <= 16) { memcpy(&d->attr[b->first], b->st, 4 * (size_t)b->n); if (b->first + b->n > d->nattr) d->nattr = b->first + b->n; } break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; d->s_vfmt = vka_stamp(); if (b->first >= 0 && b->first + b->n <= 16) { memcpy(&d->stream[b->first], b->st, 8 * (size_t)b->n); if (b->first + b->n > d->nstream) d->nstream = b->first + b->n; } break; }
    case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); d->s_vbuf = vka_stamp(); if (b->first >= 0 && b->first + b->n <= 16) memcpy(&d->vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
    case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->index >= 0 && b->index < 16) { d->ubo_dirty[b->stage] |= 1u << b->index; d->b.ubo[b->stage][b->index] = (vka_range){b->addr, b->size}; } break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->index >= 0 && b->index < 16) { d->b.ssbo[b->stage][b->index] = (vka_range){b->addr, b->size}; d->s_ssbo[b->stage] = vka_stamp(); } break; }
    case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
        if (b->n < 0 || b->n > 32) { VRED_ONCE("texture bind record with count %d: overwritten after recording", b->n); break; }
        uint64_t h[32]; memcpy(h, b->h, 8 * (size_t)b->n);
        if (b->defer && b->src && b->n) {   // BindTexturesDeferred: the handles as memory holds them now (nvn_mtl.m bh_fresh); placed guest memory read directly, else word by word through the fault-safe read
            uint32_t w[32];
            if (vka_addr(b->src, 4u * (uint64_t)b->n, NULL, NULL, NULL)) { memcpy(w, (const void *)(uintptr_t)b->src, 4 * (size_t)b->n); for (int k = 0; k < b->n; k++) h[k] = w[k]; }
            else if (!plat_mem_read(b->src, w, 4 * (size_t)b->n)) for (int k = 0; k < b->n; k++) h[k] = w[k];   // one read for the run (they are in the game's heap; profile p2: 3.4 % in process_vm_readv)
            else for (int k = 0; k < b->n; k++) if (!plat_mem_read(b->src + 4u * k, &w[k], 4)) h[k] = w[k];   // a run across an unreadable page: word by word
        }
        if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->first + b->n <= VKA_NTEX) { memcpy(&d->b.tex[b->stage][b->first], h, 8 * (size_t)b->n); d->s_tex[b->stage] = vka_stamp(); }
        break; }
    case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->n >= 0 && b->first + b->n <= 128) { memcpy(&d->b.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); d->s_tex[b->stage] = vka_stamp(); } break; }
    case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->n >= 0 && b->first + b->n <= 32) { memcpy(&d->b.img[b->stage][b->first], b->h, 8 * (size_t)b->n); d->s_tex[b->stage] = vka_stamp(); } break; }
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
    case OP_PUSH_CONST: { const p_push_const *b = a; if ((uint32_t)b->offset + b->size <= DMSH_PUSH_SIZE) { memcpy(d->b.push[b->compute ? 1 : 0] + b->offset, b->data, b->size); d->s_push = vka_stamp(); } break; }
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
static void sets_memo_drop(void);
void vka_draw_sub_begin(VkCommandBuffer cb, unsigned slot) {
    g_cb = cb; g_slot = slot % NSLOT; vka_dyn_invalidate();
    { void vka_mem_rec_thread(void); vka_mem_rec_thread(); }   // the address lookups' cache outside TLS for the recording thread (vka_mem.c)
    drv_memo_reset();   // the last submission's scratch spans go with its fence
    g_prof_all = VKA_ENV("DM_VK_PROF", 0) != 0; g_prof = g_prof_all; g_prof_every = VKA_ENV("DM_VK_PACE_LOG", 5000) > 0 ? VKA_ENV("DM_VK_PACE_PROF", 16) : 0; g_prof_img = VKA_ENV("DM_VK_PROF_IMG", 0); g_class_note = VKA_ENV("DM_VK_CLASS_NOTE", 0); g_tex_peek_any = VKA_ENV("DM_VK_TEX_PEEK_ANY", 0) || getenv("DM_VK_TEX_PEEK") != NULL; g_dyn_filter = VKA_ENV("DM_VK_DYN_FILTER", 1); g_pass_keep = VKA_ENV("DM_VK_PASS_KEEP", 1); g_vb_filter = VKA_ENV("DM_VK_VB_FILTER", 1); g_inst_on = VKA_ENV("DM_VK_INSTANCE", 0);   // live: from this submission
    g_vin_skip = VKA_ENV("DM_VK_VIN_SKIP", 1);
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
    struct { vka_img *im, *sh; uint32_t lv, l0, nl, bw, bh; } bc[8]; int nbc;   // block-compressed targets drawn through their shadows (bcrt_attach), copied back at the pass end
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
int vka_classic_rp(void) {   // decided once, after the device is made (V.imageless): pipelines and passes must agree for the whole run
    static int on = -1;
    if (on < 0 && V.dev) { const char *e = getenv("DM_VK_CLASSIC_RP"); int want = e && atoi(e) != 0; on = want && V.imageless;
        if (on) VLOG("classic render passes (DM_VK_CLASSIC_RP)"); else if (want) VRED("classic render passes (DM_VK_CLASSIC_RP) need imagelessFramebuffer, which this device lacks: dynamic rendering"); }
    return on > 0;
}
typedef struct { VkFormat cf[8]; uint32_t n; VkFormat ds; uint32_t d, s, samples, clear; } rpkey;   // clear: bit i colour i, bit 8 depth, bit 9 stencil
// Every shape's render pass is kept for the run (the pipelines made against it hold no reference, but each pass begun with it does until its fence): the list grows, so a
// shape past any cap never makes a new VkRenderPass per pass (the 256-entry array this was did, and leaked one at each)
typedef struct { rpkey k; VkRenderPass rp; } rpent;
static rpent *g_rpc; static int g_nrpc, g_rpcap; static pthread_mutex_t g_rpmu = PTHREAD_MUTEX_INITIALIZER;
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
    else {
        if (g_nrpc == g_rpcap) { int nc = g_rpcap ? 2 * g_rpcap : 64; rpent *n = realloc(g_rpc, (size_t)nc * sizeof *n); if (n) { g_rpc = n; g_rpcap = nc; } }
        if (g_nrpc < g_rpcap) { g_rpc[g_nrpc].k = *k; g_rpc[g_nrpc++].rp = r; } else VRED_ONCE("render pass list: out of memory");
        if ((g_nrpc & (g_nrpc - 1)) == 0 && g_nrpc >= 256) VLOG("classic render passes: %d shapes", g_nrpc);
    }
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
// Imageless framebuffers by shape, kept for the run (they name no image, so an image's end does not end them). An open-addressed table that doubles at half full: with
// classic passes on, every pipeline is made against a VkRenderPass, so a pass that fell back to dynamic rendering for want of a framebuffer would draw with pipelines
// it is not compatible with
typedef struct { fbkey k; VkFramebuffer fb; uint64_t h; int used; } fbent;
static fbent *g_fbc; static uint32_t g_fbcap, g_fbn;
static uint64_t fb_hash(const fbkey *k) { uint64_t h = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)k; for (size_t i = 0; i < sizeof *k; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
static int fb_grow(void) {
    uint32_t nc = g_fbcap ? 2 * g_fbcap : 1024; fbent *n = calloc(nc, sizeof *n);
    if (!n) return 0;
    for (uint32_t i = 0; i < g_fbcap; i++) if (g_fbc[i].used) { uint32_t j = (uint32_t)g_fbc[i].h & (nc - 1); while (n[j].used) j = (j + 1) & (nc - 1); n[j] = g_fbc[i]; }
    free(g_fbc); g_fbc = n; g_fbcap = nc;
    if (nc > 1024) VLOG("classic render passes: framebuffer table %u entries", nc);
    return 1;
}
static VkFramebuffer fb_obtain(const fbkey *k) {   // (the recording thread only)
    if (2 * (g_fbn + 1) > g_fbcap && !fb_grow()) { VRED_ONCE("framebuffer table: out of memory"); return VK_NULL_HANDLE; }
    uint64_t h = fb_hash(k); uint32_t i = (uint32_t)h & (g_fbcap - 1);
    for (; g_fbc[i].used; i = (i + 1) & (g_fbcap - 1)) if (g_fbc[i].h == h && !memcmp(&g_fbc[i].k, k, sizeof *k)) return g_fbc[i].fb;
    VkFramebufferAttachmentImageInfo ai[9];
    for (uint32_t a = 0; a < k->na; a++) ai[a] = (VkFramebufferAttachmentImageInfo){VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, NULL, k->a[a].fl, k->a[a].u, k->a[a].w, k->a[a].h, k->a[a].nl, k->a[a].nvf, k->a[a].vf};
    VkFramebufferAttachmentsCreateInfo fa = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, NULL, k->na, ai};
    VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .pNext = &fa, .flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, .renderPass = k->rp, .attachmentCount = k->na,
                                  .width = k->w, .height = k->h, .layers = k->layers};
    VkFramebuffer fb = VK_NULL_HANDLE; VkResult e = vk.vkCreateFramebuffer(V.dev, &ci, NULL, &fb);
    if (e) { VRED_ONCE("vkCreateFramebuffer %d", e); return VK_NULL_HANDLE; }
    g_fbc[i].k = *k; g_fbc[i].h = h; g_fbc[i].fb = fb; g_fbc[i].used = 1; g_fbn++;
    return fb;
}
static fbatt fb_att(const vka_img *im, uint32_t lv, uint32_t nl, VkFormat view) {
    fbatt a; memset(&a, 0, sizeof a);
    a.fl = im->cflags; a.u = im->usage; if (view != im->f.vk && (a.u & VK_IMAGE_USAGE_STORAGE_BIT)) a.u &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT;   // (as vka_view restricts an sRGB view)
    a.w = im->w >> lv ? im->w >> lv : 1; a.h = im->h >> lv ? im->h >> lv : 1; a.nl = nl;
    if (im->nvf) { a.nvf = im->nvf; memcpy(a.vf, im->vf, sizeof a.vf); } else { a.nvf = 1; a.vf[0] = view; }
    return a;
}
static void bcrt_flush(void);   // block-compressed render targets (below)
// DM_VK_PARTLOG: the depth state of the draws into a depth target in the part in progress, logged with the part's end (vka_rp_zstats); DM_VK_ZLOG=1 also logs each draw of a
// depth pass of at least DM_VK_ZLOG_MIN (default 4096) pixels a side and each such pass's begin and end (up to DM_VK_ZLOG_LINES lines, default 30000). For the Turnip LRZ hang
// (docs/reference/rendering.md "Turnip hangs in the first 3D frame of a cold run"): Turnip's LRZ is invalidated for the rest of a pass by a depth compare direction change
// and by depth writes with ALWAYS / NOT_EQUAL, and is affected by clears inside the pass, stencil, depth bias and discard
static struct { uint32_t nd, nt, nw, op[8], flips, naw, ns, nb, nrd, ncl, npb, nlc, nbk, nts; } g_zs;
static int g_zdir, g_zlog_pass; static uint32_t g_zlog_n, g_zpart_d;
static int zlog_room(void) { static int lim = -1; if (lim < 0) lim = VKA_ENV("DM_VK_ZLOG_LINES", 30000); return g_zlog_n < (uint32_t)lim && (g_zlog_n++, 1); }
void vka_rp_zstats(char *b, size_t n) {
    size_t k = (size_t)snprintf(b, n, "; depth draws %u: test %u write %u ops", g_zs.nd, g_zs.nt, g_zs.nw);
    for (int i = 0; i < 8 && k < n; i++) if (g_zs.op[i]) k += (size_t)snprintf(b + k, n - k, " %d:%u", i, g_zs.op[i]);
    if (k < n) snprintf(b + k, n - k, ", direction flips %u, writes with ALWAYS/NOT_EQUAL %u, stencil %u, bias %u, discard %u, clears in a pass %u, passes begun %u (%u with a depth clear), game barriers kept %u, timestamps in a pass %u",
                        g_zs.flips, g_zs.naw, g_zs.ns, g_zs.nb, g_zs.nrd, g_zs.ncl, g_zs.npb, g_zs.nlc, g_zs.nbk, g_zs.nts);
    memset(&g_zs, 0, sizeof g_zs); g_zpart_d = 0;
}
void vka_rp_note_ts(void) { if (R.on && R.df) g_zs.nts++; if (g_zlog_pass && zlog_room()) VLOG("zlog timestamp inside pass %llu", (unsigned long long)vka_n_rp); }
// Game barriers (OP_BARRIER) that came with a pass open: taken inside it (DM_VK_BAR_KEEP), ended it at once, ended it later at a draw that needed it (bar_hazard)
static uint64_t g_bar_in, g_bar_kept, g_bar_late;
int vka_rp_on(void) { return R.on; }
void vka_rp_end(void) {
    if (PC.n) pc_flush();
    if (!R.on) return;
    if (g_zlog_pass && zlog_room()) VLOG("zlog pass %llu end", (unsigned long long)vka_n_rp);
    g_zlog_pass = 0;
    if (R.classic) vk.vkCmdEndRenderPass(g_cb); else vk.vkCmdEndRendering(g_cb);
    vka_barrier(g_cb);   // the pass's writes before whatever reads them next (copies, the present, the next pass's samples)
    vka_xp_ts(g_cb, 1);
    bcrt_flush();        // block-compressed targets drawn through shadows: their blocks back (bcrt_attach)
    R.on = 0; vka_tex_feedback(NULL, NULL, 0);
    if (vka_nan_on()) vka_nan_pass(g_cb, R.cim, R.clv, R.cl0, R.nci);
}
static void rt_peek(const char *what, int i, const tex_t *t, const vka_img *im, uint32_t lv, uint32_t l0, uint32_t nl);   // DM_VK_TEX_PEEK
static void rt_range(const vka_img *im, const view_t *v, int has_v, uint32_t *lv, uint32_t *l0, uint32_t *nl);
// Block-compressed textures as render targets. A GPU block encoder (agl's BC4 encoder; Ocarina of Time fills a 64x64x64 BC4 volume one depth plane a pass) draws into
// a BC texture through a view in an uncompressed format of the block's size (RGBA16UI for BC4's 8-byte blocks), one texel per 4x4 block: on the Switch the view's texels are
// the texture's blocks. Vulkan attaches no compressed image, so such a pass draws into an uncompressed shadow of blocks x blocks in the view's format (a texture of the
// replay's own, one per format, size and layer count), filled from the texture's level (or its layers, or a 3D texture's depth planes) by a copy before the pass and copied
// back after it (vkCmdCopyImage between size-compatible formats: a texel of one is a block of the other). Only levels whose size is a whole number of blocks; others are
// not drawn into, as before, logged. DM_VK_BC_RT=0: none (the earlier behaviour)
static int bc_fmt(const vka_fmt *f) { return f->bw > 1 || f->bh > 1; }   // (vka_tex.c compressed)
static struct { int fmt; uint32_t w, h, nl; tex_t t; } g_bcsh[16]; static int g_nbcsh;   // (the recording thread only)
static vka_img *bcrt_shadow(int fmt, uint32_t w, uint32_t h, uint32_t nl) {
    int k = 0; while (k < g_nbcsh && !(g_bcsh[k].fmt == fmt && g_bcsh[k].w == w && g_bcsh[k].h == h && g_bcsh[k].nl == nl)) k++;
    if (k == g_nbcsh) {
        if (k == 16) { VRED_ONCE("block-compressed render targets: more than 16 shadow shapes: the rest are not drawn into"); return NULL; }
        tex_t *t = &g_bcsh[k].t; memset(t, 0, sizeof *t);
        t->d.target = nl > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D; t->d.format = (uint32_t)fmt; t->d.width = w; t->d.height = h; t->d.depth = nl; t->d.levels = 1;
        nvn_tex_layout(&t->d, &t->L); t->serial = nvn_new_serial();
        g_bcsh[k].fmt = fmt; g_bcsh[k].w = w; g_bcsh[k].h = h; g_bcsh[k].nl = nl; g_nbcsh++;
        VLOG("block-compressed render target: a shadow of %ux%u x%u in NVN format %#x (serial %llu)", w, h, nl, (unsigned)fmt, (unsigned long long)t->serial);
    }
    return vka_tex_get(&g_bcsh[k].t, g_cb);   // (looked up each time: the image may have been let go and made again)
}
static VkImageCopy bcrt_region(const vka_img *im, uint32_t lv, uint32_t l0, uint32_t nl, uint32_t bw, uint32_t bh, int back) {
    int is3d = im->d.target == NVN_TEXTURE_TARGET_3D;
    VkImageSubresourceLayers bc = {VK_IMAGE_ASPECT_COLOR_BIT, lv, is3d ? 0 : l0, is3d ? 1 : nl}, sh = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, nl};
    VkOffset3D bo = {0, 0, is3d ? (int32_t)l0 : 0}, so = {0, 0, 0};
    // The extent is in the source's texels: the compressed level's size copying in, the shadow's blocks copying back; a 3D texture's depth planes are the shadow's layers
    VkExtent3D ex = back ? (VkExtent3D){bw, bh, is3d ? nl : 1} : (VkExtent3D){bw * 4, bh * 4, is3d ? nl : 1};
    return back ? (VkImageCopy){sh, so, bc, bo, ex} : (VkImageCopy){bc, bo, sh, so, ex};
}
// The shadow's view for colour target i of a pass, or VK_NULL_HANDLE (not this case, or it cannot: logged); *f the view's format, *shp the shadow
static VkImageView bcrt_attach(const p_set_rt *rt, int i, vka_img *im, const vka_fmt *f, uint32_t *w, uint32_t *h, uint32_t *nlo, vka_img **shp) {
    if (!VKA_ENV("DM_VK_BC_RT", 1) || !(rt->has_cview[i] && rt->cview[i].set_format) || !bc_fmt(&im->f) || f->kind >= 3 || bc_fmt(f) || f->bpb != im->f.bpb) return VK_NULL_HANDLE;
    uint32_t lv, l0, nl; rt_range(im, &rt->cview[i], rt->has_cview[i], &lv, &l0, &nl);
    if (lv >= im->levels || !nl || R.nbc == 8) return VK_NULL_HANDLE;
    uint32_t mw = im->w >> lv ? im->w >> lv : 1, mh = im->h >> lv ? im->h >> lv : 1;
    if (mw % im->f.bw || mh % im->f.bh) { VRED_ONCE("block-compressed render target %ux%u (level %u) is not a whole number of blocks: not drawn into", mw, mh, lv); return VK_NULL_HANDLE; }
    uint32_t bw = mw / im->f.bw, bh = mh / im->f.bh;
    vka_img *sh = bcrt_shadow(rt->cview[i].format, bw, bh, nl);
    if (!sh || !(sh->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) { VRED_ONCE("block-compressed render target: no shadow image of NVN format %#x: not drawn into", rt->cview[i].format); return VK_NULL_HANDLE; }
    vka_tex_own(im, g_cb); vka_tex_own(sh, g_cb);
    VkImageView v = vka_view(sh, f->view, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, nl, NULL);
    if (!v) return VK_NULL_HANDLE;
    VkImageCopy rc = bcrt_region(im, lv, l0, nl, bw, bh, 0);   // what the texture holds, for a pass that loads it or covers only part of it
    vka_barrier(g_cb); vk.vkCmdCopyImage(g_cb, im->img, VK_IMAGE_LAYOUT_GENERAL, sh->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rc); vka_barrier(g_cb);
    R.bc[R.nbc].im = im; R.bc[R.nbc].sh = sh; R.bc[R.nbc].lv = lv; R.bc[R.nbc].l0 = l0; R.bc[R.nbc].nl = nl; R.bc[R.nbc].bw = bw; R.bc[R.nbc++].bh = bh;
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VLOG("block-compressed render target: texture serial %llu (NVN format %#x, %ux%u level %u, layers or planes %u+%u) drawn through a %ux%u shadow in NVN format %#x",
        (unsigned long long)im->serial, im->d.format, mw, mh, lv, l0, nl, bw, bh, rt->cview[i].format);
    *w = bw; *h = bh; *nlo = nl; *shp = sh;
    return v;
}
static void bcrt_flush(void) {   // the shadows' blocks into their textures (after the pass's barrier)
    if (!R.nbc) return;
    for (int k = 0; k < R.nbc; k++) {
        VkImageCopy rc = bcrt_region(R.bc[k].im, R.bc[k].lv, R.bc[k].l0, R.bc[k].nl, R.bc[k].bw, R.bc[k].bh, 1);
        vk.vkCmdCopyImage(g_cb, R.bc[k].sh->img, VK_IMAGE_LAYOUT_GENERAL, R.bc[k].im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
        vka_img_written(R.bc[k].im);
    }
    R.nbc = 0; vka_barrier(g_cb);
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
        if (!(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || f.kind >= 3) {
            uint32_t bw, bh, bnl; vka_img *sh = NULL; VkImageView bv = f.kind < 3 ? bcrt_attach(rt, i, im, &f, &bw, &bh, &bnl, &sh) : VK_NULL_HANDLE;
            if (bv) {   // a block-compressed texture drawn into through an uncompressed view: its shadow (bcrt_attach)
                ca[i].imageView = bv; R.cf[i] = f.view; R.ckind[i] = f.kind; samples = 1; fb_im[nfb] = sh; fb_lv[nfb++] = 0; s_im[i] = sh; s_lv[i] = 0; s_nl[i] = bnl;
                R.cnl[i] = bnl; R.cext[i] = (VkExtent2D){bw, bh};
                if (bw < w) w = bw; if (bh < h) h = bh; if (bnl < layers) layers = bnl;
                rt_peek("compressed colour", i, t, im, R.bc[R.nbc - 1].lv, R.bc[R.nbc - 1].l0, bnl);
                continue;
            }
            VRED_ONCE("colour target %d (format %#x) cannot be a Vulkan colour attachment: not drawn into", i, t->d.format); rt_peek("refused colour", i, t, im, 0, 0, 0); continue; }
        if (im->depth > 1 && !(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;   // its slices are drawn into through 2D-array views (img_create)
        uint32_t lv, l0, nl; rt_range(im, &rt->cview[i], rt->has_cview[i], &lv, &l0, &nl);
        if (lv >= im->levels || !nl) continue;
        if (samples && im->samples != samples) { VRED_ONCE("colour targets of different sample counts"); continue; }
        vka_tex_own(im, g_cb);   // (a zero-shared image gets memory of its own before it is drawn into: no pass is open here)
        VkImageView v = vka_view(im, f.view, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, l0, nl, NULL);
        if (!v) continue;
        ca[i].imageView = v; R.cf[i] = f.view; R.ckind[i] = f.kind; samples = im->samples; fb_im[nfb] = im; fb_lv[nfb++] = lv; s_im[i] = im; s_lv[i] = lv; s_nl[i] = nl;
        R.cim[R.nci] = im; R.clv[R.nci] = lv; R.cl0[R.nci++] = l0;
        rt_peek("colour", i, t, im, lv, l0, nl);
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
                rt_peek("depth", 0, rt->depth, im, lv, l0, nl);
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
        VkFramebuffer fb = rp ? fb_obtain(&fk) : VK_NULL_HANDLE;   // (none only when the driver refused one: the pass falls back to dynamic rendering, logged red)
        if (fb) {
            VkRenderPassAttachmentBeginInfo ab = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, NULL, na, views};
            VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .pNext = na ? &ab : NULL, .renderPass = rp, .framebuffer = fb, .renderArea = R.area, .clearValueCount = na, .pClearValues = cv};
            vk.vkCmdBeginRenderPass(g_cb, &rb, VK_SUBPASS_CONTENTS_INLINE); R.classic = 1;
        }
    }
    if (dp) {   // DM_VK_XP_TS bit 4, narrowed by DM_VK_XP_TS_SEL (experiment): 1 only when the depth image differs from the last depth pass's, 2 only with a depth clear
                // at the begin, 4 only after a depth pass of at least 8192 pixels a side (each set bit must hold)
        static const vka_img *last; static uint32_t lastw; int sel = VKA_ENV("DM_VK_XP_TS_SEL", 0); const vka_img *cur = s_im[8];
        if ((!(sel & 1) || cur != last) && (!(sel & 2) || dp->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) && (!(sel & 4) || lastw >= 8192)) vka_xp_ts(g_cb, 4);
        last = cur; lastw = w;
    }
    if (!R.classic) vk.vkCmdBeginRendering(g_cb, &ri);
    R.on = 1; vka_n_rp++; if (g_dyn_filter == 2) vka_dyn_invalidate();   // DM_VK_DYN_FILTER=2: the dynamic state set again at each pass (bisect)
    g_zdir = 0;
    if (dp) { g_zs.npb++; if (dp->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) g_zs.nlc++; for (int k = 0; k < nlate; k++) if (late[k]->asp & VK_IMAGE_ASPECT_DEPTH_BIT) g_zs.ncl++; }
    { static int zl = -1, zmin; if (zl < 0) { zl = VKA_ENV("DM_VK_ZLOG", 0); zmin = VKA_ENV("DM_VK_ZLOG_MIN", 4096); }
      g_zlog_pass = zl && dp && w >= (uint32_t)zmin && h >= (uint32_t)zmin;
      if (g_zlog_pass && zlog_room()) VLOG("zlog pass %llu begin %ux%ux%u depth serial %llu VkFormat %d %s, %d late clears, %d colour targets, stencil %d, %s", (unsigned long long)vka_n_rp, w, h, layers,
                                           (unsigned long long)rt->depth->serial, R.df, dp->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "CLEAR" : "LOAD", nlate, n, sp != NULL, R.classic ? "render pass" : "dynamic rendering"); }
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
    if (R.swrite || !VKA_ENV("DM_VK_BAR_KEEP", 1)) return 0;
    R.bar_pending = 1; g_bar_kept++;
    if (R.df) g_zs.nbk++;
    if (g_zlog_pass && zlog_room()) VLOG("zlog game barrier %#x kept inside pass %llu", bits, (unsigned long long)vka_n_rp);
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
// The open pass has a depth or stencil attachment; *w, *h: its render area (vka_exec.c chunk_check: a pass ended to split a submission is begun again with LOAD)
int vka_rp_depth(uint32_t *w, uint32_t *h) { if (w) *w = R.area.extent.width; if (h) *h = R.area.extent.height; return R.on && (R.df || R.sf); }
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
    if (c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) { g_zs.ncl++; if (g_zlog_pass && zlog_room()) VLOG("zlog depth clear inside pass %llu: %.6f at %d,%d %ux%u", (unsigned long long)vka_n_rp, c->in.depthStencil.depth, rc.offset.x, rc.offset.y, rc.extent.width, rc.extent.height); }
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
int vka_cpu_read(uint64_t addr, void *out, size_t n) { return cpu_read(addr, out, n); }
int vka_mesh_on(void) { return V.mesh; }
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
// DM_VK_SB_PEEK=<hex key>[,…] (diagnostic): the storage buffers those shaders may write, as the CPU sees them when the draw or dispatch is recorded and again once its
// submission has completed (vka_draw_peek_commit), to follow sums a shader leaves for the CPU (agl's scene analysis). The first DM_VK_SB_PEEK_LINES (default 200) lines from frame DM_VK_SB_PEEK_FROM (default 0) are logged
typedef struct sb_peek { struct sb_peek *next; uint64_t key, addr, size, frame, fence; int st, binding, flags, n; uint32_t w[64]; } sb_peek;
static sb_peek *g_peek;   // this submission's, handed to the completion at its commit
static _Atomic int g_peek_lines;
static int peek_room(void) { return atomic_fetch_add(&g_peek_lines, 1) < VKA_ENV("DM_VK_SB_PEEK_LINES", 200); }
static void peek_line(const sb_peek *p, const char *when, uint64_t fence) {
    int nz = 0; for (int i = 0; i < p->n; i++) nz += p->w[i] != 0;
    int at = VKA_ENV("DM_VK_SB_PEEK_AT", 0); if (at < 0 || at + 8 > p->n) at = 0;   // the first of the 8 words shown
    const uint32_t *w = p->w + at;
    float f[8] = {0}; memcpy(f, w, sizeof f);
    VLOG("sb peek %s: frame %llu shader %016llx stage %d binding %d flags %#x addr %#llx size %#llx fence %llu: %d of %d words non-zero; words %d-%d %08x %08x %08x %08x %08x %08x %08x %08x (%g %g %g %g %g %g %g %g)", when,
         (unsigned long long)p->frame, (unsigned long long)p->key, p->st, p->binding, p->flags, (unsigned long long)p->addr, (unsigned long long)p->size, (unsigned long long)fence, nz, p->n, at, at + 7,
         w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}
static void peek_done(void *arg) {
    for (sb_peek *p = arg, *nx; p; p = nx) {
        nx = p->next;
        if (cpu_read(p->addr, p->w, (size_t)p->n * 4) && peek_room()) peek_line(p, "done", p->fence);
        free(p);
    }
}
void vka_draw_peek_commit(uint64_t fence) { sb_peek *p = g_peek; g_peek = NULL; for (sb_peek *q = p; q; q = q->next) q->fence = fence; if (p) vka_after(fence, peek_done, p); }
// DM_VK_RANGE_WATCH=<hex addr>[+<hex bytes>] (diagnostic, default 16 bytes, at most 64): who writes those guest bytes, whatever the path (a storage binding, a store through a
// loaded address, a copy, a fill, inline data). In DM_VK_RANGE_WATCH_FRAMES (default 3) frames from DM_VK_RANGE_WATCH_FROM, the GPU copies the range into a slot of its own
// before the submission's first dispatch or transfer (vka_rw_pre) and after each one (vka_rw_point); once the submission is done, every step that changed the bytes is logged with its
// shader key or transfer (`range watch`), then the bytes the CPU reads. Draws are not separated: what a draw writes shows at the next step or the submission's end ('e')
enum { RW_SLOTS = 192, RW_SUBS = 8 };
typedef struct { vka_span s; uint64_t frame, fence; int n, len, busy; uint8_t rec[64]; struct { uint64_t key, a, b; char kind; } ev[RW_SLOTS]; } rw_sub;
static rw_sub g_rws[RW_SUBS], *g_rwc; static _Atomic int g_rw_lines;
static int rw_range(uint64_t *addr, int *len) {
    static uint64_t a; static int n = -1;
    if (n < 0) { const char *q = getenv("DM_VK_RANGE_WATCH"); char *e = NULL; a = q ? strtoull(q, &e, 16) : 0; n = 0;
        if (a) { n = e && *e == '+' ? (int)strtol(e + 1, NULL, 16) : 16; if (n <= 0) n = 16; if (n > 64) n = 64; n = (n + 3) & ~3; } }
    *addr = a; *len = n; return n > 0;
}
static void rw_copy(VkCommandBuffer cb, rw_sub *w, uint64_t addr) {
    VkBuffer b; VkDeviceSize o; if (!vka_addr(addr, (uint64_t)w->len, &b, &o, NULL)) return;
    VkBufferCopy c = {o, w->s.off + (VkDeviceSize)w->n * 64, (VkDeviceSize)w->len}; vk.vkCmdCopyBuffer(cb, b, w->s.buf, 1, &c); vka_barrier(cb);
}
static int rw_on(VkCommandBuffer cb, uint64_t *addr, int *len) {
    uint64_t vka_frame(void);
    if (!cb || !rw_range(addr, len)) return 0;
    uint64_t f = vka_frame(), from = (uint64_t)VKA_ENV("DM_VK_RANGE_WATCH_FROM", 0);
    return f >= from && f < from + (uint64_t)VKA_ENV("DM_VK_RANGE_WATCH_FRAMES", 3);
}
// Before a dispatch or transfer: the submission's first one takes a slot set and copies the bytes before it (slot 0)
void vka_rw_pre(VkCommandBuffer cb) {
    uint64_t addr, vka_frame(void); int len; if (g_rwc || !rw_on(cb, &addr, &len)) return;
    for (int i = 0; i < RW_SUBS && !g_rwc; i++) if (!__atomic_load_n(&g_rws[i].busy, __ATOMIC_ACQUIRE)) g_rwc = &g_rws[i];
    if (!g_rwc) return;   // all in flight: this submission is not followed
    if (!g_rwc->s.ptr && !vka_static((VkDeviceSize)RW_SLOTS * 64, 64, &g_rwc->s)) { g_rwc = NULL; return; }
    g_rwc->busy = 1; g_rwc->n = 0; g_rwc->len = len; g_rwc->frame = vka_frame(); cpu_read(addr, g_rwc->rec, (size_t)len);
    g_rwc->ev[0].kind = 's'; vka_barrier(cb); rw_copy(cb, g_rwc, addr); g_rwc->n = 1;
}
// After a dispatch (kind 'd', key: its shader) or a transfer ('c' copy a -> b, 'f' fill of b, 'u' inline data into b, 't' a texture's bytes written into b) that the replay recorded into cb
void vka_rw_point(VkCommandBuffer cb, char kind, uint64_t key, uint64_t a, uint64_t b) {
    uint64_t addr; int len; if (!g_rwc || !rw_on(cb, &addr, &len) || g_rwc->n >= RW_SLOTS) return;
    g_rwc->ev[g_rwc->n].kind = kind; g_rwc->ev[g_rwc->n].key = key; g_rwc->ev[g_rwc->n].a = a; g_rwc->ev[g_rwc->n].b = b;
    vka_barrier(cb); rw_copy(cb, g_rwc, addr); g_rwc->n++;
}
static void rw_hex(const uint8_t *p, int n, char *o) { for (int i = 0; i < n; i += 4) { uint32_t v; memcpy(&v, p + i, 4); o += sprintf(o, "%s%08x", i ? " " : "", v); } }
static void rw_done(void *arg) {   // a submission whose bytes no step changed, which the CPU reads as the GPU left them and as it read them when recorded, is not logged
    rw_sub *w = arg; uint64_t addr; int len; rw_range(&addr, &len);
    char h[64 * 9 + 8], h2[64 * 9 + 8], fl[160]; uint8_t now[64];
    int any = 0; for (int i = 1; i < w->n && !any; i++) any = memcmp(w->s.ptr + (size_t)i * 64, w->s.ptr + (size_t)(i - 1) * 64, (size_t)w->len) != 0;
    int rd = cpu_read(addr, now, (size_t)w->len), last = w->n - 1;
    if (!any && rd && !memcmp(now, w->s.ptr + (size_t)last * 64, (size_t)w->len) && !memcmp(now, w->rec, (size_t)w->len)) { __atomic_store_n(&w->busy, 0, __ATOMIC_RELEASE); return; }
    if (atomic_fetch_add(&g_rw_lines, 1) < VKA_ENV("DM_VK_RANGE_WATCH_LINES", 400)) { rw_hex(w->rec, w->len, h); rw_hex(w->s.ptr, w->len, h2);
        VLOG("range watch: frame %llu fence %llu: %d steps; at recording the CPU read %s, the GPU before the first step %s", (unsigned long long)w->frame, (unsigned long long)w->fence, w->n - 1, h, h2); }
    for (int i = 1; i < w->n; i++) {
        const uint8_t *p = w->s.ptr + (size_t)i * 64, *q = p - 64;
        if (!memcmp(p, q, (size_t)w->len) || atomic_fetch_add(&g_rw_lines, 1) >= VKA_ENV("DM_VK_RANGE_WATCH_LINES", 400)) continue;
        rw_hex(p, w->len, h); fl[0] = 0; for (int j = 0; j < w->len / 4 && j < 8; j++) { float x; memcpy(&x, p + 4 * j, 4); snprintf(fl + strlen(fl), sizeof fl - strlen(fl), "%s%g", j ? " " : "", x); }
        if (w->ev[i].kind == 'd') VLOG("range watch: frame %llu step %d dispatch of %016llx wrote %s (%s)", (unsigned long long)w->frame, i, (unsigned long long)w->ev[i].key, h, fl);
        else if (w->ev[i].kind == 'e') VLOG("range watch: frame %llu step %d the submission's end (draws since the last step) wrote %s (%s)", (unsigned long long)w->frame, i, h, fl);
        else VLOG("range watch: frame %llu step %d %s %#llx -> %#llx wrote %s (%s)", (unsigned long long)w->frame, i, w->ev[i].kind == 'c' ? "copy" : w->ev[i].kind == 'f' ? "fill" : w->ev[i].kind == 'u' ? "inline data" : "texture to buffer",
                  (unsigned long long)w->ev[i].a, (unsigned long long)w->ev[i].b, h, fl);
    }
    if (rd && atomic_fetch_add(&g_rw_lines, 1) < VKA_ENV("DM_VK_RANGE_WATCH_LINES", 400)) { rw_hex(now, w->len, h);
        VLOG("range watch: frame %llu fence %llu done: the CPU reads %s", (unsigned long long)w->frame, (unsigned long long)w->fence, h); }
    __atomic_store_n(&w->busy, 0, __ATOMIC_RELEASE);
}
void vka_rw_commit(uint64_t fence) { rw_sub *w = g_rwc; g_rwc = NULL; if (w) { w->fence = fence; vka_after(fence, rw_done, w); } }
// DM_VK_SB_WATCH=1 with DM_VK_SB_PEEK (diagnostic): the storage buffers the peeked shaders may write are watched, and every other shader's uniform or storage buffer binding
// over one of them is logged once (shader, stage, binding, uniform or storage, its range and whether a uniform buffer is bound in place or as a copy of the CPU's bytes), to
// find who consumes what the peeked shaders leave (agl's scene analysis). Reads through raw global memory or loaded addresses are not seen
static struct { uint64_t key, lo, hi; } g_sbw[64]; static _Atomic int g_nsbw;
static void sb_watch_add(uint64_t key, const vka_res *r) {
    if (!VKA_ENV("DM_VK_SB_WATCH", 0) || !(r->flags & DMSH_VK_BF_RW)) return;
    uint64_t lo = r->addr, hi = r->addr + (r->size ? r->size : 256);
    int n = atomic_load(&g_nsbw); for (int i = 0; i < n; i++) if (g_sbw[i].lo == lo && g_sbw[i].hi == hi) return;
    if (n >= 64) return;
    g_sbw[n].key = key; g_sbw[n].lo = lo; g_sbw[n].hi = hi; atomic_store(&g_nsbw, n + 1);   // (the recording thread only)
    VLOG("sb watch: shader %016llx binding %u writes %#llx-%#llx: watched", (unsigned long long)key, r->binding, (unsigned long long)lo, (unsigned long long)hi);
}
static int tex_peek_new(uint64_t h);
static void sb_watch_use(uint64_t key, int st, const vka_res *r, int storage, const VkDescriptorBufferInfo *bi) {
    int n = atomic_load(&g_nsbw); if (!n) return;
    uint64_t lo = r->addr, hi = r->addr + (r->size ? r->size : 65536);
    for (int i = 0; i < n; i++) {
        if (g_sbw[i].key == key || lo >= g_sbw[i].hi || hi <= g_sbw[i].lo) continue;
        if (!tex_peek_new(~((key * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)r->binding << 48) ^ ((uint64_t)i << 32) ^ ((uint64_t)st << 40) ^ (uint64_t)storage))) continue;
        VkBuffer b; VkDeviceSize o; int inplace = 0;
        if (!storage && bi) inplace = vka_addr_run(r->addr, 1, &b, &o) && bi->buffer == b;
        uint64_t vka_frame(void);
        VLOG("sb watch: frame %llu shader %016llx stage %d binding %u %s %#llx size %#llx flags %#x reads what %016llx writes at %#llx-%#llx", (unsigned long long)vka_frame(), (unsigned long long)key, st, r->binding,
             storage ? "storage" : inplace ? "uniform (in place)" : "uniform (a copy of the CPU's bytes)", (unsigned long long)r->addr, (unsigned long long)r->size, r->flags,
             (unsigned long long)g_sbw[i].key, (unsigned long long)g_sbw[i].lo, (unsigned long long)g_sbw[i].hi);
    }
}
// DM_VK_GMEM_REACH=<MiB> (diagnostic, with DM_VK_SB_PEEK): each 64-bit word of a peeked buffer's first 32 words that lies in a placed segment is taken as a pointer the
// shader loads (raw global memory, the GMEM_MAP table's search), and that pointer and the given MiB after it (a 64 KiB step) are classified as the table finds them: in no
// segment, by the hint of its 2 MiB chunk, by a search of the whole table, or also by a 6-step search (entries 0..63 only: libraries built before e5c087f6, whose
// simplify_helpers cut the search, docs/reference/shaders.md "Global memory at an address loaded from memory"). Logged as `gmem reach`
static void gmem_reach(uint64_t key, const sb_peek *p) {
    static int span = -1; if (span < 0) span = VKA_ENV("DM_VK_GMEM_REACH", 0);
    if (span <= 0) return;
    static uint64_t lo[DMSH_GMEM_MAP_ENTRIES], hi[DMSH_GMEM_MAP_ENTRIES], bda[DMSH_GMEM_MAP_ENTRIES];
    static uint32_t slot[DMSH_GMEM_MAP_ENTRIES];   // the hint each slot holds as gmem_map writes it: chunk << 13 | entry
    int k = vka_mem_chunks(lo, hi, bda, DMSH_GMEM_MAP_ENTRIES - 1);
    memset(slot, 0, sizeof slot);
    for (int i = 0; i < k; i++) {
        uint64_t sz = 1ull << DMSH_GMEM_HINT_SHIFT, first = (lo[i] + sz - 1) >> DMSH_GMEM_HINT_SHIFT, last = hi[i] >> DMSH_GMEM_HINT_SHIFT;
        for (uint64_t c = first; c < last && c < (1ull << (32 - 13)); c++) slot[c % DMSH_GMEM_MAP_ENTRIES] = (uint32_t)(c << 13 | (unsigned)(i + 1));
    }
    #define SEG(a, out) do { int l_ = 0, h_ = k - 1; out = -1; while (l_ <= h_) { int m_ = (l_ + h_) / 2; if ((a) < lo[m_]) h_ = m_ - 1; else if ((a) >= hi[m_]) l_ = m_ + 1; else { out = m_; break; } } } while (0)
    for (int w = 0; w + 1 < p->n && w < 32; w += 2) {
        uint64_t ptr = (uint64_t)p->w[w] | (uint64_t)p->w[w + 1] << 32; int s0; SEG(ptr, s0);
        if (s0 < 0) continue;
        int none = 0, hint = 0, srch6 = 0, lost = 0, n = 0, maxe = 0;
        for (uint64_t a = ptr; a < ptr + ((uint64_t)span << 20); a += 1u << 16, n++) {
            int s; SEG(a, s);
            if (s < 0) { none++; continue; }
            uint64_t c = a >> DMSH_GMEM_HINT_SHIFT; uint32_t h = slot[c % DMSH_GMEM_MAP_ENTRIES];
            int e = s + 1; if (e > maxe) maxe = e;
            if ((h & 8191) == (uint32_t)e && (h >> 13) == (uint32_t)c && (a >> 32) < 256) hint++;
            else if (e <= 63) srch6++;
            else lost++;
        }
        VLOG("gmem reach: frame %llu shader %016llx binding %d word %d pointer %#llx: table entry %d of %d (segment %#llx-%#llx); its %d MiB in 64 KiB steps: %d in no segment, %d by the hint, %d by a 6-step search, %d only by the whole-table search (entries up to %d)",
             (unsigned long long)p->frame, (unsigned long long)key, p->binding, w, (unsigned long long)ptr, s0 + 1, k, (unsigned long long)lo[s0], (unsigned long long)hi[s0], span, none, hint, srch6, lost, maxe);
    }
    #undef SEG
}
static void sb_peek_note(uint64_t key, int st, const vka_res *r) {
    static uint64_t keys[64]; static int nk = -1;
    if (nk < 0) { nk = 0; for (const char *q = getenv("DM_VK_SB_PEEK"); q && *q && nk < 64;) { char *e; unsigned long long k = strtoull(q, &e, 16); if (e == q) break; keys[nk++] = k; q = *e == ',' ? e + 1 : e; } }
    int hit = 0; for (int i = 0; i < nk && !hit; i++) hit = keys[i] == key;
    if (hit) sb_watch_add(key, r);
    if (!hit || atomic_load(&g_peek_lines) >= VKA_ENV("DM_VK_SB_PEEK_LINES", 200)) return;
    uint64_t vka_frame(void);
    if (vka_frame() < (uint64_t)VKA_ENV("DM_VK_SB_PEEK_FROM", 0)) return;   // not yet: the lines are kept for the frames asked for
    sb_peek *p = calloc(1, sizeof *p); if (!p) return;
    p->key = key; p->addr = r->addr; p->size = r->size; p->frame = vka_frame(); p->st = st; p->binding = (int)r->binding; p->flags = (int)r->flags;
    p->n = r->size && r->size < 256 ? (int)(r->size / 4) : 64;
    if (!cpu_read(p->addr, p->w, (size_t)p->n * 4)) { free(p); return; }
    if (peek_room()) { peek_line(p, "recorded", 0); gmem_reach(key, p); }
    p->next = g_peek; g_peek = p;
}
// DM_VK_UB_PEEK=<hex key>[,…] (diagnostic): the uniform buffers (constant buffers) those shaders' draws and dispatches bind from guest memory, as the CPU sees them when
// recorded: 16 words from word DM_VK_UB_PEEK_AT (default 0) as hex and float, to check a pass's parameters (a tone map's curve, a colour matrix). Shares DM_VK_SB_PEEK_FROM / _LINES
static void ub_peek_note(uint64_t key, int st, const vka_res *r) {
    static uint64_t keys[64]; static int nk = -1;
    if (nk < 0) { nk = 0; for (const char *q = getenv("DM_VK_UB_PEEK"); q && *q && nk < 64;) { char *e; unsigned long long k = strtoull(q, &e, 16); if (e == q) break; keys[nk++] = k; q = *e == ',' ? e + 1 : e; } }
    int hit = 0; for (int i = 0; i < nk && !hit; i++) hit = keys[i] == key;
    uint64_t vka_frame(void);
    if (!hit || vka_frame() < (uint64_t)VKA_ENV("DM_VK_SB_PEEK_FROM", 0) || !peek_room()) return;
    int at = VKA_ENV("DM_VK_UB_PEEK_AT", 0); if (at < 0) at = 0;
    uint32_t w[16]; float f[16];
    if (!cpu_read(r->addr + (uint64_t)at * 4, w, sizeof w)) { VLOG("ub peek: frame %llu shader %016llx stage %d binding %u addr %#llx: not readable", (unsigned long long)vka_frame(), (unsigned long long)key, st, r->binding, (unsigned long long)r->addr); return; }
    memcpy(f, w, sizeof f);
    VLOG("ub peek: frame %llu shader %016llx stage %d binding %u addr %#llx size %#llx words %d-%d %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x"
         " (%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g)", (unsigned long long)vka_frame(), (unsigned long long)key, st, r->binding, (unsigned long long)r->addr, (unsigned long long)r->size, at, at + 15,
         w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15],
         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
}
// DM_VK_TEX_PEEK=<hex key>[,…] or `all` (diagnostic): each texture and image binding of those shaders' draws and dispatches (texture ID, what it stands for, its storage and its
// image, the view made or none), and the render targets of every pass that opens (rt_peek), to follow which textures share an image (a cube probe's faces and the lighting
// that samples it). The first DM_VK_TEX_PEEK_LINES (default 400) lines from frame DM_VK_TEX_PEEK_FROM (default 0)
static _Atomic int g_tpeek_lines;
static int tex_peek_on(uint64_t key) {
    static uint64_t keys[64]; static int nk = -1, all;
    if (nk < 0) { int n = 0; const char *q = getenv("DM_VK_TEX_PEEK");
        if (q && !strcmp(q, "all")) all = 1;
        else for (; q && *q && n < 64;) { char *e; unsigned long long k = strtoull(q, &e, 16); if (e == q) break; keys[n++] = k; q = *e == ',' ? e + 1 : e; }
        nk = n; }
    if (!all && !nk) return 0;
    int hit = all || !key; for (int i = 0; i < nk && !hit; i++) hit = keys[i] == key;
    uint64_t vka_frame(void);
    return hit && vka_frame() >= (uint64_t)VKA_ENV("DM_VK_TEX_PEEK_FROM", 0) && atomic_load(&g_tpeek_lines) < VKA_ENV("DM_VK_TEX_PEEK_LINES", 400);
}
static int tex_peek_room(void) { return atomic_fetch_add(&g_tpeek_lines, 1) < VKA_ENV("DM_VK_TEX_PEEK_LINES", 400); }
static int tex_peek_new(uint64_t h) {   // a line not logged yet (each binding, texture ID and outcome, each target and range: once a run)
    static _Atomic uint64_t seen[8192]; h |= 1;
    for (uint32_t i = (uint32_t)(h ^ h >> 29) & 8191, n = 0; n < 8192; i = (i + 1) & 8191, n++) {
        uint64_t o = 0; if (atomic_compare_exchange_strong(&seen[i], &o, h)) return 1;
        if (o == h) return 0;
    }
    return 0;
}
static void tex_peek(uint64_t key, int st, const vka_res *r, VkImageView v, int storage) {
    if (!tex_peek_on(key) || !tex_peek_new((key * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)r->binding << 48) ^ ((uint64_t)(uint32_t)r->tid << 8) ^ (uint64_t)(v != 0) ^ ((uint64_t)st << 40)) || !tex_peek_room()) return;
    char why[400]; vka_tex_id_peek(r->tid, why, sizeof why);
    uint64_t vka_frame(void);
    VLOG("tex peek: frame %llu shader %016llx stage %d binding %u shape %#x%s texture ID %d sampler ID %d view %s: %s", (unsigned long long)vka_frame(), (unsigned long long)key, st, r->binding, r->dim,
         storage ? " (storage)" : "", r->tid, r->sid, v ? "made" : "none", why);
}
static void rt_peek(const char *what, int i, const tex_t *t, const vka_img *im, uint32_t lv, uint32_t l0, uint32_t nl) {
    if (!tex_peek_on(0) || !tex_peek_new((t->serial * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)lv << 56) ^ ((uint64_t)l0 << 40) ^ ((uint64_t)nl << 24) ^ (uint64_t)(uint8_t)what[0]) || !tex_peek_room()) return;
    uint64_t vka_frame(void);
    VLOG("rt peek: frame %llu %s %d: serial %llu target %d format %#x %ux%ux%u levels %u mem %p; drawn level %u layers %u+%u; image serial %llu mem %p %ux%u layers %u levels %u", (unsigned long long)vka_frame(), what, i,
         (unsigned long long)t->serial, t->d.target, t->d.format, t->d.width, t->d.height, t->d.depth, t->d.levels, (void *)t->mem, lv, l0, nl,
         im ? (unsigned long long)im->serial : 0ULL, im ? (const void *)im->mem : NULL, im ? im->w : 0, im ? im->h : 0, im ? im->layers : 0, im ? im->levels : 0);
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
static struct { int valid; vka_span s; uint64_t k[3]; uint8_t b[DMSH_DRV_SIZE]; } g_dm[VKA_NSTAGE][3];   // k: what the bytes were filled from (drv_key)
static struct { int valid; uint64_t gen; vka_span s; } g_gm;
static struct { int valid; uint32_t ref; vka_span s; } g_sm_ref;   // the support buffer with the alpha test's reference (stage_writes)
static int g_drv_memo = 1, g_drv_stamp = 1; static uint64_t g_dm_hit, g_dm_miss, g_dm_stamp;
static void drv_memo_reset(void) { for (int st = 0; st < VKA_NSTAGE; st++) for (int b = 0; b < 3; b++) g_dm[st][b].valid = 0; g_gm.valid = 0; g_sm_ref.valid = 0; g_drv_memo = VKA_ENV("DM_VK_DRV_MEMO", 1); g_drv_stamp = VKA_ENV("DM_VK_DRV_STAMP", 1); sets_memo_drop(); }   // (and the sets' memo: the spans g_w holds go with the submission's fence)
// What a graphics stage's segment is filled from (vka_bind.c vka_drv_fill), as stamps and values: c0 from the stage's storage buffers, the base instance and the sample
// count; c2 from the stage's texture, sampler and image handles (and the pools' bases when the shader reads the pools); the push bank from the pushed constants. The same
// key as the memo's: the same bytes, so the draw binds the memo's span without filling the segment and comparing it (DM_VK_DRV_STAMP, live from the next submission,
// default 1; 0: filled and compared every draw, as before). A c2 fill and compare is ~0.3 us a stage on the Mac (its 2.5 KB, 300 handle words)
static void drv_key(const vka_dstate *d, int st, int bi_, int pool, uint64_t k[3]) {
    if (bi_ == 0) { k[0] = d->s_ssbo[st]; k[1] = d->b.base_instance; k[2] = d->b.samples; }
    else if (bi_ == 1) { k[0] = d->s_tex[st]; k[1] = pool ? (uint64_t)(uint32_t)d->b.tex_base | (uint64_t)(uint32_t)d->b.samp_base << 32 : 0; k[2] = (uint64_t)(pool != 0); }
    else { k[0] = d->s_push; k[1] = k[2] = 0; }
}
static int drv_seg(const vka_dstate *d, int st, int bank, int pool, VkDescriptorBufferInfo *bi, c0_at *c0) {
    size_t sz = vka_drv_size(bank); vka_span s;
    // One memo slot per segment kind: c0, c2 and the push bank. Any other driver bank (an all-zero c17-style read of DMSH_DRV_SIZE bytes) takes the fill below: sharing
    // the push bank's slot, an all-zero push block matched it and the 0x100-byte span was bound with a 0xA00-byte range (other draws' scratch, or past the buffer)
    int bi_ = bank == 0 ? 0 : bank == 2 ? 1 : bank == DMSH_PUSH_BANK ? 2 : -1;
    if (g_drv_memo && bi_ >= 0 && st != 5 && sz <= DMSH_DRV_SIZE) {
        static uint8_t tmp[DMSH_DRV_SIZE];
        typeof(g_dm[0][0]) *m = &g_dm[st][bi_];
        uint64_t key[3]; drv_key(d, st, bi_, pool, key);
        if (g_drv_stamp && m->valid && !memcmp(m->k, key, sizeof key)) { g_dm_hit++; g_dm_stamp++; *bi = (VkDescriptorBufferInfo){m->s.buf, m->s.off, sz}; return 1; }
        vka_drv_fill(&d->b, st, bank, pool, tmp);
        if (m->valid && !memcmp(m->b, tmp, sz)) { memcpy(m->k, key, sizeof key); g_dm_hit++; *bi = (VkDescriptorBufferInfo){m->s.buf, m->s.off, sz}; return 1; }
        if (!vka_scratch(sz, V.ub_align, &s)) return 0;
        memcpy(s.ptr, tmp, sz); memcpy(m->b, tmp, sz); memcpy(m->k, key, sizeof key); m->s = s; m->valid = 1; g_dm_miss++;
        *bi = (VkDescriptorBufferInfo){s.buf, s.off, sz};
        return 1;
    }
    if (!vka_scratch(sz, V.ub_align, &s)) return 0;
    vka_drv_fill(&d->b, st, bank, pool, s.ptr);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, sz};
    if (bank == 0 && c0) { c0->buf = s.buf; c0->off = s.off; }
    return 1;
}
// Raw global memory's table (dmsh_abi.h DMSH_GMEM_MAP_*): entry 0 empty (its last 8 bytes: the sink, vka_mem_sink), then the placed chunks by address with delta = device address - guest address, unused entries at ~0,
// and the chunks' lookup hints in word 6 (vka_gmem.h)
// (kept while no segment comes or goes: vka_mem_gen, and DM_VK_DRV_MEMO)
uint64_t vka_mem_gen(void);
static int gmem_map(VkDescriptorBufferInfo *bi) {
    size_t n = (size_t)DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY;
    uint64_t gen = vka_mem_gen();
    if (g_drv_memo && g_gm.valid && g_gm.gen == gen) { *bi = (VkDescriptorBufferInfo){g_gm.s.buf, g_gm.s.off, n}; return 1; }
    vka_span s; if (!vka_scratch(n, V.sb_align, &s)) return 0;
    memset(s.ptr, 0xff, n); memset(s.ptr, 0, DMSH_GMEM_MAP_ENTRY);
    { uint64_t sink = vka_mem_sink(); memcpy(s.ptr + 24, &sink, 8); }   // entry 0's last words: where an access in no pool goes (vk_android_spv.py gmem_sink)
    static uint64_t lo[DMSH_GMEM_MAP_ENTRIES], hi[DMSH_GMEM_MAP_ENTRIES], bda[DMSH_GMEM_MAP_ENTRIES];
    int k = vka_mem_chunks(lo, hi, bda, DMSH_GMEM_MAP_ENTRIES - 1);
    for (int i = 0; i < k && i + 1 < DMSH_GMEM_MAP_ENTRIES; i++) { uint64_t e[4] = {lo[i], hi[i], bda[i] - lo[i], 0}; memcpy(s.ptr + (size_t)(i + 1) * DMSH_GMEM_MAP_ENTRY, e, sizeof e); }
    vka_gmem_hints(s.ptr, lo, hi, k);   // the 2 MiB chunks' lookup hints, none at entry 0 (the sink's words: vka_gmem.h)
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
    size_t n = vka_gml_bytes(vka_shader_lib_differs());   // (none free: a cleared one nobody reads, of the playing library's format, vka_gml.h)
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

// Where a draw's CPU time goes: the virtual counter at each phase boundary of vka_draw, in the draws timed (every draw with DM_VK_PROF=1, logged with the replay statistics;
// else one in DM_VK_PACE_PROF for the pace line). Cumulative; each reader keeps its own last reading. g_pt_n: draws timed, g_pt_tot: their whole time
static uint64_t g_pt_last, g_pt[9], g_ps[8], g_pt_n, g_pt_tot; static int g_pt_ph;
static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
// ---------- Set 0 from the walk ----------
typedef struct { VkWriteDescriptorSet w[512]; VkDescriptorBufferInfo bi[512]; VkDescriptorImageInfo ii[512]; VkBufferView bv[512]; uint64_t ad[512]; uint32_t n; } writes_t;   // ad: a guest buffer's address (0 = other)
static writes_t g_w;
// A texture or image read by a shader that declares it in another numeric class (float / uint / sint) than the view the game binds: logged once per shader, stage and binding
// (the first 64), with the texture. Such a binding is read through a view of the shader's class of the same texel size (vka_tex.c class_twin) unless its module is retyped
static void class_note(const vka_shader *sh, int st, const vka_res *r, int tid, int storage) {
    static uint64_t seen[4096]; static int nlog;
    int want = r->dim & DMSH_VK_DIM_SINT ? 2 : r->dim & DMSH_VK_DIM_UINT ? 1 : 0;
    uint64_t h = (sh->key ^ (uint64_t)st << 56 ^ (uint64_t)r->binding << 40 ^ (uint64_t)want << 38) | 1;
    uint32_t s = (uint32_t)(h * 0x9e3779b97f4a7c15ull >> 52);
    for (uint32_t n = 0; n < 4096; n++, s = (s + 1) & 4095) { if (seen[s] == h) return; if (!seen[s]) break; }
    if (nlog >= 64) return;
    seen[s] = h;
    int have = vka_tex_id_class_rec(tid);
    if (have == want) return;
    nlog++;
    static const char *cn[3] = {"float", "uint", "sint"}; char why[320]; vka_tex_id_desc(tid, why, sizeof why);
    VLOG("class: shader %016llx stage %d binding %u declares a %s %s, texture ID %d is %s (%s)", (unsigned long long)sh->key, st, r->binding, cn[want], storage ? "image" : "texture", tid, cn[have], why);
}
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
                if (ok && storage) sb_peek_note(sh->key, st, &r[i]);
                if (ok && !storage) ub_peek_note(sh->key, st, &r[i]);
                if (ok) sb_watch_use(sh->key, st, &r[i], storage, bi);
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
            uint64_t tw_ = 0, tv_ = 0;   // how much of an image descriptor is the ID's lookup into a view (g_ps[6]) and how much everything after it (g_ps[7])
            VkImageView v = VK_NULL_HANDLE; VkSampler s = VK_NULL_HANDLE; VkImageLayout lay = VK_IMAGE_LAYOUT_GENERAL;
            if (r[i].desc != VK_DESCRIPTOR_TYPE_SAMPLER) {
                int tid = r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG ? r[i].tid : 0;
                tw_ = g_prof && g_prof_img && tid ? cntvct() : 0;
                if (tid) v = vka_tex_id_view_rec(tid, g_cb, r[i].dim, storage, &lay);
                if (tw_) { tv_ = cntvct() - tw_; g_ps[6] += tv_; }
                if (g_class_note && tid && v) class_note(sh, st, &r[i], tid, storage);
                if (g_tex_peek_any && tid) tex_peek(sh->key, st, &r[i], v, storage);
                // A 2D texture in a 3D or cube slot: what the game leaves bound where a shader's branch does not sample (DKB's materials keep a 1x1 2D texture in their volume slot 12).
                // It reads as unbound, zeros, as nvn_mtl.m's zero 3D / zero cube fallback does; noted once. Other mismatches are reported
                int dd = r[i].dim & DMSH_VK_DIM_MASK, tt = tid && !v ? vka_tex_id_target_rec(tid) : -1;
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
            w->pImageInfo = ii; if (g_prof) { uint64_t tt_ = cntvct() - ti_; g_ps[4] += tt_; g_ps[5]++; if (tw_) g_ps[7] += tt_ - tv_; }
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
// A graphics stage's float textures and images bound to texture views of an integer format (an R16G16_UINT, R32_UINT or RGBA8_UINT view): on the Switch the shader gets the
// integers' bits in its registers; the translator types them float (no shader facts), and a float view of the same size in their place (vka_tex.c class_twin) converts
// them (as half floats, as normalized bytes; an R32 float view may flush denormal or NaN patterns). So the draw's pipeline takes the stage's module with those bindings
// retyped (vka_shader_tex_module, vka_pso.c pstate_fill) and their records ask for views of the integer class. From the walk (walk_all, just before); a stage whose module
// the rewrite refuses keeps the class twins. DM_VK_TEX_RETYPE=0 (live): never, as before
static void tex_masks(vka_shader *const sh[VKA_NSTAGE], uint64_t tu[5], uint64_t ts[5]) {
    if (!VKA_ENV("DM_VK_TEX_RETYPE", 1)) return;
    static const int st_of[] = {0, 1, 2, 3, 4, 6, 7};
    for (size_t j = 0; j < sizeof st_of / sizeof *st_of; j++) {
        int st = st_of[j]; vka_shader *s = sh[st]; if (!s) continue;
        int rg = vka_shader_tex_range(s); if (rg < 0 || rg >= 5) continue;
        vka_res *r = g_wk.r[st]; uint64_t um = 0, sm = 0, fm = 0, lo = (uint64_t)rg * DMSH_VK_STAGE_BINDINGS;
        for (int i = 0; i < g_wk.n[st]; i++) {
            if ((r[i].what != VKA_R_TEX && r[i].what != VKA_R_IMG) || !r[i].tid || (r[i].dim & (DMSH_VK_DIM_SINT | DMSH_VK_DIM_UINT))) continue;
            if (r[i].desc != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && r[i].desc != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && r[i].desc != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
            if (r[i].binding < lo || r[i].binding >= lo + 64) continue;
            int c = vka_tex_id_class_rec(r[i].tid); uint64_t bit = 1ull << (r[i].binding - lo);
            if (r[i].elem) fm |= bit;   // (an array binding keeps its float type)
            if (c == 1) um |= bit; else if (c == 2) sm |= bit; else fm |= bit;
        }
        uint64_t mixed = (um & sm) | ((um | sm) & fm); um &= ~mixed; sm &= ~mixed;
        if (!(um | sm)) continue;
        vka_texmod_key tk = {DMSH_VK_MOD_BASE, 0, 0, 0, 0, -1, um, sm};   // (the rewrite's check on the base module; the pipeline's own module is made in pstate_fill)
        if (!vka_shader_tex_module(s, &tk)) continue;
        tu[rg] |= um; ts[rg] |= sm;
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG) && r[i].binding >= lo && r[i].binding < lo + 64 && ((um | sm) >> (r[i].binding - lo) & 1))
            r[i].dim |= um >> (r[i].binding - lo) & 1 ? DMSH_VK_DIM_UINT : DMSH_VK_DIM_SINT;
    }
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
// The last draw's writes, kept as what made them (DM_VK_SETS_MEMO, live, default 1). Building them (stage_writes) is 7.7 us a draw on the phone: 7.6 image descriptors of it,
// each resolving its texture ID to a view. The resolution's inputs are the walk (walk_all), the shaders' keys (their c1 bytes), the texture IDs' view and sampler versions, the
// open pass's attachments, the memory segments' generation, and the draw state the driver segments and the alpha test's copy are filled from. When all of those are the ones
// the last draw had, the descriptors are the same and are not built again: g_w still holds them (a draw resolves set 0 once, and the only other writer, the instanced run's
// range shrinking, drops the memo). Only graphics draws whose walks resolve without an image of their own, a storage image, a shared-memory spill, a texel buffer or raw global
// memory's log are kept: those take memory or a new scratch span each time. DM_VK_SETS_MEMO=0: every draw builds them, as before
typedef struct {
    uint64_t lay, texgen, memgen, fbgen, alpha, spush, pools, base, sk[VKA_NSTAGE], stemo[VKA_NSTAGE][2];
    int rast_discard;
} sets_key;
static struct { sets_key k; int ok, any, n[VKA_NSTAGE], pool[VKA_NSTAGE]; uint64_t rv[VKA_NSTAGE][512]; vka_res r[VKA_NSTAGE][512]; } g_smemo;
static void sets_memo_drop(void) { g_smemo.ok = 0; }
// The key of this draw's writes: the layout, the shaders, the generations the views and samplers resolve from, and the state the driver segments are filled from
static void sets_key_of(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, sets_key *k) {
    memset(k, 0, sizeof *k);   // (memcmp'd: its padding too)
    uint32_t ab; memcpy(&ab, &d->alpha_ref, 4);
    k->lay = (uint64_t)lay->layout; k->texgen = vka_tex_gen(); k->memgen = vka_mem_gen(); k->fbgen = vka_tex_fb_gen();
    k->alpha = (uint64_t)ab | (uint64_t)d->color.alpha_func << 32;
    k->spush = d->s_push; k->pools = (uint64_t)(uint32_t)d->b.tex_base | (uint64_t)(uint32_t)d->b.samp_base << 32;
    k->base = (uint64_t)d->b.base_instance | (uint64_t)d->b.samples << 32; k->rast_discard = d->rast_discard;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        k->sk[st] = sh[st] ? sh[st]->key : 0;
        k->stemo[st][0] = d->s_ssbo[st]; k->stemo[st][1] = d->s_tex[st];
    }
}
// A record's view and sampler versions (the memos' keys): what a texture or image binding resolves to changes only with them
static uint64_t sets_rv(const vka_res *r) {
    if (r->what != VKA_R_TEX && r->what != VKA_R_IMG && r->what != VKA_R_TBUF) return 0;
    return (uint64_t)vka_tex_ver(r->tid) | (uint64_t)vka_samp_ver(r->sid) << 32;
}
// 0 when the draw's writes cannot be kept: they take memory or a new scratch span each time, so no two draws' are ever the same
static int sets_keepable(vka_shader *const sh[VKA_NSTAGE], VkPipelineBindPoint bp) {
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS || !g_drv_memo) return 0;   // (a compute stage's driver segment is filled into a new span every dispatch)
    if (sh[2] && sh[2]->gs_fx > 0) return 0;                              // (the claim words: a zeroed span per draw)
    for (int st = 0; st < VKA_NSTAGE; st++) for (int i = 0; i < g_wk.n[st]; i++) {
        const vka_res *r = &g_wk.r[st][i];
        if (r->what == VKA_R_IMG || r->what == VKA_R_TBUF || r->what == VKA_R_SHARED_SPILL || r->what == VKA_R_GMEM_LOG) return 0;
        if (r->desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || r->desc == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || r->desc == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return 0;
    }
    return 1;
}
// The walk's records compared field by field (vka_res has padding bytes vka_walk's initializer does not fix: a raw memcmp would read them)
static inline int res_same(const vka_res *a, const vka_res *b) {
    return a->what == b->what && a->desc == b->desc && a->flags == b->flags && a->dim == b->dim && a->binding == b->binding && a->elem == b->elem
        && a->bank == b->bank && a->tid == b->tid && a->sid == b->sid && a->addr == b->addr && a->size == b->size;
}
// 1 when the last kept draw's walk and view and sampler versions are this draw's
static uint64_t g_sm_hit, g_sm_try;   // draws whose writes were kept, of the draws that could be (vka_draw_pace's line)
static int sets_memo_same(const sets_key *k) {
    if (memcmp(k, &g_smemo.k, sizeof *k)) return 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        int n = g_wk.n[st];
        if (n != g_smemo.n[st] || g_wk.pool[st] != g_smemo.pool[st]) return 0;
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) if (sets_rv(&g_wk.r[st][i]) != g_smemo.rv[st][i] || !res_same(&g_wk.r[st][i], &g_smemo.r[st][i])) return 0;
    }
    return 1;
}
// The writes the build below makes, kept for the next draw (the key is the one the hit test read: what the build does must not change it)
static void sets_memo_store(const sets_key *k, int any_pool) {
    g_smemo.k = *k; g_smemo.any = any_pool;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        int n = g_smemo.n[st] = g_wk.n[st]; g_smemo.pool[st] = g_wk.pool[st];
        for (int i = 0; i < n; i++) g_smemo.rv[st][i] = sets_rv(&g_wk.r[st][i]);
        if (n > 0) memcpy(g_smemo.r[st], g_wk.r[st], (size_t)n * sizeof(vka_res));
    }
    g_smemo.ok = 1;
}
static int bind_sets(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, VkPipelineBindPoint bp, c0_at *c0) {
    int keep = VKA_ENV("DM_VK_SETS_MEMO", 1) && sets_keepable(sh, bp);
    sets_key k; if (keep) sets_key_of(d, sh, lay, &k);   // (before the build: what it resolves must not change it)
    if (keep && g_smemo.ok) {
        g_sm_try++;
        if (sets_memo_same(&k)) {   // g_w as the last draw left it: the same writes
            g_sm_hit++;
            if (g_sets_dry) { g_dry_pool = g_smemo.any; return 1; }
            return sets_push(lay, bp, g_smemo.any);
        }
    }
    g_smemo.ok = 0;   // (anything below that fails leaves it dropped)
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
    if (keep) sets_memo_store(&k, any_pool);
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
static uint32_t g_vi_ser;   // raised at each vin_build: VI may have changed
static void vin_build(const vka_dstate *d, const vka_shader *vs, vka_gkey *k) {
    VI.nb = VI.na = VI.used = 0; VI.zero = 0; g_vi_ser++;
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
// The vertex input unchanged since the last draw (DM_VK_VIN_SKIP, live from the next submission, default 1; 0: built and compared every draw, as before). With vertex input
// dynamic state the descriptions (VI) are a function of the vertex shader and the attribute and stream records alone (vin_build), so a draw with the same shader and no
// such record since (d->s_vfmt) keeps VI and the key's retyped inputs (VC). And vin_bind of an unchanged VI, with no vertex buffer record since (d->s_vbuf) and no segment
// come or gone (vka_mem_gen: the address lookups), into a command buffer that holds what the last vin_bind bound (VB.valid, no pipeline bind since: vin_stale), sets and
// binds nothing (every slot equal to the bound one, DM_VK_VB_FILTER), so it is not run (VBC). Any other vin_bind (an instanced run's) forgets VBC
static uint64_t g_vin_kept, g_vin_built, g_vb_kept, g_vb_bound;
static struct { int valid; const vka_shader *vs; uint64_t s_vfmt; uint32_t vin_u, vin_s; } VC;
static struct { int valid; uint32_t ser; uint64_t s_vbuf, mg; } VBC;
static void vin_build_kept(const vka_dstate *d, const vka_shader *vs, vka_gkey *k) {
    if (g_vin_skip && V.vin_dyn && VC.valid && VC.vs == vs && VC.s_vfmt == d->s_vfmt) { k->vin_u = VC.vin_u; k->vin_s = VC.vin_s; g_vin_kept++; return; }
    vin_build(d, vs, k); g_vin_built++;
    VC.valid = 1; VC.vs = vs; VC.s_vfmt = d->s_vfmt; VC.vin_u = k->vin_u; VC.vin_s = k->vin_s;
}
static void vin_bind_kept(const vka_dstate *d) {
    uint64_t mg = vka_mem_gen();
    if (g_vin_skip && V.vin_dyn && g_vb_filter && VB.valid && !VB.vin_stale && VBC.valid && VBC.ser == g_vi_ser && VBC.s_vbuf == d->s_vbuf && VBC.mg == mg) { g_vb_kept++; return; }
    vin_bind(d); g_vb_bound++;
    VBC.valid = 1; VBC.ser = g_vi_ser; VBC.s_vbuf = d->s_vbuf; VBC.mg = mg;
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
void vka_dyn_invalidate(void) { DY.valid = 0; VB.valid = 0; VBC.valid = 0; IB.valid = 0; g_pd[0].n = g_pd[1].n = 0; inst_reset(); }
#define PT(i) do { if (g_prof) { uint64_t t_ = cntvct(); if ((i) > 0 && g_pt_ph == (i) - 1) g_pt[(i) > 0 ? (i) - 1 : 0] += t_ - g_pt_last; g_pt_last = t_; g_pt_ph = (i); } } while (0)
// ---------- Instanced runs (DM_VK_INSTANCE) ----------
// Donkey Kong Bananza draws many objects as runs of draws that repeat one mesh (the same pipeline, index range, base vertex and vertex streams) and differ only in where one
// 256-byte uniform block sits: the object's constants, read by the vertex shader and by the pixel shader through one binding each, moved by the same amount (a third of the
// level's draws, in runs of 32 on average). On an Adreno 740 (AYN Odin 2 Portal) every draw costs the GPU about 1-1.5 us however small, so such a run
// is drawn as one instanced draw: from its second draw on, the shaders are instanced variants (vka_spv_instance: the block read at data[i + d], d per instance
// from a uint vertex input at location 30, passed to the pixel shader as a flat input), the block is bound as a window from the run's first offset, and each further draw
// only writes its d into the run's table (an instance-rate vertex buffer) and raises the instance count of the run's indirect draw, in host-visible memory before the
// submission. A draw joins only when nothing was recorded since the run's last one (the encoder's call count): anything else closes the run and the draw is drawn alone.
// Variants are rewritten from the library's SPIR-V when a run first asks for them; a pair without both is never instanced. DM_VK_INSTANCE=1 turns the runs on (live; off by default)
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
// The instanced variant of shader s with the block at binding moved (NULL = none: the rewrite refused it)
static vka_shader *inst_variant_make(vka_shader *s, uint32_t binding);
// The frame path and the prewarm threads (instanced variants in the prewarm list: vka_inst_variant) share the cache, so one variant is made once
static pthread_mutex_t g_ivmu = PTHREAD_MUTEX_INITIALIZER;
static vka_shader *inst_variant(vka_shader *s, uint32_t binding) { pthread_mutex_lock(&g_ivmu); vka_shader *v = inst_variant_make(s, binding); pthread_mutex_unlock(&g_ivmu); return v; }
vka_shader *vka_inst_variant(vka_shader *s, uint32_t binding) { return VKA_ENV("DM_VK_INSTANCE", 0) ? inst_variant(s, binding) : NULL; }
static vka_shader *inst_variant_make(vka_shader *s, uint32_t binding) {
    static struct { uint64_t key; uint32_t binding; vka_shader *v; } cache[128]; static int ncache;
    for (int i = 0; i < ncache; i++) if (cache[i].key == s->key && cache[i].binding == binding) return cache[i].v;
    if (ncache == 128) return NULL;
    vka_shader *v = NULL;
    {   // made from the library's SPIR-V (vka_spv_instance)
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
typedef struct { uint64_t pt[9], ps[8], n, tot; } dprof;
static void dprof_take(dprof *last, dprof *d) {   // the timed draws since the reader's last reading
    dprof c; memcpy(c.pt, g_pt, sizeof c.pt); memcpy(c.ps, g_ps, sizeof c.ps); c.n = g_pt_n; c.tot = g_pt_tot;
    for (int i = 0; i < 9; i++) d->pt[i] = c.pt[i] - last->pt[i];
    for (int i = 0; i < 8; i++) d->ps[i] = c.ps[i] - last->ps[i];
    d->n = c.n - last->n; d->tot = c.tot - last->tot; *last = c;
}
static double tick_us(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return 1e6 / (double)f; }
static const char *g_pt_nm[8] = {"resolve", "layout", "images+pass+key", "vertex input", "pipeline", "sets", "vertex buffers+dynamic", "draw"};
void vka_draw_prof(char *b, size_t n, double draws) {
    static dprof last; dprof d; dprof_take(&last, &d);
    if (!g_prof_all || draws <= 0 || !d.n) { if (n) b[0] = 0; return; }
    double us = tick_us() / (double)d.n;
    const char **nm = g_pt_nm;
    size_t o = 0; for (int i = 0; i < 8 && o < n; i++) o += snprintf(b + o, n - o, "%s%s %.2f", i ? ", " : "", nm[i], d.pt[i] * us);
    if (o < n) o += snprintf(b + o, n - o, "; pushes %.0f%% skipped as the last one", g_pd_skip + g_pd_push ? 100.0 * g_pd_skip / (double)(g_pd_skip + g_pd_push) : 0.0);
    g_pd_skip = g_pd_push = 0;
    if (o < n) o += snprintf(b + o, n - o, "; sets: walk %.2f, writes %.2f (driver segments %.2f, %.1f images %.2f: view %.2f, rest %.2f), push %.2f",
                             d.ps[0] * us, d.ps[1] * us, d.ps[3] * us, d.ps[5] / (double)d.n, d.ps[4] * us, d.ps[6] * us, d.ps[7] * us, d.ps[2] * us);
    if (g_inst_on && o < n) o += snprintf(b + o, n - o, "; instanced runs %llu (%.1f draws joined each, %.0f%% of them without the walk), %llu fell back to single draws",
                                          (unsigned long long)g_inst_runs, g_inst_runs ? (double)g_inst_joined / (double)g_inst_runs : 0.0,
                                          g_inst_joined ? 100.0 * (double)g_inst_fast / (double)g_inst_joined : 0.0, (unsigned long long)g_inst_falls);
    g_inst_runs = g_inst_joined = g_inst_fast = g_inst_falls = 0;
}
// The pace line's view of the draw path (vka_perf.c pace_tick, on the recording thread): the timed draws' mean per draw (us) and by phase, and the shortcuts' shares.
// *draw_us: the mean whole time of a draw (0: none timed)
void vka_draw_pace(char *b, size_t n, double *draw_us) {
    static dprof last; static uint64_t vk0, vb0, vk1, vb1, dh0, dm0, ds0, sh0, st0;
    static uint64_t mh0, mo0, mv0, mi0, mg0;   // the view memo's counters at the last line
    static uint64_t msh0, mst0;                // the sampler memo's
    dprof d; dprof_take(&last, &d);
    uint64_t vk = g_vin_kept - vk0, vb = g_vin_built - vb0, bk = g_vb_kept - vk1, bb = g_vb_bound - vb1, dh = g_dm_hit - dh0, dm = g_dm_miss - dm0, ds = g_dm_stamp - ds0;
    uint64_t sh = g_sm_hit - sh0, st = g_sm_try - st0;
    vk0 = g_vin_kept; vb0 = g_vin_built; vk1 = g_vb_kept; vb1 = g_vb_bound; dh0 = g_dm_hit; dm0 = g_dm_miss; ds0 = g_dm_stamp; sh0 = g_sm_hit; st0 = g_sm_try;
    *draw_us = 0; if (n) b[0] = 0;
    size_t o = 0;
    if (d.n) {
        double us = tick_us() / (double)d.n; *draw_us = d.tot * us;
        o += (size_t)snprintf(b + o, n - o, "draw path %.2f us a draw (1 in %d timed:", d.tot * us, g_prof_all ? 1 : g_prof_every);
        for (int i = 0; i < 8 && o < n; i++) o += (size_t)snprintf(b + o, n - o, " %s %.2f%s", g_pt_nm[i], d.pt[i] * us, i < 7 ? "," : "");
        char vs[64] = "";
        if (g_prof_img) snprintf(vs, sizeof vs, ": view %.2f, rest %.2f", d.ps[6] * us, d.ps[7] * us);
        if (o < n) o += (size_t)snprintf(b + o, n - o, "; sets: walk %.2f, writes %.2f (%.1f images %.2f%s; driver segments %.2f), push %.2f)",
                                      d.ps[0] * us, d.ps[1] * us, d.ps[5] / (double)d.n, d.ps[4] * us, vs, d.ps[3] * us, d.ps[2] * us);
    }
    {   // the view memo's own hits and misses (vka_tex.c vka_tex_memo_stats): which of its four reasons a lookup was not answered from it
        uint64_t h, other, ver, infb, gen2; vka_tex_memo_stats(&h, &other, &ver, &infb, &gen2);
        uint64_t dh2 = h - mh0, dot = other - mo0, dv = ver - mv0, di = infb - mi0, dg = gen2 - mg0;
        mh0 = h; mo0 = other; mv0 = ver; mi0 = infb; mg0 = gen2;
        uint64_t tot = dh2 + dot + dv + di + dg;
        if (o < n && tot) o += (size_t)snprintf(b + o, n - o, "%sview memo answered %.0f %% of %llu lookups: another ID in the slot %.0f, the ID's version moved (registered again, its texture dropped or its image remade) %.0f, a pass's own target %.0f, a texture dropped since %.0f",
                                               o ? "; " : "", 100.0 * (double)dh2 / (double)tot, (unsigned long long)tot, 100.0 * (double)dot / (double)tot, 100.0 * (double)dv / (double)tot,
                                               100.0 * (double)di / (double)tot, 100.0 * (double)dg / (double)tot);
    }
    {   // the sampler memo (vka_tex.c vka_samp_memo_stats)
        uint64_t h, t; vka_samp_memo_stats(&h, &t);
        uint64_t dh3 = h - msh0, dt3 = t - mst0; msh0 = h; mst0 = t;
        if (o < n && dt3) o += (size_t)snprintf(b + o, n - o, "%ssampler memo answered %.0f %% of %llu lookups", o ? "; " : "", 100.0 * (double)dh3 / (double)dt3, (unsigned long long)dt3);
    }
    if (o < n) o += (size_t)snprintf(b + o, n - o, "%sset 0 writes kept %.0f %% of the draws that could (DM_VK_SETS_MEMO)",
                                      o ? "; " : "", st ? 100.0 * (double)sh / (double)st : 0.0);
    if (o < n) o += (size_t)snprintf(b + o, n - o, "%svertex input kept %.0f %% (DM_VK_VIN_SKIP), vertex buffers kept %.0f %%, driver segments %.0f %% by their inputs (DM_VK_DRV_STAMP) of %.0f %% bound again",
                                      o ? "; " : "", vk + vb ? 100.0 * vk / (double)(vk + vb) : 0.0, bk + bb ? 100.0 * bk / (double)(bk + bb) : 0.0, dh ? 100.0 * ds / (double)dh : 0.0, dh + dm ? 100.0 * dh / (double)(dh + dm) : 0.0);
}
static void draw_body(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di);
// A draw, timed when it is DM_VK_PROF's (every draw) or the pace line's sample (one in DM_VK_PACE_PROF, default 16: ten counter reads in a timed draw, none in the others)
void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di) {
    static int tick;
    if (!g_prof_all && (g_prof_every <= 0 || ++tick < g_prof_every)) { draw_body(d, rt, has_rt, sc, has_sc, dw, di); return; }
    tick = 0; g_prof = 1; g_pt_ph = -1;
    uint64_t t0 = cntvct();
    draw_body(d, rt, has_rt, sc, has_sc, dw, di);
    g_pt_tot += cntvct() - t0; g_pt_n++; g_prof = g_prof_all;
}
// Profile mesh_prepass_cut (<prepass mesh shader key> <colour pass mesh shader key> pairs). A WORKAROUND, cause open (docs/reference/rendering.md "Depth-only mesh prepass
// drawn as spikes on Qualcomm's driver"): on Qualcomm's driver the depth-only prepass mesh draws of Ocarina of Time's foliage draw some primitives from vertex slots never
// written (white spikes converging on the screen centre), while the colour pass's own mesh modules draw the same instances right; believed to be the driver's compiler on
// the prepass modules, not proven. So a depth-only draw (depth test and write on, no colour channel written) of a listed prepass mesh shader is skipped (-1), and a draw of
// its colour pass's mesh shader tests LEQUAL where the game asked for EQUAL (1): the colour pass then draws against the depth without the foliage. Cost: the foliage writes
// no depth (its colour pass writes none), so later passes that read the depth (fog, shadows) may differ where it is. On Qualcomm's driver (driverID) by default;
// DM_VK_MESH_PREPASS_SKIP=0 off, =1 on every driver
static int mesh_cut(const vka_dstate *d, const p_set_rt *rt, int has_rt, uint64_t key) {
    static const uint64_t cut[][2] = DM_GAME_MESH_PREPASS_CUT;
    if (!DM_GAME_NMESH_PREPASS_CUT) return 0;
    int m = VKA_ENV("DM_VK_MESH_PREPASS_SKIP", -1);
    if (m < 0 ? V.driver_id != VK_DRIVER_ID_QUALCOMM_PROPRIETARY : !m) return 0;
    for (int i = 0; i < DM_GAME_NMESH_PREPASS_CUT; i++) {
        if (key == cut[i][1]) return 1;
        if (key != cut[i][0]) continue;
        if (!d->ds.dt || !d->ds.dw) return 0;
        for (int c = 0; has_rt && rt && c < rt->n && c < 8; c++) if (rt->color[c] && (d->chmask.mask >> 4 * c & 0xf)) return 0;
        return -1;
    }
    return 0;
}
static void draw_body(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di) {
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
    int eq_relax = 0;   // mesh_cut: EQUAL drawn as LEQUAL
    if (mesh) {
        if (!V.mesh) SKIP("mesh draw (nvnCommandBufferDrawMeshTasks) on a device without VK_EXT_mesh_shader (or DM_VK_MESH=0): skipped");
        // The program whose task and mesh shaders run: the fragment stage's when it has a mesh shader (a program's stages are bound together), else the one bound at the
        // mesh stage bit. Task and mesh come from the same program (a program's task shader feeds its own mesh shader)
        const prog_t *mp = d->bst[7];
        { vka_shader *m = NULL; unsigned b2 = 0; if (d->bst[1] && d->bst[1] != mp && resolve(d->bst[1], 7, &m, &b2) && (m || (b2 & 1u << 7))) mp = d->bst[1]; }
        if (!resolve(mp, 6, &sh[6], &bad) || !resolve(mp, 7, &sh[7], &bad) || !resolve(d->bst[1], 1, &sh[1], &bad)) SKIP("mesh draw skipped: no shader library");
        if (bad) { g_skipped++; return; }   // a refused or missing shader (logged when it was made)
        if (!sh[7]) SKIP("mesh draw with no mesh shader bound: skipped");
        // Diagnostics: DM_VK_MESH_LOG=1 logs each (task, mesh, pixel) shader triple once with its first draw's workgroups; DM_VK_MESH_SKIP=<mesh key>,... skips the mesh
        // draws of those mesh shaders (a wrong mesh draw found by taking draws away in a replay)
        if (VKA_ENV("DM_VK_MESH_LOG", 0)) { static uint64_t seen[256][2]; static int ns; uint64_t a = sh[7]->key, b = sh[1] ? sh[1]->key : 0; int k = 0;
            while (k < ns && !(seen[k][0] == a && seen[k][1] == b)) k++;
            if (k == ns && ns < 256) { seen[ns][0] = a; seen[ns++][1] = b;
                VLOG("mesh draw: task %016llx mesh %016llx pixel %016llx, %s %d workgroups%s", sh[6] ? (unsigned long long)sh[6]->key : 0ull, (unsigned long long)a, (unsigned long long)b,
                     dw->ind ? "indirect" : "direct", dw->count, (sh[7]->e->flags & DMSH_VK_F_MESH_PRIM) ? ", per-primitive outputs" : ""); } }
        { const char *sk = getenv("DM_VK_MESH_SKIP"); char hex[17]; snprintf(hex, sizeof hex, "%016llx", (unsigned long long)sh[7]->key);
          if (sk && *sk && strstr(sk, hex)) { g_skipped++; return; } }
        { int c = mesh_cut(d, rt, has_rt, sh[7]->key); if (c < 0) { g_skipped++; return; } eq_relax = c > 0; }   // profile mesh_prepass_cut (a workaround)
        if (d->bst[6] && d->bst[6] != mp) VRED_ONCE("mesh draw: a program bound at the task stage bit apart from the mesh program; drawn with the mesh program's own task shader, if any");
        // A pixel shader with per-primitive inputs reads what its mesh shader writes per primitive (Vulkan matches only equally decorated interfaces). The other way round
        // (a mesh shader with per-primitive outputs, a pixel shader without the decoration: vk_android_build.py leaves a pixel shader also drawn with vertex shaders per
        // vertex) the flags cannot tell whether it reads those locations: a red line
        if (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM) && !(sh[7]->e->flags & DMSH_VK_F_MESH_PRIM))
            SKIP("mesh draw: pixel shader %016llx reads per-primitive inputs that mesh shader %016llx does not write: skipped", (unsigned long long)sh[1]->key, (unsigned long long)sh[7]->key);
        if (sh[1] && !(sh[1]->e->flags & DMSH_VK_F_MESH_PRIM) && (sh[7]->e->flags & DMSH_VK_F_MESH_PRIM))
            VRED_ONCE("mesh draw: mesh shader %016llx writes per-primitive outputs and pixel shader %016llx has no per-primitive inputs: any it reads there are undefined", (unsigned long long)sh[7]->key, (unsigned long long)sh[1]->key);
        if (dw->first) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("mesh draw with first workgroup %d (count %d): drawn from 0 (the translated task / mesh shaders read their workgroup index with no base; open)", dw->first, dw->count); }
        if (!dw->ind && dw->count <= 0) return;
        if (d->tf_on) VRED_ONCE("transform feedback with a mesh draw: drawn without capture");
    } else {
        for (int st = 0; st < 5; st++) if (!resolve(d->bst[st], st, &sh[st], &bad)) SKIP("draw skipped: no shader library");
        if (bad) { g_skipped++; return; }   // a refused or missing shader (logged when it was made)
        if (!sh[0]) SKIP("draw with no vertex shader bound: skipped");
        if (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM)) SKIP("pixel shader %016llx reads per-primitive mesh outputs and is drawn with a vertex shader: skipped", (unsigned long long)sh[1]->key);
        if (d->rast_discard && d->tf_on) SKIP("transform feedback is not on Vulkan yet (F_TF): the draw is skipped");
        if (d->tf_on) VRED_ONCE("transform feedback is not on Vulkan yet: the draw is drawn without capture");
    }
    // DM_VK_DRAW_SKIP_KEY=<pixel shader key>,... (diagnostic): the draws of those pixel shaders are not drawn (their pass still opens and clears), to see a pass's share of the picture
    { static char sk[1024]; static int got; if (!got) { const char *e = getenv("DM_VK_DRAW_SKIP_KEY"); snprintf(sk, sizeof sk, "%s", e ? e : ""); got = 1; }
      if (sk[0] && sh[1]) { char hex[17]; snprintf(hex, sizeof hex, "%016llx", (unsigned long long)sh[1]->key); if (strstr(sk, hex)) { g_skipped++; return; } } }
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
    tex_masks(sh, k.tex_u, k.tex_s);
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
        k.att[i].mask = written ? (uint8_t)(m & fs->fcomp[i]) : 0;   // outputs, and components of an output, the shader does not write leave the target as it was (Maxwell writes only the shader's outputs)
        if (written && (d->color.blend_en >> i & 1) && att_cls == DMSH_VK_TYPE_FLOAT) {
            blend_st b = adv_blend(d->blend[i]);
            if (variant != DMSH_VK_MOD_DUAL && (is_src1(b.src_rgb) || is_src1(b.dst_rgb) || is_src1(b.src_a) || is_src1(b.dst_a))) {   // no second source: it reads as zero
                src1_used = 1; b.src_rgb = zero_src1(b.src_rgb); b.dst_rgb = zero_src1(b.dst_rgb); b.src_a = zero_src1(b.src_a); b.dst_a = zero_src1(b.dst_a); }
            k.att[i] = (typeof(k.att[i])){1, (uint8_t)bfac(b.src_rgb), (uint8_t)bfac(b.dst_rgb), (uint8_t)bop(b.eq_rgb), (uint8_t)bfac(b.src_a), (uint8_t)bfac(b.dst_a), (uint8_t)bop(b.eq_a), (uint8_t)m};
        }
    }
    if (src1_used) VRED_ONCE("a SRC1 blend factor with no dual-source module (MOD_DUAL; the profile's dual_want.tsv) or no dualSrcBlend: the second source reads as zero");

    PT(3);
    if (!mesh) vin_build_kept(d, sh[0], &k);
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
                g_w.bi[i_wv].range = wv; g_w.bi[i_wp].range = wp; sets_memo_drop();   // (g_w is no longer what the walk alone makes it)
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
            vin_bind(d); VBC.valid = 0;
            VI.nb--; VI.na--;
            VkDeviceSize tsz = 4 * INST_CAP; vk.vkCmdBindVertexBuffers2(g_cb, INST_BIND, 1, &i_ta.buf, &i_ta.off, &tsz, NULL);
        } else vin_bind_kept(d);
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
    uint32_t dfv = eq_relax && d->ds.df == NVN_DEPTH_FUNC_EQUAL ? NVN_DEPTH_FUNC_LEQUAL : d->ds.df;   // mesh_cut
    DY_SET1(dc, (uint32_t)(dfv >= 1 && dfv <= 8 ? (VkCompareOp)(dfv - 1) : VK_COMPARE_OP_ALWAYS), vk.vkCmdSetDepthCompareOp(g_cb, DY.dc));
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
    if (hd) {   // DM_VK_PARTLOG / DM_VK_ZLOG: the draw's depth state (vka_rp_zstats)
        int zt = (int)DY.dt, zw = (int)DY.dw, op = (int)DY.dc & 7, dir = !zt ? 0 : op == VK_COMPARE_OP_LESS || op == VK_COMPARE_OP_LESS_OR_EQUAL ? 1 : op == VK_COMPARE_OP_GREATER || op == VK_COMPARE_OP_GREATER_OR_EQUAL ? 2 : 0;
        g_zs.nd++; g_zs.nt += zt; g_zs.nw += zw; if (zt) g_zs.op[op]++;
        int flip = dir && g_zdir && dir != g_zdir; if (flip) g_zs.flips++; if (dir) g_zdir = dir;
        if (zw && (op == VK_COMPARE_OP_ALWAYS || op == VK_COMPARE_OP_NOT_EQUAL)) g_zs.naw++;
        g_zs.ns += DY.stt != 0; g_zs.nb += DY.dbe != 0; g_zs.nrd += DY.rd != 0;
        if (g_zlog_pass && zlog_room())
            VLOG("zlog draw %u of the part in pass %llu: test %d write %d op %d%s stencil %u bias %u (%g %g %g) discard %u cull %u %s %016llx/%016llx vp %g,%g %gx%g %g-%g sc %d,%d %ux%u", g_zpart_d, (unsigned long long)vka_n_rp,
                 zt, zw, op, flip ? " FLIP" : "", DY.stt, DY.dbe, DY.db[0], DY.db[1], DY.db[2], DY.rd, DY.cull, mesh ? "mesh" : "vertex", (unsigned long long)(mesh ? (sh[7] ? sh[7]->key : 0) : sh[0] ? sh[0]->key : 0),
                 (unsigned long long)(sh[1] ? sh[1]->key : 0), DY.vp.x, DY.vp.y, DY.vp.width, DY.vp.height, DY.vp.minDepth, DY.vp.maxDepth, DY.sc.offset.x, DY.sc.offset.y, DY.sc.extent.width, DY.sc.extent.height);
    }
    g_zpart_d++;

    PT(7);
    // The draw
    uint32_t inst = (uint32_t)((dw ? dw->instances : di->instances) > 0 ? (dw ? dw->instances : di->instances) : 1), binst = d->b.base_instance;
    if (vka_gp_active) vka_gp_note(mesh ? 'm' : 'v', mesh ? sh[7]->key : sh[0] ? sh[0]->key : 0, sh[1] ? sh[1]->key : 0, mesh ? (ind ? 0 : (uint64_t)dw->count) : (uint64_t)(dw ? dw->count : di->count) * inst);
    if (mesh) {   // count workgroups of the task shader, or of the mesh shader without one (the device's limit per dimension: maxTaskWorkGroupCount / maxMeshWorkGroupCount, 65535 on the Adreno 840)
        if (ind) {
            VkBuffer b; VkDeviceSize o;
            if (!vka_mdx_args(ind, &b, &o)) SKIP("indirect mesh draw record at %#llx not in placed guest RAM, or its {count, 1, 1} copy not made: skipped", (unsigned long long)ind);
            if (vk.vkCmdDrawMeshTasksIndirectEXT) vk.vkCmdDrawMeshTasksIndirectEXT(g_cb, b, o, 1, 12);
            else SKIP("vkCmdDrawMeshTasksIndirectEXT not available: skipped");
        } else {
            static uint32_t largest; if ((uint32_t)dw->count > largest) { largest = (uint32_t)dw->count; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("mesh draw: largest so far %u workgroups (task %016llx mesh %016llx)", largest, sh[6] ? (unsigned long long)sh[6]->key : 0ull, (unsigned long long)sh[7]->key); }
            uint32_t maxg = sh[6] ? V.task_groups : V.mesh_groups;
            if ((uint32_t)dw->count > maxg) SKIP("mesh draw of %d workgroups: more than the device's %u in one dimension; skipped", dw->count, maxg);
            vk.vkCmdDrawMeshTasksEXT(g_cb, (uint32_t)dw->count, 1, 1);
        }
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
            vin_bind(d); VBC.valid = 0;
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

int dm_sdk_ounce(void);
static int g_nvn2 = -1;
void vka_set_nvn2(int on) { g_nvn2 = on != 0; }
int vka_nvn2(void) { if (g_nvn2 < 0) g_nvn2 = dm_sdk_ounce(); return g_nvn2; }   // first called at a dispatch or a capture, after the modules are loaded
int vka_is_ndve_key(uint64_t k) {
    switch (k) {
        case 0xc2fc2ddd604c25e7ull: case 0x4f41ccf24f163e0dull: case 0xba079f68ae288dbaull:
        case 0xa8b4fb48fee0ccf8ull: case 0xe2eebd7ba18fe35full: case 0xe6be82d835fbe4d3ull:
        case 0x9442573ee4ace011ull: case 0x0a56002cdc7e1fceull: case 0xd15370a382630d4eull:
        case 0x7f7ff0f36b54d90full: case 0x5a7d69b067a7f011ull: case 0x329e4b0f0b3f2bc6ull:
        case 0x7653fe5f6ef0b2f2ull: case 0xb6e34b46dea44278ull: case 0x3f18f89630c05813ull:
        case 0x539ad3b353758de0ull: case 0xd72077c4b6cc264full: case 0xab9a92b55950ec02ull:
        case 0xc41f451b88a050f6ull: case 0x55ee6dc89a2b64e2ull: case 0x49dff010b9832251ull:
        case 0x8e98d86c0e4a7b53ull:
            return 1;
        default:
            return 0;
    }
}
// A compute shader's float storage images bound to textures viewed in an integer format (Ocarina of Time packs RGB9E5 and R11G11B10 texels into integer bits and stores them
// through R32_UINT texture views, agl_ndsr_vr_light a335c1db6270c355): on the Switch the shader's registers are written as the bound format's integers, bit for bit. The
// translator types such an image float (its format is not in the shader facts), and a float view of the same size in its place (vka_tex.c class_twin) converts the bits as
// floats. So the dispatch uses a pipeline whose module has those images retyped to the texture's class (vka_shader_img_module), and their records ask for views of that class.
// From the walk (walk_all, just before); the base pipeline p when nothing is retyped or the module cannot be made. DM_VK_IMG_RETYPE=0: never (the class twin views, as before)
static VkPipeline img_retype(vka_shader *cs, const vka_layout *lay, VkPipeline p) {
    if (!VKA_ENV("DM_VK_IMG_RETYPE", 1)) return p;
    vka_res *r = g_wk.r[5]; uint64_t um = 0, sm = 0, fm = 0;
    for (int i = 0; i < g_wk.n[5]; i++) {
        int img = r[i].what == VKA_R_IMG && r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        int tex = r[i].what == VKA_R_TEX && (r[i].desc == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || r[i].desc == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);   // (a texture: its fetches and samples)
        if (!(img || tex) || r[i].binding >= 64 || (r[i].dim & (DMSH_VK_DIM_SINT | DMSH_VK_DIM_UINT))) continue;
        int c = r[i].tid ? vka_tex_id_class_rec(r[i].tid) : 0; uint64_t bit = 1ull << r[i].binding;
        if (r[i].elem) c = 0;   // (an array binding keeps its float type)
        if (c == 1) um |= bit; else if (c == 2) sm |= bit; else fm |= bit;
    }
    uint64_t mixed = (um & sm) | ((um | sm) & fm); um &= ~mixed; sm &= ~mixed;   // (an array binding with elements of more than one class keeps its float type)
    if (!(um | sm)) return p;
    VkPipeline q = vka_cs_pipeline_img(cs, lay, um, sm);
    if (!q) return p;
    for (int i = 0; i < g_wk.n[5]; i++) if ((r[i].what == VKA_R_IMG || r[i].what == VKA_R_TEX) && r[i].binding < 64 && ((um | sm) >> r[i].binding & 1)) r[i].dim |= um >> r[i].binding & 1 ? DMSH_VK_DIM_UINT : DMSH_VK_DIM_SINT;
    return q;
}
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
    if (VKA_ENV("DM_VK_SKIP_NDVE", 1) && vka_is_ndve_key(sh[5]->key)) {
        if (vka_nan_on()) vka_tex_watch_dispatch(sh[5]->key);
        vka_barrier(g_cb);
        g_dispatches++;
        return;
    }
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
    p = img_retype(sh[5], &lay, p);
    {   // DM_VK_BIND_LOG=<hex key>[,…]: the binding records of those compute shaders' dispatches (the first 12 in all) (what each binding is bound to), to follow a pass's inputs and outputs
        const char *bl = getenv("DM_VK_BIND_LOG"); static _Atomic int nb; int hit = 0;
        for (const char *q = bl; q && *q && !hit;) { char *e; unsigned long long k = strtoull(q, &e, 16); if (e == q) break; hit = k == sh[5]->key; q = *e == ',' ? e + 1 : e; }
        if (hit && atomic_fetch_add(&nb, 1) < 12)
            for (int i = 0; i < g_wk.n[5]; i++) { const vka_res *x = &g_wk.r[5][i]; char why[320] = "";
                if ((x->what == VKA_R_TEX || x->what == VKA_R_IMG) && x->tid) vka_tex_id_desc(x->tid, why, sizeof why);
                VLOG("bindings of %016llx: binding %u elem %u what %u desc %u dim %#x flags %#x tid %d sid %d addr %#llx size %#llx %s", (unsigned long long)sh[5]->key, x->binding, x->elem, x->what, x->desc,
                     x->dim, x->flags, x->tid, x->sid, (unsigned long long)x->addr, (unsigned long long)x->size, why); }
    }
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
    if (!skip && VKA_ENV("DM_VK_SKIP_NDVE", 1) && vka_is_ndve_key(sh[5]->key)) skip = 1;
    // DM_VK_DISPATCH_SKIP_KEY=<hex key>[,…]: the dispatches of those compute shaders are recorded without their vkCmdDispatch (by shader instead of by index)
    const char *sks = getenv("DM_VK_DISPATCH_SKIP_KEY");
    for (const char *p = sks; !skip && p && *p;) { char *e; unsigned long long k = strtoull(p, &e, 16); if (e == p) break; skip = k == sh[5]->key; p = *e == ',' ? e + 1 : e; }
    if (lg) VLOG("dispatch %llu: %016llx groups %d,%d,%d%s local %u,%u,%u shared %u flags %#x%s", di, (unsigned long long)sh[5]->key, dp->x, dp->y, dp->z, dp->ind ? " (indirect)" : "",
                 sh[5]->e->local[0], sh[5]->e->local[1], sh[5]->e->local[2], sh[5]->e->shared, sh[5]->e->flags, skip ? " SKIPPED" : "");
    if (skip) { vka_barrier(g_cb); g_dispatches++; return; }
    if (vka_gp_active) vka_gp_note('c', sh[5]->key, 0, dp->ind ? 0 : (uint64_t)(dp->x > 0 ? dp->x : 0) * (uint64_t)(dp->y > 0 ? dp->y : 0) * (uint64_t)(dp->z > 0 ? dp->z : 0));
    vka_rw_pre(g_cb);   // DM_VK_RANGE_WATCH
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (dp->ind) vk.vkCmdDispatchIndirect(g_cb, ib, io);
    else if (dp->x > 0 && dp->y > 0 && dp->z > 0) {
        if (sb && (uint64_t)dp->x * dp->y * dp->z > ns) spill_dispatch((uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z, ns);
        else vk.vkCmdDispatch(g_cb, (uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z);
    }
    vka_barrier(g_cb);   // (also frees the shared memory spill buffer for the next dispatch)
    vka_rw_point(g_cb, 'd', sh[5]->key, 0, 0);
    if (vka_nan_on()) {   // DM_VK_NANSCAN: the storage images this dispatch wrote
        vka_img *ims[32]; int m = 0;
        for (int i = 0; i < g_wk.n[5] && m < 32; i++) if (g_wk.r[5][i].what == VKA_R_IMG && g_wk.r[5][i].tid) ims[m++] = vka_tex_id_img(g_wk.r[5][i].tid, g_cb);
        vka_nan_dispatch(g_cb, sh[5]->key, ims, m);
    }
    g_dispatches++;
}
void vka_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *dispatches, uint64_t *pipelines) {
    if (draws) *draws = g_draws; if (skipped) *skipped = g_skipped; if (dispatches) *dispatches = g_dispatches; if (pipelines) *pipelines = 0;
}
#endif
