// NVN2's DLSS (OP_DLSS) on Vulkan: the network has no Vulkan counterpart, so the replay writes the evaluation's output texture with an upscaler of its own
// (vka_exec.c dlss_eval picks it, DM_VK_DLSS, live):
//   2 = FSR 1: the input colour tonemapped (c / (1 + max(c)), the colour is the scene's linear HDR) into a render-size work image, AMD's EASU to the output size,
//       AMD's RCAS, the tonemap undone. EASU's sampling position follows the frame's sub-pixel jitter (nvnDlssParametersSetJitterOffset; DM_VK_DLSS_UNJITTER,
//       default 1), so a still edge lands on the same output pixels every frame instead of crawling.
//   3 = Snapdragon GSR2 3-pass (the default; BSD-3-Clause shaders in gsr2/, LICENSE there): convert, activate (depth clip, luma history), upscale (history
//       accumulation: the temporal anti-aliasing DLSS did), then RCAS and the tonemap undone. DM_VK_GSR2_CLARITY 0..4 (default 2) picks the row c0..c4 of G_TUNES (c2 is m2),
//       from more history (steadier) to less ghosting; DM_VK_GSR2_TUNE (0..7) forces a row; DM_VK_GSR2_VREJ / _VLO / _VK / _SFCAP / _MVNEAR / _BOXMOTW / _BSMOV override fields.
// Both end in a 1:1 blit of an RGBA16F work image into the output texture. DM_VK_DLSS_SHARP (0..100, default 15) is RCAS's strength; DM_VK_DLSS_JX / _JY and
// DM_VK_GSR2_MV are the jitter's and the motion vectors' signs. Every switch is read at every evaluation; DM_VK_GPUPROF lists the passes.
// 0 = not done (no push descriptors, a pipeline or work image failed, no depth or motion for GSR2, the output format cannot be blitted): the caller's bilinear blit
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_dlss_fsr.h"
#include "gsr2/vka_gsr2_spv.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { P_PREP, P_EASU, P_RCAS, P_GCONV, P_GACT, P_GUP, NPIPE };
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; } wimg;
// GSR2's tunes: motion -> current weight, motion -> box shrink, history weight scale, 3-pass box tolerance, the motion ramp (lo, hi; hi <= lo = off), history
// weight scale on the ramp (-1 = hscale), anti-flicker fade and box shrink on the ramp, Catmull-Rom history (1 everywhere, 2 along the ramp), the cap of the
// still-pixel box size (upstream 20), velocity rejection (strength, threshold and slope in pixels of a 640x360 grid), the motion of the output pixel's render
// texel (1) in place of the bilinear one (0)
typedef struct { const char *name; float motw, boxmotw, hscale, boxtol3, mlo, mhi, hsmov, afmot, bsmov, hcr, sfcap, vrej, vlo, vk, mvnear; } g_tune;
static const g_tune G_TUNES[] = {
    {"m1", 0.05f, 0.12f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 0,     2, 20, 0,    0,     0,     0},
    {"r3", 0.03f, 0.02f, 1.25f, 0.03f, 0,    0,    -1, 0, 0,     0, 20, 0,    0,     0,     0},   // dev A/B only
    {"up", 0.05f, 0.05f, 1.0f,  0.05f, 0,    0,    -1, 0, 0,     0, 20, 0,    0,     0,     0},
    {"c0", 0.05f, 0.15f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 0.5f,  2, 12, 0,    0,     0,     1},   // the clarity steps, DM_VK_GSR2_CLARITY 0..4
    {"c1", 0.05f, 0.20f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 0.75f, 2, 8,  0.25f, 0.5f, 1,     1},
    {"m2", 0.05f, 0.30f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 1,     2, 4,  0.5f, 0.5f,  1,     1},   // c2, the default
    {"c3", 0.05f, 0.50f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 1,     2, 2,  0.9f, 0.5f,  1,     1},
    {"c4", 0.05f, 0.50f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 1,     2, 2,  1.0f, 0.75f, 0.75f, 1},
};
enum { G_NT = sizeof G_TUNES / sizeof G_TUNES[0], G_CLARITY0 = 3 };
static float ovr(float v, float o) { return isnan(o) ? v : o; }
static struct {
    int state; VkDescriptorSetLayout dsl; VkPipelineLayout lay; VkPipeline p[NPIPE]; VkSampler ns, ls; int blit_ok; VkFormat blit_fmt;
    wimg t, e, r, ycc, mda[2], mdc, luma[2], h[2];   // FSR 1: t (render size); both: e (scene colour), r (RCAS); GSR2: ycc, mda, mdc, luma (render size), h (history)
    int m, hc, lmode; uint64_t n; float lsig[20];
} D;

static int dl_ready(void) {
    if (D.state) return D.state > 0;
    D.state = -1;
    if (!V.push_desc) { VRED("DLSS stand-in: no push descriptors: the bilinear blit instead"); return 0; }
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; i++) b[i] = (VkDescriptorSetLayoutBinding){i, i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 6, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &D.dsl);
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &D.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (!r) r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &D.lay);
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
    if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &D.ns);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &D.ls);
    const uint32_t *code[NPIPE] = {vka_dlss_prep, vka_dlss_easu, vka_dlss_rcas, vka_gsr2_convert, vka_gsr2_activate, vka_gsr2_upscale};
    size_t size[NPIPE] = {sizeof vka_dlss_prep, sizeof vka_dlss_easu, sizeof vka_dlss_rcas, sizeof vka_gsr2_convert, sizeof vka_gsr2_activate, sizeof vka_gsr2_upscale};
    for (int k = 0; k < NPIPE && !r; k++) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[k], .pCode = code[k]}; VkShaderModule m = VK_NULL_HANDLE;
        r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = D.lay};
        if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &D.p[k]);
        if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    }
    if (r) { VRED("DLSS stand-in: pipelines %d: the bilinear blit instead", r); return 0; }
    VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { VRED("DLSS stand-in: RGBA16F is no storage image / blit source here: the bilinear blit instead"); return 0; }
    VLOG("DLSS stand-in: FSR 1 and Snapdragon GSR2 3-pass pipelines ready (GSR2: BSD-3-Clause, dm/gfx/vk_android/gsr2/LICENSE)");
    D.state = 1; return 1;
}
static void wimg_free(void *p) { wimg *w = p; vk.vkDestroyImageView(V.dev, w->view, NULL); vk.vkDestroyImage(V.dev, w->img, NULL); vk.vkFreeMemory(V.dev, w->mem, NULL); free(p); }
// An RGBA16F work image of w x h (storage, sampled, blit source, clear target) in GENERAL; a new one is cleared to zeros (a history's first read has weight 0, and 0 * NaN
// is NaN). Returns 2 when it was made now (the history starts again), 1 when it was there, 0 on failure. The old one goes once the GPU is past it
static int wimg_get(VkCommandBuffer cb, wimg *w, uint32_t width, uint32_t height) {
    if (w->img && w->w == width && w->h == height) return 1;
    if (w->img) { wimg *o = malloc(sizeof *o); if (o) { *o = *w; vka_after(vka_fence_last(), wimg_free, o); } }
    memset(w, 0, sizeof *w);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &w->img);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, w->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &w->mem);
    if (!r) r = vk.vkBindImageMemory(V.dev, w->img, w->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = w->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, &w->view);
    if (r) {
        VRED_ONCE("DLSS stand-in: work image %ux%u: %d", width, height, r);
        if (w->view) vk.vkDestroyImageView(V.dev, w->view, NULL);
        if (w->img) vk.vkDestroyImage(V.dev, w->img, NULL);
        if (w->mem) vk.vkFreeMemory(V.dev, w->mem, NULL);
        memset(w, 0, sizeof *w); return 0;
    }
    vka_img_barrier(cb, w->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue z = {{0, 0, 0, 0}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(cb, w->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    w->w = width; w->h = height; return 2;
}
// One compute pass: inputs in[0..3] (sampled, GENERAL; missing ones repeat in[0]) with samplers sm, outputs out[0..1] (storage; a missing second repeats the first)
static void dpass(VkCommandBuffer cb, int pipe, const VkImageView in[4], const VkSampler sm[4], VkImageView o0, VkImageView o1, const void *pc, uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[6]; VkWriteDescriptorSet wr[6];
    for (int i = 0; i < 6; i++) {
        ii[i] = i < 4 ? (VkDescriptorImageInfo){sm[i] ? sm[i] : D.ns, in[i] ? in[i] : in[0], VK_IMAGE_LAYOUT_GENERAL} : (VkDescriptorImageInfo){VK_NULL_HANDLE, i == 4 || !o1 ? o0 : o1, VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                       .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[i]};
    }
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.p[pipe]);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.lay, 0, 6, wr);
    vk.vkCmdPushConstants(cb, D.lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, pc);
    vk.vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    vka_barrier(cb);
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static VkImageView src_view(vka_img *im, uint32_t lv, VkImageAspectFlags asp) {
    if (im->samples > 1 || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return VK_NULL_HANDLE;
    return vka_view(im, im->f.view ? im->f.view : im->f.vk, VK_IMAGE_VIEW_TYPE_2D, asp, lv, 1, 0, 1, NULL);
}

int vka_dlss_run(VkCommandBuffer cb, const p_dlss *o, int mode, vka_img *s, uint32_t sl, int sx, int sy, int iw, int ih, vka_img *d, uint32_t dl, int dx, int dy, int ow, int oh) {
    if (mode < 2 || mode > 3 || !dl_ready()) return 0;
    if (d->f.kind) { VRED_ONCE("DLSS stand-in: an integer or depth output (VkFormat %d): the bilinear path instead", d->f.vk); return 0; }
    if (D.blit_fmt != d->f.vk) {
        VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, d->f.vk, &fp); D.blit_fmt = d->f.vk; D.blit_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        if (!D.blit_ok) VRED("DLSS stand-in: the output's VkFormat %d is no blit destination: the bilinear blit instead", d->f.vk);
    }
    if (!D.blit_ok) return 0;
    VkImageView cv = src_view(s, sl, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!cv) { VRED_ONCE("DLSS stand-in: no sampled view of the input colour (VkFormat %d): the bilinear blit instead", s->f.vk); return 0; }
    float sharp = VKA_ENVF("DM_VK_DLSS_SHARP", 15); sharp = sharp < 0 ? 0 : sharp > 100 ? 100 : sharp; sharp /= 100;
    float jsx = VKA_ENVF("DM_VK_DLSS_JX", 1), jsy = VKA_ENVF("DM_VK_DLSS_JY", 1);
    float jx = o->jitter[0] * jsx, jy = o->jitter[1] * jsy;
    const char *how = "";
    if (mode == 3) {
        const tex_t *dt = o->tex[DLSS_DEPTH], *mt = o->tex[DLSS_MOTION];
        vka_img *di = dt ? vka_tex_get(dt, cb) : NULL, *mi = mt ? vka_tex_get(mt, cb) : NULL;
        uint32_t dlv = 0, mlv = 0;
        VkImageView dv = di && (di->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && vka_img_lv(di, &dlv) ? src_view(di, dlv, VK_IMAGE_ASPECT_DEPTH_BIT) : VK_NULL_HANDLE;
        VkImageView mv = mi && (mi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && vka_img_lv(mi, &mlv) ? src_view(mi, mlv, VK_IMAGE_ASPECT_COLOR_BIT) : VK_NULL_HANDLE;
        if (!dv || !mv) { VRED_ONCE("DLSS stand-in: GSR2 without a sampled depth (%s) or motion (%s) image: FSR 1 instead", dv ? "ok" : "none", mv ? "ok" : "none"); mode = 2; }
        else {
            if (o->base[DLSS_DEPTH][0] || o->base[DLSS_DEPTH][1] || o->base[DLSS_MOTION][0] || o->base[DLSS_MOTION][1]) VRED_ONCE("DLSS stand-in: depth / motion rectangles away from the origin: read from the origin");
            int f = 0, g;
            if (!(g = wimg_get(cb, &D.ycc, iw, ih)) || !(g = wimg_get(cb, &D.mdc, iw, ih))) return 0;
            for (int k = 0; k < 2; k++) {
                if (!(g = wimg_get(cb, &D.mda[k], iw, ih))) return 0; f |= g == 2;
                if (!(g = wimg_get(cb, &D.luma[k], iw, ih))) return 0; f |= g == 2;
                if (!(g = wimg_get(cb, &D.h[k], ow, oh))) return 0; f |= g == 2;
            }
            if (!wimg_get(cb, &D.e, ow, oh) || !wimg_get(cb, &D.r, ow, oh)) return 0;
            if (D.lmode != 3) f = 1;   // a switch back into GSR2 starts the history again
            vka_barrier(cb);
            if (D.lmode != 3) VLOG("DLSS stand-in: GSR2 depth VkFormat %d %ux%u, motion VkFormat %d %ux%u, colour VkFormat %d %ux%u, mv scale %.1f %.1f, flags %#x",
                                    di->f.vk, di->w, di->h, mi->f.vk, mi->w, mi->h, s->f.vk, s->w, s->h, o->mv[0], o->mv[1], o->flags);
            // The tune: DM_VK_GSR2_TUNE when set (0..7, dev A/B), else the clarity step (the launcher's Motion clarity slider, DM_VK_GSR2_CLARITY 0..4, default 2 = m2)
            int ti = VKA_ENV("DM_VK_GSR2_TUNE", -1), cl = VKA_ENV("DM_VK_GSR2_CLARITY", 2); cl = cl < 0 ? 0 : cl > 4 ? 4 : cl;
            if (ti < 0 || ti >= G_NT) ti = G_CLARITY0 + cl;
            g_tune T = G_TUNES[ti];
            T.vrej = ovr(T.vrej, VKA_ENVF("DM_VK_GSR2_VREJ", NAN)); T.vlo = ovr(T.vlo, VKA_ENVF("DM_VK_GSR2_VLO", NAN)); T.vk = ovr(T.vk, VKA_ENVF("DM_VK_GSR2_VK", NAN));
            T.sfcap = ovr(T.sfcap, VKA_ENVF("DM_VK_GSR2_SFCAP", NAN)); T.mvnear = ovr(T.mvnear, VKA_ENVF("DM_VK_GSR2_MVNEAR", NAN));
            T.boxmotw = ovr(T.boxmotw, VKA_ENVF("DM_VK_GSR2_BOXMOTW", NAN)); T.bsmov = ovr(T.bsmov, VKA_ENVF("DM_VK_GSR2_BSMOV", NAN));
            float sg = VKA_ENVF("DM_VK_GSR2_MV", 1), kfov = VKA_ENVF("DM_VK_GSR2_KFOV", 1), minlerp = VKA_ENVF("DM_VK_GSR2_MINLERP", 0.3f), hmin = VKA_ENVF("DM_VK_GSR2_HMIN", 0.1f);
            float hsmov = T.hsmov < 0 ? T.hscale : T.hsmov;
            float sig[20] = {sg, jsx, jsy, kfov, minlerp, hmin, (float)ti, sharp, 0, (float)cl, T.sfcap, T.vrej, T.vlo, T.vk, T.mvnear, T.boxmotw, T.bsmov, 0, 0, 0};
            if (memcmp(sig, D.lsig, sizeof sig) || D.lmode != 3) { memcpy(D.lsig, sig, sizeof sig);
                VLOG("DLSS stand-in: GSR2 3-pass, tune %s (clarity %d; motw %.2f boxmotw %.2f bsmov %.2f hscale %.2f boxtol %.2f ramp %.0f..%.0f hcr %.0f sfcap %.0f vrej %.2f/%.2f/%.2f mvnear %.0f), RCAS %.0f, "
                     "signs: motion %+.0f, jitter x %+.0f y %+.0f; Kfov %.2f, min lerp %.2f, hmin %.2f",
                     T.name, cl, T.motw, T.boxmotw, T.bsmov, T.hscale, T.boxtol3, T.mlo, T.mhi, T.hcr, T.sfcap, T.vrej, T.vlo, T.vk, T.mvnear, sharp * 100, sg, jsx, jsy, kfov, minlerp, hmin); }
            float mvs[2] = {(o->mv[0] ? o->mv[0] : 1.0f) / (float)iw * sg, (o->mv[1] ? o->mv[1] : 1.0f) / (float)ih * sg};
            float rst = (o->reset || f) ? 1.0f : 0.0f;
            struct { float rs[2], rr[2], mvs[2], kfov, reset; int32_t cbase[2]; float vrej, vlo, vk, pad[19]; } pc3 = {{(float)iw, (float)ih}, {1.0f / iw, 1.0f / ih}, {mvs[0], mvs[1]}, kfov, rst, {sx, sy},
                T.vrej, T.vlo, T.vk, {0}};
            int m = D.m ^ 1;
            { VkImageView in[4] = {dv, cv, mv, dv}; VkSampler sm[4] = {D.ns, D.ns, D.ns, D.ns}; dpass(cb, P_GCONV, in, sm, D.ycc.view, D.mda[m].view, &pc3, iw, ih); }
            { VkImageView in[4] = {D.luma[m ^ 1].view, D.mda[m].view, D.ycc.view, D.mda[m ^ 1].view}; VkSampler sm[4] = {D.ns, D.ns, D.ns, D.ns}; dpass(cb, P_GACT, in, sm, D.mdc.view, D.luma[m].view, &pc3, iw, ih); }
            D.m = m;
            int cur = D.hc ^ 1; float area = (float)ow * oh / ((float)iw * ih), sr = area * area * area;
            struct { float rs[2], os[2], rr[2], orr[2], jit[2], scale[2], minlerp, reset; int32_t tm, tap9; float hmin, motw, boxtol, boxmotw, hscale, mlo, mrcp, hsmov, afmot, bsmov, hcr, mvnear, pad[4]; } pc1 = {
                {(float)iw, (float)ih}, {(float)ow, (float)oh}, {1.0f / iw, 1.0f / ih}, {1.0f / ow, 1.0f / oh}, {jx, jy}, {(float)ow / iw, sr < T.sfcap ? sr : T.sfcap}, minlerp, rst, 1, 1,
                hmin, T.motw, T.boxtol3, T.boxmotw, T.hscale, T.mlo, T.mhi > T.mlo ? 1.0f / (T.mhi - T.mlo) : 0.0f, hsmov, T.afmot, T.bsmov, T.hcr, T.mvnear, {0}};
            _Static_assert(sizeof pc1 == 128 && sizeof pc3 == 128, "push constants");
            { VkImageView in[4] = {D.h[D.hc].view, D.mdc.view, D.ycc.view, D.ycc.view}; VkSampler sm[4] = {D.ls, D.ls, D.ns, D.ns}; dpass(cb, P_GUP, in, sm, D.h[cur].view, D.e.view, &pc1, ow, oh); }
            D.hc = cur;
            how = "GSR2 3-pass + RCAS";
        }
    }
    if (mode == 2) {
        if (!wimg_get(cb, &D.t, iw, ih) || !wimg_get(cb, &D.e, ow, oh) || !wimg_get(cb, &D.r, ow, oh)) return 0;
        vka_barrier(cb);
        float un = VKA_ENVF("DM_VK_DLSS_UNJITTER", 1);
        if (D.lmode != 2 || D.lsig[8] != un || D.lsig[7] != sharp || D.lsig[1] != jsx || D.lsig[2] != jsy) { D.lsig[8] = un; D.lsig[7] = sharp; D.lsig[1] = jsx; D.lsig[2] = jsy;
            VLOG("DLSS stand-in: FSR 1 (EASU + RCAS %.0f), un-jitter x%.2f (jitter signs x %+.0f y %+.0f)", sharp * 100, un, jsx, jsy); }
        struct { int32_t size[2], base[2], tm; int32_t pad[27]; } pp = {{iw, ih}, {sx, sy}, 1, {0}};
        { VkImageView in[4] = {cv}; VkSampler sm[4] = {D.ns}; dpass(cb, P_PREP, in, sm, D.t.view, VK_NULL_HANDLE, &pp, iw, ih); }
        // AMD's FsrEasuCon for the whole work image, its sampling position moved by the frame's jitter: the game drew the point that belongs at input position p at
        // p + jitter (GSR2 reads the colour at its output UV + jitter / render size the same way), so output pixel o reads (o + 0.5) * scale - 0.5 + jitter
        float sxr = (float)iw / ow, syr = (float)ih / oh, rw = 1.0f / iw, rh = 1.0f / ih;
        float easu[16] = {sxr, syr, 0.5f * sxr - 0.5f + jx * un, 0.5f * syr - 0.5f + jy * un, rw, rh, rw, -rh, -rw, 2 * rh, rw, 2 * rh, 0, 4 * rh, 0, 0};
        uint32_t pe[32] = {0}; for (int i = 0; i < 16; i++) pe[i] = fbits(easu[i]); pe[16] = (uint32_t)ow; pe[17] = (uint32_t)oh;
        { VkImageView in[4] = {D.t.view}; VkSampler sm[4] = {D.ls}; dpass(cb, P_EASU, in, sm, D.e.view, VK_NULL_HANDLE, pe, ow, oh); }
        how = "FSR 1 EASU + RCAS";
    }
    {   // RCAS on the scene colour (tonemapped), the tonemap undone
        struct { uint32_t con0[4]; int32_t size[2], itm; int32_t pad[25]; } pr = {{fbits(sharp), 0, 0, 0}, {ow, oh}, 1, {0}};
        VkImageView in[4] = {D.e.view}; VkSampler sm[4] = {D.ns}; dpass(cb, P_RCAS, in, sm, D.r.view, VK_NULL_HANDLE, &pr, ow, oh);
    }
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {ow, oh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, dl, 0, 1}, {{dx, dy, 0}, {dx + ow, dy + oh, 1}}};
    vk.vkCmdBlitImage(cb, D.r.img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vka_barrier(cb);
    D.lmode = mode; D.n++;
    if (D.n == 1 || (!(D.n & (D.n - 1)) && D.n <= 4096) || D.n % 18000 == 0)
        VLOG("DLSS stand-in (%s): %dx%d → %dx%d, reset %u, jitter %.3f %.3f (%llu evaluations)", how, iw, ih, ow, oh, o->reset, o->jitter[0], o->jitter[1], (unsigned long long)D.n);
    return mode;
}
#endif
