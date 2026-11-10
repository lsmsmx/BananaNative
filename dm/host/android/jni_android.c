// JNI bindings of input_android.h and nxhost_main for the launcher's Kotlin class (android/PORTING.md, "Launcher interface"). JNI_OnLoad registers them on
// NXHOST_JNI_CLASS when the APK has that class; without it nothing is bound and the C functions stay callable from another JNI library
#include "../dm.h"
#include "input_android.h"
#include <jni.h>
#include <stdatomic.h>

#ifndef NXHOST_JNI_CLASS
#define NXHOST_JNI_CLASS "dev/nxnative/NxHost"
#endif
static JavaVM *g_vm;

int nxhost_main(int argc, char **argv);   // front_android.c
static jint j_main(JNIEnv *env, jclass c, jobjectArray args) {
    (void)c; int n = args ? (*env)->GetArrayLength(env, args) : 0;
    char **argv = calloc((size_t)n + 2, sizeof *argv); argv[0] = strdup("nxhost");
    for (int i = 0; i < n; i++) {
        jstring s = (*env)->GetObjectArrayElement(env, args, i); const char *u = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
        argv[i + 1] = strdup(u ? u : ""); if (u) (*env)->ReleaseStringUTFChars(env, s, u); if (s) (*env)->DeleteLocalRef(env, s);
    }
    return nxhost_main(n + 1, argv);   // argv stays: the host keeps pointers into it
}
static jint j_device_added(JNIEnv *env, jclass c, jint dev, jstring name, jboolean labelled) {
    (void)c; const char *u = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    int p = nxhost_input_device_added(dev, u, labelled); if (u) (*env)->ReleaseStringUTFChars(env, name, u); return p;
}
static void j_device_removed(JNIEnv *env, jclass c, jint dev) { (void)env; (void)c; nxhost_input_device_removed(dev); }
static jint j_device_player(JNIEnv *env, jclass c, jint dev) { (void)env; (void)c; return nxhost_input_device_player(dev); }
static jint j_device_first(JNIEnv *env, jclass c, jint dev, jstring name) {
    (void)c; const char *u = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    int p = nxhost_input_device_first(dev, u); if (u) (*env)->ReleaseStringUTFChars(env, name, u); return p;
}
static jboolean j_key(JNIEnv *env, jclass c, jint dev, jint key, jboolean down) { (void)env; (void)c; return nxhost_input_key(dev, key, down); }
static jboolean j_axes(JNIEnv *env, jclass c, jint dev, jintArray axes, jfloatArray values) {
    (void)c; if (!axes || !values) return JNI_FALSE;
    int n = (*env)->GetArrayLength(env, axes), m = (*env)->GetArrayLength(env, values); if (m < n) n = m; if (n > 64) n = 64;
    jint a[64]; jfloat v[64];
    (*env)->GetIntArrayRegion(env, axes, 0, n, a); (*env)->GetFloatArrayRegion(env, values, 0, n, v);
    return nxhost_input_axes(dev, (const int *)a, v, n);
}
static void j_deadzone(JNIEnv *env, jclass c, jfloat dz) { (void)env; (void)c; nxhost_input_deadzone(dz); }
static void j_touch(JNIEnv *env, jclass c, jint action, jint id, jfloat x, jfloat y) { (void)env; (void)c; nxhost_input_touch(action, id, x, y); }
static void j_touch_layout(JNIEnv *env, jclass c, jfloat vw, jfloat vh, jfloat px, jfloat py, jfloat pw, jfloat ph) { (void)env; (void)c; nxhost_touch_layout(vw, vh, px, py, pw, ph); }
static void j_touch_enable(JNIEnv *env, jclass c, jboolean on) { (void)env; (void)c; nxhost_touch_controls_enable(on); }
// Six floats per control: kind, buttons (DM_BTN_* bits, at most bit 15: exact in a float), stick, x, y, r
static jfloatArray j_touch_controls(JNIEnv *env, jclass c) {
    (void)c; nxhost_touch_control t[NXT_MAX]; int n = nxhost_touch_controls(t, NXT_MAX); if (n > NXT_MAX) n = NXT_MAX;
    jfloat f[NXT_MAX * 6];
    for (int i = 0; i < n; i++) { jfloat *o = f + 6 * i; o[0] = (jfloat)t[i].kind; o[1] = (jfloat)t[i].buttons; o[2] = (jfloat)t[i].stick; o[3] = t[i].x; o[4] = t[i].y; o[5] = t[i].r; }
    jfloatArray a = (*env)->NewFloatArray(env, n * 6); if (a) (*env)->SetFloatArrayRegion(env, a, 0, n * 6, f);
    return a;
}
// The launcher's own layout, in touchControls' six floats per control (labels left empty: the launcher draws its own); null or empty: the default layout
static jint j_touch_set(JNIEnv *env, jclass c, jfloatArray a) {
    (void)c; int n = a ? (*env)->GetArrayLength(env, a) / 6 : 0; if (n > NXT_MAX) n = NXT_MAX;
    jfloat f[NXT_MAX * 6]; nxhost_touch_control t[NXT_MAX]; memset(t, 0, sizeof t);
    if (n > 0) (*env)->GetFloatArrayRegion(env, a, 0, n * 6, f);
    for (int i = 0; i < n; i++) { const jfloat *o = f + 6 * i; t[i].kind = (int)o[0]; t[i].buttons = (uint64_t)o[1]; t[i].stick = (int)o[2]; t[i].x = o[3]; t[i].y = o[4]; t[i].r = o[5]; }
    return nxhost_touch_controls_set(n > 0 ? t : NULL, n);
}
static jobjectArray j_touch_labels(JNIEnv *env, jclass c) {
    (void)c; nxhost_touch_control t[NXT_MAX]; int n = nxhost_touch_controls(t, NXT_MAX); if (n > NXT_MAX) n = NXT_MAX;
    jobjectArray a = (*env)->NewObjectArray(env, n, (*env)->FindClass(env, "java/lang/String"), NULL);
    for (int i = 0; a && i < n; i++) { jstring s = (*env)->NewStringUTF(env, t[i].label); (*env)->SetObjectArrayElement(env, a, i, s); (*env)->DeleteLocalRef(env, s); }
    return a;
}
static void j_touch_state(JNIEnv *env, jclass c, jlongArray pressed, jfloatArray stick) {
    (void)c; uint64_t p[NXT_MAX] = {0}; float s[4];
    nxhost_touch_controls_state(p, NXT_MAX, s);
    if (pressed) { int n = (*env)->GetArrayLength(env, pressed); if (n > NXT_MAX) n = NXT_MAX; (*env)->SetLongArrayRegion(env, pressed, 0, n, (const jlong *)p); }
    if (stick && (*env)->GetArrayLength(env, stick) >= 4) (*env)->SetFloatArrayRegion(env, stick, 0, 4, s);
}
static jboolean j_motion_start(JNIEnv *env, jclass c) { (void)env; (void)c; return nxhost_motion_start(); }
static void j_motion_stop(JNIEnv *env, jclass c) { (void)env; (void)c; nxhost_motion_stop(); }
static void j_motion_rotation(JNIEnv *env, jclass c, jint r) { (void)env; (void)c; nxhost_motion_rotation(r); }

// Rumble: listener.onRumble(player: Int, intensity: Float, sharpness: Float), called on the host's rumble thread (attached to the VM once, for its life)
static jobject _Atomic g_listener; static jmethodID g_on_rumble;
static void rumble(void *ctx, int player, float intensity, float sharpness) {
    (void)ctx; jobject l = atomic_load(&g_listener); if (!l || !g_vm) return;
    JNIEnv *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK && (*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
    (*env)->CallVoidMethod(env, l, g_on_rumble, (jint)player, (jfloat)intensity, (jfloat)sharpness);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
}
static void j_rumble_listener(JNIEnv *env, jclass c, jobject l) {
    (void)c;
    if (l) {
        jclass k = (*env)->GetObjectClass(env, l); jmethodID m = (*env)->GetMethodID(env, k, "onRumble", "(IFF)V");
        if (!m) { (*env)->ExceptionClear(env); LOG("[RED] rumble: the listener has no onRumble(Int, Float, Float)"); return; }
        g_on_rumble = m;
        jobject old = atomic_exchange(&g_listener, (*env)->NewGlobalRef(env, l)); if (old) (*env)->DeleteGlobalRef(env, old);
        nxhost_rumble_callback(rumble, NULL);
    } else { jobject old = atomic_exchange(&g_listener, NULL); if (old) (*env)->DeleteGlobalRef(env, old); }   // the callback stays installed and finds no listener
}
static void j_rumble_route(JNIEnv *env, jclass c, jint p, jboolean on) { (void)env; (void)c; nxhost_rumble_route(p, on); }
static void j_lifecycle(JNIEnv *env, jclass c, jint ev) { (void)env; (void)c; nxhost_lifecycle(ev); }
static void j_audio_interrupt(JNIEnv *env, jclass c, jboolean on) { (void)env; (void)c; nxhost_audio_interrupt(on); }
static void j_audio_route(JNIEnv *env, jclass c) { (void)env; (void)c; nxhost_audio_route_changed(); }
static void j_codecs_log(JNIEnv *env, jclass c) { (void)env; (void)c; nxhost_codecs_log(); }

static const JNINativeMethod k_methods[] = {
    {"run", "([Ljava/lang/String;)I", (void *)j_main},
    {"deviceAdded", "(ILjava/lang/String;Z)I", (void *)j_device_added},
    {"deviceRemoved", "(I)V", (void *)j_device_removed},
    {"devicePlayer", "(I)I", (void *)j_device_player},
    {"deviceFirst", "(ILjava/lang/String;)I", (void *)j_device_first},
    {"key", "(IIZ)Z", (void *)j_key},
    {"axes", "(I[I[F)Z", (void *)j_axes},
    {"deadzone", "(F)V", (void *)j_deadzone},
    {"touch", "(IIFF)V", (void *)j_touch},
    {"touchLayout", "(FFFFFF)V", (void *)j_touch_layout},
    {"touchControlsEnable", "(Z)V", (void *)j_touch_enable},
    {"touchControls", "()[F", (void *)j_touch_controls},
    {"touchControlLabels", "()[Ljava/lang/String;", (void *)j_touch_labels},
    {"touchControlsSet", "([F)I", (void *)j_touch_set},
    {"touchControlsState", "([J[F)V", (void *)j_touch_state},
    {"motionStart", "()Z", (void *)j_motion_start},
    {"motionStop", "()V", (void *)j_motion_stop},
    {"motionRotation", "(I)V", (void *)j_motion_rotation},
    {"setRumbleListener", "(Ljava/lang/Object;)V", (void *)j_rumble_listener},
    {"rumbleRoute", "(IZ)V", (void *)j_rumble_route},
    {"lifecycle", "(I)V", (void *)j_lifecycle},
    {"audioInterrupt", "(Z)V", (void *)j_audio_interrupt},
    {"audioRouteChanged", "()V", (void *)j_audio_route},
    {"codecsLog", "()V", (void *)j_codecs_log},
};
__attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved; g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;
    jclass k = (*env)->FindClass(env, NXHOST_JNI_CLASS);
    // Not an error: the nxnative launcher has no such class and calls the C functions of input_android.h from its own JNI library (android/app/src/main/cpp/nxlaunch.c)
    if (!k) { (*env)->ExceptionClear(env); LOG("JNI: no class %s: nothing bound; the launcher calls the C functions of input_android.h", NXHOST_JNI_CLASS); return JNI_VERSION_1_6; }
    // Each method on its own: a class that declares only some of them still gets those
    int bound = 0;
    for (size_t i = 0; i < sizeof k_methods / sizeof *k_methods; i++) {
        if ((*env)->RegisterNatives(env, k, &k_methods[i], 1) == JNI_OK) bound++;
        else (*env)->ExceptionClear(env);
    }
    LOG("JNI: %d of %zu natives bound on %s", bound, sizeof k_methods / sizeof *k_methods, NXHOST_JNI_CLASS);
    return JNI_VERSION_1_6;
}
