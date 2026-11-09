package dev.nxnative.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintWriter;
import java.io.RandomAccessFile;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * How the game's process ended, told to the player (android/README.md, "When the game stops unexpectedly"). The game's process keeps the run's exit record,
 * &lt;app files&gt;/last-exit.txt (ExitNotice has its keys): written at the start, "state=stopped" appended on Stop, "state=java" by the uncaught-exception handler
 * installed here, "state=native" by nxlaunch.c's signal handler (NX_EXIT_RECORD), "state=host_exit" when the host returns. The launcher reads it when it comes to the
 * front, and every 2 s while it shows and the record says a game is running (so a game that dies while the launcher shows is noticed at once); once the game's process is
 * gone it adds Android's ApplicationExitInfo for that process and the end of the run's log, and shows "The game stopped unexpectedly" with the cause, Share log and OK,
 * once per exit ("shown=1" is appended). LogDigest reads the end of that log with dm/tools/log_digest.py's rules: the notice adds the likely cause and "What it means"
 * (the rule's doc) when a rule matched, and Share log sends the digest's text beside the log.
 */
final class ExitWatch {
    private ExitWatch() {}

    static File record(Context c) { return new File(c.getFilesDir(), "last-exit.txt"); }

    private static void append(File f, String text) {
        try (FileOutputStream o = new FileOutputStream(f, true)) { o.write(text.getBytes(StandardCharsets.UTF_8)); }
        catch (IOException ignored) { /* no record: no notice */ }
    }

    static Map<String, String> read(Context c) {
        File f = record(c);
        try { return ExitNotice.parse(f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8) : null); }
        catch (IOException e) { return ExitNotice.parse(null); }
    }

    // ---- The game's process ----
    private static volatile File recordFile, runLog, logDir;
    private static volatile String runId, runName;
    private static volatile boolean owned;   // this process wrote the record (a screen restored into a new process did not)

    /** GameActivity.onCreate, first: the handler; fresh: a new run starts (not a screen created again, not one restored into a new process), so the record is new */
    static void game(Context c, String id, String name, boolean fresh) {
        recordFile = record(c);
        logDir = LogProvider.dir(c);
        runId = id; runName = name;
        Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        if (!(prev instanceof OnCrash)) Thread.setDefaultUncaughtExceptionHandler(new OnCrash(prev));
        if (fresh && !owned) begin();
    }

    private static synchronized void begin() {
        owned = true;
        try {
            Files.write(recordFile.toPath(), ("v=1\ntitle=" + runId + "\nname=" + oneLine(runName) + "\npid=" + android.os.Process.myPid() + "\nstart=" + System.currentTimeMillis()
                    + "\nstate=running\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) { /* no record: no notice */ }
    }

    private static String oneLine(String s) { return String.valueOf(s).replace('\n', ' '); }

    /** The run's log (GameActivity.start) */
    static void log(File log) {
        runLog = log;
        if (owned) append(recordFile, "log=" + log.getName() + "\n");
    }

    /** The player stopped the game (Stop, Back at a start that cannot go on): no notice */
    static void stopped() { if (owned && recordFile != null) append(recordFile, "state=stopped\n"); }

    /** An end the game's screen told the player about itself (a screen restored into a new process): the launcher does not tell it again */
    static void shown(Context c) { append(record(c), "shown=1\n"); }

    /** An uncaught exception on any thread of the game's process: its trace into the run's log ([RED] crash: lines; a log of its own when none was started) and the
     *  record, then the handler before (Android's: logcat's crash buffer, the process ends) */
    private static final class OnCrash implements Thread.UncaughtExceptionHandler {
        private final Thread.UncaughtExceptionHandler prev;
        private final AtomicBoolean once = new AtomicBoolean();
        OnCrash(Thread.UncaughtExceptionHandler prev) { this.prev = prev; }

        @Override public void uncaughtException(Thread t, Throwable e) {
            try {
                if (once.compareAndSet(false, true) && recordFile != null) {
                    if (!owned) begin();
                    StringWriter w = new StringWriter();
                    e.printStackTrace(new PrintWriter(w));
                    File log = runLog;
                    if (log == null) {
                        log = new File(logDir, runId + "-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()) + ".log");
                        append(log, "[nxlaunch] build " + BuildConfig.VERSION_NAME + " (" + BuildConfig.NX_COMMIT + "): the game process crashed before its log started\n");
                        append(recordFile, "log=" + log.getName() + "\n");
                    }
                    append(log, ExitNotice.crashBlock(t.getName(), w.toString(), 60));   // the log is opened O_APPEND by nxlaunch too: whole lines, no overlap
                    append(recordFile, "state=java\nexception=" + ExitNotice.exceptionLine(e.getClass().getName(), e.getMessage()) + "\nthread=" + oneLine(t.getName()) + "\n");
                }
            } catch (Throwable ignored) {
                // nothing more can be written: the process ends as before
            } finally {
                if (prev != null) prev.uncaughtException(t, e);
                else { android.os.Process.killProcess(android.os.Process.myPid()); System.exit(10); }
            }
        }
    }

    /** DM_TEST_CRASH=java (the test app only, GameActivity.start): an exception on the main thread 10 s after the start */
    static void testCrash(Handler ui) {
        ui.postDelayed(() -> { throw new IllegalStateException("DM_TEST_CRASH=java: a test crash on purpose"); }, 10000);
    }

    // ---- Android's side and the cause ----
    /** The game's process with this pid still runs */
    static boolean alive(Context c, int pid) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> l = am == null ? null : am.getRunningAppProcesses();
        if (l != null) for (ActivityManager.RunningAppProcessInfo p : l) if (p.pid == pid && p.processName != null && p.processName.endsWith(":game")) return true;
        return false;
    }

    /** Android's record of how the process with this pid ended, or null */
    static ApplicationExitInfo exitInfo(Context c, int pid, long start) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am == null || pid <= 0) return null;
        try {
            for (ApplicationExitInfo e : am.getHistoricalProcessExitReasons(c.getPackageName(), pid, 4))
                if (e.getProcessName() != null && e.getProcessName().endsWith(":game") && e.getTimestamp() >= start) return e;
        } catch (RuntimeException ignored) { /* not available: the record alone */ }
        return null;
    }

    static File logOf(Context c, Map<String, String> rec) {
        String n = rec.get("log");
        if (n == null || n.contains("/")) return null;
        File f = new File(LogProvider.dir(c), n);
        return f.isFile() ? f : null;
    }

    static String tail(File log, int bytes) {
        if (log == null) return "";
        try (RandomAccessFile r = new RandomAccessFile(log, "r")) {
            long n = r.length(), from = Math.max(0, n - bytes);
            byte[] b = new byte[(int) (n - from)];
            r.seek(from);
            r.readFully(b);
            return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) { return ""; }
    }

    private static long num(String s) { try { return Long.parseLong(s); } catch (RuntimeException e) { return -1; } }

    /** A run's end as the notice tells it: the cause sentence and Android's exit info it came from (reason ExitNotice.NONE without one) */
    static final class End {
        final String why, desc; final int reason, status; final boolean background;
        End(String why, int reason, int status, String desc, boolean background) { this.why = why; this.reason = reason; this.status = status; this.desc = desc; this.background = background; }
    }

    /** The record's end, or null (nothing to tell, or the process still runs). waitForAndroid: a run the app did not see end has no cause until Android has its exit info */
    static End end(Context c, Map<String, String> rec, boolean waitForAndroid) {
        if (!ExitNotice.pending(rec)) return null;
        int pid = (int) num(rec.get("pid"));
        if (pid == android.os.Process.myPid() || pid > 0 && alive(c, pid)) return null;
        ApplicationExitInfo e = exitInfo(c, pid, num(rec.get("start")) - 1000);
        if (e == null && waitForAndroid && !ExitNotice.selfExplained(rec)) return null;
        boolean bg = e != null && e.getImportance() > ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE;
        int reason = e == null ? ExitNotice.NONE : e.getReason(), status = e == null ? 0 : e.getStatus();
        String desc = e == null ? null : e.getDescription();
        String why = ExitNotice.cause(rec, reason, status, desc, bg, tail(logOf(c, rec), 32 * 1024));
        return why == null ? null : new End(why, reason, status, desc, bg);
    }

    /** The record's cause sentence, or null (as end) */
    static String cause(Context c, Map<String, String> rec, boolean waitForAndroid) {
        End e = end(c, rec, waitForAndroid);
        return e == null ? null : e.why;
    }

    // ---- The log's digest (LogDigest, with dm/tools/log_rules.tsv and log_noise.tsv from the APK's assets) ----
    private static List<LogDigest.Row> rules, noise;

    private static String asset(Context c, String name) throws IOException {
        try (InputStream in = c.getAssets().open(name); ByteArrayOutputStream o = new ByteArrayOutputStream()) {
            byte[] b = new byte[16384];
            for (int k; (k = in.read(b)) > 0; ) o.write(b, 0, k);
            return o.toString("UTF-8");
        }
    }

    /** The digest of the run's log for this end, its text written beside the log (<log name without .log>.digest.txt, for Share log); null when it cannot be made */
    static LogDigest.Verdict digest(Context c, File log, Map<String, String> rec, End end) {
        if (log == null) return null;
        try {
            synchronized (ExitWatch.class) {
                if (rules == null) { rules = LogDigest.rules(asset(c, LogDigest.RULES_ASSET)); noise = LogDigest.noise(asset(c, LogDigest.NOISE_ASSET)); }
            }
            LogDigest d = new LogDigest(rules, noise);
            d.readTail(log, LogDigest.TAIL_BYTES);
            LogDigest.Verdict v = d.verdict(rec, end.reason, end.status, end.desc, end.background);
            Files.write(digestOf(log).toPath(), d.render(v).getBytes(StandardCharsets.UTF_8));
            return v;
        } catch (IOException | RuntimeException e) {
            return null;   // no digest: the notice keeps its sentence
        }
    }

    /** The digest file beside a run's log */
    static File digestOf(File log) { return new File(log.getParentFile(), log.getName().replaceFirst("\\.log$", "") + LogProvider.DIGEST); }

    // ---- The launcher ----
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static Runnable poll;
    private static int waits;
    private static AlertDialog showing;
    private static boolean digesting;   // a notice's digest is being made (off the main thread): no second check meanwhile

    /** MainActivity.onResume: a check now, and every 2 s while it shows and a game runs */
    static void resume(MainActivity a) {
        pause();
        waits = 0;
        poll = new Runnable() {
            @Override public void run() {
                if (a.isFinishing() || a.isDestroyed()) return;
                if (check(a)) UI.postDelayed(this, 2000);
            }
        };
        UI.post(poll);
    }

    static void pause() {
        if (poll != null) UI.removeCallbacks(poll);
        poll = null;
    }

    /** One check; true: look again later (a game runs, or its end is not known to Android yet) */
    private static boolean check(MainActivity a) {
        if (showing != null && showing.isShowing() || digesting) return false;
        Map<String, String> rec = read(a);
        if (!ExitNotice.pending(rec)) return false;
        int pid = (int) num(rec.get("pid"));
        if (pid > 0 && alive(a, pid)) return true;   // still running
        // Gone: Android writes its exit info as the process dies; a run the app did not see end waits for it up to about 6 s, then is told without it
        boolean wait = waits++ < 3;
        End end = end(a, rec, wait);
        if (end != null) { show(a, rec, end); return false; }
        if (wait) return true;
        shown(a);   // nothing to tell (the player swiped it away, an update, the game's own exit): not read again
        return false;
    }

    /** The notice, once the log's digest is made on a thread of its own (the end of a log of many MB is read): its likely cause and doc link when a rule matched.
     *  Marked shown only when it shows (a launcher gone meanwhile tells it at its next resume) */
    private static void show(MainActivity a, Map<String, String> rec, End end) {
        File log = logOf(a, rec);
        digesting = true;
        new Thread(() -> {
            LogDigest.Verdict v = digest(a, log, rec, end);
            UI.post(() -> {
                digesting = false;
                if (a.isFinishing() || a.isDestroyed()) return;
                shown(a);
                File dg = log == null ? null : digestOf(log);
                String url = v == null ? null : v.docUrl();
                String likely = LogDigest.likely(v, end.why);
                AlertDialog.Builder b = new AlertDialog.Builder(a).setTitle(ExitNotice.TITLE).setMessage(ExitNotice.message(rec.get("name"), end.why, likely))
                        .setPositiveButton("OK", null);
                if (log != null) b.setNeutralButton("Share log", (d, w) -> a.shareLog(log, dg != null && dg.isFile() ? dg : null));
                if (likely != null && url != null) b.setNegativeButton("What it means", (d, w) -> a.openLink(url));
                showing = b.show();
            });
        }, "exit-digest").start();
    }

    /** The game's screen restored into a new process (GameActivity.onCreate): the cause of the end before, when the record has one, else null; marked shown */
    static String restoredCause(Activity a) {
        Map<String, String> rec = read(a);
        String why = cause(a, rec, false);
        if (ExitNotice.pending(rec)) shown(a);
        return why;
    }
}
