// Frame generation in the present (Lossless Scaling's LSFG, ported from Eden in dm/gfx/vk_android/lsfg, GPL-3.0-or-later): the present thread hands each game frame here
// before it presents it (vka_fg_frame, one call in vka_present.c present_main). The frame's picture (the window texture's crop) is copied into the chain's input, the shared
// passes run, and the generated frames between the previous game frame and this one are written to images of their own, which the present shows before the game frame,
// each held for an equal share of the game's measured frame interval (display timing). The work is one command buffer per game frame on the replay's queue: it waits for the
// frame's own timeline value as the present does, and the presents after it read the outputs in queue order behind a full barrier. Off by default; the user's own
// Lossless.dll (the launcher finds it and passes DM_LSFG_DLL) is read once. Without NX_LSFG (a build without the GPL folder) every call reports "not in this build".
// Settings: DM_LSFG=1, DM_LSFG_MULT=2..4, DM_LSFG_TARGET=0|fps, DM_LSFG_FLOW=0 (auto: the optical flow at the game's own resolution) or 25..100 (%), changed while running by
// nxhost_framegen_set (the launcher's in-game settings)
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef NX_LSFG
#include "lsfg/lsfg.h"
#endif

static _Atomic int g_on = -1, g_mult = 2, g_target, g_flow = 75, g_state;   // g_state: 0 off, 1 generating, 2 on and not generating, -1 unavailable
static char g_reason[256] = "off";
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static void reason(const char *fmt, const char *a) { pthread_mutex_lock(&g_rmu); snprintf(g_reason, sizeof g_reason, fmt, a); pthread_mutex_unlock(&g_rmu); }

static void settings_init(void) {
    int exp = -1;
    if (atomic_load(&g_on) != -1) return;
    const char *e;
    if ((e = getenv("DM_LSFG_MULT"))) atomic_store(&g_mult, atoi(e));
    if ((e = getenv("DM_LSFG_TARGET"))) atomic_store(&g_target, atoi(e));
    if ((e = getenv("DM_LSFG_FLOW"))) atomic_store(&g_flow, atoi(e));
    e = getenv("DM_LSFG");
    atomic_compare_exchange_strong(&g_on, &exp, e && atoi(e) ? 1 : 0);
}

void nxhost_framegen_set(int on, int multiplier, int target_rate, int flow_pct) {
    atomic_store(&g_mult, multiplier < 2 ? 2 : multiplier > 4 ? 4 : multiplier);
    atomic_store(&g_target, target_rate < 0 ? 0 : target_rate);
    atomic_store(&g_flow, flow_pct <= 0 ? 0 : flow_pct < 25 ? 25 : flow_pct > 100 ? 100 : flow_pct);
    atomic_store(&g_on, on ? 1 : 0);
    VLOG("frame generation: %s, x%d, target %d fps, flow %d%%", on ? "on" : "off", atomic_load(&g_mult), atomic_load(&g_target), atomic_load(&g_flow));
}

int nxhost_framegen_status(char *buf, int n) {
    settings_init();
    int st = atomic_load(&g_state);
    if (buf && n > 0) { pthread_mutex_lock(&g_rmu); snprintf(buf, (size_t)n, "%s", g_reason); pthread_mutex_unlock(&g_rmu); }
    return st;
}

#ifndef NX_LSFG
int vka_fg_frame(uint64_t serial, const int *crop, double period, uint64_t fence, vka_img **gen, double *hold, double *out_period) {
    (void)serial; (void)crop; (void)period; (void)fence; (void)gen;
    settings_init(); *hold = 0; *out_period = 0;
    if (atomic_load(&g_on) > 0) { atomic_store(&g_state, -1); reason("%s", "not in this build (built without NX_LSFG)"); vka_fps_fg(-1, 1); }
    return 0;
}
#else
enum { NFG = 3 };
static struct {
    int state;                         // 0 not tried, 1 ready (modules loaded), -1 unavailable
    lsfg_modules mods;
    lsfg_chain *chain; VkExtent2D ext; int flow;
    vka_img *out[LSFG_MAX_GENERATIONS]; VkImageView out_view[LSFG_MAX_GENERATIONS];
    VkCommandPool pool; struct { VkCommandBuffer cb; VkFence fence; vka_img *held; } fr[NFG]; unsigned fi;
    lsfg_pacer pacer; uint64_t count; uint32_t warm_streak;
    double out_period, longer_since;   // the output period the window's frame rate is set for, and since when a longer one is wanted
} F;

static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void unavailable(const char *why, const char *a) { F.state = -1; reason(why, a); atomic_store(&g_state, -1); VRED("frame generation unavailable: %s", g_reason); }

// The device must have what the shaders use (Float16, StorageImageExtendedFormats, StorageImageWriteWithoutFormat), null descriptors, and storage images of the chain's formats
static int device_ok(void) {
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    const char *miss = !f12.shaderFloat16 ? "shaderFloat16" : !f2.features.shaderStorageImageExtendedFormats ? "shaderStorageImageExtendedFormats"
                     : !f2.features.shaderStorageImageWriteWithoutFormat ? "shaderStorageImageWriteWithoutFormat" : !V.null_desc ? "null descriptors (robustness2)" : NULL;
    VkFormat fmts[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (int i = 0; i < 3 && !miss; i++) {
        VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, fmts[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) miss = "storage images of R8, RGBA8 or RGBA16F";
    }
    if (miss) { unavailable("the GPU lacks %s", miss); return 0; }
    return 1;
}

static int load(void) {
    if (F.state) return F.state > 0;
    if (!device_ok()) return 0;
    const char *path = getenv("DM_LSFG_DLL");
    if (!path || !*path) { unavailable("%s", "no Lossless.dll (the launcher looks in Documents/Emulation/bios, Switch2 and the app's files; or pick it in the launcher)"); return 0; }
    uint64_t t0 = now_ns();
    int r = lsfg_dll_load(path, &F.mods);
    if (r) { char b[200]; snprintf(b, sizeof b, "%s: %s", path, lsfg_status_name(r)); unavailable("%s", b); return 0; }
    VLOG("frame generation: %d shaders from %s (%.1f ms)", F.mods.n, path, (now_ns() - t0) / 1e6);
    F.state = 1;
    return 1;
}

static void teardown(void) {
    if (!F.chain && !F.out[0]) return;
    pthread_mutex_lock(&V.qmu); vk.vkQueueWaitIdle(V.q); pthread_mutex_unlock(&V.qmu);
    for (int i = 0; i < NFG; i++) if (F.fr[i].held) { vka_tex_drop(F.fr[i].held); F.fr[i].held = NULL; }
    lsfg_chain_free(F.chain); F.chain = NULL;
    for (int i = 0; i < LSFG_MAX_GENERATIONS; i++) {
        if (F.out_view[i]) vk.vkDestroyImageView(V.dev, F.out_view[i], NULL);
        F.out_view[i] = VK_NULL_HANDLE;
        if (F.out[i]) vka_tex_drop(F.out[i]);   // the last reference: destroyed by vka_tex.c (with the views the present made of it)
        F.out[i] = NULL;
    }
    lsfg_pacer_reset(&F.pacer); F.count = 0; F.warm_streak = 0; F.out_period = 0; F.longer_since = 0;
}

// An output as a vka_img, so the present shows it as it shows the window texture (blit or the rotated copy) and keeps it alive while it reads it
static vka_img *out_new(VkExtent2D e, int i, VkImageView *view) {
    vka_img *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->serial = 0xF6000000ull + (uint64_t)i; im->refs = 1; im->inited = 1; im->gpu_owned = 1;
    im->f = (vka_fmt){VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 0, 0, 4, 1, 1};
    im->d.target = NVN_TEXTURE_TARGET_2D; im->d.width = e.width; im->d.height = e.height;
    im->usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im->aspect = VK_IMAGE_ASPECT_COLOR_BIT; im->w = e.width; im->h = e.height; im->depth = im->layers = im->levels = im->samples = 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im->f.vk, .extent = {e.width, e.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = im->usage};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &im->img);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &im->m.dm);   // its own allocation (vka_imem with no block; size 0: not in the image memory stats)
    if (!r) r = vk.vkBindImageMemory(V.dev, im->img, im->m.dm, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im->f.vk, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, view);
    if (r) { VRED("frame generation: output image %ux%u: %d", e.width, e.height, r); vka_tex_drop(im); return NULL; }
    return im;
}

static int rebuild(VkExtent2D e, int flow) {
    teardown();
    VkImage imgs[LSFG_MAX_GENERATIONS];
    for (int i = 0; i < LSFG_MAX_GENERATIONS; i++) { if (!(F.out[i] = out_new(e, i, &F.out_view[i]))) { teardown(); return 0; } imgs[i] = F.out[i]->img; }
    uint64_t t0 = now_ns();
    F.chain = lsfg_chain_new(&F.mods, e, flow ? flow / 100.0f : 1.0f, imgs, F.out_view);
    if (!F.chain) { teardown(); return 0; }
    F.ext = e; F.flow = flow;
    VLOG("frame generation: chain for %ux%u made in %.1f ms", e.width, e.height, (now_ns() - t0) / 1e6);
    return 1;
}

int vka_fg_frame(uint64_t serial, const int *crop, double period, uint64_t fence, vka_img **gen, double *hold, double *out_period) {
    settings_init();
    *hold = 0; *out_period = 0;
    int on = atomic_load(&g_on) > 0, mult = atomic_load(&g_mult), target = atomic_load(&g_target), flow = atomic_load(&g_flow);
    if (!on) {
        if (F.chain) { teardown(); VLOG("frame generation off"); }
        if (F.state >= 0) { atomic_store(&g_state, 0); reason("%s", "off"); }
        vka_fps_fg(F.state < 0 ? -1 : 0, 1);
        return 0;
    }
    if (!load()) { vka_fps_fg(-1, 1); return 0; }
    // The game's frame period is never shorter than the host's frame limit allows (hle_fpslock.c): a game that asks for interval 1 (16.7 ms) but is held to 30 fps has two
    // refreshes of a 60 Hz panel a frame, room for one generated frame, and its output rate is 60, not 120. Donkey Kong Bananza's presents ride the host's tick
    // (DM_PRESENT_INTERVAL=display, 8.3 ms at 120 Hz) whatever its limit
    int nxhost_fps_limit_get(void) __attribute__((weak)), nxhost_fps_decoupled(void) __attribute__((weak));
    { int lim = &nxhost_fps_limit_get ? nxhost_fps_limit_get() : 0; if (lim > 0 && period < 1.0 / lim) period = 1.0 / lim; }
    // The window's frame rate: a shorter output period at once, a longer one only after it held for 2 s. DKB switches its own present interval between 1 and 2 (60 and
    // 30 fps) many times a minute, and each switch would change the panel's mode (120 <-> 60 Hz); the generated frames are paced by hold, not by the panel's rate
    double want = target > 0 ? 1.0 / target : period / (mult < 2 ? 2 : mult > 4 ? 4 : mult), tnow = mono_s();
    if (F.out_period <= 0 || want < F.out_period - 1e-4) { F.out_period = want; F.longer_since = 0; }
    else if (want > F.out_period + 1e-4) { if (!F.longer_since) F.longer_since = tnow; else if (tnow - F.longer_since >= 2.0) { F.out_period = want; F.longer_since = 0; } }
    else F.longer_since = 0;
    *out_period = F.out_period;
    vka_img *im = vka_tex_acquire(serial);
    if (!im) return 0;
    int cx = 0, cy = 0, cw = (int)im->w, ch = (int)im->h;
    if (crop && crop[2] > 0 && crop[3] > 0) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    VkFormatProperties fp = {0}; vk.vkGetPhysicalDeviceFormatProperties(V.pd, im->f.vk, &fp);
    if (!im->inited || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->f.kind || im->samples != 1 || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || cw <= 0 || ch <= 0) {
        VRED_ONCE("frame generation: window texture serial %llu (format %#x) cannot be copied into the chain: not generating", (unsigned long long)im->serial, im->d.format);
        vka_tex_drop(im); F.warm_streak = 0; atomic_store(&g_state, 2); vka_fps_fg(2, 1); return 0;
    }
    VkExtent2D e = {(uint32_t)cw, (uint32_t)ch};
    if (!F.chain || F.ext.width != e.width || F.ext.height != e.height || F.flow != flow) {
        if (!rebuild(e, flow)) { vka_tex_drop(im); unavailable("%s", "the frame generation chain could not be made (see the [RED] lines)"); vka_fps_fg(-1, 1); return 0; }
    }
    F.pacer.decoupled = &nxhost_fps_decoupled && nxhost_fps_decoupled();   // the game's speed follows its measured rate (hle_fpslock.c sead_rate), not only held by the limiter
    F.pacer.min_base = (float)VKA_ENV("DM_LSFG_MIN_FPS", 30);   // its floor (live)
    lsfg_plan plan = lsfg_pacer_plan(&F.pacer, LSFG_MAX_GENERATIONS, mult, target, mono_s());
    uint64_t count = F.count++;
    int warm = plan.warm && count + 1 >= 2;
    F.warm_streak = warm ? F.warm_streak + 1 : 0;
    size_t ng = warm && F.warm_streak >= 2 ? plan.generations : 0;
    if (ng > LSFG_MAX_GENERATIONS) ng = LSFG_MAX_GENERATIONS;
    // The panel shows one image a refresh (FIFO): more images per game frame than refreshes in the game's period hold the present thread, and the game behind it, to a
    // fraction of its rate. The window asks for the output rate (out_period above), but the system may keep the panel lower: on the Honor phone (LTPO) it sometimes stayed at
    // 60 Hz with the request for 120, and x2 held Donkey Kong Bananza's 60 fps menus at 30 fps (its level was reached 18 s later, int-792f6e4d t1). Generated frames only
    // as many as the refreshes in the game's own period leave room for (its requested period, not the measured interval, which a throttled game would lengthen)
    // DM_VK_FG_REFRESH_CAP=0: no cap (a driver whose display timing reports a refresh the panel does not run at)
    uint64_t rns = VKA_ENV("DM_VK_FG_REFRESH_CAP", 1) ? vka_refresh_ns() : 0; int capped = 0;
    if (ng && rns && period > 0) {
        // The game's frame period: the present's (at least the frame limit's, above), or the measured one when longer. With DM_PRESENT_INTERVAL=display (Donkey Kong
        // Bananza's uncapped mode) every present declares the host's tick (8.3 ms at 120 Hz) whatever the game's own rate, which left no refresh for a generated frame and
        // generation never started; held at 60 on a 120 Hz panel its measured interval came out at 15.3-15.5 ms (1.85 refreshes, under the 1.9 below), where the limit's
        // 16.7 ms leaves room for the one generated frame that fits
        double gp = F.pacer.smoothed_interval > period ? F.pacer.smoothed_interval : period;
        double n = gp * 1e9 / (double)rns;   // refreshes in a game frame (60 fps on 120.46 Hz: 2.01; on 60.00 Hz: 1.00)
        size_t fit = n < 1.9 ? 0 : (size_t)(n + 0.1) - 1;
        if (ng > fit) { ng = fit; capped = 1; F.pacer.issued = ng; }   // the pacer counts what was generated (its unloaded rate is measured on frames without)
    }

    unsigned k = F.fi++ % NFG;
    if (!F.fr[k].cb) {
        if (!F.pool) { VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf}; vk.vkCreateCommandPool(V.dev, &pi, NULL, &F.pool); }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = F.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        vk.vkAllocateCommandBuffers(V.dev, &ai, &F.fr[k].cb); vk.vkCreateFence(V.dev, &fi, NULL, &F.fr[k].fence);
    }
    vk.vkWaitForFences(V.dev, 1, &F.fr[k].fence, VK_TRUE, UINT64_MAX);
    if (F.fr[k].held) { vka_tex_drop(F.fr[k].held); F.fr[k].held = NULL; }
    vk.vkResetFences(V.dev, 1, &F.fr[k].fence);
    VkCommandBuffer cb = F.fr[k].cb; vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    vka_barrier(cb);   // the replay's writes to the window texture, and the presents still reading the outputs (earlier submissions)
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {cw, ch, 1}}};
    vk.vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, lsfg_chain_input(F.chain, count), VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    if (warm) lsfg_chain_shared(F.chain, cb, count);
    for (size_t g = 0; g < ng; g++) lsfg_chain_generate(F.chain, cb, count, ng, g);
    vka_barrier(cb);   // the outputs for the presents after this
    vk.vkEndCommandBuffer(cb);
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &fence};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .waitSemaphoreCount = fence ? 1 : 0, .pWaitSemaphores = &V.tl, .pWaitDstStageMask = &st,
                       .commandBufferCount = 1, .pCommandBuffers = &cb};
    pthread_mutex_lock(&V.qmu); VkResult r = vk.vkQueueSubmit(V.q, 1, &si, F.fr[k].fence); pthread_mutex_unlock(&V.qmu);
    F.fr[k].held = im;   // the window texture stays alive until this submission is done
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("frame generation submission %d", r); ng = 0; }
    for (size_t g = 0; g < ng; g++) gen[g] = F.out[g];
    double interval = F.pacer.smoothed_interval > 0 ? F.pacer.smoothed_interval : period;
    if (interval < period) interval = period;
    if (ng) *hold = interval / (double)(ng + 1);
    // A hold within 0.15 refresh of a whole number of refreshes is that number: the measured interval runs a little long (34 ms at a 30 fps frame step), and a hold of 17 ms on a
    // 16.7 ms panel let each image's desired time (vka_present.c, the previous one's plus the hold) drift across a refresh every few dozen frames, two images in one refresh
    if (ng && rns) { double r = *hold * 1e9 / (double)rns, w = floor(r + 0.5); if (w >= 1 && fabs(r - w) <= 0.15) *hold = w * (double)rns / 1e9; }
    int st2 = ng ? 1 : 2;
    static int was_capped;
    if (atomic_exchange(&g_state, st2) != st2 || (st2 == 2 && capped != was_capped)) {
        char why[96]; snprintf(why, sizeof why, "on, the display runs at %.0f Hz: no refresh left for a generated frame", rns ? 1e9 / (double)rns : 0.0);
        reason("%s", st2 == 1 ? "generating" : capped ? why : "on, waiting for a steady frame rate");
        static int ncap; if (st2 == 2 && capped && ncap++ < 4) VLOG("frame generation: not generating, %s (game frame period %.1f ms, measured %.1f ms)", why + 4, period * 1e3, F.pacer.smoothed_interval * 1e3);
        static int logged; if (st2 == 1 && !logged) { logged = 1; VLOG("frame generation: first %zu generated frame(s), game frame interval %.1f ms", ng, interval * 1e3); }
    }
    was_capped = st2 == 2 && capped;
    vka_fps_fg(st2, (int)ng + 1);
    return (int)ng;
}
#endif
#endif
