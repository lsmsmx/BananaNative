// NVN queue dependencies on the one Vulkan queue: the replay worker (vka_worker.c) replays the game's submissions in the order it made them, which is one valid order only
// while no queue waits for another. nvnCommandBufferWaitEvent / WaitSync and nvnQueueWaitSync make a queue's later work wait for a producer on another queue, which the game may
// submit later: Ocarina of Time submits its ambient-cache pass on queue 5 (it waits for an event) before the SH lighting pass on queue 1 that signals it, every frame. Replayed
// in submission order, the consumer read last frame's lighting (a pale outline around Link, scene-wide lighting flashes: upstream ea0c98df on the Mac).
//
// Here an unresolved wait parks only its own logical queue, and the other queues' submissions go on being replayed. A submission whose first wait comes before any event
// signal in it (qs_sub.w0kind) and does not hold when the worker reaches it is parked whole, before any of it is replayed: no cut, no extra commit (Ocarina of Time's
// waits). Otherwise the submission is cut at the wait (the part before it goes to the GPU); the rest and that queue's later submissions wait in its list. Once the producer has been replayed, the parked queue goes on, after it on the one
// in-order Vulkan queue, so the GPU runs them in the producer-consumer order.
// - Event waits use the SDK's predicates: EQUAL is exact equality, GEQUAL_WRAP is (uint32_t)(current - target) < 0x80000000. The value they see is the word as the replay
//   leaves it: a signal replayed but not completed yet counts with the value it writes (WRITE) or adds (ADD) (qs_ev), so a wait on a queued signal goes on at once (upstream
//   04419bc2); otherwise the word in memory (a completed signal, nvnEventSignal on the CPU, or a plain store through mapped pool memory, polled every millisecond while parked).
// - A sync wait looks at what the sync held when its submission was submitted (qs_wsnap): never fenced, the wait passes at once (DRAGON BALL Sparking! ZERO waits on a sync
//   before its own queue fences it, upstream 9b69d27d); fenced, it goes on once that fence's submission has been handed to the queue whole.
// - Fences stay one timeline, reserved in submission order (mtl_last_fence, the syncs). A submission replayed while a lower fence is parked is handed to the queue without a
//   signal; the next submission after which every fence up to some value v has been handed signals v (qs_commit). Its completion effects (events, counter reports) are posted
//   with the signal that covers them, in the order they were recorded, so they never land before their GPU work.
// - Safety valves, each counted (the stats line): a present, a queue finalization, a CPU wait (SyncWait, Finish, a pool's release) for a fence a park holds once the
//   worker has nothing else to replay (the producer is not coming while that thread waits), a parked queue's
//   backlog past QS_BACKLOG_MAX, and QS_DEFER_MAX unsignalled submissions (their completion effects wait for a covering signal) release every parked queue in
//   fence order with their waits passed, as the replay ran before. A park longer than the park time (DM_VK_EVENT_PARK_MS, default 200) is released too, with a [RED] line:
//   its producer did not come.
// - Waits for the CPU (gates): a wait whose event is set by the CPU (nvnEventSignal, or a store through mapped memory) has no producer in the replay. Parking it held the
//   replay of the waiting queue, and of every queue waiting on it, until the CPU's signal: Ocarina of Time begins each frame's main submission (queue 1) with a wait on an
//   event its CPU sets right after its SyncWait on the previous frame's main fence, so the next frame's 20 ms of recording began only once the GPU had finished the last one
//   (22 fps instead of 37 on the Honor, 2026-11-10). An event word whose parks were released twice in a row by memory, with no signal of the replay in flight (QS_GATE_LEARN),
//   is a CPU event: a later wait on it that does not hold is a gate (qs.gate): the replay goes on at once and the GPU waits (vka_dev.c "Gates": the next submission waits
//   for a semaphore value the host signals once the word holds). A gate that runs out of its time bans its word (back to parking).
// Pure C (no Vulkan): the unit test dm/host/tests/t_android_qsched.c runs the scheduler with a stand-in replay.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { QS_NRET = 17, QS_NQ = 16, QS_NMARK = 256, QS_NDEF = 64, QS_NEV0 = 256, QS_DEFER_MAX = 32, QS_BACKLOG_MAX = 256, QS_NGW = 32, QS_GATE_LEARN = 2 };
enum { QS_WHY_PRESENT, QS_WHY_FORGET, QS_WHY_CPU_WAIT, QS_WHY_SLOTS, QS_WHY_BACKLOG, QS_WHY_TIMEOUT, QS_NWHY };
static const char *const qs_why_name[QS_NWHY] = {"present", "queue finalized", "CPU wait", "command buffers", "backlog", "park time"};

// Where a submission cut at a wait goes on: the record after the wait, and the return points of the command lists that called it (innermost last)
typedef struct qs_cont { const uint8_t *p; int nret; const uint8_t *ret[QS_NRET]; } qs_cont;
typedef struct { const uint8_t *rec; uint64_t fence; } qs_wsnap;   // a WAIT_SYNC record and the fence its sync held when submitted (0: not fenced, the wait passes)

typedef struct qs_sub {
    struct qs_sub *next;
    uint64_t serial, fence, frame, seq; int origin_ll;   // fence 0: a queue-level wait (wait_fence), no commands
    const uint8_t **h; int nh;                            // the command handles (owned: freed with the submission)
    qs_wsnap *ws; int nws, wsi;                           // its WAIT_SYNC snapshots in replay order (owned), the next one
    uint64_t wait_fence;                                  // nvnQueueWaitSync: the fence its queue's later work waits for
    int hi, resumed; qs_cont cont;                        // replay goes on at handle hi (at cont when resumed)
    int wkind; volatile uint32_t *word; uint32_t mode, value; uint64_t wfence, t_park;   // the wait it is parked at: 1 event, 2 fence
    int w0kind; volatile uint32_t *w0word; uint32_t w0mode, w0value; uint64_t w0fence;   // its first wait when no event signal comes before it (1 event, 2 sync; 0 none): asked before replay
    volatile uint32_t *gword; uint32_t gmode, gvalue;                                    // the wait it was gated at last (asked again when the replay reaches it: passes)
    const volatile uint32_t *aword; uint32_t amode, avalue;                              // the wait asked last (a second ask of the same wait, after its park or before the replay, is not counted again)
} qs_sub;
typedef struct { uint64_t serial; qs_sub *head, *tail; int n; } qs_queue;   // head: parked at its wait; the rest wait behind it
typedef struct { volatile uint32_t *word; uint32_t value; uint64_t fence, frame; } qs_ev;   // an event word as the replay leaves it, until fence completes (frame: the game's frame of the submission that replayed the signal)
typedef struct { const volatile uint32_t *word; int cpu; } qs_gw;                     // an event word's releases by memory in a row (QS_GATE_LEARN: a CPU event)

typedef struct qs {
    qs_queue q[QS_NQ]; int nq;
    uint64_t lo, sig;                         // every fence <= lo handed to the queue whole; the highest fence signalled
    uint64_t mark[QS_NMARK]; int nmark;       // fences above lo handed whole
    uint64_t def[QS_NDEF]; int ndef;          // fences of the commits no signal covers yet (one per commit)
    qs_ev *ev; uint32_t evcap, nev, evpeak;   // the words a replayed signal is in flight for: open addressing by word, no deletions (stale entries go at a rebuild); evpeak: the most in flight at a commit (qs_commit)
    qs_gw gw[QS_NGW]; int ngw, ngw_next;
    int force; uint64_t seq;
    qs_sub *cur;                              // the submission being replayed (qs_wait_* and qs_event_replayed refer to it)
    // The replay: run replays s from its start or its continuation, commits it with qs_commit's signal and returns 1 when it parked (qs_wait_* said no), 0 when done
    int (*run)(void *ctx, qs_sub *s);
    uint64_t (*done)(void *ctx);              // the last fence completed (its effects landed)
    uint64_t (*now)(void *ctx);               // ns
    void (*timeout)(void *ctx, const qs_sub *s);   // a park ran past park_ns: logged, then released
    void (*note)(void *ctx, int what, const qs_sub *s);   // optional (DM_VK_EVENT_LOG): 0 parked, 1 released by its wait, 2 gated
    // optional: a wait of s on a CPU event that does not hold: 1 when the GPU waits for it instead (the replay goes on), 0 to park
    int (*gate)(void *ctx, qs_sub *s, volatile uint32_t *w, uint32_t mode, uint32_t value);
    void *ctx;
    uint64_t park_ns;
    uint64_t n_park, n_whole, n_release, n_gate, n_force[QS_NWHY];
    // Waits that held when first asked (no park): by a signal of the replay still queued from the waiting submission's own frame or a later one, by one queued from an earlier
    // frame (the word's last use: a double-buffered event word seen before this frame's producer was replayed), or by the word in memory. The last one asked (note 3)
    uint64_t n_pass_queued, n_pass_old, n_pass_mem;
    const volatile uint32_t *pass_word; uint32_t pass_mode, pass_value; int pass_src;   // pass_src: 1 queued, 2 queued from an earlier frame, 3 memory
    int force_why;                            // the valve of the release in progress (note 4)
} qs;

static inline int qs_pred(uint32_t cur, uint32_t mode, uint32_t target) { return mode == 0 ? cur == target : (uint32_t)(cur - target) < 0x80000000u; }   // NVNeventWaitMode

// ---------- Fences ----------
static inline int qs_whole(const qs *sc, uint64_t f) {   // fence f's submission has been handed to the queue whole
    if (f <= sc->lo) return 1;
    for (int i = 0; i < sc->nmark; i++) if (sc->mark[i] == f) return 1;
    return 0;
}
// A commit of fence f (final: its submission's last part; otherwise the part before a wait): the timeline value its submit signals, 0 = none yet
static inline void qs_ev_peak(qs *sc);
static inline uint64_t qs_commit(qs *sc, uint64_t f, int final) {
    qs_ev_peak(sc);
    if (final && f > sc->lo && !qs_whole(sc, f)) {
        if (sc->nmark < QS_NMARK) sc->mark[sc->nmark++] = f;
        for (int moved = 1; moved;) { moved = 0; for (int i = 0; i < sc->nmark; i++) if (sc->mark[i] == sc->lo + 1) { sc->lo++; sc->mark[i] = sc->mark[--sc->nmark]; moved = 1; break; } }
    }
    if (final && sc->lo >= f && sc->lo > sc->sig) {
        sc->sig = sc->lo;
        int k = 0; for (int i = 0; i < sc->ndef; i++) if (sc->def[i] > sc->sig) sc->def[k++] = sc->def[i];
        sc->ndef = k;
        return sc->sig;
    }
    if (sc->ndef < QS_NDEF) sc->def[sc->ndef++] = f;
    return 0;
}
static inline uint64_t qs_park_min(const qs *sc) {   // the lowest fence a park holds back (UINT64_MAX: none)
    uint64_t m = UINT64_MAX;
    for (int i = 0; i < sc->nq; i++) for (const qs_sub *s = sc->q[i].head; s; s = s->next) if (s->fence && s->fence < m) m = s->fence;
    for (int i = 0; i < sc->ndef; i++) if (sc->def[i] < m) m = sc->def[i];
    return m;
}
static inline int qs_parked(const qs *sc) { for (int i = 0; i < sc->nq; i++) if (sc->q[i].head) return 1; return 0; }

// ---------- Events ----------
// Every word a replayed signal is in flight for is kept until its fence completes. Before 2026-11-10 the table held 256 words and a signal past that was not recorded: a
// wait on its word then saw the word in memory (the last time's value) and could pass before its producer had run. Each replayed signal also searched the whole table
// twice and pruned it (O(n) a signal, n up to 256). Mario Kart World 1.8.0 ReportValues to many words a frame, and its frames pass work between queues 1 and 6 six times
static inline uint32_t qs_ev_slot(const volatile uint32_t *w, uint32_t cap) { uint64_t x = ((uint64_t)(uintptr_t)w >> 2) * 0x9e3779b97f4a7c15ull; return (uint32_t)(x >> 40) & (cap - 1); }
static inline qs_ev *qs_ev_find(qs *sc, const volatile uint32_t *w) {
    if (!sc->evcap) return NULL;
    for (uint32_t i = qs_ev_slot(w, sc->evcap);; i = (i + 1) & (sc->evcap - 1)) { if (sc->ev[i].word == w) return &sc->ev[i]; if (!sc->ev[i].word) return NULL; }
}
// The table again with only the words still in flight, in at least cap slots (grown so they fill a quarter at most): 0 = no memory (the old table stays)
static inline int qs_ev_rebuild(qs *sc, uint32_t cap) {
    uint64_t d = sc->done(sc->ctx); uint32_t live = 0;
    for (uint32_t i = 0; i < sc->evcap; i++) if (sc->ev[i].word && sc->ev[i].fence > d) live++;
    if (live > sc->evpeak) sc->evpeak = live;
    if (cap < QS_NEV0) cap = QS_NEV0;
    while ((uint64_t)(live + 1) * 4 > cap) cap *= 2;
    qs_ev *n = calloc(cap, sizeof *n); if (!n) return 0;
    for (uint32_t i = 0; i < sc->evcap; i++) {
        qs_ev *e = &sc->ev[i]; if (!e->word || e->fence <= d) continue;
        uint32_t k = qs_ev_slot(e->word, cap); while (n[k].word) k = (k + 1) & (cap - 1);
        n[k] = *e;
    }
    free(sc->ev); sc->ev = n; sc->evcap = cap; sc->nev = live;
    return 1;
}
static inline void qs_ev_prune(qs *sc) { if (sc->evcap) qs_ev_rebuild(sc, sc->evcap); }
static inline void qs_ev_free(qs *sc) { free(sc->ev); sc->ev = NULL; sc->evcap = sc->nev = 0; }
static inline uint32_t qs_ev_value(qs *sc, const volatile uint32_t *w) {
    qs_ev *e = qs_ev_find(sc, w);
    if (e && e->fence > sc->done(sc->ctx)) return e->value;
    return *w;
}
static inline int qs_ev_inflight(qs *sc, const volatile uint32_t *w) { qs_ev *e = qs_ev_find(sc, w); return e && e->fence > sc->done(sc->ctx); }   // the replay's view is a queued signal
// The words a replayed signal is in flight for now (a walk of the table). Until 2026-11-10 the peak was taken only at a rebuild, which comes at half full (128 words): Mario
// Kart World's frames use 4 words (two pairs, alternate frames), the table never filled, and the stats line read "at most 0" in every run. Now sampled at every commit
static inline uint32_t qs_ev_live(qs *sc) {
    if (!sc->evcap) return 0;
    uint64_t d = sc->done(sc->ctx); uint32_t n = 0;
    for (uint32_t i = 0; i < sc->evcap; i++) if (sc->ev[i].word && sc->ev[i].fence > d) n++;
    return n;
}
static inline void qs_ev_peak(qs *sc) { if (sc->nev > sc->evpeak) { uint32_t n = qs_ev_live(sc); if (n > sc->evpeak) sc->evpeak = n; } }
// Gate learning: a release by memory counts for the word, a release by a queued signal of the replay sets it back (the producer is GPU work the replay orders)
static inline qs_gw *qs_gw_find(qs *sc, const volatile uint32_t *w, int add) {
    for (int i = 0; i < sc->ngw; i++) if (sc->gw[i].word == w) return &sc->gw[i];
    if (!add) return NULL;
    qs_gw *g = sc->ngw < QS_NGW ? &sc->gw[sc->ngw++] : &sc->gw[sc->ngw_next++ % QS_NGW];
    g->word = w; g->cpu = 0; return g;
}
static inline void qs_gw_note(qs *sc, const volatile uint32_t *w, int by_memory) {
    qs_gw *g = qs_gw_find(sc, w, by_memory); if (!g) return;
    g->cpu = by_memory ? g->cpu + 1 : 0;
}
static inline void qs_gate_ban(qs *sc, const volatile uint32_t *w) { qs_gw *g = qs_gw_find(sc, w, 0); if (g) g->cpu = -1000000; }   // back to parking for good
static inline int qs_gw_cpu(qs *sc, const volatile uint32_t *w) { qs_gw *g = qs_gw_find(sc, w, 0); return g && g->cpu >= QS_GATE_LEARN; }
// A signal replayed (OP_SIGNAL_EVENT, mode 0 WRITE / 1 ADD): the value the word will hold once the current submission's effects land
static inline void qs_event_replayed(qs *sc, volatile uint32_t *w, uint32_t mode, uint32_t v) {
    if (!sc->cur || !sc->cur->fence) return;
    uint32_t base = qs_ev_value(sc, w);
    qs_ev *e = qs_ev_find(sc, w);
    if (!e) {
        if ((sc->nev + 1) * 2 > sc->evcap && !qs_ev_rebuild(sc, sc->evcap)) return;   // (half full: rebuilt without the completed words, grown when they are many)
        uint32_t k = qs_ev_slot(w, sc->evcap); while (sc->ev[k].word) k = (k + 1) & (sc->evcap - 1);
        e = &sc->ev[k]; e->word = w; sc->nev++;
    }
    e->value = mode == 1 ? base + v : v; e->fence = sc->cur->fence; e->frame = sc->cur->frame;
}

// ---------- Waits (asked by the replay at a WAIT record of sc->cur) ----------
// A CPU thread that waits for a fence a park holds back does not pass the wait here: its producer may already be on its way (Ocarina of Time's queues 1 and 5 hand
// each other events within one frame while the game already waits for queue 1's fence), and passing it ran the consumer before its producer (issue #98: whole frames
// dark while walking, 2026-11-10). The worker releases the parks only when it cannot go on otherwise (vka_worker.c qs_step)
static inline int qs_wait_event(qs *sc, volatile uint32_t *w, uint32_t mode, uint32_t value) {
    qs_sub *s = sc->cur;
    if (!s || sc->force) return 1;
    int again = s->aword == w && s->amode == mode && s->avalue == value;   // asked before (parked there, or passed as its first wait before the replay)
    s->aword = w; s->amode = mode; s->avalue = value;
    if (qs_pred(qs_ev_value(sc, w), mode, value)) {
        if (!again) {   // held at once: by whom (the stats line; DM_VK_QS_TRACE)
            qs_ev *e = qs_ev_find(sc, w); int queued = e && e->fence > sc->done(sc->ctx);
            int src = queued ? (e->frame < s->frame ? 2 : 1) : 3;
            if (src == 1) sc->n_pass_queued++; else if (src == 2) sc->n_pass_old++; else sc->n_pass_mem++;
            if (sc->note) { sc->pass_word = w; sc->pass_mode = mode; sc->pass_value = value; sc->pass_src = src; sc->note(sc->ctx, 3, s); }
        }
        return 1;
    }
    if (s->gword == w && s->gmode == mode && s->gvalue == value) return 1;   // gated already (its first wait, asked before the replay)
    if (sc->gate && s->fence && qs_gw_cpu(sc, w) && sc->gate(sc->ctx, s, w, mode, value)) {
        s->gword = w; s->gmode = mode; s->gvalue = value; sc->n_gate++;
        if (sc->note) { s->wkind = 1; s->word = w; s->mode = mode; s->value = value; sc->note(sc->ctx, 2, s); s->wkind = 0; }
        return 1;
    }
    s->wkind = 1; s->word = w; s->mode = mode; s->value = value; s->t_park = sc->now(sc->ctx);
    return 0;
}
static inline int qs_fence_wait(qs *sc, uint64_t f) {
    qs_sub *s = sc->cur;
    if (!s || sc->force || !f || (s->fence && f >= s->fence) || qs_whole(sc, f)) return 1;
    s->wkind = 2; s->wfence = f; s->t_park = sc->now(sc->ctx);
    return 0;
}
static inline int qs_wait_sync(qs *sc, const uint8_t *rec) {
    qs_sub *s = sc->cur; if (!s) return 1;
    int i = s->wsi < s->nws && s->ws[s->wsi].rec == rec ? s->wsi : -1;
    for (int k = 0; i < 0 && k < s->nws; k++) if (s->ws[k].rec == rec) i = k;
    if (i < 0) return 1;   // not snapshotted (a replay outside a worker job): passes, as before
    if (!qs_fence_wait(sc, s->ws[i].fence)) return 0;
    s->wsi = i + 1; return 1;
}
static inline int qs_released(qs *sc, const qs_sub *s) {   // the wait s is parked at holds now
    if (s->wkind == 1) return qs_pred(qs_ev_value(sc, s->word), s->mode, s->value);
    if (s->wkind == 2) return qs_whole(sc, s->wfence);
    return 1;
}

// ---------- Scheduling ----------
static inline qs_queue *qs_queue_of(qs *sc, uint64_t serial) {
    for (int i = 0; i < sc->nq; i++) if (sc->q[i].serial == serial) return &sc->q[i];
    for (int i = 0; i < sc->nq; i++) if (!sc->q[i].head) { sc->q[i].serial = serial; return &sc->q[i]; }
    if (sc->nq == QS_NQ) return NULL;
    qs_queue *q = &sc->q[sc->nq++]; memset(q, 0, sizeof *q); q->serial = serial; return q;
}
static inline void qs_free(qs_sub *s) { free((void *)s->h); free(s->ws); free(s); }
static inline void qs_force_all(qs *sc, int why);
// Room for one more commit: QS_DEFER_MAX unsignalled commits release everything first (their completion effects wait for a covering signal: vka_exec.c carries at most
// NCARRY). Asked before a submission is taken off its queue's list, so the release keeps each queue's order
static inline int qs_room(qs *sc) { if (sc->force || sc->ndef < QS_DEFER_MAX) return 1; qs_force_all(sc, QS_WHY_SLOTS); return 0; }
// Replays s (nothing of its queue is parked ahead of it): 1 when it parked and became its queue's head
static inline int qs_run_one(qs *sc, qs_queue *q, qs_sub *s) {
    sc->cur = s; s->wkind = 0;
    int parked = 0;
    if (!s->fence) parked = !qs_fence_wait(sc, s->wait_fence);   // a queue-level wait: nothing to replay
    else if (!s->hi && !s->resumed && s->w0kind && !sc->force &&
             !(s->w0kind == 1 ? qs_wait_event(sc, s->w0word, s->w0mode, s->w0value) : qs_fence_wait(sc, s->w0fence))) { parked = 1; sc->n_whole++; }   // parked whole: no cut, no commit
    else parked = sc->run(sc->ctx, s);
    sc->cur = NULL;
    if (parked) { s->next = q->head; q->head = s; if (!q->tail) q->tail = s; q->n++; sc->n_park++; if (sc->note) sc->note(sc->ctx, 0, s); return 1; }
    qs_free(s); return 0;
}
// Replays q's parked head and the submissions behind it until one parks again or none is left
static inline void qs_drain_queue(qs *sc, qs_queue *q) {
    while (q->head) {
        if (!qs_room(sc)) return;
        qs_sub *s = q->head; q->head = s->next; if (!q->head) q->tail = NULL; q->n--; s->next = NULL;
        if (qs_run_one(sc, q, s)) return;
    }
}
// A submission (or a queue-level wait) as the worker takes it from the game, in submission order
static inline void qs_submit(qs *sc, qs_sub *s) {
    s->seq = ++sc->seq;
    qs_queue *q = qs_queue_of(sc, s->serial);
    if (!q) { qs_force_all(sc, QS_WHY_BACKLOG); q = qs_queue_of(sc, s->serial); }
    if (q && q->head) {   // its queue is parked: behind it
        s->next = NULL; q->tail->next = s; q->tail = s; q->n++;
        if (q->n > QS_BACKLOG_MAX) qs_force_all(sc, QS_WHY_BACKLOG);
        return;
    }
    qs_room(sc);
    if (!q) { int f = sc->force; sc->force = 1; qs_queue tmp = {0}; qs_run_one(sc, &tmp, s); sc->force = f; return; }   // (cannot happen: QS_NQ queues all parked)
    qs_run_one(sc, q, s);
}
// Parked queues whose wait holds now go on (in turn, until nothing more moves); a park past park_ns is released with its wait passed. Returns how many went on
static inline int qs_poll(qs *sc) {
    int n = 0;
    for (int moved = 1; moved;) {
        moved = 0;
        for (int i = 0; i < sc->nq; i++) {
            qs_queue *q = &sc->q[i]; qs_sub *s = q->head; if (!s) continue;
            int ok = qs_released(sc, s);
            if (!ok && sc->park_ns && sc->now(sc->ctx) - s->t_park > sc->park_ns) {
                if (!qs_room(sc)) { n++; moved = 1; continue; }
                if (sc->timeout) sc->timeout(sc->ctx, s);
                if (s->wkind == 1) qs_gw_note(sc, s->word, 0);
                sc->n_force[QS_WHY_TIMEOUT]++;
                // Released: the rest of this submission runs with its waits passed, then its queue goes on (a later submission's wait may park it again)
                q->head = s->next; if (!q->head) q->tail = NULL; q->n--; s->next = NULL;
                int f = sc->force; sc->force = 1; qs_run_one(sc, q, s); sc->force = f;
                qs_drain_queue(sc, q);
                n++; moved = 1; continue;
            }
            if (!ok) continue;
            if (s->wkind == 1) qs_gw_note(sc, s->word, !qs_ev_inflight(sc, s->word));
            sc->n_release++; n++; moved = 1;
            if (sc->note) sc->note(sc->ctx, 1, s);
            qs_drain_queue(sc, q);
        }
    }
    return n;
}
// Every parked submission replayed now, lowest fence first, with its waits passed: the order the replay had before waits were honoured
static inline void qs_force_all(qs *sc, int why) {
    if (sc->force || !qs_parked(sc)) return;
    sc->n_force[why]++;
    if (sc->note) { sc->force_why = why; sc->note(sc->ctx, 4, NULL); }
    sc->force = 1;
    for (;;) {
        qs_queue *best = NULL;
        for (int i = 0; i < sc->nq; i++) if (sc->q[i].head && (!best || sc->q[i].head->seq < best->head->seq)) best = &sc->q[i];
        if (!best) break;
        qs_sub *s = best->head; best->head = s->next; if (!best->head) best->tail = NULL; best->n--; s->next = NULL;
        qs_run_one(sc, best, s);
    }
    sc->force = 0;
}
