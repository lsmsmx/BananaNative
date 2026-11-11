// The command encoder thread (DM_VK_ENC, live, default 1): the replay thread's vkCmd* calls into the submission's command buffer are copied into a ring and made on a
// second thread, so the replay thread's own work (walking the game's commands, descriptors, pipeline lookups) and the driver's recording (about a quarter of the replay
// thread's time on the Adreno 840) run side by side.
//
// - Only calls into the command buffer the replay thread records (vka_enc_start ... vka_enc_stop) are deferred; every other thread, and that thread for any other
//   command buffer, calls the driver directly (the vk table's vkCmd* entries go through here: vka_enc_install).
// - Arguments are copied when the call is made (arrays and the structures they point to; a pNext chain the encoder does not copy makes the call wait for the ring and go
//   directly), so the caller may reuse its memory at once. Objects the calls name must stay alive until the submission's fence (as for any recorded command).
// - vka_enc_stop (before vkEndCommandBuffer, a part's submit or any other use of the command pool) waits until the encoder thread has made every call: the command
//   buffer is then complete and the pool is free. vka_enc_sync does that in the middle of recording, for a direct call that must land in order (the GPU profiler's
//   timestamps).
// Single producer (the replay thread), single consumer (the encoder thread); the consumer spins for a short while when the ring runs empty and then sleeps on a futex.
#if defined(__ANDROID__)
#include "vka.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define ENC_FNS(X) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDispatchBase) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetBlendConstants) X(vkCmdSetLineWidth) \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdSetVertexInputEXT) X(vkCmdPushConstants) X(vkCmdDrawMeshTasksEXT) X(vkCmdDrawMeshTasksIndirectEXT) \
    X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)
#define ENC_ID(f) E_##f,
enum { E_WRAP = 0, E_CALL, ENC_FNS(ENC_ID) E_N };
#define ENC_REAL(f) PFN_##f f;
static struct { ENC_FNS(ENC_REAL) } R;   // the driver's entry points

enum { RING = 8 << 20 };   // bytes; a frame's ~8,000 draws make ~2 MB of calls
static uint8_t *g_ring;
static _Atomic uint64_t g_head, g_tail;   // bytes made / taken (positions mod RING)
static uint64_t g_ph;                      // the producer's head (published at each call's end)
static _Atomic int g_csleep, g_pwait;      // the consumer sleeps on g_cseq; the producer waits on g_pseq
static _Atomic uint32_t g_cseq, g_pseq;
static _Atomic uint64_t g_pwant;          // the tail the producer waits for
static _Atomic(VkCommandBuffer) g_ecb;
static _Atomic int g_running = -1;        // the record the encoder thread is in (-1: none), for the stall line    // the command buffer whose calls are deferred (NULL: none)
static int g_up;                           // 1 thread running, -1 failed
static uint64_t g_n_calls, g_n_direct, g_n_waits, g_wait_ns; static _Atomic uint64_t g_busy_ns;   // statistics (vka_enc_stats)
static _Atomic uint64_t g_wait_all;   // cumulative: the replay thread's waits for the encoder (vka_enc_waits, the [longframe] line)
uint64_t vka_enc_waits(void) { return atomic_load(&g_wait_all); }

typedef struct { uint32_t id, size; } hdr;   // size: the whole record, 8-byte multiple
#define A8(n) (((size_t)(n) + 7) & ~(size_t)7)

static long fwait(_Atomic uint32_t *a, uint32_t v) { struct timespec t = {1, 0}; return syscall(SYS_futex, (uint32_t *)a, FUTEX_WAIT_PRIVATE, v, &t, NULL, 0); }   // (bounded: the waits check again)   // (bounded: the waits check again)
static void fwake(_Atomic uint32_t *a) { syscall(SYS_futex, (uint32_t *)a, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); }
// Waits (low power) until *a may have changed from old: the exclusive monitor on its cache line wakes WFE when another core writes it (the event stream bounds the wait)
static inline uint64_t wait_change(_Atomic uint64_t *a, uint64_t old) {
    uint64_t v; __asm__ volatile("ldaxr %0, [%1]" : "=r"(v) : "r"(a) : "memory");
    if (v == old) __asm__ volatile("wfe" ::: "memory");
    return v;
}
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// How long either thread waits with WFE before the futex (DM_VK_ENC_SPIN). The clock is read at every wake, not every so many wakes: with nothing stored to the line, WFE
// returns only at the kernel's event stream tick (10 kHz), so a count of wakes is a count of 100 us ticks. A thread on WFE still runs: it counts as CPU time, keeps its core
// out of idle and looks busy to the frequency governor
static _Atomic uint64_t g_spin_ns = 200000;
static inline int spin_over(uint64_t t0) { return mono_ns() - t0 >= atomic_load_explicit(&g_spin_ns, memory_order_relaxed); }   // checked before each wait: 0 goes straight to the futex

// The producer waits until the consumer's tail reaches at least `want` (spinning a little, then on the futex)
static inline void flush_pub(void);
static void wait_tail(uint64_t want) {
    flush_pub();   // what it waits for must be handed over
    for (uint64_t t0_ = mono_ns();;) {   // the encoder is usually close behind: wait on its tail for a short while
        uint64_t t = atomic_load_explicit(&g_tail, memory_order_acquire); if (t >= want) return;
        if (spin_over(t0_)) break;
        wait_change(&g_tail, t);
    }
    g_n_waits++; uint64_t t0 = mono_ns();
    // An event count: the sequence is read before the flag is set, so an encoder that sees the flag (and a tail at or past want) bumps it after that read and the futex
    // wait returns. Only this thread clears its flag (a clear by the encoder could hit the flag of the next wait)
    atomic_store(&g_pwant, want);
    for (;;) {
        uint32_t s = atomic_load(&g_pseq);
        atomic_store(&g_pwait, 1);
        atomic_thread_fence(memory_order_seq_cst);   // the flag before the tail is read again (the encoder stores the tail, then reads the flag)
        if (atomic_load(&g_tail) >= want) break;
        fwait(&g_pseq, s);
        uint64_t el = mono_ns() - t0;
        if (el > 500000000ull) { static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 8) VRED("command encoder: the replay thread has waited %.1f s for the encoder (tail %llu, want %llu, head %llu, encoder %s, in record %d)", el / 1e9,
            (unsigned long long)atomic_load(&g_tail), (unsigned long long)want, (unsigned long long)atomic_load(&g_head), atomic_load(&g_csleep) ? "asleep" : "awake", atomic_load(&g_running)); }
    }
    atomic_store(&g_pwait, 0);
    { uint64_t w = mono_ns() - t0; g_wait_ns += w; atomic_fetch_add_explicit(&g_wait_all, w, memory_order_relaxed); }
}
static inline void publish(void) {
    atomic_store(&g_head, g_ph);
    atomic_thread_fence(memory_order_seq_cst);   // the head before the encoder's sleep flag is read
    if (atomic_load(&g_csleep) && atomic_exchange(&g_csleep, 0)) { atomic_fetch_add(&g_cseq, 1); fwake(&g_cseq); }   // one wake a sleep (clearing a flag of the encoder's next sleep is safe: it read the sequence before setting it, and the bump follows)
}
// Room for a record of n bytes (8-byte multiple), contiguous in the ring
static void *rec_alloc(uint32_t id, size_t n) {
    uint64_t pos = g_ph % RING;
    if (pos + n > RING) {   // a wrap record fills the rest of the ring
        uint64_t rest = RING - pos;
        if (g_ph + rest - atomic_load_explicit(&g_tail, memory_order_acquire) > RING) wait_tail(g_ph + rest - RING);
        ((hdr *)(g_ring + pos))->id = E_WRAP; ((hdr *)(g_ring + pos))->size = (uint32_t)rest;
        g_ph += rest; pos = 0;
    }
    if (g_ph + n - atomic_load_explicit(&g_tail, memory_order_acquire) > RING) wait_tail(g_ph + n - RING);
    hdr *h = (hdr *)(g_ring + pos); h->id = id; h->size = (uint32_t)n;
    return h;
}
// Calls are handed over in batches of DM_VK_ENC_BATCH bytes (default 4096; 0 = each call), and at once at every wait, sync, part switch and stop: each store to the
// head wakes the encoder from WFE, and a frame makes ~45,000 calls. One wake a call kept the encoder thread busy on a prime core between calls (profile p2: 70 % of its
// cycles in its own loop: power and heat). Nothing reaches the GPU before a part's submit, which flushes, so the batch only delays when the encoder starts on a call
static uint64_t g_pub;        // the head published last
static uint32_t g_batch = 4096;
static uint64_t g_n_total;   // every call recorded, never reset (vka_enc_total: the instanced runs' "nothing recorded in between")
static inline void rec_done(size_t n) { g_ph += n; g_n_calls++; g_n_total++; if (g_ph - g_pub >= g_batch) { g_pub = g_ph; publish(); } }
static inline void flush_pub(void) { if (g_pub != g_ph) { g_pub = g_ph; publish(); } }

// ---------- The records: each holds the call's arguments, pointers aimed at copies inside the record ----------
#define CB_FIELD hdr h; VkCommandBuffer cb;
typedef struct { CB_FIELD VkPipelineStageFlags s, d; VkDependencyFlags f; uint32_t nm, nb, ni; const VkMemoryBarrier *m; const VkBufferMemoryBarrier *b; const VkImageMemoryBarrier *i; } r_bar;
typedef struct { CB_FIELD VkBuffer s, d; uint32_t n; const VkBufferCopy *r; } r_cpb;
typedef struct { CB_FIELD VkBuffer d; VkDeviceSize o, n; const void *p; } r_upd;
typedef struct { CB_FIELD VkBuffer d; VkDeviceSize o, n; uint32_t v; } r_fill;
typedef struct { CB_FIELD VkBuffer b; VkImage im; VkImageLayout l; uint32_t n; const VkBufferImageCopy *r; } r_b2i;
typedef struct { CB_FIELD VkImage s, d; VkImageLayout sl, dl; uint32_t n; const void *r; VkFilter f; } r_i2i;   // copy, blit, resolve
typedef struct { CB_FIELD VkImage im; VkImageLayout l; VkClearValue v; uint32_t n; const VkImageSubresourceRange *r; } r_clr;
typedef struct { CB_FIELD uint32_t na, nr; const VkClearAttachment *a; const VkClearRect *r; } r_clra;
typedef struct { CB_FIELD VkRenderingInfo ri; } r_rend;
typedef struct { CB_FIELD VkRenderPassBeginInfo rb; VkRenderPassAttachmentBeginInfo ab; VkSubpassContents sc; } r_rpb;
typedef struct { CB_FIELD VkPipelineBindPoint bp; VkPipeline p; } r_pipe;
typedef struct { CB_FIELD VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; const VkDescriptorSet *s; const uint32_t *d; } r_sets;
typedef struct { CB_FIELD uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o, *sz, *st; } r_vb;
typedef struct { CB_FIELD VkBuffer b; VkDeviceSize o; VkIndexType t; } r_ib;
typedef struct { CB_FIELD uint32_t a, b, c, d; int32_t e; uint32_t f; } r_draw;   // draw (a count, b instances, c first, d first instance), indexed (e vertex offset, f first instance), dispatch (vkCmdDispatchBase: base a b c, groups d f e)
typedef struct { CB_FIELD VkBuffer b; VkDeviceSize o; uint32_t n, st; } r_ind;
typedef struct { CB_FIELD VkBuffer b, cb2; VkDeviceSize o, o2; uint32_t n, st; } r_indc;   // an indirect-count draw: the records, the count, at most n records st bytes apart
typedef struct { CB_FIELD uint32_t first, n; const void *p; } r_vps;   // viewports or scissors
typedef struct { CB_FIELD uint32_t v[5]; float f[4]; } r_val;               // the one-value states
typedef struct { CB_FIELD VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t set, n; const VkWriteDescriptorSet *w; } r_push;
typedef struct { CB_FIELD uint32_t nb, na; const VkVertexInputBindingDescription2EXT *b; const VkVertexInputAttributeDescription2EXT *a; } r_vin;
typedef struct { CB_FIELD VkPipelineLayout l; VkShaderStageFlags st; uint32_t o, n; const void *p; } r_pc;
typedef struct { CB_FIELD void (*fn)(const void *); } r_call;   // a function of the replay's own, made in order with the calls (its argument bytes follow)

static inline int mine(VkCommandBuffer cb) { return cb && cb == atomic_load_explicit(&g_ecb, memory_order_relaxed); }
static inline uint8_t *cpy(uint8_t **at, const void *src, size_t n) { uint8_t *d = *at; if (n) memcpy(d, src, n); *at += A8(n); return d; }
#define BEGIN(T, id, extra) T *r = rec_alloc(id, A8(sizeof(T)) + (extra)); uint8_t *at = (uint8_t *)r + A8(sizeof(T)); (void)at; r->cb = cb;
#define END() rec_done(r->h.size)
#define DIRECT(f, ...) do { if (!mine(cb)) { R.f(__VA_ARGS__); return; } } while (0)
#define FALLBACK(f, ...) do { g_n_direct++; g_n_total++; vka_enc_sync(); R.f(__VA_ARGS__); return; } while (0)

static void VKAPI_CALL e_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    DIRECT(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    for (uint32_t k = 0; k < nm; k++) if (m[k].pNext) FALLBACK(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    for (uint32_t k = 0; k < nb; k++) if (b[k].pNext) FALLBACK(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    for (uint32_t k = 0; k < ni; k++) if (i[k].pNext) FALLBACK(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    BEGIN(r_bar, E_vkCmdPipelineBarrier, A8(nm * sizeof *m) + A8(nb * sizeof *b) + A8(ni * sizeof *i));
    r->s = s; r->d = d; r->f = f; r->nm = nm; r->nb = nb; r->ni = ni;
    r->m = (void *)cpy(&at, m, nm * sizeof *m); r->b = (void *)cpy(&at, b, nb * sizeof *b); r->i = (void *)cpy(&at, i, ni * sizeof *i);
    END();
}
static void VKAPI_CALL e_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *rg) {
    DIRECT(vkCmdCopyBuffer, cb, s, d, n, rg);
    BEGIN(r_cpb, E_vkCmdCopyBuffer, A8(n * sizeof *rg)); r->s = s; r->d = d; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer d, VkDeviceSize o, VkDeviceSize n, const void *p) {
    DIRECT(vkCmdUpdateBuffer, cb, d, o, n, p);
    if (n > 65536) FALLBACK(vkCmdUpdateBuffer, cb, d, o, n, p);
    BEGIN(r_upd, E_vkCmdUpdateBuffer, A8(n)); r->d = d; r->o = o; r->n = n; r->p = cpy(&at, p, (size_t)n); END();
}
static void VKAPI_CALL e_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer d, VkDeviceSize o, VkDeviceSize n, uint32_t v) {
    DIRECT(vkCmdFillBuffer, cb, d, o, n, v);
    BEGIN(r_fill, E_vkCmdFillBuffer, 0); r->d = d; r->o = o; r->n = n; r->v = v; END();
}
static void VKAPI_CALL e_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg) {
    DIRECT(vkCmdCopyBufferToImage, cb, b, im, l, n, rg);
    BEGIN(r_b2i, E_vkCmdCopyBufferToImage, A8(n * sizeof *rg)); r->b = b; r->im = im; r->l = l; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *rg) {
    DIRECT(vkCmdCopyImageToBuffer, cb, im, l, b, n, rg);
    BEGIN(r_b2i, E_vkCmdCopyImageToBuffer, A8(n * sizeof *rg)); r->b = b; r->im = im; r->l = l; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *rg) {
    DIRECT(vkCmdCopyImage, cb, s, sl, d, dl, n, rg);
    BEGIN(r_i2i, E_vkCmdCopyImage, A8(n * sizeof *rg)); r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->r = cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f) {
    DIRECT(vkCmdBlitImage, cb, s, sl, d, dl, n, rg, f);
    BEGIN(r_i2i, E_vkCmdBlitImage, A8(n * sizeof *rg)); r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->f = f; r->r = cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *rg) {
    DIRECT(vkCmdResolveImage, cb, s, sl, d, dl, n, rg);
    BEGIN(r_i2i, E_vkCmdResolveImage, A8(n * sizeof *rg)); r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->r = cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdClearColorImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    DIRECT(vkCmdClearColorImage, cb, im, l, v, n, rg);
    BEGIN(r_clr, E_vkCmdClearColorImage, A8(n * sizeof *rg)); r->im = im; r->l = l; r->v.color = *v; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    DIRECT(vkCmdClearDepthStencilImage, cb, im, l, v, n, rg);
    BEGIN(r_clr, E_vkCmdClearDepthStencilImage, A8(n * sizeof *rg)); r->im = im; r->l = l; r->v.depthStencil = *v; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *rc) {
    DIRECT(vkCmdClearAttachments, cb, na, a, nr, rc);
    BEGIN(r_clra, E_vkCmdClearAttachments, A8(na * sizeof *a) + A8(nr * sizeof *rc)); r->na = na; r->nr = nr; r->a = (void *)cpy(&at, a, na * sizeof *a); r->r = (void *)cpy(&at, rc, nr * sizeof *rc); END();
}
static void VKAPI_CALL e_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    DIRECT(vkCmdBeginRendering, cb, ri);
    int pn = ri->pNext != NULL;
    for (uint32_t k = 0; k < ri->colorAttachmentCount; k++) pn |= ri->pColorAttachments[k].pNext != NULL;
    if (ri->pDepthAttachment) pn |= ri->pDepthAttachment->pNext != NULL;
    if (ri->pStencilAttachment) pn |= ri->pStencilAttachment->pNext != NULL;
    if (pn) FALLBACK(vkCmdBeginRendering, cb, ri);
    size_t na = ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo), one = sizeof(VkRenderingAttachmentInfo);
    BEGIN(r_rend, E_vkCmdBeginRendering, A8(na) + 2 * A8(one));
    r->ri = *ri;
    r->ri.pColorAttachments = (void *)cpy(&at, ri->pColorAttachments, na);
    r->ri.pDepthAttachment = ri->pDepthAttachment ? (void *)cpy(&at, ri->pDepthAttachment, one) : NULL;
    r->ri.pStencilAttachment = ri->pStencilAttachment ? (void *)cpy(&at, ri->pStencilAttachment, one) : NULL;
    END();
}
static void VKAPI_CALL e_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *rb, VkSubpassContents sc) {   // (with or without one VkRenderPassAttachmentBeginInfo)
    DIRECT(vkCmdBeginRenderPass, cb, rb, sc);
    const VkRenderPassAttachmentBeginInfo *ab = rb->pNext;
    if (ab && (ab->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO || ab->pNext)) FALLBACK(vkCmdBeginRenderPass, cb, rb, sc);
    size_t nv = ab ? ab->attachmentCount * sizeof(VkImageView) : 0, nc = rb->clearValueCount * sizeof(VkClearValue);
    BEGIN(r_rpb, E_vkCmdBeginRenderPass, A8(nv) + A8(nc));
    r->rb = *rb; r->sc = sc;
    r->rb.pClearValues = nc ? (void *)cpy(&at, rb->pClearValues, nc) : NULL;
    if (ab) { r->ab = *ab; r->ab.pAttachments = (void *)cpy(&at, ab->pAttachments, nv); r->rb.pNext = NULL; }   // (pNext set at the replay: the record moves)
    else r->ab.sType = 0;
    END();
}
static void VKAPI_CALL e_vkCmdEndRenderPass(VkCommandBuffer cb) { DIRECT(vkCmdEndRenderPass, cb); BEGIN(r_val, E_vkCmdEndRenderPass, 0); END(); }
static void VKAPI_CALL e_vkCmdEndRendering(VkCommandBuffer cb) { DIRECT(vkCmdEndRendering, cb); BEGIN(r_val, E_vkCmdEndRendering, 0); END(); }
static void VKAPI_CALL e_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    DIRECT(vkCmdBindPipeline, cb, bp, p); BEGIN(r_pipe, E_vkCmdBindPipeline, 0); r->bp = bp; r->p = p; END();
}
static void VKAPI_CALL e_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    DIRECT(vkCmdBindDescriptorSets, cb, bp, l, first, n, s, nd, d);
    BEGIN(r_sets, E_vkCmdBindDescriptorSets, A8(n * sizeof *s) + A8(nd * sizeof *d)); r->bp = bp; r->l = l; r->first = first; r->n = n; r->nd = nd;
    r->s = (void *)cpy(&at, s, n * sizeof *s); r->d = nd ? (void *)cpy(&at, d, nd * sizeof *d) : NULL; END();
}
static void VKAPI_CALL e_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *sz, const VkDeviceSize *st) {
    DIRECT(vkCmdBindVertexBuffers2, cb, first, n, b, o, sz, st);
    size_t a = n * sizeof(VkDeviceSize);
    BEGIN(r_vb, E_vkCmdBindVertexBuffers2, A8(n * sizeof *b) + 3 * A8(a)); r->first = first; r->n = n;
    r->b = (void *)cpy(&at, b, n * sizeof *b); r->o = (void *)cpy(&at, o, a); r->sz = sz ? (void *)cpy(&at, sz, a) : NULL; r->st = st ? (void *)cpy(&at, st, a) : NULL; END();
}
static void VKAPI_CALL e_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    DIRECT(vkCmdBindIndexBuffer, cb, b, o, t); BEGIN(r_ib, E_vkCmdBindIndexBuffer, 0); r->b = b; r->o = o; r->t = t; END();
}
static void VKAPI_CALL e_vkCmdDraw(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, uint32_t fi) {
    DIRECT(vkCmdDraw, cb, n, i, f, fi); BEGIN(r_draw, E_vkCmdDraw, 0); r->a = n; r->b = i; r->c = f; r->d = fi; END();
}
static void VKAPI_CALL e_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, int32_t vo, uint32_t fi) {
    DIRECT(vkCmdDrawIndexed, cb, n, i, f, vo, fi); BEGIN(r_draw, E_vkCmdDrawIndexed, 0); r->a = n; r->b = i; r->c = f; r->e = vo; r->f = fi; END();
}
static void VKAPI_CALL e_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st) {
    DIRECT(vkCmdDrawIndirect, cb, b, o, n, st); BEGIN(r_ind, E_vkCmdDrawIndirect, 0); r->b = b; r->o = o; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st) {
    DIRECT(vkCmdDrawIndexedIndirect, cb, b, o, n, st); BEGIN(r_ind, E_vkCmdDrawIndexedIndirect, 0); r->b = b; r->o = o; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t n, uint32_t st) {
    DIRECT(vkCmdDrawIndirectCount, cb, b, o, c, co, n, st); BEGIN(r_indc, E_vkCmdDrawIndirectCount, 0); r->b = b; r->o = o; r->cb2 = c; r->o2 = co; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t n, uint32_t st) {
    DIRECT(vkCmdDrawIndexedIndirectCount, cb, b, o, c, co, n, st); BEGIN(r_indc, E_vkCmdDrawIndexedIndirectCount, 0); r->b = b; r->o = o; r->cb2 = c; r->o2 = co; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawMeshTasksEXT(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    DIRECT(vkCmdDrawMeshTasksEXT, cb, x, y, z); BEGIN(r_draw, E_vkCmdDrawMeshTasksEXT, 0); r->a = x; r->b = y; r->c = z; END();
}
static void VKAPI_CALL e_vkCmdDrawMeshTasksIndirectEXT(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st) {
    DIRECT(vkCmdDrawMeshTasksIndirectEXT, cb, b, o, n, st); BEGIN(r_ind, E_vkCmdDrawMeshTasksIndirectEXT, 0); r->b = b; r->o = o; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    DIRECT(vkCmdDispatch, cb, x, y, z); BEGIN(r_draw, E_vkCmdDispatch, 0); r->a = x; r->b = y; r->c = z; END();
}
static void VKAPI_CALL e_vkCmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) {
    DIRECT(vkCmdDispatchBase, cb, bx, by, bz, x, y, z); BEGIN(r_draw, E_vkCmdDispatchBase, 0); r->a = bx; r->b = by; r->c = bz; r->d = x; r->f = y; r->e = (int32_t)z; END();
}
static void VKAPI_CALL e_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    DIRECT(vkCmdDispatchIndirect, cb, b, o); BEGIN(r_ind, E_vkCmdDispatchIndirect, 0); r->b = b; r->o = o; END();
}
static void VKAPI_CALL e_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    DIRECT(vkCmdSetViewport, cb, first, n, v); BEGIN(r_vps, E_vkCmdSetViewport, A8(n * sizeof *v)); r->first = first; r->n = n; r->p = cpy(&at, v, n * sizeof *v); END();
}
static void VKAPI_CALL e_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *v) {
    DIRECT(vkCmdSetScissor, cb, first, n, v); BEGIN(r_vps, E_vkCmdSetScissor, A8(n * sizeof *v)); r->first = first; r->n = n; r->p = cpy(&at, v, n * sizeof *v); END();
}
#define ONE(f, T) static void VKAPI_CALL e_##f(VkCommandBuffer cb, T v) { DIRECT(f, cb, v); BEGIN(r_val, E_##f, 0); r->v[0] = (uint32_t)v; END(); }
ONE(vkCmdSetCullMode, VkCullModeFlags) ONE(vkCmdSetFrontFace, VkFrontFace) ONE(vkCmdSetPrimitiveTopology, VkPrimitiveTopology)
ONE(vkCmdSetDepthTestEnable, VkBool32) ONE(vkCmdSetDepthWriteEnable, VkBool32) ONE(vkCmdSetDepthCompareOp, VkCompareOp) ONE(vkCmdSetDepthBoundsTestEnable, VkBool32)
ONE(vkCmdSetStencilTestEnable, VkBool32) ONE(vkCmdSetDepthBiasEnable, VkBool32) ONE(vkCmdSetRasterizerDiscardEnable, VkBool32) ONE(vkCmdSetPrimitiveRestartEnable, VkBool32)
static void VKAPI_CALL e_vkCmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags fc, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d) {
    DIRECT(vkCmdSetStencilOp, cb, fc, a, b, c, d); BEGIN(r_val, E_vkCmdSetStencilOp, 0); r->v[0] = fc; r->v[1] = a; r->v[2] = b; r->v[3] = c; r->v[4] = d; END();
}
#define TWO(f) static void VKAPI_CALL e_##f(VkCommandBuffer cb, VkStencilFaceFlags fc, uint32_t v) { DIRECT(f, cb, fc, v); BEGIN(r_val, E_##f, 0); r->v[0] = fc; r->v[1] = v; END(); }
TWO(vkCmdSetStencilCompareMask) TWO(vkCmdSetStencilWriteMask) TWO(vkCmdSetStencilReference)
static void VKAPI_CALL e_vkCmdSetDepthBias(VkCommandBuffer cb, float a, float b, float c) {
    DIRECT(vkCmdSetDepthBias, cb, a, b, c); BEGIN(r_val, E_vkCmdSetDepthBias, 0); r->f[0] = a; r->f[1] = b; r->f[2] = c; END();
}
static void VKAPI_CALL e_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    DIRECT(vkCmdSetBlendConstants, cb, c); BEGIN(r_val, E_vkCmdSetBlendConstants, 0); memcpy(r->f, c, 16); END();
}
static void VKAPI_CALL e_vkCmdSetLineWidth(VkCommandBuffer cb, float w) { DIRECT(vkCmdSetLineWidth, cb, w); BEGIN(r_val, E_vkCmdSetLineWidth, 0); r->f[0] = w; END(); }
static size_t info_size(const VkWriteDescriptorSet *w) {
    if (w->pImageInfo && (w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                          w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || w->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) return w->descriptorCount * sizeof(VkDescriptorImageInfo);
    if (w->pTexelBufferView && (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)) return w->descriptorCount * sizeof(VkBufferView);
    if (w->pBufferInfo) return w->descriptorCount * sizeof(VkDescriptorBufferInfo);
    return 0;
}
static int is_img(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; }
static int is_texel(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; }
// A write's descriptor array (the one its type reads) and its size in bytes; inline uniform blocks and acceleration structures (pNext) are not copied
static const void *info_of(const VkWriteDescriptorSet *w, size_t *n) {
    if (is_img(w->descriptorType)) { *n = w->descriptorCount * sizeof(VkDescriptorImageInfo); return w->pImageInfo; }
    if (is_texel(w->descriptorType)) { *n = w->descriptorCount * sizeof(VkBufferView); return w->pTexelBufferView; }
    *n = w->descriptorCount * sizeof(VkDescriptorBufferInfo); return w->pBufferInfo;
}
static void VKAPI_CALL e_vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    DIRECT(vkCmdPushDescriptorSetKHR, cb, bp, l, set, n, w);
    size_t extra = A8(n * sizeof *w);
    for (uint32_t k = 0; k < n; k++) { size_t sz; if (w[k].pNext || !info_of(&w[k], &sz)) FALLBACK(vkCmdPushDescriptorSetKHR, cb, bp, l, set, n, w); extra += A8(sz); }
    BEGIN(r_push, E_vkCmdPushDescriptorSetKHR, extra); r->bp = bp; r->l = l; r->set = set; r->n = n;
    VkWriteDescriptorSet *ww = (VkWriteDescriptorSet *)cpy(&at, w, n * sizeof *w); r->w = ww;
    for (uint32_t k = 0; k < n; k++) {
        size_t sz; const void *src = info_of(&w[k], &sz); void *p = cpy(&at, src, sz);
        ww[k].pImageInfo = NULL; ww[k].pBufferInfo = NULL; ww[k].pTexelBufferView = NULL;
        if (is_img(w[k].descriptorType)) ww[k].pImageInfo = p; else if (is_texel(w[k].descriptorType)) ww[k].pTexelBufferView = p; else ww[k].pBufferInfo = p;
    }
    END();
}
static void VKAPI_CALL e_vkCmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na, const VkVertexInputAttributeDescription2EXT *a) {
    DIRECT(vkCmdSetVertexInputEXT, cb, nb, b, na, a);
    for (uint32_t k = 0; k < nb; k++) if (b[k].pNext) FALLBACK(vkCmdSetVertexInputEXT, cb, nb, b, na, a);
    for (uint32_t k = 0; k < na; k++) if (a[k].pNext) FALLBACK(vkCmdSetVertexInputEXT, cb, nb, b, na, a);
    BEGIN(r_vin, E_vkCmdSetVertexInputEXT, A8(nb * sizeof *b) + A8(na * sizeof *a)); r->nb = nb; r->na = na; r->b = (void *)cpy(&at, b, nb * sizeof *b); r->a = (void *)cpy(&at, a, na * sizeof *a); END();
}
static void VKAPI_CALL e_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t o, uint32_t n, const void *p) {
    DIRECT(vkCmdPushConstants, cb, l, st, o, n, p);
    BEGIN(r_pc, E_vkCmdPushConstants, A8(n)); r->l = l; r->st = st; r->o = o; r->n = n; r->p = cpy(&at, p, n); END();
}

// ---------- The encoder thread ----------
static void run(const hdr *h) {
    #define C(T) const T *r = (const T *)h; (void)r
    switch (h->id) {
    case E_vkCmdPipelineBarrier: { C(r_bar); R.vkCmdPipelineBarrier(r->cb, r->s, r->d, r->f, r->nm, r->nm ? r->m : NULL, r->nb, r->nb ? r->b : NULL, r->ni, r->ni ? r->i : NULL); break; }
    case E_vkCmdCopyBuffer: { C(r_cpb); R.vkCmdCopyBuffer(r->cb, r->s, r->d, r->n, r->r); break; }
    case E_vkCmdUpdateBuffer: { C(r_upd); R.vkCmdUpdateBuffer(r->cb, r->d, r->o, r->n, r->p); break; }
    case E_vkCmdFillBuffer: { C(r_fill); R.vkCmdFillBuffer(r->cb, r->d, r->o, r->n, r->v); break; }
    case E_vkCmdCopyBufferToImage: { C(r_b2i); R.vkCmdCopyBufferToImage(r->cb, r->b, r->im, r->l, r->n, r->r); break; }
    case E_vkCmdCopyImageToBuffer: { C(r_b2i); R.vkCmdCopyImageToBuffer(r->cb, r->im, r->l, r->b, r->n, r->r); break; }
    case E_vkCmdCopyImage: { C(r_i2i); R.vkCmdCopyImage(r->cb, r->s, r->sl, r->d, r->dl, r->n, r->r); break; }
    case E_vkCmdBlitImage: { C(r_i2i); R.vkCmdBlitImage(r->cb, r->s, r->sl, r->d, r->dl, r->n, r->r, r->f); break; }
    case E_vkCmdResolveImage: { C(r_i2i); R.vkCmdResolveImage(r->cb, r->s, r->sl, r->d, r->dl, r->n, r->r); break; }
    case E_vkCmdClearColorImage: { C(r_clr); R.vkCmdClearColorImage(r->cb, r->im, r->l, &r->v.color, r->n, r->r); break; }
    case E_vkCmdClearDepthStencilImage: { C(r_clr); R.vkCmdClearDepthStencilImage(r->cb, r->im, r->l, &r->v.depthStencil, r->n, r->r); break; }
    case E_vkCmdClearAttachments: { C(r_clra); R.vkCmdClearAttachments(r->cb, r->na, r->a, r->nr, r->r); break; }
    case E_vkCmdBeginRendering: { C(r_rend); R.vkCmdBeginRendering(r->cb, &r->ri); break; }
    case E_vkCmdEndRendering: { C(r_val); R.vkCmdEndRendering(r->cb); break; }
    case E_vkCmdBeginRenderPass: { C(r_rpb); VkRenderPassBeginInfo rb = r->rb; if (r->ab.sType) rb.pNext = &r->ab; R.vkCmdBeginRenderPass(r->cb, &rb, r->sc); break; }
    case E_vkCmdEndRenderPass: { C(r_val); R.vkCmdEndRenderPass(r->cb); break; }
    case E_vkCmdBindPipeline: { C(r_pipe); R.vkCmdBindPipeline(r->cb, r->bp, r->p); break; }
    case E_vkCmdBindDescriptorSets: { C(r_sets); R.vkCmdBindDescriptorSets(r->cb, r->bp, r->l, r->first, r->n, r->s, r->nd, r->d); break; }
    case E_vkCmdBindVertexBuffers2: { C(r_vb); R.vkCmdBindVertexBuffers2(r->cb, r->first, r->n, r->b, r->o, r->sz, r->st); break; }
    case E_vkCmdBindIndexBuffer: { C(r_ib); R.vkCmdBindIndexBuffer(r->cb, r->b, r->o, r->t); break; }
    case E_vkCmdDraw: { C(r_draw); R.vkCmdDraw(r->cb, r->a, r->b, r->c, r->d); break; }
    case E_vkCmdDrawIndexed: { C(r_draw); R.vkCmdDrawIndexed(r->cb, r->a, r->b, r->c, r->e, r->f); break; }
    case E_vkCmdDrawIndirect: { C(r_ind); R.vkCmdDrawIndirect(r->cb, r->b, r->o, r->n, r->st); break; }
    case E_vkCmdDrawIndexedIndirect: { C(r_ind); R.vkCmdDrawIndexedIndirect(r->cb, r->b, r->o, r->n, r->st); break; }
    case E_vkCmdDrawIndirectCount: { C(r_indc); R.vkCmdDrawIndirectCount(r->cb, r->b, r->o, r->cb2, r->o2, r->n, r->st); break; }
    case E_vkCmdDrawIndexedIndirectCount: { C(r_indc); R.vkCmdDrawIndexedIndirectCount(r->cb, r->b, r->o, r->cb2, r->o2, r->n, r->st); break; }
    case E_vkCmdDispatch: { C(r_draw); R.vkCmdDispatch(r->cb, r->a, r->b, r->c); break; }
    case E_vkCmdDrawMeshTasksEXT: { C(r_draw); R.vkCmdDrawMeshTasksEXT(r->cb, r->a, r->b, r->c); break; }
    case E_vkCmdDrawMeshTasksIndirectEXT: { C(r_ind); R.vkCmdDrawMeshTasksIndirectEXT(r->cb, r->b, r->o, r->n, r->st); break; }
    case E_vkCmdDispatchBase: { C(r_draw); R.vkCmdDispatchBase(r->cb, r->a, r->b, r->c, r->d, r->f, (uint32_t)r->e); break; }
    case E_vkCmdDispatchIndirect: { C(r_ind); R.vkCmdDispatchIndirect(r->cb, r->b, r->o); break; }
    case E_vkCmdSetViewport: { C(r_vps); R.vkCmdSetViewport(r->cb, r->first, r->n, r->p); break; }
    case E_vkCmdSetScissor: { C(r_vps); R.vkCmdSetScissor(r->cb, r->first, r->n, r->p); break; }
    case E_vkCmdSetCullMode: { C(r_val); R.vkCmdSetCullMode(r->cb, r->v[0]); break; }
    case E_vkCmdSetFrontFace: { C(r_val); R.vkCmdSetFrontFace(r->cb, (VkFrontFace)r->v[0]); break; }
    case E_vkCmdSetPrimitiveTopology: { C(r_val); R.vkCmdSetPrimitiveTopology(r->cb, (VkPrimitiveTopology)r->v[0]); break; }
    case E_vkCmdSetDepthTestEnable: { C(r_val); R.vkCmdSetDepthTestEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetDepthWriteEnable: { C(r_val); R.vkCmdSetDepthWriteEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetDepthCompareOp: { C(r_val); R.vkCmdSetDepthCompareOp(r->cb, (VkCompareOp)r->v[0]); break; }
    case E_vkCmdSetDepthBoundsTestEnable: { C(r_val); R.vkCmdSetDepthBoundsTestEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetStencilTestEnable: { C(r_val); R.vkCmdSetStencilTestEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetStencilOp: { C(r_val); R.vkCmdSetStencilOp(r->cb, r->v[0], (VkStencilOp)r->v[1], (VkStencilOp)r->v[2], (VkStencilOp)r->v[3], (VkCompareOp)r->v[4]); break; }
    case E_vkCmdSetStencilCompareMask: { C(r_val); R.vkCmdSetStencilCompareMask(r->cb, r->v[0], r->v[1]); break; }
    case E_vkCmdSetStencilWriteMask: { C(r_val); R.vkCmdSetStencilWriteMask(r->cb, r->v[0], r->v[1]); break; }
    case E_vkCmdSetStencilReference: { C(r_val); R.vkCmdSetStencilReference(r->cb, r->v[0], r->v[1]); break; }
    case E_vkCmdSetDepthBias: { C(r_val); R.vkCmdSetDepthBias(r->cb, r->f[0], r->f[1], r->f[2]); break; }
    case E_vkCmdSetDepthBiasEnable: { C(r_val); R.vkCmdSetDepthBiasEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetRasterizerDiscardEnable: { C(r_val); R.vkCmdSetRasterizerDiscardEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetPrimitiveRestartEnable: { C(r_val); R.vkCmdSetPrimitiveRestartEnable(r->cb, r->v[0]); break; }
    case E_vkCmdSetBlendConstants: { C(r_val); R.vkCmdSetBlendConstants(r->cb, r->f); break; }
    case E_vkCmdSetLineWidth: { C(r_val); R.vkCmdSetLineWidth(r->cb, r->f[0]); break; }
    case E_vkCmdPushDescriptorSetKHR: { C(r_push); R.vkCmdPushDescriptorSetKHR(r->cb, r->bp, r->l, r->set, r->n, r->w); break; }
    case E_vkCmdSetVertexInputEXT: { C(r_vin); R.vkCmdSetVertexInputEXT(r->cb, r->nb, r->b, r->na, r->a); break; }
    case E_vkCmdPushConstants: { C(r_pc); R.vkCmdPushConstants(r->cb, r->l, r->st, r->o, r->n, r->p); break; }
    case E_CALL: { C(r_call); r->fn((const uint8_t *)r + A8(sizeof *r)); break; }
    default: break;
    }
    #undef C
}
void vka_perf_thread(int nice);
// The encoder's tail, stored every DM_VK_ENC_TAIL bytes of calls made (live from the next submission; default 16384; 0 = after every call, as before), and always when it has
// caught up with the head it read (so vka_enc_sync, which waits for the whole ring, is never held back by it) or while the replay thread sleeps on it. A tail store is a
// store-release, a full fence and a load: after each of the ~60,000 calls of a heavy frame the fence waited for the driver's stores of that call to drain. The replay thread
// needs the tail only for room in the 8 MB ring and at its syncs
static _Atomic uint32_t g_tail_every = 16384;
// Where the encoder's time goes (the pace line, vka_enc_pace): one call in 16 timed by its kind (the virtual counter), and every call counted. Written by the encoder thread only
static uint64_t g_kn[E_N], g_kt[E_N], g_ktn[E_N];
static _Atomic int g_enc_tid;
int vka_enc_tid(void) { return atomic_load_explicit(&g_enc_tid, memory_order_relaxed); }
static inline uint64_t enc_cnt(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#define KADD(a, i, v) __atomic_store_n(&(a)[i], (a)[i] + (v), __ATOMIC_RELAXED)
static void *enc_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-enc"); vka_fp_host("vk-enc");
    vka_perf_thread(-8);   // the ADPF session and the replay thread's priority
    atomic_store(&g_enc_tid, gettid());
    uint64_t t = 0, tpub = 0; unsigned samp = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire);
        if (t == h) {
            // Empty: spin while the replay thread is recording (its next call comes within microseconds), then sleep until a call comes
            for (uint64_t t0 = mono_ns();;) {
                h = atomic_load_explicit(&g_head, memory_order_acquire); if (h != t) break;
                if (spin_over(t0)) break;
                wait_change(&g_head, t);
            }
            if (h == t) {
                uint32_t s = atomic_load(&g_cseq);   // (an event count, as wait_tail's)
                atomic_store(&g_csleep, 1);
                atomic_thread_fence(memory_order_seq_cst);
                if (atomic_load(&g_head) == t && fwait(&g_cseq, s) && errno == ETIMEDOUT && atomic_load(&g_head) != t) {
                    static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 8) VRED("command encoder: woke by its timeout with calls waiting (head %llu, tail %llu, sleep flag %d)", (unsigned long long)atomic_load(&g_head), (unsigned long long)t, atomic_load(&g_csleep)); }
                atomic_store(&g_csleep, 0);
                continue;
            }
        }
        uint64_t b0 = mono_ns(); uint32_t every = atomic_load_explicit(&g_tail_every, memory_order_relaxed);
        while (t != h) {
            const hdr *r = (const hdr *)(g_ring + t % RING);
            if (r->id != E_WRAP) {
                uint32_t id = r->id < E_N ? r->id : 0; KADD(g_kn, id, 1);
                atomic_store_explicit(&g_running, (int)r->id, memory_order_relaxed);
                if (++samp >= 16) { samp = 0; uint64_t c0 = enc_cnt(); run(r); KADD(g_kt, id, enc_cnt() - c0); KADD(g_ktn, id, 1); } else run(r);
                atomic_store_explicit(&g_running, -1, memory_order_relaxed);
            }
            t += r->size;
            if (t == h || t - tpub >= every || atomic_load_explicit(&g_pwait, memory_order_relaxed)) {
                tpub = t;
                atomic_store(&g_tail, t);
                atomic_thread_fence(memory_order_seq_cst);   // the tail before the replay thread's wait flag is read
                if (atomic_load(&g_pwait) && t >= atomic_load(&g_pwant)) { atomic_fetch_add(&g_pseq, 1); fwake(&g_pseq); }
                if (t == h) h = atomic_load_explicit(&g_head, memory_order_acquire);   // more came meanwhile
            }
        }
        atomic_fetch_add_explicit(&g_busy_ns, mono_ns() - b0, memory_order_relaxed);
    }
    return NULL;
}

// ---------- Interface ----------
#define ENC_HOOK(f) R.f = vk.f; if (R.f) vk.f = e_##f;
void vka_enc_install(void) {   // vka_dev.c, once the device's entry points are loaded
    static int done; if (done) return; done = 1;
    ENC_FNS(ENC_HOOK)
}
static int up(void) {
    if (g_up) return g_up > 0;
    g_ring = malloc(RING);
    pthread_t th;
    if (!g_ring || pthread_create(&th, NULL, enc_main, NULL)) { VRED("cannot start the command encoder thread: the replay thread records directly"); g_up = -1; return 0; }
    pthread_detach(th); g_up = 1;
    VLOG("command encoder: the replay thread's Vulkan commands are recorded on a thread of their own (DM_VK_ENC=0: on the replay thread)");
    return 1;
}
void vka_enc_sync(void) {
    flush_pub();
    if (g_ph != atomic_load_explicit(&g_tail, memory_order_acquire)) wait_tail(g_ph);
}
// The replay thread starts recording cb (after vkBeginCommandBuffer): its calls go through the ring when DM_VK_ENC is on
void vka_enc_start(VkCommandBuffer cb) {
    vka_enc_sync();
    extern int vka_gp_active;
    if (!VKA_ENV("DM_VK_ENC", 1) || vka_gp_active || vka_nan_on() || vka_texstats_on() || !up()) { atomic_store(&g_ecb, VK_NULL_HANDLE); return; }
    { int bt = VKA_ENV("DM_VK_ENC_BATCH", 4096); g_batch = bt < 0 ? 0 : bt > (1 << 20) ? (1 << 20) : (uint32_t)bt; }   // live: from this submission
    { int us = VKA_ENV("DM_VK_ENC_SPIN", 200); atomic_store_explicit(&g_spin_ns, (uint64_t)(us < 0 ? 0 : us > 100000 ? 100000 : us) * 1000, memory_order_relaxed); }   // live: from this submission
    { int tb = VKA_ENV("DM_VK_ENC_TAIL", 16384); atomic_store_explicit(&g_tail_every, tb <= 0 ? 0 : tb > (1 << 20) ? (1u << 20) : (uint32_t)tb, memory_order_relaxed); }   // live: from this submission
    atomic_store(&g_ecb, cb);
}
// Before vkEndCommandBuffer, a submit, or any other use of the command buffer or its pool: every call made
void vka_enc_stop(void) {
    if (!atomic_load_explicit(&g_ecb, memory_order_relaxed)) return;
    vka_enc_sync();
    atomic_store(&g_ecb, VK_NULL_HANDLE);
}
uint64_t vka_enc_total(void) { return g_n_total; }
int vka_enc_on(void) { return atomic_load_explicit(&g_ecb, memory_order_relaxed) != VK_NULL_HANDLE; }
// fn(arg copy) on the encoder thread, in order with the deferred calls (1); 0 = the encoder is off: the caller makes the call itself
int vka_enc_call(void (*fn)(const void *), const void *arg, size_t n) {
    VkCommandBuffer cb = atomic_load_explicit(&g_ecb, memory_order_relaxed); if (!cb) return 0;
    BEGIN(r_call, E_CALL, A8(n)); r->fn = fn; cpy(&at, arg, n); END();
    return 1;
}
// The replay thread goes on in another command buffer (a part of the submission, vka_exec.c chunk) whose reset and begin it queued with vka_enc_call
void vka_enc_switch(VkCommandBuffer cb) { atomic_store(&g_ecb, cb); flush_pub(); }   // (the part's submit handed over at once)
// The pace line's view of the encoder: per frame, the time of each kind of call (from the timed sample) and their counts, since the last reading
void vka_enc_pace(char *b, size_t n, double frames) {
    static uint64_t ln[E_N], lt[E_N], ltn[E_N];
    enum { G_PUSH, G_DRAW, G_PIPE, G_VTX, G_SETS, G_STATE, G_PASS, G_BAR, G_COPY, G_DISP, G_OWN, G_N };
    static const char *gn[G_N] = {"push descriptors", "draws", "pipelines", "vertex/index buffers", "sets+constants", "dynamic state", "passes", "barriers", "copies/clears", "dispatches", "part submits"};
    double ms[G_N] = {0}, cnt[G_N] = {0};
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    for (int i = 0; i < E_N; i++) {
        uint64_t kn = __atomic_load_n(&g_kn[i], __ATOMIC_RELAXED), kt = __atomic_load_n(&g_kt[i], __ATOMIC_RELAXED), ktn = __atomic_load_n(&g_ktn[i], __ATOMIC_RELAXED);
        uint64_t dn = kn - ln[i], dt = kt - lt[i], dtn = ktn - ltn[i]; ln[i] = kn; lt[i] = kt; ltn[i] = ktn;
        int g = i == E_vkCmdPushDescriptorSetKHR ? G_PUSH : i == E_vkCmdDraw || i == E_vkCmdDrawIndexed || i == E_vkCmdDrawIndirect || i == E_vkCmdDrawIndexedIndirect || i == E_vkCmdDrawIndirectCount || i == E_vkCmdDrawIndexedIndirectCount || i == E_vkCmdDrawMeshTasksEXT ? G_DRAW
              : i == E_vkCmdBindPipeline ? G_PIPE : i == E_vkCmdBindVertexBuffers2 || i == E_vkCmdBindIndexBuffer || i == E_vkCmdSetVertexInputEXT ? G_VTX
              : i == E_vkCmdBindDescriptorSets || i == E_vkCmdPushConstants ? G_SETS
              : i == E_vkCmdBeginRendering || i == E_vkCmdEndRendering || i == E_vkCmdBeginRenderPass || i == E_vkCmdEndRenderPass || i == E_vkCmdClearAttachments ? G_PASS
              : i == E_vkCmdPipelineBarrier ? G_BAR : i == E_vkCmdDispatch || i == E_vkCmdDispatchBase || i == E_vkCmdDispatchIndirect ? G_DISP : i == E_CALL ? G_OWN
              : i >= E_vkCmdCopyBuffer && i <= E_vkCmdClearDepthStencilImage ? G_COPY : G_STATE;
        cnt[g] += (double)dn;
        if (dtn) ms[g] += (double)dt / (double)dtn * (double)dn / (double)f * 1e3;   // the timed calls' mean, times every call of the kind
    }
    size_t o = 0; if (n) b[0] = 0;
    if (frames <= 0) return;
    for (int g = 0; g < G_N && o < n; g++) if (cnt[g]) o += (size_t)snprintf(b + o, n - o, "%s%s %.2f (%.0f)", o ? ", " : "", gn[g], ms[g] / frames, cnt[g] / frames);
}
void vka_enc_stats(char *b, size_t n, double frames) {   // per frame
    if (!g_n_calls || frames <= 0) { if (n) b[0] = 0; return; }
    snprintf(b, n, "command encoder per frame: %.0f calls (%.0f made directly to keep order), busy %.2f ms; the replay thread waited %.2f ms (%.1f times)", g_n_calls / frames, g_n_direct / frames,
             atomic_exchange(&g_busy_ns, 0) / 1e6 / frames, g_wait_ns / 1e6 / frames, g_n_waits / frames);
    g_n_calls = g_n_direct = g_n_waits = g_wait_ns = 0;
}
#endif
