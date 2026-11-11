// Host-signalled timelines (vka_dev.c "Host-signalled timelines"): the submissions that would signal a timeline value from the GPU get a VkFence instead, in queue
// order, and the signal thread waits for each fence and then signals the value on the host. This is the ring between the submitting threads (under the queue lock,
// so in queue order) and the signal thread, and the guard that keeps each timeline's host signals strictly increasing (a value at or below one already signalled is
// covered by it). Plain C over opaque handles: dm/host/tests t_android_tlsig
#ifndef VKA_TLSIG_H
#define VKA_TLSIG_H
#include <stdint.h>

enum { TLS_N = 256, TLS_SEMS = 4 };
typedef struct { void *sem; uint64_t v; void *fence; } tls_ent;
typedef struct {
    tls_ent e[TLS_N]; unsigned head, tail;          // e[head % TLS_N] is the oldest; tail - head entries
    struct { void *sem; uint64_t last; } seen[TLS_SEMS]; int nseen;   // the last value signalled on each timeline
} tls_ring;

static inline int tls_full(const tls_ring *r) { return r->tail - r->head >= TLS_N; }
static inline int tls_empty(const tls_ring *r) { return r->tail == r->head; }
static inline int tls_push(tls_ring *r, void *sem, uint64_t v, void *fence) {   // 0 = full
    if (tls_full(r)) return 0;
    r->e[r->tail++ % TLS_N] = (tls_ent){sem, v, fence}; return 1;
}
static inline int tls_pop(tls_ring *r, tls_ent *out) {   // 0 = empty
    if (tls_empty(r)) return 0;
    *out = r->e[r->head++ % TLS_N]; return 1;
}
// 1 = signal v on sem now (and it is recorded as that timeline's last value); 0 = an equal or higher value was already signalled there
static inline int tls_take(tls_ring *r, void *sem, uint64_t v) {
    for (int i = 0; i < r->nseen; i++) if (r->seen[i].sem == sem) { if (v <= r->seen[i].last) return 0; r->seen[i].last = v; return 1; }
    if (r->nseen < TLS_SEMS) { r->seen[r->nseen].sem = sem; r->seen[r->nseen].last = v; r->nseen++; }
    return 1;
}
// The host's own signal of v on sem (outside the ring: a failed submission's value, vka_dev.c): later ring entries at or below it are then skipped
static inline void tls_note(tls_ring *r, void *sem, uint64_t v) { (void)tls_take(r, sem, v); }
#endif
