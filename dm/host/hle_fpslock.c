// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md).
// Unreal Engine needs no row: the rewriter finds its console variables by their registered names in any build (dm/tools/ue_cvar.py, main's fixups UE_CVAR:<name>), and the host writes
// the frame limit, the profile's ue_fps_cvar (t.MaxFPS, or a game's own variable its settings apply to t.MaxFPS: SHIN MEGAMI TENSEI V: Vengeance's Project.FrameRateLimit), and the
// present interval, rhi.SyncInterval (nvn.SyncInterval in UE 4.18's Switch RHI: DRAGON QUEST XI S). Each is a TAutoConsoleVariable whose +0x10 points to its value, the game thread's
// copy then the render thread's. The games' configs set the limit to 30 and the interval to 2 (their notes name the .ini files)
#include "hle.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <errno.h>

// What a field holds: the frame rate as a float, double or int32, vsyncs per frame (60 / frame rate) as an int32, or the frame time in milliseconds as a float. Copies: consecutive copies of the
// value (Unreal keeps one for the game thread, one for the render thread). A row with no pointer (ptr_off 0) has its field at main + field: a constant in main's read-only data, which the loader
// leaves writable (the image's data block on iOS too, loader.c map_image).
// At 60 a frame time gets MS_MARGIN less than the vsync: such a limiter counts from the frame's start, and the work after it (the GPU fence wait, the next frame's start) adds to the
// period, so frames at exactly 16.667 ms came 0.11 ms late each on the Mac (59.6 fps, every 150th frame shown two vsyncs). Slightly shorter, the host's present pacing sets the pace
// (the acquire waits): Luigi's Mansion 3's display interval p99 18.69 → 16.67 ms, 60.0 fps. Not at 30: with present_interval display the presents follow every vsync and hold no
// frame for two, so a shorter time showed some frames for one (30.4 fps); 30 writes the game's own 33.333 ms
// Also: a frame time in ticks as a uint32 (scale / frame rate: 19,200,000 for the Switch's 19.2 MHz system tick), a frame step in seconds as a float (1 / frame rate), and a mode
// switch an engine reads before it takes those fields at all (written 1 at any target)
// And a floor: the least vsyncs per frame (60 / frame rate) a dynamic present interval controller may pick (sead's, Donkey Kong Bananza). Unlike the others it does not fix the
// rate: the game still raises its interval when frames run long and steps its logic by the interval it picked, so presentation keeps following the game's interval
// (dm_fps_lock_engine leaves floors out). Target 60 writes 1, the controller's own floor. The int32 after the floor is the controller's own maximum, which the game sets (3 in
// Donkey Kong Bananza): a floor above it breaks the controller's min <= max, and DKB's then stayed at the floor (interval 4 at a 15 cap) and never came back down when the host
// put its floor back, until the game restarted. So a floor row never writes above that maximum, and a rate needing more vsyncs than that is not offered (row_holds)
// The vsync kinds hold only rates that divide 60 (60, 30, 20, 15); the rate kinds any target (rate_ok)
// And a step count: a framework that runs a whole number of fixed steps each frame and presents at that many vsyncs, picked by a mode field before it (a profile's fps_steps rows; steps_tick)
enum { FPS_F32, VSYNCS_I32, MS_F32, FPS_I32, TICKS_U32, SECS_F32, MODE_U32, FPS_F64, MIN_VSYNCS_I32, STEPS_I32 };
#define MS_MARGIN 0.5f
static float ms_want(int fps) { return 1000.0f / (float)fps - (fps == 60 ? MS_MARGIN : 0); }
// deref: after [main + ptr_off], nderef more pointers, each at the object reached so far + deref[i] (an object behind a global, then members)
typedef struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies; uint32_t scale; uint8_t nderef; uint16_t deref[3]; } lock_t;
static int vsync_kind(int kind) { return kind == VSYNCS_I32 || kind == MIN_VSYNCS_I32 || kind == STEPS_I32; }
static int follows_game(int kind) { return kind == MIN_VSYNCS_I32 || kind == STEPS_I32; }   // the game still sets its present interval itself (dm_fps_lock_engine leaves these out)
static int rate_ok(int kind, int target) { return !vsync_kind(kind) || 60 % target == 0; }
static const lock_t k_locks[] = {
    // Donkey Kong Bananza v458752: sead's dynamic present interval controller (main+0x204a18, called each frame from the framework's main+0x203674). It picks the interval from
    // the last frame's start to the later of its submission and its GPU end, clamps it to [+0, +4] (its settings, 1 and 3, made in nnMain at main+0x5bf180 and copied in by
    // main+0x5bfb20), and hands 60 / interval to the framework's frame rate (main+0x204c60, framework+0xe0) and the interval to the window (main+0x204c00, framework+0x88): the
    // game's logic steps by that interval, as BotW's VFR does (docs/reference/frame-rate.md). A floor of 2 makes the game itself run at 30 and stay at the right speed.
    // The controller is at [framework+0x218] (main+0x5bfb00); the framework at [app], the app object at main+0x26cc208 (nnMain main+0x5bdff4, its GOT slot main+0x2455ca0)
    { "e25c90c1c136638885659fd55bf8a03b", "sead frame-rate controller minimum interval", 0x26cc208, 0x0, MIN_VSYNCS_I32, 1, 0, 2, {0x0, 0x218} },
    // Pokemon Brilliant Diamond 1.3.0: Unity's Application.targetFrameRate. Its icall registration (main+0xe34e60) resolves to main+0xdc8e70 -> +0x5fa640,
    // which stores w0 at main+0x4afc868; the getter at +0x5fa630 reads the same int32. The game sets 30 when entering the field; display pacing alone does not remove that cap.
    { "94ceae325c205c4b9d6f7235552f28fd", "Unity targetFrameRate", 0, 0x4afc868, FPS_I32, 1 },
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Luigi's Mansion 3 1.4.0: Next Level Games' frame limiter. At the end of each frame the main thread calls the frame callbacks (main+0xb7c308) with the milliseconds since the frame
    // started; the limiter (main+0x7a3e70) runs jobs or sleeps until 33.333 ms have passed, the float at main+0x13c94dc (read only there, at main+0x7a3ea8 and main+0x7a3f18), and the game
    // steps by the time that really passed. The game asks for present interval 2 too, which the profile's present_interval display ignores; both are needed
    { "79e5950ffa85acf63e28c9aec051eac2", "NLG frame limiter target", 0, 0x13c94dc, MS_F32, 1 },
    // Need for Speed: Hot Pursuit Remastered 1.0.3: Criterion's frame timing. A uint32 in main's bss (main+0xf0043c) selects the engine's 60 fps mode, in which it takes its frame time from
    // the timer object at [main+0xed96a8]: +0x30 the frame in system ticks (19.2 MHz, 640,000 at 30 fps) and +0x28 a tenth of it; the game's own step, at [main+0xeffc60] +0x30 (2D) and +0x50
    // (3D), is a float of seconds per frame. Its limiter, a spin on the system tick at main+0x483588..+0x4835a4, is cut by the rewriter (patches.py). From masagrator's FPSLocker config for this
    // build (FPSLocker-Warehouse, patches/010029B0118E8000/799D1061182C1302.yaml; the frame-time instruction found by theboy181)
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 60 fps mode", 0, 0xf0043c, MODE_U32, 1, 0 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion frame ticks", 0xed96a8, 0x30, TICKS_U32, 1, 19200000 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion frame ticks / 10", 0xed96a8, 0x28, TICKS_U32, 1, 1920000 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 2D game step", 0xeffc60, 0x30, SECS_F32, 1, 0 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 3D game step", 0xeffc60, 0x50, SECS_F32, 1, 0 },
    // EA SPORTS FC 27 1.8d.310c: the game's frame regulator, an object at [main+0xda20e38] (its constructor, main+0x99e80c8, sets 60.0). Its tick (main+0x99ea580) waits until
    // 1 / [+0x8] seconds have passed since the last frame; [+0x10] is the rate it reports. main+0x5d501c0 sets both to 30.0 whenever GameRenderSettings.Enable is 1 (fmov v1.2d, #30.0;
    // stur q1, [x9, #8]), a constant of the Switch build, so it is written here. The simulation's own rate is Frostbite's SimulationTimeSettings, set below by name: on a build without
    // this row the game keeps 30 fps at the right speed (the simulation at 60 and the regulator at 30: match clock as at 30). The game asks for present interval 2
    // (BaseDisplaySettings.PresentInterval), which the profile's present_interval display ignores
    { "a48c0de3bc353efa0e4d359bde05ef24", "FIFA frame regulator", 0xda20e38, 0x8, FPS_F64, 2, 0 },
};
#define NLOCKS ((int)(sizeof k_locks / sizeof k_locks[0]))
// Fixed steps per frame, the profile's fps_steps rows (dm/games/game.py; one per main build): a framework at [main + global] then [that + each member], whose frame function picks
// a step count into a field from a mode field before its update, runs the update that many fixed steps, and sets the window's present interval to the count. Mario Kart World 1.8.0
// (dm/games/0400C3F00006E000/README.md, Frame rate cap): procFrame main+0x69cbb0, mode +0x210 (0 = 1 step, or 2 after its own work overran; 1 = 1; 2 = 2, the game's 30 fps form for
// three or four players; any other value leaves the count as it is), count +0x214. A cap of 30 writes mode 2, the game's own 30 fps; 20 writes the count 3 and a mode outside
// 0..2 (STEPS_HOLD), which keeps it; 60 and 0 leave the game's own mode (it runs 2 steps on a frame that overran: full speed at 60 or below). Each step stays 1/60 s, so the race,
// ghosts, time trials and online races step as at 60. DM_FPS_STEPS_ROWS: the unit tests' own rows
#ifndef DM_FPS_STEPS_ROWS
#define DM_FPS_STEPS_ROWS DM_GAME_FPS_STEPS
#define DM_FPS_NSTEPS_ROWS DM_GAME_NFPS_STEPS
#endif
typedef struct { const char *build; uint64_t ptr_off; uint32_t mode, count; uint8_t nderef; uint16_t deref[3]; } steps_row_t;
static const steps_row_t k_steps[] = DM_FPS_STEPS_ROWS;
#define NSTEPS DM_FPS_NSTEPS_ROWS
#define STEPS_MAX 3    // 20 fps: the most steps a frame offered (each 1/60 s)
#define STEPS_HOLD 3   // a mode outside the game's 0..2: its pick then leaves the count the host wrote
static int rd(uint64_t a, void *out, size_t n) { return a >= 0x1000 && !plat_mem_read(a, out, n); }

// NieR 1.0.3 exposes its preset minimum but ships fixed mode-specific fallbacks.
// Keep the native current-scale and UV controller intact; only select its
// verified lower bound. This is independent of the FPS choice.
// The 1.0.3 builds the rewrite cuts (patches.py): the scale clamp and the publish branch that are checked, the two fallback floors the cuts
// replace, and the preset minimum. The Asian release (0100B8E016F76000) shifts the function by 0x13c0; its preset minimum is the target of the
// relocation of the slot the function loads it through (main+0x12c8500).
// display is the game's display object: its first word points to the NVN device (the present interval at +0x9c160), and +0x504 is the frame-interval
// mode (0: 60 fps, 1: 30 fps) the setter main+0x9c0590 (Asian release; the global's 0x13c0 before) writes; in the Asian release it is the target of the
// relocation of the slot that setter reads (main+0x12c2d18).
static const struct { const char *build; uint32_t clamp, publish, floor0, floor1, minimum, display; } k_nier_dr[] = {
    {"3c10ec35c5ef4c3686b4053afb0e58e7", 0x9bf568, 0x9bf578, 0x9bf528, 0x9bf534, 0x128e068, 0x192be00},
    {"98397a10419894652f372d4dc1b6fc2c", 0x9c0928, 0x9c0938, 0x9c08e8, 0x9c08f4, 0x128f130, 0x192d600},
};
static void dynamic_resolution_tick(void) {
    static int state = -1, dynamic, row;
    static uint8_t *base;
    if (state < 0) {
        state = 0;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
            for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; row = k; }
        if (!base) return;
        const char *e=getenv("DM_DYNAMIC_RESOLUTION");
        if (e && strcmp(e,"0") && strcmp(e,"1")) LOG("[RED] DM_DYNAMIC_RESOLUTION=%s: must be 0 or 1; keeping full resolution",e);
        dynamic=e && !strcmp(e,"1");
        if (dynamic) {
            uint32_t clamp = 0, publish = 0;
            uint64_t floors[2] = {0};
            if (!rd((uint64_t)(base + k_nier_dr[row].clamp), &clamp, 4) || !rd((uint64_t)(base + k_nier_dr[row].publish), &publish, 4) ||
                !rd((uint64_t)(base + k_nier_dr[row].floor0), &floors[0], 8) || !rd((uint64_t)(base + k_nier_dr[row].floor1), &floors[1], 8) ||
                clamp != 0x1e254c80u || publish != 0x54000489u ||
                floors[0] != 0x72a7f00c5280000cull || floors[1] != 0x72a7f00c5280000cull) {
                LOG("[RED] dynamic resolution needs a verified native scale/UV rewrite; keeping full resolution");
                dynamic = 0;
            }
        }
        state=1;
        LOG("main: NieR dynamic resolution %s (DM_DYNAMIC_RESOLUTION)",dynamic?"on; native controller, 70–100% scale":"off; full resolution");
    }
    if (!state) return;
    uint32_t cur[2];
    uint64_t a = (uint64_t)(base + k_nier_dr[row].minimum);
    if (!rd(a, cur, sizeof cur)) return;
    uint32_t minimum = dynamic ? 0x3f333333u : 0x3f800000u;
    if (cur[0] != minimum) {
        uint32_t *p = (uint32_t *)(uintptr_t)a;
        dm_guest_write_prepare(p, 4);
        __atomic_store_n(p, minimum, __ATOMIC_RELAXED);
    }
    if (!dynamic && cur[1] != 0x3f800000u) {
        uint32_t *p = (uint32_t *)(uintptr_t)(a + 4);
        dm_guest_write_prepare(p, 4);
        __atomic_store_n(p, 0x3f800000u, __ATOMIC_RELAXED);
    }
}

// Frostbite's simulation rate, found by name (no fixed addresses, so it holds across a game's updates). Frostbite reflects its settings classes: a class record (its name, then at
// +0x40 its field list) and per field a record of 0x20 bytes (name, hash, offset at +0x10, type). SimulationTimeSettings' ForceSimRate (int32) and MaxSimFps (float) cap the
// simulation's ticks a second: 30 in EA SPORTS FC 27, whose frames then cannot go faster than 30 whatever paces them. A static constructor registers the class: it stores the record's
// address into its type info (adrp / add of the record, then str into the type info just made by adrp / add), and the code that reads the settings asks the settings registry for the
// instance with that type info in x1 and a cache word in x2 (adrp / add each, then bl), which keeps the instance's address. The instance holds its type info at +8, which checks the cache.
// Both fields are written each frame they differ (the game may load its settings again), like the frame limits above
static struct { int n; uint8_t *base; uint64_t ti, cache[8]; int ncache; uint32_t off_rate, off_max; } fb;
static int fb_name_at(uint8_t *base, uint64_t size, uint64_t p, const char *want) {   // p, a relocated pointer, points into the module at the string want
    uint64_t b = (uint64_t)(uintptr_t)base; size_t n = strlen(want);
    return p >= b && p - b + n + 1 <= size && !memcmp((const void *)(uintptr_t)p, want, n + 1);
}
static int64_t fb_pair(const uint32_t *t, uint64_t i, int reg) {   // the target of adrp / add into reg at words i, i + 1, or -1 (module offsets: the text sits at offset 0)
    uint32_t w = t[i], x = t[i + 1];
    if ((w & 0x9F000000u) != 0x90000000u || (int)(w & 31) != reg || (x & 0xFFC00000u) != 0x91000000u || (x & 31) != (uint32_t)reg || ((x >> 5) & 31) != (uint32_t)reg) return -1;
    int64_t imm = (int64_t)(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
    return (int64_t)((i * 4) & ~0xfffull) + imm * 4096 + ((x >> 10) & 0xfff);
}
static void fb_find(void) {
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || !m->text_end || !m->ro_off || m->ro_off >= m->size) return;
    static const char cls[] = "SimulationTimeSettings";
    uint8_t *base = m->base, *ro = base + m->ro_off; uint64_t size = m->back_off ? m->back_off : m->size, b = (uint64_t)(uintptr_t)base;
    const uint8_t *s = memmem(ro, (size_t)(size - m->ro_off), "\0SimulationTimeSettings\0", sizeof cls + 1); if (!s) return;   // not Frostbite
    uint64_t name = (uint64_t)(uintptr_t)(s + 1), rec = 0;
    for (uint64_t o = (m->ro_off + 7) & ~7ull; o + 0x48 <= size && !rec; o += 8) {
        if (*(uint64_t *)(base + o) != name) continue;
        uint64_t fl = *(uint64_t *)(base + o + 0x40); if (fl < b || fl - b + 0x20 > size) continue;
        for (uint64_t f = fl - b; f + 0x20 <= size; f += 0x20) {   // the field list ends where a name stops being one
            uint64_t fn = *(uint64_t *)(base + f); if (fn < b || fn - b >= size) break;
            uint32_t off = *(uint32_t *)(base + f + 0x10);
            if (fb_name_at(base, size, fn, "ForceSimRate")) fb.off_rate = off;
            else if (fb_name_at(base, size, fn, "MaxSimFps")) fb.off_max = off;
        }
        if (fb.off_rate && fb.off_max) rec = o;
    }
    if (!rec) { LOG("[RED] main: Frostbite's %s found, but not its ForceSimRate and MaxSimFps fields; leaving the simulation rate", cls); return; }
    const uint32_t *t = (const uint32_t *)base; uint64_t nw = m->text_end / 4;
    for (uint64_t i = 0; i + 3 < nw && !fb.ti; i++) {   // the registration: adrp / add xR = record, str xR, [xT] with xT = the type info from an adrp / add just before
        int r = (int)(t[i] & 31); if (fb_pair(t, i, r) != (int64_t)rec) continue;
        uint32_t st = t[i + 2]; if ((st & 0xFFFFFC00u) != 0xF9000000u || (int)(st & 31) != r) continue;   // str xR, [xT] (no offset)
        int xt = (int)((st >> 5) & 31);
        for (uint64_t k = i >= 6 ? i - 6 : 0; k < i; k++) { int64_t ti = fb_pair(t, k, xt); if (ti > 0) fb.ti = (uint64_t)ti; }
    }
    if (!fb.ti) { LOG("[RED] main: Frostbite's %s: its type info was not found; leaving the simulation rate", cls); return; }
    for (uint64_t i = 8; i + 12 < nw && fb.ncache < 8; i++) {   // the readers: adrp / add x1 = type info, adrp / add x2 = cache word, near each other before a bl
        if (fb_pair(t, i, 1) != (int64_t)fb.ti) continue;
        for (uint64_t k = i - 8; k < i + 10; k++) {
            int64_t c = fb_pair(t, k, 2); if (c <= 0 || (uint64_t)c + 8 > m->size) continue;
            int known = 0; for (int j = 0; j < fb.ncache; j++) known |= fb.cache[j] == (uint64_t)c;
            if (!known) fb.cache[fb.ncache++] = (uint64_t)c;
            break;
        }
    }
    if (!fb.ncache) { LOG("[RED] main: Frostbite's %s: no settings lookup found; leaving the simulation rate", cls); return; }
    fb.base = base; fb.n = 1;
    LOG("main: Frostbite %s at main+%#llx (ForceSimRate +%#x, MaxSimFps +%#x), type info main+%#llx, %d instance caches", cls, (unsigned long long)rec, fb.off_rate, fb.off_max,
        (unsigned long long)fb.ti, fb.ncache);
}
static void fb_tick(int target, const char *src) {
    static int nlog; uint64_t ti = (uint64_t)(uintptr_t)(fb.base + fb.ti);
    for (int j = 0; j < fb.ncache; j++) {
        uint64_t inst = 0, tinfo = 0; int32_t rate; float max;
        if (!rd((uint64_t)(uintptr_t)(fb.base + fb.cache[j]), &inst, sizeof inst) || !inst || !rd(inst + 8, &tinfo, sizeof tinfo) || tinfo != ti) continue;   // not made yet, or another word
        if (!rd(inst + fb.off_rate, &rate, sizeof rate) || !rd(inst + fb.off_max, &max, sizeof max)) continue;
        if (rate != target) { int32_t *p = (int32_t *)(uintptr_t)(inst + fb.off_rate); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, target, __ATOMIC_RELAXED); }
        if (max != (float)target) { float w = (float)target, *p = (float *)(uintptr_t)(inst + fb.off_max); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n((uint32_t *)p, *(uint32_t *)&w, __ATOMIC_RELAXED); }
        if ((rate != target || max != (float)target) && nlog < 8) { nlog++; LOG("main: Frostbite simulation rate ForceSimRate %d, MaxSimFps %g → %d (%s)", rate, max, target, src); }
        return;
    }
}

// The Unreal rows of this main: the profile's frame-limit variable and the present intervals, at the objects the rewriter found (0 rows: not an Unreal main). *missing: an Unreal main
// (the rewriter found some of its variables) without the profile's frame-limit variable
static int ue_rows(const module_t *m, lock_t *out, int *missing) {
    static char what[3][48]; int n = 0, have = 0;
    for (int k = 0; k < m->n_ue_cvar && n < 3; k++) {
        const char *name = m->ue_cvar[k].name; int fps = !strcmp(name, DM_GAME_UE_FPS_CVAR);
        if (!fps && strcmp(name, "rhi.SyncInterval") && strcmp(name, "nvn.SyncInterval")) continue;
        have |= fps; snprintf(what[n], sizeof what[n], "Unreal %s", name);
        out[n] = (lock_t){ "", what[n], m->ue_cvar[k].off + 0x10, 0, fps ? FPS_F32 : VSYNCS_I32, 2, 0 }; n++;
    }
    *missing = m->n_ue_cvar > 0 && !have;
    return n;
}

// The running main's engine limiters, found once: by the first present (dm_fps_lock_tick) or by the graphics layer's present cap, which may ask first (dm_fps_lock_engine, from the acquire)
#define NROWS (NLOCKS + 3 + NSTEPS)
static lock_t ue[3], steps[NSTEPS > 0 ? NSTEPS : 1]; static const lock_t *rows[NROWS]; static int nrow, nier, ue_missing; static uint8_t *base; static uint32_t display;
static pthread_once_t g_found = PTHREAD_ONCE_INIT;
static void lock_find(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; nier = 1; display = k_nier_dr[k].display; }
        for (int k = 0; k < NLOCKS; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = &k_locks[k]; base = g_mod[i].base; }
        int nue = ue_rows(&g_mod[i], ue, &ue_missing);
        for (int k = 0; k < nue; k++) { rows[nrow++] = &ue[k]; base = g_mod[i].base; }
        for (int k = 0; k < NSTEPS; k++) if (k_steps[k].build[0] && !strcmp(g_mod[i].build_id, k_steps[k].build)) {   // a step count: field = the count, scale = the mode field
            const steps_row_t *S = &k_steps[k]; lock_t *L = &steps[k];
            *L = (lock_t){ S->build, "framework step count", S->ptr_off, S->count, STEPS_I32, 1, S->mode, S->nderef, {S->deref[0], S->deref[1], S->deref[2]} };
            rows[nrow++] = L; base = g_mod[i].base;
        }
    }
    fb_find();
}
// Whether the host writes fps_target into an engine limiter of this main (a row for its build, NieR's frame interval, Unreal's variables or Frostbite's simulation rate, found by name).
// Without one, and without a frame step state page, nothing makes the game follow the cap, so the graphics layer keeps the game's own present interval (hle_nvn.c fps_present_cap):
// presenting at a cap the game does not step by runs a fixed-step game fast or slow (Tears of the Kingdom 1.4.2 at 2x with DM_FPS_TARGET=60)
// A floor or a step count does not fix the rate on its own terms: the game still sets its present interval (at or above the floor; the step count, which at a cap below 60 is the cap's)
static int floor_only(void) { for (int r = 0; r < nrow; r++) if (!follows_game(rows[r]->kind)) return 0; return nrow > 0 && !nier && !fb.n; }
int dm_fps_lock_engine(void) { pthread_once(&g_found, lock_find); return (nrow || nier || fb.n) && !floor_only(); }
int dm_fps_lock_floor(void) { pthread_once(&g_found, lock_find); for (int r = 0; r < nrow; r++) if (follows_game(rows[r]->kind)) return 1; return 0; }   // the game picks its interval at or above a floor the host writes, or by the step count it writes
static int steps_row(void) { pthread_once(&g_found, lock_find); for (int r = 0; r < nrow; r++) if (rows[r]->kind == STEPS_I32) return 1; return 0; }
// The cap: DM_FPS_TARGET or the profile's fps_target at start, then whatever the front end sets while the game runs (dm_fps_target_set: the Android launcher's in-game panel,
// DM_CTL "fps <n>"). 0 = the game's own; else 15, 20, 30, 40, 45 or 60, each applied by the limiters that can hold it (rate_ok)
static int target_ok(int t) { return t == 0 || t == 15 || t == 20 || t == 30 || t == 40 || t == 45 || t == 60; }
static _Atomic int g_target = -1, g_target_gen; static const char *g_target_src;
__attribute__((weak)) uint8_t *dm_fps_state;   // loader.c: the frame step state page (NULL without a frame step); the unit tests have no loader
void dm_fps_step_fold(void);   // below: the frame limit folded into the frame step's cap
int dm_fps_target(void) {
    int t = atomic_load(&g_target);
    if (t >= 0) return t;
    const char *e = getenv("DM_FPS_TARGET"); t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (!target_ok(t)) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 15, 20, 30, 40, 45 or 60; leaving the game's frame limit", e); t = 0; }
    int was = -1;
    if (atomic_compare_exchange_strong(&g_target, &was, t)) g_target_src = e ? "DM_FPS_TARGET" : "game profile fps_target";
    return atomic_load(&g_target);
}
int dm_fps_target_gen(void) { return atomic_load(&g_target_gen); }
void dm_fps_target_set(int t, const char *src) {
    if (!target_ok(t)) { LOG("[RED] frame rate cap %d (%s): must be 0, 15, 20, 30, 40, 45 or 60; unchanged", t, src); return; }
    dm_fps_target(); g_target_src = src; atomic_store(&g_target, t); atomic_fetch_add(&g_target_gen, 1);
    LOG("frame rate cap %d%s (%s)", t, t ? " fps" : " (the game's own)", src);
    // A frame step (loader.c dm_fps_state_apply) reads its cap from the state page at every frame: 0, 30 and 60 go there too (with the frame limit folded in, dm_fps_step_fold)
    if (dm_fps_state) {
        if (t == 0 || t == 30 || t == 60) dm_fps_step_fold();
        else LOG("[RED] frame rate cap %d: this game's frame step holds 30 or 60 only; its cap stays until the next start", t);
    }
}
// The object a row's field is in, or 0 while the game has not made it yet
static uint64_t row_obj(const lock_t *L) {
    uint64_t obj = (uint64_t)(uintptr_t)base;
    if (!L->ptr_off) return obj;
    if (!rd((uint64_t)(uintptr_t)(base + L->ptr_off), &obj, sizeof obj) || !obj) return 0;
    for (int i = 0; i < L->nderef; i++) if (!rd(obj + L->deref[i], &obj, sizeof obj) || !obj) return 0;
    return obj;
}
// A floor row's controller maximum (the int32 after the floor, set by the game), or 0 while the game has not made the controller or set it
static int floor_max(const lock_t *L) {
    uint64_t obj = row_obj(L); int32_t mx;
    if (!obj || !rd(obj + L->field + 4, &mx, sizeof mx) || mx < 1 || mx > 60) return 0;
    return mx;
}
// Whether a row can hold cap t: its kind's rates (rate_ok), and for a floor no more vsyncs than the controller's own maximum (while that is unknown, the write waits for it)
static int row_holds(const lock_t *L, int t) {
    if (!rate_ok(L->kind, t)) return 0;
    int mx = L->kind == MIN_VSYNCS_I32 ? floor_max(L) : L->kind == STEPS_I32 ? STEPS_MAX : 0;
    return !mx || 60 / t <= mx;
}
static int holds(int t) {
    int ok = (nier && 60 % t == 0) || fb.n;
    for (int r = 0; r < nrow && !ok; r++) ok = row_holds(rows[r], t);
    return ok;
}
// Which caps this main's limiters hold, as a bit per rate (bit 0: 15, 1: 20, 2: 30, 3: 40, 4: 45, 5: 60); 0 = none, the game keeps its own frame rate
static const int k_rates[] = {15, 20, 30, 40, 45, 60};
int dm_fps_lock_rates(void) {
    pthread_once(&g_found, lock_find);
    int m = 0;
    for (int i = 0; i < 6; i++) if (holds(k_rates[i])) m |= 1 << i;   // a floor's maximum is read each time: the panel asks again while the game runs
    return m;
}
// sead's dynamic interval controller (a floor row's object: Donkey Kong Bananza main+0x204a18) at a 60 cap, on a device whose GPU clocks up only under steady load (the
// Odin 2 Portal's Adreno 740 at 401 of 680 MHz). The controller steps to a longer interval after +0x0c consecutive late frames (the game's 2) and back only when its
// cooldown +0x4c, refilled from +0x10, has counted down on frames that fit the shorter interval. At a low clock no frame fits 60, so once at 30 it stays there and
// the GPU, now idle half the time, never clocks up: the trap. Two writes break it:
// - patience: +0x0c late frames before stepping down, as many as DM_FPS_PATIENCE seconds (default 3, what the GPU governor needs to raise the clock; 0 leaves the
//   game's) take at the presents' real rate, kept between the game's own count and 180. Counted in time, not frames: the game steps its logic by the interval, so
//   late frames at interval 1 run it slow, and 180 frames of a 20 fps scene were 9 s of slow motion;
// - retry: after DM_FPS_RETRY seconds (default 10; 0 = never) above the floor, the cooldown to 0 and the next shorter interval (+0x44) to one step shorter than now,
//   so the controller tries the next faster rate (20 → 30, 30 → 60) on its next frame and the GPU gets the load that clocks it up. One step at a time: the same trap
//   holds a light scene at 20 (a 2,000-draw area of Donkey Kong Bananza at 475-550 MHz), and a jump from 20 straight to 60 failed in heavy ones (8,000 draws)
// Decoupling the game's speed from its present interval (DM_FPS_DECOUPLE, default 1; Android only): the controller hands the framework a frame rate of 60 / interval
// (framework+0xe0, through main+0x204c60 in Donkey Kong Bananza v458752), and the game steps its world by that rate: a frame presented late at interval 1 (54 fps on
// screen) still advanced the world 1/60 s, so the game ran slow. The framework's own VRR controller (main+0x204640, taken only on a display that reports VRR) hands
// it the measured rate in the same field; this does the same in the interval controller. The rewriter puts observation gate OBS_SEAD_RATE on the controller's
// sdiv of 60 by the interval in the main of a game whose profile opts in (fps_decouple sead; dm/tools/nso_code.py sead_find: found by its code, no build ID; a profile's OBS row names it for a build), with the
// dividend N, the divisor M (the interval) and the controller's register C in the gate number's upper bits. The callback writes N = rate * M, so the sdiv yields the
// rate, and the controller stores it and hands it to the framework as it would its own. Measured on the Portal (2026-11-05, the earlier form that loaded the rate
// from the controller): at interval 1 in a 20 fps area a half-second walk went as far as at the game's own interval 3, where without this it went a third of that.
// The rate is a whole number of frames a second (the field is an int32), the game's time step 1 / rate. It is the time between this frame's start and the last
// one's (the presents between two calls, so a frame the controller skips does not count twice), with the rounding error carried to the next frame: at 20.4 fps the
// rates alternate 20 and 21 so that the steps add up to the time that passed. Clamped to 10..60: below 10 fps the game slows down, as on the Switch, and the debt is
// dropped after a long frame (a load), so the game does not run ahead to catch up. sead_rate_log every 10 s: the game time handed out against the time that passed
#define SEAD_RATE_MIN 10
#define SEAD_RATE_MAX 60
static _Atomic uint32_t g_presents;   // presents so far (dm_fps_lock_tick)
// The rate for a frame of dt seconds with the carried rounding error *debt (seconds of real time not yet stepped), and the debt after it
static int32_t sead_rate_pick(double dt, double *debt) {
    double want = dt + *debt;
    int32_t r = want > 0 ? (int32_t)(1.0 / want + 0.5) : SEAD_RATE_MAX;
    if (r < SEAD_RATE_MIN) r = SEAD_RATE_MIN;
    if (r > SEAD_RATE_MAX) r = SEAD_RATE_MAX;
    *debt = want - 1.0 / r;
    if (*debt > 1.0 / SEAD_RATE_MAX) *debt = 0;   // a frame longer than the floor's: the game slows down, and does not catch up afterwards
    if (*debt < -1.0 / SEAD_RATE_MAX) *debt = -1.0 / SEAD_RATE_MAX;
    return r;
}
static struct { uint64_t ctl; double debt, game_s, wall_s; uint32_t presents, frames; struct timespec last, last_log; int32_t rate; } g_sr;
// On or off: the Android launcher's "Full game speed at low frame rates" (Game settings, and the in-game panel live: nxhost_fps_decouple_set), else DM_FPS_DECOUPLE
// at the first frame (default on; the launcher passes DM_FPS_DECOUPLE=0 for a start with the setting off). Off, the gate leaves the controller's 60 / interval as it is
// (the game as on the console) and lets go of the pin; on again, it starts from a fresh measurement. Read at every frame, so a change applies at the next one
static _Atomic int g_decouple = -1;
static const char *g_decouple_src;
__attribute__((visibility("default"))) void nxhost_fps_decouple_set(int on) {
    atomic_store(&g_decouple, on != 0); g_decouple_src = "the launcher's in-game panel";
    LOG("main: frame rate decoupling %s (%s)", on ? "on" : "off", g_decouple_src);
}
static int sead_rate_on(void) {
    int v = atomic_load(&g_decouple);
    if (v >= 0) return v;
    const char *e = getenv("DM_FPS_DECOUPLE"); int was = -1;
    v = !e || atoi(e) != 0;
    if (atomic_compare_exchange_strong(&g_decouple, &was, v)) { g_decouple_src = e ? "DM_FPS_DECOUPLE" : "default"; if (!v) LOG("main: frame rate decoupling off (DM_FPS_DECOUPLE=0)"); }
    return atomic_load(&g_decouple);
}
// The gate's work at time now (the unit tests call it with their own clock): regs x0..x29 as the gate saved them, param N | M << 5 | C << 10
static void sead_rate_at(uint64_t *r, int param, struct timespec now) {
    int n = param & 31, m = (param >> 5) & 31, c = (param >> 10) & 31; static int nlog;
    if (n > 28 || m > 28 || c > 28) return;
    int32_t interval = (int32_t)r[m];
    g_sr.ctl = r[c];
    uint32_t pr = atomic_load(&g_presents), frames = pr - g_sr.presents; g_sr.presents = pr;
    struct timespec last = g_sr.last; g_sr.last = now;
    if (!last.tv_sec && !last.tv_nsec) { g_sr.last_log = now; return; }   // the first frame: its own 60 / interval
    if (interval < 1 || interval > 8) return;   // not an interval: the game's own division
    double dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
    if (frames > 1) dt /= frames;
    int32_t rate = sead_rate_pick(dt, &g_sr.debt);
    r[n] = (uint32_t)(rate * interval);   // sdiv wD, wN, wM: rate * interval / interval
    g_sr.rate = rate; g_sr.frames++; g_sr.game_s += 1.0 / rate; g_sr.wall_s += dt * (frames > 1 ? frames : 1);
    if (!nlog++) LOG("main: frame rate decoupled from the present interval: sead's controller (%#llx) hands the game the measured rate (now %d fps, interval %d) instead of 60 / interval (DM_FPS_DECOUPLE)",
                     (unsigned long long)g_sr.ctl, rate, interval);
    double since = (double)(now.tv_sec - g_sr.last_log.tv_sec) + (now.tv_nsec - g_sr.last_log.tv_nsec) / 1e9;
    if (since >= 10) {   // the check of full speed: game time = wall time, whatever the frame rate
        LOG("main: sead rate: %.2f s of game time in %.2f s (%.1f%%), %u frames, %.1f fps, rate now %d", g_sr.game_s, g_sr.wall_s, g_sr.wall_s > 0 ? 100 * g_sr.game_s / g_sr.wall_s : 0,
            g_sr.frames, g_sr.wall_s > 0 ? g_sr.frames / g_sr.wall_s : 0, rate);
        g_sr.game_s = g_sr.wall_s = 0; g_sr.frames = 0; g_sr.last_log = now;
    }
}
// OBS_SEAD_RATE: on the game's main thread, once a frame, before the controller's sdiv. Also holds the controller's longest interval at 1 where the interval does not
// pace the game (sead_pin)
static void sead_pin(uint64_t ctl, int on);
static void sead_rate_gate(uint64_t *r, int param, struct timespec now) {   // the gate's work, the platform aside (the unit tests call it)
    int c = (param >> 10) & 31;
    if (!sead_rate_on()) {   // off: the game's own 60 / interval, and the controller free to switch intervals again
        g_sr.last = (struct timespec){0}; g_sr.debt = 0;
        if (c <= 28 && r[c]) sead_pin(r[c], 0);
        return;
    }
    sead_rate_at(r, param, now);
    static int display = -1;
    if (display < 0) { const char *m = getenv("DM_PRESENT_INTERVAL"); display = m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY; }
    if (g_sr.ctl) sead_pin(g_sr.ctl, display && dm_fps_target() == 0);
}
static void sead_rate(uint64_t *r, int param) {
    if (!PLAT_ANDROID) return;   // Android only: elsewhere the game keeps its own division
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    sead_rate_gate(r, param, now);
}
__attribute__((constructor)) static void sead_rate_attach(void) { gate_obs_set(OBS_SEAD_RATE, sead_rate); }

// Without a cap (the uncapped mode and the frame limiter's caps: presents on the host's own tick, the limiter pacing) the controller kept the game's tuning: two late
// frames moved it to a longer interval, six frames later it tried the shortest again. Each switch back to interval 1 let the game start its next frame without waiting:
// two frames of GPU work queued, the one after waited up to ~100 ms, and the decoupling took one 60 fps measurement: a hitch about once a second (Donkey Kong Bananza at
// 1080p, the controller flipping 3 and 1 several times a second, 2026-11-05). Here the interval does not pace the game (the GPU or the limiter does: 19-20 fps at
// interval 1 and 3 alike), so its longest interval (+0x04) is held at 1 and the controller never switches; the game's own longest comes back with a cap that needs it.
// Only where presentation follows the host's tick (DM_PRESENT_INTERVAL=display, the launcher's frame limit): where the interval paces the presents, interval 1 would trade a
// steady 30 for an uneven rate. The controller is the one the gate saw (sead_rate). DM_FPS_PIN=0 off
static int32_t g_pin_orig;
static void sead_pin(uint64_t ctl, int on) {
    static int en = -1, nlog;
    if (en < 0) { const char *e = getenv("DM_FPS_PIN"); en = PLAT_ANDROID && (!e || atoi(e) != 0); }   // Android only, like the rest of this pacing
    int32_t v[2]; if (!rd(ctl, v, sizeof v) || v[0] < 1 || v[1] < 1 || v[1] > 8) return;   // +0 shortest, +4 longest interval
    int32_t *p = (int32_t *)(uintptr_t)(ctl + 4);
    if (on && en) {
        if (v[1] != 1) { g_pin_orig = v[1]; dm_guest_write_prepare(p, 4); __atomic_store_n(p, 1, __ATOMIC_RELAXED);
                         if (nlog++ < 4) LOG("main: frame-rate controller held at interval 1 (its longest was %d): the GPU or the frame limiter paces the game (DM_FPS_PIN)", g_pin_orig); }
    } else if (g_pin_orig && v[1] == 1) { dm_guest_write_prepare(p, 4); __atomic_store_n(p, g_pin_orig, __ATOMIC_RELAXED); LOG("main: frame-rate controller's longest interval back to %d", g_pin_orig); g_pin_orig = 0; }
}

static void sead_ctl_tune(uint64_t ctl, int target, const char *src) {
    static int retry_s = -1, nlog, nretry; static float patience_s = -1, frame_s; static int32_t game_patience; static struct timespec slow_since, last_tick; static int slow;
    if (!PLAT_ANDROID) return;   // the GPU clock trap this works around was measured on Android phones; the Mac and iOS keep the game's own tuning
    if (patience_s < 0) { const char *e = getenv("DM_FPS_PATIENCE"); patience_s = e ? (float)atof(e) : 3; e = getenv("DM_FPS_RETRY"); retry_s = e ? atoi(e) : 10; }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (last_tick.tv_sec) {   // the presents' real interval (this runs once a present), smoothed over about 30 presents
        float dt = (float)((now.tv_sec - last_tick.tv_sec) + (now.tv_nsec - last_tick.tv_nsec) / 1e9);
        if (dt > 0 && dt < 0.5f) frame_s = frame_s ? frame_s + (dt - frame_s) / 30 : dt;
    }
    last_tick = now;
    int32_t v[0x14]; if (!rd(ctl, v, sizeof v)) return;   // +0x00 .. +0x4c
    int32_t floor_ = v[0], late = v[3], interval = v[0x10], lower = v[0x11];
    if (floor_ < 1 || interval < 1 || interval > 4) return;   // not set up yet
    if (target != 60) {   // the game's own patience back, for caps that hold a longer interval anyway
        if (game_patience && late != game_patience) { int32_t *p = (int32_t *)(uintptr_t)(ctl + 0xc); dm_guest_write_prepare(p, 4); __atomic_store_n(p, game_patience, __ATOMIC_RELAXED); }
        slow = 0; return;
    }
    if (patience_s > 0 && frame_s > 0 && late >= 1 && late <= 1000) {
        if (!game_patience) game_patience = late;
        int32_t want = (int32_t)(patience_s / frame_s + 0.5f);
        if (want > 180) want = 180;
        if (want < game_patience) want = game_patience;
        if (want != late && (want - late > 4 || late - want > 4 || want == game_patience)) {   // a few frames either way is not worth a write
            int32_t *p = (int32_t *)(uintptr_t)(ctl + 0xc); dm_guest_write_prepare(p, 4); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog++ < 6) LOG("main: frame-rate controller patience %d → %d late frames (%.1f s at %.1f fps; DM_FPS_PATIENCE; %s)", late, want, patience_s, 1 / frame_s, src);
        }
    }
    if (retry_s <= 0) return;
    if (interval <= floor_) { slow = 0; return; }
    if (!slow) { slow = 1; slow_since = now; return; }
    if ((now.tv_sec - slow_since.tv_sec) + (now.tv_nsec - slow_since.tv_nsec) / 1e9 < retry_s) return;
    int32_t *lo = (int32_t *)(uintptr_t)(ctl + 0x44), *cd = (int32_t *)(uintptr_t)(ctl + 0x4c);
    int32_t step = interval - 1 > floor_ ? interval - 1 : floor_;
    dm_guest_write_prepare(lo, 4); __atomic_store_n(lo, step, __ATOMIC_RELAXED);
    dm_guest_write_prepare(cd, 4); __atomic_store_n(cd, 0, __ATOMIC_RELAXED);
    if (nretry++ < 20) LOG("main: frame-rate controller at interval %d for %d s: trying interval %d (was next %d; DM_FPS_RETRY)", interval, retry_s, step, lower);
    slow_since = now;
}

// The host's frame limiter: it holds the game to at most a limit's frames a second, waiting on the game's thread before the present until 1 / limit after the previous
// frame's deadline; a frame that comes late starts a new schedule. For games whose speed follows the measured frame rate (sead_rate) it is the frame rate cap: with the
// present interval no longer pacing the game (DM_PRESENT_INTERVAL=display) the game runs as fast as it can. For any other game it is a cap below the game's own rate: the
// game keeps its present interval, so it is right only for a game whose logic steps by the time between frames (a game that counts frames would run slow); a steady
// frame time and less heat in place of a rate that wanders below the game's own.
// The limit: the launcher's in-game panel while the game runs (nxhost_fps_limit_set), else DM_FPS_LIMIT (the launcher's choice at start, or the player's own line),
// else the profile's fps_limit (Android only); 0 = none. Only Android waits (fps_limit_wait)
static _Atomic int g_fps_limit = -1; static const char *g_fps_limit_src;
void dm_fps_step_fold(void);
__attribute__((visibility("default"))) void nxhost_fps_limit_set(int fps) { atomic_store(&g_fps_limit, fps > 0 ? fps : 0); g_fps_limit_src = "the launcher's in-game panel"; LOG("main: frame limit %d fps%s", fps, fps > 0 ? "" : " (none)"); dm_fps_step_fold(); }
static int fps_limit_start(void) {
    const char *e = getenv("DM_FPS_LIMIT"); int l = e ? atoi(e) : PLAT_ANDROID ? DM_GAME_FPS_LIMIT : 0;
    if (l < 0) l = 0;
    int was = -1;
    if (atomic_compare_exchange_strong(&g_fps_limit, &was, l)) { g_fps_limit_src = e ? "DM_FPS_LIMIT" : "game profile fps_limit"; if (l) LOG("main: frame limit %d fps (%s)", l, g_fps_limit_src); }
    return atomic_load(&g_fps_limit);
}
static int limit_raw(void) { int l = atomic_load(&g_fps_limit); if (l < 0) l = fps_limit_start(); return l > 0 ? l : 0; }
// A game with a frame step (the rewriter's stubs, a state page: loader.c) steps its game time by the vsyncs the host hands it each frame (hle_nvn.c fstep_tick). The limiter
// alone held such a game's frames to the limit while the game went on stepping by its own interval: Ocarina of Time (0400486024DEC000) kept interval 1 (1/60 s a frame) at
// the profile's 30 fps limit and ran at half speed, and at 25 fps uncapped at 0.4. So for such a game the limit is the frame step's cap, in whole vsyncs: 60, 30 or 20 fps
// (the step's 1, 2 or 3 vsyncs; FSTEP_MAX is 3); a limit between them is held at the whole-vsync rate below it (45 and 40: 30 fps, with game time = real time), and the
// frame step paces the presents, so the limiter does not wait as well (its wait would count in the step's cost and look like late frames). Below 20 the limiter waits as
// before (a step of 4 vsyncs is not one the games' own logic uses). 0 = no limit, or no frame step. A framework's step count (a profile's fps_steps row, steps_tick) holds the limit the
// same way: its steps a frame and its present interval are the limit's vsyncs
static int limit_step(int l) { return l >= 60 ? 60 : l >= 30 ? 30 : l >= 20 ? 20 : 0; }
int dm_fps_limit_step(void) { return dm_fps_state || steps_row() ? limit_step(limit_raw()) : 0; }
// The frame step's cap in the state page (+0, float; 0 = the stubs run the game's own logic): the frame rate cap (fps_target, the in-game panel's cap) and the frame limit's
// step, whichever is lower. The stubs know 30 and 60 (several compare with 30.0): a 20 fps limit writes 30 there, and hle_nvn.c fps_step_cap takes the 20 from dm_fps_limit_step
void dm_fps_step_fold(void) {
    if (!dm_fps_state) return;
    uint32_t b = __atomic_load_n((uint32_t *)dm_fps_state, __ATOMIC_RELAXED); float was; memcpy(&was, &b, sizeof was);
    int t = dm_fps_target(), l = dm_fps_limit_step(), c = t == 30 || t == 60 ? t : 0;
    if (l && (!c || l < c)) c = l;
    float f = c ? (c < 30 ? 30.0f : (float)c) : 0.0f;
    __atomic_store_n((uint32_t *)dm_fps_state, *(uint32_t *)&f, __ATOMIC_RELAXED);
    if (f != was || l) {
        static int n; if (n++ < 16) {
            int raw = limit_raw();
            if (l) LOG("main: frame limit %d fps held by the frame step at %d fps (%d vsync%s a frame%s; frame rate cap %d)", raw, l, 60 / l, l == 60 ? "" : "s", raw != l ? ", the whole-vsync rate below the limit" : "", t);
            else LOG("main: frame step cap %g (%s)", f, f ? "fps" : "the game's own");
        }
    }
}
int nxhost_fps_limit_get(void) { int s = dm_fps_limit_step(); return s ? s : limit_raw(); }   // the rate the limit holds (0: none; a frame-step game's whole-vsync rate); probe.c aims the dynamic resolution at it, frame generation fits its frames to it
// Whether the game's speed follows its measured frame rate (the rewriter gated sead's controller in this main, DM_FPS_DECOUPLE not 0): frame generation's pacer then takes any base rate
// above its floor (a slower base does not slow the game), where a game held by the limiter alone steps as it would at that rate
int nxhost_fps_decoupled(void) {
    pthread_once(&g_found, lock_find);
    return PLAT_ANDROID && gate_obs_present(OBS_SEAD_RATE) && sead_rate_on();   // live: the in-game panel may turn it off
}
static void fps_limit_wait(void) {
    if (!PLAT_ANDROID) return;   // only the Android launcher sets a frame limit
    int lim = limit_raw();
    static uint64_t next;
    if (!lim || dm_fps_limit_step()) { next = 0; return; }   // none, or the frame step holds it (dm_fps_limit_step)
    uint64_t now = plat_abs_time(), period = 1000000000ull / (uint64_t)lim;   // the platform's monotonic clock and sleep (plat.h): clock_nanosleep is Linux only
    if (next > now && next - now <= period) { plat_wait_until(next); now = next; }
    next = (next > now || (next && now - next < period / 2) ? next : now) + period;   // keep the phase through small lateness, restart after a long frame
}

// A step count row (STEPS_I32, a profile's fps_steps): the cap, and the frame limit's whole-vsync rate if lower (dm_fps_limit_step), as the framework's steps a frame. n steps of
// 1/60 s each frame at 60 / n fps is game time = real time; the framework's present interval follows the count, so presentation needs nothing more (dm_fps_lock_engine leaves the
// row out: the game's own interval stays). 1 step (60, or no cap): the game's own mode, which runs 2 steps on a frame its work overran (full speed below 60, as on the Switch).
// 2: the mode that picks 2, the game's own 30 fps form. 3: STEPS_HOLD and the count 3 (the game's mode adjustments for 30 fps, made where the mode is 2, then take their 60 fps
// values; README of the game). The mode the game had is given back when the cap goes up again; one it sets meanwhile (a scene's setup) is the one given back, and the host's
// goes in again over it while the cap holds. One frame may run the game's own pick at a change: the gate is the game's own procFrame, which reads the mode before the count
static struct { int32_t game, mine; uint8_t ours; int nlog; } g_st[NROWS];   // ours: the mode in is the host's (mine); game: the game's own, given back
static int steps_cap(int target) { int c = limit_step(target), l = limit_step(limit_raw()); return l && (!c || l < c) ? l : c; }
static void steps_tick(const lock_t *L, int r, int target, const char *src) {
    uint64_t obj = row_obj(L); int32_t cur, cnt;
    if (!obj || !rd(obj + L->scale, &cur, sizeof cur) || !rd(obj + L->field, &cnt, sizeof cnt)) return;   // not made yet
    int c = steps_cap(target), n = c ? 60 / c : 1, want = n <= 1 ? -1 : n == 2 ? 2 : STEPS_HOLD;
    int32_t *mode = (int32_t *)(uintptr_t)(obj + L->scale), *count = (int32_t *)(uintptr_t)(obj + L->field);
    if (g_st[r].ours && cur != g_st[r].mine) g_st[r].ours = 0;   // the game set its own since (a scene's setup): that is the one to give back
    if (!g_st[r].ours) g_st[r].game = cur;
    if (want >= 0) {
        if (want == STEPS_HOLD && cnt != n) { dm_guest_write_prepare(count, sizeof *count); __atomic_store_n(count, n, __ATOMIC_RELAXED); }   // the count before the mode that keeps it
        if (cur != want) {
            dm_guest_write_prepare(mode, sizeof *mode); __atomic_store_n(mode, want, __ATOMIC_RELAXED); g_st[r].ours = 1; g_st[r].mine = want;
            if (g_st[r].nlog < 16) { g_st[r].nlog++; LOG("main: %s %d a frame at %d fps: its mode %d → %d (%s%s)", L->what, n, c, cur, want, src, c != limit_step(target) ? "; held by the frame limit" : ""); }
        }
    } else if (g_st[r].ours) {
        dm_guest_write_prepare(mode, sizeof *mode); __atomic_store_n(mode, g_st[r].game, __ATOMIC_RELAXED); g_st[r].ours = 0;
        if (g_st[r].nlog < 16) { g_st[r].nlog++; LOG("main: %s back to the game's own mode %d (%s)", L->what, g_st[r].game, src); }
    }
}

void dm_fps_lock_tick(void) {
    fps_limit_wait();
    atomic_fetch_add_explicit(&g_presents, 1, memory_order_relaxed);   // sead_rate's frame count
    dynamic_resolution_tick();   // separate from the FPS cap
    static int state = -2, nlog[NROWS], target, gen = -1, start_cap; static const char *src; static uint8_t over[NROWS];
    static uint32_t orig[NROWS][2]; static uint8_t have_orig[NROWS][2];   // what a row held before the host first wrote it: put back when the cap goes to 0 while the game runs
    if (state == -1) return;
    if (state == -2) {
        pthread_once(&g_found, lock_find);
        if (!nrow && !nier && !fb.n && !ue_missing) { state = -1; return; }
        int t = dm_fps_target();
        if (t && ue_missing) LOG("[RED] main: Unreal %s not found by the rewriter (ue_cvar.py NAMES; game profile ue_fps_cvar): the game's own frame limit stays", DM_GAME_UE_FPS_CVAR);
        if (!nrow && !nier && !fb.n) { state = -1; return; }
        state = 0;
    }
    int g = dm_fps_target_gen();
    if (g != gen) {   // the start, or a cap set while the game runs
        int first = gen < 0, t = dm_fps_target(); gen = g; src = g_target_src;
        if (!t) {
            if (first) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", rows[r]->what, src); if (nier) LOG("main: NieR frame interval left as the game sets it (%s)", src);
                         if (fb.n) LOG("main: Frostbite simulation rate left as the game sets it (%s)", src); }
            else for (int r = 0; r < nrow; r++) {   // back to the game's own: the values the game had before the host wrote them (NieR and Frostbite keep the last cap until the game sets them again)
                uint64_t obj = row_obj(rows[r]); if (!obj) continue;
                for (int c = 0; c < rows[r]->copies && c < 2; c++) if (have_orig[r][c] && rows[r]->kind != FPS_F64) {
                    uint32_t *p = (uint32_t *)(uintptr_t)(obj + rows[r]->field + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, orig[r][c], __ATOMIC_RELAXED);
                    int k = rows[r]->kind; float fv; memcpy(&fv, &orig[r][c], sizeof fv);
                    if (k == FPS_F32 || k == MS_F32 || k == SECS_F32) LOG("main: %s back to the game's %g (%s)", rows[r]->what, fv, src);
                    else LOG("main: %s back to the game's %u (%s)", rows[r]->what, orig[r][c], src);
                }
            }
        }
        else for (int r = 0; r < nrow; r++) if (!rate_ok(rows[r]->kind, t) && rows[r]->kind != STEPS_I32) LOG("main: %s cannot hold %d fps (whole vsyncs only): left as the game sets it (%s)", rows[r]->what, t, src);
        target = t; start_cap = first && t;
        if (!first) for (int r = 0; r < NROWS; r++) nlog[r] = over[r] = 0;
    }
    for (int r = 0; r < nrow; r++) if (rows[r]->kind == STEPS_I32) steps_tick(rows[r], r, target, src);   // every frame: the frame limit counts too, without a cap
    if (!target) return;   // no cap (the decoupling and the pin run in sead_rate, the controller's own gate)
    // The cap at start (DM_FPS_TARGET: the Android panel's saved cap, env.txt; or the profile) may be one this game cannot hold: 15 on Donkey Kong Bananza, whose controller goes to
    // 3 vsyncs at most. Once every floor's maximum is known, such a cap becomes 0, the game's own, so the front end and presentation show what the game really does
    if (start_cap) {
        int known = 1; for (int r = 0; r < nrow; r++) if (rows[r]->kind == MIN_VSYNCS_I32 && !floor_max(rows[r])) known = 0;
        if (known) {
            start_cap = 0;
            if (!holds(target)) {
                LOG("main: frame rate cap %d fps (%s) is not one this game can hold: the game's own frame rate instead", target, src);
                dm_fps_target_set(0, "a start cap this game cannot hold"); return;
            }
        }
    }
    if (nier && 60 % target == 0) {
        // NieR:Automata US/EU 1.0.3 (3c10ec35c5ef4c3686b4053afb0e58e7):
        // - [base + 0x192be00] points to the NVN graphics device object whose +0x9c160 holds the present interval (1 = 60 FPS, 2 = 30 FPS),
        //   and base + 0x192c304 (+0x504) holds the requested interval mode (0 = 60 FPS, 1 = 30 FPS).
        // Resolution policy is applied independently above, so 30/60 FPS does
        // not implicitly enable or disable the native resolution controller.
        uint64_t p = 0; static int nlog_nier;
        uint32_t want = (uint32_t)(60 / target), want_mode = want - 1, cur_mode = 0;
        if (rd((uint64_t)(uintptr_t)(base + display + 0x504), &cur_mode, sizeof cur_mode) && cur_mode != want_mode && (cur_mode == 0 || cur_mode == 1)) {
            uint32_t *dst = (uint32_t *)(uintptr_t)(base + display + 0x504);
            dm_guest_write_prepare(dst, sizeof *dst);
            __atomic_store_n(dst, want_mode, __ATOMIC_RELAXED);
        }
        if (rd((uint64_t)(uintptr_t)(base + display), &p, sizeof p) && p) {
            uint64_t a = p + 0x9c160; uint32_t cur = 0;
            if (rd(a, &cur, sizeof cur) && cur != want && (cur == 1 || cur == 2)) {
                uint32_t *dst = (uint32_t *)(uintptr_t)a;
                dm_guest_write_prepare(dst, sizeof *dst); __atomic_store_n(dst, want, __ATOMIC_RELAXED);
                if (nlog_nier++ < 8) LOG("main: NieR frame interval %u → %u (%d fps; %s)", cur, want, target, src);
            }
        }
    }
    if (fb.n) fb_tick(target, src);
    for (int r = 0; r < nrow; r++) {
        const lock_t *L = rows[r]; if (!rate_ok(L->kind, target) || L->kind == STEPS_I32) continue;   // step counts: steps_tick
        uint64_t obj = row_obj(L); if (!obj) continue;   // not created yet
        if (L->kind == MIN_VSYNCS_I32) { sead_pin(obj + L->field, 0); sead_ctl_tune(obj + L->field, target, src); }
        if (L->kind == FPS_F64) {
            for (int c = 0; c < L->copies; c++) {
                uint64_t a = obj + L->field + 8 * (uint64_t)c; double cur, want = target;
                if (!rd(a, &cur, sizeof cur)) break;
                if (cur == want || !(cur >= 10 && cur <= 120)) continue;   // already set, or not set yet
                double *p = (double *)(uintptr_t)a;
                dm_guest_write_prepare(p, sizeof *p); __atomic_store_n((uint64_t *)p, *(uint64_t *)&want, __ATOMIC_RELAXED);
                if (nlog[r] < 8) { nlog[r]++; LOG("main: %s%s %g → %d fps (%s)", L->what, L->copies > 1 ? (c ? " (reported rate)" : " (target)") : "", cur, target, src); }
            }
            continue;
        }
        for (int c = 0; c < L->copies; c++) {
            uint64_t a = obj + L->field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (L->kind == FPS_F32 || L->kind == MS_F32) {
                int ms = L->kind == MS_F32; float f, w = ms ? ms_want(target) : (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(ms ? f >= 8 && f <= 100 : f >= 10 && f <= 120)) continue;   // already set, or not set yet (RE Engine: 6000 at boot; Unreal: 0 = no limit until the config sets it)
                snprintf(was, sizeof was, ms ? "%.3f ms" : "%g", f);
            } else if (L->kind == SECS_F32) {
                float f, w = 1.0f / (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(f >= 1.0f / 120 && f <= 0.1f)) continue;   // not set yet (0 before the game's first frame)
                snprintf(was, sizeof was, "%.5f s", f);
            } else if (L->kind == TICKS_U32) {
                want = L->scale / (uint32_t)target;
                if (cur == want || cur < L->scale / 120 || cur > L->scale / 10) continue;
                snprintf(was, sizeof was, "%u", cur);
            } else if (L->kind == MODE_U32) {
                want = 1;
                if (cur) continue;   // already 1, or not this switch: 0 is the engine's default
                snprintf(was, sizeof was, "%u", cur);
            } else {
                int fps = L->kind == FPS_I32;
                want = fps ? (uint32_t)target : 60 / (uint32_t)target;
                if (cur == want || cur < (fps ? 10u : 1u) || cur > (fps ? 120u : 4u)) continue;
                if (L->kind == MIN_VSYNCS_I32) {   // never above the controller's own maximum (row_holds)
                    int mx = floor_max(L); if (!mx) continue;   // not set yet
                    if (want > (uint32_t)mx) { if (!over[r]) { over[r] = 1; LOG("main: %s left at %u: %d fps needs %u vsyncs, above the controller's maximum %d (%s)", L->what, cur, target, want, mx, src); } continue; }
                }
                snprintf(was, sizeof was, "%u", cur);
            }
            if (c < 2 && !have_orig[r][c]) { orig[r][c] = cur; have_orig[r][c] = 1; }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[r] < 8) { nlog[r]++; char now[24];
                if (L->kind == MS_F32) snprintf(now, sizeof now, "%.3f ms (%d fps)", ms_want(target), target);
                else if (L->kind == SECS_F32) snprintf(now, sizeof now, "%.5f s (%d fps)", 1.0 / target, target);
                else if (L->kind == TICKS_U32) snprintf(now, sizeof now, "%u (%d fps)", want, target);
                else if (L->kind == MODE_U32) snprintf(now, sizeof now, "1 (%d fps)", target);
                else snprintf(now, sizeof now, "%u%s", L->kind == FPS_F32 ? (unsigned)target : want, L->kind == FPS_F32 || L->kind == FPS_I32 ? " fps" : " vsyncs");
                LOG("main: %s%s %s → %s (%s)", L->what, L->copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, now, src); }
        }
    }
}

// The Mac's DM_CTL "cvarf <main offset> <value>" (hle_hid.c), for trying an Unreal console variable while the game runs: the TAutoConsoleVariable at main+offset holds at +0x10
// the pointer to its value, the game thread's copy then the render thread's; both are set to the float
void dm_ctl_cvar_f32(uint64_t off, float v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvarf main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { float *p = (float *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvarf main+%#llx = %g (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
// "peek <guest address> <count>": count floats (and their bits) from guest memory, read safely, to the log
void dm_ctl_peek(uint64_t a, int n) {
    for (int i = 0; i < n && i < 64; i += 4) {
        uint32_t w[4] = {0}; int k = n - i < 4 ? n - i : 4;
        if (!rd(a + 4 * (uint64_t)i, w, 4 * (size_t)k)) { LOG("DM_CTL: peek %#llx: not readable", (unsigned long long)(a + 4 * (uint64_t)i)); return; }
        float f[4]; memcpy(f, w, sizeof f);
        LOG("DM_CTL: peek %#llx: %g %g %g %g (%08x %08x %08x %08x)", (unsigned long long)(a + 4 * (uint64_t)i), f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
    }
}
// "memdump <guest address> <bytes> <path>": guest memory to a file, read safely (stops at the first unreadable page)
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] DM_CTL: memdump: cannot open %s", path); return; }
    static uint8_t buf[1 << 16]; uint64_t done = 0;
    while (done < n) { size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf; if (!rd(a + done, buf, k)) break; fwrite(buf, 1, k, f); done += k; }
    fclose(f); LOG("DM_CTL: memdump %#llx: %llu of %llu bytes → %s", (unsigned long long)a, (unsigned long long)done, (unsigned long long)n, path);
}
// "cvari <main offset> <value>": the same for an int console variable
void dm_ctl_cvar_i32(uint64_t off, int32_t v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvari main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { int32_t *p = (int32_t *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvari main+%#llx = %d (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
