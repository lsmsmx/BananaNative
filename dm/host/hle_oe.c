// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default: the profile's op_mode, console unless set; console corresponds to the console-mode render profile); DM_LANG=language code (default en-US; a BCP-47 style tag such as fr-FR is mapped to the console's code, dm_lang_code; the iOS app sets it from the IPA's DMLanguage, main.m).
#include "hle.h"
#include <ctype.h>

// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : DM_GAME_OPMODE; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
    return m;
}
// ---------- System notification queue (AppletMessage): front end state changes → fetched by the game every frame with TryPopNotificationMessage ----------
// Message numbers the host sends: 15 focus change, 30 operation mode change, 31 performance mode change, 300 graphics scale change (Switch 2 SDKs: the games' notification
// handlers take 300 to oe::GetCurrentGraphicsScale, as 30 to GetOperationMode and 31 to GetPerformanceMode: Mario Kart World 1.8.0 main+0x29957b8, Ocarina of Time
// main+0x3bc038c, Splatoon Raiders main+0x3e91528)
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31, MSG_GFXSCALE = 300 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_mq_cond = PTHREAD_COND_INITIALIZER;   // cond: wakes the blocking PopNotificationMessage
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode, g_ntf_gfxscale;          // only pushed after the game turns it on with Set*NotificationEnabled
// Notification event (Hades main+0x3ed18c polls it every frame and pops one message each time it fires): the SDK's version returns the event oe::Initialize would have
// created, and the host's oe::Initialize is a no-op, so TryWaitSystemEvent asserted on it. A host-owned auto-clear SystemEvent instead, which follows the queue as the am service's
// does: signalled while a notification is queued, cleared when the last one is popped. Monster Hunter Rise's MessageSuperviseThread (main+0x6bc8fc4) waits on it with WaitAny,
// which does not clear it, then pops until the queue is empty; when the event was only ever signalled, the first focus change on iOS left it set and the thread spun on a core
// (995 ms/s, 5-6 W on an iPhone). The count is read and the event set or cleared under g_mq_lock, so a push racing a pop cannot leave it cleared over a queued notification
// (lock order g_mq_lock → G → g_se_lock; nothing takes g_mq_lock while holding G)
static uint8_t g_mq_ev[0x80] __attribute__((aligned(16)));
static _Atomic int g_mq_ev_on;
static void mq_ev_sync(void) {
    if (!atomic_load(&g_mq_ev_on)) return;
    pthread_mutex_lock(&g_mq_lock);
    if (g_mqn) hle_signal_system_event(g_mq_ev); else hle_clear_system_event(g_mq_ev);
    pthread_mutex_unlock(&g_mq_lock);
}
static void mq_push_(uint32_t m);
static void mq_push(uint32_t m) { mq_push_(m); mq_ev_sync(); }
static void mq_push_(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31/300 are invalidation notifications meaning "query the current state", not events carrying historical values.
    // While one of the same type has not been taken yet, the first queue position is kept: a consumer loop only reads the latest state, repeated notifications write back the same thing, and with the same size there is no further resize.
    // Only these four state notifications are merged; future per-occurrence events (such as Resume/Exit) must not use this rule.
    // Legitimate producers only send these four, so at most four items are pending, and any number of focus losses/restores no longer hits the 32-slot limit; after a pop, new changes are enqueued as usual.
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE || m == MSG_GFXSCALE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe notification queue overflow (is the game not popping?)");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_cond_broadcast(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_lock);
}
static void oe_state_set(int m, int scale);   // with the graphics scale, below
// Called by the front end (overheat downgrade etc.): change the state and push 30 / 31 (and 300 when the scale follows the mode) according to the switches the game has turned on
void dm_set_opmode(int m) { oe_state_set(m, -2); }
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background. The front end calls dm_focus_set (iOS lifecycle / Mac window losing focus)
static _Atomic int g_focus = 1; static _Atomic uint64_t g_focus_in_ns;   // the front end's state; time of its most recent entry into InFocus
// Focus handling mode the game set (-1: none). The SDK's oe::SetFocusHandlingMode (SDK 18 sdk+0x3c5770) turns it into am's SetFocusHandlingMode(notify, ?, suspend) and
// SetOutOfFocusSuspendingEnabled: 0 → (0, 0, 1), 1 → (1, 1, 0), 2 → (1, 0, 1), 3 → (0, 0, 1) + out-of-focus suspending. Only 1 and 2 ask for focus notifications; under 0 and 3
// the system suspends the game out of focus and it never sees message 15 (the host's suspend is the guest clock freeze and the graphics gate). Super Mario Galaxy 2 sets 3, and
// its Wii layer's pause on focus 2 (main+0x663ba0) never finished: the main loop waits for the GX thread to pause (main+0x5ce870), which waits for work the paused CPU never sends
static _Atomic int g_focus_mode = -1;
static int focus_notifies(void) { int m = atomic_load(&g_focus_mode); return m != 0 && m != 3; }   // no mode set: notify, as before
// Library applets the game has open (dm_applet_enter / dm_applet_leave, DM_APPLET in hle.h). On Horizon an applet the application starts takes its focus: am sends message 15
// with OutOfFocus when it opens (libnx: AppletFocusState_OutOfFocus, "LibraryApplet open") and 15 with InFocus when it closes, and the application's Show call returns once it
// has closed. The host's stand-ins answer on the caller's thread, so the game sees that pair around the call. Mario Kart 8 Deluxe applies the controller applet's answer only
// after the InFocus notification (its notification thread main+0x8ea560 sets the flag main+0x87b5d0 takes each frame); with none its 2P character select never took input
// (GitHub issue #550). The game sees the front end's state, or 2 while an applet is open over an application in focus; a change of what it sees is one notification 15
static int g_applets; static _Atomic int g_seen = 1; static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static void focus_sync(int applets) {   // applets: +1 an applet opens, -1 one closes, 0 the front end's state changed
    pthread_mutex_lock(&g_seen_lock);
    g_applets += applets;
    if (g_applets < 0) die("dm_applet_leave without dm_applet_enter");
    int w = atomic_load(&g_focus), s = w == 1 && g_applets ? 2 : w, changed = atomic_exchange(&g_seen, s) != s;
    pthread_mutex_unlock(&g_seen_lock);
    if (!changed) return;   // pushed outside the lock: 15 only says "query the state again", so pushes from two threads need no order
    LOG("oe focus state → %d%s", s, applets > 0 ? " (library applet open)" : applets < 0 ? " (library applet closed)" : "");
    if (focus_notifies()) mq_push(MSG_FOCUS);
}
void dm_applet_enter(const char *name) { LOG("oe library applet: %s", name); focus_sync(1); }
void dm_applet_leave(void) { focus_sync(-1); }
int dm_focus_now(void) { return atomic_load(&g_focus); }   // the front end's state, not an applet's: the display link follows the window
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }   // graphics layer display link watchdog: stalls are only computed while active, counted from activation (nvn_mtl.m mtl_present_wait)
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set: invalid state %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    focus_sync(0);
}
static int opmode(void) { return dm_opmode(); }

// Screenshot / video recording switches: the host has no system screenshot service, no-op
HLE_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "dismiss the system boot screen (through the am application proxy); the host has no boot screen", void, h_oe_FinishLogo, void) {}   // Metroid Dread calls it at main+0xadfed0
// am retains the requested inactivity extension even when auto-sleep is disabled.
// SDK 12.3.9 maps the normal setter to mode 1, Unsafe to mode 2, and false to 0.
// Xenoblade Chronicles 3 reads it after the opening movie (sdk+0x3391f0).
static _Atomic int g_inactivity_mode;
HLE_FORCE("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", "inactivity extension mode 2 or 0; the host never turns the screen off", void, h_oe_InactivityExt, unsigned on) { g_inactivity_mode = (on & 1) ? 2 : 0; }
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "inactivity extension mode 1 or 0; the host never turns the screen off", void, h_oe_InactivityExtSafe, unsigned on) { g_inactivity_mode = on & 1; }
HLE_FORCE("_ZN2nn2oe43GetUserInactivityDetectionTimeExtensionModeEv", "inactivity extension mode requested by the application", int, h_oe_InactivityMode, void) { return g_inactivity_mode; }
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "video recording switch; the host has no system recording", void, h_oe_EnableRec, void) {}
// Applet resource user ID (ARUID, the number am assigns to this process): the host has no am, and the SDK's own getter returns 0. SDK 20's OpenAudioRenderer (the CPU rendering + manual execution path, sdk+0x221730)
//   aborts when it gets 0 (The Legend of Zelda: Breath of the Wild 1.9.0); SDK 16 did not check. Report a fixed non-zero ID, used only as an identifier
HLE_FORCE("_ZN2nn6applet23GetAppletResourceUserIdEv", "applet resource user ID; the host returns a fixed non-zero value", uint64_t, h_applet_Aruid, void) { return 0x80; }
// Launch parameter (present only when another program launched this one with one; returns bool): the host never passes one
HLE_FORCE("_ZN2nn2oe21TryPopLaunchParameterEPmPvm", "launch parameter: none", unsigned, h_oe_PopLaunchParam, size_t *out, void *buf, size_t n) { (void)buf; (void)n; if (out) *out = 0; return 0; }
// Tears of the Kingdom (SDK 20.5.22): the index of the program that launched this one in a multi-program application. The SDK asks the applet proxy, which the host never opens
// (null read in applet::GetPreviousProgramIndex right after the window setup); -1 = launched from the home menu
HLE_FORCE("_ZN2nn2oe23GetPreviousProgramIndexEv", "previous program index: launched from HOME, -1", int, h_oe_PrevProgram, void) { return -1; }
// Launch another program of a multi-program application (Octopath Traveler 0 imports it); on hardware it does not return. The host has only this program: logged, and the game carries on
HLE_FORCE("_ZN2nn2oe14ExecuteProgramEiPKvm", "launch another program of the application: the host runs only this one (logged)", void, h_oe_ExecProgram, int index, const void *param, size_t n) {
    (void)param; LOG("[RED] oe::ExecuteProgram(%d, %zu bytes of parameters): the host runs only this program; the game carries on", index, n);
}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "video recording switch; the host has no system recording", void, h_oe_DisableRec, void) {}
// Silence the game's sound in system video captures (through am; FINAL FANTASY RESONANCE imports both): the host records nothing
HLE_FORCE("_ZN2nn2oe13MuteRecordingEv", "mute the sound of system video recording; the host has no system recording", void, h_oe_MuteRec, void) {}
HLE_FORCE("_ZN2nn2oe15UnmuteRecordingEv", "unmute the sound of system video recording; the host has no system recording", void, h_oe_UnmuteRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "screenshot switch; the host has no system screenshot", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "screenshot switch; the host has no system screenshot", void, h_oe_DisableShot, void) {}
// Super Mario Odyssey: HOME button blocking around its own menus, screenshot orientation, and the auto-sleep query (all through am)
HLE_FORCE("_ZN2nn2oe23BeginBlockingHomeButtonEv", "temporarily block the HOME button (through am); the host has no HOME button", void, h_oe_BlockHome, void) {}
HLE_FORCE("_ZN2nn2oe21EndBlockingHomeButtonEv", "restore the HOME button (through am)", void, h_oe_UnblockHome, void) {}
HLE_FORCE("_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", "screenshot orientation (through am); the host has no system screenshot", void, h_oe_ShotOrient, int o) { (void)o; }
// DRAGON QUEST XI S: auto-sleep disabled sections (through am). The host never sleeps the device; the query reports whether a section is open, as the applet does
static _Atomic int g_nosleep;
HLE_FORCE("_ZN2nn2oe29BeginAutoSleepDisabledSectionEv", "open an auto-sleep disabled section (through am); the host never auto-sleeps, counted for the query", void, h_oe_NoSleepBegin, void) { g_nosleep++; }
HLE_FORCE("_ZN2nn2oe27EndAutoSleepDisabledSectionEv", "close an auto-sleep disabled section (through am)", void, h_oe_NoSleepEnd, void) { if (g_nosleep > 0) g_nosleep--; }
HLE_FORCE("_ZN2nn2oe19IsAutoSleepDisabledEv", "whether an auto-sleep disabled section is open", unsigned, h_oe_NoSleepQ, void) { return g_nosleep > 0; }
HLE_FORCE("_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", "whether the application requested an inactivity extension", unsigned, h_oe_InactivityExtQ, void) { return g_inactivity_mode != 0; }
// Hyrule Warriors: Age of Calamity (SDK 9.6.0, main+0x8d1238): the copyright frame buffer, an overlay the system draws only into screenshots and recordings. The SDK hands the buffer to am as transfer memory
// (svcCreateTransferMemory, then a null applet proxy). The host has no system capture, so it keeps nothing
HLE_FORCE("_ZN2nn2oe30InitializeCopyrightFrameBufferEPvm", "copyright overlay buffer (for screenshots / recording, through am); the host has no system screenshot", void, h_oe_CopyrightInit, void *buf, size_t n) { (void)buf; (void)n; }
HLE_FORCE("_ZN2nn2oe17SetCopyrightImageEPKvmiiiiNS0_16WindowOriginModeE", "copyright overlay image; the host has no system screenshot", void, h_oe_CopyrightImage, const void *p, size_t n, int x, int y, int w, int h, int origin) { (void)p; (void)n; (void)x; (void)y; (void)w; (void)h; (void)origin; }
HLE_FORCE("_ZN2nn2oe22SetCopyrightVisibilityEb", "copyright overlay visibility; the host has no system screenshot", void, h_oe_CopyrightVisible, int on) { (void)on; }

// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "operation mode query; a fixed value from DM_OPMODE", int, h_oe_GetOpMode, void) { return opmode(); }
// Profile perf_mode boost: Boost whatever the operation mode, a combination a Switch 1 never reports, so a profile choice and not a default. Rhythm Heaven Groove keeps one flag per mode
//   (main+0xa764 / main+0xa784): console means a TV, which gets a latency calibration (main+0x20d0, scene script main+0x4c0820), and Boost alone picks its 1920x1080 draw size
//   (table main+0xada6d0, else 1280x720). Handheld with Boost: no TV calibration on a phone, full-size drawing
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "performance mode query; same as the operation mode (profile perf_mode boost: Boost)", int, h_oe_GetPerfMode, void) { return DM_GAME_PERF_BOOST ? 1 : opmode(); }
// SDK 11 calls of Monster Hunter Rise: the CPU overclock query reads an apm session the host never opened (null read in sdk+0x35258 at the start of nnMain, main+0x6bc8650);
// the default display resolution is the TV's (1080p docked) or the panel's (720p handheld) and only changes with the operation mode
// Taiko Risshiden V DX (SDK 12.3.3) turns it on in nnMain (main+0x2e1e4c): the setter goes through the same apm session (null read at sdk+0x362b8). The host does not scale clocks;
// the query reports what was set, as apm does
static unsigned char g_cpu_oc;
HLE_FORCE("_ZN2nn2oe21IsCpuOverclockEnabledEv", "CPU overclock state (through apm): what SetCpuOverclockEnabled set, off at start", unsigned, h_oe_CpuOc, void) { return g_cpu_oc; }
HLE_FORCE("_ZN2nn2oe22SetCpuOverclockEnabledEb", "CPU overclock switch (through apm); the host does not scale clocks", void, h_oe_SetCpuOc, unsigned on) { g_cpu_oc = on & 0xff; }
// Switch 2 SDKs: the graphics scale, which the SDK takes from am's IGraphicsScalingParametersFunctions (a session the host never opened: null read in
// oe::GetGraphicsScalingParameters). The SDK maps the parameter's first word 10 / 20 / 30 to scale 0 / 1 / 2. Donkey Kong Bananza keeps it as a quality tier: a byte of its graphics
// settings (+0x1c7, main+0x549348, next to the handheld flag it takes from the operation mode at +0x1c4), and an index into {3, 12, 16} (main+0x1d1a604) whose value less 3 (0, 9, 13)
// goes to a renderer parameter (main+0x549614). Which tier the console reports in which mode is not measured: the host answers 0 handheld and 1 console, following the operation mode.
// Mario Kart World sizes its 3D scenes by it (its display state takes it at main+0x8728f4 in 1.8.0): handheld scale 0 draws 1920x1080, console scale 1 2048x1152 of its 3840x2160
// window, scale 2 smaller still, and its final composite stretches the scene to the window. DM_GRAPHICS_SCALE=0, 1 or 2 answers that scale in every mode (the launcher's lower
// resolution on a phone GPU); GetGraphicsScalingParameters answers the matching 10 / 20 / 30, so the SDK's two queries agree
// The setting is runtime state like the operation mode: the launcher's in-game panel changes it while the game runs (nxhost_graphics_mode_set, below)
static _Atomic int g_gfx_scale = -1;   // -1 not read yet, 0-2 DM_GRAPHICS_SCALE (or the panel's), 3 following the operation mode
static int gfx_setting(void) {
    int v = g_gfx_scale;
    if (v < 0) {
        const char *e = getenv("DM_GRAPHICS_SCALE");
        if (e && *e >= '0' && *e <= '2' && !e[1]) { v = *e - '0'; LOG("oe: graphics scale %d in every operation mode (DM_GRAPHICS_SCALE)", v); }
        else { if (e && *e) LOG("[RED] DM_GRAPHICS_SCALE=%s: must be 0, 1 or 2; the scale follows the operation mode", e); v = 3; }
        int exp = -1; atomic_compare_exchange_strong(&g_gfx_scale, &exp, v); v = g_gfx_scale;
    }
    return v;
}
static int gfx_scale_of(int setting, int mode) { return setting < 3 ? setting : mode == 1; }
static int gfx_scale(void) { return gfx_scale_of(gfx_setting(), opmode()); }
HLE_FORCE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", "graphics scale (Switch 2, through am): 0 handheld, 1 console (DM_GRAPHICS_SCALE or the in-game panel: 0-2 in every mode)", int, h_oe_GfxScale, void) { return gfx_scale(); }
HLE_FORCE("_ZN2nn2oe28GetGraphicsScalingParametersEv", "graphics scaling parameters (Switch 2, through am): 10 / 20 / 30 for graphics scale 0 / 1 / 2", unsigned, h_oe_GfxScaleParams, void) { return 10u * (gfx_scale() + 1); }
HLE_FORCE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", "graphics scale change notification switch (Switch 2, through am): message 300",
          void, h_oe_GfxScaleNotify, unsigned on) { g_ntf_gfxscale = on & 0xff; }
// Default display resolution change events the game asked for (GetDefaultDisplayResolutionChangeEvent): signalled when an operation mode change changes the resolution
#define DEFRES_EV 4
static void *g_defres_ev[DEFRES_EV]; static _Atomic int g_defres_n;
// Operation mode and graphics scale changes, as the console makes them (docking, the system's graphics settings): the whole new state first, then one notification per query
// whose answer changed (30 and 31 for the mode, 300 for the scale), each only when the game turned it on, so a game that re-queries on the first one already sees all of it.
// m -1 keeps the mode; scale -2 keeps the setting, 3 follows the mode, 0-2 holds that scale in every mode
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;   // two producers (the panel, an overheat downgrade) do not interleave their read and store
static void oe_state_set(int m, int scale) {
    pthread_mutex_lock(&g_state_lock);
    int om = opmode(), os = gfx_setting(), nm = m < 0 ? om : m, ns = scale == -2 ? os : scale < 0 || scale > 2 ? 3 : scale;
    int scale_changed = gfx_scale_of(ns, nm) != gfx_scale_of(os, om);
    g_opmode = nm; g_gfx_scale = ns;
    pthread_mutex_unlock(&g_state_lock);
    if (nm != om) LOG("oe operation mode %d → %d", om, nm);
    if (ns != os) LOG("oe: graphics scale setting %d → %d (3: following the operation mode), the game's scale %d", os, ns, gfx_scale_of(ns, nm));
    if (nm != om) {
        if (g_ntf_opmode) mq_push(MSG_OPMODE);
        if (g_ntf_perfmode && !DM_GAME_PERF_BOOST) mq_push(MSG_PERFMODE);   // the performance mode follows the operation mode (profile perf_mode boost: always Boost, it does not change)
        int n = atomic_load(&g_defres_n); for (int i = 0; i < n; i++) hle_signal_system_event(g_defres_ev[i]);
    }
    if (scale_changed && g_ntf_gfxscale) mq_push(MSG_GFXSCALE);
}
// The launcher's in-game panel (Game resolution while the game runs, Native.graphicsModeSet): the graphics scale and operation mode of the choice's graphics_scale row
// (Profile.resolutionLive). scale -1 follows the mode and m -1 is the profile's op_mode: both -1 is what a start without the lines gives (Original without a row)
__attribute__((visibility("default"))) void nxhost_graphics_mode_set(int scale, int m) {
    LOG("main: Game resolution from the launcher's in-game panel: graphics scale %d, operation mode %d (-1: the game's own)", scale, m);
    oe_state_set(m < 0 ? DM_GAME_OPMODE : m, scale < 0 ? 3 : scale);
}
// The frame length the game aims at (Switch 2 SDKs): oe::SetGraphicsTargetFrameRate maps up to 59 fps to mode 1, up to 119 to mode 2 and above to mode 3, SetGraphicsTargetFrameRateAuto
// to mode 0, and both call this, which locks the gs graphics server's context and sends it the mode. The host has no graphics server (nv::InitializeGraphics), so the context is null and
// the lock crashed: Hyrule Warriors: Age of Imprisonment sets 60 / its present interval at boot (main+0x1e5d244). The host paces by the window's present interval, so the mode is only noted
HLE_FORCE("_ZN2nn2gs3sdk26SetGraphicsFrameLengthModeENS1_23GraphicsFrameLengthModeE", "target frame length (Switch 2, gs): noted; the host paces by the present interval",
          void, h_gs_FrameLength, int mode) {
    static int last = -1; if (mode != last) { LOG("gs: graphics frame length mode %d (0 auto, 1 30 fps, 2 60 fps, 3 120 fps)", mode); last = mode; }
}
HLE_FORCE("_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_", "default display resolution: 1920x1080 docked, 1280x720 handheld", void, h_oe_DefRes, int *w, int *h) {
    int con = opmode() == 1; if (w) *w = con ? 1920 : 1280; if (h) *h = con ? 1080 : 720;
}
HLE_FORCE("_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE", "default display resolution change event: signalled when the operation mode changes",
          void, h_oe_DefResEv, void *ev) {
    hle_bind_system_event(ev, 0);
    int n = atomic_load(&g_defres_n);
    for (int i = 0; i < n; i++) if (g_defres_ev[i] == ev) return;
    if (n < DEFRES_EV) { g_defres_ev[n] = ev; atomic_store(&g_defres_n, n + 1); }
    else LOG("[RED] oe: more than %d default display resolution change events; the later ones are not signalled", DEFRES_EV);
}
HLE_FORCE("_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", "server maintenance overlay (through am); the host has no system overlay", void, h_oe_MaintOverlay, int64_t a, int64_t b) { (void)a; (void)b; }
// apm performance configurations: Normal 0x00010000 (CPU 1020 / GPU 384 / EMC 1600 MHz, the handheld default), Boost 0x00010001 (GPU 768 MHz, the docked default)
HLE_FORCE("_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", "query the CPU/GPU clock config (through apm); reports the default for the mode", int, h_oe_GetPerfCfg, int mode) { return mode == 1 ? 0x00010001 : 0x00010000; }
HLE_FORCE("_ZN2nn2oe24SetAlbumImageOrientationENS_5album16ImageOrientationE", "screenshot orientation (through am); the host has no system screenshot", void, h_oe_AlbumOrient, int o) { (void)o; }
// The Great Ace Attorney Chronicles (SDK 10), first thing in nnMain (main+0x6b188c): the SDK's version asks am through the applet proxy the host never made (null read at sdk+0x338070)
HLE_FORCE("_ZN2nn2oe34DisableAlbumImageTakenNotificationEv", "screenshot-taken notifications (through am); the host takes no system screenshots", void, h_oe_AlbumNotifyOff, void) {}
// Enable uses the same am application proxy (SDK 13.3.5 sdk+0x36ec50); the host produces no system-album notifications.
// Xenoblade Chronicles 3 (SDK 12.3.9) enables this at main+0x127fac4;
// the SDK otherwise dereferences the absent am SelfController at sdk+0x33b0f0.
// As with Disable, there is no producer: the host has no Horizon album captures.
HLE_FORCE("_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", "screenshot-taken notifications (through am); the host takes no system screenshots", void, h_oe_AlbumNotifyOn, void) {}
HLE_FORCE("_ZN2nn2oe24IsInStoreDemoModeEnabledEv", "retail store demo mode (through am); never", unsigned, h_oe_StoreDemo, void) { return 0; }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "set the CPU/GPU clock config; the host does not scale clocks", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x (ignored)", mode, cfg);
}

// Scripted state changes (verify lifecycle / overheat downgrade / a graphics scale change without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30,gscale=2@40" (second N;
// gscale=-1 follows the operation mode again); bg=1 / bg=0 run the whole background round trip (see script_bg)
// Time counts from the game's first notification fetch. Coexists with front end calls.
#include <unistd.h>
// bg=1 / bg=0: the whole "to background / back to foreground" sequence, in the same order as the scene lifecycle in front_ios.m (resign active -> enter background; enter foreground -> become active), minus the audio session reset iOS does on returning.
// Used to reproduce "after going to the background and back" problems on the Mac (audio take-over and resume, guest clock freeze, graphics suspend); to imitate the process being suspended as well, send dmhost SIGSTOP / SIGCONT between the two items
static void script_bg(int on) {
    if (on) { dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
    else { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); dm_focus_set(1); }
}
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT entry '%s' should be key=value@seconds", tok);
        uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
        if (due > now) usleep((useconds_t)((due - now) / 1000));
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else if (!strcmp(key, "gscale")) oe_state_set(-1, v < 0 ? 3 : v);   // 0-2, or -1: follow the operation mode again
        else if (!strcmp(key, "bg")) script_bg(v);
        else die("DM_OE_SCRIPT unknown key '%s' (focus / opmode / gscale / bg)", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT thread");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "foreground / background state; the front end's dm_focus_set, out of focus while a library applet is open", int, h_oe_GetFocus, void) { return atomic_load(&g_seen); }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "focus handling mode; 1 and 2 get focus notifications, 0 and 3 none (see g_focus_mode)", void, h_oe_SetFocusMode, int m) {
    atomic_store(&g_focus_mode, m); LOG("oe::SetFocusHandlingMode %d (%s)", m, focus_notifies() ? "focus notifications" : "suspended out of focus, no focus notifications");
}

// Notification: returns bool (w0), the host has no system notifications
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "system notification queue; front end state changes are queued", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    if (ok) LOG("oe notification popped by the game: %u", *msg);   // state changes are rare, printing each one does not flood the log
    mq_ev_sync();   // signalled again while notifications remain, cleared when the queue is empty (also a signal left behind by a pop that found it empty)
    return ok;
}
// The blocking form (Link's Awakening pops on a thread of its own, main+0xfa1ff8): the SDK's version waits on the system event oe::Initialize would have created,
// and the host's oe::Initialize is a no-op, so it aborted on an uninitialized event. Wait for the host queue instead, then take the message the Try path's way
HLE_FORCE("_ZN2nn2oe22PopNotificationMessageEv", "system notification queue (blocking); waits on the host queue", uint32_t, h_oe_PopMsg, void) {
    pthread_once(&g_oe_once, oe_script_start);   // DM_OE_SCRIPT also for games that only pop this way (Mario Kart 8 Deluxe): the wait below reaches the Try path only once a notification is queued
    uint32_t m = 0;
    for (;;) {
        pthread_mutex_lock(&g_mq_lock);
        while (!g_mqn) {
            dm_thread_native_wait(1);
            pthread_cond_wait(&g_mq_cond, &g_mq_lock);
            dm_thread_native_wait(0);
        }
        pthread_mutex_unlock(&g_mq_lock);
        if (h_oe_TryPopMsg(&m)) return m;   // another thread may have taken it in between: wait again
    }
}
HLE_FORCE("_ZN2nn2oe27GetNotificationMessageEventEv", "notification event: the host's auto-clear SystemEvent, signalled while the queue holds notifications", void *, h_oe_GetMsgEv, void) {
    pthread_once(&g_oe_once, oe_script_start);   // DM_OE_SCRIPT also for games that wait on the event and pop only once it fires (Monster Hunter Rise)
    if (!atomic_load(&g_mq_ev_on)) { hle_bind_system_event(g_mq_ev, 1); atomic_store(&g_mq_ev_on, 1); mq_ev_sync(); }   // bound on the game's thread (it runs SDK code)
    return g_mq_ev;
}
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "resume notification switch; the host expresses resume as focus notification 15", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "operation mode change notification switch", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "performance mode change notification switch", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// The Horizon language code (nn::settings::LanguageCode, the 18 of nn::settings::Language) for a language tag: one of those codes in any letter case, or a BCP-47 style
// tag as a phone or a player writes it (fr-FR, de_DE, es-MX, pt-PT, zh-TW, zh-Hant-HK). A tag the console has no language for gets en-US, and 0 is returned (1 for a
// code it knows). Before, DM_LANG went to the game as typed, and fr-FR, which no game knows, fell back to the game's own first language (Japanese in Mario Kart World)
static int dm_lang_code(const char *tag, char out[8]) {
    char t[32] = {0}; size_t n = 0;
    for (; tag && tag[n] && n < sizeof t - 1; n++) t[n] = tag[n] == '_' ? '-' : (char)tolower((unsigned char)tag[n]);
    char lang[4] = {0}, script[5] = {0}, region[4] = {0};
    const char *p = t; int part = 0;
    while (*p) {   // the subtags: language, then a script (4 letters) and a region (2 letters or 3 digits), each optional
        const char *q = strchr(p, '-'); size_t len = q ? (size_t)(q - p) : strlen(p);
        if (part == 0) { if (len < 2 || len > 3) break; memcpy(lang, p, len); }
        else if (len == 4 && !script[0] && !region[0]) memcpy(script, p, 4);
        else if ((len == 2 || len == 3) && !region[0]) memcpy(region, p, len);
        part++; p += len; if (*p) p++;
    }
    const char *c = NULL;
    int r = !strcmp(region, "");
    if (!strcmp(lang, "ja")) c = "ja";
    else if (!strcmp(lang, "en")) c = r || !strcmp(region, "us") || !strcmp(region, "ca") || !strcmp(region, "ph") ? "en-US" : "en-GB";   // Horizon's two: American and British
    else if (!strcmp(lang, "fr")) c = !strcmp(region, "ca") ? "fr-CA" : "fr";
    else if (!strcmp(lang, "de")) c = "de";
    else if (!strcmp(lang, "it")) c = "it";
    else if (!strcmp(lang, "es")) c = r || !strcmp(region, "es") ? "es" : "es-419";   // Spain's, else Latin America's (es-MX, es-AR, es-US, es-419 ...)
    else if (!strcmp(lang, "nl")) c = "nl";
    else if (!strcmp(lang, "pt")) c = !strcmp(region, "br") ? "pt-BR" : "pt";
    else if (!strcmp(lang, "ru")) c = "ru";
    else if (!strcmp(lang, "ko")) c = "ko";
    else if (!strcmp(lang, "zh"))   // the script decides, else the region: Traditional for Taiwan, Hong Kong and Macao
        c = !strcmp(script, "hant") || (!script[0] && (!strcmp(region, "tw") || !strcmp(region, "hk") || !strcmp(region, "mo"))) ? "zh-Hant" : "zh-Hans";
    memset(out, 0, 8);
    strncpy(out, c ? c : "en-US", 7);
    return c != NULL;
}

// The language the game is given: DM_LANG through dm_lang_code (en-US when unset), logged once with what it was mapped from
static void dm_lang(char out[8]) {
    const char *e = getenv("DM_LANG");
    if (!e || !*e) { memset(out, 0, 8); strcpy(out, "en-US"); return; }
    int ok = dm_lang_code(e, out);
    static int once;
    if (!once++) {
        if (!ok) LOG("[RED] DM_LANG=%s: no console language for it: en-US (the codes: ja, en-US, en-GB, fr, fr-CA, de, it, es, es-419, nl, pt, pt-BR, ru, ko, zh-Hans, zh-Hant)", e);
        else if (strcmp(e, out)) LOG("language: DM_LANG=%s is %s", e, out);
    }
}

// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "system language query; value from DM_LANG", uint64_t, h_oe_GetLang, void) {
    char code[8];
    dm_lang(code);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// The console's region (Animal Crossing: New Horizons 3.0.3): the SDK asks settings::system::GetRegionCode, which reaches set:sys. A region that fits the language the game is given:
// 0 Japan, 1 the Americas, 2 Europe, 4 Hong Kong / Taiwan / Korea (nn::settings::system::RegionCode; 3 Australia and 5 China are left out, no language points at them alone)
HLE_FORCE("_ZN2nn2oe13GetRegionCodeEv", "console region (set:sys); from DM_LANG", int, h_oe_GetRegion, void) {
    char e[8]; dm_lang(e);
    int r = !strncmp(e, "ja", 2) ? 0 : !strncmp(e, "ko", 2) || !strncmp(e, "zh", 2) ? 4 : !strcmp(e, "en-US") || !strcmp(e, "fr-CA") || !strcmp(e, "es-419") || !strcmp(e, "pt-BR") ? 1 : 2;
    static int once; if (!once++) LOG("oe::GetRegionCode: %d for language %s", r, e);
    return r;
}

// Whether the console limits the game's own language choice: the SDK asks the am session at vtable +0x238, which the host does not open (oe::Initialize). Hyrule Warriors:
// Age of Imprisonment asks at boot (main+0x1af4504), after picking its language from GetDesiredLanguage. The host's language is DM_LANG alone and limits nothing
HLE_FORCE("_ZN2nn2oe26IsLanguageSelectionLimitedEv", "language selection limit (through am): none", unsigned, h_oe_LangLimited, void) { return 0; }

// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "pseudo device ID; a fixed host value", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}
