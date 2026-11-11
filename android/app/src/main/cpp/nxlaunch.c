// The launcher's JNI shim (dev.nxnative.android.Native): starts the host's nxhost_main on a thread of its own and carries the host's output to a log file and to logcat.
// It also hands the game screen's Surface, the input events and the Activity lifecycle on to the host, and runs the Vulkan selftest, through the C entry points libnxhost.so
// exports (dm/host/android/input_android.h, dm/gfx/vk_android/vk_android.h). They are weak, so a host without one still loads, and the call does nothing (android/README.md).
#define _GNU_SOURCE   // cpu_set_t and sched_setaffinity (thread placement)
#include <android/log.h>
#include <dirent.h>
#include <sched.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>

#define TAG "nxnative"
#define HOST_STACK (8u << 20)   // guest code runs on this thread (main.c guest_main): a deep stack, as the Mac's main thread has

int nxhost_main(int argc, char **argv);

// ---- The host's launcher interface: the headers are the host's own, so a signature change there fails here at compile time; every entry point is weak ----
#include "input_android.h"
#include "vk_android.h"
#pragma weak nxhost_env_changed
void nxhost_env_changed(void);
#pragma weak nxhost_set_window
#pragma weak nxhost_vk_selftest
#pragma weak nxhost_input_device_added
#pragma weak nxhost_input_device_removed
#pragma weak nxhost_input_key
#pragma weak nxhost_input_axes
#pragma weak nxhost_input_touch
#pragma weak nxhost_touch_layout
#pragma weak nxhost_touch_controls_enable
#pragma weak nxhost_touch_controls_set
#pragma weak nxhost_touch_controls_state
#pragma weak nxhost_motion_start
#pragma weak nxhost_motion_stop
#pragma weak nxhost_motion_rotation
#pragma weak nxhost_lifecycle
#pragma weak nxhost_paused
#pragma weak nxhost_audio_interrupt
#pragma weak nxhost_audio_route_changed
#pragma weak nxhost_fps
#pragma weak nxhost_overlay_set
#pragma weak nxhost_overlay_state
#pragma weak nxhost_framegen_set
#pragma weak nxhost_framegen_status
#pragma weak nxhost_picture_place
#pragma weak nxhost_fps_cap_set
#pragma weak nxhost_fps_cap_status
#pragma weak nxhost_input_device_player
#pragma weak nxhost_input_device_first
#pragma weak nxhost_rumble_callback
#pragma weak nxhost_rumble_route
#pragma weak nxhost_fps_limit_set
void nxhost_fps_limit_set(int fps);   // hle_fpslock.c: the frame limiter, live from the in-game panel
#pragma weak nxhost_fps_decouple_set
void nxhost_fps_decouple_set(int on);   // hle_fpslock.c: sead's controller at the measured rate, live from the in-game panel
#pragma weak nxhost_text_input_callback

// ---- Log: the host's stdout and stderr go straight into the log file (nothing is lost when the process dies), and a thread copies new lines to logcat ----
// At the end of the file the thread sleeps until the file is written (inotify IN_MODIFY), not on a 20 ms timer: the timer woke it 50 times a second for the whole game,
// though the host writes a few lines a second once a level runs (power: every wakeup of an idle core costs). Without inotify it falls back to the timer
static int g_log_fd = -1;
static char g_log_path[1024];
static void log_wait(int in) {
    if (in >= 0) { char ev[512]; if (read(in, ev, sizeof ev) > 0 || errno == EINTR) return; }   // blocks until a write (several events in one read)
    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
}
static void *logcat_tail(void *p) {
    (void)p; prctl(PR_SET_NAME, "nxlog");
    int fd = open(g_log_path, O_RDONLY | O_CLOEXEC); if (fd < 0) return NULL;
    int in = inotify_init1(IN_CLOEXEC);   // (blocking reads)
    if (in >= 0 && inotify_add_watch(in, g_log_path, IN_MODIFY) < 0) { close(in); in = -1; }
    char buf[8192]; size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
        if (n <= 0) { log_wait(in); continue; }   // (a write between the read and the wait leaves an event: the wait returns at once)
        have += (size_t)n; buf[have] = 0;
        char *s = buf, *e;
        while ((e = memchr(s, '\n', have - (size_t)(s - buf)))) {
            *e = 0;
            int prio = strstr(s, "FATAL") || strstr(s, "CRASH") ? ANDROID_LOG_ERROR : strstr(s, "[RED]") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
            __android_log_write(prio, TAG, s);
            s = e + 1;
        }
        have -= (size_t)(s - buf);
        if (have == sizeof buf - 1) { __android_log_write(ANDROID_LOG_INFO, TAG, buf); have = 0; }   // a line longer than the buffer goes out in pieces
        else memmove(buf, s, have);
    }
    return NULL;
}
static int log_open(const char *path) {
    if (g_log_fd >= 0) return 0;   // one log per process
    snprintf(g_log_path, sizeof g_log_path, "%s", path);
    g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (g_log_fd < 0) { __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot open the log %s: %s", path, strerror(errno)); return -1; }
    fflush(stdout); fflush(stderr);
    dup2(g_log_fd, 1); dup2(g_log_fd, 2);
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t; if (!pthread_create(&t, NULL, logcat_tail, NULL)) pthread_detach(t);
    return 0;
}

// ---- The run's exit record (ExitWatch.java): the launcher wrote "state=running" before the start; how the process ends is appended here, so the launcher can tell
// the player what happened. A native crash or an abort appends "state=native" from a signal handler installed before the host's own (crash.c on_crash prints the
// whole crash into the log and then hands the signal on to the handler that was there before: this one, which hands it on to debuggerd for the tombstone). Only
// async-signal-safe calls in the handler: open, write, close, sigaction, raise; the numbers are formatted by hand ----
static char g_exit_path[1024];
static _Atomic int g_exit_written;
static struct sigaction g_exit_prev[NSIG];
static const int g_exit_sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE};
static size_t rec_str(char *b, size_t n, const char *s) { while (*s) b[n++] = *s++; return n; }
static size_t rec_num(char *b, size_t n, unsigned long long v, int hex) {
    char t[24]; int k = 0;
    if (hex) { n = rec_str(b, n, "0x"); do { t[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); }
    else do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[n++] = t[--k];
    return n;
}
static void exit_append(const char *text, size_t len) {
    if (!g_exit_path[0]) return;
    int fd = open(g_exit_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, text, len) < 0) {}
    close(fd);
}
static void on_exit_signal(int sig, siginfo_t *si, void *uc_) {
    if (!atomic_exchange(&g_exit_written, 1)) {   // the first crashing thread only
        char b[160]; size_t n = 0;
        n = rec_str(b, n, "state=native\nsig="); n = rec_num(b, n, (unsigned)sig, 0);
        n = rec_str(b, n, "\naddr="); n = rec_num(b, n, (unsigned long long)(uintptr_t)si->si_addr, 1);
#if defined(__aarch64__)
        n = rec_str(b, n, "\npc="); n = rec_num(b, n, (unsigned long long)((ucontext_t *)uc_)->uc_mcontext.pc, 1);
#endif
        n = rec_str(b, n, "\n");
        exit_append(b, n);
    }
    // On to the handler before this one (debuggerd's): a fault returns and runs the instruction again into it, the others are raised again (as crash.c does)
    sigaction(sig, &g_exit_prev[sig], NULL);
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGTRAP || sig == SIGFPE) return;
    sigset_t un; sigemptyset(&un); sigaddset(&un, sig); pthread_sigmask(SIG_UNBLOCK, &un, NULL);
    raise(sig);
}
static void exit_record_install(const char *path) {
    if (!path || !*path || g_exit_path[0]) return;
    snprintf(g_exit_path, sizeof g_exit_path, "%s", path);
    struct sigaction sa = {.sa_sigaction = on_exit_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof g_exit_sigs / sizeof *g_exit_sigs; i++) sigaction(g_exit_sigs[i], &sa, &g_exit_prev[g_exit_sigs[i]]);
    fprintf(stderr, "[nxlaunch] exit record: %s\n", path);
}
// DM_TEST_CRASH=native (the test app only: the launcher drops the line in the play app and the release): a fault on a thread of its own 10 s after the start, to try the
// crash path and the launcher's notice
static void *test_crash_thread(void *p) {
    (void)p; prctl(PR_SET_NAME, "nxtestcrash");
    struct timespec ts = {10, 0}; nanosleep(&ts, NULL);
    fprintf(stderr, "[RED] [nxlaunch] DM_TEST_CRASH=native: faulting on purpose\n");
    *(volatile int *)(uintptr_t)8 = 1;
    return NULL;
}

// ---- The host thread ----
static struct { int argc; char **argv; } g_run;
static _Atomic int g_started;
static void *host_thread(void *p) {
    (void)p; prctl(PR_SET_NAME, "nxhost");
    fprintf(stderr, "[nxlaunch] nxhost_main starts on its own thread (%u MiB stack)\n", HOST_STACK >> 20);
    int rc = nxhost_main(g_run.argc, g_run.argv);
    fprintf(stderr, "[nxlaunch] nxhost_main returned %d: the game ended\n", rc);
    { char b[48]; size_t n = rec_str(b, 0, "state=host_exit\nrc="); if (rc < 0) { b[n++] = '-'; n = rec_num(b, n, (unsigned long long)-(long long)rc, 0); } else n = rec_num(b, n, (unsigned)rc, 0); b[n++] = '\n'; exit_append(b, n); }
    struct timespec ts = {0, 300000000}; nanosleep(&ts, NULL);   // logcat_tail's last lines
    _exit(rc);   // the host cannot be started twice in one process: the game process ends with it
}
static char *jstr(JNIEnv *env, jstring s) { const char *c = (*env)->GetStringUTFChars(env, s, NULL); char *r = strdup(c ? c : ""); if (c) (*env)->ReleaseStringUTFChars(env, s, c); return r; }

// ---- The keyboard applet's input box (nxhost_text_input_callback): TextInput.ask shows the dialog on the UI thread and blocks until it is confirmed. The guest thread that
// opened the keyboard does not enter the VM itself: a thread of this shim, attached for the call, makes it while the guest thread waits in pthread_join (guest threads
// carry the guest's state in their registers and slots, and one that ended while attached would abort the process). Strings cross as UTF-8 byte arrays: NewStringUTF
// takes modified UTF-8, which a character outside the BMP in the game's text would break ----
static JavaVM *g_vm;
static jclass g_ti_class; static jmethodID g_ti_ask;
typedef struct { const char *title, *sub, *guide, *initial; int max_chars, min_chars; char *out; int out_cap, ok; } text_req;
static jbyteArray jbytes(JNIEnv *env, const char *s) {
    jsize n = (jsize)strlen(s); jbyteArray a = (*env)->NewByteArray(env, n);
    if (a && n) (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)s);
    return a;
}
static void *text_input_thread(void *p) {
    text_req *q = p; JNIEnv *env = NULL;
    JavaVMAttachArgs a = {JNI_VERSION_1_6, "nxkeyboard", NULL};
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, &a) != JNI_OK) { fprintf(stderr, "[RED] [nxlaunch] keyboard: cannot attach to the VM: cancel\n"); return NULL; }
    jbyteArray t = jbytes(env, q->title), s = jbytes(env, q->sub), g = jbytes(env, q->guide), i = jbytes(env, q->initial);
    jbyteArray r = (*env)->CallStaticObjectMethod(env, g_ti_class, g_ti_ask, t, s, g, i, (jint)q->max_chars, (jint)q->min_chars);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); fprintf(stderr, "[RED] [nxlaunch] keyboard: TextInput.ask threw: cancel\n"); r = NULL; }
    if (r && q->out_cap > 0) {
        jsize n = (*env)->GetArrayLength(env, r); if (n > q->out_cap - 1) n = q->out_cap - 1;
        (*env)->GetByteArrayRegion(env, r, 0, n, (jbyte *)q->out); q->out[n] = 0; q->ok = 1;
    }
    (*g_vm)->DetachCurrentThread(g_vm);   // (frees the local references)
    return NULL;
}
static int text_input(void *ctx, const char *title, const char *sub, const char *guide, const char *initial, int max_chars, int min_chars, char *out, int out_cap) {
    (void)ctx; text_req q = {title, sub, guide, initial, max_chars, min_chars, out, out_cap, 0};
    pthread_t t; int e = pthread_create(&t, NULL, text_input_thread, &q);
    if (e) { fprintf(stderr, "[RED] [nxlaunch] keyboard: cannot start its thread: %s: cancel\n", strerror(e)); return 0; }
    pthread_join(t, NULL);
    return q.ok;
}
static void text_input_setup(JNIEnv *env) {
    if (!nxhost_text_input_callback) { fprintf(stderr, "[nxlaunch] keyboard: not in this host (the applet cancels)\n"); return; }
    jclass k = (*env)->FindClass(env, "dev/nxnative/android/TextInput");
    jmethodID m = k ? (*env)->GetStaticMethodID(env, k, "ask", "([B[B[B[BII)[B") : NULL;
    if (!m || (*env)->GetJavaVM(env, &g_vm) != JNI_OK) { (*env)->ExceptionClear(env); fprintf(stderr, "[RED] [nxlaunch] keyboard: no TextInput.ask: the applet cancels\n"); return; }
    g_ti_class = (*env)->NewGlobalRef(env, k); g_ti_ask = m;
    nxhost_text_input_callback(text_input, NULL);
}

// A surface can arrive while the readiness dialog is still up, before Native.start opens the log file and applies env.txt / env-<id>.txt (DM_VK_DRIVER, DM_VK_GPL,
// DM_VK_MESH, DM_VK_VALIDATE, ...). Hold the window here and hand it to nxhost_set_window only once Native.start has set the environment
static ANativeWindow *g_window;
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_win_ready;

// start(args, env, log path, cwd): args[0] is the program name (main.c reads args[1] as the pack folder), env is KEY=VALUE strings. 0 = started; the host runs on
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_start(JNIEnv *env, jclass cls, jobjectArray args, jobjectArray envs, jstring log, jstring cwd) {
    (void)cls;
    if (atomic_exchange(&g_started, 1)) return -1;
    char *lp = jstr(env, log); int lr = log_open(lp); free(lp);
    if (lr) return -2;
    for (jsize i = 0, n = (*env)->GetArrayLength(env, envs); i < n; i++) {   // the build first: every log says which build it came from
        char *kv = jstr(env, (jstring)(*env)->GetObjectArrayElement(env, envs, i));
        if (!strncmp(kv, "NX_BUILD_TAG=", 13)) fprintf(stderr, "[nxlaunch] build %s\n", kv + 13);
        free(kv);
    }
    for (jsize i = 0, n = (*env)->GetArrayLength(env, envs); i < n; i++) {
        char *kv = jstr(env, (jstring)(*env)->GetObjectArrayElement(env, envs, i)); char *eq = strchr(kv, '=');
        if (eq) { *eq = 0; setenv(kv, eq + 1, 1); fprintf(stderr, "[nxlaunch] env %s=%s\n", kv, eq + 1); }
        free(kv);
    }
    pthread_mutex_lock(&g_win_mu);
    g_win_ready = 1;
    if (g_window && nxhost_set_window) nxhost_set_window(g_window);
    pthread_mutex_unlock(&g_win_mu);
    exit_record_install(getenv("NX_EXIT_RECORD"));   // before the host thread: the host's crash handler (crash.c) takes this one as the handler before its own
    { const char *tc = getenv("DM_TEST_CRASH"); pthread_t ct; if (tc && !strcmp(tc, "native") && !pthread_create(&ct, NULL, test_crash_thread, NULL)) pthread_detach(ct); }
    if (nxhost_env_changed) nxhost_env_changed();   // the replay's cached switches read the environment again (some were read before it, with the window)
    char *wd = jstr(env, cwd); if (chdir(wd)) fprintf(stderr, "[nxlaunch] chdir %s: %s\n", wd, strerror(errno)); free(wd);
    g_run.argc = (*env)->GetArrayLength(env, args);
    g_run.argv = calloc((size_t)g_run.argc + 1, sizeof *g_run.argv);
    for (int i = 0; i < g_run.argc; i++) { g_run.argv[i] = jstr(env, (jstring)(*env)->GetObjectArrayElement(env, args, i)); fprintf(stderr, "[nxlaunch] arg %d: %s\n", i, g_run.argv[i]); }
    fprintf(stderr, "[nxlaunch] hooks: window %s, input %s, lifecycle %s\n", nxhost_set_window ? "yes" : "not in this build", nxhost_input_key ? "yes" : "not in this build",
            nxhost_lifecycle ? "yes" : "not in this build");
    text_input_setup(env);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, HOST_STACK);
    pthread_t t; int r = pthread_create(&t, &a, host_thread, NULL); pthread_attr_destroy(&a);
    if (r) { fprintf(stderr, "[nxlaunch] cannot start the host thread: %s\n", strerror(r)); return -3; }
    pthread_detach(t);
    return 0;
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_note(JNIEnv *env, jclass cls, jstring line) {   // a launcher line into the same log
    (void)cls; char *s = jstr(env, line); fprintf(stderr, "[nxlaunch] %s\n", s); free(s);
}

// ---- Surface, input and lifecycle, forwarded when the host has the entry points ----
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_setSurface(JNIEnv *env, jclass cls, jobject surface) {
    (void)cls;
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g_win_mu);
    if (g_win_ready && nxhost_set_window) nxhost_set_window(w);
    if (g_window) ANativeWindow_release(g_window);   // the host takes its own reference when it keeps the window
    g_window = w;
    pthread_mutex_unlock(&g_win_mu);
}
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_hasInput(JNIEnv *env, jclass cls) { (void)env; (void)cls; return nxhost_input_key != NULL; }
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_deviceAdded(JNIEnv *env, jclass cls, jint device, jstring name, jboolean labelled) {
    (void)cls; if (!nxhost_input_device_added) return -1;
    char *n = name ? jstr(env, name) : NULL; int p = nxhost_input_device_added(device, n, labelled); free(n); return p;
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_deviceRemoved(JNIEnv *env, jclass cls, jint device) { (void)env; (void)cls; if (nxhost_input_device_removed) nxhost_input_device_removed(device); }
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_key(JNIEnv *env, jclass cls, jint device, jint keycode, jboolean down) {
    (void)env; (void)cls; return nxhost_input_key ? nxhost_input_key(device, keycode, down) : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_axes(JNIEnv *env, jclass cls, jint device, jintArray axes, jfloatArray values) {
    (void)cls; if (!nxhost_input_axes) return JNI_FALSE;
    jsize n = (*env)->GetArrayLength(env, axes), m = (*env)->GetArrayLength(env, values); if (m < n) n = m; if (n > 64) n = 64;
    jint a[64]; jfloat v[64];
    (*env)->GetIntArrayRegion(env, axes, 0, n, a); (*env)->GetFloatArrayRegion(env, values, 0, n, v);
    return nxhost_input_axes(device, (const int *)a, v, n);
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_touch(JNIEnv *env, jclass cls, jint action, jint pointer, jfloat x, jfloat y) {
    (void)env; (void)cls; if (nxhost_input_touch) nxhost_input_touch(action, pointer, x, y);
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_touchLayout(JNIEnv *env, jclass cls, jfloat vw, jfloat vh, jfloat px, jfloat py, jfloat pw, jfloat ph) {
    (void)env; (void)cls; if (nxhost_touch_layout) nxhost_touch_layout(vw, vh, px, py, pw, ph);
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_picturePlace(JNIEnv *env, jclass cls, jfloat x, jfloat y) {
    (void)env; (void)cls; if (nxhost_picture_place) nxhost_picture_place(x, y);
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_touchControlsEnable(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls; if (nxhost_touch_controls_enable) nxhost_touch_controls_enable(on);
}
// The launcher's layout (TouchControlsView): six floats per control, kind, buttons, stick, x, y, r (input_android.h). The number the host kept; -1 = this host draws none
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_touchControlsSet(JNIEnv *env, jclass cls, jfloatArray a) {
    (void)cls; if (!nxhost_touch_controls_set || !nxhost_touch_controls_state) return -1;
    int n = a ? (*env)->GetArrayLength(env, a) / 6 : 0; if (n > NXT_MAX) n = NXT_MAX;
    jfloat f[NXT_MAX * 6]; nxhost_touch_control t[NXT_MAX]; memset(t, 0, sizeof t);
    if (n > 0) (*env)->GetFloatArrayRegion(env, a, 0, n * 6, f);
    for (int i = 0; i < n; i++) { const jfloat *o = f + 6 * i; t[i].kind = (int)o[0]; t[i].buttons = (uint64_t)o[1]; t[i].stick = (int)o[2]; t[i].x = o[3]; t[i].y = o[4]; t[i].r = o[5]; }
    return nxhost_touch_controls_set(n > 0 ? t : NULL, n);
}
// What each control holds now (pressed[i]) and the sticks' deflection (lx, ly, rx, ry; -1..1, y up), for drawing and the haptic tick
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_touchControlsState(JNIEnv *env, jclass cls, jlongArray pressed, jfloatArray stick) {
    (void)cls; if (!nxhost_touch_controls_state) return;
    uint64_t p[NXT_MAX] = {0}; float s[4] = {0};
    nxhost_touch_controls_state(p, NXT_MAX, s);
    if (pressed) { int n = (*env)->GetArrayLength(env, pressed); if (n > NXT_MAX) n = NXT_MAX; (*env)->SetLongArrayRegion(env, pressed, 0, n, (const jlong *)p); }
    if (stick && (*env)->GetArrayLength(env, stick) >= 4) (*env)->SetFloatArrayRegion(env, stick, 0, 4, s);
}
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_motionStart(JNIEnv *env, jclass cls) { (void)env; (void)cls; return nxhost_motion_start ? nxhost_motion_start() : JNI_FALSE; }
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_motionStop(JNIEnv *env, jclass cls) { (void)env; (void)cls; if (nxhost_motion_stop) nxhost_motion_stop(); }
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_motionRotation(JNIEnv *env, jclass cls, jint r) { (void)env; (void)cls; if (nxhost_motion_rotation) nxhost_motion_rotation(r); }
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_lifecycle(JNIEnv *env, jclass cls, jint ev) { (void)env; (void)cls; if (nxhost_lifecycle) nxhost_lifecycle(ev); }
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_paused(JNIEnv *env, jclass cls) { (void)env; (void)cls; return nxhost_paused ? nxhost_paused() : -1; }
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_audioInterrupt(JNIEnv *env, jclass cls, jboolean on) { (void)env; (void)cls; if (nxhost_audio_interrupt) nxhost_audio_interrupt(on); }
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_audioRouteChanged(JNIEnv *env, jclass cls) { (void)env; (void)cls; if (nxhost_audio_route_changed) nxhost_audio_route_changed(); }

// The overlay's frame rate (nxhost_fps): fills out (base fps, output fps, frame time ms, frame generation state, images per game frame, GPU ms a frame or -1); false = stale or not in this host
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_fps(JNIEnv *env, jclass cls, jfloatArray out) {
    (void)cls; if (!nxhost_fps || !out) return JNI_FALSE;
    float v[6] = {0, 0, 0, 0, 0, -1}; int n = (*env)->GetArrayLength(env, out); if (n > 6) n = 6;
    int r = nxhost_fps(v, n);
    (*env)->SetFloatArrayRegion(env, out, 0, n, v);
    return r ? JNI_TRUE : JNI_FALSE;
}

// The overlay's blocks for the present to draw into the game's images (nxhost_overlay_set): px a direct ByteBuffer of stride-byte rows (an ARGB_8888 bitmap's pixels), null
// empties the slot; false = the present does not draw it (or a host without it): the launcher's views show it. The state: 1 drawn, 0 not yet, -1 not drawn by the present
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_overlaySet(JNIEnv *env, jclass cls, jint slot, jobject px, jint w, jint h, jint stride) {
    (void)cls; if (!nxhost_overlay_set) return JNI_FALSE;
    const void *p = px ? (*env)->GetDirectBufferAddress(env, px) : NULL;
    if (p && (w <= 0 || h <= 0 || stride < w * 4 || (jlong)stride * h > (*env)->GetDirectBufferCapacity(env, px))) p = NULL;
    return nxhost_overlay_set(slot, p, p ? w : 0, p ? h : 0, stride) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_overlayState(JNIEnv *env, jclass cls) { (void)env; (void)cls; return nxhost_overlay_state ? nxhost_overlay_state() : -1; }

// Frame generation (nxhost_framegen_set / _status): the in-game settings, and the state with its reason as "<state> <reason>" (state 0 off, 1 generating, 2 on and not
// generating, -1 unavailable; "-2 ..." when this host has no frame generation)
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_frameGenSet(JNIEnv *env, jclass cls, jboolean on, jint mult, jint target, jint flow) {
    (void)env; (void)cls; if (nxhost_framegen_set) nxhost_framegen_set(on, mult, target, flow);
}
JNIEXPORT jstring JNICALL Java_dev_nxnative_android_Native_frameGenStatus(JNIEnv *env, jclass cls) {
    (void)cls; char buf[300], out[320];
    if (!nxhost_framegen_status) return (*env)->NewStringUTF(env, "-2 not in this host");
    int st = nxhost_framegen_status(buf, sizeof buf);
    snprintf(out, sizeof out, "%d %s", st, buf);
    return (*env)->NewStringUTF(env, out);
}

// The frame rate cap (nxhost_fps_cap_set / _status): "<cap> <rates> <game fps> <limit held>" (input_android.h), "-1 0 0" when this host has no cap
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_fpsCapSet(JNIEnv *env, jclass cls, jint fps) { (void)env; (void)cls; if (nxhost_fps_cap_set) nxhost_fps_cap_set(fps); }
JNIEXPORT jstring JNICALL Java_dev_nxnative_android_Native_fpsCapStatus(JNIEnv *env, jclass cls) {
    (void)cls; char buf[64] = "-1 0 0";
    if (nxhost_fps_cap_status) nxhost_fps_cap_status(buf, sizeof buf);
    return (*env)->NewStringUTF(env, buf);
}

// ---- The readiness check (Readiness.java): the Vulkan facts of the driver the game would run on (nxhost_vk_readiness), before the host starts. env is the game's
// environment as start will set it (Readiness.gameEnv: KEY=VALUE, the last of each winning as in start, without the paths start sets): all of it is set here, since the
// check makes the process's first Vulkan device and a driver reads some variables only then (Mesa's FD_DEV_FEATURES, GitHub #40). Null when this host has no such entry point ----
#pragma weak nxhost_vk_readiness
JNIEXPORT jstring JNICALL Java_dev_nxnative_android_Native_readiness(JNIEnv *env, jclass cls, jobjectArray envs) {
    (void)cls; if (!nxhost_vk_readiness) return NULL;
    for (jsize i = 0, n = envs ? (*env)->GetArrayLength(env, envs) : 0; i < n; i++) {
        char *kv = jstr(env, (jstring)(*env)->GetObjectArrayElement(env, envs, i)); char *eq = strchr(kv, '=');
        if (eq && eq > kv) { *eq = 0; setenv(kv, eq + 1, 1); }
        free(kv);
    }
    size_t cap = 16384; char *buf = calloc(1, cap);
    if (!buf) return NULL;
    nxhost_vk_readiness(buf, (int)cap);
    jstring s = (*env)->NewStringUTF(env, buf); free(buf);
    return s;
}

// ---- The graphics selftest (SelftestActivity, in a process of its own): no host start, only the log and nxhost_vk_selftest ----
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_openLog(JNIEnv *env, jclass cls, jstring log) {
    (void)cls; char *lp = jstr(env, log); int r = log_open(lp); free(lp); return r;
}
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_hasSelftest(JNIEnv *env, jclass cls) { (void)env; (void)cls; return nxhost_vk_selftest != NULL; }
// Blocks for about frames / 60 s: call it on a thread of its own. 0 = every check passed; -1 = this host has no selftest
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_selftest(JNIEnv *env, jclass cls, jobject surface, jint frames) {
    (void)cls; if (!nxhost_vk_selftest) return -1;
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    // The selftest never calls Native.start: let setSurface forward its resizes and the null on leaving to the host, as start does for a game
    pthread_mutex_lock(&g_win_mu); g_win_ready = 1; pthread_mutex_unlock(&g_win_mu);
    fprintf(stderr, "[nxlaunch] graphics selftest: nxhost_vk_selftest(%p, %d)\n", (void *)w, frames);
    int r = nxhost_vk_selftest(w, frames);
    fprintf(stderr, "[nxlaunch] graphics selftest returned %d: %s\n", r, r ? "FAIL" : "PASS");
    if (w) ANativeWindow_release(w);   // the replay holds its own reference
    return r;
}

JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_devicePlayer(JNIEnv *env, jclass cls, jint device) {
    (void)env; (void)cls; return nxhost_input_device_player ? nxhost_input_device_player(device) : -1;
}
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_deviceFirst(JNIEnv *env, jclass cls, jint device, jstring name) {   // PadOrder: this device is player 1
    (void)cls; if (!nxhost_input_device_first) return -1;
    char *n = name ? jstr(env, name) : NULL; int p = nxhost_input_device_first(device, n); free(n); return p;
}

// ---- Rumble: the host calls rumble() on its rumble thread for each change of a player's vibration; it goes to listener.onRumble(player, intensity, sharpness) (Rumble.java),
// which drives the controller's or the phone's vibrator. The thread is attached to the VM on its first call and stays attached for the process's life ----
static JavaVM *g_vm;
static jobject g_rumble;   // a global reference to the listener
static jmethodID g_on_rumble;
static void rumble(void *ctx, int player, float intensity, float sharpness) {
    (void)ctx;
    static __thread JNIEnv *env;
    if (!env && (*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) { env = NULL; return; }
    (*env)->CallVoidMethod(env, g_rumble, g_on_rumble, (jint)player, (jfloat)intensity, (jfloat)sharpness);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
// Once, before Native.start: the host starts its rumble thread when the callback is set. false when this host has no rumble
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_setRumbleListener(JNIEnv *env, jclass cls, jobject listener) {
    (void)cls; if (!nxhost_rumble_callback || !listener || g_rumble) return JNI_FALSE;
    jmethodID m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, listener), "onRumble", "(IFF)V");
    if (!m) { (*env)->ExceptionClear(env); return JNI_FALSE; }
    (*env)->GetJavaVM(env, &g_vm);
    g_on_rumble = m;
    g_rumble = (*env)->NewGlobalRef(env, listener);
    nxhost_rumble_callback(rumble, NULL);
    return JNI_TRUE;
}
// Whether a player has a vibrator: the host sends that player's vibration only while it has one
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_rumbleRoute(JNIEnv *env, jclass cls, jint player, jboolean on) {
    (void)env; (void)cls; if (nxhost_rumble_route) nxhost_rumble_route(player, on);
}

// ---- Thread placement (Native.pinThreads, every few seconds while the game runs): the replay thread (vk-replay, the most work a frame: 9-18 ms in DKB) gets the
// fastest cores to itself, and the command encoder (vk-enc: 5-8 ms of work, the rest spinning for the next call, which only heats the core it is on) and the game's own
// threads (named "0x<id> ...") go to the cores at 75 % of the top clock or more, so none of them runs on the small cores; without such cores they share the fastest. The clusters are the cores' top clocks (cpuinfo_max_freq). Android may reset a thread to all cores,
// and each call places the ones not where they belong. Returns the number placed by this call ----
static long cpu_max_khz(int c) {
    char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
    FILE *f = fopen(path, "r"); long v = 0; if (f) { if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); }
    return v;
}
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_pinThreads(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    static cpu_set_t fast, mid; static int ready;
    if (!ready) {
        int n = (int)sysconf(_SC_NPROCESSORS_CONF); if (n > CPU_SETSIZE) n = CPU_SETSIZE;
        long top = 0;
        for (int c = 0; c < n; c++) { long k = cpu_max_khz(c); if (k > top) top = k; }
        if (!top) return -1;   // no cpufreq: leave the scheduler alone
        // Fast: the cores at the top clock. Middle: the others at 75 % of it or more (a mid cluster: 2.8 of 3.19 GHz on a Snapdragon 8 Gen 2, 3.53 of 4.32 on an
        // 8 Elite), never the small cores (1.8 of 2.8 on an older big.LITTLE). Without a middle group the encoder and the game share the fast cores
        CPU_ZERO(&fast); CPU_ZERO(&mid);
        for (int c = 0; c < n; c++) { long k = cpu_max_khz(c); if (k == top) CPU_SET(c, &fast); else if (k * 4 >= top * 3) CPU_SET(c, &mid); }
        if (!CPU_COUNT(&mid)) mid = fast;
        ready = 1;
        fprintf(stderr, "[nxlaunch] thread placement: the replay thread on %d core(s) at %ld MHz, the encoder and the game on %d%s\n",
                CPU_COUNT(&fast), top / 1000, CPU_COUNT(&mid), CPU_EQUAL(&mid, &fast) ? " (the same: no middle cluster)" : "");
    }
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    int placed = 0; struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        char path[64], name[32] = ""; snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0; fclose(f);
        name[strcspn(name, "\n")] = 0;
        int g = !strcmp(name, "vk-replay") ? 1 : !strcmp(name, "vk-enc") || !strncmp(name, "0x", 2) ? 2 : 0;
        if (!g) continue;   // not a placed thread; looked at again next time (a thread may be renamed after it starts)
        // Checked every call, not once: the replay thread starts under the name of the game thread that made it, and Android resets every thread to all cores
        // when it moves the app between foreground and background
        const cpu_set_t *to = g == 1 ? &fast : &mid; cpu_set_t cur;
        if (!sched_getaffinity(tid, sizeof cur, &cur) && CPU_EQUAL(&cur, to)) continue;
        if (!sched_setaffinity(tid, sizeof *to, to)) placed++;   // fails while a core is parked (core control at low load): the next call places it
    }
    closedir(d);
    return placed;
}
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_fpsLimitSet(JNIEnv *env, jclass cls, jint fps) { (void)env; (void)cls; if (nxhost_fps_limit_set) nxhost_fps_limit_set(fps); }
// Full game speed at low frame rates, live from the in-game panel (Native.decoupleSet)
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_decoupleSet(JNIEnv *env, jclass cls, jboolean on) { (void)env; (void)cls; if (nxhost_fps_decouple_set) nxhost_fps_decouple_set(on ? 1 : 0); }
// Game resolution of a game that sizes its scenes by the graphics scale, live from the in-game panel (Native.graphicsModeSet): the host changes the scale and operation mode
// and posts the console's notifications (300, 30 / 31), which the game answers by querying them again
#pragma weak nxhost_graphics_mode_set
void nxhost_graphics_mode_set(int scale, int opmode);   // hle_oe.c: -1 = the game's own
JNIEXPORT jboolean JNICALL Java_dev_nxnative_android_Native_graphicsModeSet(JNIEnv *env, jclass cls, jint scale, jint opmode) {
    (void)env; (void)cls; if (!nxhost_graphics_mode_set) return JNI_FALSE;
    nxhost_graphics_mode_set(scale, opmode); return JNI_TRUE;
}
// Dynamic resolution, live from the in-game panel (Native.drsSet)
#pragma weak nxhost_drs_set
void nxhost_drs_set(int min_pct, int max_pct);   // probe.c: Donkey Kong Bananza's dynamic resolution range, % of the window (0 = the game's own)
JNIEXPORT void JNICALL Java_dev_nxnative_android_Native_drsSet(JNIEnv *env, jclass cls, jint lo, jint hi) { (void)env; (void)cls; if (nxhost_drs_set) nxhost_drs_set(lo, hi); }
// The in-game panel's Upscaler rows (SidePanel, GameUpscaler.shown): whether the running game dispatched a network DM_VK_NDSR can replace; -1 when this host cannot say
#pragma weak nxhost_ndsr_status
int nxhost_ndsr_status(void);   // vka_draw.c
JNIEXPORT jint JNICALL Java_dev_nxnative_android_Native_ndsrStatus(JNIEnv *env, jclass cls) { (void)env; (void)cls; return nxhost_ndsr_status ? nxhost_ndsr_status() : -1; }
