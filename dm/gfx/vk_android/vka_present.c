// The present: the launcher's ANativeWindow (nxhost_set_window) → VkSurfaceKHR → a FIFO swapchain, and a present thread that blits the game's window texture into it.
// As in nvn_mtl.m, the game's QueuePresentTexture only enqueues (mtl_present returns a ticket); AcquireTexture waits for the ticket of the image it hands out (mtl_present_wait).
// The blit keeps the texture's bytes as they are (8-bit sRGB textures are stored as UNORM, vka_tex.c) into an R8G8B8A8_UNORM swapchain, the way the Metal layer is BGRA8Unorm,
// letterboxed to the window (or stretched over it: DM_VK_STRETCH, vka_present_fit.h), flipped when the window origin is lower left. Pre-rotation: when the surface's currentTransform is a rotation (a phone held in landscape whose panel
// is portrait), the swapchain takes it as its preTransform and the panel's own orientation, and a small draw (vka_present_rot.h) copies the picture rotated, so the compositor
// shows the image as it is instead of rotating every frame; a change of rotation (the phone turned over) recreates the swapchain. DM_VK_PRETRANSFORM=0 leaves the rotation to
// the compositor. Pacing: FIFO, and with VK_GOOGLE_display_timing each image is shown for at least its present period (a 30 fps game on a 60 Hz panel, a 60 fps game on a
// 120 Hz one); the window also gets the game's frame rate (ANativeWindow_setFrameRate), so the system may run the panel at it. The same extension gives the presented times
// hle_nvn.c's frame statistics read (mtl_presented_time)
#if defined(__ANDROID__)
#include "vka.h"
#include <stdio.h>
#include "vk_android.h"
#include "../nvn/nvn_mtl.h"
#include "vka_present_rot.h"
#include "vka_present_fsr.h"
#if __has_include("game_profile.h")
#include "game_profile.h"
#endif
#include "vka_present_fit.h"
#include "vka_ovl.h"
#include <android/native_window.h>
#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

__attribute__((weak)) void dm_guest_frame_tick(void) {}

// ---------- The window ----------
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pcv = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_win_new; static uint64_t g_win_gen, g_win_ack;   // set by nxhost_set_window; the present thread takes it and acknowledges the generation
static int g_thread_up;
static void *selftest_main(void *arg);
void nxhost_set_window(ANativeWindow *w) {
    static int first = 1;
    if (w) ANativeWindow_acquire(w);
    vka_init();
    pthread_mutex_lock(&g_pmu);
    if (g_win_new) ANativeWindow_release(g_win_new);   // never taken by the present thread
    g_win_new = w; uint64_t gen = ++g_win_gen;
    pthread_cond_broadcast(&g_pcv);
    if (!w && g_thread_up) {   // surfaceDestroyed: the swapchain must be gone before the launcher returns
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 2;
        while (g_win_ack < gen) if (pthread_cond_timedwait(&g_pcv, &g_pmu, &dl)) { VRED("the present thread did not let the window go within 2 s"); break; }
    }
    pthread_mutex_unlock(&g_pmu);
    VLOG("window %p%s", (void *)w, w ? "" : " (none: presents are dropped)");
    const char *e = getenv("NXHOST_VK_SELFTEST");
    if (first && w && e && atoi(e) > 0) { first = 0; pthread_t th; if (!pthread_create(&th, NULL, selftest_main, (void *)(intptr_t)atoi(e))) pthread_detach(th); }
    if (w) first = 0;
}

// ---------- Swapchain ----------
enum { NFRAME = 3, MAXIMG = 8, PQ = 8, PRING = 512 };
static ANativeWindow *g_win; static VkSurfaceKHR g_surf; static VkSwapchainKHR g_swap; static VkFormat g_swap_fmt; static VkExtent2D g_ext;
static VkImage g_simg[MAXIMG]; static VkSemaphore g_sdone[MAXIMG]; static uint32_t g_nimg;
static struct { VkCommandBuffer cb; VkFence fence; VkSemaphore acq; vka_img *held; int used; } g_fr[NFRAME];
static VkCommandPool g_ppool; static unsigned g_fi;
static uint64_t g_refresh_ns, g_last_desired, g_drawable_ns;
static int g_recreate;
static struct { uint64_t n; double t; } g_pres[PRING];
static VkSurfaceTransformFlagBitsKHR g_pre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;   // the swapchain's preTransform
static VkImageView g_sview[MAXIMG];   // the swapchain images as colour attachments (pre-rotation, FSR, the overlay)
static int g_sview_fsr;   // this swapchain made its views for FSR: a missing view then failed, and a new swapchain would not have it either
static int g_fps_set; static unsigned g_refresh_n;
static _Atomic int g_place_x = 5000, g_place_y = 5000;   // nxhost_picture_place, in 1/10000 of the free space
void nxhost_picture_place(float x, float y) {
    x = x < 0 ? 0 : x > 1 ? 1 : x; y = y < 0 ? 0 : y > 1 ? 1 : y;
    g_place_x = (int)(x * 10000 + 0.5f); g_place_y = (int)(y * 10000 + 0.5f);
}

// ---------- Pre-rotation: the rotated copy into the swapchain image ----------
#define ROT_FNS(X) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)
static struct { ROT_FNS(VKA_DECL) } rk;   // loaded here: the replay's table (vka.h) does not need them
static struct { int state; VkFormat fmt; VkPipeline pipe; VkPipelineLayout lay; VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set[NFRAME]; VkSampler samp[2]; } g_rot;
static int rot_rotates(VkSurfaceTransformFlagBitsKHR t) { return t == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || t == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR || t == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR; }
static int rot_quarter(VkSurfaceTransformFlagBitsKHR t) { return t == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || t == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR; }
static int stretch_wanted(void) { return VKA_ENV("DM_VK_STRETCH", 0) != 0; }   // Game settings › Display: Stretch to screen
static int picture_size(void) { return vka_fit_size(VKA_ENV("DM_VK_PICTURE_SIZE", 100)); }   // the in-game panel's Picture size, % of the fitted (or stretched) size
static int rot_wanted(void) { return VKA_ENV("DM_VK_PRETRANSFORM", 1) != 0; }   // live: a change makes a new swapchain (present_one)
// rk's functions, loaded once for the rotated copy and the FSR passes: 1 = loaded
static int rk_load(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
#define ROT_LOAD(f) rk.f = (PFN_##f)vk.vkGetDeviceProcAddr(V.dev, #f); if (!rk.f) { VRED("present: no %s", #f); return ok = 0; }
    ROT_FNS(ROT_LOAD)
#undef ROT_LOAD
    // The compiling calls through the replay's table: its wrappers compile with the host's FPCR on any thread, an inline present's on the game's included (vka_dev.c)
    if (vk.vkCreateShaderModule) rk.vkCreateShaderModule = vk.vkCreateShaderModule;
    if (vk.vkCreateGraphicsPipelines) rk.vkCreateGraphicsPipelines = vk.vkCreateGraphicsPipelines;
    return ok = 1;
}
// The rotated copy's pipeline for a swapchain format (g_rot's layout): blend 0 writes the picture as it is, 1 draws a premultiplied source over the image (the overlay)
static VkResult rot_pipeline(VkFormat fmt, int blend, VkPipeline *out) {
    VkShaderModule sm[2] = {0};
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_rot_vert, .pCode = vka_rot_vert}, mf = mv;
    mf.codeSize = sizeof vka_rot_frag; mf.pCode = vka_rot_frag;
    VkResult r = rk.vkCreateShaderModule(V.dev, &mv, NULL, &sm[0]);
    if (!r) r = rk.vkCreateShaderModule(V.dev, &mf, NULL, &sm[1]);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = sm[0], .pName = "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = sm[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
    if (blend) cba = (VkPipelineColorBlendAttachmentState){VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xf};   // premultiplied source over (the overlay, ovl_draw)
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_rot.lay};
    if (!r) r = rk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gi, NULL, out);
    for (int i = 0; i < 2; i++) if (sm[i]) rk.vkDestroyShaderModule(V.dev, sm[i], NULL);
    return r;
}
// The pipeline for one swapchain format: 1 = ready. A failure is logged once and leaves the rotation to the compositor
static int rot_ready(VkFormat fmt) {
    if (g_rot.state > 0 && g_rot.fmt == fmt) return 1;
    if (g_rot.state < 0) return 0;
    if (!g_rot.state) {
        if (!rk_load()) { g_rot.state = -1; return 0; }
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 6 * sizeof(float)};
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NFRAME};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NFRAME, .poolSizeCount = 1, .pPoolSizes = &ps};
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_rot.dsl);
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_rot.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        if (!r) r = rk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_rot.lay);
        if (!r) r = vk.vkCreateDescriptorPool(V.dev, &pi, NULL, &g_rot.pool);
        VkDescriptorSetLayout dsls[NFRAME]; for (int i = 0; i < NFRAME; i++) dsls[i] = g_rot.dsl;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_rot.pool, .descriptorSetCount = NFRAME, .pSetLayouts = dsls};
        if (!r) r = vk.vkAllocateDescriptorSets(V.dev, &ai, g_rot.set);
        if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_rot.samp[0]);   // nearest
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_rot.samp[1]);
        if (r) { VRED("pre-rotation: descriptor set, layout or sampler: %d", r); g_rot.state = -1; return 0; }
    }
    if (g_rot.pipe) { vka_queue_idle(); rk.vkDestroyPipeline(V.dev, g_rot.pipe, NULL); g_rot.pipe = VK_NULL_HANDLE; }   // a new swapchain format: no copy in flight
    VkResult r = rot_pipeline(fmt, 0, &g_rot.pipe);
    if (r) { VRED("pre-rotation: pipeline for format %d: %d; the compositor rotates", fmt, r); g_rot.state = -1; return 0; }
    g_rot.state = 1; g_rot.fmt = fmt;
    return 1;
}
static VkFormat raw_unorm_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
int vka_swap_rb(void) {
    const char *e = getenv("DM_VK_SWAP_RB");
    if (e && *e) return atoi(e);
#if defined(DM_GAME_SWAP_RB)
    return DM_GAME_SWAP_RB;
#else
    return 0;
#endif
}
// The letterboxed picture [dx, dx + dw) x [dy, dy + dh) of the display (what the user sees, g_ext turned back by g_pre) from the texture's crop, drawn into swapchain image idx
// turned by g_pre: the preTransform is the turn applied to the content before presentation, so ROTATE_90 draws the picture a quarter turn clockwise, and the display's
// (u, v) lands on the image's (s, t) = (1 - v, u); ROTATE_180 on (1 - u, 1 - v), ROTATE_270 on (v, 1 - u). The vertex shader maps the viewport's own (0..1) position to
// texture coordinates with m and t
static void rot_copy(VkCommandBuffer cb, unsigned k, uint32_t idx, vka_img *im, int ok, int cx, int cy, int cw, int ch, int dx, int dy, int dw, int dh, int linear) {
    if (!rot_ready(g_swap_fmt)) return;
    vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_sview[idx], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {{0, 0, 0, 1}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_ext}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk.vkCmdBeginRendering(cb, &ri);
    static const VkComponentMapping swz_rb = {
        .r = VK_COMPONENT_SWIZZLE_B,
        .g = VK_COMPONENT_SWIZZLE_G,
        .b = VK_COMPONENT_SWIZZLE_R,
        .a = VK_COMPONENT_SWIZZLE_A
    };
    VkImageView view = ok ? vka_view(im, raw_unorm_twin(im->f.vk), VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, vka_swap_rb() ? &swz_rb : NULL) : VK_NULL_HANDLE;
    if (view) {
        VkDescriptorImageInfo ii = {g_rot.samp[linear && !VKA_ENV("DM_VK_PRESENT_NEAREST", 0)], view, VK_IMAGE_LAYOUT_GENERAL};   // DM_VK_PRESENT_NEAREST: the launcher's Upscaler, nearest neighbor
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_rot.set[k], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
        vk.vkUpdateDescriptorSets(V.dev, 1, &w, 0, NULL);
        float W = (float)g_ext.width, H = (float)g_ext.height;   // the image; the display is H x W for a quarter turn
        VkViewport v = {0, 0, 0, 0, 0, 1};
        float a, b, c, d;   // display-local (ul, vl) = (a px + b py + e, c px + d py + f) for the viewport's own (px, py)
        float e, f;
        switch (g_pre) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  v.x = W - (float)(dy + dh); v.y = (float)dx; v.width = (float)dh; v.height = (float)dw; a = 0; b = 1; e = 0; c = -1; d = 0; f = 1; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: v.x = (float)dy; v.y = H - (float)(dx + dw); v.width = (float)dh; v.height = (float)dw; a = 0; b = -1; e = 1; c = 1; d = 0; f = 0; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: v.x = W - (float)(dx + dw); v.y = H - (float)(dy + dh); v.width = (float)dw; v.height = (float)dh; a = -1; b = 0; e = 1; c = 0; d = -1; f = 1; break;
        default:                                      v.x = (float)dx; v.y = (float)dy; v.width = (float)dw; v.height = (float)dh; a = 1; b = 0; e = 0; c = 0; d = 1; f = 0; break;
        }
        // texture (u, v) = (cx + ul cw, cy + vl ch) / size, vl turned over for a lower-left origin (stored bottom-up)
        float tw = (float)im->w, th = (float)im->h, su = cw / tw, sv = ch / th, ou = cx / tw, ov = cy / th;
        if (vka_origin_ll()) { sv = -sv; ov = (cy + ch) / th; }
        float pc[6] = {su * a, su * b, sv * c, sv * d, ou + su * e, ov + sv * f};
        VkRect2D sc = {{0, 0}, g_ext};
        rk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_rot.pipe);
        rk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_rot.lay, 0, 1, &g_rot.set[k], 0, NULL);
        rk.vkCmdPushConstants(cb, g_rot.lay, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        rk.vkCmdSetViewport(cb, 0, 1, &v);
        rk.vkCmdSetScissor(cb, 0, 1, &sc);
        rk.vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vk.vkCmdEndRendering(cb);
    vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

// ---------- FSR 1 upscale: EASU into an intermediate image, then RCAS into the swapchain image (vka_present_fsr.h) ----------
// DM_VK_FSR=1 replaces the bilinear blit, or the rotated copy, when the picture is scaled up; DM_VK_FSR_SHARP is RCAS's sharpness in stops (0 = the most,
// each stop halves it; AMD's default 0.25). Both are live. The intermediate image has the picture's size on the window, one per frame in flight
static struct { int state; VkFormat fmt; VkPipeline easu, rcas, easu_sc; VkPipelineLayout lay; VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set[NFRAME][2]; VkSampler samp;
                VkImage mid[NFRAME]; VkDeviceMemory mem[NFRAME]; VkImageView view[NFRAME]; uint32_t mw[NFRAME], mh[NFRAME]; } g_fsr;
static int fsr_wanted(void) { return VKA_ENV("DM_VK_FSR", 0) != 0; }
static VkPipeline fsr_pipe(const uint32_t *frag, size_t fsize, VkFormat fmt) {
    VkShaderModule sm[2] = {0}; VkPipeline p = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_fsr_vert, .pCode = vka_fsr_vert}, mf = mv;
    mf.codeSize = fsize; mf.pCode = frag;
    VkResult r = rk.vkCreateShaderModule(V.dev, &mv, NULL, &sm[0]);
    if (!r) r = rk.vkCreateShaderModule(V.dev, &mf, NULL, &sm[1]);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = sm[0], .pName = "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = sm[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_fsr.lay};
    if (!r) r = rk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
    for (int i = 0; i < 2; i++) if (sm[i]) rk.vkDestroyShaderModule(V.dev, sm[i], NULL);
    if (r) VRED("FSR: pipeline for format %d: %d", fmt, r);
    return r ? VK_NULL_HANDLE : p;
}
// The layout, sets, sampler and the EASU pipeline once, the RCAS pipeline per swapchain format: 1 = ready. A failure is logged once and leaves the bilinear blit
static int fsr_ready(VkFormat fmt) {
    if (g_fsr.state > 0 && g_fsr.fmt == fmt) return 1;
    if (g_fsr.state < 0) return 0;
    if (!g_fsr.state) {
        if (!rk_load()) { g_fsr.state = -1; return 0; }
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * sizeof(uint32_t)};   // the shaders' P: EASU's four uvec4 constants; RCAS's con0 and its turn
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * NFRAME};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * NFRAME, .poolSizeCount = 1, .pPoolSizes = &ps};
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_fsr.dsl);
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_fsr.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        if (!r) r = rk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_fsr.lay);
        if (!r) r = vk.vkCreateDescriptorPool(V.dev, &pi, NULL, &g_fsr.pool);
        VkDescriptorSetLayout dsls[2 * NFRAME]; for (int i = 0; i < 2 * NFRAME; i++) dsls[i] = g_fsr.dsl;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_fsr.pool, .descriptorSetCount = 2 * NFRAME, .pSetLayouts = dsls};
        if (!r) r = vk.vkAllocateDescriptorSets(V.dev, &ai, &g_fsr.set[0][0]);
        if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_fsr.samp);
        if (r) { VRED("FSR: descriptor set, layout or sampler: %d; the picture is blitted", r); g_fsr.state = -1; return 0; }
        if (!(g_fsr.easu = fsr_pipe(vka_fsr_easu_frag, sizeof vka_fsr_easu_frag, VK_FORMAT_R8G8B8A8_UNORM))) { g_fsr.state = -1; return 0; }
    }
    if (g_fsr.rcas) { vka_queue_idle(); rk.vkDestroyPipeline(V.dev, g_fsr.rcas, NULL); g_fsr.rcas = VK_NULL_HANDLE; }   // a new swapchain format: no pass in flight
    if (g_fsr.easu_sc) { rk.vkDestroyPipeline(V.dev, g_fsr.easu_sc, NULL); g_fsr.easu_sc = VK_NULL_HANDLE; }
    if (!(g_fsr.rcas = fsr_pipe(vka_fsr_rcas_frag, sizeof vka_fsr_rcas_frag, fmt))) { g_fsr.state = -1; return 0; }
    g_fsr.easu_sc = fsr_pipe(vka_fsr_easu_frag, sizeof vka_fsr_easu_frag, fmt);   // EASU straight into the swapchain image (sharpness off, no turn); NULL: through RCAS as before
    g_fsr.state = 1; g_fsr.fmt = fmt;
    VLOG("FSR 1 upscale on (RCAS sharpness %.2f stops)", VKA_ENVF("DM_VK_FSR_SHARP", 0.25));
    return 1;
}
// Frame k's intermediate image freed: only frame k's submissions use it, so the caller has waited for frame k's fence (or the queue idle). Its descriptor set
// g_fsr.set[k][1] is written again before its next use
static void fsr_free(unsigned k) {
    if (g_fsr.view[k]) vk.vkDestroyImageView(V.dev, g_fsr.view[k], NULL);
    if (g_fsr.mid[k]) vk.vkDestroyImage(V.dev, g_fsr.mid[k], NULL);
    if (g_fsr.mem[k]) vk.vkFreeMemory(V.dev, g_fsr.mem[k], NULL);
    g_fsr.view[k] = VK_NULL_HANDLE; g_fsr.mid[k] = VK_NULL_HANDLE; g_fsr.mem[k] = VK_NULL_HANDLE; g_fsr.mw[k] = g_fsr.mh[k] = 0;
}
// Frame k's intermediate image at w x h: made again when the size changes (frame k's fence is waited for, so nothing reads the old one). 1 = ready
static int fsr_mid(unsigned k, uint32_t w, uint32_t h) {
    if (g_fsr.mid[k] && g_fsr.mw[k] == w && g_fsr.mh[k] == h) return 1;
    fsr_free(k);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &g_fsr.mid[k]);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, g_fsr.mid[k], &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &g_fsr.mem[k]);
    if (!r) r = vk.vkBindImageMemory(V.dev, g_fsr.mid[k], g_fsr.mem[k], 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_fsr.mid[k], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, &g_fsr.view[k]);
    if (r) { VRED_ONCE("FSR: intermediate image %ux%u: %d; the picture is blitted", w, h, r); return 0; }
    g_fsr.mw[k] = w; g_fsr.mh[k] = h;
    return 1;
}
static uint32_t fsr_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
// The texture's crop [cx, cx + cw) x [cy, cy + ch) scaled to [dx, dx + dw) x [dy, dy + dh) of swapchain image idx, the rest black. The constants are AMD's FsrEasuConOffset and
// FsrRcasCon (ffx_fsr1.h) in C; for a lower-left origin (the texture stored bottom-up) EASU's rows are read from the bottom: con0's y scale negated and its offset from the crop's top.
// 0 = nothing recorded (the texture has no view to sample): the caller blits
static int fsr_copy(VkCommandBuffer cb, unsigned k, uint32_t idx, vka_img *im, int cx, int cy, int cw, int ch, int dx, int dy, int dw, int dh) {
    VkImageView src = vka_view(im, raw_unorm_twin(im->f.vk), VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, NULL);
    if (!src) { VRED_ONCE("FSR: window texture serial %llu (format %#x) has no view to sample: blitted", (unsigned long long)im->serial, im->d.format); return 0; }
    vka_barrier(cb);   // the replay's writes to the window texture (earlier submissions, in submission order)
    VkDescriptorImageInfo ii[2] = {{g_fsr.samp, src, VK_IMAGE_LAYOUT_GENERAL}, {g_fsr.samp, g_fsr.view[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_fsr.set[k][0], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_fsr.set[k][1], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
    vk.vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL);
    float sx = (float)cw / dw, sy = (float)ch / dh, rw = 1.0f / im->w, rh = 1.0f / im->h;
    float easu[16] = {sx, sy, 0.5f * sx - 0.5f + cx, 0.5f * sy - 0.5f + cy, rw, rh, rw, -rh, -rw, 2 * rh, rw, 2 * rh, 0, 4 * rh, 0, 0};
    if (vka_origin_ll()) { easu[1] = -sy; easu[3] = cy + ch - 0.5f * sy - 0.5f; }
    uint32_t pc[16];
    for (int i = 0; i < 16; i++) pc[i] = fsr_bits(easu[i]);
    // Sharpening off (DM_VK_FSR_SHARP below 0) and no turn: EASU draws straight into the letterbox of the swapchain image, no intermediate image and no RCAS pass. Its fragment
    // coordinates then start at the letterbox corner, so the constants' offsets move by (dx, dy) in output pixels
    if (VKA_ENVF("DM_VK_FSR_SHARP", 0.25) < 0 && g_fsr.easu_sc && (g_pre == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR || !g_pre)) {
        float e2 = easu[2] - dx * easu[0], e3 = easu[3] - dy * easu[1]; pc[2] = fsr_bits(e2); pc[3] = fsr_bits(e3);
        VkViewport vd = {(float)dx, (float)dy, (float)dw, (float)dh, 0, 1}; VkRect2D sd = {{0, 0}, g_ext};
        vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo cd = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_sview[idx], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {{0, 0, 0, 1}}}};   // the bars
        VkRenderingInfo rd = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sd, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &cd};
        vk.vkCmdBeginRendering(cb, &rd);
        rk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fsr.easu_sc);
        rk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fsr.lay, 0, 1, &g_fsr.set[k][0], 0, NULL);
        rk.vkCmdPushConstants(cb, g_fsr.lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        rk.vkCmdSetViewport(cb, 0, 1, &vd);
        rk.vkCmdSetScissor(cb, 0, 1, &sd);
        rk.vkCmdDraw(cb, 3, 1, 0, 0);
        vk.vkCmdEndRendering(cb);
        vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        return 1;
    }
    VkViewport v = {0, 0, (float)dw, (float)dh, 0, 1}; VkRect2D sc = {{0, 0}, {(uint32_t)dw, (uint32_t)dh}};
    vka_img_barrier(cb, g_fsr.mid[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_fsr.view[k], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk.vkCmdBeginRendering(cb, &ri);
    rk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fsr.easu);
    rk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fsr.lay, 0, 1, &g_fsr.set[k][0], 0, NULL);
    rk.vkCmdPushConstants(cb, g_fsr.lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    rk.vkCmdSetViewport(cb, 0, 1, &v);
    rk.vkCmdSetScissor(cb, 0, 1, &sc);
    rk.vkCmdDraw(cb, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cb);
    vka_img_barrier(cb, g_fsr.mid[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // RCAS: con0 = FsrRcasCon's (its packed half at word 1 is for the 16-bit path, unused here); words 4-9 turn the swapchain pixel q back to the picture's,
    // q.x * (4, 5) + q.y * (6, 7) + (8, 9): rot_copy's turns in whole pixels (ROTATE_90: the picture's (x, y) = (q.y - dx, W - 1 - dy - q.x)), the viewport as rot_copy's
    int W = (int)g_ext.width, H = (int)g_ext.height, m[6];
    switch (g_pre) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  v = (VkViewport){(float)(W - dy - dh), (float)dx, (float)dh, (float)dw, 0, 1}; memcpy(m, (int[6]){0, -1, 1, 0, -dx, W - 1 - dy}, sizeof m); break;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: v = (VkViewport){(float)dy, (float)(H - dx - dw), (float)dh, (float)dw, 0, 1}; memcpy(m, (int[6]){0, 1, -1, 0, H - 1 - dx, -dy}, sizeof m); break;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: v = (VkViewport){(float)(W - dx - dw), (float)(H - dy - dh), (float)dw, (float)dh, 0, 1}; memcpy(m, (int[6]){-1, 0, 0, -1, W - 1 - dx, H - 1 - dy}, sizeof m); break;
    default:                                      v = (VkViewport){(float)dx, (float)dy, (float)dw, (float)dh, 0, 1}; memcpy(m, (int[6]){1, 0, 0, 1, -dx, -dy}, sizeof m); break;
    }
    memset(pc, 0, sizeof pc);
    { float st = (float)VKA_ENVF("DM_VK_FSR_SHARP", 0.25);   // below 0: no sharpening at all (RCAS's strength 0 leaves every pixel as EASU made it; the pass still places and turns it)
      pc[0] = fsr_bits(st < 0 ? 0.0f : exp2f(-st)); }
    memcpy(&pc[4], m, sizeof m);
    sc = (VkRect2D){{0, 0}, g_ext};
    vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ca = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_sview[idx], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {{0, 0, 0, 1}}}};   // the bars
    ri.renderArea = sc;
    vk.vkCmdBeginRendering(cb, &ri);
    rk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fsr.rcas);
    rk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fsr.lay, 0, 1, &g_fsr.set[k][1], 0, NULL);
    rk.vkCmdPushConstants(cb, g_fsr.lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    rk.vkCmdSetViewport(cb, 0, 1, &v);
    rk.vkCmdSetScissor(cb, 0, 1, &sc);
    rk.vkCmdDraw(cb, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cb);
    vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    return 1;
}

// ---------- The launcher's overlay, drawn into the swapchain image ----------
// The launcher draws the overlay's text blocks (build tag, frame rate and the other items: Overlay.java) into bitmaps and hands them over (nxhost_overlay_set); each present
// draws them over the picture, in their corners, with the rotated copy's shaders and a premultiplied blend (ovl_draw). As Android views over the SurfaceView they made the
// app's window draw a new buffer on the GPU at every change and the compositor take a second changing layer (issue #34: about 30 fps with the overlay off, less with it on);
// here the window stays still and the overlay costs one small draw in the present's own submission. DM_VK_OVERLAY=0 (live) leaves the overlay to the launcher's views again
enum { OVL_SLOTS = 2 };   // 0 the top-left block (the build tag), 1 the top-right one (the frame rate and the other items)
static pthread_mutex_t g_omu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint8_t *px; int w, h, cap; } g_oin[OVL_SLOTS];   // the blocks as set (under g_omu): tightly packed RGBA8, premultiplied
static uint64_t g_ogen;                                           // bumped by every set (under g_omu)
static _Atomic int g_ostate;                                      // 1 = drawn into an image, 0 = not yet, -1 = cannot (a resource or the pipeline failed)
static const int g_ocorner[OVL_SLOTS] = {VKO_TOP_LEFT, VKO_TOP_RIGHT};
static struct {
    VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; int fresh;     // the overlay image: the blocks one below the other (rows y[s]..)
    VkBuffer buf[NFRAME]; VkDeviceMemory bmem[NFRAME]; void *map[NFRAME]; VkDeviceSize bcap;   // frame k's upload
    VkPipeline pipe; VkFormat fmt; VkDescriptorPool pool; VkDescriptorSet set;
    uint64_t gen; int w_[OVL_SLOTS], h_[OVL_SLOTS], y[OVL_SLOTS];               // what the image holds
} g_ovl;
static int ovl_wanted(void) { return VKA_ENV("DM_VK_OVERLAY", 1) != 0; }

int nxhost_overlay_set(int slot, const void *px, int w, int h, int stride) {
    if (slot < 0 || slot >= OVL_SLOTS) return 0;
    if (!px || w <= 0 || h <= 0 || stride < w * 4 || w > 4096 || h > 4096) w = h = 0;
    pthread_mutex_lock(&g_omu);
    int ok = 1;
    if (w && (size_t)w * h * 4 > (size_t)g_oin[slot].cap) {
        uint8_t *p = realloc(g_oin[slot].px, (size_t)w * h * 4);
        if (p) { g_oin[slot].px = p; g_oin[slot].cap = w * h * 4; } else { w = h = 0; ok = 0; }
    }
    for (int y = 0; y < h; y++) memcpy(g_oin[slot].px + (size_t)y * w * 4, (const uint8_t *)px + (size_t)y * stride, (size_t)w * 4);
    g_oin[slot].w = w; g_oin[slot].h = h; g_ogen++;
    pthread_mutex_unlock(&g_omu);
    return ok && nxhost_overlay_state() >= 0;
}
int nxhost_overlay_state(void) { return !ovl_wanted() ? -1 : atomic_load(&g_ostate); }

static void ovl_fail(const char *what, VkResult r) { VRED("overlay: %s: %d; the launcher draws it", what, r); atomic_store(&g_ostate, -1); }
static void ovl_free_image(void) {
    if (g_ovl.view) vk.vkDestroyImageView(V.dev, g_ovl.view, NULL);
    if (g_ovl.img) vk.vkDestroyImage(V.dev, g_ovl.img, NULL);
    if (g_ovl.mem) vk.vkFreeMemory(V.dev, g_ovl.mem, NULL);
    for (int k = 0; k < NFRAME; k++) {
        if (g_ovl.buf[k]) vk.vkDestroyBuffer(V.dev, g_ovl.buf[k], NULL);
        if (g_ovl.bmem[k]) vk.vkFreeMemory(V.dev, g_ovl.bmem[k], NULL);   // unmapped with it
        g_ovl.buf[k] = VK_NULL_HANDLE; g_ovl.bmem[k] = VK_NULL_HANDLE; g_ovl.map[k] = NULL;
    }
    g_ovl.view = VK_NULL_HANDLE; g_ovl.img = VK_NULL_HANDLE; g_ovl.mem = VK_NULL_HANDLE; g_ovl.w = g_ovl.h = 0; g_ovl.bcap = 0;
}
// The image (and the uploads) at least w x h, rounded up so a block a few pixels wider does not make them again (which waits for the queue): 1 = ready
static int ovl_image(uint32_t w, uint32_t h) {
    if (g_ovl.img && g_ovl.w >= w && g_ovl.h >= h) return 1;
    w = (w + 63) & ~63u; h = (h + 63) & ~63u;
    if (g_ovl.w > w) w = g_ovl.w;
    if (g_ovl.h > h) h = g_ovl.h;
    if (g_ovl.img) vka_queue_idle();   // earlier presents still read the old image
    ovl_free_image();
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &g_ovl.img);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, g_ovl.img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &g_ovl.mem);
    if (!r) r = vk.vkBindImageMemory(V.dev, g_ovl.img, g_ovl.mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_ovl.img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, &g_ovl.view);
    VkDeviceSize bsz = (VkDeviceSize)w * h * 4;
    for (int k = 0; k < NFRAME && !r; k++) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bsz, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        r = vk.vkCreateBuffer(V.dev, &bi, NULL, &g_ovl.buf[k]);
        VkMemoryRequirements br = {0}; if (!r) vk.vkGetBufferMemoryRequirements(V.dev, g_ovl.buf[k], &br);
        VkMemoryAllocateInfo ba = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size,
            .memoryTypeIndex = vka_mem_type(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        if (!r && ba.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (!r) r = vk.vkAllocateMemory(V.dev, &ba, NULL, &g_ovl.bmem[k]);
        if (!r) r = vk.vkBindBufferMemory(V.dev, g_ovl.buf[k], g_ovl.bmem[k], 0);
        if (!r) r = vk.vkMapMemory(V.dev, g_ovl.bmem[k], 0, VK_WHOLE_SIZE, 0, &g_ovl.map[k]);
    }
    if (!r && !g_ovl.pool) {
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
        r = vk.vkCreateDescriptorPool(V.dev, &pi, NULL, &g_ovl.pool);
        VkDescriptorSetAllocateInfo sa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_ovl.pool, .descriptorSetCount = 1, .pSetLayouts = &g_rot.dsl};
        if (!r) r = vk.vkAllocateDescriptorSets(V.dev, &sa, &g_ovl.set);
    }
    if (r) { ovl_free_image(); ovl_fail("image or upload buffer", r); return 0; }
    VkDescriptorImageInfo ii = {g_rot.samp[0], g_ovl.view, VK_IMAGE_LAYOUT_GENERAL};   // nearest: the blocks are drawn pixel for pixel
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ovl.set, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vk.vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);   // the queue is idle, or the set was never used
    g_ovl.w = w; g_ovl.h = h; g_ovl.bcap = bsz; g_ovl.fresh = 1; g_ovl.gen = 0;
    return 1;
}
// Frame k: the blocks set since the last upload into the overlay image (frame k's upload buffer is free: its fence was waited for), then each block drawn over swapchain
// image idx (in PRESENT_SRC, the picture in it) in its corner of the display. Nothing at all while no block is set
static void ovl_draw(VkCommandBuffer cb, unsigned k, uint32_t idx) {
    if (!ovl_wanted() || atomic_load(&g_ostate) < 0) return;
    pthread_mutex_lock(&g_omu);
    uint64_t gen = g_ogen; int need_w = 0, need_h = 0, any = 0;
    for (int s = 0; s < OVL_SLOTS; s++) { if (g_oin[s].w > need_w) need_w = g_oin[s].w; need_h += g_oin[s].h; any |= g_oin[s].w > 0; }
    pthread_mutex_unlock(&g_omu);
    if (!any) { g_ovl.gen = gen; for (int s = 0; s < OVL_SLOTS; s++) g_ovl.w_[s] = 0; return; }   // the overlay off: no pass at all
    if (!g_sview[idx]) { ovl_fail("no attachment view of the swapchain image", VK_ERROR_INITIALIZATION_FAILED); return; }
    if (!rot_ready(g_swap_fmt)) { ovl_fail("the rotated copy's layout", VK_ERROR_INITIALIZATION_FAILED); return; }
    if (!g_ovl.pipe || g_ovl.fmt != g_swap_fmt) {
        if (g_ovl.pipe) { vka_queue_idle(); rk.vkDestroyPipeline(V.dev, g_ovl.pipe, NULL); g_ovl.pipe = VK_NULL_HANDLE; }
        VkResult r = rot_pipeline(g_swap_fmt, 1, &g_ovl.pipe);
        if (r) { g_ovl.pipe = VK_NULL_HANDLE; ovl_fail("pipeline", r); return; }
        g_ovl.fmt = g_swap_fmt;
    }
    if (gen != g_ovl.gen) {   // the new blocks: packed one below the other into frame k's buffer, copied into the image
        if (!ovl_image((uint32_t)need_w, (uint32_t)need_h)) return;
        pthread_mutex_lock(&g_omu);
        int y = 0, fits = 1; VkBufferImageCopy rg[OVL_SLOTS]; uint32_t nrg = 0; VkDeviceSize off = 0;
        for (int s = 0; s < OVL_SLOTS; s++) {
            int w = g_oin[s].w, h = g_oin[s].h;
            if ((uint32_t)w > g_ovl.w || (uint32_t)(y + h) > g_ovl.h) { fits = 0; break; }   // set again, larger, since: the next present
            g_ovl.w_[s] = w; g_ovl.h_[s] = h; g_ovl.y[s] = y;
            if (w > 0) {
                memcpy((uint8_t *)g_ovl.map[k] + off, g_oin[s].px, (size_t)w * h * 4);
                rg[nrg++] = (VkBufferImageCopy){off, (uint32_t)w, (uint32_t)h, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, y, 0}, {(uint32_t)w, (uint32_t)h, 1}};
                off += (VkDeviceSize)w * h * 4;
            }
            y += h;
        }
        if (fits) gen = g_ogen;
        pthread_mutex_unlock(&g_omu);
        if (!fits) { for (int s = 0; s < OVL_SLOTS; s++) g_ovl.w_[s] = 0; return; }
        vka_img_barrier(cb, g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, g_ovl.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);   // earlier presents' reads
        g_ovl.fresh = 0;
        vk.vkCmdCopyBufferToImage(cb, g_ovl.buf[k], g_ovl.img, VK_IMAGE_LAYOUT_GENERAL, nrg, rg);
        vka_barrier(cb);
        g_ovl.gen = gen;
    }
    if (!g_ovl.img) return;
    int ew = (int)(rot_quarter(g_pre) ? g_ext.height : g_ext.width), eh = (int)(rot_quarter(g_pre) ? g_ext.width : g_ext.height);   // the display, as the user sees it
    vko_draw d[OVL_SLOTS]; int nd = 0, x0 = INT32_MAX, y0 = INT32_MAX, x1 = 0, y1 = 0;
    for (int s = 0; s < OVL_SLOTS; s++)
        if (vko_place((int)g_pre, (int)g_ext.width, (int)g_ext.height, ew, eh, g_ocorner[s], g_ovl.w_[s], g_ovl.h_[s], (int)g_ovl.w, (int)g_ovl.h, g_ovl.y[s], &d[nd])) {
            if (d[nd].x < x0) x0 = d[nd].x;
            if (d[nd].y < y0) y0 = d[nd].y;
            if (d[nd].x + d[nd].w > x1) x1 = d[nd].x + d[nd].w;
            if (d[nd].y + d[nd].h > y1) y1 = d[nd].y + d[nd].h;
            nd++;
        }
    if (!nd) return;
    vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_sview[idx], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk.vkCmdBeginRendering(cb, &ri);
    rk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ovl.pipe);
    rk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_rot.lay, 0, 1, &g_ovl.set, 0, NULL);
    for (int i = 0; i < nd; i++) {
        VkViewport v = {(float)d[i].x, (float)d[i].y, (float)d[i].w, (float)d[i].h, 0, 1};
        VkRect2D sc = {{d[i].x, d[i].y}, {(uint32_t)d[i].w, (uint32_t)d[i].h}};
        rk.vkCmdPushConstants(cb, g_rot.lay, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof d[i].pc, d[i].pc);
        rk.vkCmdSetViewport(cb, 0, 1, &v);
        rk.vkCmdSetScissor(cb, 0, 1, &sc);
        rk.vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vk.vkCmdEndRendering(cb);
    vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (atomic_load(&g_ostate) == 0) { atomic_store(&g_ostate, 1); VLOG("overlay: drawn by the present (DM_VK_OVERLAY=0: the launcher's views)"); }
}

static void swap_destroy(int surface_too) {
    if (!V.dev) return;
    vka_queue_idle();
    if (surface_too) for (unsigned k = 0; k < NFRAME; k++) fsr_free(k);   // the window is gone (the queue is idle); a new one makes them at its own size
    for (int i = 0; i < NFRAME; i++) if (g_fr[i].held) { vka_tex_drop(g_fr[i].held); g_fr[i].held = NULL; }
    for (uint32_t i = 0; i < g_nimg; i++) {
        vk.vkDestroySemaphore(V.dev, g_sdone[i], NULL); g_sdone[i] = VK_NULL_HANDLE;
        if (g_sview[i]) { vk.vkDestroyImageView(V.dev, g_sview[i], NULL); g_sview[i] = VK_NULL_HANDLE; }
    }
    g_nimg = 0;
    if (g_swap) vk.vkDestroySwapchainKHR(V.dev, g_swap, NULL);
    g_swap = VK_NULL_HANDLE;
    if (surface_too && g_surf) { vk.vkDestroySurfaceKHR(V.inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
}
static int swap_create(void) {
    VkSurfaceCapabilitiesKHR caps; VkResult r = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, g_surf, &caps);
    if (r) { VRED("surface capabilities %d", r); return 0; }
    uint32_t nf = 0; vk.vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fs[64]; if (nf > 64) nf = 64; vk.vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, g_surf, &nf, fs);
    VkSurfaceFormatKHR pick = nf ? fs[0] : (VkSurfaceFormatKHR){VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (int pass = 0; pass < 2; pass++) for (uint32_t i = 0; i < nf; i++)
        if (fs[i].format == (pass ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM) && fs[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = fs[i]; pass = 2; break; }
    int rot = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) ext = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(g_win), (uint32_t)ANativeWindow_getHeight(g_win)};
    VkSurfaceTransformFlagBitsKHR pre = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Pre-rotation: the surface's own rotation, when the swapchain can take it and the rotated copy is ready
    if (rot_rotates(caps.currentTransform) && (caps.supportedTransforms & caps.currentTransform) && (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        && rot_wanted() && rot_ready(pick.format)) pre = caps.currentTransform;
    // The extent: the window's own size (the picture as the user sees it), turned for a quarter-turn preTransform. currentExtent is in the panel's orientation on phones but
    // already in the window's on the emulator (gfxstream), so the window's size decides, and currentExtent is only the fallback
    int ww = ANativeWindow_getWidth(g_win), wh = ANativeWindow_getHeight(g_win);
    if (ww > 0 && wh > 0) ext = rot_quarter(pre) ? (VkExtent2D){(uint32_t)wh, (uint32_t)ww} : (VkExtent2D){(uint32_t)ww, (uint32_t)wh};
    else if (rot && !rot_quarter(pre)) ext = (VkExtent2D){ext.height, ext.width};
    if (!ext.width || !ext.height) return 0;
    uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount; if (n > MAXIMG) n = MAXIMG;
    VkCompositeAlphaFlagBitsKHR ca = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
                                     caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkSwapchainKHR old = g_swap;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = n, .imageFormat = pick.format, .imageColorSpace = pick.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1, .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = pre, .compositeAlpha = ca, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old};
    if (old) { vka_queue_idle(); }
    VkSwapchainKHR sw; r = vk.vkCreateSwapchainKHR(V.dev, &ci, NULL, &sw);
    if (old) {
        for (uint32_t i = 0; i < g_nimg; i++) { vk.vkDestroySemaphore(V.dev, g_sdone[i], NULL); if (g_sview[i]) vk.vkDestroyImageView(V.dev, g_sview[i], NULL); g_sview[i] = VK_NULL_HANDLE; }
        g_nimg = 0; vk.vkDestroySwapchainKHR(V.dev, old, NULL); g_swap = VK_NULL_HANDLE;
    }
    if (r) { VRED("vkCreateSwapchainKHR %ux%u format %d: %d", ext.width, ext.height, pick.format, r); return 0; }
    g_swap = sw; g_swap_fmt = pick.format; g_ext = ext; g_pre = pre; g_sview_fsr = fsr_wanted();
    g_nimg = MAXIMG; vk.vkGetSwapchainImagesKHR(V.dev, sw, &g_nimg, g_simg);
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (rot_rotates(pre) || vka_swap_rb()) rot_ready(pick.format);
    for (uint32_t i = 0; i < g_nimg; i++) {
        vk.vkCreateSemaphore(V.dev, &sci, NULL, &g_sdone[i]);
        {   // the rotated copy, the FSR passes and the overlay (ovl_draw) draw into the swapchain image
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_simg[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = pick.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if ((r = vk.vkCreateImageView(V.dev, &vi, NULL, &g_sview[i]))) { VRED("swapchain image view %u: %d", i, r); g_sview[i] = VK_NULL_HANDLE; }
        }
    }
    g_refresh_ns = 0;
    if (V.display_timing && vk.vkGetRefreshCycleDurationGOOGLE) { VkRefreshCycleDurationGOOGLE rc; if (!vk.vkGetRefreshCycleDurationGOOGLE(V.dev, sw, &rc)) g_refresh_ns = rc.refreshDuration; }
    g_last_desired = 0; g_recreate = 0;
    g_refresh_n = 0;
    VLOG("swapchain %ux%u, %u images, format %d, FIFO, transform %#x (surface %#x%s), refresh %.2f Hz%s", ext.width, ext.height, g_nimg, pick.format, pre, caps.currentTransform,
         rot_rotates(pre) ? ": pre-rotated" : rot_rotates(caps.currentTransform) ? ": the compositor rotates" : "", g_refresh_ns ? 1e9 / g_refresh_ns : 0.0,
         V.display_timing ? ", display timing" : "");
    return 1;
}
static void window_take(void) {   // g_pmu held
    ANativeWindow *w = g_win_new; uint64_t gen = g_win_gen; g_win_new = NULL;
    pthread_mutex_unlock(&g_pmu);
    swap_destroy(1);
    if (g_win) ANativeWindow_release(g_win);
    g_win = w; g_fps_set = 0;
    if (w && V.has_android_surface) {
        VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = w};
        VkResult r = vk.vkCreateAndroidSurfaceKHR(V.inst, &si, NULL, &g_surf); VkBool32 ok = VK_FALSE;
        if (!r) vk.vkGetPhysicalDeviceSurfaceSupportKHR(V.pd, V.qf, g_surf, &ok);
        if (r || !ok) { VRED("surface on window %p: %d, present support %d", (void *)w, r, ok); if (!r) vk.vkDestroySurfaceKHR(V.inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
        else swap_create();
    }
    pthread_mutex_lock(&g_pmu);
    g_win_ack = gen; pthread_cond_broadcast(&g_pcv);
}

// ---------- The present queue ----------
typedef struct { tex_t t; double period; int crop[4], has_crop; uint64_t ticket, fence; vka_img *src; double hold; } pq_item;   // src, hold: a generated frame (vka_framegen.c)
static pq_item g_pq[PQ];
static uint64_t g_pq_in, g_pq_read, g_pq_done, g_pq_fill;   // tickets: reserved, taken by the present thread, shown; filled (a ticket is reserved by the game's present, filled when the replay reaches it: vka_worker.c)

static void pres_note(uint64_t n, double t) { if (!n) return; g_pres[n % PRING].n = n; g_pres[n % PRING].t = t; }
static void timing_poll(void) {
    if (!V.display_timing || !g_swap || !vk.vkGetPastPresentationTimingGOOGLE) return;
    VkPastPresentationTimingGOOGLE pt[16]; uint32_t n = 16;
    if (vk.vkGetPastPresentationTimingGOOGLE(V.dev, g_swap, &n, pt) < 0) return;
    for (uint32_t i = 0; i < n; i++) pres_note(pt[i].presentID, pt[i].actualPresentTime / 1e9);
}

// VK_SUBOPTIMAL_KHR: recreate when the window has a new size, or the surface a new rotation that pre-rotation can follow (the phone turned over: ROTATE_90 ↔ ROTATE_270).
// Without pre-rotation, a rotation alone stays: Android reports every frame suboptimal then, and the compositor rotates
static void suboptimal(void) {
    if (!g_win || g_recreate) return;
    uint32_t ww = (uint32_t)ANativeWindow_getWidth(g_win), wh = (uint32_t)ANativeWindow_getHeight(g_win);
    uint32_t ew = rot_quarter(g_pre) ? g_ext.height : g_ext.width, eh = rot_quarter(g_pre) ? g_ext.width : g_ext.height;
    if ((ww != ew || wh != eh) && (ww != eh || wh != ew)) { g_recreate = 1; return; }
    if (g_rot.state < 0 || !rot_wanted()) return;
    VkSurfaceCapabilitiesKHR caps;
    if (!vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, g_surf, &caps) && caps.currentTransform != g_pre && (rot_rotates(caps.currentTransform) || rot_rotates(g_pre))) {
        VLOG("surface rotation %#x → %#x: new swapchain", g_pre, caps.currentTransform);
        g_recreate = 1;
    }
}
// The game's frame rate as the window's (ANativeWindow_setFrameRate): a 60 fps game on a 120 Hz panel lets the system pick 60 Hz, or keep 120 Hz and show each frame twice;
// display timing holds each image for its period either way. The refresh period is read again after a change (the panel's rate may follow).
// The window's rate is the highest the game used in the last DM_VK_FRAME_RATE_HOLD ms (default 5000, live; 0 = follow every change): up at once, down only when the lower
// rate has held that long. Donkey Kong Bananza's controller switches its interval 1 <-> 2 every few seconds on the phone (60 <-> 30 fps); following each switch sent the
// Honor panel (LTPO) to 30 Hz and back: 24-46 changes to 30 Hz in a 3-minute run (stutter-agent base1 / base2), and the first frames at 60 after each were shown at 30 Hz
// until the panel came back up, a hitch at every switch. With the panel held at 60 Hz a 30 fps stretch is shown by display timing, each image for two refreshes
static void frame_rate(double period) {
    int fps = period > 0 ? (int)(1 / period + 0.5) : 0;
    if (fps < 10 || fps > 240 || !g_win) return;
    if (!VKA_ENV("DM_VK_FRAME_RATE", 1)) return;
    {   static int top; static uint64_t top_t;   // the highest rate within the hold; when the game last ran at it
        uint64_t now = now_ns(), hold = (uint64_t)VKA_ENV("DM_VK_FRAME_RATE_HOLD", 5000) * 1000000ull;
        if (fps >= top || !hold || now - top_t > hold) { top = fps; top_t = now; }
        fps = top;
    }
    if (fps == g_fps_set) return;
    static int (*set_rate)(ANativeWindow *, float, int8_t); static int looked;   // libnativewindow (API 30), not linked: -landroid does not carry it
    if (!looked) { looked = 1; void *h = dlopen("libnativewindow.so", RTLD_NOW); if (h) set_rate = (int (*)(ANativeWindow *, float, int8_t))dlsym(h, "ANativeWindow_setFrameRate"); }
    int r = set_rate ? set_rate(g_win, (float)fps, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) : -1;
    VLOG("window frame rate %d fps%s", fps, r ? " (not taken)" : "");
    g_fps_set = fps; g_refresh_n = 1;   // the panel may switch within a few frames
}
void vka_present_rates(int *fps, double *hz) { if (fps) *fps = g_fps_set; if (hz) *hz = g_refresh_ns ? 1e9 / (double)g_refresh_ns : 0; }   // the pace line (vka_perf.c)
static int g_fg_on;   // frame generation on (present thread): its generated frames follow the panel's refresh (vka_framegen.c), so the refresh is read on while it runs
static void refresh_poll(void) {
    if (!V.display_timing || !vk.vkGetRefreshCycleDurationGOOGLE) return;
    // After a window frame rate change every 30 presents until 300; with frame generation on also every 240 after that (the panel may change on its own: LTPO, thermal)
    static unsigned slow;
    if (g_refresh_n ? ++g_refresh_n % 30 : !g_fg_on || ++slow % 240) return;
    VkRefreshCycleDurationGOOGLE rc;
    // A reading within 1 % of the last is the same refresh (the driver's estimate drifts by 0.01-0.02 Hz: 60.22 / 60.23 / 60.24 Hz): taken without a line, and the chain of
    // desired present times goes on; before 2026-11-03 each drift logged a line and restarted the chain (one frame paced from "now" instead of its slot)
    if (!vk.vkGetRefreshCycleDurationGOOGLE(V.dev, g_swap, &rc) && rc.refreshDuration && rc.refreshDuration != g_refresh_ns) {
        uint64_t a = rc.refreshDuration, b = g_refresh_ns, d = a > b ? a - b : b - a;
        if (b && d * 100 < b) g_refresh_ns = a;
        else { VLOG("display refresh %.2f Hz", 1e9 / rc.refreshDuration); g_refresh_ns = rc.refreshDuration; g_last_desired = 0; }
    }
    if (g_refresh_n > 300) g_refresh_n = 0;   // settled
}
static int present_one(const pq_item *it) {
    if (!g_surf || vka_hung()) return 0;
    { static int rw = -1; int w = rot_wanted(); if (rw >= 0 && w != rw) { VLOG("DM_VK_PRETRANSFORM=%d: new swapchain", w); g_recreate = 1; } rw = w; }
    if (!g_swap || g_recreate) { if (g_swap) { swap_destroy(0); } if (!swap_create()) return 0; }
    vka_img *im = it->src ? vka_tex_ref(it->src) : vka_tex_acquire(it->t.serial);
    unsigned k = g_fi++ % NFRAME;
    if (!g_fr[k].cb) {
        if (!g_ppool) { VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf}; vk.vkCreateCommandPool(V.dev, &pi, NULL, &g_ppool); }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_ppool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk.vkAllocateCommandBuffers(V.dev, &ai, &g_fr[k].cb); vk.vkCreateFence(V.dev, &fi, NULL, &g_fr[k].fence); vk.vkCreateSemaphore(V.dev, &sci, NULL, &g_fr[k].acq);
    }
    // The frame's last copy done, bounded: after a GPU hang (vka_dev.c) nothing more is presented
    for (VkResult w; !vka_hung() && (w = vk.vkWaitForFences(V.dev, 1, &g_fr[k].fence, VK_TRUE, 100000000ull)) == VK_TIMEOUT;) {}
    if (vka_hung()) { VRED_ONCE("GPU hang: nothing more is presented"); vka_tex_drop(im); return 0; }
    if (g_fr[k].held) { vka_tex_drop(g_fr[k].held); g_fr[k].held = NULL; }
    if (g_fsr.mid[k] && !fsr_wanted()) fsr_free(k);   // FSR switched off live: frame k's fence is signalled, so its intermediate image is no longer read
    uint32_t idx; uint64_t t0 = now_ns();
    VkResult r = vk.vkAcquireNextImageKHR(V.dev, g_swap, 1000000000ull, g_fr[k].acq, VK_NULL_HANDLE, &idx);
    g_drawable_ns += now_ns() - t0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { g_recreate = 1; vka_tex_drop(im); return 0; }
    if (r == VK_SUBOPTIMAL_KHR) suboptimal();   // presented this once; the next present recreates when the size or the rotation changed
    if (r && r != VK_SUBOPTIMAL_KHR) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("vkAcquireNextImageKHR %d", r); vka_tex_drop(im); return 0; }
    vk.vkResetFences(V.dev, 1, &g_fr[k].fence);
    VkCommandBuffer cb = g_fr[k].cb; vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    int cx = 0, cy = 0, cw = im ? (int)im->w : 1, ch = im ? (int)im->h : 1;
    if (it->has_crop && it->crop[2] > 0 && it->crop[3] > 0) { cx = it->crop[0]; cy = it->crop[1]; cw = it->crop[2]; ch = it->crop[3]; }
    int ew = (int)(rot_quarter(g_pre) ? g_ext.height : g_ext.width), eh = (int)(rot_quarter(g_pre) ? g_ext.width : g_ext.height);   // the display, as the user sees it
    // Fit (keep aspect) or stretch to the whole display (DM_VK_STRETCH), at DM_VK_PICTURE_SIZE % of that, placed by nxhost_picture_place; all live (the in-game panel
    // sends the switches through DM_CTL and gives the launcher's touch mapping the same rectangle, PictureFit.rect)
    int fr[4]; vka_fit_rect(ew, eh, cw, ch, stretch_wanted(), picture_size(), g_place_x, g_place_y, fr);
    int dx = fr[0], dy = fr[1], dw = fr[2], dh = fr[3];
    static VkFormat s_last_fmt; static VkFormatProperties s_last_fp;
    VkFormatProperties fp = {0};
    if (im) {
        if (im->f.vk != s_last_fmt) { vk.vkGetPhysicalDeviceFormatProperties(V.pd, im->f.vk, &s_last_fp); s_last_fmt = im->f.vk; }
        fp = s_last_fp;
    }
    int blit = im && im->inited && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && im->f.kind == 0 && im->samples == 1 && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
    // FSR in place of the blit or the rotated copy for a picture scaled up; switched on live, the swapchain has no attachment views yet: blitted once, and the next swapchain has them.
    // A view that failed on a swapchain made for FSR is not asked for again: each present would make a new swapchain (a queue idle) and fail the same way
    int fsr = blit > 0 && fsr_wanted() && (dw > cw || dh > ch) && (im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)
              && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (!fsr && fsr_wanted() && im) { static int told; if (!told++) VLOG("FSR wanted but not used: blit %d, picture %dx%d -> %dx%d, usage %#x, linear filter %d", blit, cw, ch, dw, dh,
                                                                   (unsigned)im->usage, (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0); }
    if (fsr && !g_sview[idx]) {
        if (!g_sview_fsr) g_recreate = 1;
        else VRED_ONCE("FSR: swapchain image %u has no attachment view: blitted", idx);
        fsr = 0;
    }
    if (fsr && fsr_ready(g_swap_fmt) && fsr_mid(k, (uint32_t)dw, (uint32_t)dh) && fsr_copy(cb, k, idx, im, cx, cy, cw, ch, dx, dy, dw, dh)) {
        blit = -1;
    } else if (rot_rotates(g_pre) || vka_swap_rb()) {
        int ok = im && im->inited && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && im->f.kind == 0 && im->samples == 1 && (im->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && g_sview[idx];
        if (ok) vka_barrier(cb);   // the replay's writes to the window texture (earlier submissions, in submission order)
        else if (im) VRED_ONCE("window texture serial %llu (format %#x) cannot be sampled for the rotated copy: presented black", (unsigned long long)im->serial, im->d.format);
        rot_copy(cb, k, idx, im, ok, cx, cy, cw, ch, dx, dy, dw, dh, !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
        blit = -1;
    } else vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (blit < 0) {}
    else if (!blit || dw < ew || dh < eh) {   // the bars: dx or dy is 0 for a picture against an edge
        VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.vkCmdClearColorImage(cb, g_simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
        vka_barrier(cb);
    }
    if (blit > 0) {
        vka_barrier(cb);   // the replay's writes to the window texture (earlier submissions, in submission order)
        int y0 = cy, y1 = cy + ch; if (vka_origin_ll()) { y0 = cy + ch; y1 = cy; }   // lower-left origin: the window texture is stored bottom-up
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{cx, y0, 0}, {cx + cw, y1, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        vk.vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g_simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                          (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && !VKA_ENV("DM_VK_PRESENT_NEAREST", 0) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else if (im && !blit) VRED_ONCE("window texture serial %llu (format %#x) cannot be blitted to the swapchain: presented black", (unsigned long long)im->serial, im->d.format);
    if (blit >= 0) vka_img_barrier(cb, g_simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    ovl_draw(cb, k, idx);   // the launcher's overlay over the picture (generated frames too)
    if (!it->src) vka_ctl_shot_record(cb, im, cx, cy, cw, ch);   // the game's own frames only
    vk.vkEndCommandBuffer(cb);
    VkSemaphore ws[2] = {g_fr[k].acq, V.tl}; uint64_t wv[2] = {0, it->fence}; VkPipelineStageFlags st[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};   // the image's first barrier is ALL_COMMANDS
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 2, .pWaitSemaphoreValues = wv, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = wv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .waitSemaphoreCount = it->fence ? 2 : 1, .pWaitSemaphores = ws, .pWaitDstStageMask = st,
                       .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_sdone[idx]};
    if (!it->fence) ti.waitSemaphoreValueCount = 1;
    frame_rate(it->period); refresh_poll();
    uint64_t want = 0, period = (uint64_t)((it->hold > 0 ? it->hold : it->period) * 1e9);
    // With frame generation every image gets a time too, at least one refresh after the one before: a game frame and its generated frames are handed over back to back,
    // and with their hold under 1.5 refreshes (4x at 25 fps: 10 ms) they went out untimed, two landed in one refresh and the compositor dropped one (on the Portal
    // about 48 of 105 a second, mostly generated frames, so the screen showed little more than the game's own rate)
    if (g_fg_on && g_refresh_ns && period < g_refresh_ns) period = g_refresh_ns;
    if (g_refresh_ns && (period > g_refresh_ns * 3 / 2 || g_fg_on)) {   // hold each image for its present period: the panel refreshes faster than the game presents
        uint64_t now = now_ns(); want = g_last_desired ? g_last_desired + period : now;
        if (want < now) want = now;
        g_last_desired = want; want -= g_refresh_ns / 2;
    } else g_last_desired = 0;
    VkPresentTimeGOOGLE pt = {(uint32_t)it->ticket, want};
    VkPresentTimesInfoGOOGLE ptv = {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &pt};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = V.display_timing ? &ptv : NULL, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_sdone[idx], .swapchainCount = 1, .pSwapchains = &g_swap, .pImageIndices = &idx};
    // The frame done on the GPU before the queue lock is taken: a submission that waits for a timeline value still pending can block inside vkQueueSubmit until it is
    // signalled (Turnip on KGSL emulates timeline semaphores: 25-35 ms a frame with the lock held, so every replay submission behind it waited; Ocarina of Time's note).
    // The copy cannot start before the frame ends anyway; only a value already handed to the queue is waited for here (a later one is handled under the lock below)
    // Test switch DM_VK_PRESENT_PREWAIT=0: no wait here (as before), for A/B runs
    if (it->fence && !vka_hung() && VKA_ENV("DM_VK_PRESENT_PREWAIT", 1) && it->fence <= __atomic_load_n(&V.tl_queued, __ATOMIC_ACQUIRE)) {
        VkResult vka_tl_wait(VkSemaphore s, uint64_t v, uint64_t timeout_ns); vka_tl_wait(V.tl, it->fence, vka_hang_ns());   // a timeout: the submission below waits as before, and the hang handling sees it
    }
    pthread_mutex_lock(&V.qmu);
    // Never a wait for a value no earlier submission signals: the queue runs its batches in order, so the copy would wait for a submission queued behind it, and neither ever
    // runs (mtl_present takes vka_fence_queued for that; this holds it under the queue lock)
    if (wv[1] > V.tl_queued) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("present ticket %llu waited for fence %llu, %llu is the last handed to the queue: waits for that", (unsigned long long)it->ticket, (unsigned long long)wv[1], (unsigned long long)V.tl_queued); wv[1] = V.tl_queued; }
    // Test switch DM_VK_TEST_HANG=<mode>,<ticket>: at that present the copy waits for a timeline value no earlier batch signals, as the present did before it took
    // vka_fence_queued. Mode 1: the next fence (a later submission signals it: the wait-before-signal on one queue); mode 2: a value nothing ever signals (a sure stall, for
    // the hang handling). Not set in the product
    { static int mode = -1, at; if (mode < 0) { const char *e = getenv("DM_VK_TEST_HANG"); mode = 0; if (e && sscanf(e, "%d,%d", &mode, &at) < 1) mode = 0; if (at <= 0) at = 600; }
      if (mode && it->ticket == (uint64_t)at && it->fence) { wv[1] = mode == 1 ? vka_fence_last() + 1 : V.tl_queued + 1000000000ull;
          VRED("DM_VK_TEST_HANG=%d: present ticket %llu waits for fence %llu (%llu handed to the queue)", mode, (unsigned long long)it->ticket, (unsigned long long)wv[1], (unsigned long long)V.tl_queued); } }
    void vka_qhold_add(int k, uint64_t ns); uint64_t th0 = now_ns();
    r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, g_fr[k].fence);
    uint64_t th1 = now_ns();
    VkResult pr = r ? r : vk.vkQueuePresentKHR(V.q, &pi);
    vka_qhold_add(0, th1 - th0); vka_qhold_add(1, now_ns() - th1);
    pthread_mutex_unlock(&V.qmu);
    g_fr[k].held = im;
    if (!r) vka_ctl_shot_done(g_fr[k].fence);
    if (r == VK_ERROR_DEVICE_LOST || pr == VK_ERROR_DEVICE_LOST) vka_device_lost(r ? "the present's submission" : "vkQueuePresentKHR");
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("present submission %d", r); return 0; }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) g_recreate = 1;
    else if (pr == VK_SUBOPTIMAL_KHR) suboptimal();
    else if (pr) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("vkQueuePresentKHR %d", pr); }
    timing_poll();
    return 1;
}

void vka_perf_thread(int nice); void vka_perf_frame(double period_s);   // vka_perf.c
static void *present_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-present"); vka_fp_host("vk-present"); vka_perf_thread(-8);
    pthread_mutex_lock(&g_pmu);
    for (;;) {
        while (g_pq_read == g_pq_fill && g_win_ack == g_win_gen) pthread_cond_wait(&g_pcv, &g_pmu);
        if (g_win_ack != g_win_gen) { window_take(); continue; }
        pq_item it = g_pq[g_pq_read % PQ]; g_pq_read++;
        pthread_mutex_unlock(&g_pmu);
        vka_img *gen[VKA_FG_MAX]; double hold = 0, out_period = 0;   // frame generation: the images shown before this frame, each for hold seconds
        int ng = g_surf ? vka_fg_frame(it.t.serial, it.has_crop ? it.crop : NULL, it.period, it.fence, gen, &hold, &out_period) : 0;
        double game_period = it.period;
        g_fg_on = out_period > 0;
        if (out_period > 0) it.period = out_period;
        for (int g = 0; g < ng; g++) { pq_item gi = it; gi.src = gen[g]; gi.has_crop = 0; gi.ticket = 0; gi.fence = 0; gi.hold = hold; vka_fps_note(present_one(&gi), 1); }
        it.hold = ng ? hold : game_period;
        int shown = present_one(&it);
        if (shown) dm_guest_frame_tick();
        if (!shown || !V.display_timing) pres_note(it.ticket, shown ? now_ns() / 1e9 : -1);
        vka_fps_note(shown, 0);
        pthread_mutex_lock(&g_pmu);
        if (it.ticket > g_pq_done) g_pq_done = it.ticket;
        pthread_cond_broadcast(&g_pcv);
    }
    return NULL;
}
void vka_present_init(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, present_main, NULL)) { VRED("cannot start the present thread"); return; }
    pthread_detach(th);
    pthread_mutex_lock(&g_pmu); g_thread_up = 1; pthread_mutex_unlock(&g_pmu);
}

// The present in two steps: the ticket when the game presents (vka_present_reserve, the game's thread: its order and the bound on frames in flight), the frame when the replay
// has handed every submission before it to the queue (vka_present_fill, the replay's thread). Without the replay worker both run at once (mtl_present)
uint64_t vka_present_reserve(void) {
    pthread_mutex_lock(&g_pmu);
    if (!g_thread_up) { uint64_t tk = ++g_pq_in; g_pq_read = g_pq_done = g_pq_fill = tk; pres_note(tk, -1); pthread_mutex_unlock(&g_pmu); return tk; }
    while (g_pq_in - g_pq_read >= PQ) pthread_cond_wait(&g_pcv, &g_pmu);
    uint64_t tk = ++g_pq_in;
    pthread_mutex_unlock(&g_pmu);
    return tk;
}
void vka_present_fill(uint64_t tk, const tex_t *t, double period_s, const int crop[4]) {
    vka_perf_frame(period_s);
    pthread_mutex_lock(&g_pmu);
    if (!g_thread_up || tk <= g_pq_fill) { pthread_mutex_unlock(&g_pmu); return; }
    pq_item *it = &g_pq[(tk - 1) % PQ];
    it->t = *t; it->period = period_s; it->has_crop = crop != NULL; if (crop) memcpy(it->crop, crop, sizeof it->crop);
    // The last submission handed to the queue, not the last fence reserved: a submission another thread is still recording (its fence taken at the top of mtl_sub_commit) would
    // be queued after this copy, which then waits for it forever (a wait-before-signal on one queue). On the Adreno 840 phone that froze the queue while the first level loaded
    // (fence N+2 never reached at N, android-perf c1ff15fa, three runs), and the compositor waiting on the frame restarted the system server
    it->ticket = tk; it->fence = vka_fence_queued(); it->src = NULL; it->hold = 0;
    uint64_t res = vka_fence_last();
    if (res > it->fence && !vka_worker_on()) { static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
        if (k <= 3 || !(k & (k - 1))) VLOG("present ticket %llu came while fence %llu was still being recorded (%llu queued): the present waits for %llu (%llu such presents)",
                                           (unsigned long long)tk, (unsigned long long)res, (unsigned long long)it->fence, (unsigned long long)it->fence, (unsigned long long)k); }
    g_pq_fill = tk;
    pthread_cond_broadcast(&g_pcv);
    pthread_mutex_unlock(&g_pmu);
}
uint64_t vka_present_inline(const tex_t *t, double period_s, const int crop[4]) { uint64_t tk = vka_present_reserve(); vka_present_fill(tk, t, period_s, crop); return tk; }
void mtl_present_wait(uint64_t tk) { pthread_mutex_lock(&g_pmu); while (g_pq_done < tk) pthread_cond_wait(&g_pcv, &g_pmu); pthread_mutex_unlock(&g_pmu); }
void mtl_present_flush(void) { pthread_mutex_lock(&g_pmu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pmu); mtl_present_wait(h); }
// The acquire sync of a window image (hle_nvn.c WindowAcquireTexture): the fence of the commits that drew it when it was last presented, not every commit so far. By the time
// the acquire returns, the present of that image has been handed to the queue (mtl_present_wait of its ticket), its copy waiting for that fence; the game's later
// submissions come after it on the one queue and begin with a full barrier, so they cannot write the image before the copy has read it. Waiting for every commit made the
// game wait for its own last frame at each acquire: Donkey Kong Bananza's level blocked ~64 ms a frame in SyncWait, recording (~45 ms) and GPU (~30 ms) one after the other
uint64_t mtl_acquire_fence(uint64_t presented) { return presented; }
double mtl_presented_time(uint64_t n) { return g_pres[n % PRING].n == n ? g_pres[n % PRING].t : 0; }
int mtl_has_window(void) { return g_surf != VK_NULL_HANDLE; }
int mtl_display_hz(void) { return !g_surf ? 0 : g_refresh_ns ? (int)(1e9 / g_refresh_ns + 0.5) : 60; }
uint64_t vka_refresh_ns(void) { return g_refresh_ns; }   // the panel's refresh period (display timing; 0 = not known). Present thread
int mtl_fps_target(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_FPS"); v = e && (atoi(e) == 30 || atoi(e) == 60) ? atoi(e) : 60; } return v; }
uint64_t mtl_drawable_ns_take(void) { uint64_t v = g_drawable_ns; g_drawable_ns = 0; return v; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
// No display link on Android: hle_nvn.c paces acquires by its own clock, and FIFO plus display timing hold each image on screen
int mtl_link_active(void) { return 0; }
int mtl_link_rate(void) { return 0; }
int mtl_link_stalled(void) { return 0; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; if (target) *target = 0; if (lead_ms) *lead_ms = 0; if (ticks) *ticks = 0; if (gap_ms) *gap_ms = 0; return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }

static void *selftest_main(void *arg) { vka_fp_host("vk-selftest"); int r = nxhost_vk_selftest(NULL, (int)(intptr_t)arg); VLOG("selftest (NXHOST_VK_SELFTEST) ended: %s", r ? "FAILED" : "passed"); return NULL; }
#endif
