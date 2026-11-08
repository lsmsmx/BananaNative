// Internal interface of the NVN → Vulkan replay (dm/gfx/vk_android): the device (vka_dev.c), placed guest memory and pools (vka_mem.c), textures and descriptor tables (vka_tex.c),
// the command replay (vka_exec.c) and the present (vka_present.c). They stand in for nvn_mtl.m behind the mtl_* interface hle_nvn.c calls (nvn_mtl.h, nvn_obj.h).
// Vulkan comes from the headers in dm/third_party (newer than the NDK's 1.3.275: VK_EXT_map_memory_placed, VK_KHR_present_wait2); every entry point is loaded through vkGetInstanceProcAddr
#pragma once
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../nvn/nvn_obj.h"
#include "../../host/dm.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define VRED(...) LOG("[RED] vk: " __VA_ARGS__)
#define VRED_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) VRED(__VA_ARGS__); } while (0)

#define VKA_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceVersion)
#define VKA_INSTANCE_FNS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkGetPhysicalDeviceSparseImageFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VKA_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkQueueBindSparse) X(vkGetImageSparseMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkMapMemory2KHR) X(vkUnmapMemory2KHR) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkGetBufferDeviceAddress) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkGetImageMemoryRequirements2) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCreateRenderPass) X(vkCreateFramebuffer) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkGetRefreshCycleDurationGOOGLE) X(vkGetPastPresentationTimingGOOGLE) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkResetDescriptorPool) X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDispatchBase) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetBlendConstants) X(vkCmdSetLineWidth) \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdSetVertexInputEXT) X(vkCmdPushConstants) X(vkCmdDrawMeshTasksEXT)
#define VKA_DECL(f) PFN_##f f;
typedef struct { PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr; VKA_GLOBAL_FNS(VKA_DECL) VKA_INSTANCE_FNS(VKA_DECL) VKA_DEVICE_FNS(VKA_DECL) } vka_fns;
extern vka_fns vk;   // a missing optional entry point is NULL

// ---------- Switches read again while the game runs (vka_ctl.c) ----------
// VKA_ENV("DM_VK_<NAME>", def) is a DM_VK_* switch's value as a number (strtod; unset or empty = def), cached per call site: one relaxed load and a compare while nothing changed.
// A live "env KEY=VALUE" control line (android/TESTING.md "Live control") bumps vka_env_gen at a frame boundary and every call site reads its switch again on its next call.
// Read switches through it, not getenv: a switch that cannot change while running (it sizes a device object, a cache) goes into vka_ctl.c's restart-only table
// A call site can run on two threads at once (the game's and the replay worker's): v is atomic (no torn read), and only one thread at a time refreshes a cache, and only to a
// newer generation (vka_env_slow), so a thread that read the value before a change cannot store it over a newer one while gen says current
typedef struct { _Atomic uint32_t gen; _Atomic double v; } vka_envc;
extern _Atomic uint32_t vka_env_gen;
double vka_env_slow(vka_envc *c, const char *key, double def);
static inline double vka_env_c(vka_envc *c, const char *key, double def) {
    return atomic_load_explicit(&c->gen, memory_order_acquire) == atomic_load_explicit(&vka_env_gen, memory_order_relaxed) ? atomic_load_explicit(&c->v, memory_order_relaxed)
                                                                                                                         : vka_env_slow(c, key, def);
}
#define VKA_ENVF(key, def) ({ static vka_envc c_; vka_env_c(&c_, key, def); })
#define VKA_ENV(key, def) ((int)VKA_ENVF(key, def))
int vka_env_str(const char *key, char *out, size_t n);   // a string switch as live control sees it (1 = set); not cached: for switches read rarely
void vka_floor_span(uint64_t frame, uint64_t ns);   // vka_exec.c: a submission of frame ended ns after the frame began (the adaptive floor)
void vka_floor_stats(char *b, size_t n);
void vka_floor_commit(int counted);   // DM_VK_GPU_FLOOR_AUTO=2: a submission counted by vka_floor_tail_add handed to the queue / done
void vka_floor_done(int counted);
void vka_gpu_floor(void);               // mtl_sub_begin: DM_VK_GPU_FLOOR's GPU clock floor on the queue (vka_dev.c)
void vka_ctl_sub(void);                 // mtl_sub_begin: live switches queued since the last frame are applied
void vka_ctl_frame(uint64_t f);         // mtl_set_frame: per-frame counters for "capture" and "stats"
void vka_exec_counts(uint64_t *ops, uint64_t *subs, uint64_t *rec_ns);   // vka_exec.c: clears / copies, submissions, recording time so far

// ---------- Device (vka_dev.c) ----------
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties memp;
    int placed, placed_range, unmap_reserve, bda, uab, display_timing, has_android_surface;
    void *perf_hint;                                // vkQueueSetPerfHintQCOM (VK_QCOM_queue_perf_hint), NULL without it: vka_gpu_floor
    // The draw path's device facts: push descriptors (and how many a set may hold), vertex input dynamic state, robustness2's null descriptors, 8-bit indices,
    // instance-rate divisors, logic operations, dual-source blending, geometry and tessellation stages, depth clamp, fill mode lines, wide lines
    int push_desc, vin_dyn, null_desc, idx8, divisor, logic_op, dual_src, geom, tess, depth_clamp, fill_lines, depth_bias_clamp, multi_vp, a2c;
    // NVN2 task and mesh programs (VK_EXT_mesh_shader: mesh = meshShader, task = taskShader too; DM_VK_MESH=0 leaves it off): their shaders are refused without it (vka_shader.c refusal)
    // and their draws skipped. mesh_inv / task_inv: the largest workgroup (maxMeshWorkGroupInvocations / maxTaskWorkGroupInvocations); mesh_out_v / mesh_out_p: the most vertices /
    // primitives a mesh workgroup may output; task_payload: maxTaskPayloadSize; task_groups / mesh_groups: maxTaskWorkGroupCount[0] / maxMeshWorkGroupCount[0] (a draw's count)
    int mesh, task; uint32_t mesh_inv, task_inv, mesh_out_v, mesh_out_p, task_payload, task_groups, mesh_groups;
    int gpl;                                        // graphics pipeline libraries with fast linking (VK_EXT_graphics_pipeline_library): a pipeline the cache lacks is linked from stage libraries at once, the optimized one made in the background (vka_pso.c)
    uint32_t max_push, max_vin_bindings, subgroup_size, subgroup_min, subgroup_max, subgroup_stages;   // subgroup_stages: VkShaderStageFlags with subgroup operations
    uint32_t subgroup_req_stages;                   // VkShaderStageFlags a pipeline may pin the subgroup size in (requiredSubgroupSizeStages; 0 without subgroupSizeControl)
    // The driver (VkPhysicalDeviceDriverProperties.driverID: VK_DRIVER_ID_QUALCOMM_PROPRIETARY, VK_DRIVER_ID_MESA_TURNIP, ...). A per-driver default keys on this and on
    // feature queries, never on the driver's name; each such default keeps its DM_VK_* switch (android/README.md "Custom GPU drivers")
    int driver_id;
    VkDeviceSize ub_align, sb_align, ub_range, sb_range;
    VkDeviceSize placed_align, max_alloc;
    uint32_t guest_type, local_type, upload_type;   // guest RAM (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED, or the nearest), images, upload arenas
    VkSemaphore tl;                                 // the timeline: value n = submission n has completed
    pthread_mutex_t qmu;                            // the queue is shared by the submit and present threads
    int sparse_res;                                 // sparse residency images can be made (vka_sparse.c): binding on the queue, 2D residency, standard block shape, strict non-resident reads
    int sparse_aliased;                             // the driver reports sparseResidencyAliased: one tile slot may be bound at several images (vka_sparse.c share_take)
    uint64_t tl_queued;                             // the highest timeline value a vkQueueSubmit so far signals (under qmu): what a later submission on the queue may wait for
} vka_dev;
extern vka_dev V;
// Custom GPU drivers (vka_driver.c): DM_VK_DRIVER=<folder with meta.json, or a .so> loads that driver through libadrenotools (restart only); unset, empty or "system" = the
// system libvulkan.so. vka_driver_open returns the libvulkan handle (NULL = use the system one); vka_driver_tag is a file-name-safe name of the custom driver in use, NULL for the system's
void *vka_driver_open(void);
const char *vka_driver_tag(void);
int vka_init(void);   // 1 = the device is up (idempotent, any thread); 0 = no Vulkan: every op logs and does nothing
int vka_ok(void);
uint32_t vka_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid);
VkResult vka_submit(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal);

// The fence ledger: fence n is "done" once the GPU passed it and its CPU-side effects (events, counters) have run
uint64_t vka_fence_next(void);                      // reserve the next fence value (submit thread)
uint64_t vka_fence_last(void);                      // the last fence reserved (its submission may still be recording on another thread)
uint64_t vka_fence_queued(void);                    // the last fence handed to the queue: the most a submission (the present's) may wait for without a wait-before-signal on its own queue
// The replay (vka_exec.c), entered by the replay worker (vka_worker.c) or inline by the mtl_* calls of the submitting thread
void vka_rp_sub_begin(void);
void vka_rp_exec(const uint8_t *handle, uint64_t fence);   // fence: the submission's, reserved before it (0 = taken at its commit)
uint64_t vka_rp_sub_commit(uint64_t reserved);
void vka_rp_queue_activate(uint64_t serial);
void vka_rp_queue_forget(uint64_t serial);
void vka_rp_set_queue_origin(int on);
int vka_worker_on(void);                              // DM_VK_WORKER (default 1): submissions are replayed on the replay worker thread
uint64_t vka_present_reserve(void);
void vka_present_fill(uint64_t tk, const tex_t *t, double period_s, const int crop[4]);
uint64_t vka_present_inline(const tex_t *t, double period_s, const int crop[4]);
uint64_t vka_hang_ns(void);                         // how long a fence may make no progress before it is a hang (DM_VK_HANG_S, default 6 s)
int vka_hung(void);
int vka_queue_idle(void);                           // vkQueueWaitIdle bounded by the hang time (0 = not idle: hung or timed out)                                 // 1 = a GPU hang was declared (vka_dev.c): nothing more is submitted, waits return at once
void vka_fence_post(uint64_t fence, void *ops, int n, void (*run)(void *ops, int n));   // effects run by the completion thread when fence completes
uint64_t vka_done(void);
void vka_fence_cpu(uint64_t fence, void *ops, int n, void (*run)(void *ops, int n));   // no GPU behind it (no device, a failed submit): the effects run now and fence is done
int vka_wait_done(uint64_t fence, uint64_t timeout_ns);
void vka_after(uint64_t fence, void (*fn)(void *), void *arg);   // deferred destruction: fn(arg) once fence is done

// Host-visible scratch memory for one submission (uploads, deswizzled texels, readbacks); valid until the submission's fence
typedef struct { VkBuffer buf; uint8_t *ptr; VkDeviceSize off; } vka_span;
int vka_scratch(VkDeviceSize n, VkDeviceSize align, vka_span *out);
void vka_scratch_retire(uint64_t fence);            // everything handed out since the last call belongs to fence
// Upload batches (vka_dev.c): a new image's first content recorded into a command buffer of its own, whose staging retires on its own completion. Replay thread only.
// vka_up_begin: the open batch (begun when none; NULL = none, record into the submission), and vka_scratch on this thread takes the batch's staging until vka_up_end.
// vka_up_point: in an upload's loop, cb itself, or, when cb is the batch and its staging has passed the batch budget, the batch sent and the next one (NULL: none could
// be begun, the upload stops). vka_up_cur: after such a loop, the open batch when cb was a batch. vka_up_flush: the open batch to the queue (before every part or
// submission the replay hands to the queue)
extern _Thread_local int vka_scr_up;
int vka_up_on(void);
VkCommandBuffer vka_up_begin(void);
void vka_up_end(void);
VkCommandBuffer vka_up_point(VkCommandBuffer cb);
VkCommandBuffer vka_up_cur(VkCommandBuffer cb);
void vka_up_flush(void);
void vka_up_throttle(void);                         // waits while more than DM_VK_UPLOAD_INFLIGHT_MB of batch staging is in flight (no lock held)
void vka_up_stats(uint64_t *batches, uint64_t *waits, double *wait_ms);
VkCommandBuffer vka_rp_cb(void);                    // the replay's command buffer being recorded (vka_exec.c; NULL: none)
// One "memory" line each time the replay's own memory (placed pools, images, scratch) has taken DM_VK_MEMLOG MiB more (allocations counted, frees not) (default 512, 0: none) since the last one: where the memory
// went in a game that the system stops before its first frame (Mario Kart World 1.8.0 on a 16 GB phone). Values read without the owners' locks (a log line, called under them)
void vka_mem_grew(uint64_t bytes);
uint64_t vka_scratch_bytes(uint32_t *blocks);
uint64_t vka_mem_placed_nolock(void);
uint64_t vka_tex_mem_nolock(int *n);

// Sparse residency images (vka_sparse.c): memory only behind the 64 KB tiles a sparse pool's texture has mapped
typedef struct vks_img vks_img;
int vks_on(void);                                   // the device can (V.sparse_res) and DM_VK_SPARSE_RES is not 0
int vks_va_take(uint64_t bytes);                    // sparse address space for an image of that virtual size: 1 = taken
void vks_va_give(uint64_t bytes);
uint64_t vks_va_free(void);                         // sparse address space free now (a texture left at a coarser level tries a finer one once more is free than when it failed)
vks_img *vks_new(VkImage img, uint32_t type, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t tw, uint32_t th, uint32_t tiled, uint32_t fmt, int drawn);   // NULL: make it dense (fmt and drawn: what a shared slot is keyed by, a drawn image never uploads into one)
int vks_tile(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty, int want, uint64_t key);   // bind / unbind one tile of level lv to the slot of the physical memory key (0: a slot of its own): 1 = bound now
int vks_tile_bound(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty);
uint32_t vks_tiled(const vks_img *s);               // image levels bound tile by tile (the later ones are bound whole)
uint64_t vks_img_bytes(vks_img *s);
void vks_free(vks_img *s);                          // img_destroy, before vkDestroyImage
void vks_queue_locked(uint32_t *nw, VkSemaphore *ws, uint64_t *wv, VkPipelineStageFlags *wst);   // before a vkQueueSubmit of the replay or an upload batch, under V.qmu
void vks_stats(char *b, size_t n);
uint64_t vks_bytes_nolock(void);

// ---------- Guest memory (vka_mem.c) ----------
// Guest RAM is anonymous memory; each NVN memory pool becomes placed Vulkan memory at its own address when it is made (mtl_pool_add) and anonymous again when it goes (mtl_pool_del):
// the CPU and GPU see the same bytes. A placed range is a segment with one VkBuffer over it
int vka_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *bda);   // 1 = [addr, addr + len) lies in one placed segment
int vka_mem_release(uintptr_t lo, uintptr_t hi);    // the host gives [lo, hi) back: segments there are given back first; 0 = the host releases the range itself
uint64_t vka_addr_run(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // bytes from addr (at most len) in the segment that holds addr, 0 = none
int vka_pool_bda(uint64_t addr, VkDeviceAddress *bda);   // the device address of a pool byte (the table the binding side reads)
void *vka_mem_mapped(size_t n);   // no placed maps: a mapped allocation as a segment where the driver maps it (the selftest on an emulator)
int vka_mem_chunks(uint64_t *lo, uint64_t *hi, uint64_t *bda, int max);   // the placed segments by address (guest range and device address); the count
uint64_t vka_mem_placed_bytes(uint64_t *peak);     // bytes of placed Vulkan memory now (and the peak)

// ---------- Textures (vka_tex.c) ----------
typedef struct {
    VkFormat vk;           // the image's own format (8-bit sRGB formats are stored as their UNORM twin: the present and copies move raw bytes, views add the sRGB decode)
    VkFormat view;         // the format views and clears use
    uint8_t kind;          // 0 float / unorm / snorm, 1 uint, 2 sint, 3 depth, 4 stencil, 5 depth-stencil
    uint8_t srgb;          // stored as UNORM, cleared with an sRGB-encoded colour
    uint8_t bpb, bw, bh;
} vka_fmt;
int vka_fmt_of(int nvn_format, vka_fmt *f);         // 0 = no Vulkan format (logged once per format)
// An image's memory (vka_imgmem.c): space in a shared block (blk, at off, units of 256 bytes) or a dedicated allocation (blk NULL, off 0); size = its memory requirements' size
// z: zero-shared memory (vka_imem_bind_zero; dm is the shared allocation, freed with its last image)
typedef struct { VkDeviceMemory dm; uint64_t off, size; void *blk; uint32_t units; int z; } vka_imem;
VkResult vka_imem_bind(VkImage img, uint32_t type, const VkMemoryRequirements *mr, vka_imem *m);   // allocated and bound, or the error with nothing allocated
VkResult vka_imem_bind_zero(VkImage img, uint32_t type, const VkMemoryRequirements *mr, const void *key, uint32_t klen, vka_imem *m, int *fresh);   // bound to the zeroed allocation of its key (vka_zshare.h)
uint64_t vka_imem_zbytes(void); uint64_t vka_imem_zbytes_nolock(void);   // bytes of the zero-shared allocations
void vka_imem_zowned(void);   // a zero-shared image got memory of its own (vka_tex.c img_own): counted for the stats line
void vka_imem_free(vka_imem *m);   // after vkDestroyImage, once the GPU is done with the image
void vka_imem_stats(char *b, size_t n);
uint64_t vka_imem_allocations(void);   // VkDeviceMemory objects the images hold (blocks + dedicated)
typedef struct vka_img {
    uint64_t serial; const uint8_t *mem; nvn_texdesc d; nvn_texlayout L;
    VkImage img; vka_imem m; vka_fmt f; VkImageUsageFlags usage; VkImageAspectFlags aspect;
    uint32_t w, h, depth, layers, levels, samples; uint64_t bytes;
    VkImageCreateFlags cflags; uint32_t nvf; VkFormat vf[6];   // as created: flags and the view format list (none: nvf 0), for imageless framebuffers (DM_VK_CLASSIC_RP)
    int refs, inited, gpu_owned, cpu_dirty;
    int no_storage;          // remaking it with storage use was refused (img_storage_upgrade): not tried again
    int holds;               // references taken from outside the recording thread (vka_tex_acquire / vka_tex_ref: the present, frame generation, capture), which read img unlocked: the image is not remade while held (img_storage_upgrade)
    // Content from guest storage (vka_tex.c storage_mode): track = re-uploaded when the CPU writes its pages (write-protected after each upload, nvn_dirty.c),
    // stamp = the dirty-page generation taken before the last upload, span = the tracked pool range that holds the storage
    int track, wb_pending; uint64_t stamp; const void *span;
    uint64_t hash;           // DM_VK_NANSCAN: a hash of the storage at the last upload (vka_tex_id_check)
    uint64_t gw_seen; int from_mem; uint64_t head, up_fence; uint8_t *snap;   // head, up_fence: DM_VK_NANSCAN's storage watch   // the newest GPU write into guest memory its last upload included (vka_tex.c gw_t); content from guest storage (ST_TRACK / ST_FILL)
    // In a sparse virtual pool (hle_nvn.c nvn_sparse_*): the storage is read through the pool's 4 KB page table, never at mem (unmapped pages there are inaccessible);
    // vtrack = re-uploaded when a mapping in its range or a physical page behind it changes, vgen = the page table's generation at the last upload
    const void *vpool; int vtrack; uint64_t vgen;
    int vbind;               // a GPU-written sparse texture's residency image (vka_tex.c sparse_gpu): its tiles follow the mappings (sparse_bind), nothing uploaded
    // base: the texture's finer levels left out of the image (vka_tex.c sparse_base): image level 0 is the texture's level base, made only of the levels where pages are mapped.
    // Texture levels in views, render targets and copies go through vka_img_lv
    // vmin: the texture level its sampled views start at (vka_tex.c sparse_base: the finest level from which every level is wholly mapped)
    uint32_t base, vmin;
    // A sparse residency image (vka_sparse.c): memory only behind its mapped 64 KB tiles (rs_tw x rs_th texels), va bytes of the sparse address space; bfloor: the finest level it
    // may start at (levels left out to fit the address space, vka_tex.c img_create), a hint kept while vks_va_free() is less than bneed (the address space the next finer start asked for when it did not fit)
    struct vks_img *rs; uint32_t rs_tw, rs_th, bfloor; uint64_t va, bneed;
    void *views;             // the image views made of it (vka_view)
    int any_fmt;             // DM_VK_UBWC=0: MUTABLE_FORMAT without a list, views of any format
    VkFormat alt;            // the one other format its views may take (its sRGB twin; img_create's format list), or VK_FORMAT_UNDEFINED: views only in its own format
    VkFormat xfmt[4]; int nx; // further formats its format list takes: ones textures over the same memory were seen to view it in (vka_tex.c, learned view formats)
    uint8_t mode;            // its content at first use (vka_tex.c ST_ZERO / ST_FILL / ST_TRACK)
    uint8_t pool_fit;        // its memory requirements fit its storage in the pool (vka_tex_alias_stats): type, size and alignment
    // pool: the texture's pool at the image's creation, pgen its Initialize serial then (hle_nvn.c nvn_pool_live; 0: none or not live); pfin: nvn_pool_fin at the last check;
    // pdead: that pool was finalized since (vka_tex.c img_pool_dead): its storage is not read again
    const void *pool; uint64_t pgen, pfin; int pdead;
    uint8_t dcap;            // a sparse texture's dense image whose finer levels DM_VK_SPARSE_DENSE_MB left out (vka_tex.c dense_floor)
    uint64_t last;           // the frame it was last bound for a draw (vka_tex.c "Idle textures": DM_VK_TEX_EVICT lets it go when it has not been for long)
    struct vka_img *zold;    // the zero-shared image it left while a holder had it (vka_tex.c img_own): destroyed once holds drop to 0
    // Large images (DM_VK_BIGIMG_MB, vka_tex.c "Large images"): draws that bound it for sampling, other textures found over its storage (the first serials)
    uint32_t nsamp, novl; uint64_t ovl[4];
} vka_img;
// Texture level lv as a level of im (a sparse texture's image starts at level im->base): 0 when the image does not hold it
static inline int vka_img_lv(const vka_img *im, uint32_t *lv) { if (*lv < im->base) return 0; *lv -= im->base; return *lv < im->levels; }
// The layers a render target view of level lv can name: a 3D texture's slices at that level (drawn into through 2D / 2D-array views, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT), else the array layers
static inline uint32_t vka_img_slices(const vka_img *im, uint32_t lv) { return im->d.target == NVN_TEXTURE_TARGET_3D ? (im->depth >> lv ? im->depth >> lv : 1) : im->layers; }
vka_img *vka_tex_get(const tex_t *t, VkCommandBuffer cb);   // the texture's image, made (and filled from guest memory, or cleared) on first use in cb; NULL = no image
uint64_t vka_tex_mem(int *n, uint64_t *gpu_written);   // bytes of image memory now: n images, of which gpu_written bytes the GPU wrote (the rest copies pool storage)
void vka_tex_alias_stats(char *b, size_t n);        // image bytes by content (cleared, filled once, tracked) and how many of them could live on their pool storage
vka_img *vka_tex_acquire(uint64_t serial);          // a reference that keeps the image alive (the present thread), dropped with vka_tex_drop
void vka_tex_drop(vka_img *im);
int vka_tex_live_refs(vka_img **out, int max);       // references to every live image (the replayer's --dump-gpu), each dropped with vka_tex_drop
vka_img *vka_tex_ref(vka_img *im);                  // one more reference to im (dropped with vka_tex_drop)
// The GPU writes im (render target, clear, copy destination, storage image): its content is the GPU's from now on (no re-upload from storage), and a linear texture's level 0
// is copied back into its storage when the submission ends (vka_tex_writeback), where the CPU reads it as it would on the Switch
void vka_img_written_at(vka_img *im, const char *file, int line);
#define vka_img_written(im) vka_img_written_at(im, __FILE_NAME__, __LINE__)   // (the site is logged for a large image's first write, DM_VK_BIGIMG_MB)
// Before a write is recorded into im (an attachment, a clear, a copy destination, a storage image): a zero-shared image gets memory of its own, cleared (vka_tex.c img_own).
// Call it before taking a view of im for the write. Any thread that records (takes the table lock); no render pass may be open in cb unless upload batches are on
void vka_tex_own(vka_img *im, VkCommandBuffer cb);
void vka_tex_big_stats(char *b, size_t n);           // the largest images the GPU has not written, with what sampled them and what lies over their storage (DM_VK_BIGIMG_MB)
int vka_rp_is_open(void);                            // a render pass is open in the replay's command buffer (vka_draw.c)
void vka_tex_writeback(VkCommandBuffer cb);   // mtl_sub_commit, after the last pass
VkImageView vka_view(vka_img *im, VkFormat fmt, VkImageViewType type, VkImageAspectFlags aspect, uint32_t level, uint32_t nlevels, uint32_t layer, uint32_t nlayers, const VkComponentMapping *swz);
void vka_tex_prepare(VkCommandBuffer cb);           // at submit begin: dirty CPU textures re-uploaded, registered descriptors written
void vka_tex_uploads(VkCommandBuffer cb);
VkDescriptorSet vka_bindless_set(void);
VkDescriptorSetLayout vka_bindless_layout(void);
void vka_barrier_at(VkCommandBuffer cb, const char *file, int line);   // a full memory barrier (the replay orders every transfer op after the previous one)
#define vka_barrier(cb) vka_barrier_at(cb, __FILE_NAME__, __LINE__)   // the site names the step that ends there in DM_VK_GPUPROF's report (vka_gprof.c)
// DM_VK_GPUPROF (vka_gprof.c): GPU timestamps between the steps of a profiled frame
extern int vka_gp_active;
void vka_gp_mark(VkCommandBuffer cb, const char *file, int line);
void vka_gp_pass(const char *desc);
void vka_gp_part(VkCommandBuffer cb);
void vka_gp_sub_begin(VkCommandBuffer cb, uint64_t frame);
void vka_gp_commit(VkCommandBuffer cb, uint64_t fence);
void vka_gp_sub_frame(uint64_t f);
void vka_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to);
// The control line "screenshot" (vka_ctl.c): the present copies the window texture's crop into its command buffer, then hands the texels to the PNG writer once that submission is done
void vka_ctl_shot_record(VkCommandBuffer cb, vka_img *im, int cx, int cy, int cw, int ch);
void vka_ctl_shot_done(VkFence fence);

VkImageView vka_tex_id_view(int id, VkCommandBuffer cb, uint8_t dim, int storage, VkImageLayout *layout);   // texture ID id as a view of shape dim (DMSH_VK_DIM_*); VK_NULL_HANDLE = none fits
void vka_tex_id_desc(int id, char *b, size_t n);   // texture ID id described for a [RED] line
void vka_tex_feedback(vka_img *const *ims, const uint32_t *levels, int n);   // the open pass's attachments and their levels (n 0: none), for vka_tex_id_view
int vka_tex_id_in_pass(int id);   // texture ID id's image is an attachment of the open pass (vka_tex_feedback's set)
int vka_tex_id_target(int id);                      // the NVN target texture ID id stands for (its view's), -1 = not registered
int vka_tex_id_pending(int id);                     // texture ID id is registered and its image is not made yet (its first use records commands)
VkImageView vka_tex_id_view_rec(int id, VkCommandBuffer cb, uint8_t dim, int storage, VkImageLayout *layout);   // the same on the recording thread, through a memo (DM_VK_TEX_MEMO)
int vka_tex_id_pending_rec(int id, uint8_t dim);
int vka_tex_id_storage_pending(int id);            // texture ID id's image exists without storage use and would be remade with it at its next storage binding (a draw ends its pass first)
VkSampler vka_samp_id_rec(int id);
VkSampler vka_samp_id(int id);                      // sampler ID id; VK_NULL_HANDLE = not registered
VkSampler vka_samp_default(void);                   // the sampler an unbound sampler slot reads with (nearest, clamp to edge)

// ---------- Shaders (vka_shader.c) ----------
// The game's Vulkan shader library, <pack>/shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib (DM_VK_SHLIB overrides the path), loaded at the first program the replay resolves.
// A shader is one library entry with its module made; it lives as long as the process
#include "vka_shlib.h"
#include "vka_bind.h"
typedef struct vka_shader {
    uint64_t key; const dmsh_vk_entry *e; const dmsh_vk_bind *b;
    const vka_shlib *lib;               // the library the entry is in (its header's translator answers; its other modules)
    VkShaderModule mods[5];             // by DMSH_VK_MOD_* variant, made when a pipeline first asks (the base when the shader is made)
    struct vka_vinmod *vin;             // vertex modules with integer-fed inputs retyped, by masks (vka_shader_vin_module)
    struct vka_vinmod *fout;            // pixel modules with float outputs retyped for integer targets, by masks (vka_shader_fout_module)
    vka_span c1; VkDeviceSize c1_len;   // the c1 bytes in static memory (none when c1_len is 0)
    int bad;                            // refused: no module, an unknown binding, an NV-only capability (logged once with its key)
    int gs_fx;                          // a geometry shader's memory writes (vka_spv_gs_claim): above 0, its draws bind a claim buffer at VKA_GS_CLAIM_BINDING
    VkShaderModule gs_claim;            // its module with those writes under the per-primitive claim, made when a pipeline first asks
    uint32_t inst_bind;                 // an instanced variant: the uniform binding it was made for + 1 (the prewarm list records it with the base key)
} vka_shader;
// An instanced variant's key (vka_draw.c's instanced runs): its base shader's key XOR this. It is no key of the shader library, and the variant depends on the uniform
// binding it was made for, which a pipeline key does not record: the prewarm list records the base key and the binding instead (pwrec.inst)
#define VKA_INST_KEY_XOR 0x494e5354494e5354ull
static inline int vka_shader_is_variant(const vka_shader *s) { return s && s->e && s->key != s->e->key; }
vka_shader *vka_inst_variant(vka_shader *s, uint32_t binding);   // the instanced variant of s for that binding (vka_draw.c), for the prewarm threads; NULL with DM_VK_INSTANCE off
int vka_shader_variant_key(uint64_t key);   // 1 when a key is no library key but an instanced variant's (an older prewarm list's record), without a red line
typedef struct { vka_shader *sh[VKA_NSTAGE]; unsigned bad; } vka_progsh;   // by NVN stage; bad: stages whose shader is refused (bit = NVN stage) or VKA_BAD_MISSING (their draws are skipped)
#define VKA_BAD_MISSING (1u << VKA_NSTAGE)   // vka_progsh.bad: a stage's shader is not in the library (its NVN stage unknown)
int vka_prog_shaders(const prog_t *p, vka_progsh *out);      // 1 = resolved (stages the program lacks are NULL); 0 = no library or no program
void vka_prog_put(uint64_t serial, const vka_progsh *ps);   // a program made by the host itself (the selftest), by serial
vka_shader *vka_shader_make(const vka_shlib *L, uint64_t key);
const char *vka_shader_lib_path(void);   // the library file loaded ("" before or without one): this version's, or an older one of the same ABI
vka_shader *vka_shader_by_key(uint64_t key);   // the game library's shader of key (the library loaded first if needed); NULL = not there or refused
VkShaderModule vka_shader_module(vka_shader *s, int variant);
uint32_t vka_shader_sg_pin(int stage);              // DMSH_VK_STAGE_*: the subgroup size its subgroup shaders' pipelines require (vka_shader.c sg_plan), 0 = none
VkShaderModule vka_shader_cs_module(vka_shader *s);  // a compute shader's module: its DMSH_VK_MOD_SHUFFLE_LDS one where the device takes it (vka_shader.c shuffle_plan), else the base
VkShaderModule vka_shader_inst_module(vka_shader *s, uint32_t binding, uint32_t loc);   // an instanced variant of the base module (vka_spv_instance), made each call
VkShaderModule vka_shader_gs_module(vka_shader *s);   // a geometry shader's module with its memory writes made once per primitive (vka_spv_gs_claim)
#define VKA_GS_CLAIM_BINDING (DMSH_VK_STAGE_GS * DMSH_VK_STAGE_BINDINGS + DMSH_VK_STAGE_BINDINGS - 1)   // set 0: the claim words of such a shader (the stage's last binding)
VkShaderModule vka_shader_fout_module(vka_shader *s, uint32_t umask, uint32_t smask);
int vka_shader_fout_retyped(vka_shader *s, uint32_t umask, uint32_t smask);   // 1 when that module is a rewritten one (not the base module it falls back to)   // the base pixel module with those float outputs as uint / int vectors of their bits (vka_spv_fout_retype), made once per masks
#define VKA_FS_RETYPE 4                  // vka_gkey.fs_variant beyond the library's pixel DMSH_VK_MOD_* (4 is DMSH_VK_MOD_SHUFFLE_LDS of compute modules, never a pixel variant): the base pixel module with its float outputs at integer attachments retyped (vka_pso.c fout_masks)
VkShaderModule vka_shader_vin_module(vka_shader *s, uint32_t umask, uint32_t smask);   // the base module with those float inputs as uvec4 / ivec4 (vka_spv_vin_retype), made once per masks   // the module of that variant (DMSH_VK_MOD_*), made on first use; VK_NULL_HANDLE = the entry has none   // the shader of key in library L (the game's, or the selftest's own); NULL = not there or refused
int vka_shlib_check_all(const vka_shlib *L);   // every module and compute pipeline of L made (DM_VK_SHLIB_CHECK=1): the failures
int vka_static(VkDeviceSize n, VkDeviceSize align, vka_span *out);   // host-visible memory for the process's life (c1 bytes, the support and zero buffers)

// ---------- Pipelines (vka_pso.c) ----------
typedef struct {   // what a graphics pipeline is made from beyond its shaders and layout: zero-filled, then compared bytewise
    uint32_t cfmt[8], dfmt, sfmt;     // attachment formats (VK_FORMAT_UNDEFINED = none)
    uint8_t ncolor, samples, topo, poly_mode, depth_clamp, a2c, logic_en, logic_op, gl_depth, fs_variant, alpha_func, gs_fx;   // fs_variant: DMSH_VK_MOD_*; alpha_func: NVN_ALPHA_FUNC_* or 0; gs_fx: 1 = the geometry module with the claim (vka_shader_gs_module)
    uint32_t vin_u, vin_s;            // float vertex inputs fed UINT / SINT formats: retyped in the vertex module (vka_spvpatch.c), by location
    struct { uint8_t en, src, dst, op, srca, dsta, opa, mask; } att[8];
    uint32_t vin_static;              // no vertex input dynamic state: the attributes and bindings below are part of the pipeline
    struct { uint32_t fmt, offset; uint8_t binding, used, pad[2]; } va[32];   // by location
    struct { uint32_t stride, divisor; uint8_t used, pad[3]; } vb[33];         // by binding (stream; 32 = the zero buffer of inputs no attribute feeds)
} vka_gkey;
typedef struct { VkPipelineLayout layout; VkDescriptorSetLayout set0; int push; uint32_t ndesc; } vka_layout;
int vka_layout_of(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out);   // the set-0 layout of these stages' records with the pools' set 1
VkPipeline vka_gfx_pipeline(vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k);
VkPipeline vka_cs_pipeline(vka_shader *cs, const vka_layout *lay);
void vka_pso_save(int force);         // the VkPipelineCache and the prewarm list to the app's files (DM_TEMP): due DM_VK_PCACHE_SAVE_S after the last save, on the saver thread; force = now, on the caller's
void vka_pso_waits(uint64_t *save_ns, uint64_t *pw_wait_ns);   // cumulative: time saving the cache (saver thread), the frame path's waits for a prewarm thread making the same pipeline
double vka_gpu_span_ms(uint64_t f);   // frame f's submissions from first part's start to last part's end (vka_exec.c); -1 = not in yet

// ---------- Draws and dispatches (vka_draw.c) ----------
// The NVN state a queue keeps between submissions, as nvn_mtl.m's S (state_reset gives NVN's defaults)
typedef struct {
    vka_bstate b;
    const prog_t *bst[VKA_NSTAGE], *cprog;   // the program bound per graphics stage (NVN stage bits; [5] unused, [6] / [7] NVN2's task and mesh bits), the compute program
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp, depth_clamp; uint8_t vp_sw[16][4];
    float blend_color[4], alpha_ref;
    p_prim_restart restart; int rast_discard, tf_on;
    int inited;
    uint32_t bind_gen;                 // raised by every state record but a uniform buffer bind (vka_draw.c's instanced runs: a draw that joins without its walk)
    uint32_t ubo_dirty[VKA_NSTAGE];    // uniform buffer slots bound since the open run's last draw, a bit per slot
} vka_dstate;
void vka_dstate_reset(vka_dstate *d);
int vka_classic_rp(void);   // DM_VK_CLASSIC_RP: the game's passes as VkRenderPasses with imageless framebuffers (vka_draw.c)
VkRenderPass vka_rp_compat(const VkFormat *cf, uint32_t n, VkFormat df, VkFormat sf, uint32_t samples);   // a render pass pipelines of those targets are made against
int vka_state_op(vka_dstate *d, uint32_t op, const void *a);   // 1 = a state record, taken
// The render pass the draws of a submission go into: opened at a draw, closed by anything that is not a draw or state (vka_rp_end), and by a new SET_RT
void vka_draw_sub_begin(VkCommandBuffer cb, unsigned slot);    // the slot's per-draw descriptor sets are free again (its last fence is done)
void vka_rp_end(void);
int vka_rp_split_ok(const p_set_rt *rt, int has_rt);
void vka_draw_cb_switch(VkCommandBuffer cb);
void vka_dyn_invalidate(void);                       // the draws' dynamic state is set again in full at the next draw (another pipeline was bound)
int vka_pass_keep(void);                             // DM_VK_PASS_KEEP=0: a SET_RT ends the pass, as before
int vka_rp_open_for(const p_set_rt *rt, int has_rt);   // a pass is open over these targets
int vka_rp_game_barrier(uint32_t bits);                // a game barrier (OP_BARRIER): 1 = taken inside the open pass (DM_VK_BAR_KEEP), 0 = the caller ends the pass and records a full barrier
void vka_rp_bar_stats(char *b, size_t n, double frames);
void vka_rp_prep_clear(const p_set_rt *rt, int has_rt); // before a clear of these targets: a pass or pending clears of other targets end
// A clear of a bound target: everything the clear outside a pass needs (vka_clear_outside, vka_exec.c), and the value for the attachment inside one
typedef struct { int ci; vka_img *im; VkFormat fmt; VkImageAspectFlags asp; uint32_t lv, l0, nl; VkRect2D rc; int whole; VkClearValue out, in; } vka_pclear;
void vka_clear_outside(const vka_pclear *c);         // the clear on the image, outside any pass, then a barrier
// A clear of bound target c->ci (< 0: depth / stencil) into the render pass over those targets: an open one takes it as vkCmdClearAttachments; with none open it waits for
// the next draw's pass (loadOp CLEAR), or is done outside when anything else comes first (vka_rp_end). 0 = not taken: the caller ends the pass and clears outside
int vka_rp_clear(const p_set_rt *rt, int has_rt, const vka_dstate *d, const vka_pclear *c);
void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di);
void vka_dispatch(vka_dstate *d, const p_dispatch *dp);
// Whether the game is a Switch 2 one (NVN2: its compute reads the group counts at c0 + DMSH_C0_DISPATCH_NVN2): dm_sdk_ounce() unless set (the replayer sets it from the capture)
int vka_nvn2(void);
void vka_set_nvn2(int on);
void vka_draw_force(int on);   // draws on whatever the memory (the selftest, whose memory may be a mapped allocation)
// DM_VK_NANSCAN (vka_nanscan.c): NaN / infinite texels in each pass's float attachments, logged with the pass's shaders
int vka_nan_on(void);
// The command encoder thread (vka_enc.c, DM_VK_ENC): the replay thread's vkCmd* calls into its command buffer, made on a thread of their own
void vka_enc_install(void);                 // vka_dev.c: the vk table's vkCmd* entries go through the encoder
void vka_enc_start(VkCommandBuffer cb);     // after vkBeginCommandBuffer on the replay thread: its calls into cb are deferred (when DM_VK_ENC is on)
void vka_enc_stop(void);                    // before vkEndCommandBuffer / a submit / a pool use: every deferred call made, none deferred from now on
void vka_enc_sync(void);                    // every deferred call made (a direct call that must land in order follows)
int vka_enc_on(void);
int vka_enc_call(void (*fn)(const void *), const void *arg, size_t n);   // fn on the encoder thread in order (1), or 0: encoder off
void vka_enc_switch(VkCommandBuffer cb);    // recording goes on in cb (queued reset / begin)
void vka_enc_stats(char *b, size_t n, double frames);   // the replay statistics' line (empty when unused)
int vka_nan_split(vka_img *const *ims, int n);
int vka_nan_probe(uint64_t fs);
void vka_nan_inputs(VkCommandBuffer cb, vka_img *const *ims, const int *binds, const int *tids, int n);
vka_img *vka_tex_id_img(int id, VkCommandBuffer cb);
void vka_tex_id_dump(int id);
void vka_tex_id_check(int id);                      // DM_VK_NANSCAN: has texture ID id's storage changed since its upload, and does the tracker know
// GPU writes into guest memory, as recorded (vka_tex.c gw_t): textures over them are uploaded with their bytes, or again once they are done
void vka_tex_gw_copy(uint64_t src, uint64_t dst, uint64_t n);
void vka_tex_gw_fill(uint64_t dst, uint64_t n, uint32_t value);
void vka_tex_gw_update(uint64_t dst, uint64_t n, const void *data);
void vka_tex_gw_opaque(uint64_t dst, uint64_t n);   // bytes the host does not know at recording (shader stores, texture → buffer copies)
int vka_tex_gml_take(VkDescriptorBufferInfo *bi);  // a raw global memory write log for one draw or dispatch, read when its submission is done (0 = none free)
void vka_tex_gw_commit(uint64_t fence);
void vka_tex_watch_dispatch(uint64_t key);         // DM_VK_NANSCAN: compute shaders by submission, for the storage watch             // the writes recorded since the last commit belong to this submission
void vka_nan_draw(uint64_t vs, uint64_t fs);
void vka_nan_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n);
void vka_nan_commit(uint64_t fence);
void vka_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *dispatches, uint64_t *pipelines);

// ---------- Present (vka_present.c) ----------
int vka_origin_ll(void);
int vka_queue_origin_ll(void);   // the submitting queue's raster origin (nvnQueueBuilder flags): the winding flips with it
int vka_depth_zero(void);        // nvnDeviceSetDepthMode NEAR_IS_ZERO (else GL's near = -w)
void vka_present_init(void);
// The overlay's counts (vka_fps.c): each image the present thread handed to the swapchain (shown), the game's own or a generated one; frame generation's state
void vka_fps_note(int shown, int generated);
void vka_fps_fg(int state, int mult);
// Frame generation (vka_framegen.c; LSFG in lsfg/, GPL-3.0-or-later): for the game frame about to be presented (its window texture, crop, period and timeline value), the images
// to present before it (gen, at most VKA_FG_MAX, the present takes its own references), how long each image of this frame should stay (hold, seconds; 0 = the game's period)
// and the output period for the window's frame rate (0 = frame generation off). Present thread only
enum { VKA_FG_MAX = 3 };
int vka_fg_frame(uint64_t serial, const int *crop, double period, uint64_t fence, vka_img **gen, double *hold, double *out_period);
uint64_t vka_refresh_ns(void);   // vka_present.c: the panel's refresh period in ns from display timing, 0 when not known (present thread)
