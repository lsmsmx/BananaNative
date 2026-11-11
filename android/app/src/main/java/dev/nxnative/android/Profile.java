package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * A game this APK runs: its profile (dm/games/&lt;title ID&gt;) was compiled into a host of its own, libnxhost_&lt;title ID&gt;.so (build.gradle nxGames,
 * src/main/cpp/CMakeLists.txt), and what the launcher needs of it comes from build.gradle as BuildConfig.NX_PROFILES: one line per game, tab-separated: title ID,
 * name, the shader count Prepare translates, the shader inputs digest, the builds (game.tsv build rows, "version module=build ID …", ';' between builds), and, when the APK has them, the readiness rows (the profile's readiness.tsv: "level|fact|min|what", ';' between rows: Readiness.parseReqs), its status row (playable or bring-up; dm/games/README.md "Readiness rows"), the shader delta rows (vk_delta.tsv's "from" rows: "version=count", ',' between them), its "stamp" rows ("version:old SHA-256:new SHA-256", ';' between them), what Game resolution sets (resolutionEnv), the RomFS checks (romfsChecks), the GPU timestamp mode (gpuTs), the default Game resolution (window), whether the game is paced (decoupled), dynamic resolution (drs) and the default frame limit (fpsLimit).
 * A game without a profile here cannot run in this APK (its host has no profile for it): the launcher says so instead of starting it.
 * No Android classes: the JVM test (android/app/src/jvmtest) runs it on the Mac.
 */
final class Profile {
    final String titleId, name, shaderInputs, builds, reqs, status;
    final int shaderTotal;
    String delta = "";   // the shader delta manifest's "from" rows: "version=shaders to translate", ',' between them (ShaderLibs.toTranslate)
    String stamp = "";   // its "stamp" rows: "version:old SHA-256:new SHA-256", ';' between them (ShaderLibs.restamp)
    String resolution = "";   // what Game resolution sets (build.gradle resolutionOf): "window", "scale:<choice>=<scale>/<console|handheld>,…" or "" (nothing: not offered)
    String gpuTs = "submission";   // game.tsv gpu_timestamps (build.gradle gpuTsOf): "segment" or "submission", what the host does without DM_NVN_GPU_TS (Game settings › GPU timing)
    String window = "";   // game.tsv android_window (build.gradle windowOf): the default Game resolution when the player chose none (Prefs.gameWindow), "" for the main screen's
    int fpsLimit;   // game.tsv fps_limit (build.gradle fpsLimitOf): the host's frame limit by default, frames a second (0 = none); the launcher offers a frame limit for it (Prefs.limited)
    boolean drs;   // game.tsv has drs_ctl rows (build.gradle drsOf): the host can steer the game's dynamic resolution (Prefs.drsGame)
    boolean decoupled;   // game.tsv has fps_decouple rows (build.gradle decoupleOf): the game's speed follows its measured frame rate, so it gets a frame limit (Prefs.paced)
    String romfsChecks = "";   // game.tsv romfs_check rows (build.gradle romfsChecksOf): "<path>|<line>|<expected or main>", ';' between them (RomfsCheck)

    /** Whether Game resolution does something for this game (its profile has window_fn, res_levels or graphics_scale rows) */
    boolean hasResolution() { return resolution.equals("window") || resolution.startsWith("scale:"); }

    /**
     * The host's environment lines for a Game resolution choice (Prefs.WINDOWS: "original", "2560x1440", "1920x1080", "1280x720", "960x540", "848x480"): DM_WINDOW_SIZE=&lt;choice&gt; for a game with
     * window_fn rows (its window size) or res_levels rows (a cap on its dynamic resolution); DM_GRAPHICS_SCALE and DM_OPMODE from the choice's graphics_scale row for a game that sizes its scenes by the graphics scale. Such a
     * game has only the few sizes its scales give, so a size choice without a row of its own takes the row of the largest size that fits in it, or the smallest row when none fits (Phone 540p on a game whose
     * smallest is 1280x720); Original without a row sets nothing (the game's own). None for a game with neither.
     */
    List<String> resolutionEnv(String choice) {
        List<String> env = new ArrayList<>();
        if (resolution.equals("window")) env.add("DM_WINDOW_SIZE=" + choice);
        else {
            int[] m = scaleRow(choice);
            if (m != null) { env.add("DM_GRAPHICS_SCALE=" + m[0]); env.add("DM_OPMODE=" + m[1]); }
        }
        return env;
    }

    /**
     * Whether Game resolution changes while the game runs (the in-game panel): only for a game with graphics_scale rows, whose host changes the graphics scale and operation mode
     * live and posts the console's notifications (Native.graphicsModeSet). A window_fn game's window size is written into its code at load, and a res_levels game's level lists
     * are rewritten in its read-only data at load (loader.c): both apply at the next start.
     */
    boolean resolutionLive() { return resolution.startsWith("scale:"); }

    /**
     * The graphics scale and operation mode for a choice while the game runs (Native.graphicsModeSet): the row resolutionEnv takes, or {-1, -1} where it sets nothing
     * (Original without a row: the game's own, as a start without the lines); null for a game without graphics_scale rows.
     */
    int[] resolutionLive(String choice) {
        if (!resolutionLive()) return null;
        int[] m = scaleRow(choice);
        return m != null ? m : new int[] {-1, -1};
    }

    /** The choice's graphics_scale row as {scale, operation mode (0 handheld, 1 console)}: its own row, else the largest that fits in it, else the smallest; null when none applies */
    private int[] scaleRow(String choice) {
        String r = scalePick(choice);
        if (r == null) return null;
        int eq = r.indexOf('='), sl = r.indexOf('/');
        try { return new int[] {Integer.parseInt(r.substring(eq + 1, sl)), r.substring(sl + 1).equals("handheld") ? 0 : 1}; } catch (NumberFormatException e) { return null; }
    }

    /**
     * A choice's label for this game: Prefs' name, plus the size the game really draws when its graphics_scale rows have no row of that size
     * ("Phone 540p (960x540) · draws 1280x720 here"), so a choice that falls back to another does not look like it did nothing
     */
    String windowLabel(String choice, String name) {
        String r = scalePick(choice);
        if (r == null || r.startsWith(choice + "=")) return name;
        String c = r.substring(0, r.indexOf('='));
        return size(c) == null ? name : name + " \u00b7 draws " + c + " here";
    }

    /** The graphics_scale row a choice takes ("WxH=scale/mode"): its own, else the largest that fits in it, else the smallest; null without rows */
    private String scalePick(String choice) {
        if (!resolution.startsWith("scale:")) return null;
        String hit = null, fit = null, least = null;
        long fitPx = -1, leastPx = Long.MAX_VALUE;
        int[] want = size(choice);
        for (String r : resolution.substring(6).split(",")) {
            int eq = r.indexOf('='), sl = r.indexOf('/');
            if (eq <= 0 || sl < eq) continue;
            String c = r.substring(0, eq);
            if (c.equals(choice)) { hit = r; break; }
            int[] s = size(c);
            if (s == null || want == null) continue;
            long px = (long) s[0] * s[1];
            if (s[0] <= want[0] && s[1] <= want[1] && px > fitPx) { fit = r; fitPx = px; }
            if (px < leastPx) { least = r; leastPx = px; }
        }
        return hit != null ? hit : fit != null ? fit : least;
    }

    /** "WxH" as {w, h}; null for "original" or anything else */
    private static int[] size(String c) {
        int x = c.indexOf('x');
        if (x <= 0) return null;
        try { return new int[] {Integer.parseInt(c.substring(0, x)), Integer.parseInt(c.substring(x + 1))}; } catch (NumberFormatException e) { return null; }
    }

    /** Whether any game of this APK has Game resolution (Settings shows its default only then) */
    static boolean anyResolution() { for (Profile p : all()) if (p.hasResolution()) return true; return false; }

    Profile(String titleId, String name, int shaderTotal, String shaderInputs, String builds, String reqs, String status) {
        this.reqs = reqs;
        this.status = status;
        this.titleId = titleId;
        this.name = name;
        this.shaderTotal = shaderTotal;
        this.shaderInputs = shaderInputs;
        this.builds = builds;
    }

    /** The profiles of a BuildConfig.NX_PROFILES table; a malformed line is skipped */
    static List<Profile> parse(String table) {
        List<Profile> out = new ArrayList<>();
        for (String line : table.split("\n")) {
            String[] c = line.split("\t", -1);
            if (c.length < 5 || !c[0].matches("[0-9A-F]{16}")) continue;
            int total;
            try { total = Integer.parseInt(c[2]); } catch (NumberFormatException e) { total = 0; }
            Profile p = new Profile(c[0], c[1].isEmpty() ? c[0] : c[1], total, c[3], c[4], c.length > 5 ? c[5] : "", c.length > 6 ? c[6] : "");
            p.delta = c.length > 7 ? c[7] : "";
            p.stamp = c.length > 8 ? c[8] : "";
            p.resolution = c.length > 9 ? c[9] : "";
            p.romfsChecks = c.length > 10 ? c[10] : "";
            if (c.length > 11 && !c[11].isEmpty()) p.gpuTs = c[11];
            p.window = c.length > 12 ? c[12] : "";
            p.decoupled = c.length > 13 && c[13].equals("1");
            p.drs = c.length > 14 && c[14].equals("1");
            try { p.fpsLimit = c.length > 15 && !c[15].isEmpty() ? Math.max(0, Integer.parseInt(c[15])) : 0; } catch (NumberFormatException e) { p.fpsLimit = 0; }
            out.add(p);
        }
        return Collections.unmodifiableList(out);
    }

    private static List<Profile> all;

    /** The games this APK runs, in build.gradle's order (the first is the default) */
    static synchronized List<Profile> all() {
        if (all == null) all = parse(BuildConfig.NX_PROFILES);
        return all;
    }

    /** This APK's profile of a title ID, null when it has none */
    static Profile of(String titleId) {
        for (Profile p : all()) if (p.titleId.equals(titleId)) return p;
        return null;
    }

    /** The default game (the first): the graphics self-test runs its host */
    static Profile first() { return all().get(0); }

    /**
     * Why this APK cannot run a title, in one plain sentence (null when it has its profile). Nothing inside this app can fix it (the host has the profile compiled in),
     * so the sentence says what to get and no button offers Prepare or Retry. name: the game's name when known (a pack's, a job's), else null. The title IDs this app
     * runs are in Settings › About
     */
    static String missing(String titleId, String name) {
        if (of(titleId) != null) return null;
        return "This app version does not include " + (name == null || name.isEmpty() || name.equals(titleId) ? "this game" : name) + " (" + titleId + "). Install a build that includes it.";
    }

    /** A version of a dump in words: 0 is the base game itself (no update installed; Mario Kart World 1.8.0's supported build), any other an update */
    static String versionName(long version) { return version == 0 ? "the base game" : "update " + version; }

    /** Which versions of this game the APK supports, as a sentence for a dump it refuses (DumpCheck): its builds, by display version when game.tsv names one */
    String supported() {
        List<String> all = new ArrayList<>();
        for (Long v : buildMap().keySet()) all.add(buildName(v));
        return "This app supports " + name + " (" + titleId + ") " + String.join(" or ", all) + " only.";
    }

    /** The builds this APK supports, in the player's words only ("1.8.0", "update 458752 or the base game"), for a refusal's main text (supported() has the build IDs) */
    String supportedShort() {
        List<String> all = new ArrayList<>();
        for (Long v : buildMap().keySet()) { String d = display(v); all.add(d != null ? d : versionName(v)); }
        return String.join(" or ", all);
    }

    /** One of the profile's builds in words: "1.8.0 (the base game, version 0, main d00e4f399e411d06…)", "update 458752 (main e25c90c1c1366388…)" */
    String buildName(long v) {
        String d = display(v), main = buildMap().getOrDefault(v, Map.of()).get("main");
        String ids = main == null ? "" : "main " + main.substring(0, Math.min(16, main.length())) + "…";
        if (d != null) return d + " (" + (v == 0 ? "the base game, version 0" : "update " + v) + (ids.isEmpty() ? "" : ", " + ids) + ")";
        return versionName(v) + (ids.isEmpty() ? "" : " (" + ids + ")");
    }

    /** The display version game.tsv's build row gives a build ("1.8.0"), null when it gives none */
    String display(long v) {
        for (String b : builds.split(";")) {
            String[] c = b.trim().split(" ");
            if (c.length > 1 && c[0].equals(String.valueOf(v))) for (String t : c) if (t.startsWith("@")) return t.substring(1);
        }
        return null;
    }

    /** The profile's builds: version → module → build ID (the first 16 bytes, lower case, as game.tsv writes them) */
    Map<Long, Map<String, String>> buildMap() {
        Map<Long, Map<String, String>> out = new LinkedHashMap<>();
        for (String b : builds.split(";")) {
            String[] c = b.trim().split(" ");
            if (c.length < 2 || !c[0].matches("\\d+")) continue;
            Map<String, String> m = new LinkedHashMap<>();
            for (int i = 1; i < c.length; i++) {
                int eq = c[i].indexOf('=');
                if (eq > 0) m.put(c[i].substring(0, eq), c[i].substring(eq + 1).toLowerCase(Locale.ROOT));
            }
            out.put(Long.parseLong(c[0]), m);
        }
        return out;
    }
}
