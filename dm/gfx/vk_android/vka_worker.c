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
//   replayed (vka_qsched.h); the other queues' submissions go on. DM_VK_EVENT_WAIT=0 passes every wait, as before 2026-11-09 (submission order only).
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
static uint64_t wns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// Where the time goes (logged every 300 presents): the worker busy replaying, idle waiting for a job; the game waiting for room in the queue
static uint64_t g_busy_ns, g_idle_ns, g_room_ns, g_sub_ns;
static _Atomic uint64_t g_room_all, g_sub_all, g_sub_cpu_all;   // cumulative (vka_worker_waits: the [longframe] line's attribution); sub_cpu: the worker's CPU time in submissions
void vka_worker_waits(uint64_t *room_ns, uint64_t *sub_ns) { if (room_ns) *room_ns = atomic_load(&g_room_all); if (sub_ns) *sub_ns = atomic_load(&g_sub_all); }
uint64_t vka_worker_sub_cpu(void) { return atomic_load(&g_sub_cpu_all); }   // a recording time far above it: the replay thread was blocked in the submission, not busy
static uint64_t thread_cpu_ns(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

enum { J_SUB = 1, J_PRESENT, J_FORGET, J_WAIT };
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
static _Atomic uint64_t g_park_min = UINT64_MAX, g_replaying, g_cpu_want;   // what a park holds back; the fence being replayed; the highest fence a CPU thread blocked on
static _Atomic int g_force_req;
static int g_kick;   // (under g_mu) an event word written on the CPU
static uint64_t qs_done_cb(void *c) { (void)c; return vka_done(); }
static uint64_t qs_now_cb(void *c) { (void)c; return wns(); }
static uint64_t qs_want_cb(void *c) { (void)c; return atomic_load(&g_cpu_want); }
static void qs_timeout_cb(void *c, const qs_sub *s) {
    (void)c; static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k > 8 && (k & (k - 1))) return;
    if (s->wkind == 1) VRED("a GPU wait parked queue %llu for %.0f ms: its producer did not come (event %p: wants %s %u, holds %u; fence %llu): released (%llu so far)",
                            (unsigned long long)s->serial, (wns() - s->t_park) / 1e6, (const void *)s->word, s->mode ? ">= (wrapping)" : "==", s->value, *s->word, (unsigned long long)s->fence, (unsigned long long)k);
    else VRED("a GPU wait parked queue %llu for %.0f ms: fence %llu was not handed to the queue (submission fence %llu): released (%llu so far)", (unsigned long long)s->serial,
              (wns() - s->t_park) / 1e6, (unsigned long long)s->wfence, (unsigned long long)s->fence, (unsigned long long)k);
}
static void qs_publish(void) { atomic_store(&g_park_min, qs_park_min(&g_qs)); }
// DM_VK_EVENT_LOG=n (restart): the first n parks and releases, with the wait, the value it saw and the submission replayed last (the producer, for a release)
static int g_evlog; static uint64_t g_last_serial, g_last_fence;
static void qs_note_cb(void *c, int what, const qs_sub *s) {
    (void)c; if (g_evlog <= 0) return; g_evlog--;
    char w[160];
    if (s->wkind == 1) snprintf(w, sizeof w, "event %p %s %u (the replay sees %u, memory %u)", (const void *)s->word, s->mode ? ">=" : "==", s->value, qs_ev_value(&g_qs, s->word), *s->word);
    else snprintf(w, sizeof w, "fence %llu", (unsigned long long)s->wfence);
    if (!what) VLOG("GPU wait: queue %llu submission %llu parked %s at %s; last replayed: queue %llu submission %llu", (unsigned long long)s->serial, (unsigned long long)s->fence,
                    !s->hi && !s->resumed ? "whole" : "after a cut", w, (unsigned long long)g_last_serial, (unsigned long long)g_last_fence);
    else VLOG("GPU wait: queue %llu submission %llu goes on after %.2f ms (%s) after queue %llu submission %llu", (unsigned long long)s->serial, (unsigned long long)s->fence,
              (wns() - s->t_park) / 1e6, w, (unsigned long long)g_last_serial, (unsigned long long)g_last_fence);
}
static int qs_run_cb(void *c, qs_sub *s) {
    (void)c;
    atomic_store(&g_replaying, s->fence);   // before any wait asks for g_cpu_want (vka_worker_cpu_wait does the opposite: its want, then this)
    vka_rp_queue_activate(s->serial); vka_rp_set_queue_origin(s->origin_ll);
    vka_gp_sub_frame(s->frame); vka_exec_sub_frame(s->frame); vka_rp_sub_begin();
    int parked = 0;
    while (s->hi < s->nh) {
        int r = s->resumed ? vka_rp_resume(&s->cont, s->fence, &s->cont) : vka_rp_exec_pk(s->h[s->hi], s->fence, &s->cont);
        if (r) { s->resumed = 1; parked = 1; break; }
        s->resumed = 0; s->hi++;
    }
    vka_rp_sub_commit_sig(s->fence, qs_commit(&g_qs, s->fence, !parked));
    qs_publish(); atomic_store(&g_replaying, 0);
    g_last_serial = s->serial; g_last_fence = s->fence;
    return parked;
}
static int qs_on(void) {
    if (g_qs_on < 0) {
        const char *e = getenv("DM_VK_EVENT_WAIT"); g_qs_on = !e || atoi(e);
        e = getenv("DM_VK_EVENT_PARK_MS"); double ms = e ? atof(e) : 200; if (!(ms >= 0)) ms = 200;
        g_qs.run = qs_run_cb; g_qs.done = qs_done_cb; g_qs.now = qs_now_cb; g_qs.cpu_want = qs_want_cb; g_qs.timeout = qs_timeout_cb; g_qs.park_ns = (uint64_t)(ms * 1e6);
        e = getenv("DM_VK_EVENT_LOG"); g_evlog = e ? atoi(e) : 0; if (g_evlog > 0) g_qs.note = qs_note_cb;
        VLOG("GPU waits across NVN queues: %s", g_qs_on ? "a wait parks its queue until its producer is replayed (DM_VK_EVENT_WAIT=0: every wait passes)" : "every wait passes (DM_VK_EVENT_WAIT=0)");
    }
    return g_qs_on;
}
// Waits asked by the replay (vka_exec.c); only the worker's own replays are parked, at its submissions (g_qs.cur)
int vka_wait_pass(uint32_t op, const void *a, const uint8_t *rec) {
    if (!t_worker || !g_qs.cur) return 1;
    if (op == OP_WAIT_EVENT) { p_wait_event w; memcpy(&w, a, sizeof w); return !w.val || qs_wait_event(&g_qs, w.val, w.mode, w.value); }
    return qs_wait_sync(&g_qs, rec);
}
void vka_event_replayed(volatile uint32_t *w, uint32_t mode, uint32_t v) { if (t_worker && g_qs.cur && w) qs_event_replayed(&g_qs, w, mode, v); }
// A CPU thread is about to block on fence: a park that holds it back (or the submission being replayed, which may park) is released, as the GPU would not leave that
// thread waiting for a producer the game has not submitted yet
void vka_worker_cpu_wait(uint64_t fence) {
    if (t_worker || g_qs_on <= 0) return;
    uint64_t w = atomic_load(&g_cpu_want); while (w < fence && !atomic_compare_exchange_weak(&g_cpu_want, &w, fence)) {}
    uint64_t pm = atomic_load(&g_park_min), rp = atomic_load(&g_replaying);
    if (pm <= fence || (rp && rp <= fence)) { atomic_store(&g_force_req, 1); pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu); }
}
static void qs_step(void) {   // a CPU wait's release, or the parked queues whose waits hold now
    if (!qs_parked(&g_qs)) { atomic_store(&g_force_req, 0); return; }
    if (atomic_exchange(&g_force_req, 0)) qs_force_all(&g_qs, QS_WHY_CPU_WAIT); else qs_poll(&g_qs);
    qs_publish();
}
// After a job: a park that holds back a fence a CPU thread already blocked on is released (a queue-level wait, or a submission behind a park, never asks qs_cpu_passes;
// the waiter saw no park then, so it sent no request). A fence not done that a CPU thread waited for is still being waited for
static void qs_cpu_check(void) {
    if (qs_parked(&g_qs) && atomic_load(&g_cpu_want) >= atomic_load(&g_park_min)) { qs_force_all(&g_qs, QS_WHY_CPU_WAIT); qs_publish(); }
}
static void qs_stats(uint64_t ticket) {
    static uint64_t p0, w0, r0, c0, f0[QS_NWHY];
    uint64_t df[QS_NWHY], any = g_qs.n_park - p0 + g_qs.n_pass_cpu - c0;
    for (int i = 0; i < QS_NWHY; i++) { df[i] = g_qs.n_force[i] - f0[i]; any += df[i]; }
    if (any) {
        char b[300]; int n = 0;
        for (int i = 0; i < QS_NWHY; i++) if (df[i]) n += snprintf(b + n, sizeof b - (size_t)n, ", %s %llu", qs_why_name[i], (unsigned long long)df[i]);
        VLOG("GPU waits up to present %llu: %llu parked (%llu whole, before any of the submission), %llu went on after their producer, %llu passed for a CPU wait; released early by%s",
             (unsigned long long)ticket, (unsigned long long)(g_qs.n_park - p0), (unsigned long long)(g_qs.n_whole - w0), (unsigned long long)(g_qs.n_release - r0),
             (unsigned long long)(g_qs.n_pass_cpu - c0), n ? b : " nothing");
    }
    p0 = g_qs.n_park; w0 = g_qs.n_whole; r0 = g_qs.n_release; c0 = g_qs.n_pass_cpu; memcpy(f0, g_qs.n_force, sizeof f0);
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
            if (g_kick || atomic_load(&g_force_req)) break;
            // Parked with nothing to replay: a word may change through a plain store to mapped pool memory, and the park time runs: looked at every millisecond
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &dl);
            break;
        }
        g_kick = 0;
        pthread_mutex_unlock(&g_mu);
        uint64_t t1 = wns(), c1 = have && j.kind == J_SUB ? thread_cpu_ns() : 0; g_idle_ns += t1 - t0;
        if (qon) qs_step();
        if (!have) continue;
        aff_check();
        if (j.kind == J_SUB && qon) {
            qs_sub *q = calloc(1, sizeof *q);
            q->serial = j.serial; q->fence = j.fence; q->frame = j.frame; q->origin_ll = j.origin_ll; q->h = j.h; q->nh = j.nh; q->ws = j.ws; q->nws = j.nws;
            q->w0kind = j.w0kind; q->w0word = j.w0word; q->w0mode = j.w0mode; q->w0value = j.w0value; q->w0fence = j.w0fence;
            qs_submit(&g_qs, q); qs_poll(&g_qs); qs_publish();
        } else if (j.kind == J_SUB) {
            vka_rp_queue_activate(j.serial); vka_rp_set_queue_origin(j.origin_ll);
            vka_gp_sub_frame(j.frame); vka_exec_sub_frame(j.frame); vka_rp_sub_begin();
            for (int i = 0; i < j.nh; i++) vka_rp_exec(j.h[i], j.fence);
            vka_rp_sub_commit(j.fence);
            free(j.h); free(j.ws);
        } else if (j.kind == J_WAIT) {
            if (qon) { qs_sub *q = calloc(1, sizeof *q); q->serial = j.serial; q->wait_fence = j.fence; q->frame = j.frame; qs_submit(&g_qs, q); qs_publish(); }
        } else if (j.kind == J_PRESENT) {
            // Presented after every submission before it was handed to the queue and signalled (vka_present_fill waits for the last value signalled): a park still open here
            // is released first, so the frame holds the work the game submitted before the present
            if (qon) { qs_force_all(&g_qs, QS_WHY_PRESENT); qs_publish(); }
            vka_present_fill(j.ticket, &j.t, j.period, j.has_crop ? j.crop : NULL);
        } else if (j.kind == J_FORGET) {
            if (qon) { qs_force_all(&g_qs, QS_WHY_FORGET); qs_publish(); }
            vka_rp_queue_forget(j.serial);
        }
        if (qon) qs_cpu_check();
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
void mtl_event_cpu_signal(void) { if (g_up <= 0) return; pthread_mutex_lock(&g_mu); g_kick = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu); }
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    if (!up()) { uint64_t f = vka_fp_host(NULL), r = vka_present_inline(t, period_s, crop); vka_fp_back(f); return r; }
    job_t j = {J_PRESENT}; j.ticket = vka_present_reserve(); j.t = *t; j.period = period_s; j.has_crop = crop != NULL; if (crop) memcpy(j.crop, crop, sizeof j.crop);
    push(&j);
    return j.ticket;
}
#endif
