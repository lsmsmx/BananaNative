package dev.nxnative.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The part of dm/tools/log_digest.py the "The game stopped unexpectedly" notice needs (pure Java: dm/tools/tests/test_android_logdigest.py runs it on the JVM and checks
 * its text against the Python tool's on the same logs). It reads the end of the run's log (the last TAIL_BYTES, line numbers counted from the start of the file) with the
 * tool's own tables, dm/tools/log_rules.tsv and dm/tools/log_noise.tsv, which build.gradle copies into the APK's assets (digest/), and gives the tool's verdict (its kind,
 * label, sentence and likely cause: the first rule of the verdict's kind that a line matched, with its doc link), the crash block and the [RED] / error line groups, as
 * the tool prints them. The rest of the tool's digest (header, stage, symptom index, performance, tail) stays with the tool: the notice's digest says so in its last line.
 * \w, \d, \b and (?i) mean what they mean in Python's re: on the JVM the regexes are compiled with UNICODE_CHARACTER_CLASS; Android's java.util.regex (ICU) refuses
 * that flag and is Unicode-aware without it.
 */
final class LogDigest {
    static final long TAIL_BYTES = 2L << 20;
    static final String RULES_ASSET = "digest/log_rules.tsv", NOISE_ASSET = "digest/log_noise.tsv";
    /** A rule's doc (a repository path with its anchor) is read on the public repository's android branch */
    static final String DOCS_BASE = "https://git.eden-emu.dev/moceure/BananaNative/src/branch/android/";
    static final int CUT = 200, MAX_GROUPS = 15, TRACK_GROUPS = 4000;
    private static final int FLAGS = "The Android Project".equals(System.getProperty("java.vendor")) ? 0 : Pattern.UNICODE_CHARACTER_CLASS;

    // ApplicationExitInfo.REASON_* (as ExitNotice; the tool's R_*)
    private static final int R_EXIT_SELF = 1, R_SIGNALED = 2, R_LOW_MEMORY = 3, R_CRASH = 4, R_CRASH_NATIVE = 5, R_ANR = 6, R_RESOURCES = 9;

    // ---------------------------------------------------------------- tables

    static final class Row {
        final String id, kind, regex, cause, doc;
        final Pattern rx;
        Row(String id, String kind, String regex, String cause, String doc) {
            this.id = id; this.kind = kind; this.regex = regex; this.cause = cause; this.doc = doc; this.rx = Pattern.compile(regex, FLAGS);
        }
    }

    /** The rows of a rule table (the tool's read_tsv): '#' lines and empty lines skipped, the first other line the header, which must start with cols. Throws
     *  IllegalArgumentException for a wrong header, PatternSyntaxException for a regex Java does not take */
    static List<Row> table(String text, String... cols) {
        List<Row> rows = new ArrayList<>();
        boolean header = false;
        int n = 0;
        for (String line : text.split("\n", -1)) {
            n++;
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] f = line.split("\t", -1);
            if (!header) {
                for (int i = 0; i < cols.length; i++)
                    if (i >= f.length || !f[i].equals(cols[i])) throw new IllegalArgumentException("line " + n + ": header is not " + String.join(" ", cols));
                header = true;
                continue;
            }
            String[] v = new String[cols.length];
            for (int i = 0; i < v.length; i++) v[i] = i < f.length ? f[i] : "";
            // rules: id kind regex cause doc; noise: id regex note
            rows.add(cols.length == 5 ? new Row(v[0], v[1], v[2], v[3], v[4]) : new Row(v[0], "", v[1], v[2], ""));
        }
        return rows;
    }

    static List<Row> rules(String text) { return table(text, "id", "kind", "regex", "cause", "doc"); }
    static List<Row> noise(String text) { return table(text, "id", "regex", "note"); }

    /** The index of the first row whose regex is found in the line, or -1 */
    static int search(List<Row> rows, String line) {
        for (int i = 0; i < rows.size(); i++) if (rows.get(i).rx.matcher(line).find()) return i;
        return -1;
    }

    // ---------------------------------------------------------------- redaction and normalisation (the tool's, and scripts/bug_report.py's key patterns)

    private static final String HEX = "[0-9a-fA-F]";
    static final Pattern KEY_LINE = Pattern.compile("(?i)key\\w*[\\s=:\"'\\[\\],]*" + HEX + "{32,}", FLAGS);
    static final Pattern KEY_LINE2 = Pattern.compile("(?i)(?:title|master|prod|common|aes|kek|rights)[\\w ]*[=:]\\s*\"?" + HEX + "{32,}\\b", FLAGS);
    private static final Pattern HEX_RUN = Pattern.compile("[0-9a-fA-F]{32,}"), HEX_LONG = Pattern.compile("[0-9a-fA-F]{32}");
    private static final Pattern[] PATHS = {
        Pattern.compile("/(?:storage/emulated/\\d+|sdcard)/Android/data/[\\w.]+/files", FLAGS),
        Pattern.compile("/data/(?:user/\\d+|data)/[\\w.]+/files", FLAGS),
        Pattern.compile("/data/(?:user/\\d+|data)/[\\w.]+/cache", FLAGS),
        Pattern.compile("/data/app/[^\\s/]*/[\\w.]+-[^\\s/]+/", FLAGS),
        Pattern.compile("/(?:storage/emulated/\\d+|sdcard)/", FLAGS),
        Pattern.compile("/storage/[0-9A-F]{4}-[0-9A-F]{4}/", FLAGS),
        Pattern.compile("/(?:Users|home)/[^\\s/]+", FLAGS),
    };
    private static final String[] PATH_REP = {"<app files>", "<app internal>", "<app cache>", "<apk>/", "<sdcard>/", "<sd card>/", "~"};
    private static final String HOME = System.getProperty("user.home", "");

    static boolean keyLike(String line) { return HEX_LONG.matcher(line).find() && (KEY_LINE.matcher(line).find() || KEY_LINE2.matcher(line).find()); }

    /** The first n code points of s (Python's s[:n]) */
    private static String cut(String s, int n) {
        if (s.length() <= n || s.codePointCount(0, s.length()) <= n) return s;
        return s.substring(0, s.offsetByCodePoints(0, n));
    }

    private static String rstripNl(String s) {
        int e = s.length();
        while (e > 0 && (s.charAt(e - 1) == '\r' || s.charAt(e - 1) == '\n')) e--;
        return s.substring(0, e);
    }

    static String redact(String s, int cutAt) {
        if (!HOME.isEmpty() && !HOME.equals("/") && s.contains(HOME)) s = s.replace(HOME, "~");
        if (s.indexOf('/') >= 0) for (int i = 0; i < PATHS.length; i++) s = PATHS[i].matcher(s).replaceAll(Matcher.quoteReplacement(PATH_REP[i]));
        s = HEX_RUN.matcher(rstripNl(s)).replaceAll("<hex>");
        return s.codePointCount(0, s.length()) <= cutAt ? s : cut(s, cutAt - 1) + "…";
    }

    static String redact(String s) { return redact(s, CUT); }

    private static final Pattern[] NORM = {
        Pattern.compile("(?:/[\\w.~@+%=,-]+)+/?", FLAGS),
        Pattern.compile("\\b0x[0-9a-fA-F]+\\b", FLAGS),
        Pattern.compile("\\b(?=[0-9a-fA-F]*\\d)[0-9a-fA-F]{6,}\\b", FLAGS),
        Pattern.compile("\\d+(?:\\.\\d+)?", FLAGS),
        Pattern.compile("\\s+", FLAGS),
    };
    private static final String[] NORM_REP = {"<path>", "#", "#", "#", " "};

    /** A line's shape: paths, hex, numbers and spacing made equal (for grouping) */
    static String normalise(String s) {
        for (int i = 0; i < NORM.length; i++) s = NORM[i].matcher(s).replaceAll(NORM_REP[i]);
        int a = 0, e = s.length();
        while (a < e && Character.isWhitespace(s.charAt(a))) a++;
        while (e > a && Character.isWhitespace(s.charAt(e - 1))) e--;
        return s.substring(a, e);
    }

    // ---------------------------------------------------------------- the lines the digest looks at

    private static final Pattern[] STAGES = {
        Pattern.compile("^\\[nxlaunch\\] (env |arg |hooks: )", FLAGS),
        Pattern.compile("^\\[dm\\] build: commit ", FLAGS),
        Pattern.compile("^\\[dm\\] thread slots: ", FLAGS),
        Pattern.compile("^\\[dm\\] (stubs=|sdk: NintendoSDK )", FLAGS),
        Pattern.compile("^\\[dm\\] (>>> nnosInitialize|guest AS )", FLAGS),
        Pattern.compile("^\\[dm\\] >>> nnMain", FLAGS),
        Pattern.compile("^\\[dm\\] \\[nvn\\] device API ", FLAGS),
        Pattern.compile("^\\[dm\\] (\\[nvn\\] (frame \\d+:|frame interval up to|timing up to)|\\[vk\\] fps: |\\[longframe\\] frame )", FLAGS),
        Pattern.compile("^\\[dm\\] \\[mark\\] frame ", FLAGS),
    };
    private static final Pattern ERROR_RX = Pattern.compile("\\[RED\\]|\\bFATAL |CRASH sig=|terminating due to|Abort message|Fatal signal", FLAGS);
    private static final Pattern EXPECTED_RX = Pattern.compile("not yet on (?:Android|Vulkan)", FLAGS);
    private static final Pattern CRASH_RX = Pattern.compile("^\\[dm\\] CRASH sig=(\\d+)(.*)$", FLAGS);
    private static final Pattern CRASH_FIELD = Pattern.compile("\\b(addr|pc|lr|sp)=(\\S+)", FLAGS);
    private static final Pattern STACK_SCAN = Pattern.compile("\\[[+-]\\d+\\](\\S+)", FLAGS);
    private static final Pattern GAME_MOD = Pattern.compile("^(main|sdk|subsdk\\d*|rtld)\\+0x[0-9a-f]+$", FLAGS);
    private static final Pattern JAVA_CRASH_RX = Pattern.compile("\\[RED\\] crash: uncaught exception on thread (.+?), the game process ends", FLAGS);

    static boolean errorLine(String line) {
        return (line.contains("[RED]") || line.contains("FATAL") || line.contains("CRASH") || line.contains("terminating") || line.contains("Abort") || line.contains("Fatal"))
                && ERROR_RX.matcher(line).find();
    }

    // ---------------------------------------------------------------- the digest

    static final class Hit { int count, first, last; String example; }
    static final class Group { int count, first, last; String example; }
    static final class Crash {
        int line; String text; Integer sig; String pc, lr, addr;
        final List<Object[]> before = new ArrayList<>();   // {Integer line, String text}
        List<String> bt = new ArrayList<>(), stackScan = new ArrayList<>();
    }
    static final class Java { int line; String thread; final List<String> trace = new ArrayList<>(); }

    /** The verdict (the tool's Digest.verdict): kind, label, sentence (or null), the likely cause's rule id, cause, doc (or null) and line (0 for none) */
    static final class Verdict {
        String kind, label, sentence, rule, cause, doc;
        int line;
        /** The rule's doc as a web link, or null */
        String docUrl() { return doc == null || doc.isEmpty() ? null : DOCS_BASE + doc; }
    }

    final List<Row> rules, noise;
    String name = "";
    long bytes;
    int n;   // the line number of the last line read
    final ArrayDeque<Object[]> before = new ArrayDeque<>();
    final boolean[] stage = new boolean[STAGES.length];
    final LinkedHashMap<String, Group> groups = new LinkedHashMap<>();
    int untracked, expectedN, droppedKeys, graceful, crashN, crashWait;
    final HashMap<String, Integer> expected = new HashMap<>();
    final Map<Integer, Hit> hits = new HashMap<>();
    Crash crash;
    Java java;

    LogDigest(List<Row> rules, List<Row> noise) { this.rules = rules; this.noise = noise; }

    /** One line of the run log (its terminator already taken off) */
    void feed(String raw) {
        int ln = ++n;
        String line = rstripNl(raw);
        if (keyLike(line)) { droppedKeys++; return; }
        if (crashWait > 0) crashMore(line);
        for (int i = 0; i < STAGES.length; i++) if (!stage[i] && STAGES[i].matcher(line).lookingAt()) stage[i] = true;
        if (line.contains("graceful exit")) graceful++;
        int r = search(rules, line);
        if (r >= 0) {
            Hit h = hits.get(r);
            if (h == null) { h = new Hit(); h.first = ln; h.example = line; hits.put(r, h); }
            h.count++; h.last = ln;
        }
        if (errorLine(line)) error(ln, line);
        if (line.startsWith("[dm] CRASH sig=")) crash(ln, line);
        else if (line.contains("uncaught exception on thread") && java == null) {
            Matcher m = JAVA_CRASH_RX.matcher(line);
            if (m.find()) { java = new Java(); java.line = ln; java.thread = m.group(1); }
        } else if (java != null && java.trace.size() < 8 && line.contains("[RED] crash: ") && ln > java.line)
            java.trace.add(redact(line.substring(line.indexOf("[RED] crash: ") + 13)));
        before.addLast(new Object[] {ln, line});
        if (before.size() > 12) before.removeFirst();
    }

    private void error(int ln, String line) {
        if (EXPECTED_RX.matcher(line).find()) {
            expectedN++;
            String k = cut(normalise(line), 160);
            if (expected.size() < TRACK_GROUPS || expected.containsKey(k)) expected.merge(k, 1, Integer::sum);
            return;
        }
        String key = cut(normalise(line), 400);
        Group g = groups.get(key);
        if (g == null) {
            if (groups.size() >= TRACK_GROUPS) { untracked++; return; }
            g = new Group(); g.first = ln; g.example = line; groups.put(key, g);
        }
        g.count++; g.last = ln;
    }

    private void crash(int ln, String line) {
        crashN++;
        if (crash != null) return;
        Crash c = new Crash();
        Matcher m = CRASH_RX.matcher(line);
        String rest = "";
        if (m.lookingAt()) { c.sig = Integer.valueOf(m.group(1)); rest = m.group(2); }
        Matcher f = CRASH_FIELD.matcher(rest);
        while (f.find()) {
            switch (f.group(1)) {
                case "pc": c.pc = f.group(2); break;
                case "lr": c.lr = f.group(2); break;
                case "addr": c.addr = f.group(2); break;
                default: break;
            }
        }
        c.line = ln; c.text = line;
        for (Object[] b : before) if (search(noise, (String) b[1]) < 0) c.before.add(b);
        crash = c;
        crashWait = 80;
    }

    private void crashMore(String line) {
        crashWait--;
        Crash c = crash;
        if (c == null) { crashWait = 0; return; }
        if (line.startsWith("[dm] bt:") && c.bt.isEmpty()) {
            for (String s : line.substring(8).trim().split("\\s+")) if (!s.isEmpty() && c.bt.size() < 8) c.bt.add(s);
        } else if (line.startsWith("[dm] stack scan") && c.stackScan.isEmpty()) {
            Matcher m = STACK_SCAN.matcher(line);
            while (m.find() && c.stackScan.size() < 6) c.stackScan.add(m.group(1));
        }
        if (!c.bt.isEmpty() && !c.stackScan.isEmpty()) crashWait = 0;
    }

    /** A text's lines as Python reads a file with newline='': split after \n, \r\n or \r, the terminator taken off; no line after a final terminator */
    void feedText(String text) {
        int i = 0, len = text.length();
        while (i < len) {
            int j = i;
            while (j < len && text.charAt(j) != '\n' && text.charAt(j) != '\r') j++;
            feed(text.substring(i, j));
            if (j < len && text.charAt(j) == '\r' && j + 1 < len && text.charAt(j + 1) == '\n') j++;
            i = j + 1;
        }
    }

    // ---------------------------------------------------------------- results

    private int firstRule(String kind) {
        for (int i = 0; i < rules.size(); i++) if (rules.get(i).kind.equals(kind) && hits.containsKey(i)) return i;
        return -1;
    }

    private static String lower(String s) { return s == null ? "" : s.toLowerCase(Locale.ROOT); }

    /**
     * The tool's verdict for this log, the run's exit record (last-exit.txt's keys: state, rc) and Android's exit info for its process (reason ExitNotice.NONE without
     * one; background: its importance was below visible)
     */
    Verdict verdict(Map<String, String> rec, int reason, int status, String desc, boolean background) {
        String st = rec.get("state");
        String d = lower(desc);
        int gpu = firstRule("driver"), oom = firstRule("oom"), fatal = firstRule("fatal"), hang = firstRule("hang");
        boolean lmk = reason == R_LOW_MEMORY || d.contains("lmk") || d.contains("low memory") || d.contains("lowmemory") || d.contains("oom");
        boolean crashed = crash != null || "native".equals(st) || reason == R_CRASH_NATIVE;
        Verdict v = new Verdict();
        int rule = -1;
        if ("stopped".equals(st) || reason == 10 || reason == 11 || reason == 15 || reason == 16) set(v, "stopped", "stopped by the player", null);
        else if (gpu >= 0) { set(v, "driver_lost", "driver lost", ExitNotice.GPU); rule = gpu; }
        else if ("java".equals(st) || reason == R_CRASH || java != null) { set(v, "java_crash", "crashed (the app)", ExitNotice.JAVA); rule = firstRule("crash"); }
        else if (crashed && hang >= 0 && crash != null && crash.sig != null && crash.sig == 14) { set(v, "hang", "hang (watchdog)", rules.get(hang).cause); rule = hang; }
        else if (crashed) {
            if (oom >= 0) { set(v, "out_of_memory", "out of memory", ExitNotice.MEMORY); rule = oom; }
            else if (fatal >= 0) { set(v, "host_fatal", "host FATAL", ExitNotice.FATAL); rule = fatal; }
            else { set(v, "crashed", "crashed", ExitNotice.NATIVE); rule = firstRule("crash"); }
        }
        else if (hang >= 0 && rules.get(hang).id.equals("watchdog")) { set(v, "hang", "hang (watchdog)", rules.get(hang).cause); rule = hang; }
        else if ("host_exit".equals(st)) {
            if ("0".equals(rec.getOrDefault("rc", "0"))) set(v, "normal_exit", "ran and exited normally", null);
            else { set(v, "host_error", "host error", ExitNotice.HOST_ERROR); rule = fatal; }
        }
        else if (lmk) { set(v, "out_of_memory", "out of memory", background ? ExitNotice.MEMORY_BG : ExitNotice.MEMORY); rule = oom; }
        else if (reason == R_ANR) set(v, "anr", "not responding", ExitNotice.ANR_TEXT);
        else if (reason == R_RESOURCES) set(v, "killed", "killed by Android", ExitNotice.RESOURCES);
        else if (reason == R_SIGNALED)
            set(v, "killed", "killed by Android", background && status == 9 ? ExitNotice.MEMORY_BG : status == 9 ? ExitNotice.KILLED : ExitNotice.NATIVE);
        else if (fatal >= 0) { set(v, "host_fatal", "host FATAL", ExitNotice.FATAL); rule = fatal; }
        else if (graceful > 0) set(v, "normal_exit", "ran and exited normally", null);
        else if (reason == R_EXIT_SELF && status != 0) set(v, "host_error", "host error", ExitNotice.HOST_ERROR);
        else set(v, "cut_short", "log cut short", ExitNotice.UNKNOWN_TEXT + " The log ends without an exit line (stopped from outside, or the log was cut).");
        if (rule >= 0) {
            Row r = rules.get(rule);
            v.rule = r.id; v.cause = r.cause; v.doc = r.doc; v.line = hits.get(rule).first;
        }
        int last = -1;
        for (int i = 0; i < stage.length; i++) if (stage[i]) last = i + 1;
        if (v.kind.equals("cut_short") && last > 0 && last < 8) v.sentence += " It stopped before stage " + (last + 1) + ".";
        return v;
    }

    private static void set(Verdict v, String kind, String label, String sentence) { v.kind = kind; v.label = label; v.sentence = sentence; }

    /** The notice's "Likely cause" sentence: the verdict's rule cause, unless it only says the notice's sentence again; null without one */
    static String likely(Verdict v, String noticeSentence) {
        if (v == null || v.cause == null || v.cause.isEmpty() || v.cause.equals(noticeSentence)) return null;
        return v.cause;
    }

    private static String fmtLines(int first, int last) { return "L" + first + (last != first ? "–L" + last : ""); }

    /** The digest's text: the tool's first line, verdict, crash, [RED] / error lines and rules sections, as log_digest.py prints them, then where the rest is */
    String render(Verdict v) {
        StringBuilder o = new StringBuilder();
        o.append("== log digest: ").append(name).append(" (").append(n).append(" lines, ").append(String.format(Locale.ROOT, "%.1f", bytes / 1e6)).append(" MB)\n");
        o.append("-- verdict\n");
        o.append("  ").append(v.label).append(v.sentence != null ? ": " + v.sentence : "").append('\n');
        if (v.rule != null)
            o.append("  likely cause [").append(v.rule).append(v.line > 0 ? ", L" + v.line : "").append("]: ").append(v.cause)
                    .append(v.doc != null && !v.doc.isEmpty() ? " (" + v.doc + ")" : "").append('\n');
        if (crash != null || java != null) {
            o.append("-- crash\n");
            Crash c = crash;
            if (c != null) {
                for (int i = Math.max(0, c.before.size() - 3); i < c.before.size(); i++)
                    o.append("  L").append(c.before.get(i)[0]).append(": ").append(redact((String) c.before.get(i)[1])).append('\n');
                o.append("  L").append(c.line).append(": ").append(redact(c.text)).append(crashN > 1 ? "  (+" + (crashN - 1) + " more CRASH lines)" : "").append('\n');
                String game = null;
                List<String> frames = new ArrayList<>();
                frames.add(c.pc); frames.add(c.lr); frames.addAll(c.bt);
                for (String f : frames) if (f != null && !f.isEmpty() && GAME_MOD.matcher(f).matches()) { game = f; break; }
                if (game != null) o.append("  first game frame: ").append(game).append('\n');
                if (!c.bt.isEmpty()) o.append("  bt: ").append(String.join(" ", c.bt)).append('\n');
                if (!c.stackScan.isEmpty()) o.append("  stack scan: ").append(String.join(" ", c.stackScan)).append('\n');
            }
            if (java != null) {
                o.append("  L").append(java.line).append(": uncaught exception on thread ").append(java.thread).append('\n');
                for (String t : java.trace) o.append("    ").append(t).append('\n');
            }
        }
        if (!groups.isEmpty() || expectedN > 0) {
            int total = untracked, shown = 0;
            for (Group g : groups.values()) total += g.count;
            int more = Math.max(0, groups.size() - MAX_GROUPS) + (untracked > 0 ? 1 : 0);
            o.append("-- [RED] / error lines (").append(total).append(" in ").append(Math.min(groups.size(), MAX_GROUPS) + more).append(" groups)\n");
            for (Group g : groups.values()) {
                if (shown++ == MAX_GROUPS) break;
                o.append("  ×").append(g.count).append(' ').append(fmtLines(g.first, g.last)).append(": ").append(redact(g.example)).append('\n');
            }
            if (more > 0) o.append("  +").append(more).append(" more groups\n");
            if (expectedN > 0) o.append("  expected stubs (\"not yet on Android/Vulkan\"): ").append(expectedN).append(" lines in ").append(expected.size()).append(" groups\n");
        }
        List<Integer> hit = new ArrayList<>(hits.keySet());
        hit.removeIf(i -> rules.get(i).id.equals("graceful-exit") && rules.get(i).kind.equals("info"));
        hit.sort((a, b) -> {
            int c = Integer.compare(hits.get(a).first, hits.get(b).first);
            return c != 0 ? c : rules.get(a).id.compareTo(rules.get(b).id);
        });
        if (!hit.isEmpty()) {
            o.append("-- rules\n");
            for (int k = 0; k < Math.min(12, hit.size()); k++) {
                Row r = rules.get(hit.get(k)); Hit h = hits.get(hit.get(k));
                o.append("  [").append(r.id).append("] ").append(r.kind).append(" ×").append(h.count).append(" L").append(h.first).append(": ").append(r.cause)
                        .append(r.doc.isEmpty() ? "" : " (" + r.doc + ")").append('\n');
            }
            if (hit.size() > 12) o.append("  +").append(hit.size() - 12).append(" more rules\n");
        }
        if (droppedKeys > 0) o.append("(").append(droppedKeys).append(" key-like line(s) left out)\n");
        o.append("(the launcher's short digest of the log's last ").append(TAIL_BYTES >> 20).append(" MB; python3 scripts/log_digest.py on the log gives the whole digest)\n");
        return o.toString();
    }

    // ---------------------------------------------------------------- the log's end

    /**
     * Reads the last tailBytes of a log: the lines from the first line start inside them (the whole file when it is smaller), numbered from the file's start (the
     * newlines before them are counted)
     */
    void readTail(File log, long tailBytes) throws IOException {
        name = log.getName();
        bytes = log.length();
        long from = Math.max(0, bytes - tailBytes);
        byte[] b;
        try (RandomAccessFile r = new RandomAccessFile(log, "r")) {
            if (from > 0) {   // start after the first \n at or after from - 1 (a line that starts at from is whole)
                r.seek(from - 1);
                int c;
                while ((c = r.read()) >= 0 && c != '\n') from++;
                if (c < 0) from = bytes;
            }
            b = new byte[(int) (bytes - from)];
            r.seek(from);
            r.readFully(b);
        }
        n = from > 0 ? countNewlines(log, from) : 0;
        feedText(new String(b, StandardCharsets.UTF_8));
    }

    private static int countNewlines(File f, long upTo) throws IOException {
        int c = 0;
        byte[] buf = new byte[1 << 16];
        try (InputStream in = new FileInputStream(f)) {
            long left = upTo;
            while (left > 0) {
                int k = in.read(buf, 0, (int) Math.min(buf.length, left));
                if (k < 0) break;
                for (int i = 0; i < k; i++) if (buf[i] == '\n') c++;
                left -= k;
            }
        }
        return c;
    }
}
