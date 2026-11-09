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
// The game runs at most DM_VK_WORKER_FRAMES (default 1) presents ahead of the worker. DM_VK_WORKER=0 replays inline on the submitting thread, as before.
#if defined(__ANDROID__)
#include "vka.h"
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

enum { J_SUB = 1, J_PRESENT, J_FORGET };
typedef struct {
    int kind;
    uint64_t serial, fence, frame; int origin_ll;   // J_SUB: the queue, its window origin, the fence reserved for it, the game's frame when it submitted
    const uint8_t **h; int nh;                   // J_SUB: the command handles, in order
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
static void *worker_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-replay"); vka_fp_host("vk-replay");
    for (;;) {
        uint64_t t0 = wns();
        pthread_mutex_lock(&g_mu);
        while (g_qout == g_qin) pthread_cond_wait(&g_cv, &g_mu);
        job_t j = g_q[g_qout % NJOB];
        pthread_mutex_unlock(&g_mu);
        uint64_t t1 = wns(), c1 = j.kind == J_SUB ? thread_cpu_ns() : 0; g_idle_ns += t1 - t0;
        aff_check();
        if (j.kind == J_SUB) {
            vka_rp_queue_activate(j.serial); vka_rp_set_queue_origin(j.origin_ll);
            vka_gp_sub_frame(j.frame); vka_exec_sub_frame(j.frame); vka_rp_sub_begin();
            for (int i = 0; i < j.nh; i++) vka_rp_exec(j.h[i], j.fence);
            vka_rp_sub_commit(j.fence);
            free(j.h);
        } else if (j.kind == J_PRESENT) vka_present_fill(j.ticket, &j.t, j.period, j.has_crop ? j.crop : NULL);
        else if (j.kind == J_FORGET) vka_rp_queue_forget(j.serial);
        uint64_t t2 = wns(); g_busy_ns += t2 - t1; if (j.kind == J_SUB) { g_sub_ns += t2 - t1; atomic_fetch_add_explicit(&g_sub_all, t2 - t1, memory_order_relaxed); atomic_fetch_add_explicit(&g_sub_cpu_all, thread_cpu_ns() - c1, memory_order_relaxed); }
        if (j.kind == J_PRESENT && j.ticket % 300 == 0) {
            VLOG("replay worker up to present %llu: busy %.1f ms a frame (submissions %.1f), idle %.1f; the game waited %.1f ms a frame for the worker",
                 (unsigned long long)j.ticket, g_busy_ns / 300e6, g_sub_ns / 300e6, g_idle_ns / 300e6, g_room_ns / 300e6);
            g_busy_ns = g_idle_ns = g_room_ns = g_sub_ns = 0; }
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

// Syncs fenced inside the submission's command lists (OP_FENCE_SYNC), given the submission's fence now: the lists are walked as exec walks them (jumps, calls)
static void fence_syncs(const uint8_t *p, uint64_t fence, int depth) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, a, 8); if (depth < 16) fence_syncs((const uint8_t *)(uintptr_t)t, fence, depth + 1); }
        if (h.op == OP_FENCE_SYNC) { sync_t *s; memcpy(&s, a, 8); s->fence = fence; s->fenced = 1; }
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
    for (int i = 0; i < j.nh; i++) fence_syncs(j.h[i], j.fence, 0);
    push(&j);
    return j.fence;
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    if (!up()) { uint64_t f = vka_fp_host(NULL), r = vka_present_inline(t, period_s, crop); vka_fp_back(f); return r; }
    job_t j = {J_PRESENT}; j.ticket = vka_present_reserve(); j.t = *t; j.period = period_s; j.has_crop = crop != NULL; if (crop) memcpy(j.crop, crop, sizeof j.crop);
    push(&j);
    return j.ticket;
}
#endif
