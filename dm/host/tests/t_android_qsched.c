// NVN queue dependencies on the Android replay (dm/gfx/vk_android/vka_qsched.h): a GPU wait parks only its own queue until its producer has been replayed, with a stand-in
// replay that walks command lists as vka_exec.c does (calls, a cut at a wait, the continuation). The ideas of upstream's t_wait_event / t_render_enable_queue (ea0c98df) on the
// scheduler: Ocarina of Time's consumer on queue 5 submitted before its producer on queue 1, the SDK's predicates, syncs never fenced, the timeline's signals, the effects'
// order, a continuation inside a called list, and the safety valves
#include "../../gfx/vk_android/vka_qsched.h"
#include <stdio.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Records: {op, size} + payload, as nvn_obj.h's
enum { T_END, T_WORK, T_SIGNAL, T_WAIT, T_WSYNC, T_CALL };
typedef struct { uint32_t op, size; } hdr;
typedef struct { uint8_t b[4096]; size_t n; } list_t;
static uint8_t *put(list_t *l, uint32_t op, const void *pl, uint32_t n) {
    uint8_t *r = l->b + l->n; hdr h = {op, 8 + ((n + 7) & ~7u)}; memcpy(r, &h, 8); if (n) memcpy(r + 8, pl, n); l->n += h.size; return r;
}
static void work(list_t *l, uint32_t id) { put(l, T_WORK, &id, 4); }
static void ev(list_t *l, uint32_t op, volatile uint32_t *w, uint32_t mode, uint32_t v) { struct { volatile uint32_t *w; uint32_t m, v; } p = {w, mode, v}; put(l, op, &p, sizeof p); }
static const uint8_t *wsync(list_t *l) { return put(l, T_WSYNC, NULL, 0); }
static void call(list_t *l, const list_t *t) { const uint8_t *p = t->b; put(l, T_CALL, &p, 8); }
static void end(list_t *l) { put(l, T_END, NULL, 0); }

static qs sc;
static uint64_t g_done, g_now, g_want;
static uint32_t g_order[256]; static int g_norder;                       // the work the stand-in replay recorded, in GPU order
static struct { uint64_t fence, signal; int final; } g_commit[64]; static int g_ncommit;
static uint32_t g_effects[256]; static int g_neffects;                   // signals' values in the order they are posted with a signal
static uint32_t g_pend[64][16]; static uint64_t g_pend_f[64]; static int g_pend_n[64], g_npend;   // effects of unsignalled commits (vka_exec.c carry)
static int g_timeouts;
static const uint8_t *g_xret[QS_NRET]; static int g_nxret;
static uint32_t g_cur_eff[16]; static int g_ncur_eff;

static int t_exec(const uint8_t *p, int depth, qs_cont *out) {
    for (;;) {
        hdr h; memcpy(&h, p, 8); const uint8_t *a = p + 8;
        switch (h.op) {
        case T_END: return 0;
        case T_WORK: { uint32_t id; memcpy(&id, a, 4); g_order[g_norder++] = id; break; }
        case T_SIGNAL: { struct { volatile uint32_t *w; uint32_t m, v; } e; memcpy(&e, a, sizeof e); g_cur_eff[g_ncur_eff++] = e.v; qs_event_replayed(&sc, e.w, e.m, e.v); break; }
        case T_WAIT: { struct { volatile uint32_t *w; uint32_t m, v; } e; memcpy(&e, a, sizeof e);
            if (!qs_wait_event(&sc, e.w, e.m, e.v)) { out->p = p + h.size; out->nret = g_nxret; memcpy(out->ret, g_xret, sizeof(g_xret[0]) * (size_t)g_nxret); return 1; } break; }
        case T_WSYNC: if (!qs_wait_sync(&sc, p)) { out->p = p + h.size; out->nret = g_nxret; memcpy(out->ret, g_xret, sizeof(g_xret[0]) * (size_t)g_nxret); return 1; } break;
        case T_CALL: { const uint8_t *t; memcpy(&t, a, 8); g_xret[g_nxret++] = p + h.size; int r = t_exec(t, depth + 1, out); g_nxret--; if (r) return 1; break; }
        }
        p += h.size;
    }
}
static int t_resume(const qs_cont *c, qs_cont *out) {
    qs_cont k = *c; g_nxret = k.nret; memcpy(g_xret, k.ret, sizeof(g_xret[0]) * (size_t)k.nret);
    if (t_exec(k.p, k.nret, out)) return 1;
    for (int i = k.nret - 1; i >= 0; i--) { g_nxret = i; if (t_exec(k.ret[i], i, out)) return 1; }
    g_nxret = 0; return 0;
}
// As vka_exec.c sub_commit's carry: effects of unsignalled commits go with the first signal that covers their fence, in recorded order
static void t_post(uint64_t fence, uint64_t signal) {
    if (!signal) { g_pend_f[g_npend] = fence; g_pend_n[g_npend] = g_ncur_eff; memcpy(g_pend[g_npend++], g_cur_eff, sizeof g_cur_eff); g_ncur_eff = 0; return; }
    int k = 0;
    for (int i = 0; i < g_npend; i++) {
        if (g_pend_f[i] <= signal) { for (int e = 0; e < g_pend_n[i]; e++) g_effects[g_neffects++] = g_pend[i][e]; }
        else { g_pend_f[k] = g_pend_f[i]; g_pend_n[k] = g_pend_n[i]; memcpy(g_pend[k], g_pend[i], sizeof g_pend[i]); k++; }
    }
    g_npend = k;
    for (int e = 0; e < g_ncur_eff; e++) g_effects[g_neffects++] = g_cur_eff[e];
    g_ncur_eff = 0;
}
static int t_run(void *ctx, qs_sub *s) {
    (void)ctx; int parked = 0;
    while (s->hi < s->nh) {
        int r = s->resumed ? t_resume(&s->cont, &s->cont) : (g_nxret = 0, t_exec(s->h[s->hi], 0, &s->cont));
        if (r) { s->resumed = 1; parked = 1; break; }
        s->resumed = 0; s->hi++;
    }
    uint64_t sig = qs_commit(&sc, s->fence, !parked);
    g_commit[g_ncommit].fence = s->fence; g_commit[g_ncommit].signal = sig; g_commit[g_ncommit++].final = !parked;
    t_post(s->fence, sig);
    return parked;
}
static uint64_t t_done(void *c) { (void)c; return g_done; }
static uint64_t t_now(void *c) { (void)c; return g_now; }
static void t_timeout(void *c, const qs_sub *s) { (void)c; (void)s; g_timeouts++; }
static void reset(void) {
    qs_ev_free(&sc); memset(&sc, 0, sizeof sc);
    sc.run = t_run; sc.done = t_done; sc.now = t_now; sc.timeout = t_timeout; sc.park_ns = 200000000;
    g_done = g_now = g_want = 0; g_norder = g_ncommit = g_neffects = g_npend = g_timeouts = g_ncur_eff = 0;
}
static uint64_t g_fence;
static void submit(uint64_t serial, int n, const list_t *const *l, qs_wsnap *ws, int nws) {
    qs_sub *s = calloc(1, sizeof *s); s->serial = serial; s->fence = ++g_fence; s->nh = n;
    const uint8_t **h = malloc(sizeof *h * (size_t)n); for (int i = 0; i < n; i++) h[i] = l[i]->b; s->h = h;
    if (nws) { s->ws = malloc(sizeof *ws * (size_t)nws); memcpy(s->ws, ws, sizeof *ws * (size_t)nws); s->nws = nws; }
    qs_submit(&sc, s); qs_poll(&sc);
}
static void submit1(uint64_t serial, const list_t *l) { submit(serial, 1, &l, NULL, 0); }
static int order_is(const uint32_t *want, int n) { if (g_norder != n) return 0; for (int i = 0; i < n; i++) if (g_order[i] != want[i]) return 0; return 1; }
static void print_order(void) { printf("    order:"); for (int i = 0; i < g_norder; i++) printf(" %u", g_order[i]); printf("\n"); }

static void test_pred(void) {
    CHECK(qs_pred(5, 0, 5) && !qs_pred(6, 0, 5) && !qs_pred(4, 0, 5), "EQUAL is exact equality");
    CHECK(qs_pred(5, 1, 5) && qs_pred(6, 1, 5) && !qs_pred(4, 1, 5), "GEQUAL_WRAP: at or past the target");
    CHECK(qs_pred(1, 1, 0xffffffffu) && !qs_pred(0xffffffffu, 1, 1), "GEQUAL_WRAP wraps: (uint32_t)(current - target) < 0x80000000");
    CHECK(qs_pred(0x7fffffffu, 1, 0) && !qs_pred(0x80000000u, 1, 0), "GEQUAL_WRAP's half range");
}
// Ocarina of Time: the ambient-cache consumer (queue 5) is submitted before the SH lighting producer (queue 1) and waits for its event
static void test_wait_event(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 0;
    list_t c = {0}, p = {0};
    work(&c, 50); ev(&c, T_WAIT, &e, 0, 1); work(&c, 51); end(&c);
    work(&p, 10); ev(&p, T_SIGNAL, &e, 0, 1); end(&p);
    submit1(5, &c);
    CHECK(g_norder == 1 && qs_parked(&sc) && sc.n_park == 1, "the consumer replays up to its wait and parks (%d records)", g_norder);
    CHECK(qs_park_min(&sc) == 1, "the park holds fence 1 back (%llu)", (unsigned long long)qs_park_min(&sc));
    submit1(1, &p);
    static const uint32_t want[] = {50, 10, 51};
    CHECK(order_is(want, 3), "the consumer's rest runs after the producer"); if (!order_is(want, 3)) print_order();
    CHECK(!qs_parked(&sc) && sc.n_release == 1, "released by its producer");
    CHECK(e == 0, "the producer's signal is not written by the replay (its completion writes it)");
    // The timeline: the part before the wait and the producer go without a signal, the consumer's rest signals 2 (both fences)
    CHECK(g_ncommit == 3 && !g_commit[0].final && g_commit[0].signal == 0 && g_commit[1].fence == 2 && g_commit[1].signal == 0 && g_commit[2].fence == 1 && g_commit[2].signal == 2,
          "signals: %llu/%llu/%llu", (unsigned long long)g_commit[0].signal, (unsigned long long)g_commit[1].signal, (unsigned long long)g_commit[2].signal);
    CHECK(sc.lo == 2 && sc.sig == 2 && !sc.ndef && qs_park_min(&sc) == UINT64_MAX, "every fence signalled: lo %llu sig %llu def %d", (unsigned long long)sc.lo, (unsigned long long)sc.sig, sc.ndef);
    CHECK(g_neffects == 1 && g_effects[0] == 1 && !g_npend, "the producer's effect is posted with the signal that covers it");
    // A wait the word already satisfies passes; no park
    reset(); g_fence = 0; e = 7;
    list_t d = {0}; work(&d, 1); ev(&d, T_WAIT, &e, 1, 3); work(&d, 2); end(&d);
    submit1(5, &d);
    CHECK(g_norder == 2 && !sc.n_park && g_ncommit == 1 && g_commit[0].signal == 1, "a wait that holds passes (EVENT 7 >= 3)");
}
// A submission whose first wait comes before any signal of its own parks whole when the worker reaches it: nothing of it replayed, no cut, no extra commit
static void test_whole(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 0;
    list_t c = {0}, p = {0}, o = {0};
    work(&c, 50); ev(&c, T_WAIT, &e, 1, 1); work(&c, 51); end(&c);
    work(&o, 20); end(&o);
    work(&p, 10); ev(&p, T_SIGNAL, &e, 0, 1); end(&p);
    qs_sub *s = calloc(1, sizeof *s); s->serial = 5; s->fence = ++g_fence; s->nh = 1;
    const uint8_t **h = malloc(sizeof *h); h[0] = c.b; s->h = h;
    s->w0kind = 1; s->w0word = &e; s->w0mode = 1; s->w0value = 1;
    qs_submit(&sc, s); qs_poll(&sc);
    CHECK(!g_norder && !g_ncommit && qs_parked(&sc) && sc.n_whole == 1, "parked whole: nothing replayed, no commit (%d %d)", g_norder, g_ncommit);
    CHECK(qs_park_min(&sc) == 1, "the whole park holds fence 1 back");
    submit1(1, &o); submit1(1, &p);
    static const uint32_t want[] = {20, 10, 50, 51};
    CHECK(order_is(want, 4), "the whole consumer after its producer"); if (!order_is(want, 4)) print_order();
    CHECK(g_ncommit == 3 && g_commit[0].signal == 0 && g_commit[1].signal == 0 && g_commit[2].fence == 1 && g_commit[2].final && g_commit[2].signal == 3, "one commit each; the consumer signals 3 (%llu)",
          (unsigned long long)g_commit[2].signal);
    CHECK(g_neffects == 1 && !g_npend && !sc.ndef, "effects posted, nothing left unsignalled");
    // Its first wait already holds: replayed at once
    reset(); g_fence = 0; e = 1;
    s = calloc(1, sizeof *s); s->serial = 5; s->fence = ++g_fence; s->nh = 1; h = malloc(sizeof *h); h[0] = c.b; s->h = h;
    s->w0kind = 1; s->w0word = &e; s->w0mode = 1; s->w0value = 1;
    qs_submit(&sc, s);
    CHECK(g_norder == 2 && !sc.n_park && g_commit[0].signal == 1, "a first wait that holds: no park");
}
// The value a wait sees: a replayed signal counts at once (WRITE: its value; ADD: added to the word as the replay leaves it); once its fence is done, the word in memory
static void test_queued_values(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 5;
    list_t a = {0}, b = {0}, w = {0};
    ev(&a, T_SIGNAL, &e, 1, 1); end(&a);   // ADD 1 -> 6
    ev(&b, T_SIGNAL, &e, 1, 2); end(&b);   // ADD 2 -> 8
    work(&w, 1); ev(&w, T_WAIT, &e, 0, 8); work(&w, 2); end(&w);
    submit1(1, &a); submit1(1, &b);
    CHECK(qs_ev_value(&sc, &e) == 8 && e == 5, "queued ADDs: %u (memory %u)", qs_ev_value(&sc, &e), e);
    submit1(5, &w);
    CHECK(g_norder == 2 && !sc.n_park, "a wait on queued signals goes on at encode, after them on the one queue");
    g_done = 2; e = 8;
    CHECK(qs_ev_value(&sc, &e) == 8, "done: memory");
    e = 3; CHECK(qs_ev_value(&sc, &e) == 3, "done: the word as memory has it (a CPU store) %u", qs_ev_value(&sc, &e));
    // A CPU store releases a park (polled)
    reset(); g_fence = 0; e = 0;
    list_t c = {0}; work(&c, 1); ev(&c, T_WAIT, &e, 1, 4); work(&c, 2); end(&c);
    submit1(5, &c);
    CHECK(qs_parked(&sc), "parked on a word nothing replayed writes");
    qs_poll(&sc); CHECK(qs_parked(&sc), "still parked");
    e = 4; qs_poll(&sc);
    CHECK(!qs_parked(&sc) && g_norder == 2 && sc.n_release == 1, "a store through mapped memory releases it");
}
// Later submissions of a parked queue wait behind it, other queues go on; fences are signalled once everything below is handed over
static void test_backlog(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 0;
    list_t c1 = {0}, c2 = {0}, o = {0}, p = {0};
    work(&c1, 50); ev(&c1, T_WAIT, &e, 0, 1); work(&c1, 51); end(&c1);
    work(&c2, 52); end(&c2);
    work(&o, 20); end(&o);
    work(&p, 10); ev(&p, T_SIGNAL, &e, 0, 1); end(&p);
    submit1(5, &c1);   // 1 parks
    submit1(1, &o);    // 2 runs, unsignalled
    submit1(5, &c2);   // 3 waits behind 1
    CHECK(sc.q[0].n == 2 || sc.q[1].n == 2, "the queue's second submission waits behind its park");
    submit1(1, &p);    // 4 releases 1, then 3
    static const uint32_t want[] = {50, 20, 10, 51, 52};
    CHECK(order_is(want, 5), "order"); if (!order_is(want, 5)) print_order();
    // commits: 1 part (0), 2 (0), 4 (0), 1 rest (2: fences 1 and 2), 3 (4: fences 3 and 4)
    CHECK(g_ncommit == 5 && g_commit[3].fence == 1 && g_commit[3].signal == 2 && g_commit[4].fence == 3 && g_commit[4].signal == 4, "signals %llu %llu",
          (unsigned long long)g_commit[3].signal, (unsigned long long)g_commit[4].signal);
    CHECK(!sc.ndef && sc.sig == 4, "all signalled");
}
// A wait inside a called list: its continuation is the rest of that list, then the caller's
static void test_nested(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 0;
    list_t inner = {0}, outer = {0}, p = {0};
    work(&inner, 2); ev(&inner, T_WAIT, &e, 0, 9); work(&inner, 3); end(&inner);
    work(&outer, 1); call(&outer, &inner); work(&outer, 4); call(&outer, &inner); work(&outer, 5); end(&outer);
    ev(&p, T_SIGNAL, &e, 0, 9); work(&p, 100); end(&p);
    const list_t *two[2] = {&outer, &outer};
    submit(5, 2, two, NULL, 0);
    CHECK(g_norder == 2, "parked inside the call");
    submit1(1, &p);
    static const uint32_t want[] = {1, 2, 100, 3, 4, 2, 3, 5, 1, 2, 3, 4, 2, 3, 5};
    CHECK(order_is(want, 15), "the inner list's rest, the caller's rest, then the next handle"); if (!order_is(want, 15)) print_order();
}
// Sync waits: a sync never fenced when submitted passes at once; fenced by a submission still parked, the waiter parks until it has been handed over
static void test_wait_sync(void) {
    reset(); g_fence = 0;
    list_t a = {0}; work(&a, 1); const uint8_t *r = wsync(&a); work(&a, 2); end(&a);
    qs_wsnap none = {r, 0};
    const list_t *l = &a; submit(3, 1, &l, &none, 1);
    CHECK(g_norder == 2 && !sc.n_park, "a sync not fenced at submission passes (Sparking! ZERO)");
    reset(); g_fence = 0;
    volatile uint32_t e = 0;
    list_t c = {0}, w = {0}, p = {0};
    work(&c, 50); ev(&c, T_WAIT, &e, 0, 1); work(&c, 51); end(&c);
    work(&w, 60); const uint8_t *rw = wsync(&w); work(&w, 61); end(&w);
    work(&p, 10); ev(&p, T_SIGNAL, &e, 0, 1); end(&p);
    submit1(5, &c);                            // fence 1 parks
    qs_wsnap s1 = {rw, 1}; l = &w; submit(7, 1, &l, &s1, 1);   // fence 2 waits for fence 1's submission
    CHECK(g_norder == 2 && sc.n_park == 2, "a wait on a parked fence parks too");
    submit1(1, &p);                            // fence 3: releases 1, then 2
    static const uint32_t want[] = {50, 60, 10, 51, 61};
    CHECK(order_is(want, 5), "order"); if (!order_is(want, 5)) print_order();
    CHECK(!qs_parked(&sc) && sc.sig == 3, "all signalled (%llu)", (unsigned long long)sc.sig);
    // A queue-level wait (nvnQueueWaitSync) on a parked fence holds the queue's later submissions
    reset(); g_fence = 0; e = 0;
    list_t later = {0}; work(&later, 70); end(&later);
    submit1(5, &c);   // 1 parks
    qs_sub *qw = calloc(1, sizeof *qw); qw->serial = 7; qw->wait_fence = 1; qs_submit(&sc, qw);
    submit1(7, &later);   // 2: behind the queue-level wait
    CHECK(g_norder == 1, "queue 7 waits for fence 1 (%d)", g_norder);
    submit1(1, &p);   // 3
    static const uint32_t want2[] = {50, 10, 51, 70};
    CHECK(order_is(want2, 4), "order"); if (!order_is(want2, 4)) print_order();
}
// Safety valves: a release of everything (a present, a CPU wait) in fence order with waits passed; QS_DEFER_MAX unsignalled commits; the park time; a CPU wait already on
static void test_valves(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 0, f = 0;
    list_t c = {0}, d = {0}, o = {0}, x = {0};
    work(&c, 50); ev(&c, T_WAIT, &e, 0, 1); work(&c, 51); end(&c);
    work(&d, 60); ev(&d, T_WAIT, &f, 0, 1); work(&d, 61); end(&d);
    work(&o, 20); end(&o);
    work(&x, 52); end(&x);
    submit1(5, &c); submit1(6, &d); submit1(1, &o); submit1(5, &x);
    int c0 = g_ncommit;
    qs_force_all(&sc, QS_WHY_PRESENT);
    static const uint32_t want[] = {50, 60, 20, 51, 61, 52};
    CHECK(order_is(want, 6), "released in submission order"); if (!order_is(want, 6)) print_order();
    CHECK(!qs_parked(&sc) && sc.sig == 4 && !sc.ndef && sc.n_force[QS_WHY_PRESENT] == 1, "all signalled: %llu", (unsigned long long)sc.sig);
    for (int i = c0; i < g_ncommit; i++) if (g_commit[i].final) CHECK(g_commit[i].signal >= g_commit[i].fence, "a released commit signals its fence (%llu: %llu)",
                                                                                           (unsigned long long)g_commit[i].fence, (unsigned long long)g_commit[i].signal);
    // QS_DEFER_MAX unsignalled commits release the park before the next one (the replay's command buffers are reused after their fences)
    reset(); g_fence = 0; e = 0;
    submit1(5, &c);
    for (int i = 0; i < QS_DEFER_MAX + 2; i++) submit1(1, &o);
    CHECK(sc.n_force[QS_WHY_SLOTS] == 1 && !qs_parked(&sc), "released for the command buffers (%llu)", (unsigned long long)sc.n_force[QS_WHY_SLOTS]);
    int maxdef = 0, run = 0; for (int i = 0; i < g_ncommit; i++) { run = g_commit[i].signal ? 0 : run + 1; if (run > maxdef) maxdef = run; }
    CHECK(maxdef <= QS_DEFER_MAX, "at most %d commits in a row without a signal (%d)", QS_DEFER_MAX, maxdef);
    // The park time
    reset(); g_fence = 0; e = 0;
    submit1(5, &c); g_now = 100000000; qs_poll(&sc); CHECK(qs_parked(&sc) && !g_timeouts, "within the park time");
    g_now = 300000000; qs_poll(&sc); CHECK(!qs_parked(&sc) && g_timeouts == 1 && sc.n_force[QS_WHY_TIMEOUT] == 1 && g_norder == 2, "past it: logged and released");
}
// Issue #98: queues 1 and 5 hand each other events within one frame (Ocarina of Time) while a CPU thread already waits for queue 1's fence. Each wait parks until its
// producer is replayed, whatever the CPU waits for (the scheduler has no CPU-wait pass; the worker releases parks only once it is stuck): the consumer's last part
// never runs before the producer's part that signals it
static void test_pingpong(void) {
    reset(); g_fence = 0;
    volatile uint32_t a = 0, b = 0, c2 = 0;
    list_t q5 = {0}, q1 = {0};
    work(&q5, 50); ev(&q5, T_WAIT, &a, 1, 1); work(&q5, 51); ev(&q5, T_SIGNAL, &b, 1, 1); ev(&q5, T_WAIT, &c2, 1, 1); work(&q5, 52); end(&q5);
    work(&q1, 10); ev(&q1, T_SIGNAL, &a, 1, 1); ev(&q1, T_WAIT, &b, 1, 1); work(&q1, 11); ev(&q1, T_SIGNAL, &c2, 1, 1); work(&q1, 12); end(&q1);
    submit1(5, &q5); submit1(1, &q1);
    static const uint32_t want[] = {50, 10, 51, 11, 12, 52};
    CHECK(order_is(want, 6), "each part after its producer"); if (!order_is(want, 6)) print_order();
    CHECK(!qs_parked(&sc) && sc.n_park == 3 && sc.n_release == 3, "three parks, each released by its producer (%llu %llu)", (unsigned long long)sc.n_park, (unsigned long long)sc.n_release);
}

// Waits for the CPU (gates): Ocarina of Time's main submission (queue 1) begins with a wait on an event its CPU sets after its SyncWait on the previous frame. Parked, its
// replay (and that of every queue waiting on it) began only at the CPU's signal; after two releases by memory the word is a CPU event and its waits are gates: the replay
// goes on and the stand-in GPU waits. A word a replayed signal released is not learned; a banned word parks again
static int g_ngates, g_gate_ok = 1; static uint32_t g_gate_at[16];
static int t_gate(void *c, qs_sub *s, volatile uint32_t *w, uint32_t mode, uint32_t value) { (void)c; (void)s; (void)w; (void)mode; (void)value; if (!g_gate_ok) return 0; g_gate_at[g_ngates++] = (uint32_t)g_norder; return 1; }
static void test_gate(void) {
    reset(); g_fence = 0; sc.gate = t_gate; g_ngates = 0; g_gate_ok = 1;
    volatile uint32_t cpu = 0, sh = 0;
    list_t m = {0}, k = {0};
    // the main submission: its CPU gate first, then work that signals the SH event; the consumer (queue 5) waits for the SH event
    ev(&m, T_WAIT, &cpu, 0, 1); work(&m, 10); ev(&m, T_SIGNAL, &sh, 1, 1); work(&m, 11); end(&m);
    for (int frame = 0; frame < 2; frame++) {
        cpu = 0;
        list_t *kk = &k; k.n = 0; work(kk, 50); ev(kk, T_WAIT, &sh, 1, (uint32_t)frame + 1); work(kk, 51); end(kk);
        qs_sub *s = calloc(1, sizeof *s); s->serial = 5; s->fence = ++g_fence; s->nh = 1; const uint8_t **h = malloc(sizeof *h); h[0] = k.b; s->h = h; qs_submit(&sc, s);
        submit1(1, &m);
        CHECK(qs_parked(&sc) && !g_ngates, "frame %d: the main submission parks at its CPU wait (not learned yet)", frame);
        cpu = 1; qs_poll(&sc);   // the CPU's signal (memory)
        CHECK(!qs_parked(&sc), "frame %d: released by the CPU's signal", frame);
        sh = qs_ev_value(&sc, &sh); g_done = g_fence;   // the frame completes: its signals land
    }
    CHECK(qs_gw_cpu(&sc, &cpu) && !qs_gw_cpu(&sc, &sh), "the CPU word is learned (%d), the SH word (released by a replayed signal) is not", sc.gw[0].cpu);
    // Frame 3: gated: replayed at once, the gate asked once (its first wait), the consumer goes on after the main work, nothing parks
    cpu = 0; g_norder = 0; uint64_t parks = sc.n_park;
    k.n = 0; work(&k, 50); ev(&k, T_WAIT, &sh, 1, 3); work(&k, 51); end(&k);
    qs_sub *s = calloc(1, sizeof *s); s->serial = 5; s->fence = ++g_fence; s->nh = 1; const uint8_t **h = malloc(sizeof *h); h[0] = k.b; s->h = h; qs_submit(&sc, s);
    s = calloc(1, sizeof *s); s->serial = 1; s->fence = ++g_fence; s->nh = 1; h = malloc(sizeof *h); h[0] = m.b; s->h = h;
    s->w0kind = 1; s->w0word = &cpu; s->w0mode = 0; s->w0value = 1;
    qs_submit(&sc, s); qs_poll(&sc);
    static const uint32_t want[] = {50, 10, 11, 51};
    CHECK(order_is(want, 4), "gated: the main work and the consumer's rest replayed before the CPU's signal"); if (!order_is(want, 4)) print_order();
    CHECK(g_ngates == 1 && g_gate_at[0] == 1 && sc.n_gate == 1 && sc.n_park == parks + 1 && !qs_parked(&sc), "one gate, before the main work (gates %d at %u; parks %llu)",
          g_ngates, g_gate_at[0], (unsigned long long)(sc.n_park - parks));
    // Banned (the gate could not be made): it parks as before
    g_gate_ok = 0; cpu = 0; g_norder = 0; g_done = g_fence;
    s = calloc(1, sizeof *s); s->serial = 1; s->fence = ++g_fence; s->nh = 1; h = malloc(sizeof *h); h[0] = m.b; s->h = h;
    s->w0kind = 1; s->w0word = &cpu; s->w0mode = 0; s->w0value = 1;
    qs_submit(&sc, s);
    CHECK(qs_parked(&sc) && !g_norder, "no gate: parked whole");
    cpu = 1; qs_poll(&sc); CHECK(!qs_parked(&sc) && g_norder == 2, "released by the CPU's signal");
    qs_gate_ban(&sc, &cpu); CHECK(!qs_gw_cpu(&sc, &cpu), "a banned word is not a CPU event");
}

// Many words in flight (more than the 256 the table held before): every one is seen by a wait, none falls back to memory
static void test_many_words(void) {
    reset(); g_fence = 0;
    enum { N = 3000 };
    static volatile uint32_t w[N]; static list_t c;
    memset((void *)w, 0, sizeof w); memset(&c, 0, sizeof c);
    qs_sub prod = {0}; prod.fence = ++g_fence; sc.cur = &prod;   // one producer submission replays N signals (WRITE 1), not done yet
    for (int i = 0; i < N; i++) qs_event_replayed(&sc, &w[i], 0, 1);
    sc.cur = NULL;
    int seen = 1; for (int i = 0; i < N; i++) seen &= qs_ev_value(&sc, &w[i]) == 1;
    CHECK(seen && w[0] == 0 && sc.evcap >= 4 * N / 2, "%d words signalled and not done: the replay sees each one's value (table %u)", N, sc.evcap);
    work(&c, 1); ev(&c, T_WAIT, &w[N - 1], 0, 1); ev(&c, T_WAIT, &w[0], 0, 1); work(&c, 2); end(&c);
    submit1(5, &c);
    CHECK(g_norder == 2 && !sc.n_park, "a wait on the 3000th word goes on at once");
    CHECK(sc.evpeak == N, "the stats line's words in flight: %u at the commit (it read 0 when only a rebuild sampled it)", sc.evpeak);
    g_done = g_fence; w[5] = 7;
    CHECK(qs_ev_value(&sc, &w[5]) == 7, "done: the word in memory");
    qs_sub next = {0}; next.fence = ++g_fence; sc.cur = &next;   // signals after they are done: an ADD on memory's value; the done words go at the next rebuild
    qs_event_replayed(&sc, &w[1], 1, 2);
    for (int i = 0; i < N; i++) qs_event_replayed(&sc, &w[i], 0, 3);
    sc.cur = NULL;
    CHECK(qs_ev_value(&sc, &w[1]) == 3 && qs_ev_value(&sc, &w[N - 1]) == 3 && sc.nev <= N + 1, "after they are done: the new values, %u words kept", sc.nev);
    reset();
}
// Waits that hold when first asked, by whom (the stats line, DM_VK_QS_TRACE): a signal queued this frame, one queued in an earlier frame (a double-buffered event word
// whose last use is still on the GPU: Mario Kart World's 0x...7000 / 0x...7100 pairs alternate frames), or memory. Each wait counts once, also after its park
static void submit_fr(uint64_t serial, uint64_t frame, const list_t *l) {
    qs_sub *s = calloc(1, sizeof *s); s->serial = serial; s->fence = ++g_fence; s->frame = frame; s->nh = 1;
    const uint8_t **h = malloc(sizeof *h); h[0] = l->b; s->h = h;
    qs_submit(&sc, s); qs_poll(&sc);
}
static void test_pass_sources(void) {
    reset(); g_fence = 0;
    volatile uint32_t e = 0, m = 0;
    list_t p1 = {0}, c3 = {0}, p3 = {0}, c3b = {0}, cm = {0}, cp = {0}, pp = {0};
    ev(&p1, T_SIGNAL, &e, 1, 5); work(&p1, 1); end(&p1);                 // frame 1: the producer leaves 5 (ADD), not done on the GPU
    submit_fr(1, 1, &p1);
    work(&c3, 30); ev(&c3, T_WAIT, &e, 1, 1); work(&c3, 31); end(&c3);    // frame 3 waits for >= 1 before its own producer: frame 1's queued 5 holds it
    submit_fr(6, 3, &c3);
    CHECK(sc.n_pass_old == 1 && !sc.n_pass_queued && !sc.n_pass_mem && !sc.n_park, "a wait held by an earlier frame's queued signal (old %llu queued %llu memory %llu parks %llu)",
          (unsigned long long)sc.n_pass_old, (unsigned long long)sc.n_pass_queued, (unsigned long long)sc.n_pass_mem, (unsigned long long)sc.n_park);
    ev(&p3, T_SIGNAL, &e, 1, 2); end(&p3); submit_fr(1, 3, &p3);           // this frame's signal, then a wait on it
    ev(&c3b, T_WAIT, &e, 1, 7); work(&c3b, 32); end(&c3b); submit_fr(6, 3, &c3b);
    CHECK(sc.n_pass_queued == 1, "a wait held by this frame's queued signal (%llu)", (unsigned long long)sc.n_pass_queued);
    g_done = g_fence; m = 1;                                                // all done: the word in memory
    ev(&cm, T_WAIT, &m, 0, 1); work(&cm, 33); end(&cm); submit_fr(5, 4, &cm);
    CHECK(sc.n_pass_mem == 1, "a wait held by memory (%llu)", (unsigned long long)sc.n_pass_mem);
    uint64_t n0 = sc.n_pass_old + sc.n_pass_queued + sc.n_pass_mem;        // parked, then released: asked again, not counted
    volatile uint32_t f = 0;
    ev(&cp, T_WAIT, &f, 0, 1); work(&cp, 34); end(&cp); submit_fr(5, 5, &cp);
    ev(&pp, T_SIGNAL, &f, 0, 1); end(&pp); submit_fr(1, 5, &pp);
    CHECK(sc.n_park == 1 && sc.n_release == 1 && sc.n_pass_old + sc.n_pass_queued + sc.n_pass_mem == n0, "a park and its release are not a wait held at once");
    reset();
}
int main(void) {
    test_many_words(); test_pred(); test_wait_event(); test_whole(); test_queued_values(); test_backlog(); test_nested(); test_wait_sync(); test_valves(); test_pingpong(); test_gate(); test_pass_sources();
    if (g_fail) { printf("t_android_qsched: %d FAILED\n", g_fail); return 1; }
    printf("PASS: GPU waits across NVN queues (vka_qsched.h): parks, producers, predicates, syncs, signals, valves, gates\n");
    return 0;
}
