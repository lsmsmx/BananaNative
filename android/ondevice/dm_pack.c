// The on-device packer's Python start-up and tool runner (dm_pack.h, android/ONDEVICE.md): CPython 3.14 embedded, each tool run by runpy.run_path with its sys.argv.
// Build: android/ondevice/build.sh (NDK clang, linked against the official CPython Android build's libpython3.14.so)
#define _GNU_SOURCE   // cpu_set_t and sched_setaffinity (thread placement)
#include "dm_pack.h"
#include <Python.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int started;
static PyObject *runner;   // _dm_pack.run
// The run in hand, for dm_pack_interrupt: its thread's Python ident (0 when no run), whether an interrupt was asked for during it
static pthread_mutex_t ilock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long run_tid;
static int interrupted;

// The runner, in Python: what `python3 <tool> args` does, in this process, undone after the run (argv, path, environment, cwd, streams, the tools' modules)
static const char RUNNER[] =
"import os, runpy, signal, sys, traceback\n"
"_signal, _execs = signal.signal, {k: getattr(os, k) for k in ('fork', 'execv', 'execve')}\n"
"def _no_signal(s, h):   # signal handlers are the app's: a tool's (patch_nso.py's SIGTERM) is not installed, which also lets it run off Python's main thread\n"
"    return signal.getsignal(s)\n"
"def _no_exec(*a, **k):   # a tool must not fork or replace the process (it is the app's)\n"
"    raise OSError('no fork or exec in the packer process (android/ONDEVICE.md)')\n"
"def _open(log):   # appended to; /dev/stdout (dm_pack_cli) is the process's fd 1 as it is (adb shell's terminal cannot be opened again by its path)\n"
"    kw = dict(buffering=1, encoding='utf-8', errors='backslashreplace')\n"
"    return open(1, 'w', closefd=False, **kw) if log == '/dev/stdout' else open(log, 'a', **kw)\n"
"def _reason(e):   # a run that ends with an exception: one [RED] line after its traceback, the reason the step's card shows (PackService's Tail reads only [RED] lines)\n"
"    msg = (str(e).strip().splitlines() or [''])[0]\n"
"    tb = traceback.extract_tb(e.__traceback__)\n"
"    at = f' ({os.path.basename(tb[-1].filename)}:{tb[-1].lineno})' if tb else ''\n"
"    return f'[RED] {type(e).__name__}' + (f': {msg}' if msg else '') + at\n"
"def run(tool, args, env, log):\n"
"    saved = (sys.argv, list(sys.path), dict(os.environ), set(sys.modules), sys.stdout, sys.stderr, os.getcwd())\n"
"    f = _open(log) if log else None\n"
"    signal.signal = _no_signal\n"
"    for k in _execs: setattr(os, k, _no_exec)\n"
"    if f: sys.stdout = sys.stderr = f\n"
"    code = 0\n"
"    try:\n"
"        for kv in env or ():\n"
"            k, _, v = kv.partition('='); os.environ[k] = v\n"
"        sys.argv = [tool] + list(args)\n"
"        sys.path.insert(0, os.path.dirname(os.path.abspath(tool)))\n"
"        runpy.run_path(tool, run_name='__main__')\n"
"    except SystemExit as e:\n"
"        c = e.code\n"
"        if c is None: code = 0\n"
"        elif isinstance(c, int): code = c\n"
"        else:   # sys.exit(<text>): the text is why it stopped, a [RED] line as red() writes it\n"
"            c = str(c); print(c if c.startswith('[RED] ') else '[RED] ' + c, file=sys.stderr); code = 1\n"
"    except KeyboardInterrupt:   # dm_pack_interrupt\n"
"        print('[dm_pack] interrupted', file=sys.stderr); code = 130\n"
"    except BaseException as e:   # an uncaught exception: its traceback, then its type and message as a [RED] line (MemoryError too: the card's out-of-memory text)\n"
"        code = 1\n"
"        try: traceback.print_exc()\n"
"        except BaseException: pass\n"
"        try: print(_reason(e), file=sys.stderr)\n"
"        except BaseException: pass\n"
"    finally:\n"
"        try: sys.stdout.flush(); sys.stderr.flush()\n"
"        except Exception: pass\n"
"        argv, path, environ, mods, out, err, cwd = saved\n"
"        sys.argv, sys.path[:], sys.stdout, sys.stderr = argv, path, out, err\n"
"        signal.signal = _signal\n"
"        for k, v in _execs.items(): setattr(os, k, v)\n"
"        for k in set(os.environ) - set(environ): del os.environ[k]\n"
"        for k, v in environ.items():\n"
"            if os.environ.get(k) != v: os.environ[k] = v\n"
"        os.chdir(cwd)\n"
"        base = os.path.realpath(sys.prefix) + os.sep\n"
"        for m in set(sys.modules) - mods:\n"
"            fn = getattr(sys.modules.get(m), '__file__', None) or ''\n"
"            if not os.path.realpath(fn).startswith(base): sys.modules.pop(m, None)\n"
"        if f: f.close()\n"
"    return code\n";

int dm_pack_init(const char *home) {
    pthread_mutex_lock(&lock);
    if (started) { pthread_mutex_unlock(&lock); return 0; }
    PyConfig c; PyStatus st;
    PyConfig_InitIsolatedConfig(&c);   // no PYTHON* variables, no user site, no current folder on sys.path
    c.install_signal_handlers = 0;     // the app's runtime owns the signals
    c.site_import = 1;                 // site-packages (capstone, lz4)
    st = PyConfig_SetBytesString(&c, &c.home, home);
    if (!PyStatus_Exception(st)) st = PyConfig_SetBytesString(&c, &c.program_name, "python3");
    if (!PyStatus_Exception(st)) st = Py_InitializeFromConfig(&c);
    PyConfig_Clear(&c);
    if (PyStatus_Exception(st)) {
        fprintf(stderr, "[RED] dm_pack_init: Python did not start (home %s): %s\n", home, st.err_msg ? st.err_msg : "?");
        pthread_mutex_unlock(&lock); return -1;
    }
    PyObject *m = PyImport_AddModule("_dm_pack"), *d = m ? PyModule_GetDict(m) : NULL;
    PyObject *r = d ? PyRun_String(RUNNER, Py_file_input, d, d) : NULL;
    runner = r ? PyDict_GetItemString(d, "run") : NULL;
    Py_XINCREF(runner); Py_XDECREF(r);
    if (!runner) { PyErr_Print(); PyEval_SaveThread(); pthread_mutex_unlock(&lock); return -1; }
    PyEval_SaveThread();   // release the GIL: runs take it from whichever thread calls
    started = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

int dm_pack_run(const char *tool, int argc, char **argv, const char *const *env, const char *log_path) {
    pthread_mutex_lock(&lock);
    if (!started) { pthread_mutex_unlock(&lock); fprintf(stderr, "[RED] dm_pack_run before dm_pack_init\n"); return -1; }
    PyGILState_STATE g = PyGILState_Ensure();
    int code = 1;
    unsigned long tid = PyThread_get_thread_ident();
    pthread_mutex_lock(&ilock); run_tid = tid; interrupted = 0; pthread_mutex_unlock(&ilock);
    PyObject *args = PyList_New(0), *envl = PyList_New(0);
    for (int i = 0; i < argc; i++) { PyObject *s = PyUnicode_DecodeFSDefault(argv[i]); PyList_Append(args, s); Py_DECREF(s); }
    for (const char *const *e = env; e && *e; e++) { PyObject *s = PyUnicode_DecodeFSDefault(*e); PyList_Append(envl, s); Py_DECREF(s); }
    PyObject *r = PyObject_CallFunction(runner, "(sOOz)", tool, args, envl, log_path);
    if (r) { code = (int)PyLong_AsLong(r); Py_DECREF(r); } else PyErr_Print();
    pthread_mutex_lock(&ilock);
    run_tid = 0;
    if (interrupted) {
        PyThreadState_SetAsyncExc(tid, NULL);   // one not yet delivered (the tool ended first) must not hit the next run
        if (code != 0) code = 130;              // also when it landed in the runner's own clean-up
    }
    pthread_mutex_unlock(&ilock);
    if (PyErr_Occurred()) PyErr_Clear();
    Py_DECREF(args); Py_DECREF(envl);
    PyGILState_Release(g);
    pthread_mutex_unlock(&lock);
    return code;
}

// PyErr_SetInterrupt is not used: it acts only on Python's main thread and only with Python's SIGINT handler installed, and the packer runs with
// install_signal_handlers = 0 on a worker thread of the app. An asynchronous exception reaches the run's own thread at its next bytecode instead
// (a long C call, e.g. one zstd frame or a Capstone pass over a function, finishes first).
int dm_pack_interrupt(void) {
    int sent = 0;
    PyGILState_STATE g;
    pthread_mutex_lock(&ilock);
    if (!started || !run_tid) { pthread_mutex_unlock(&ilock); return 0; }
    pthread_mutex_unlock(&ilock);
    g = PyGILState_Ensure();   // waits for the running tool's next switch interval (5 ms) or GIL release
    pthread_mutex_lock(&ilock);
    if (run_tid) { sent = PyThreadState_SetAsyncExc(run_tid, PyExc_KeyboardInterrupt) > 0; interrupted = 1; }
    pthread_mutex_unlock(&ilock);
    PyGILState_Release(g);
    return sent;
}

#ifdef __ANDROID__
static char **strings(JNIEnv *e, jobjectArray a, int *n) {   // a Java String[] as a NULL-terminated C array (n: its length); free with unstrings
    int len = a ? (*e)->GetArrayLength(e, a) : 0;
    char **v = calloc(len + 1, sizeof *v);
    for (int i = 0; i < len; i++) {
        jstring s = (*e)->GetObjectArrayElement(e, a, i);
        const char *u = (*e)->GetStringUTFChars(e, s, NULL); v[i] = strdup(u); (*e)->ReleaseStringUTFChars(e, s, u); (*e)->DeleteLocalRef(e, s);
    }
    if (n) *n = len;
    return v;
}
static void unstrings(char **v) { for (char **p = v; p && *p; p++) free(*p); free(v); }

#ifdef __ANDROID__
// Preparation runs as a foreground service in :pack, and without an affinity mask Android leaves the work on the little cores:
// the shader translation (about eleven thousand shaders) then takes an hour instead of minutes. Pin the process -- CPython and
// the translator's threads inherit it -- to the cores at 75 % of the top clock or more (the big cluster: A78 + X1 on a 888,
// never the A55s). The same rule as the launcher's "Use fast cores" (android/app/src/main/cpp/nxlaunch.c).
static long cpu_max_khz(int c) {
    char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
    FILE *f = fopen(path, "r"); long v = 0; if (f) { if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); }
    return v;
}
static void pin_fast_cores(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_CONF); if (n > CPU_SETSIZE) n = CPU_SETSIZE;
    long top = 0;
    for (int c = 0; c < n; c++) { long k = cpu_max_khz(c); if (k > top) top = k; }
    if (!top) return;   // no cpufreq: leave the scheduler alone
    cpu_set_t cs; CPU_ZERO(&cs); int cnt = 0;
    for (int c = 0; c < n; c++) { long k = cpu_max_khz(c); if (k * 4 >= top * 3) { CPU_SET(c, &cs); cnt++; } }
    if (!cnt) return;
    if (sched_setaffinity(0, sizeof cs, &cs)) fprintf(stderr, "[pack] thread placement: sched_setaffinity failed, errno %d\n", errno);
    else fprintf(stderr, "[pack] thread placement: %d of %d cores (%ld MHz or more): the little cores stay with the system\n", cnt, n, top * 3 / 4000);
}
#endif

JNIEXPORT jint JNICALL Java_dev_nxnative_android_PackNative_init0(JNIEnv *e, jclass c, jstring home) {
    pin_fast_cores();
    const char *h = (*e)->GetStringUTFChars(e, home, NULL);
    int r = dm_pack_init(h);
    (*e)->ReleaseStringUTFChars(e, home, h);
    return r;
}

JNIEXPORT jint JNICALL Java_dev_nxnative_android_PackNative_run0(JNIEnv *e, jclass c, jstring tool, jobjectArray args, jobjectArray env, jstring log) {
    pin_fast_cores();   // again per tool: Android resets a process's mask when it changes foreground state
    int argc; char **argv = strings(e, args, &argc), **envv = strings(e, env, NULL);
    const char *t = (*e)->GetStringUTFChars(e, tool, NULL), *l = log ? (*e)->GetStringUTFChars(e, log, NULL) : NULL;
    int r = dm_pack_run(t, argc, argv, (const char *const *)envv, l);
    (*e)->ReleaseStringUTFChars(e, tool, t); if (l) (*e)->ReleaseStringUTFChars(e, log, l);
    unstrings(argv); unstrings(envv);
    return r;
}

JNIEXPORT jint JNICALL Java_dev_nxnative_android_PackNative_interrupt0(JNIEnv *e, jclass c) { return dm_pack_interrupt(); }
#endif
