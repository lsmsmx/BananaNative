// The replay worker: the game's submissions and presents are replayed on a thread of their own, so the game's thread goes on to its next frame while the
// Vulkan commands of this one are recorded (on the Adreno 840 phone Donkey Kong Bananza's level spent ~48 ms a frame recording and ~29 ms in its own
// work on the same thread, one after the other).
//
// What the game sees stays what NVN promises, as the inline replay gave it:
// - Order: submissions, presents and queue finalizations are replayed in the order the game made them (one FIFO, one thread).
// - Fences: a submission's fence is reserved when the game submits (vka_fence_next, in order), so mtl_last_fence, QueueFenceSync and the window's acquire syncs name
//   it at once; a sync fenced inside a command buffer (OP_FENCE_SYNC) is given it before the submission is queued (fence_syncs), as the game may wait on it right
//   after. A wait for a fence the worker has not submitted yet waits for it (vka_wait_done waits for completion, not for submission); the hang detection only
//   counts fences handed to the GPU.
// - Presents: the ticket is reserved when the game presents (vka_present_reserve: its order and the bound of the present queue), the frame is filled when the
//   worker reaches it, after every submission before it was handed to the queue.
// - CPU reads at replay time (deferred texture binds, render conditions, indirect counts, texture uploads from storage) happen later than inline, which is
//   closer to the GPU reading them when it runs the commands; the game cannot rely on the old timing (its GPU runs a frame or more behind).
// - mtl_finish / mtl_drain wait for the last fence reserved, so for the worker too.
// - GPU waits (nvnCommandBufferWaitEvent / WaitSync, nvnQueueWaitSync): a wait that does not hold parks only its own NVN queue until its producer on another queue has been
//   replayed (vka_qsched.h); the other queues' submissions go on. DM_VK_EVENT_WAIT=0 passes every wait, as before 2026-11-09 (submission order only). A wait for an event the
//   CPU sets is a gate instead: replayed at once, the GPU waits (vka_qsched.h "Waits for the CPU"; DM_VK_EVENT_GATE=0 parks it as in v0.2.8).
// The game runs at most DM_VK_WORKER_FRAMES (default 1) presents ahead of the worker. DM_VK_WORKER=0 replays inline on the submitting thread, as before (waits pass).
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_qsched.h"
#include "../nvn/nvn_mtl.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint64_t vka_frame(void);
void vka_exec_sub_frame(uint64_t f);
void vka_rp_gate(uint64_t g, VkEvent ev);   // vka_exec.c
static uint64_t wns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// Where the time goes (logged every 300 presents): the worker busy replaying, idle waiting for a job; the game waiting for room in the queue
static uint64_t g_busy_ns, g_idle_ns, g_room_ns, g_sub_ns;
static _Atomic uint64_t g_room_all, g_sub_all, g_sub_cpu_all;   // cumulative (vka_worker_waits: the [longframe] line's attribution); sub_cpu: the worker's CPU time in submissions
void vka_worker_waits(uint64_t *room_ns, uint64_t *sub_ns) { if (room_ns) *room_ns = atomic_load(&g_room_all); if (sub_ns) *sub_ns = atomic_load(&g_sub_all); }
uint64_t vka_worker_sub_cpu(void) { return atomic_load(&g_sub_cpu_all); }   // a recording time far above it: the replay thread was blocked in the submission, not busy
static uint64_t thread_cpu_ns(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

enum { J_SUB = 1, J_PRESENT, J_FORGET, J_WAIT, J_PROG_FORGET };
typedef struct {
    int kind;
    uint64_t serial, fence, frame; int origin_ll;   // J_SUB: the queue, its window origin, the fence reserved for it, the game's frame when it submitted; J_WAIT: the queue, the fence it waits for
    const uint8_t **h; int nh;                   // J_SUB: the command handles, in order
    qs_wsnap *ws; int nws;                       // J_SUB: what the syncs of its WAIT_SYNC records held when it was submitted
    int w0done, w0kind; volatile uint32_t *w0word; uint32_t w0mode, w0value; uint64_t w0fence;   // J_SUB: its first wait, when no event signal comes before it (qs_sub.w0kind)
    uint64_t ticket; tex_t t; double period; int has_crop, crop[4];   // J_PRESENT
} job_t;
enum { NJOB = 256 };
static job_t g_q[NJOB]; static uint64_t g_qin, g_qout, g_pres_in, g_pres_out;   // jobs queued / done; presents queued / done
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_on = -1, g_frames = 1, g_up;
// The submission being gathered on the game's thread (the host submits under its own lock: one at a time)
static job_t g_cur; static int g_cur_cap, g_in_sub; static uint64_t g_pend_serial; static int g_pend_origin;

int vka_worker_on(void) {
    if (g_on < 0) { const char *e = getenv("DM_VK_WORKER"); g_on = !e || atoi(e); e = getenv("DM_VK_WORKER_FRAMES"); g_frames = e && atoi(e) > 0 ? atoi(e) : 1; }
    return g_on;
}
// Core placement experiments (live, CPU masks as decimal numbers, 0 = leave as is: 192 = cpus 6-7): DM_VK_AFF_REPLAY for this thread, DM_VK_AFF_ALL for every thread of the process (read at each job)
#include <sched.h>
#include <dirent.h>
#include <errno.h>
static void aff_set(pid_t tid, unsigned mask) {
    cpu_set_t cs; CPU_ZERO(&cs); for (int c = 0; c < 32; c++) if (mask >> c & 1) CPU_SET(c, &cs);
    if (sched_setaffinity(tid, sizeof cs, &cs)) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("sched_setaffinity(%d, %#x): errno %d", (int)tid, mask, errno); }
}
static void aff_check(void) {
    static unsigned r_was, a_was;
    unsigned r = (unsigned)VKA_ENV("DM_VK_AFF_REPLAY", 0), a = (unsigned)VKA_ENV("DM_VK_AFF_ALL", 0);
    if (a != a_was) { a_was = a; if (a) { DIR *d = opendir("/proc/self/task"); struct dirent *e; int n = 0;
            while (d && (e = readdir(d))) if (e->d_name[0] != '.') { aff_set((pid_t)atoi(e->d_name), a); n++; }
            if (d) closedir(d); VLOG("DM_VK_AFF_ALL=%#x: %d threads", a, n); } }
    if (r != r_was) { r_was = r; if (r) { aff_set(0, r); VLOG("DM_VK_AFF_REPLAY=%#x", r); } }
}
// ---------- Queue dependencies (vka_qsched.h), on the worker thread ----------
uint64_t vka_rp_sub_commit_sig(uint64_t reserved, uint64_t signal);   // vka_exec.c
int vka_rp_exec_pk(const uint8_t *handle, uint64_t fence, qs_cont *out);
int vka_rp_resume(const qs_cont *c, uint64_t fence, qs_cont *out);
static qs g_qs;
static int g_qs_on = -1;   // DM_VK_EVENT_WAIT (default 1; read when the worker starts)
static _Thread_local int t_worker;
static _Atomic uint64_t g_park_min = UINT64_MAX, g_cpu_want; static _Atomic int g_cpu_nwait; static uint64_t g_stuck_t;   // what a park holds back; the highest fence CPU threads block on, how many; since when the worker is stuck
static int g_kick;   // (under g_mu) an event word written on the CPU
static uint64_t qs_done_cb(void *c) { (void)c; return vka_done(); }
static uint64_t qs_now_cb(void *c) { (void)c; return wns(); }
static void qs_timeout_cb(void *c, const qs_sub *s) {
    (void)c; static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k > 8 && (k & (k - 1))) return;
    if (s->wkind == 1) VRED("a GPU wait parked queue %llu for %.0f ms: its producer did not come (event %p: wants %s %u, holds %u; fence %llu): released (%llu so far)",
                            (unsigned long long)s->serial, (wns() - s->t_park) / 1e6, (const void *)s->word, s->mode ? ">= (wrapping)" : "==", s->value, *s->word, (unsigned long long)s->fence, (unsigned long long)k);
    else VRED("a GPU wait parked queue %llu for %.0f ms: fence %llu was not handed to the queue (submission fence %llu): released (%llu so far)", (unsigned long long)s->serial,
              (wns() - s->t_park) / 1e6, (unsigned long long)s->wfence, (unsigned long long)s->fence, (unsigned long long)k);
}
static void qs_publish(void) { atomic_store(&g_park_min, qs_park_min(&g_qs)); }
// ---------- The GPU-wait trace ----------
// Mario Kart World 1.8.0 on Turnip V39 lost the device at random, 7 to 70 s into play, with GPU waits on (2026-11-10): each time a queue-6 compute submission cut at a
// wait and its queue-1 producer were the work in flight, and the logs held no more than that. DM_VK_QS_TRACE=n (restart; default 512, 0 = off) keeps the last n events of
// the queue scheduler and the submissions in a ring: each submission as the worker takes it, each park, each wait that held at once and whose value it saw (a signal
// still queued on the replay, from which frame, or memory), each release and valve, each replayed event signal and the value it leaves, each commit with its timeline
// signal (or none), the GPU's progress and the last value handed to the queue, each part handed over inside a submission, each event value the completion thread wrote,
// presents, CPU threads' fence waits and failed submits. Nothing is logged while all goes well; when a GPU hang is declared (a lost device, or no progress) the ring is
// printed oldest first (vka_qtrace_dump, from vka_exec_hang_report). The cost: a mutex and 64 bytes per event, about 40 events a frame
typedef struct { uint64_t t, q, f, x; uint32_t a, b, c, d; const volatile void *w; uint8_t kind; } qt_t;
static qt_t *g_qt; static uint32_t g_qtn; static uint64_t g_qti; static pthread_mutex_t g_qtmu = PTHREAD_MUTEX_INITIALIZER; static pthread_once_t g_qt_once = PTHREAD_ONCE_INIT;
static void qt_init(void) {
    const char *e = getenv("DM_VK_QS_TRACE"); long n = e && *e ? atol(e) : 512;
    if (n > 65536) n = 65536;
    if (n > 0 && (g_qt = calloc((size_t)n, sizeof *g_qt))) g_qtn = (uint32_t)n;
}
int vka_qtrace_on(void) { pthread_once(&g_qt_once, qt_init); return g_qtn > 0; }
void vka_qtrace(int kind, uint64_t q, uint64_t fence, uint64_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const volatile void *w) {
    if (!vka_qtrace_on()) return;
    uint64_t t = wns();
    pthread_mutex_lock(&g_qtmu);
    g_qt[g_qti++ % g_qtn] = (qt_t){t, q, fence, x, a, b, c, d, w, (uint8_t)kind};
    pthread_mutex_unlock(&g_qtmu);
}
static const char *qt_op(uint32_t mode) { return mode ? ">=" : "=="; }
void vka_qtrace_dump(void) {
    if (!vka_qtrace_on()) return;
    pthread_mutex_lock(&g_qtmu);
    uint64_t n = g_qti < g_qtn ? g_qti : g_qtn, i0 = g_qti - n, now = wns();
    qt_t *c = malloc((size_t)(n ? n : 1) * sizeof *c);
    if (c) for (uint64_t i = 0; i < n; i++) c[i] = g_qt[(i0 + i) % g_qtn];
    pthread_mutex_unlock(&g_qtmu);
    if (!c) return;
    VRED("GPU hang: the last %llu events of the GPU-wait trace (DM_VK_QS_TRACE), oldest first; times are before this line", (unsigned long long)n);
    static const char *const src[] = {"?", "a signal queued this frame", "a signal queued in an earlier frame", "memory"};
    for (uint64_t i = 0; i < n; i++) {
        const qt_t *e = &c[i]; double ms = now > e->t ? (now - e->t) / 1e6 : 0;
        unsigned long long q = (unsigned long long)e->q, f = (unsigned long long)e->f, x = (unsigned long long)e->x;
        switch (e->kind) {
        case QT_SUB: if (e->a == 1) VLOG("qtrace -%.2f ms: queue %llu submission %llu (frame %llu) taken; its first wait: event %p %s %u", ms, q, f, x, (const void *)e->w, qt_op(e->b), e->c);
                     else VLOG("qtrace -%.2f ms: queue %llu submission %llu (frame %llu) taken%s", ms, q, f, x, e->a == 2 ? "; its first wait: a sync" : e->a == 3 ? " (a queue-level wait for a fence)" : ""); break;
        case QT_PARK: if (!e->w) { VLOG("qtrace -%.2f ms: queue %llu submission %llu parked at a wait for fence %llu", ms, q, f, x); break; }
                      VLOG("qtrace -%.2f ms: queue %llu submission %llu parked %s at event %p %s %u: the replay sees %u (%s%llu), memory %u", ms, q, f, e->a & 1 ? "whole" : "after a cut",
                           (const void *)e->w, qt_op(e->a >> 1), e->b, e->c, x ? "a signal queued by submission " : "memory", x, e->d); break;
        case QT_PASS: VLOG("qtrace -%.2f ms: queue %llu submission %llu: its wait on event %p %s %u held at once: %u from %s (submission %llu), memory %u", ms, q, f, (const void *)e->w,
                           qt_op(e->a >> 4), e->b, e->c, src[e->a & 3], x, e->d); break;
        case QT_RELEASE: VLOG("qtrace -%.2f ms: queue %llu submission %llu goes on after %.2f ms (event %p %s %u: sees %u, memory %u)", ms, q, f, x / 1e3, (const void *)e->w, qt_op(e->a), e->b, e->c, e->d); break;
        case QT_GATE: VLOG("qtrace -%.2f ms: queue %llu submission %llu gated at event %p %s %u (memory %u)", ms, q, f, (const void *)e->w, qt_op(e->a), e->b, e->d); break;
        case QT_FORCE: VLOG("qtrace -%.2f ms: every park released with its waits passed (%s)", ms, e->a < QS_NWHY ? qs_why_name[e->a] : "?"); break;
        case QT_SIGNAL: VLOG("qtrace -%.2f ms: queue %llu submission %llu replays a signal of event %p (%s %u): the replay sees %u", ms, q, f, (const void *)e->w, e->a ? "ADD" : "WRITE", e->b, e->c); break;
        case QT_COMMIT: VLOG("qtrace -%.2f ms: queue %llu submission %llu %s: %s%llu; GPU done to %u, %u handed to the queue", ms, q, f, e->a ? "committed whole" : "cut: its part so far committed",
                             x ? "timeline signal " : "no timeline signal (its own fence) ", x, e->b, e->c); break;
        case QT_PART: VLOG("qtrace -%.2f ms: submission %llu: part %u handed to the queue (a part split)", ms, f, e->a); break;
        case QT_LAND: VLOG("qtrace -%.2f ms: completion: event %p %s %u written, now %u (GPU done past %llu)", ms, (const void *)e->w, e->a ? "ADD" : "WRITE", e->b, e->c, f); break;
        case QT_PRESENT: VLOG("qtrace -%.2f ms: present %llu", ms, x); break;
        case QT_CPU_WAIT: if (e->a) VLOG("qtrace -%.2f ms: a CPU thread blocks on fence %llu", ms, f); else VLOG("qtrace -%.2f ms: a CPU thread's fence wait ended", ms); break;
        case QT_SLOT: VLOG("qtrace -%.2f ms: pool table slot %u of binding %u now holds view %p instead of %p (written at %s)", ms, e->b, e->a, (const void *)e->w, (const void *)(uintptr_t)e->x,
                           e->q == 1 ? "a binding" : "a submission's start"); break;
        case QT_SUBMIT_ERR: VLOG("qtrace -%.2f ms: submission %llu: a submit returned %d", ms, f, (int)e->a); break;
        default: break;
        }
    }
    free(c);
}
// A park, a release, a wait that held at once: what the replay sees and from where (a queued signal: by which submission; 0 = the word in memory)
static void qt_wait(int kind, const qs_sub *s, const volatile uint32_t *w, uint32_t mode, uint32_t value, uint32_t a) {
    qs_ev *e = qs_ev_find(&g_qs, w); uint64_t by = e && e->fence > vka_done() ? e->fence : 0;
    vka_qtrace(kind, s->serial, s->fence, kind == QT_RELEASE ? (wns() - s->t_park) / 1000 : by, kind == QT_RELEASE ? mode : a, value, qs_ev_value(&g_qs, w), *w, w);
}
// DM_VK_EVENT_LOG=n (restart): the first n parks and releases, with the wait, the value it saw and the submission replayed last (the producer, for a release)
static int g_evlog; static uint64_t g_last_serial, g_last_fence;
static void qs_note_cb(void *c, int what, const qs_sub *s) {
    (void)c;
    if (vka_qtrace_on()) {
        if (what == 0 && s->wkind == 1) qt_wait(QT_PARK, s, s->word, s->mode, s->value, (uint32_t)(!s->hi && !s->resumed) | s->mode << 1);
        else if (what == 0 && s->wkind == 2) vka_qtrace(QT_PARK, s->serial, s->fence, s->wfence, 0, 0, 0, 0, NULL);
        else if (what == 1 && s->wkind == 1) qt_wait(QT_RELEASE, s, s->word, s->mode, s->value, 0);
        else if (what == 2) vka_qtrace(QT_GATE, s->serial, s->fence, 0, s->gmode, s->gvalue, 0, s->gword ? *s->gword : 0, s->gword);
        else if (what == 3) qt_wait(QT_PASS, s, g_qs.pass_word, g_qs.pass_mode, g_qs.pass_value, (uint32_t)g_qs.pass_src | g_qs.pass_mode << 4);
        else if (what == 4) vka_qtrace(QT_FORCE, 0, 0, 0, (uint32_t)g_qs.force_why, 0, 0, 0, NULL);
    }
    if (what >= 3 || g_evlog <= 0) return;
    g_evlog--;
    char w[160];
    if (s->wkind == 1) snprintf(w, sizeof w, "event %p %s %u (the replay sees %u, memory %u)", (const void *)s->word, s->mode ? ">=" : "==", s->value, qs_ev_value(&g_qs, s->word), *s->word);
    else snprintf(w, sizeof w, "fence %llu", (unsigned long long)s->wfence);
    if (what == 2) { VLOG("GPU wait: queue %llu submission %llu gated at %s (the GPU waits for the CPU's signal; the replay goes on)", (unsigned long long)s->serial, (unsigned long long)s->fence, w); return; }
    if (!what) VLOG("GPU wait: queue %llu submission %llu parked %s at %s; last replayed: queue %llu submission %llu", (unsigned long long)s->serial, (unsigned long long)s->fence,
                    !s->hi && !s->resumed ? "whole" : "after a cut", w, (unsigned long long)g_last_serial, (unsigned long long)g_last_fence);
    else VLOG("GPU wait: queue %llu submission %llu goes on after %.2f ms (%s) after queue %llu submission %llu", (unsigned long long)s->serial, (unsigned long long)s->fence,
              (wns() - s->t_park) / 1e6, w, (unsigned long long)g_last_serial, (unsigned long long)g_last_fence);
}
static int qs_run_cb(void *c, qs_sub *s) {
    (void)c;
    vka_rp_queue_activate(s->serial); vka_rp_set_queue_origin(s->origin_ll);
    vka_gp_sub_frame(s->frame); vka_exec_sub_frame(s->frame); vka_rp_sub_cont(s->resumed || s->hi > 0); vka_rp_sub_begin();
    int parked = 0;
    while (s->hi < s->nh) {
        int r = s->resumed ? vka_rp_resume(&s->cont, s->fence, &s->cont) : vka_rp_exec_pk(s->h[s->hi], s->fence, &s->cont);
        if (r) { s->resumed = 1; parked = 1; break; }
        s->resumed = 0; s->hi++;
    }
    uint64_t sig = qs_commit(&g_qs, s->fence, !parked);
    vka_rp_sub_commit_sig(s->fence, sig);
    vka_qtrace(QT_COMMIT, s->serial, s->fence, sig, !parked, (uint32_t)vka_done(), (uint32_t)vka_fence_queued(), 0, NULL);
    qs_publish();
    g_last_serial = s->serial; g_last_fence = s->fence;
    return parked;
}
// A wait for an event the CPU sets: the gate is made (vka_dev.c) and the replay's next submission waits for it
static int g_gate_on;
static int qs_gate_cb(void *c, qs_sub *s, volatile uint32_t *w, uint32_t mode, uint32_t value) {
    (void)c; if (!g_gate_on) return 0;
    if (vka_gate_banned(w)) { qs_gate_ban(&g_qs, w); return 0; }
    VkEvent ev = VK_NULL_HANDLE;
    uint64_t g = vka_gate_add(w, mode, value, s->fence, g_qs.park_ns, &ev);
    if (!g) return 0;
    vka_rp_gate(g, ev);
    return 1;
}
static int qs_on(void) {
    if (g_qs_on < 0) {
        const char *e = getenv("DM_VK_EVENT_WAIT"); g_qs_on = !e || atoi(e);
        e = getenv("DM_VK_EVENT_PARK_MS"); double ms = e ? atof(e) : 200; if (!(ms >= 0)) ms = 200;
        g_qs.run = qs_run_cb; g_qs.done = qs_done_cb; g_qs.now = qs_now_cb; g_qs.timeout = qs_timeout_cb; g_qs.park_ns = (uint64_t)(ms * 1e6);
        e = getenv("DM_VK_EVENT_LOG"); g_evlog = e ? atoi(e) : 0; if (g_evlog > 0 || vka_qtrace_on()) g_qs.note = qs_note_cb;
        e = getenv("DM_VK_EVENT_GATE"); g_gate_on = !e || atoi(e); if (g_gate_on) g_qs.gate = qs_gate_cb;
        VLOG("GPU waits across NVN queues: %s%s", g_qs_on ? "a wait parks its queue until its producer is replayed (DM_VK_EVENT_WAIT=0: every wait passes)" : "every wait passes (DM_VK_EVENT_WAIT=0)",
             !g_qs_on ? "" : g_gate_on ? "; a wait for an event the CPU sets is replayed at once and the GPU waits (DM_VK_EVENT_GATE=0: parked)" : "; waits for the CPU park too (DM_VK_EVENT_GATE=0)");
    }
    return g_qs_on;
}
// Waits asked by the replay (vka_exec.c); only the worker's own replays are parked, at its submissions (g_qs.cur)
int vka_wait_pass(uint32_t op, const void *a, const uint8_t *rec) {
    if (!t_worker || !g_qs.cur) return 1;
    if (op == OP_WAIT_EVENT) { p_wait_event w; memcpy(&w, a, sizeof w); return !w.val || qs_wait_event(&g_qs, w.val, w.mode, w.value); }
    return qs_wait_sync(&g_qs, rec);
}
void vka_event_replayed(volatile uint32_t *w, uint32_t mode, uint32_t v) {
    if (!t_worker || !g_qs.cur || !w) return;
    qs_event_replayed(&g_qs, w, mode, v);
    vka_qtrace(QT_SIGNAL, g_qs.cur->serial, g_qs.cur->fence, 0, mode, v, qs_ev_value(&g_qs, w), 0, w);
}
// A CPU thread blocks on fence (vka_wait_done) until vka_worker_cpu_wait_end. A park that holds that fence back is released only once the worker is stuck: nothing left
// to replay, no parked wait that holds, for QS_STUCK_NS; then the producer is not coming while that thread waits (the GPU would not leave it waiting for work the game has
// not submitted yet). Released at once, as before 2026-11-10, it ran consumers before producers the worker was about to replay: Ocarina of Time's queues 1 and 5 hand
// each other events within a frame while its game thread already waits for queue 1's fence, and those frames lost their lighting (issue #98)
enum { QS_STUCK_NS = 2000000 };
uint64_t vka_worker_cpu_blocked(void) { return atomic_load(&g_cpu_nwait) > 0 ? atomic_load(&g_cpu_want) : 0; }   // the fence CPU threads block on (0: none)
int vka_worker_cpu_wait(uint64_t fence) {
    if (t_worker || g_qs_on <= 0) return 0;
    atomic_fetch_add(&g_cpu_nwait, 1);
    vka_qtrace(QT_CPU_WAIT, 0, fence, 0, 1, 0, 0, 0, NULL);
    uint64_t w = atomic_load(&g_cpu_want); while (w < fence && !atomic_compare_exchange_weak(&g_cpu_want, &w, fence)) {}
    vka_gate_cpu_wait(fence);   // a gate before that fence opens if the GPU waits at it with nothing before it (vka_dev.c)
    pthread_mutex_lock(&g_mu); g_kick = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return 1;
}
void vka_worker_cpu_wait_end(int was) {
    if (!was) return;
    vka_qtrace(QT_CPU_WAIT, 0, 0, 0, 0, 0, 0, 0, NULL);
    if (atomic_fetch_sub(&g_cpu_nwait, 1) == 1) atomic_store(&g_cpu_want, 0);   // (the last waiter: what it waited for is no longer waited for)
}
static void qs_step(int have) {   // the parked queues whose waits hold now; with nothing to replay, a CPU wait's release once stuck
    if (!qs_parked(&g_qs)) { g_stuck_t = 0; return; }
    int moved = qs_poll(&g_qs);
    uint64_t want = vka_worker_cpu_blocked();
    if (moved || have || !qs_parked(&g_qs) || !want || want < qs_park_min(&g_qs)) g_stuck_t = 0;
    else if (!g_stuck_t) g_stuck_t = wns();
    else if (wns() - g_stuck_t >= QS_STUCK_NS) { qs_force_all(&g_qs, QS_WHY_CPU_WAIT); g_stuck_t = 0; }
    qs_publish();
}
static void qs_stats(uint64_t ticket) {
    static uint64_t p0, w0, r0, f0[QS_NWHY], g0, gw0, gc0, gt0, gn0, pq0, po0, pm0;
    uint64_t gw, gc, gt, gn; vka_gate_stats(&gw, &gc, &gt, &gn);
    uint64_t df[QS_NWHY], any = g_qs.n_park - p0 + g_qs.n_gate - g0;
    for (int i = 0; i < QS_NWHY; i++) { df[i] = g_qs.n_force[i] - f0[i]; any += df[i]; }
    if (any) {
        char b[300]; int n = 0;
        for (int i = 0; i < QS_NWHY; i++) if (df[i]) n += snprintf(b + n, sizeof b - (size_t)n, ", %s %llu", qs_why_name[i], (unsigned long long)df[i]);
        VLOG("GPU waits up to present %llu: %llu parked (%llu whole, before any of the submission), %llu went on after their producer; released early by%s; %llu held when first asked "
             "(%llu by a signal queued this frame, %llu by one queued in an earlier frame, %llu by memory); event words in flight at most %u",
             (unsigned long long)ticket, (unsigned long long)(g_qs.n_park - p0), (unsigned long long)(g_qs.n_whole - w0), (unsigned long long)(g_qs.n_release - r0),
             n ? b : " nothing", (unsigned long long)(g_qs.n_pass_queued - pq0 + g_qs.n_pass_old - po0 + g_qs.n_pass_mem - pm0), (unsigned long long)(g_qs.n_pass_queued - pq0),
             (unsigned long long)(g_qs.n_pass_old - po0), (unsigned long long)(g_qs.n_pass_mem - pm0), g_qs.evpeak);
        { static uint64_t sw0, sc0; uint64_t sw, sc; vka_tex_slot_counts(&sw, &sc);
          VLOG("texture pool table up to present %llu: %llu slots written, %llu of them changed the view a slot held (each a write under parts that may read it)", (unsigned long long)ticket,
               (unsigned long long)(sw - sw0), (unsigned long long)(sc - sc0)); sw0 = sw; sc0 = sc; }
        if (g_qs.n_gate - g0) VLOG("GPU waits for the CPU up to present %llu: %llu gated (the replay went on, the GPU waited %.2f ms each); opened by their event %llu, by a CPU wait %llu, by their time %llu",
                                   (unsigned long long)ticket, (unsigned long long)(g_qs.n_gate - g0), (gw - gw0 + gc - gc0 + gt - gt0) ? (gn - gn0) / 1e6 / (double)(gw - gw0 + gc - gc0 + gt - gt0) : 0.0,
                                   (unsigned long long)(gw - gw0), (unsigned long long)(gc - gc0), (unsigned long long)(gt - gt0));
    }
    g0 = g_qs.n_gate; gw0 = gw; gc0 = gc; gt0 = gt; gn0 = gn;
    p0 = g_qs.n_park; w0 = g_qs.n_whole; r0 = g_qs.n_release; memcpy(f0, g_qs.n_force, sizeof f0);
    pq0 = g_qs.n_pass_queued; po0 = g_qs.n_pass_old; pm0 = g_qs.n_pass_mem;
}
static void *worker_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-replay"); vka_fp_host("vk-replay");
    t_worker = 1; int qon = qs_on();
    for (;;) {
        uint64_t t0 = wns();
        pthread_mutex_lock(&g_mu);
        int have = 0; job_t j = {0};
        for (;;) {
            if (g_qout != g_qin) { j = g_q[g_qout % NJOB]; have = 1; break; }
            int parked = qon && qs_parked(&g_qs);
            if (!parked) { pthread_cond_wait(&g_cv, &g_mu); continue; }
            if (g_kick) break;
            // Parked with nothing to replay: a word may change through a plain store to mapped pool memory, and the park time runs: looked at every millisecond
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &dl);
            break;
        }
        g_kick = 0;
        pthread_mutex_unlock(&g_mu);
        uint64_t t1 = wns(), c1 = have && j.kind == J_SUB ? thread_cpu_ns() : 0; g_idle_ns += t1 - t0;
        if (qon) qs_step(have);
        if (!have) continue;
        aff_check();
        if (j.kind == J_SUB && qon) {
            qs_sub *q = calloc(1, sizeof *q);
            q->serial = j.serial; q->fence = j.fence; q->frame = j.frame; q->origin_ll = j.origin_ll; q->h = j.h; q->nh = j.nh; q->ws = j.ws; q->nws = j.nws;
            q->w0kind = j.w0kind; q->w0word = j.w0word; q->w0mode = j.w0mode; q->w0value = j.w0value; q->w0fence = j.w0fence;
            vka_qtrace(QT_SUB, j.serial, j.fence, j.frame, (uint32_t)j.w0kind, j.w0mode, j.w0value, 0, j.w0word);
            qs_submit(&g_qs, q); qs_poll(&g_qs); qs_publish();
        } else if (j.kind == J_SUB) {
            vka_rp_queue_activate(j.serial); vka_rp_set_queue_origin(j.origin_ll);
            vka_gp_sub_frame(j.frame); vka_exec_sub_frame(j.frame); vka_rp_sub_begin();
            for (int i = 0; i < j.nh; i++) vka_rp_exec(j.h[i], j.fence);
            vka_rp_sub_commit(j.fence);
            free(j.h); free(j.ws);
        } else if (j.kind == J_WAIT) {
            if (qon) { qs_sub *q = calloc(1, sizeof *q); q->serial = j.serial; q->wait_fence = j.fence; q->frame = j.frame; vka_qtrace(QT_SUB, j.serial, 0, j.frame, 3, 0, 0, 0, NULL); qs_submit(&g_qs, q); qs_publish(); }
        } else if (j.kind == J_PRESENT) {
            // Presented after every submission before it was handed to the queue and signalled (vka_present_fill waits for the last value signalled): a park still open here
            // is released first, so the frame holds the work the game submitted before the present
            if (qon) { qs_force_all(&g_qs, QS_WHY_PRESENT); qs_publish(); }
            vka_qtrace(QT_PRESENT, 0, 0, j.ticket, 0, 0, 0, 0, NULL);
            vka_present_fill(j.ticket, &j.t, j.period, j.has_crop ? j.crop : NULL);
        } else if (j.kind == J_FORGET) {
            if (qon) { qs_force_all(&g_qs, QS_WHY_FORGET); qs_publish(); }
            vka_rp_queue_forget(j.serial);
        } else if (j.kind == J_PROG_FORGET) {   // the program's address (job_t.serial); a park still open is released first, as for a queue finalized
            if (qon) { qs_force_all(&g_qs, QS_WHY_PROG); qs_publish(); }
            vka_rp_prog_forget((const prog_t *)(uintptr_t)j.serial);
        }
        uint64_t t2 = wns(); g_busy_ns += t2 - t1; if (j.kind == J_SUB) { g_sub_ns += t2 - t1; atomic_fetch_add_explicit(&g_sub_all, t2 - t1, memory_order_relaxed); atomic_fetch_add_explicit(&g_sub_cpu_all, thread_cpu_ns() - c1, memory_order_relaxed); }
        if (j.kind == J_PRESENT && j.ticket % 300 == 0) {
            VLOG("replay worker up to present %llu: busy %.1f ms a frame (submissions %.1f), idle %.1f; the game waited %.1f ms a frame for the worker",
                 (unsigned long long)j.ticket, g_busy_ns / 300e6, g_sub_ns / 300e6, g_idle_ns / 300e6, g_room_ns / 300e6);
            g_busy_ns = g_idle_ns = g_room_ns = g_sub_ns = 0;
            if (qon) qs_stats(j.ticket); }
        pthread_mutex_lock(&g_mu);
        g_qout++; if (j.kind == J_PRESENT) g_pres_out++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static int up(void) {
    if (!vka_worker_on()) return 0;
    if (g_up) return g_up > 0;
    pthread_t th;
    if (pthread_create(&th, NULL, worker_main, NULL)) { VRED("cannot start the replay worker: submissions are replayed inline"); g_up = -1; g_on = 0; return 0; }
    pthread_detach(th); g_up = 1;
    VLOG("replay worker: submissions and presents are replayed on their own thread, at most %d frame%s behind the game (DM_VK_WORKER=0: inline)", g_frames, g_frames == 1 ? "" : "s");
    return 1;
}
static void push(const job_t *j) {
    uint64_t t0 = wns();
    pthread_mutex_lock(&g_mu);
    while (g_qin - g_qout >= NJOB || (j->kind == J_PRESENT && g_pres_in - g_pres_out >= (uint64_t)g_frames)) pthread_cond_wait(&g_cv, &g_mu);
    { uint64_t w = wns() - t0; g_room_ns += w; atomic_fetch_add_explicit(&g_room_all, w, memory_order_relaxed); }
    g_q[g_qin % NJOB] = *j; g_qin++; if (j->kind == J_PRESENT) g_pres_in++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// Syncs fenced inside the submission's command lists (OP_FENCE_SYNC), given the submission's fence now: the lists are walked as exec walks them (jumps, calls). On the
// way, each WAIT_SYNC record's sync as it stands at that point (a fence earlier in the same submission included): what the replay's wait looks at (vka_qsched.h qs_wait_sync)
static void fence_syncs(const uint8_t *p, uint64_t fence, int depth, job_t *j, int *cap) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, a, 8); if (depth < 16) fence_syncs((const uint8_t *)(uintptr_t)t, fence, depth + 1, j, cap); }
        if (h.op == OP_FENCE_SYNC) { sync_t *s; memcpy(&s, a, 8); s->fence = fence; s->fenced = 1; }
        if (h.op == OP_WAIT_SYNC && j) {
            const sync_t *s; memcpy(&s, a, 8);
            if (j->nws == *cap) { *cap = *cap ? 2 * *cap : 8; j->ws = realloc(j->ws, (size_t)*cap * sizeof *j->ws); }
            j->ws[j->nws++] = (qs_wsnap){p, s && s->fenced ? s->fence : 0};
            if (!j->w0done) { j->w0done = 1; j->w0kind = 2; j->w0fence = j->ws[j->nws - 1].fence; }
        }
        if (h.op == OP_WAIT_EVENT && j && !j->w0done) { p_wait_event w; memcpy(&w, a, sizeof w); j->w0done = 1; if (w.val) { j->w0kind = 1; j->w0word = w.val; j->w0mode = w.mode; j->w0value = w.value; } }
        if (h.op == OP_SIGNAL_EVENT && j) j->w0done = 1;   // a wait after a signal of its own: another queue may wait for that signal, so it is cut at the wait instead
        if (h.size < 8) return;
        p += h.size;
    }
}

// ---------- The mtl_* calls of the host's submit (hle_nvn.c nvnQueueSubmitCommands) and present ----------
// Replayed inline (DM_VK_WORKER=0, or no worker thread), on the game's thread: with the host's FPCR (vka_fpenv.h) for the recording and what the driver compiles
// meanwhile, and the game's put back before the call returns to it
void mtl_queue_activate(uint64_t serial) { if (up()) g_pend_serial = serial; else { uint64_t f = vka_fp_host(NULL); vka_rp_queue_activate(serial); vka_fp_back(f); } }
void mtl_set_queue_origin_lower_left(int on) { if (up()) g_pend_origin = on != 0; else vka_rp_set_queue_origin(on); }
void mtl_queue_forget(uint64_t serial) { if (up()) { job_t j = {J_FORGET}; j.serial = serial; push(&j); } else { uint64_t f = vka_fp_host(NULL); vka_rp_queue_forget(serial); vka_fp_back(f); } }
void mtl_prog_forget(const prog_t *p) { if (up()) { job_t j = {J_PROG_FORGET}; j.serial = (uint64_t)(uintptr_t)p; push(&j); } else { uint64_t f = vka_fp_host(NULL); vka_rp_prog_forget(p); vka_fp_back(f); } }
void mtl_sub_begin(void) {
    if (!up()) { uint64_t f = vka_fp_host(NULL); vka_rp_sub_begin(); vka_fp_back(f); return; }
    g_in_sub = 1; g_cur.nh = 0;
}
void mtl_exec(const uint8_t *handle) {
    if (!g_in_sub) { uint64_t f = vka_fp_host(NULL); vka_rp_exec(handle, 0); vka_fp_back(f); return; }
    if (!handle) return;
    if (g_cur.nh == g_cur_cap) { g_cur_cap = g_cur_cap ? 2 * g_cur_cap : 64; g_cur.h = realloc(g_cur.h, (size_t)g_cur_cap * sizeof *g_cur.h); }
    g_cur.h[g_cur.nh++] = handle;
}
uint64_t mtl_sub_commit(void) {
    if (!g_in_sub) { uint64_t f = vka_fp_host(NULL), r = vka_rp_sub_commit(0); vka_fp_back(f); return r; }
    g_in_sub = 0;
    job_t j = {J_SUB}; j.serial = g_pend_serial; j.origin_ll = g_pend_origin; j.nh = g_cur.nh;
    j.h = malloc((size_t)(j.nh ? j.nh : 1) * sizeof *j.h); if (j.nh) memcpy(j.h, g_cur.h, (size_t)j.nh * sizeof *j.h);
    j.fence = vka_fence_next(); j.frame = vka_frame();
    int cap = 0; for (int i = 0; i < j.nh; i++) fence_syncs(j.h[i], j.fence, 0, &j, &cap);
    push(&j);
    return j.fence;
}
// nvnQueueWaitSync on a fenced sync (hle_nvn.c): the queue's later work waits for that fence's submission
void mtl_queue_wait_fence(uint64_t serial, uint64_t fence) {
    if (!fence || !up()) return;   // inline: in submission order, which already has the fence's submission before it
    job_t j = {J_WAIT}; j.serial = serial; j.fence = fence; j.frame = vka_frame(); push(&j);
}
// nvnEventSignal wrote an event word on the CPU: a queue parked on it looks again
// (a gate on the word opens now, from this thread)
void mtl_event_cpu_signal(void) { if (g_up <= 0) return; vka_gate_poll(); pthread_mutex_lock(&g_mu); g_kick = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu); }
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    if (!up()) { uint64_t f = vka_fp_host(NULL), r = vka_present_inline(t, period_s, crop); vka_fp_back(f); return r; }
    job_t j = {J_PRESENT}; j.ticket = vka_present_reserve(); j.t = *t; j.period = period_s; j.has_crop = crop != NULL; if (crop) memcpy(j.crop, crop, sizeof j.crop);
    push(&j);
    return j.ticket;
}
#endif
