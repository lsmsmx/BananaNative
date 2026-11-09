package dev.nxnative.android;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Why the game's process (":game") ended, in one plain sentence for the launcher's notice (ExitWatch; pure Java: dm/tools/tests/test_android_exitnotice.py runs it on
 * the JVM). Three sources are combined: the run's exit record (last-exit.txt in the app's files: GameActivity writes "state=running" at the start and "state=stopped"
 * on Stop; the uncaught-exception handler appends "state=java", nxlaunch.c's signal handler "state=native", the host's return "state=host_exit"), Android's
 * ApplicationExitInfo for that process, and the end of the run's log (the host's "CRASH sig=", "FATAL" and "GPU hang" lines). Keys are "key=value" lines, the last of a
 * key wins (the record is only appended to).
 */
final class ExitNotice {
    private ExitNotice() {}

    // ApplicationExitInfo.REASON_* (the values Android documents), mirrored so this class needs no Android
    static final int UNKNOWN = 0, EXIT_SELF = 1, SIGNALED = 2, LOW_MEMORY = 3, CRASH = 4, CRASH_NATIVE = 5, ANR = 6, INITIALIZATION_FAILURE = 7, PERMISSION_CHANGE = 8,
            EXCESSIVE_RESOURCE_USAGE = 9, USER_REQUESTED = 10, USER_STOPPED = 11, DEPENDENCY_DIED = 12, OTHER = 13, FREEZER = 14, PACKAGE_STATE_CHANGE = 15, PACKAGE_UPDATED = 16;
    /** No ApplicationExitInfo was found for the run's process */
    static final int NONE = -1;

    static final String TITLE = "The game stopped unexpectedly";

    static final String JAVA = "The app ran into an error and had to close the game.";
    static final String NATIVE = "The game ran into an error and crashed.";
    static final String FATAL = "The game reached something the app cannot handle yet, so it was stopped.";
    static final String GPU = "The GPU driver stopped responding, so the game had to close.";
    static final String MEMORY = "Android closed it: the phone ran out of memory. Closing other apps before playing can help.";
    static final String MEMORY_BG = "Android closed it while it was in the background: the phone ran out of memory. Unsaved progress is lost.";
    static final String KILLED = "Android closed it without saying why. This is often for memory: closing other apps before playing can help.";
    static final String ANR_TEXT = "Android closed it because it stopped responding.";
    static final String RESOURCES = "Android closed it for using too much of the phone's resources.";
    static final String PERMISSION = "Android closed it because one of the app's permissions changed.";
    static final String HOST_ERROR = "The game ended with an error.";
    static final String UNKNOWN_TEXT = "It closed without saying why.";

    /** The record's keys (last value wins); an empty map for no record */
    static Map<String, String> parse(String text) {
        Map<String, String> m = new HashMap<>();
        if (text == null) return m;
        for (String l : text.split("\n")) {
            int eq = l.indexOf('=');
            if (eq > 0) m.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
        }
        return m;
    }

    /** The record has an end to tell about: a run that was not stopped by the player, not yet shown */
    static boolean pending(Map<String, String> rec) {
        String st = rec.get("state");
        return st != null && !st.equals("stopped") && !"1".equals(rec.get("shown"));
    }

    /** Whether the record's own state says enough without Android's exit reason (a crash the app saw itself) */
    static boolean selfExplained(Map<String, String> rec) {
        String st = rec.get("state");
        return "java".equals(st) || "native".equals(st) || "host_exit".equals(st);
    }

    /** An end Android reports as for memory: its low-memory reason, or a kill whose description names the low-memory killer */
    static boolean memoryKill(int reason, String desc) {
        if (reason == LOW_MEMORY) return true;
        String d = desc == null ? "" : desc.toLowerCase(Locale.ROOT);
        return d.contains("lmk") || d.contains("low memory") || d.contains("lowmemory") || d.contains("oom");
    }

    /**
     * The sentence for a run's end, or null when there is nothing to tell (the player stopped it, swiped it away, the app was updated, the game ended by itself).
     * reason / status / desc: the process's ApplicationExitInfo (reason NONE without one); background: Android's importance for it was below visible; logTail: the
     * end of the run's log ("" without one)
     */
    static String cause(Map<String, String> rec, int reason, int status, String desc, boolean background, String logTail) {
        if (!pending(rec)) return null;
        String st = rec.get("state"), log = logTail == null ? "" : logTail;
        boolean gpu = log.contains("GPU hang") || log.contains("VK_ERROR_DEVICE_LOST") || log.contains("device lost");
        boolean crashLine = log.contains("CRASH sig="), fatal = log.contains("FATAL ");
        boolean oomLine = log.toLowerCase(Locale.ROOT).contains("out of memory");
        // What the player did or an update: no notice
        if (reason == USER_REQUESTED || reason == USER_STOPPED || reason == PACKAGE_UPDATED || reason == PACKAGE_STATE_CHANGE) return null;
        if (gpu) return GPU;
        if ("java".equals(st) || reason == CRASH) return JAVA;
        if ("native".equals(st) || reason == CRASH_NATIVE || crashLine) {
            if (oomLine) return MEMORY;
            return fatal ? FATAL : NATIVE;   // die(): a FATAL line, then abort
        }
        if ("host_exit".equals(st)) {
            String rc = rec.get("rc");
            return rc == null || rc.equals("0") ? null : HOST_ERROR;
        }
        // state=running: the process ended without the app seeing it
        if (memoryKill(reason, desc)) return background ? MEMORY_BG : MEMORY;
        switch (reason) {
            case ANR: return ANR_TEXT;
            case EXCESSIVE_RESOURCE_USAGE: return RESOURCES;
            case PERMISSION_CHANGE: return PERMISSION;
            case SIGNALED: return status == 9 ? (background ? MEMORY_BG : KILLED) : NATIVE;   // SIGKILL from the system, else a crash signal
            case EXIT_SELF:
                if (status >= 128 + 1 && status < 128 + 32) return NATIVE;   // the host's crash path ends with _exit(128 + signal)
                if (fatal) return FATAL;
                if (status != 0) return HOST_ERROR;
                return log.contains("graceful exit") ? null : UNKNOWN_TEXT;   // the game's own exit (dm_host_shutdown) ends with 0 after "graceful exit"
            case NONE: case UNKNOWN: case OTHER: case FREEZER: case DEPENDENCY_DIED: case INITIALIZATION_FAILURE: default:
                return fatal ? FATAL : UNKNOWN_TEXT;
        }
    }

    /** The notice's text: the game's name and the sentence, then where to find the details */
    static String message(String name, String cause) { return message(name, cause, null); }

    /** The same with the log's likely cause (LogDigest.likely: what a line of the log means, from dm/tools/log_rules.tsv), when one was found */
    static String message(String name, String cause, String likely) {
        String n = name == null || name.isEmpty() || name.equals("null") ? "The game" : name;
        return n + ": " + cause + (likely == null || likely.isEmpty() ? "" : "\n\nLikely cause: " + likely) + "\n\nShare log sends the log of that run, which says more.";
    }

    /** The log's block for an uncaught Java exception: "[RED] crash: " before every line of the trace, at most maxLines lines */
    static String crashBlock(String thread, String trace, int maxLines) {
        StringBuilder b = new StringBuilder("[RED] crash: uncaught exception on thread ").append(thread).append(", the game process ends\n");
        String[] l = trace == null ? new String[0] : trace.replace("\r", "").split("\n");
        int n = 0;
        for (String s : l) {
            if (s.trim().isEmpty()) continue;
            if (n++ == maxLines) { b.append("[RED] crash: ... ").append(l.length - maxLines).append(" more lines\n"); break; }
            b.append("[RED] crash: ").append(s).append('\n');
        }
        return b.toString();
    }

    /** The record's line for an uncaught exception: its class and message on one line, no line breaks */
    static String exceptionLine(String cls, String msg) {
        String s = msg == null ? cls : cls + ": " + msg;
        return s.replace('\n', ' ').replace('\r', ' ');
    }
}
