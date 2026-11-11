// Exercise production build gates and frame-limit writes against sparse synthetic module memory; Unreal's rows come from the console variables the rewriter names in main's fixups.
// A framework step count row of the tests' own (hle_fpslock.c fps_steps rows; the profile's are DM_GAME_FPS_STEPS): the framework at [[main+0x100000]+8], mode +0x210, count +0x214
#define DM_FPS_STEPS_ROWS {{"5eb5eb5eb5eb5eb5eb5eb5eb5eb5eb5e", 0x100000, 0x210, 0x214, 1, {0x8}}}
#define DM_FPS_NSTEPS_ROWS 1
#include "../hle_fpslock.c"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static module_t test_modules[1];
module_t *g_mod = test_modules;
int g_nmod;
static int writes;
void dm_guest_write_prepare(void *p, size_t n) { (void)p; assert(n == 4 || n == 8); writes++; }
static void (*g_sead_gate)(uint64_t *, int);
void gate_obs_set(int id, void (*fn)(uint64_t *, int)) { if (id == OBS_SEAD_RATE) g_sead_gate = fn; }
int gate_obs_present(int id) { (void)id; return 0; }

// adrp / add of a module offset into register r, at word i of the text (module offsets: the text at 0)
static void cvar(const char *name, uint64_t off) { strcpy(g_mod[0].ue_cvar[g_mod[0].n_ue_cvar].name, name); g_mod[0].ue_cvar[g_mod[0].n_ue_cvar++].off = off; }

static void adrp_add(uint32_t *t, uint32_t i, int r, uint64_t target) {
    int64_t imm = (int64_t)((target & ~0xfffull) - ((i * 4ull) & ~0xfffull)) >> 12;
    t[i] = 0x90000000u | (uint32_t)(imm & 3) << 29 | (uint32_t)((imm >> 2) & 0x7ffff) << 5 | (uint32_t)r;
    t[i + 1] = 0x91000000u | (uint32_t)(target & 0xfff) << 10 | (uint32_t)r << 5 | (uint32_t)r;
}
// A Frostbite main in miniature: the class name and field names in read-only data, the class record and its field list, the static constructor's registration of the
// type info and a reader's settings lookup in the text, the instance cache word and the instance
static uint8_t *frostbite(uint8_t *base, uint8_t *inst) {
    g_mod[0].text_end = 0x1000; g_mod[0].ro_off = 0x2000; g_mod[0].size = 0x13000000;
    uint64_t b = (uint64_t)(uintptr_t)base, rec = 0x3008, fields = 0x3100, ti = 0x40000, cache = 0x41000;
    static const char names[] = "\0SimulationTimeSettingsX\0SimulationTimeSettings\0ForceSimRate\0MaxSimFps";
    memcpy(base + 0x2000, names, sizeof names);
    *(uint64_t *)(base + 0x3000) = b + 0x2001;   // a record of another class whose name starts the same
    *(uint64_t *)(base + rec) = b + 0x2019; *(uint64_t *)(base + rec + 0x40) = b + fields;
    *(uint64_t *)(base + fields) = b + 0x203d; *(uint32_t *)(base + fields + 0x10) = 0x34;
    *(uint64_t *)(base + fields + 0x20) = b + 0x2030; *(uint32_t *)(base + fields + 0x30) = 0x38;
    uint32_t *t = (uint32_t *)base;
    adrp_add(t, 100, 19, ti); adrp_add(t, 102, 8, rec); t[104] = 0xF9000000u | 19 << 5 | 8;   // str x8, [x19]
    adrp_add(t, 200, 2, 0x42000);   // another word in x2 well before the lookup
    adrp_add(t, 220, 1, ti); adrp_add(t, 222, 2, cache); t[224] = 0x94000000u;   // bl
    *(uint64_t *)(inst + 8) = b + ti; *(uint64_t *)(base + cache) = (uint64_t)(uintptr_t)inst;
    return inst;
}

static void check(int which) {
    size_t len = 0x27000000;   // up to the floor row's global (main+0x26cc208)
    uint8_t *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(base != MAP_FAILED);
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base;
    strcpy(g_mod[0].build_id, "94ceae325c205c4b9d6f7235552f28fd");
    setenv("DM_FPS_TARGET", which == 1 || which == 10 ? "30" : which == 2 || which == 11 ? "0" : "60", 1);
    int32_t *unity = (int32_t *)(base + 0x4afc868);
    *unity = which == 1 ? 60 : which == 4 ? -1 : which == 5 ? 0 : 30;
    uint8_t object[0x500] = {0};
    float *f = NULL; int32_t *sync = NULL;
    if (which == 3) g_mod[0].build_id[0] = '0';
    if (which == 6) {
        strcpy(g_mod[0].build_id, "c9a3dd7702075ecd90101f4555389582");
        *(uintptr_t *)(base + 0x1295b3f0) = (uintptr_t)object;
        f = (float *)(object + 0x434); *f = 30;
    } else if (which == 7) {
        strcpy(g_mod[0].build_id, "79e5950ffa85acf63e28c9aec051eac2");
        f = (float *)(base + 0x13c94dc); *f = 1000.0f / 30;
    } else if (which == 8) {   // Octopath Traveler 0: the frame limit and the present interval, and a variable the host does not write
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        cvar("r.Streaming.PoolSize", 0x7c71608); cvar(DM_GAME_UE_FPS_CVAR, 0x7c738d0); cvar("rhi.SyncInterval", 0x7b32690);
        *(uintptr_t *)(base + 0x7c738d0 + 0x10) = (uintptr_t)object;
        *(uintptr_t *)(base + 0x7b32690 + 0x10) = (uintptr_t)(object + 16);
        f = (float *)object; f[0] = f[1] = 30;
        sync = (int32_t *)(object + 16); sync[0] = sync[1] = 2;
    } else if (which == 14) {
        strcpy(g_mod[0].build_id, "a48c0de3bc353efa0e4d359bde05ef24");
        *(uintptr_t *)(base + 0xda20e38) = (uintptr_t)object;
        double *d = (double *)(object + 8); d[0] = d[1] = 30;
    } else if (which == 15 || which == 16) {
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        frostbite(base, object);
        *(float *)(object + 0x34) = 30; *(int32_t *)(object + 0x38) = 30;
        if (which == 16) *(uint64_t *)(object + 8) = 0;   // the cache word holds something else
    } else if (which >= 17 && which <= 19) {   // Donkey Kong Bananza's dynamic interval controller: [main+0x26cc208] app, [app] framework, [framework+0x218] controller, its floor at +0, its maximum at +4
        strcpy(g_mod[0].build_id, "e25c90c1c136638885659fd55bf8a03b");
        uint8_t *app = object + 0x100, *fw = object + 0x200, *ctl = object + 0x400;
        *(uintptr_t *)(base + 0x26cc208) = (uintptr_t)app; *(uintptr_t *)app = (uintptr_t)fw; *(uintptr_t *)(fw + 0x218) = which == 19 ? 0 : (uintptr_t)ctl;   // 19: the controller made later
        sync = (int32_t *)ctl; sync[0] = 1; sync[1] = 3;
    } else if (which >= 9) {   // the present interval alone (UE 4.18's nvn.SyncInterval); 12: a main the rewriter found no variable in
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        if (which != 12) cvar("nvn.SyncInterval", 0x8adca10);
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = which == 13 ? 0 : (uintptr_t)object;
        sync = (int32_t *)object; sync[0] = sync[1] = which == 10 ? 1 : 2;
    }
    if (which == 17) setenv("DM_FPS_TARGET", "30", 1);
    if (which == 18) setenv("DM_FPS_TARGET", "0", 1);
    if (which == 19) setenv("DM_FPS_TARGET", "15", 1);   // a saved cap the controller cannot hold (4 vsyncs, its maximum 3)
    int engine = which != 3 && which != 12 && which < 17;   // 17-19: a floor does not fix the rate, so presentation keeps the game's interval   // a limiter the host writes: any but another build's main (3) and a main the rewriter found no Unreal variable in (12)
    if (which & 1) assert(dm_fps_lock_engine() == engine);   // the graphics layer's present cap may ask before the first present (the acquire)
    dm_fps_lock_tick();
    assert(dm_fps_lock_engine() == engine);
    if (which == 0) {
        assert(*unity == 60 && writes == 1);
        dm_fps_lock_tick(); assert(writes == 1);
        *unity = 30; dm_fps_lock_tick(); assert(*unity == 60 && writes == 2);
    } else if (which == 1) assert(*unity == 30 && writes == 1);
    else if (which >= 2 && which <= 5) {
        assert(writes == 0);
        assert(*unity == (which == 4 ? -1 : which == 5 ? 0 : 30));
    } else if (which == 6) {
        assert(*f == 60 && writes == 1);
        assert(dm_fps_lock_rates() == 63);   // a rate the engine paces itself: any cap
        dm_fps_target_set(45, "test"); dm_fps_lock_tick(); assert(*f == 45 && writes == 2);   // set while the game runs
        dm_fps_target_set(0, "test"); dm_fps_lock_tick(); assert(*f == 30 && writes == 3);   // back to the game's own
    }
    else if (which == 7) assert(fabsf(*f - (1000.0f / 60 - 0.5f)) < 0.001f && writes == 1);
    else if (which == 8) assert(f[0] == 60 && f[1] == 60 && sync[0] == 1 && sync[1] == 1 && writes == 4);
    else if (which == 9) {
        assert(sync[0] == 1 && sync[1] == 1 && writes == 2);
        dm_fps_lock_tick(); assert(writes == 2);
        sync[0] = sync[1] = 2; dm_fps_lock_tick();
        assert(sync[0] == 1 && sync[1] == 1 && writes == 4);
    } else if (which == 10) assert(sync[0] == 2 && sync[1] == 2 && writes == 2);
    else if (which == 14) { double *d = (double *)(object + 8); assert(d[0] == 60 && d[1] == 60 && writes == 2); }
    else if (which == 15) {
        assert(*(float *)(object + 0x34) == 60 && *(int32_t *)(object + 0x38) == 60 && writes == 2);
        dm_fps_lock_tick(); assert(writes == 2);
        *(int32_t *)(object + 0x38) = 30; dm_fps_lock_tick(); assert(*(int32_t *)(object + 0x38) == 60 && writes == 3);
    } else if (which == 16) assert(*(float *)(object + 0x34) == 30 && *(int32_t *)(object + 0x38) == 30 && writes == 0);
    else if (which == 13) {
        assert(sync[0] == 2 && sync[1] == 2 && writes == 0);
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = (uintptr_t)object;
        dm_fps_lock_tick();
        assert(sync[0] == 1 && sync[1] == 1 && writes == 2);
    }
    else if (which == 17) {
        assert(sync[0] == 2 && sync[1] == 3 && writes == 1);   // the floor alone: the game may still go to 3
        assert(dm_fps_lock_rates() == (2 | 4 | 32));   // 20, 30, 60: whole vsyncs only, and no more than the controller's maximum of 3 (15 would need 4)
        dm_fps_target_set(40, "test"); dm_fps_lock_tick(); assert(sync[0] == 2 && writes == 1);   // 40 is no whole vsync: left alone
        dm_fps_target_set(20, "test"); dm_fps_lock_tick(); assert(sync[0] == 3 && writes == 2);
        dm_fps_target_set(0, "test"); dm_fps_lock_tick(); assert(sync[0] == 1 && writes == 3);   // back to the game's own floor
        dm_fps_lock_tick(); assert(writes == 3);
        dm_fps_target_set(33, "test"); assert(dm_fps_target() == 0);   // not a cap: unchanged
    } else if (which == 18) {
        assert(sync[0] == 1 && writes == 0);   // the game's own at start: nothing written, nothing to restore
        dm_fps_target_set(30, "test"); dm_fps_lock_tick(); assert(sync[0] == 2 && writes == 1);
        dm_fps_target_set(60, "test"); dm_fps_lock_tick(); assert(sync[0] == 1 && writes == 2);
    } else if (which == 19) {
        uint8_t *fw = object + 0x200;
        assert(writes == 0 && dm_fps_target() == 15);   // no controller yet: its maximum is unknown, the cap waits
        assert(dm_fps_lock_rates() == (1 | 2 | 4 | 32));   // offered by kind until the maximum can be read
        *(uintptr_t *)(fw + 0x218) = (uintptr_t)(object + 0x400);
        dm_fps_lock_tick(); assert(sync[0] == 1 && sync[1] == 3 && writes == 0 && dm_fps_target() == 0);   // the start cap becomes the game's own; nothing written
        assert(dm_fps_lock_rates() == (2 | 4 | 32));   // 15 is not offered
        dm_fps_lock_tick(); assert(sync[0] == 1 && writes == 0);
        dm_fps_target_set(15, "test"); dm_fps_lock_tick(); dm_fps_lock_tick(); assert(sync[0] == 1 && writes == 0);   // set while running (DM_CTL): still never above the maximum
        dm_fps_target_set(30, "test"); dm_fps_lock_tick(); assert(sync[0] == 2 && writes == 1);   // another cap still applies
        dm_fps_target_set(0, "test"); dm_fps_lock_tick(); assert(sync[0] == 1 && writes == 2);
    }
    else assert(sync[0] == 2 && sync[1] == 2 && writes == 0);
    munmap(base, len);
}

// sead's frame-rate controller gate (sead_rate): the rate the sdiv yields is the measured one, the steps add up to the time that passed, 10..60
static struct timespec at(double s) { struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; return t; }
static double run(double fps, int frames, int *lo, int *hi) {   // frames at a steady fps through the gate (dividend w8, divisor w21, controller x20): game seconds stepped
    uint64_t r[30] = {0}; double game = 0, t = 1000; *lo = 1000; *hi = 0;
    memset(&g_sr, 0, sizeof g_sr);
    for (int i = 0; i <= frames; i++, t += 1.0 / fps) {
        r[8] = 60; r[21] = i % 3 + 1; r[20] = 0x1234000;   // the controller's own 60 and an interval of 1 to 3
        atomic_fetch_add(&g_presents, 1);
        sead_rate_at(r, 8 | 21 << 5 | 20 << 10, at(t));
        int32_t q = (int32_t)r[8] / (int32_t)r[21];   // the sdiv
        if (!i) { assert(q == 60 / (int)r[21]); continue; }   // the first frame keeps the game's own
        assert(q * (int32_t)r[21] == (int32_t)r[8] && g_sr.ctl == 0x1234000);
        game += 1.0 / q; if (q < *lo) *lo = q; if (q > *hi) *hi = q;
    }
    return game;
}
static void sead_rate_check(void) {
    int lo, hi;
    for (double fps = 10; fps <= 60; fps += 0.7) {   // 10 to 60 at awkward rates: the game's time follows real time within one frame's step
        double game = run(fps, 600, &lo, &hi), wall = 600 / fps;
        assert(fabs(game - wall) < 1.0 / 60 + 1e-9 && lo >= 10 && hi <= 60 && hi - lo <= 1);
    }
    double g = run(20, 400, &lo, &hi); assert(lo == 20 && hi == 20 && fabs(g - 20) < 1e-6);   // a 20 fps limit: 20 every frame
    g = run(30, 300, &lo, &hi); assert(lo == 30 && hi == 30);
    g = run(5, 50, &lo, &hi); assert(lo == 10 && hi == 10 && fabs(g - 5) < 1e-6);   // below the floor: 10, the game at half speed (as on the Switch), no catching up
    g = run(120, 240, &lo, &hi); assert(lo == 60 && hi == 60);   // above the cap: 60
    uint64_t r[30] = {0}; r[8] = 60; r[21] = 0; memset(&g_sr, 0, sizeof g_sr);   // no interval (0): the game's own division, untouched
    sead_rate_at(r, 8 | 21 << 5 | 20 << 10, at(1)); atomic_fetch_add(&g_presents, 1); sead_rate_at(r, 8 | 21 << 5 | 20 << 10, at(1.05)); assert(r[8] == 60);
    r[21] = 1; atomic_fetch_add(&g_presents, 2); sead_rate_at(r, 8 | 21 << 5 | 20 << 10, at(1.15)); assert(r[8] == 20);   // two presents in 0.1 s since the last call: 20 fps, not 10
    assert(g_sead_gate == sead_rate);   // the callback the gate number reaches
    // On and off: DM_FPS_DECOUPLE=0 at start, then the in-game panel live (nxhost_fps_decouple_set); off leaves the dividend at 60
    const int P = 8 | 21 << 5 | 20 << 10;
    setenv("DM_FPS_DECOUPLE", "0", 1); assert(!sead_rate_on()); memset(&g_sr, 0, sizeof g_sr);
    r[20] = 0x1234000; r[8] = 60; r[21] = 2; atomic_fetch_add(&g_presents, 1); sead_rate_gate(r, P, at(2)); atomic_fetch_add(&g_presents, 1); sead_rate_gate(r, P, at(2.05)); assert(r[8] == 60);
    nxhost_fps_decouple_set(1); assert(sead_rate_on());
    atomic_fetch_add(&g_presents, 1); sead_rate_gate(r, P, at(2.10)); assert(r[8] == 60);   // the first frame on: a fresh measurement, the game's own
    atomic_fetch_add(&g_presents, 1); sead_rate_gate(r, P, at(2.15)); assert(r[8] == 40);   // 20 fps at interval 2: 40 / 2
    nxhost_fps_decouple_set(0); r[8] = 60; atomic_fetch_add(&g_presents, 1); sead_rate_gate(r, P, at(2.20)); assert(r[8] == 60 && !g_sr.last.tv_sec);
    unsetenv("DM_FPS_DECOUPLE");
}

// The frame limit on a main with a frame step (Ocarina of Time's profile: fps_target 60, fps_limit 30): the limit becomes the step's cap in whole vsyncs (dm_fps_step_fold)
static float step_page(const uint8_t *page) { float f; memcpy(&f, page, sizeof f); return f; }
static void step_check(void) {
    static uint8_t page[0x40]; dm_fps_state = page;
    setenv("DM_FPS_TARGET", "60", 1); setenv("DM_FPS_LIMIT", "30", 1);
    dm_fps_step_fold();
    assert(step_page(page) == 30 && dm_fps_limit_step() == 30 && nxhost_fps_limit_get() == 30);   // 2 vsyncs a frame; the limiter does not wait as well
    nxhost_fps_limit_set(45); assert(step_page(page) == 30 && dm_fps_limit_step() == 30 && nxhost_fps_limit_get() == 30);   // between whole-vsync rates: the one below
    nxhost_fps_limit_set(40); assert(step_page(page) == 30 && dm_fps_limit_step() == 30);
    nxhost_fps_limit_set(20); assert(step_page(page) == 30 && dm_fps_limit_step() == 20 && nxhost_fps_limit_get() == 20);   // the stubs see 30; the host holds 3 vsyncs
    nxhost_fps_limit_set(15); assert(step_page(page) == 60 && dm_fps_limit_step() == 0 && nxhost_fps_limit_get() == 15);   // below 20: the limiter waits, the step keeps the cap
    nxhost_fps_limit_set(0); assert(step_page(page) == 60 && dm_fps_limit_step() == 0 && nxhost_fps_limit_get() == 0);   // no limit: the cap
    dm_fps_target_set(30, "test"); assert(step_page(page) == 30);
    dm_fps_target_set(0, "test"); assert(step_page(page) == 0);   // the game's own logic
    nxhost_fps_limit_set(30); assert(step_page(page) == 30 && dm_fps_limit_step() == 30);   // the limit alone drives the step
    nxhost_fps_limit_set(60); assert(step_page(page) == 60 && dm_fps_limit_step() == 60);
    dm_fps_state = NULL; nxhost_fps_limit_set(30); assert(dm_fps_limit_step() == 0 && nxhost_fps_limit_get() == 30);   // no frame step: the limiter alone
}

// A framework's fixed steps per frame (steps_tick): the cap is the steps a frame through its mode, the game's own mode comes back, the frame limit folds in, presentation follows the game
static void steps_check(int which) {
    size_t len = 0x200000; uint8_t *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(base != MAP_FAILED);
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base; strcpy(g_mod[0].build_id, which == 2 ? "5eb5eb5eb5eb5eb5eb5eb5eb5eb5eb5f" : "5eb5eb5eb5eb5eb5eb5eb5eb5eb5eb5e");
    static uint8_t holder[0x10], fw[0x300]; int32_t *mode = (int32_t *)(fw + 0x210), *count = (int32_t *)(fw + 0x214);
    unsetenv("DM_FPS_LIMIT"); setenv("DM_FPS_TARGET", which == 1 ? "15" : "30", 1); writes = 0;
    if (which == 2) { dm_fps_lock_tick(); assert(dm_fps_lock_rates() == 0 && !dm_fps_lock_floor() && dm_fps_limit_step() == 0); return; }   // another build: no row
    dm_fps_lock_tick(); assert(writes == 0);   // the framework not made yet
    *(uintptr_t *)(base + 0x100000) = (uintptr_t)holder; *(uintptr_t *)(holder + 8) = (uintptr_t)fw; *mode = 0; *count = 1;
    assert(dm_fps_lock_rates() == (2 | 4 | 32) && !dm_fps_lock_engine() && dm_fps_lock_floor());   // 20, 30, 60 (no more than 3 steps); the game's own interval stays
    if (which == 1) { dm_fps_lock_tick(); dm_fps_lock_tick(); assert(dm_fps_target() == 0 && *mode == 0 && writes == 0); return; }   // 15 at start: not held, the game's own
    dm_fps_lock_tick(); assert(*mode == 2 && *count == 1 && writes == 1);   // 30: the game's own two-step mode (its pick writes the 2)
    dm_fps_lock_tick(); assert(writes == 1);
    *mode = 1; dm_fps_lock_tick(); assert(*mode == 2 && writes == 2);   // the game set its own (a scene's setup): the cap goes in again
    dm_fps_target_set(20, "test"); dm_fps_lock_tick(); assert(*mode == STEPS_HOLD && *count == 3 && writes == 4);   // 20: 3 steps, held
    dm_fps_lock_tick(); assert(writes == 4);
    dm_fps_target_set(60, "test"); dm_fps_lock_tick(); assert(*mode == 1 && writes == 5);   // 60: the game's own mode, the one it set last
    dm_fps_target_set(0, "test"); dm_fps_lock_tick(); assert(*mode == 1 && writes == 5);
    // The game's own 30 fps mode (three or four players): nothing to write at 30, and it comes back after 20
    *mode = 2; *count = 2; dm_fps_target_set(30, "test"); dm_fps_lock_tick(); assert(*mode == 2 && writes == 5);
    dm_fps_target_set(20, "test"); dm_fps_lock_tick(); assert(*mode == STEPS_HOLD && *count == 3 && writes == 7);
    dm_fps_target_set(30, "test"); dm_fps_lock_tick(); assert(*mode == 2 && writes == 8);   // 20 → 30: the host's mode stays the host's
    dm_fps_target_set(0, "test"); dm_fps_lock_tick(); assert(*mode == 2 && writes == 9);
    // The frame limit (DM_FPS_LIMIT, the panel's limit) is held the same way, in whole vsyncs, and the limiter does not wait as well
    *mode = 0; *count = 1;
    nxhost_fps_limit_set(30); assert(dm_fps_limit_step() == 30 && nxhost_fps_limit_get() == 30); dm_fps_lock_tick(); assert(*mode == 2 && writes == 10);
    nxhost_fps_limit_set(45); assert(dm_fps_limit_step() == 30); dm_fps_lock_tick(); assert(*mode == 2 && writes == 10);
    dm_fps_target_set(20, "test"); dm_fps_lock_tick(); assert(*mode == STEPS_HOLD && *count == 3);   // the lower of the cap and the limit
    nxhost_fps_limit_set(0); dm_fps_target_set(0, "test"); dm_fps_lock_tick(); assert(*mode == 0 && dm_fps_limit_step() == 0);
    munmap(base, len);
}

int main(void) {
    for (int i = 0; i < 3; i++) { pid_t p = fork(); assert(p >= 0); if (!p) { steps_check(i); _exit(0); } int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    sead_rate_check();
    { pid_t p = fork(); assert(p >= 0); if (!p) { step_check(); _exit(0); } int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    // Each process starts the production one-time initialization with a fresh build and target.
    for (int i = 0; i < 20; i++) {
        pid_t p = fork(); assert(p >= 0);
        if (!p) { check(i); _exit(0); }
        int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Frame limits: integer/float FPS, milliseconds, vsync copies, floors behind pointers and under the controller's maximum, caps set while running, build gates and overrides, sead's controller at the measured rate (on and off live), framework step counts passed");
}
