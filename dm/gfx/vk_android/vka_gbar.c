// The game's barriers between compute dispatches (Android): recorded as the narrowest Vulkan barrier that keeps NVN's ordering (the choice: vka_gbar.h).
// A game whose compute dispatches are many and small (Pokémon Pokopia's GPU particle emitters: up to 1,500 1-group dispatches a frame, each followed by the game's barrier)
// paid a full ALL_COMMANDS barrier for each, about 8 us of Adreno GPU time apiece (docs/reference/rendering.md "A full barrier between the game's compute dispatches").
// A game barrier taken outside a draw pass (vka_exec.c OP_BARRIER) is left pending here when nothing but dispatches and state was recorded since the last full barrier; the
// command recorded next into the replay's command buffer decides what it becomes. To see every command, the vk table's work commands go through here (vka_gbar_install,
// on top of the command encoder's entries): copies, fills, updates, clears, blits, resolves, a pass's begin, dispatches and pipeline barriers. State commands, and the draws
// and clears inside a pass (a pass begins with a work command, which settles anything pending), are not wrapped. Commands into any other command buffer, or from any other
// thread, pass straight through.
// DM_VK_BAR_NARROW=0 (live) records every game barrier as a full barrier at once, as before.
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_gbar.h"
#include <stdio.h>

static _Atomic(VkCommandBuffer) g_tcb;   // the replay's command buffer being recorded (vka_gbar_cb); the state below is the replay thread's
static vkgb G; static vkgb_stats GS, GS0;

#define GB_FNS(X) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdBeginRenderPass) \
    X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDispatchBase)
#define GB_DECL(f) PFN_##f f;
static struct { GB_FNS(GB_DECL) } N;   // the next layer (the encoder's entries, or the driver's)

static void rec(VkCommandBuffer cb, int b, int ind) {
    VkPipelineStageFlags src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
    if (b == VKGB_B_NARROW) {   // compute writes to the next dispatches' reads and writes (and their indirect group counts)
        dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | (ind ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT : 0);
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | (ind ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : 0);
    } else if (b == VKGB_B_INDIRECT) { dst = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT; mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT; }
    else { dst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; }   // VKGB_B_SETTLE
    N.vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}
static inline void pre(VkCommandBuffer cb, int kind) {
    if (!cb || cb != atomic_load_explicit(&g_tcb, memory_order_relaxed)) return;
    int ind, b = vkgb_cmd(&G, kind, &ind, &GS);
    if (b) rec(cb, b, ind);
}
#define W(f, kind, params, ...) static void VKAPI_CALL g_##f params { pre(cb, kind); N.f(cb, __VA_ARGS__); }
W(vkCmdCopyBuffer, VKGB_C_WORK, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r), s, d, n, r)
W(vkCmdUpdateBuffer, VKGB_C_WORK, (VkCommandBuffer cb, VkBuffer d, VkDeviceSize o, VkDeviceSize n, const void *p), d, o, n, p)
W(vkCmdFillBuffer, VKGB_C_WORK, (VkCommandBuffer cb, VkBuffer d, VkDeviceSize o, VkDeviceSize n, uint32_t v), d, o, n, v)
W(vkCmdCopyBufferToImage, VKGB_C_WORK, (VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r), b, im, l, n, r)
W(vkCmdCopyImageToBuffer, VKGB_C_WORK, (VkCommandBuffer cb, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r), im, l, b, n, r)
W(vkCmdCopyImage, VKGB_C_WORK, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r), s, sl, d, dl, n, r)
W(vkCmdBlitImage, VKGB_C_WORK, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f), s, sl, d, dl, n, r, f)
W(vkCmdResolveImage, VKGB_C_WORK, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r), s, sl, d, dl, n, r)
W(vkCmdClearColorImage, VKGB_C_WORK, (VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r), im, l, v, n, r)
W(vkCmdClearDepthStencilImage, VKGB_C_WORK, (VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r), im, l, v, n, r)
W(vkCmdBeginRendering, VKGB_C_WORK, (VkCommandBuffer cb, const VkRenderingInfo *ri), ri)
W(vkCmdBeginRenderPass, VKGB_C_WORK, (VkCommandBuffer cb, const VkRenderPassBeginInfo *rb, VkSubpassContents sc), rb, sc)
W(vkCmdDispatch, VKGB_C_DISPATCH, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), x, y, z)
W(vkCmdDispatchIndirect, VKGB_C_DISPATCH_IND, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), b, o)
W(vkCmdDispatchBase, VKGB_C_DISPATCH, (VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z), bx, by, bz, x, y, z)
// A full barrier (vka_barrier_at: every stage, every write to every read) covers anything pending; any other barrier (an image's layout change, the submission's end
// toward the host) is work of its own: what is pending is recorded before it
static void VKAPI_CALL g_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
                                              uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    int full = 0;
    if (s == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && d == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
        for (uint32_t k = 0; k < nm && !full; k++) full = (m[k].srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT) && (m[k].dstAccessMask & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    pre(cb, full ? VKGB_C_FULL : VKGB_C_WORK);
    N.vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, b, ni, i);
}
#define GB_HOOK(f) N.f = vk.f; if (N.f) vk.f = g_##f;
void vka_gbar_install(void) {   // vka_dev.c, after vka_enc_install
    static int done; if (done) return; done = 1;
    GB_FNS(GB_HOOK)
}
// The replay's command buffer: a submission's first, or the next part's (vka_exec.c). What came before it is not known until its first full barrier
void vka_gbar_cb(VkCommandBuffer cb) { vkgb_reset(&G); atomic_store_explicit(&g_tcb, cb, memory_order_relaxed); }
// Before the command buffer ends (a part of the submission goes to the queue): what is pending or owed, recorded (a barrier in one part covers the parts after it)
void vka_gbar_settle(VkCommandBuffer cb) { pre(cb, VKGB_C_WORK); }
// A game barrier outside a draw pass (vka_exec.c OP_BARRIER): 1 = taken, 0 = the caller records the full barrier
int vka_gbar_game(VkCommandBuffer cb, uint32_t bits) {
    if (!cb || cb != atomic_load_explicit(&g_tcb, memory_order_relaxed) || vka_rp_is_open()) { GS.full++; return 0; }
    if (!vkgb_game(&G, bits, VKA_ENV("DM_VK_BAR_NARROW", 1) != 0, &GS)) return 0;
    // The dispatches before it need no barrier of the host's after them ("Dispatch order"): this one is recorded before any other work
    { extern uint64_t vka_disp_cleared; VkCommandBuffer p = cb; if (atomic_compare_exchange_strong(&vka_disp_pend, &p, VK_NULL_HANDLE)) vka_disp_cleared++; }
    return 1;
}
// The replay statistics line (vka_exec.c, every 300 frames)
void vka_gbar_stats(char *b, size_t n, double frames) {
    vkgb_stats d = {GS.full - GS0.full, GS.narrow - GS0.narrow, GS.merged - GS0.merged, GS.settled - GS0.settled, GS.owed - GS0.owed, GS.indirect - GS0.indirect};
    GS0 = GS;
    if (frames <= 0 || !(d.full + d.narrow + d.merged)) { if (n) b[0] = 0; return; }
    snprintf(b, n, "game barriers outside a draw pass per frame: %.1f full, %.1f compute-to-compute, %.1f merged; barriers before other work after them %.1f pending, %.1f owed, %.1f for indirect reads (DM_VK_BAR_NARROW)",
             d.full / frames, d.narrow / frames, d.merged / frames, d.settled / frames, d.owed / frames, d.indirect / frames);
}
#endif
