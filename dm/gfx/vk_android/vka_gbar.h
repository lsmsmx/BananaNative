// The game's barriers between compute dispatches (vka_gbar.c), the choice of barrier as pure logic, free of Vulkan so the Mac's unit test
// (dm/host/tests/t_android_gbar.c) can check it.
// NVN orders the work before a barrier (nvnCommandBufferBarrier) against the work after it; the replay recorded each as a full barrier (ALL_COMMANDS to ALL_COMMANDS, every
// write to every read), which on a tiler waits for the whole GPU to drain and flushes its caches: about 8 us of Adreno GPU time for a barrier between two 1-group dispatches.
// A game barrier taken outside a draw pass is left pending (vkgb_game) when every command recorded into the command buffer since the last full barrier is a dispatch or a
// state command (since: VKGB_COMPUTE). The command recorded next decides it (vkgb_cmd):
// - a dispatch: a compute-to-compute barrier (VKGB_B_NARROW: shader writes to shader reads and writes and uniform reads; with indirect-command reads as well when the
//   barrier asked for indirect data or the dispatch is indirect). It covers every dispatch before it: all of them came after the last full barrier;
// - any other GPU work (draws, a pass's begin or end, clears, copies, fills, updates, blits, a barrier other than the full one): compute writes to every later command
//   (VKGB_B_SETTLE: COMPUTE_SHADER to ALL_COMMANDS), before that work. Work before the last full barrier is already ordered against everything after it, so a source scope
//   of compute alone is complete;
// - a full barrier: covers it (nothing more recorded).
// After a narrow barrier the compute work before it is still owed to the later non-compute work in the same stretch (NVN's barrier orders it against everything after it):
// the first such command gets the settle barrier too, and an indirect dispatch the narrow barrier did not cover indirect reads for gets VKGB_B_INDIRECT (compute writes to
// indirect-command reads). A game barrier with nothing recorded since the last barrier (none pending, or one pending already) adds nothing and is merged.
// Invariant: pending or owed only while every command since the last full barrier is a dispatch or a state command.
#ifndef VKA_GBAR_H
#define VKA_GBAR_H
#include <stdint.h>

enum { VKGB_NONE, VKGB_COMPUTE, VKGB_OTHER };                                    // vkgb.since: what was recorded since the last barrier
enum { VKGB_C_STATE, VKGB_C_DISPATCH, VKGB_C_DISPATCH_IND, VKGB_C_WORK, VKGB_C_FULL };   // a command's kind (vkgb_cmd)
enum { VKGB_B_NONE, VKGB_B_NARROW, VKGB_B_INDIRECT, VKGB_B_SETTLE };            // the barrier to record before it
enum { VKGB_OWED = 1, VKGB_OWED_IND = 2 };                                       // vkgb.cov
#define VKGB_NVN_ORDER_INDIRECT_DATA 0x8u                                        // NVN_BARRIER_ORDER_INDIRECT_DATA_BIT

typedef struct {
    uint8_t since;   // VKGB_NONE: nothing since the last full or narrow barrier; VKGB_COMPUTE: dispatches only; VKGB_OTHER: other work, or not known
    uint8_t pend;    // a game barrier is pending
    uint8_t cov;     // VKGB_OWED: a narrow barrier was recorded and the non-compute work after it still needs one; VKGB_OWED_IND: it covered indirect reads
    uint32_t bits;   // the pending barriers' NVN bits, or-ed
} vkgb;
typedef struct { uint64_t full, narrow, merged, settled, owed, indirect; } vkgb_stats;   // game barriers recorded full (old path), narrow, merged; settle barriers (pending, owed); indirect ones

// A new command buffer: what came before it is not known (the first full barrier makes it known)
static inline void vkgb_reset(vkgb *g) { g->since = VKGB_OTHER; g->pend = 0; g->cov = 0; g->bits = 0; }
// A game barrier outside a draw pass. on: the narrow path (DM_VK_BAR_NARROW). 1: taken (nothing to record now); 0: the caller records a full barrier now (vkgb_cmd's
// VKGB_C_FULL follows from it)
static inline int vkgb_game(vkgb *g, uint32_t bits, int on, vkgb_stats *s) {
    if (!on || g->since == VKGB_OTHER) { s->full++; return 0; }
    if (g->pend || g->since == VKGB_NONE) { s->merged++; if (g->pend) g->bits |= bits; return 1; }   // nothing recorded since the barrier before it
    g->pend = 1; g->bits = bits;
    return 1;
}
// A command about to be recorded: the barrier to record before it (VKGB_B_*). *ind: the narrow barrier also covers indirect-command reads
static inline int vkgb_cmd(vkgb *g, int kind, int *ind, vkgb_stats *s) {
    *ind = 0;
    switch (kind) {
    case VKGB_C_STATE: return VKGB_B_NONE;
    case VKGB_C_FULL: g->since = VKGB_NONE; g->pend = 0; g->cov = 0; g->bits = 0; return VKGB_B_NONE;
    case VKGB_C_DISPATCH: case VKGB_C_DISPATCH_IND: {
        int r = VKGB_B_NONE, di = kind == VKGB_C_DISPATCH_IND;
        if (g->pend) {
            *ind = di || (g->bits & VKGB_NVN_ORDER_INDIRECT_DATA);
            g->pend = 0; g->bits = 0; g->cov = VKGB_OWED | (*ind ? VKGB_OWED_IND : 0); g->since = VKGB_NONE;
            s->narrow++; r = VKGB_B_NARROW;
        } else if (di && (g->cov & VKGB_OWED) && !(g->cov & VKGB_OWED_IND)) {
            *ind = 1; g->cov |= VKGB_OWED_IND; s->indirect++; r = VKGB_B_INDIRECT;
        }
        if (g->since == VKGB_NONE) g->since = VKGB_COMPUTE;
        return r;
    }
    default: {
        int r = VKGB_B_NONE;
        if (g->pend || g->cov) { r = VKGB_B_SETTLE; if (g->pend) s->settled++; else s->owed++; g->pend = 0; g->cov = 0; g->bits = 0; }
        g->since = VKGB_OTHER;
        return r;
    }
    }
}
#endif
