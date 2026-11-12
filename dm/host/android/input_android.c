// The launcher's input, motion, rumble and lifecycle calls (input_android.h) into the host: gamepads → dm_pad_set_player, the on-screen controls and phone shakes → dm_input_set
// (the touch gamepad's path, as on iOS), touchscreen fingers → dm_touch_set, the phone's sensors → dm_motion_set_switch, game vibration → the launcher's rumble callback
#include "../hle.h"
#include "../front.h"
#include "../hle_haptics.h"
#include "../pad_slots.h"
#include "input_android.h"
#include <android/looper.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp (DM_PAD_IGNORE)
#include <unistd.h>
#include <android/sensor.h>
#include <media/NdkMediaCodec.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // pads, slots, touch, shake
static pad_slots g_slots;
static int g_labelled[DM_PLAYERS];
static nxin_pad g_pad[DM_PLAYERS];
static float g_dz = -1;
static nxin_touch g_touch; static int g_touch_init;
static uint64_t g_shake_btn;
static int g_held;   // the text input box is up: pad and touch events are dropped (android_text_input)

static void touch_once(void) { if (!g_touch_init) { nxin_touch_init(&g_touch); g_touch_init = 1; } }
static const void *owner(int device) { return (const void *)(intptr_t)((int64_t)device + 0x100000000ll); }   // never NULL, even for device 0
static float deadzone(void) {
    if (g_dz < 0) { const char *e = getenv("DM_PAD_DEADZONE"); g_dz = e ? (float)atof(e) : 0.1f; }
    return g_dz;
}
static void publish_pad(int p) {   // under g_mu
    uint64_t b; int32_t lx, ly, rx, ry;
    nxin_pad_out(&g_pad[p], deadzone(), &b, &lx, &ly, &rx, &ry);
    dm_pad_set_player(p, b, lx, ly, rx, ry);
}
static void publish_touch_pad(void) {   // under g_mu
    uint64_t b; int32_t lx, ly, rx, ry;
    nxin_touch_pad(&g_touch, &b, &lx, &ly, &rx, &ry);
    dm_input_set(b | g_shake_btn, lx, ly, rx, ry);
}
static void publish_fingers(void) {   // under g_mu
    _Static_assert(sizeof(nxin_finger) == sizeof(dm_touch), "finger layout");
    dm_touch t[16]; int n = nxin_touch_fingers(&g_touch, (nxin_finger *)t, 16);
    dm_touch_set(t, n);
}
static nxin_map g_map; static int g_map_on = -1;   // DM_PAD_MAP (the launcher's Button mapping), read once
static const nxin_map *pad_map(void) {   // under g_mu
    if (g_map_on < 0) {
        const char *e = getenv("DM_PAD_MAP"); char bad[64];
        g_map_on = e && *e && nxin_map_parse(&g_map, e, bad, sizeof bad) > 0;
        if (e && *e) LOG("input: DM_PAD_MAP %s: %d button(s) remapped", e, __builtin_popcount(g_map.set));
        if (e && *e && *bad) LOG("[RED] input: DM_PAD_MAP: \"%s\" is not <Android button>=<Switch button> (names in docs/env_switches.tsv): left out", bad);
    }
    return g_map_on ? &g_map : NULL;
}
// DM_PAD_IGNORE: devices whose name holds one of these (comma-separated, case-insensitive) never get a player. A phone can expose a *virtual* gamepad of its own
// (Xiaomi's "uinput-goodix" touch node answers SOURCE_GAMEPAD on some models, so the launcher reports it) and it arrives before the pad in the hand: it took player 1
// and the real controller became player 2, which a single-player game never reads. `DM_PAD_IGNORE=uinput` leaves player 1 to the pad
static int g_ign[8]; static int g_nign;   // the device IDs refused by the name list (a key or axis event from them is refused too)
static int ignored_dev(int device) { for (int i = 0; i < g_nign; i++) if (g_ign[i] == device) return 1; return 0; }
static int name_ignored(const char *name) {
    static char list[512]; static int got;
    if (!got++) { const char *e = getenv("DM_PAD_IGNORE"); snprintf(list, sizeof list, "%s", e ? e : ""); if (list[0]) LOG("input: DM_PAD_IGNORE=%s: such devices get no player", list); }
    if (!list[0] || !name) return 0;
    for (char *p = list; *p; ) { char *c = strchr(p, ','); size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n && strncasecmp(p, name, n) == 0) return 1;
        p = c ? c + 1 : p + n; }
    return 0;
}
static int take(int device, const char *name, int labelled) {   // under g_mu
    if (ignored_dev(device)) return -1;
    if (name_ignored(name)) {
        int n = g_nign; g_nign = n < 8 ? n + 1 : 8; g_ign[(n < 8 ? n : 7)] = device;
        LOG("input: device %d (%s) is left out of the players (DM_PAD_IGNORE)", device, name ? name : "?");
        return -1;
    }
    int had = pad_slot_of(&g_slots, owner(device)) >= 0;
    int p = pad_slot_take(&g_slots, owner(device), name);
    if (p < 0) { if (!had) LOG("[RED] input: device %d (%s): every player is taken", device, name ? name : "?"); return -1; }
    if (!had) {
        const char *e = getenv("DM_PAD_LABELLED");
        g_labelled[p] = e ? atoi(e) : labelled; memset(&g_pad[p], 0, sizeof g_pad[p]); g_pad[p].map = pad_map();   // 1: all four face buttons by label; 2: A and B by label, X and Y by position
        dm_pad_players(pad_slot_mask(&g_slots));
        LOG("input: device %d (%s) is player %d%s", device, name && *name ? name : "unnamed", p + 1, g_labelled[p] == 1 ? ", face buttons by label" : g_labelled[p] == 2 ? ", A and B by label, X and Y by position" : "");
    }
    return p;
}
int nxhost_input_device_added(int device, const char *name, int labelled) {
    pthread_mutex_lock(&g_mu); int p = take(device, name, labelled); pthread_mutex_unlock(&g_mu); return p;
}
void nxhost_input_device_removed(int device) {
    pthread_mutex_lock(&g_mu);
    int p = pad_slot_free(&g_slots, owner(device));
    if (p >= 0) {
        memset(&g_pad[p], 0, sizeof g_pad[p]); dm_pad_set_player(p, 0, 0, 0, 0, 0);
        dm_pad_players(pad_slot_mask(&g_slots)); dm_motion_clear(p);
        LOG("input: device %d (player %d) removed", device, p + 1);
    }
    pthread_mutex_unlock(&g_mu);
}
int nxhost_input_device_first(int device, const char *name) {
    pthread_mutex_lock(&g_mu);
    int p = take(device, name, 0), from = p, moved = -1;   // take: connected (labels as its added call gave them, or none)
    if (p > 0) {
        int lab = g_labelled[0]; g_labelled[0] = g_labelled[p]; g_labelled[p] = lab;
        p = pad_slot_first(&g_slots, owner(device), name, &moved);
        for (int q = 0; q < DM_PLAYERS; q++) if (q == 0 || q == from) { memset(&g_pad[q], 0, sizeof g_pad[q]); g_pad[q].map = pad_map(); publish_pad(q); }
        dm_pad_players(pad_slot_mask(&g_slots));
        if (moved > 0) LOG("input: device %d (%s) is player 1; player 1's device moves to player %d", device, name && *name ? name : "unnamed", moved + 1);
        else LOG("input: device %d (%s) is player 1", device, name && *name ? name : "unnamed");
    }
    pthread_mutex_unlock(&g_mu); return p;
}
int nxhost_input_device_player(int device) { pthread_mutex_lock(&g_mu); int p = pad_slot_of(&g_slots, owner(device)); pthread_mutex_unlock(&g_mu); return p; }
bool nxhost_input_key(int device, int keycode, bool down) {
    pthread_mutex_lock(&g_mu);
    const nxin_map *m = pad_map(); int src = nxin_src_of_key(keycode);
    if (!nxin_key_button(keycode, 0) && !(m && src >= 0 && (m->set & (1u << src)))) { pthread_mutex_unlock(&g_mu); return false; }   // BUTTON_C / Z only when mapped
    if (g_held) { pthread_mutex_unlock(&g_mu); return true; }
    int p = take(device, NULL, 0);
    if (p >= 0 && nxin_pad_key(&g_pad[p], keycode, down, g_labelled[p])) publish_pad(p);
    pthread_mutex_unlock(&g_mu);
    return true;
}
bool nxhost_input_axes(int device, const int *axes, const float *values, int n) {
    nxin_pad probe = {0};
    if (!nxin_pad_axes(&probe, axes, values, n)) return false;
    pthread_mutex_lock(&g_mu);
    if (g_held) { pthread_mutex_unlock(&g_mu); return true; }
    int p = take(device, NULL, 0);
    if (p >= 0) { nxin_pad_axes(&g_pad[p], axes, values, n); publish_pad(p); }
    pthread_mutex_unlock(&g_mu);
    return true;
}
bool nxhost_input_axis(int device, int axis, float value) { return nxhost_input_axes(device, &axis, &value, 1); }
void nxhost_input_deadzone(float dz) { pthread_mutex_lock(&g_mu); g_dz = dz < 0 ? 0 : dz > 0.9f ? 0.9f : dz; pthread_mutex_unlock(&g_mu); }

void nxhost_input_touch(int action, int id, float x, float y) {
    pthread_mutex_lock(&g_mu); touch_once();
    if (g_held) { pthread_mutex_unlock(&g_mu); return; }
    int r = nxin_touch_event(&g_touch, action, id, x, y);
    if (r & 1) publish_touch_pad();
    if (r & 2) publish_fingers();
    pthread_mutex_unlock(&g_mu);
}
void nxhost_touch_layout(float vw, float vh, float px, float py, float pw, float ph) {
    pthread_mutex_lock(&g_mu); touch_once();
    if (vw > 0 && vh > 0) { g_touch.sw = vw; g_touch.sh = vh; }
    if (pw > 0 && ph > 0) { g_touch.gx = px; g_touch.gy = py; g_touch.gw = pw; g_touch.gh = ph; }
    pthread_mutex_unlock(&g_mu);
}
static void touch_release_all(void) {   // under g_mu
    if (!g_touch_init) return;
    nxin_touch_event(&g_touch, AM_CANCEL, 0, 0, 0); publish_touch_pad(); publish_fingers();
}
void nxhost_touch_controls_enable(bool on) {
    pthread_mutex_lock(&g_mu); touch_once();
    if (g_touch.enabled != on) { touch_release_all(); g_touch.enabled = on; }
    pthread_mutex_unlock(&g_mu);
}
int nxhost_touch_controls_set(const nxhost_touch_control *c, int n) {
    pthread_mutex_lock(&g_mu); touch_once();
    int kept = 0, r = nxin_touch_set_layout(&g_touch, c, n, &kept);
    if (r & 1) publish_touch_pad();
    if (r & 2) publish_fingers();
    pthread_mutex_unlock(&g_mu);
    LOG("input: on-screen controls: %d, %s layout", kept, c && n > 0 ? "the launcher's" : "the default");
    return kept;
}
int nxhost_touch_controls(nxhost_touch_control *out, int max) {
    pthread_mutex_lock(&g_mu); touch_once();
    int n = g_touch.n < max ? g_touch.n : max; if (out && n > 0) memcpy(out, g_touch.c, (size_t)n * sizeof *out);
    int all = g_touch.n;
    pthread_mutex_unlock(&g_mu);
    return all;
}
void nxhost_touch_controls_state(uint64_t *pressed, int max, float stick[4]) {
    pthread_mutex_lock(&g_mu); touch_once();
    for (int i = 0; pressed && i < max && i < g_touch.n; i++) pressed[i] = g_touch.pressed[i];
    if (stick) { stick[0] = g_touch.sx[0]; stick[1] = g_touch.sy[0]; stick[2] = g_touch.sx[1]; stick[3] = g_touch.sy[1]; }
    pthread_mutex_unlock(&g_mu);
}

// ---- Text input: the launcher's input box for the keyboard applet (front_text_input) ----
// The box takes the window's focus, so the game screen never gets the key-up of the pad button or the touch-up of the tap that opened it: everything the game saw held
// is let go when the box comes up, and pad and touch events are dropped until it goes (on the Mac the input box clears the keys the same way, front_mac.m), so nothing
// stays pressed and nothing typed or pressed in the box reaches the game. The guest clock keeps running, as on the Switch (hle_swkbd.c)
static nxhost_text_input_fn _Atomic g_ti_fn; static void *_Atomic g_ti_ctx;
void nxhost_text_input_callback(nxhost_text_input_fn fn, void *ctx) {
    atomic_store(&g_ti_ctx, ctx); atomic_store(&g_ti_fn, fn);
    LOG("input: the keyboard applet goes to the launcher's input box%s", fn ? "" : " (none: cancel)");
}
static void input_hold(int on) {
    pthread_mutex_lock(&g_mu);
    g_held = on;
    for (int p = 0; p < DM_PLAYERS; p++) if (pad_slot_mask(&g_slots) >> p & 1) { memset(&g_pad[p], 0, sizeof g_pad[p]); g_pad[p].map = pad_map(); publish_pad(p); }
    touch_release_all();
    pthread_mutex_unlock(&g_mu);
}
int android_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, int min_chars, char *out, int out_cap) {
    nxhost_text_input_fn fn = atomic_load(&g_ti_fn);
    if (!fn) return -1;
    input_hold(1);
    int r = fn(atomic_load(&g_ti_ctx), title ? title : "", sub ? sub : "", guide ? guide : "", initial ? initial : "", max_chars, min_chars, out, out_cap);
    input_hold(0);
    if (r > 0 && out_cap > 0) out[out_cap - 1] = 0;
    return r > 0 ? 1 : 0;
}

// ---- Motion: ASensorManager on a looper thread of its own ----
static pthread_mutex_t g_mo_mu = PTHREAD_MUTEX_INITIALIZER;   // start / stop
static pthread_t g_mo_th; static _Atomic int g_mo_run; static ALooper *_Atomic g_mo_looper; static int g_mo_on;
static _Atomic int g_rotation = 1;   // a phone held in landscape, its top to the left, until the launcher says
void nxhost_motion_rotation(int r) { atomic_store(&g_rotation, r & 3); }
static int env_off(const char *n) { const char *e = getenv(n); return e && !strcmp(e, "0"); }
static void *motion_thread(void *arg) {
    (void)arg; plat_thread_set_name("dm sensors");
    ALooper *lp = ALooper_prepare(0); atomic_store(&g_mo_looper, lp);
    ASensorManager *m = ASensorManager_getInstanceForPackage("dev.nxnative.host");
    const ASensor *acc = m ? ASensorManager_getDefaultSensor(m, ASENSOR_TYPE_ACCELEROMETER) : NULL, *gyr = m ? ASensorManager_getDefaultSensor(m, ASENSOR_TYPE_GYROSCOPE) : NULL;
    ASensorEventQueue *q = m ? ASensorManager_createEventQueue(m, lp, 1, NULL, NULL) : NULL;
    int gyro = gyr && !env_off("DM_MOTION") && !env_off("DM_PHONE_MOTION");
    if (!q || !acc) { LOG("[RED] input: no accelerometer (sensor manager %p): no phone motion", (void *)m); atomic_store(&g_mo_run, 0); }
    else {
        ASensorEventQueue_registerSensor(q, acc, 10000, 0);
        if (gyro) ASensorEventQueue_registerSensor(q, gyr, 10000, 0);
        LOG("input: phone sensors on: accelerometer for shakes%s", gyro ? ", gyroscope as player 1's six-axis sensor" : gyr ? " (gyroscope off by DM_MOTION / DM_PHONE_MOTION)" : " (no gyroscope)");
    }
    nxin_shake sh = {0}; const char *e = getenv("DM_SHAKE_G"); sh.thr = e && atof(e) > 0 ? (float)atof(e) : 1.5f;
    float a[3] = {0, 0, 9.80665f}; int have_a = 0;
    while (atomic_load(&g_mo_run)) {
        ALooper_pollOnce(200, NULL, NULL, NULL);
        ASensorEvent ev[32]; ssize_t n;
        while (q && (n = ASensorEventQueue_getEvents(q, ev, 32)) > 0) for (ssize_t i = 0; i < n; i++) {
            if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) {
                memcpy(a, ev[i].acceleration.v, sizeof a); have_a = 1;
                float g[3] = {-a[0] / 9.80665f, -a[1] / 9.80665f, -a[2] / 9.80665f};
                uint64_t want = nxin_shake_feed(&sh, g, (uint64_t)ev[i].timestamp) ? DM_BTN_SHAKE : 0;
                pthread_mutex_lock(&g_mu);
                if (want != g_shake_btn) { g_shake_btn = want; touch_once(); publish_touch_pad(); }
                pthread_mutex_unlock(&g_mu);
            } else if (ev[i].type == ASENSOR_TYPE_GYROSCOPE && have_a) {
                float acc_s[3], rot_s[3];
                nxin_motion_to_switch(atomic_load(&g_rotation), a, ev[i].vector.v, acc_s, rot_s);
                dm_motion_set_switch(0, acc_s, rot_s);
            }
        }
    }
    if (q) { if (acc) ASensorEventQueue_disableSensor(q, acc); if (gyro) ASensorEventQueue_disableSensor(q, gyr); ASensorManager_destroyEventQueue(m, q); }
    atomic_store(&g_mo_looper, NULL);
    return NULL;
}
bool nxhost_motion_start(void) {
    pthread_mutex_lock(&g_mo_mu);
    if (!g_mo_on) {
        atomic_store(&g_mo_run, 1);
        g_mo_on = !pthread_create(&g_mo_th, NULL, motion_thread, NULL);
    }
    int on = g_mo_on;
    pthread_mutex_unlock(&g_mo_mu);
    return on;
}
void nxhost_motion_stop(void) {
    pthread_mutex_lock(&g_mo_mu);
    if (g_mo_on) {
        atomic_store(&g_mo_run, 0);
        ALooper *lp = atomic_load(&g_mo_looper); if (lp) ALooper_wake(lp);
        pthread_join(g_mo_th, NULL); g_mo_on = 0;
        pthread_mutex_lock(&g_mu); if (g_shake_btn) { g_shake_btn = 0; touch_once(); publish_touch_pad(); } pthread_mutex_unlock(&g_mu);
    }
    pthread_mutex_unlock(&g_mo_mu);
}

// ---- Rumble: the host's snapshots (hle_haptics.h) go to a thread of their own, which calls the launcher ----
static pthread_mutex_t g_rb_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_rb_cv = PTHREAD_COND_INITIALIZER;
static nxhost_rumble_fn g_rb_fn; static void *g_rb_ctx;
static dm_haptic_state g_rb_state[DM_PLAYERS]; static uint32_t g_rb_dirty; static float g_rb_gain = 1;
static void rumble_sink(dm_haptic_state s) {   // under the haptics lock: copy and wake only
    if (s.player < 0 || s.player >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_rb_mu); g_rb_state[s.player] = s; g_rb_dirty |= 1u << s.player; pthread_cond_signal(&g_rb_cv); pthread_mutex_unlock(&g_rb_mu);
}
static void *rumble_thread(void *arg) {
    (void)arg; plat_thread_set_name("dm rumble");
    dm_haptic_level sent[DM_PLAYERS] = {0};
    for (;;) {
        pthread_mutex_lock(&g_rb_mu);
        while (!g_rb_dirty) pthread_cond_wait(&g_rb_cv, &g_rb_mu);
        uint32_t d = g_rb_dirty; g_rb_dirty = 0; dm_haptic_state st[DM_PLAYERS]; memcpy(st, g_rb_state, sizeof st);
        nxhost_rumble_fn fn = g_rb_fn; void *ctx = g_rb_ctx;
        pthread_mutex_unlock(&g_rb_mu);
        for (int p = 0; p < DM_PLAYERS; p++) {
            if (!(d & 1u << p)) continue;
            dm_haptic_level l = dm_haptics_level(st[p].motor, 2, g_rb_gain);
            l.intensity = roundf(l.intensity * 255) / 255; l.sharpness = roundf(l.sharpness * 32) / 32;   // the vibrator's amplitude steps: no call for a change it cannot show
            if (l.intensity == sent[p].intensity && (l.intensity == 0 || l.sharpness == sent[p].sharpness)) continue;
            sent[p] = l;
            if (fn) fn(ctx, p, l.intensity, l.sharpness);
        }
    }
    return NULL;
}
static void rumble_start(void) {
    const char *e = getenv("DM_RUMBLE_GAIN"); if (e) { float v = strtof(e, NULL); if (isfinite(v)) g_rb_gain = fmaxf(0, fminf(3, v)); }
    pthread_t t; if (!pthread_create(&t, NULL, rumble_thread, NULL)) pthread_detach(t);
    e = getenv("DM_RUMBLE"); dm_haptics_init(rumble_sink, !e || atoi(e) != 0);
    LOG("input: rumble goes to the launcher's vibrator callback%s", e && !atoi(e) ? " (off: DM_RUMBLE=0)" : "");
}
void nxhost_rumble_callback(nxhost_rumble_fn fn, void *ctx) {
    pthread_mutex_lock(&g_rb_mu); g_rb_fn = fn; g_rb_ctx = ctx; pthread_mutex_unlock(&g_rb_mu);
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (fn) pthread_once(&once, rumble_start);
}
void nxhost_rumble_route(int player, bool on) { dm_haptics_route(player, on); }

// ---- Lifecycle ----
static pthread_mutex_t g_lc_mu = PTHREAD_MUTEX_INITIALIZER;
// The app is suspended while it is in the background (onStop) or paused (the user's Pause, or the screen coming back with pause-on-return on: events 4 / 5): the guest clock
// frozen, the graphics drained and gated, the guest threads stopped (dm_guest_suspend), audio suspended, and the game told Background (focus 3, after OutOfFocus 2 as the
// system does). Coming out of it (neither left) the game is OutOfFocus (2), and InFocus (1) only once the activity is resumed as well
static int g_bg, g_intr, g_pause, g_susp, g_resumed;
static void audio_apply(void) { dm_audio_suspend(g_susp || g_intr); }
// CPU time of every thread of the process since the last call, from /proc/self/task/<tid>/stat (utime + stime): the busiest named, for the pause / resume lines
#define CPU_TIDS 512
static struct { int tid; uint64_t ticks; } g_cpu[CPU_TIDS]; static int g_ncpu; static uint64_t g_cpu_at;
static void cpu_line(const char *what) {
    static long hz; if (!hz) hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    struct { char name[20]; uint64_t dt; } top[6] = {{{0}}}; uint64_t total = 0, now = now_ns(); int n = 0, threads = 0;
    static struct { int tid; uint64_t ticks; } cur[CPU_TIDS];
    for (struct dirent *e; (e = readdir(d)) && n < CPU_TIDS;) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        char p[64], b[512]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(p, "r"); if (!f) continue;
        size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); b[k] = 0;
        char *o = strchr(b, '('), *c = strrchr(b, ')'); if (!o || !c) continue;
        char name[20]; size_t nl = (size_t)(c - o - 1) < sizeof name - 1 ? (size_t)(c - o - 1) : sizeof name - 1; memcpy(name, o + 1, nl); name[nl] = 0;
        unsigned long ut = 0, st = 0;   // fields 14 and 15, counted after the name: state is field 3
        if (sscanf(c + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
        uint64_t ticks = (uint64_t)ut + st, was = 0;
        for (int i = 0; i < g_ncpu; i++) if (g_cpu[i].tid == tid) { was = g_cpu[i].ticks; break; }
        uint64_t dt = ticks > was ? ticks - was : 0;
        cur[n].tid = tid; cur[n].ticks = ticks; n++; threads++; total += dt;
        for (int i = 0; i < 6; i++) if (dt > top[i].dt) { memmove(&top[i + 1], &top[i], (5 - i) * sizeof top[0]); snprintf(top[i].name, sizeof top[i].name, "%s", name); top[i].dt = dt; break; }
    }
    closedir(d);
    double wall = g_cpu_at ? (now - g_cpu_at) / 1e9 : 0;
    char tl[512]; size_t tlen = 0; tl[0] = 0;
    for (int i = 0; i < 6 && top[i].dt; i++) tlen += (size_t)snprintf(tl + tlen, sizeof tl - tlen, "%s'%s' %.2f s", i ? ", " : "", top[i].name, (double)top[i].dt / hz);
    if (g_cpu_at) LOG("[lifecycle] %s: threads' CPU since the last pause or resume %.2f s in %.1f s (%d threads)%s%s", what, (double)total / hz, wall, threads, tlen ? "; busiest: " : "", tl);
    else LOG("[lifecycle] %s: threads' CPU since the process started %.2f s (%d threads)%s%s", what, (double)total / hz, threads, tlen ? "; busiest: " : "", tl);   // the first line: no earlier reading
    memcpy(g_cpu, cur, (size_t)n * sizeof cur[0]); g_ncpu = n; g_cpu_at = now;
}
static void suspend_apply(const char *why) {
    int want = g_bg || g_pause;
    if (want == g_susp) return;
    g_susp = want;
    if (want) {
        dm_focus_set(2); dm_focus_set(3); audio_apply(); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); dm_guest_suspend(1); crash_watch_hold(1);
        cpu_line(why);
    } else {
        cpu_line(why);
        crash_watch_hold(0); dm_guest_suspend(0); dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); audio_apply(); dm_focus_set(2);
        if (g_resumed) dm_focus_set(1);
    }
}
void nxhost_audio_interrupt(bool on) {
    pthread_mutex_lock(&g_lc_mu);
    if (g_intr != on) { g_intr = on; LOG("audio: %s", on ? "focus lost (interruption)" : "focus back"); audio_apply(); dm_haptics_suspend(DM_HAPTICS_INTERRUPTED, on); }
    pthread_mutex_unlock(&g_lc_mu);
}
void nxhost_audio_route_changed(void) { LOG("audio: the output device changed; opening the streams again"); dm_audio_reset(); }
static void input_release(void) {   // what is held when the app leaves the screen is let go: the game must not see a button stuck down
    pthread_mutex_lock(&g_mu);
    for (int p = 0; p < DM_PLAYERS; p++) if (g_slots.owner[p]) { memset(&g_pad[p], 0, sizeof g_pad[p]); g_pad[p].map = pad_map(); dm_pad_set_player(p, 0, 0, 0, 0, 0); }
    touch_release_all();
    pthread_mutex_unlock(&g_mu);
}
void nxhost_lifecycle(int ev) {
    pthread_mutex_lock(&g_lc_mu);
    switch (ev) {
    case 0: g_resumed = 0; input_release(); if (!g_susp) dm_focus_set(2); dm_haptics_suspend(DM_HAPTICS_INACTIVE, 1); break;   // onPause
    case 1: g_bg = 1; suspend_apply(g_pause ? "background (paused)" : "background"); break;                                    // onStop
    case 2: g_bg = 0; suspend_apply("foreground"); if (g_pause) LOG("[lifecycle] foreground, paused: the game waits for a tap"); break;   // onStart
    case 3: g_resumed = 1; dm_haptics_suspend(DM_HAPTICS_INACTIVE, 0); if (!g_susp) dm_focus_set(1); break;                     // onResume
    case 4: if (!g_pause) { g_pause = 1; input_release(); suspend_apply("paused"); } break;                                   // Pause (the in-game panel, or the return)
    case 5: if (g_pause) { g_pause = 0; suspend_apply("continued"); } break;                                                    // the tap that continues
    default: LOG("[RED] nxhost_lifecycle: unknown event %d", ev);
    }
    pthread_mutex_unlock(&g_lc_mu);
}
int nxhost_paused(void) {
    pthread_mutex_lock(&g_lc_mu); int p = g_pause; pthread_mutex_unlock(&g_lc_mu);
    return p;
}

// Which decoders the phone has, for a bring-up log (input_android.h): created and deleted at once
void nxhost_codecs_log(void) {
    static const char *types[] = {"video/avc", "video/hevc", "video/x-vnd.on2.vp8", "video/x-vnd.on2.vp9", "video/av01", "audio/mp4a-latm"};
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        AMediaCodec *c = AMediaCodec_createDecoderByType(types[i]); char *name = NULL;
        if (c) AMediaCodec_getName(c, &name);
        LOG("codecs: %-20s %s", types[i], c ? (name ? name : "(unnamed)") : "none");
        if (c) { if (name) AMediaCodec_releaseName(c, name); AMediaCodec_delete(c); }
    }
}

// ---- Frame rate cap ----
void dm_fps_target_set(int t, const char *src); int dm_fps_target(void); int dm_fps_lock_rates(void); int nvn_game_fps(void); int nxhost_fps_limit_get(void);
void nxhost_fps_cap_set(int fps) { dm_fps_target_set(fps, "the launcher's in-game panel"); }
int nxhost_fps_cap_status(char *buf, int n) {
    int loaded = dm_main_build()[0] != 0, cap = dm_fps_target(), rates = loaded ? dm_fps_lock_rates() : 0;   // the limiters are looked for once, so only after main is loaded
    // the fourth: the rate the frame limit holds (hle_fpslock.c nxhost_fps_limit_get; a frame-step game holds 60, 30 or 20 only, so 45 and 40 run at 30), 0 = none
    if (buf && n > 0) snprintf(buf, (size_t)n, "%d %d %d %d", cap, rates, loaded ? nvn_game_fps() : 0, loaded ? nxhost_fps_limit_get() : 0);
    return cap;
}
