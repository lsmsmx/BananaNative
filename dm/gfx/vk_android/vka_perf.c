// CPU scheduling for the replay on Android: what Apple's time-constraint policy does for nvn_mtl.m's present thread (hle_nvn.c set_time_constraint_comp), here an ADPF
// performance hint session (APerformanceHint_*, Android 13): the threads that carry a frame (the game's submitting and presenting threads, the replay's present and completion
// threads) are in one session whose target is the present period, and each frame reports how long the submitting thread worked on it (its CPU time between two presents).
// The system then keeps the clocks those threads need, and moves them to faster cores, instead of ramping down between frames and back up late (the frame-to-frame swings on the
// SM8850 phone). The replay's own threads also run at THREAD_PRIORITY_URGENT_DISPLAY (nice -8). DM_VK_ADPF=0 turns the session off.
// The entry points are looked up at run time: libandroid has them from API 33, setThreads from 34.
// From API 35 the report also carries the frame's GPU time (APerformanceHint_reportActualWorkDuration2, AWorkDuration_setActualGpuDurationNanos): the GPU's governor sees
// the frame's GPU work against the target instead of only its busy percentage (on the SM8850 phone the same replayed frame took 35 ms of GPU time right after a busy
// stretch and 94-98 ms after 2.6 s of idle GPU: its clock follows load). DM_VK_ADPF_GPU=0 (live) reports CPU time only
#if defined(__ANDROID__)
#include "vka.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <pthread.h>
uint64_t vka_frame(void); double mtl_gpu_ms(uint64_t f);

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
static struct {
    APerformanceHintManager *(*get)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*target)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    int (*threads)(APerformanceHintSession *, const int32_t *, size_t);
    void (*close)(APerformanceHintSession *);
    void *(*wd_create)(void); void (*wd_release)(void *); void (*wd_start)(void *, int64_t); void (*wd_total)(void *, int64_t); void (*wd_cpu)(void *, int64_t);
    void (*wd_gpu)(void *, int64_t); int (*report2)(APerformanceHintSession *, void *);
    // API 36: a session made from a creation config, marked as part of the graphics pipeline (the frame's work, CPU and GPU, rather than any thread's); the feature query
    void *(*cfg_create)(void); void (*cfg_release)(void *); void (*cfg_tids)(void *, const int32_t *, size_t); void (*cfg_target)(void *, int64_t); void (*cfg_pipeline)(void *, _Bool);
    int (*create_cfg)(APerformanceHintManager *, void *, APerformanceHintSession **); _Bool (*feature)(int);
} H;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static APerformanceHintSession *g_s; static int g_state;   // 0 not tried, 1 usable, -1 not available / off
enum { MAXT = 16 };
static int32_t g_tid[MAXT]; static int g_nt, g_dirty;
static int64_t g_target = 16666667;

static int avail(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    const char *e = getenv("DM_VK_ADPF"); if (e && !atoi(e)) { VLOG("DM_VK_ADPF=0: no performance hint session"); return 0; }
    void *h = dlopen("libandroid.so", RTLD_NOW);
    if (!h) return 0;
    H.get = dlsym(h, "APerformanceHint_getManager"); H.create = dlsym(h, "APerformanceHint_createSession"); H.target = dlsym(h, "APerformanceHint_updateTargetWorkDuration");
    H.report = dlsym(h, "APerformanceHint_reportActualWorkDuration"); H.threads = dlsym(h, "APerformanceHint_setThreads"); H.close = dlsym(h, "APerformanceHint_closeSession");
    H.wd_create = dlsym(h, "AWorkDuration_create"); H.wd_release = dlsym(h, "AWorkDuration_release"); H.wd_start = dlsym(h, "AWorkDuration_setWorkPeriodStartTimestampNanos");
    H.wd_total = dlsym(h, "AWorkDuration_setActualTotalDurationNanos"); H.wd_cpu = dlsym(h, "AWorkDuration_setActualCpuDurationNanos");
    H.wd_gpu = dlsym(h, "AWorkDuration_setActualGpuDurationNanos"); H.report2 = dlsym(h, "APerformanceHint_reportActualWorkDuration2");
    if (!H.get || !H.create || !H.target || !H.report || !H.get()) { VRED("no ADPF performance hint manager: the frame's threads run on the default governor"); return 0; }
    H.cfg_create = dlsym(h, "ASessionCreationConfig_create"); H.cfg_release = dlsym(h, "ASessionCreationConfig_release"); H.cfg_tids = dlsym(h, "ASessionCreationConfig_setTids");
    H.cfg_target = dlsym(h, "ASessionCreationConfig_setTargetWorkDurationNanos"); H.cfg_pipeline = dlsym(h, "ASessionCreationConfig_setGraphicsPipeline");
    H.create_cfg = dlsym(h, "APerformanceHint_createSessionUsingConfig"); H.feature = dlsym(h, "APerformanceHint_isFeatureSupported");
    if (H.feature) {   // APerformanceHintFeature in the NDK's order: sessions, power efficiency, surface binding, graphics pipeline, auto CPU, auto GPU
        static const char *const nm[] = {"sessions", "power efficiency", "surface binding", "graphics pipeline", "auto CPU", "auto GPU"};
        char b[160]; size_t o = 0; for (int i = 0; i < 6 && o < sizeof b; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s%s %d", i ? "," : "", nm[i], (int)H.feature(i));
        VLOG("ADPF features: %s; report with GPU time %s", b, H.report2 && H.wd_gpu ? "yes" : "no");
    }
    g_state = 1;
    return 1;
}
static void session_update(void) {   // g_mu held
    if (!g_dirty || !g_nt) return;
    g_dirty = 0;
    if (g_s && H.threads && !H.threads(g_s, g_tid, (size_t)g_nt)) return;
    if (g_s && H.close) H.close(g_s);
    g_s = NULL; int pipe = 0;
    // DM_VK_ADPF_PIPELINE=0 (restart with the session; default 1): a plain session. With 1 (API 36) the session is made as part of the graphics pipeline, whose reported
    // GPU time is the platform's input for the GPU's clock as well; a session the platform refuses as one (it limits graphics pipeline threads) is made plainly
    if (H.cfg_create && H.cfg_tids && H.cfg_target && H.cfg_pipeline && H.create_cfg && VKA_ENV("DM_VK_ADPF_PIPELINE", 1)) {
        void *c = H.cfg_create();
        if (c) { H.cfg_tids(c, g_tid, (size_t)g_nt); H.cfg_target(c, g_target); H.cfg_pipeline(c, 1);
                 int r = H.create_cfg(H.get(), c, &g_s); if (r || !g_s) { static int n; if (n++ < 2) VRED("ADPF graphics pipeline session of %d threads not made (%d): a plain one", g_nt, r); g_s = NULL; } else pipe = 1;
                 if (H.cfg_release) H.cfg_release(c); }
    }
    if (!g_s) g_s = H.create(H.get(), g_tid, (size_t)g_nt, g_target);
    if (!g_s) { static int n; if (n++ < 2) VRED("ADPF session of %d threads not made", g_nt); }
    else VLOG("ADPF session%s: %d threads, target %.2f ms", pipe ? " (graphics pipeline)" : "", g_nt, g_target / 1e6);
}
// The calling thread takes part in frames: into the session (once)
void vka_perf_thread(int nice) {
    static __thread int in;
    if (in) return;
    in = 1;
    if (nice) setpriority(PRIO_PROCESS, 0, nice);
    if (!avail()) return;
    int32_t t = (int32_t)gettid();
    pthread_mutex_lock(&g_mu);
    int k = 0; while (k < g_nt && g_tid[k] != t) k++;
    if (k == g_nt && g_nt < MAXT) { g_tid[g_nt++] = t; g_dirty = 1; }
    session_update();
    pthread_mutex_unlock(&g_mu);
}
// Once per frame, on the thread that presents (mtl_present): the present period the game asked for, and this thread's CPU time since its last present as the work done
// DM_VK_ADPF_SPAN=p (live; default 0 = off): the reports with GPU time say how long the frame's work took, not how long the frame was shown. Without it the total
// duration of each report is the wall time between two presents, which is the present period itself: every report reads "exactly on target" whatever the work took
// (DKB's 20 ms of recording at 30 fps, or 36 ms at a 2 GHz cap that put it at 20 fps), so the session can neither see slack to give back nor a frame it is about to
// miss. With p > 0 the report is the newest frame whose GPU work is done: its start the replay's start of recording its first submission, its total that start to
// the GPU end of its last submission (vka_exec.c, the same span as the adaptive GPU clock floor; the game's frame-rate controller measures about the same), CPU the
// replay thread's time, GPU its busy time; the target p % of the period, at most 33.3 ms (so a game the controller dropped to 20 fps still aims at 30: with the 50 ms
// period as target the session would lower the clocks until the frame took 50 ms and keep it there). Each frame is reported once. Not phone-checked (STUTTER-TEST.md)
// Since 2026-11-05 the period is the game's shortest (vka_dev.c vka_floor_period_ns: 16.7 ms unless the game asked for less, or DM_VK_GPU_FLOOR_PERIOD), not the current
// one capped at 33.3 ms: at 30 fps a frame the controller could only take back to 60 fps once it fits in a vsync. Unset (-1), DM_VK_ADPF_SPAN is 100 when the session is
// the GPU clock floor (vka_floor_adpf: neither VK_QCOM_queue_perf_hint nor the KGSL constraint) and 0 otherwise. With the queue hint or KGSL as the floor the session still
// sets the CPU clocks and cores of the replay's threads, and the old reports read a frame recorded in 30 ms at 20 fps as on target against the 50 ms period; whether that
// holds the frame rate low after the phone cools is open (docs/reference/frame-rate.md "A dynamic interval that does not step back up after the phone cools on the Vulkan
// replay"): DM_VK_ADPF_SPAN=100 is that A/B arm, the default stays until a phone run shows it helps
static int span_pct(void) { int vka_floor_adpf(void); int p = VKA_ENV("DM_VK_ADPF_SPAN", -1); if (p < 0) p = vka_floor_adpf() ? 100 : 0; return p <= 0 ? 0 : p > 100 ? 100 : p; }
static int64_t span_target(double period_s, int pct) { (void)period_s; int64_t vka_floor_period_ns(void); return vka_floor_period_ns() * pct / 100; }
uint64_t vka_floor_span_of(uint64_t f); uint64_t vka_frame_start_ns(uint64_t f); uint64_t now_ns(void);
static void pace_tick(double period_s);
void vka_perf_frame(double period_s) {
    pace_tick(period_s);
    if (period_s > 0.004 && period_s < 0.2) {   // the shortest period the game asked for (vka_dev.c vka_floor_period_ns)
        extern _Atomic int64_t vka_period_min_ns; int64_t p = (int64_t)(period_s * 1e9), m = atomic_load_explicit(&vka_period_min_ns, memory_order_relaxed);
        if (!m || p < m) atomic_store_explicit(&vka_period_min_ns, p, memory_order_relaxed);
    }
    if (!avail()) return;
    vka_perf_thread(0);
    static __thread int64_t last; struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    int64_t c = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec, work = last ? c - last : 0; last = c;
    static __thread int64_t wlast; clock_gettime(CLOCK_MONOTONIC, &ts); int64_t w = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec, w0 = wlast; wlast = w;
    int64_t gpu = 0;   // the newest frame whose GPU time is in (it comes in a few frames late)
    int64_t s_start = 0, s_total = 0;   // DM_VK_ADPF_SPAN: that frame's start (CLOCK_MONOTONIC) and its span
    int with_gpu = H.report2 && H.wd_create && H.wd_gpu && VKA_ENV("DM_VK_ADPF_GPU", 1), pct = with_gpu ? span_pct() : 0;   // (the span goes in the reports with GPU time only)
    if (with_gpu) {
        static __thread uint64_t reported; uint64_t f = vka_frame();
        for (uint64_t k = 2; k < 14 && k < f; k++) {
            uint64_t g = f - k; double gm = mtl_gpu_ms(g); if (!(gm > 0)) continue;
            if (!pct) { gpu = (int64_t)(gm * 1e6); break; }
            if (g <= reported) break;   // reported already: nothing newer is in
            uint64_t sp = vka_floor_span_of(g), st = vka_frame_start_ns(g), now = now_ns();
            if (!sp || !st || st > now || now - st > 2000000000ull) continue;
            gpu = (int64_t)(gm * 1e6); s_total = (int64_t)sp; s_start = w - (int64_t)(now - st); reported = g;
            break;
        }
    }
    int64_t tgt = pct ? span_target(period_s, pct) : period_s > 0.004 && period_s < 0.2 ? (int64_t)(period_s * 1e9) : 16666667;
    pthread_mutex_lock(&g_mu);
    session_update();
    if (g_s && tgt != g_target) { g_target = tgt; H.target(g_s, tgt); VLOG("ADPF target %.2f ms", tgt / 1e6); }
    if (g_s && pct) {
        if (gpu > 0 && s_total > 0 && s_start > 0) {
            static void *wd; if (!wd) wd = H.wd_create();
            if (wd) { H.wd_start(wd, s_start); H.wd_total(wd, s_total); H.wd_cpu(wd, work > 0 ? work : 0); H.wd_gpu(wd, gpu);
                      int r = H.report2(g_s, wd); static int nl; if (r && nl++ < 3) VRED("ADPF report of the frame's span: %d", r); else if (!r && !nl++) VLOG("ADPF: frames report their span (%.2f ms, GPU %.2f ms) against %.2f ms (DM_VK_ADPF_SPAN=%d)", s_total / 1e6, gpu / 1e6, tgt / 1e6, pct); }
        }
    } else if (g_s && gpu > 0 && w0 && w > w0) {
        static void *wd; if (!wd) wd = H.wd_create();
        if (wd) { H.wd_start(wd, w0); H.wd_total(wd, w - w0); H.wd_cpu(wd, work > 0 ? work : 0); H.wd_gpu(wd, gpu);
                  int r = H.report2(g_s, wd); static int nl; if (r && nl++ < 3) VRED("ADPF report with GPU time: %d", r); else if (!r && !nl++) VLOG("ADPF: frames report their GPU time (%.2f ms)", gpu / 1e6); }
    } else if (g_s && work > 0) H.report(g_s, work);
    pthread_mutex_unlock(&g_mu);
}
// ---------- The pace line (vka_pace.h) ----------
// DM_VK_PACE_LOG=<ms> (live; default 5000, 0 off): every that many ms, on the replay thread at a present, one line with what decides a dynamic interval's frame rate:
//   [vk] pace up to frame F (5.0 s): interval 2 60 %, 3 40 %; 24.1 fps; GPU busy p50/p90 31.2/40.3 ms, recording start to GPU end p50/p90 48.0/60.1 ms; replay recording
//   32.0 ms a frame (its CPU 21.0), 21000 draws; GPU clock floor KGSL 100 % (0); ADPF span against 16.7 ms; window 30 fps, panel 30.00 Hz; thermal 3, headroom nan; CPU MHz 2745/3628 3129/4608
// The game's controller (DKB main+0x204a18) steps up only once its measure of a frame (start to GPU end, about the span here) fits the faster interval, so the line tells
// a frame that cannot fit (the span, the recording or the GPU time over the period of the next faster interval) from one held by the clocks: the floor, ADPF, the thermal
// status (AThermal_getCurrentThermalStatus: 0 none, 1 light, 2 moderate, 3 severe, 4 critical) and the CPU clocks (cpufreq policies: now / cap). The window's rate and the
// panel's refresh show whether the panel runs below the game's rate (vka_present.c frame_rate, DM_VK_FRAME_RATE_HOLD)
#include "vka_pace.h"
void vka_worker_waits(uint64_t *room_ns, uint64_t *sub_ns); uint64_t vka_worker_sub_cpu(void); void mtl_stats(uint64_t *passes, uint64_t *draws);
const char *vka_floor_state(int *pct, int *res); void vka_present_rates(int *fps, double *hz);
static int pace_thermal(float *headroom) {   // the thermal status (-1: not available) and the headroom (NaN when the platform has none)
    static int looked; static void *mgr; static int (*st)(void *); static float (*hr)(void *, int);
    if (!looked) { looked = 1; void *h = dlopen("libandroid.so", RTLD_NOW); void *(*acq)(void) = h ? (void *(*)(void))dlsym(h, "AThermal_acquireManager") : NULL;
                   if (acq) { mgr = acq(); st = (int (*)(void *))dlsym(h, "AThermal_getCurrentThermalStatus"); hr = (float (*)(void *, int))dlsym(h, "AThermal_getThermalHeadroom"); } }
    *headroom = mgr && hr ? hr(mgr, 0) : __builtin_nanf("");
    return mgr && st ? st(mgr) : -1;
}
static void pace_cpu(char *b, size_t n) {   // "now/cap" MHz of each cpufreq policy
    size_t o = 0; b[0] = 0;
    for (int k = 0; k < 16 && o < n; k++) {
        char p[96]; unsigned long cur = 0, max = 0; FILE *f;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", k); if (!(f = fopen(p, "r"))) continue; if (fscanf(f, "%lu", &cur) != 1) cur = 0; fclose(f);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", k); if ((f = fopen(p, "r"))) { if (fscanf(f, "%lu", &max) != 1) max = 0; fclose(f); }
        int w = snprintf(b + o, n - o, "%s%lu/%lu", o ? " " : "", cur / 1000, max / 1000); if (w < 0) break; o += (size_t)w;
    }
    if (!b[0]) snprintf(b, n, "not readable");
}
// The second pace line: where the recording's time goes. Its parts a frame: the draws (the timed sample's mean per draw, vka_draw.c, times the draws), the texture work at each
// submission's start (vka_tex_prepare), the rest of the start, the commit; the remainder is the walk of the other records (passes, clears, copies, barriers, state). Then
// the replay thread's and the encoder's scheduling: time on a core, time runnable but waiting for one (another thread had it), and their switches (voluntary: blocked
// on a lock, a futex or a fence; involuntary: preempted); then the encoder's time by kind of call and the draw path by phase
void vka_draw_pace(char *b, size_t n, double *draw_us); void vka_enc_pace(char *b, size_t n, double frames); void vka_exec_phases(uint64_t *begin, uint64_t *prep, uint64_t *commit);
int vka_rec_tid(void); int vka_enc_tid(void);
static int pace_thr(int tid, vkpace_thr *o) {
    if (tid <= 0) return 0;
    char p[64], b[2048]; FILE *f; int ok = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    if ((f = fopen(p, "re"))) { size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f); ok = vkpace_schedstat(b, o); }
    snprintf(p, sizeof p, "/proc/self/task/%d/status", tid);
    if (ok && (f = fopen(p, "re"))) { size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f); if (!vkpace_status(b, o)) o->vol = o->invol = 0; }
    return ok;
}
static void pace_cost(uint64_t f, double nf, uint64_t draws, double rec_ms) {
    static int rtid, etid; static vkpace_thr r0, e0; static uint64_t b0, p0, c0, tp0[4];
    uint64_t bg, pr, cm; vka_exec_phases(&bg, &pr, &cm);
    uint64_t tp1[4]; vka_tex_prep_phases(&tp1[0], &tp1[1], &tp1[2], &tp1[3]);
    int rt = vka_rec_tid(), et = vka_enc_tid(); vkpace_thr r1 = {0}, e1 = {0};
    int rok = pace_thr(rt, &r1), eok = pace_thr(et, &e1);
    int rfirst = rt != rtid, efirst = et != etid;   // (a new thread: its counters from the next window)
    vkpace_thr rd = vkpace_thr_delta(&r0, &r1), ed = vkpace_thr_delta(&e0, &e1);
    char dp[700], ep[400]; double dus = 0; vka_draw_pace(dp, sizeof dp, &dus); vka_enc_pace(ep, sizeof ep, nf);
    double dms = nf ? dus * (double)draws / nf / 1e3 : 0, pms = nf ? (pr - p0) / 1e6 / nf : 0, bms = nf ? (bg - b0) / 1e6 / nf - pms : 0, cms = nf ? (cm - c0) / 1e6 / nf : 0;
    double tm[4] = {0, 0, 0, 0};   // texture prepare's own parts a frame: the pruner, the CPU re-uploads, the write log, the GPU-write scan
    if (nf) for (int i = 0; i < 4; i++) { tm[i] = (double)(tp1[i] - tp0[i]) / 1e6 / nf; tp0[i] = tp1[i]; }
    char rs[160], es[160];
    if (rok && !rfirst && nf) snprintf(rs, sizeof rs, "on a core %.1f ms, waited for a core %.1f ms, %.0f blocked / %.0f preempted switches", rd.run_ns / 1e6 / nf, rd.wait_ns / 1e6 / nf, rd.vol / nf, rd.invol / nf);
    else snprintf(rs, sizeof rs, "%s", rok ? "from the next line" : "not readable");
    if (eok && !efirst && nf) snprintf(es, sizeof es, "on a core %.1f ms, waited for a core %.1f ms, %.0f blocked / %.0f preempted switches", ed.run_ns / 1e6 / nf, ed.wait_ns / 1e6 / nf, ed.vol / nf, ed.invol / nf);
    else snprintf(es, sizeof es, "%s", eok ? "from the next line" : et ? "not readable" : "off");
    if (nf) VLOG("pace cost up to frame %llu, a frame: recording %.1f ms = draws %.1f + texture prepare %.1f (prune %.1f, re-uploads %.1f, write log %.1f, GPU-write scan %.1f, rest %.1f) + submission start %.1f + commit %.1f + other records %.1f; replay thread %s; encoder %s; "
                 "encoder calls ms (count): %s; %s", (unsigned long long)f, rec_ms, dms, pms, tm[0], tm[1], tm[2], tm[3], pms - tm[0] - tm[1] - tm[2] - tm[3], bms, cms,
                 rec_ms - dms - pms - bms - cms, rs, es, ep[0] ? ep : "none", dp[0] ? dp : "no draws timed");
    rtid = rt; etid = et; if (rok) r0 = r1; if (eok) e0 = e1; b0 = bg; p0 = pr; c0 = cm;
}
static void pace_tick(double period_s) {
    static vkpace P; static uint64_t sampled;
    int ms = VKA_ENV("DM_VK_PACE_LOG", 5000); if (ms <= 0) return;
    uint64_t now = now_ns(), f = vka_frame(), draws = 0, rec = 0, cpu = vka_worker_sub_cpu(); mtl_stats(NULL, &draws); vka_worker_waits(NULL, &rec);
    if (!P.t0) { vkpace_reset(&P, now, f, draws, rec, cpu); sampled = f > 10 ? f - 10 : 0; return; }   // (sampled <= f - 10 from here on: the catch-up below cannot wrap)
    vkpace_frame(&P, period_s);
    if (f > 10 + 64 && f - 10 - sampled > 64) sampled = f - 10 - 64;   // (the rings keep 64 frames)
    for (uint64_t g = sampled + 1; f > 10 && g <= f - 10; g++) { double sp = vka_floor_span_of(g) / 1e6; vkpace_gpu(&P, mtl_gpu_ms(g), sp); }   // GPU times come in a few frames late
    if (f > 10 && f - 10 > sampled) sampled = f - 10;
    if (now - P.t0 < (uint64_t)ms * 1000000ull) return;
    double s = (now - P.t0) / 1e9, nf = f > P.f0 ? (double)(f - P.f0) : 0;
    char itv[96], cpus[160]; vkpace_intervals(&P, itv, sizeof itv); pace_cpu(cpus, sizeof cpus);
    int fl = -1, flr = 0; const char *fk = vka_floor_state(&fl, &flr); int wfps = 0; double hz = 0; vka_present_rates(&wfps, &hz);
    float head; int th = pace_thermal(&head);
    int pct = H.report2 && H.wd_create && H.wd_gpu && VKA_ENV("DM_VK_ADPF_GPU", 1) ? span_pct() : 0;
    pthread_mutex_lock(&g_mu); int have = g_state > 0 && g_s; int64_t tg = g_target; pthread_mutex_unlock(&g_mu);   // (other threads join the session under g_mu)
    char adpf[64]; if (!have) snprintf(adpf, sizeof adpf, "no session"); else if (pct) snprintf(adpf, sizeof adpf, "span against %.1f ms", tg / 1e6); else snprintf(adpf, sizeof adpf, "present period against %.1f ms", tg / 1e6);
    char flb[48]; if (fl < 0) snprintf(flb, sizeof flb, "%s", fk); else snprintf(flb, sizeof flb, "%s %d %% (%d)", fk, fl, flr);
    VLOG("pace up to frame %llu (%.1f s): interval %s; %.1f fps; GPU busy p50/p90 %.1f/%.1f ms, recording start to GPU end p50/p90 %.1f/%.1f ms; replay recording %.1f ms a frame (its CPU %.1f), %.0f draws; "
         "GPU clock floor %s; ADPF %s; window %d fps, panel %.2f Hz; thermal %d, headroom %.2f; CPU MHz %s",
         (unsigned long long)f, s, itv, nf / s, vkpace_pct(P.gpu_us, P.ng, 50), vkpace_pct(P.gpu_us, P.ng, 90), vkpace_pct(P.span_us, P.ns, 50), vkpace_pct(P.span_us, P.ns, 90),
         nf ? (rec - P.rec0) / 1e6 / nf : 0.0, nf ? (cpu - P.cpu0) / 1e6 / nf : 0.0, nf ? (draws - P.draws0) / nf : 0.0, flb, adpf, wfps, hz, th, (double)head, cpus);
    pace_cost(f, nf, draws - P.draws0, nf ? (rec - P.rec0) / 1e6 / nf : 0.0);
    vkpace_reset(&P, now, f, draws, rec, cpu);
}
// hle_nvn.c's time-constraint request for its acquire / present thread (Apple's THREAD_TIME_CONSTRAINT_POLICY): that thread joins the session, at display priority
void dm_perf_time_constraint(uint64_t period_ns, uint64_t comp_ns) {
    (void)comp_ns;
    vka_perf_thread(-8);
    if (!avail()) return;
    pthread_mutex_lock(&g_mu);
    if (!(H.report2 && span_pct()) && period_ns >= 4000000 && period_ns <= 200000000 && (int64_t)period_ns != g_target) { g_target = (int64_t)period_ns; if (g_s) H.target(g_s, g_target); }   // (with DM_VK_ADPF_SPAN vka_perf_frame sets it)
    pthread_mutex_unlock(&g_mu);
}
// ---------- The [longframe] line's attribution on the Vulkan replay (hle_nvn.c lf_attr's hooks) ----------
// mtl_frame_extra, at the end of each game frame on the game's presenting thread: what the replay side spent or waited since the last frame. "game waited for the replay
// worker" is the game's submit blocked on a full job queue (vka_worker.c push: the replay is a frame behind and still recording); "replay recording" the worker's time in
// submissions; "replay waited for the encoder" the replay thread blocked on the command encoder's ring (vka_enc.c); "cache save" the saver thread's time writing the
// pipeline cache (off the frame path since 2026-11-03, listed to rule it out); "waited for prewarm" the frame path waiting for a prewarm thread that was making the same
// pipeline; "pool placement" the time memory pools took to become placed Vulkan memory on any thread (vka_mem.c; the work only, not the wait for another placement),
// "give-back" the time finalized pools took to become anonymous memory again (with the wait for the GPU to be done with them), and "the pool table's lock waited for"
// the lookups that waited for the segment table meanwhile, on the replay thread and on the others (the game's NVN calls). mtl_gpu_extra, when the line is printed (8 frames later): the GPU's span against its busy time (vka_exec.c): a span far above the busy time is GPU time spent
// waiting for the recording, i.e. a CPU stall on the replay thread, not GPU work
void vka_worker_waits(uint64_t *room_ns, uint64_t *sub_ns); uint64_t vka_enc_waits(void); uint64_t vka_worker_sub_cpu(void);
void vka_mem_waits(uint64_t *place_ns, uint64_t *place_bytes, uint64_t *place_n, uint64_t *give_ns, uint64_t *wait_rec_ns, uint64_t *wait_other_ns);
// The busiest threads of a long frame. A sampler thread (dm-lfthreads) reads every thread's run time (/proc/self/task/<tid>/schedstat, ns; else stat's utime + stime in
// clock ticks) every DM_LONGFRAME_THREADS ms (250; 0 = off, restart) into a ring. A frame of at least 100 ms posts a request at its end; the sampler takes a fresh reading
// and names the 3 threads that ran most since the newest reading older than the frame's start (the window is the frame plus at most one period, and the moment the sampler
// took it). The line is printed 8 frames later (mtl_gpu_extra), by when the answer is in; the presenting thread never scans /proc itself. Names are /proc's comm: the guest
// threads carry "<handle> <guest name>" (hle_thread.c), the replay's "vk-…". A loading hitch whose own thread is idle then says who ran: the game's loader, a vk- thread, nobody
enum { TS_MAX = 192, TS_RING = 16, TS_REQ = 16 };
typedef struct { uint64_t t; int n; struct { int tid; uint64_t ns; } e[TS_MAX]; } tsnap;
static tsnap g_ts[TS_RING]; static unsigned g_tsn;
static struct { uint64_t f, since; int state; char s[220]; } g_tsq[TS_REQ];   // state 1 = asked, 2 = answered (under g_tsmu)
static pthread_mutex_t g_tsmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_tscv = PTHREAD_COND_INITIALIZER;
static uint64_t ts_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int ts_read(tsnap *s) {
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    static long hz; if (!hz) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    struct dirent *de; s->n = 0; s->t = ts_now(); char path[64], b[512];
    while ((de = readdir(d)) && s->n < TS_MAX) {
        int tid = atoi(de->d_name); if (tid <= 0) continue;
        uint64_t ns = 0; int ok = 0;
        snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", tid);
        FILE *f = fopen(path, "re");
        if (f) { unsigned long long r; ok = fscanf(f, "%llu", &r) == 1; fclose(f); ns = ok ? r : 0; }
        if (!ok) {
            snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
            if ((f = fopen(path, "re"))) {
                size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); b[k] = 0;
                char *q = strrchr(b, ')'); unsigned long long ut, st;
                if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) { ns = (ut + st) * (1000000000ull / (uint64_t)hz); ok = 1; }
            }
        }
        if (ok) { s->e[s->n].tid = tid; s->e[s->n].ns = ns; s->n++; }
    }
    closedir(d);
    return s->n;
}
static int ts_period_ms(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_LONGFRAME_THREADS"); v = e ? atoi(e) : 250; if (v < 0) v = 0; } return v; }
static void ts_busiest(const tsnap *base, const tsnap *now, char *out, size_t n) {   // "busiest threads over <ms>: '<name>' <ms>, …" between two readings
    struct { int tid; uint64_t d; } top[3] = {{0}};
    for (int i = 0; i < now->n; i++) {
        uint64_t prev = 0; for (int j = 0; j < base->n; j++) if (base->e[j].tid == now->e[i].tid) { prev = base->e[j].ns; break; }   // a thread born since: all of its time
        uint64_t d = now->e[i].ns > prev ? now->e[i].ns - prev : 0;
        for (int k = 0; k < 3; k++) if (d > top[k].d) { for (int m = 2; m > k; m--) top[m] = top[m - 1]; top[k].tid = now->e[i].tid; top[k].d = d; break; }
    }
    size_t o = (size_t)snprintf(out, n, "busiest threads over %.0f ms:", (now->t - base->t) / 1e6);
    for (int k = 0; k < 3 && top[k].d && o < n; k++) {
        char path[64], nm[32] = "?"; snprintf(path, sizeof path, "/proc/self/task/%d/comm", top[k].tid);
        FILE *f = fopen(path, "re"); if (f) { if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0; fclose(f); }
        o += (size_t)snprintf(out + o, n - o, "%s '%s' %.0f", k ? "," : "", nm, top[k].d / 1e6);
    }
}
static void *ts_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "dm-lfthreads");
    tsnap *tmp = malloc(sizeof *tmp), *base = malloc(sizeof *base); if (!tmp || !base) return NULL;
    uint64_t next = 0;
    pthread_mutex_lock(&g_tsmu);
    for (;;) {
        int q = -1; for (int i = 0; i < TS_REQ; i++) if (g_tsq[i].state == 1) { q = i; break; }
        uint64_t now = ts_now();
        if (q < 0 && now < next) {   // nothing asked and not yet time for a reading: wait for either
            uint64_t dl = next - now; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(dl / 1000000000ull); ts.tv_nsec += (long)(dl % 1000000000ull); if (ts.tv_nsec >= 1000000000l) { ts.tv_sec++; ts.tv_nsec -= 1000000000l; }
            pthread_cond_timedwait(&g_tscv, &g_tsmu, &ts); continue;
        }
        uint64_t since = q >= 0 ? g_tsq[q].since : 0, fq = q >= 0 ? g_tsq[q].f : 0;
        pthread_mutex_unlock(&g_tsmu);
        int ok = ts_read(tmp);
        pthread_mutex_lock(&g_tsmu);
        if (q >= 0) {
            int got = 0;
            for (unsigned k = 0; k < TS_RING && k < g_tsn; k++) { const tsnap *s = &g_ts[(g_tsn - 1 - k) % TS_RING]; if (s->t <= since) { *base = *s; got = 1; break; } }
            g_tsq[q].s[0] = 0;
            char b[220] = "";
            if (got && ok) { pthread_mutex_unlock(&g_tsmu); ts_busiest(base, tmp, b, sizeof b); pthread_mutex_lock(&g_tsmu); }
            if (g_tsq[q].f == fq && g_tsq[q].state == 1) { memcpy(g_tsq[q].s, b, sizeof b); g_tsq[q].state = 2; }   // (else a newer frame took the slot meanwhile: asked again)
        }
        if (ok) { g_ts[g_tsn++ % TS_RING] = *tmp; next = ts_now() + (uint64_t)ts_period_ms() * 1000000ull; }
    }
    return NULL;
}
void mtl_frame_extra(uint64_t f, char *out, size_t n) {
    static uint64_t proom, psub, penc, psave, ppw, ppl, pplb, ppln, pgv, pwr, pwo, pt, psc; static int init;
    uint64_t room = 0, sub = 0, enc = vka_enc_waits(), save = 0, pw = 0, pl = 0, plb = 0, pln = 0, gv = 0, wr = 0, wo = 0, t = ts_now(), sc = vka_worker_sub_cpu(); vka_worker_waits(&room, &sub); vka_pso_waits(&save, &pw);
    vka_mem_waits(&pl, &plb, &pln, &gv, &wr, &wo);
    static int ts_on = -1; if (ts_on < 0) { ts_on = ts_period_ms() > 0; if (ts_on) { pthread_t th; if (pthread_create(&th, NULL, ts_main, NULL)) ts_on = 0; else pthread_detach(th); } }
    if (init && n) {
        snprintf(out, n, "game waited for the replay worker %.1f, replay recording %.1f (its CPU %.1f), replay waited for the encoder %.1f, cache save %.1f, waited for prewarm %.1f; "
                         "pool placement %.1f ms (%llu pools, %.1f MB), give-back %.1f ms, the pool table's lock waited for: replay %.1f, others %.1f",
                 (room - proom) / 1e6, (sub - psub) / 1e6, (sc - psc) / 1e6, (enc - penc) / 1e6, (save - psave) / 1e6, (pw - ppw) / 1e6,
                 (pl - ppl) / 1e6, (unsigned long long)(pln - ppln), (plb - pplb) / 1048576.0, (gv - pgv) / 1e6, (wr - pwr) / 1e6, (wo - pwo) / 1e6);
        if (ts_on && t - pt >= 100000000ull) {   // a long frame: the sampler names its busiest threads (read back at print time, mtl_gpu_extra)
            pthread_mutex_lock(&g_tsmu);
            int q = (int)(f % TS_REQ); g_tsq[q].f = f; g_tsq[q].since = pt; g_tsq[q].state = 1; g_tsq[q].s[0] = 0;
            pthread_cond_signal(&g_tscv);
            pthread_mutex_unlock(&g_tsmu);
        }
    } else if (n) out[0] = 0;
    proom = room; psub = sub; penc = enc; psave = save; ppw = pw; ppl = pl; pplb = plb; ppln = pln; pgv = gv; pwr = wr; pwo = wo; pt = t; psc = sc; init = 1;
}
void mtl_gpu_extra(uint64_t f, char *out, size_t n) {
    double s = vka_gpu_span_ms(f), b = mtl_gpu_ms(f);
    if (!n) return;
    size_t o = 0; out[0] = 0;
    if (s >= 0 && b >= 0) o = (size_t)snprintf(out, n, " (busy; span %.1f ms, %.1f waiting for the recording)", s, s > b ? s - b : 0.0);
    pthread_mutex_lock(&g_tsmu);
    int q = (int)(f % TS_REQ);
    if (g_tsq[q].f == f && g_tsq[q].state == 2 && g_tsq[q].s[0] && o < n) snprintf(out + o, n - o, "; %s", g_tsq[q].s);
    else if (g_tsq[q].f == f && g_tsq[q].state == 1 && o < n) snprintf(out + o, n - o, "; busiest threads: not read yet");
    pthread_mutex_unlock(&g_tsmu);
}
#endif
