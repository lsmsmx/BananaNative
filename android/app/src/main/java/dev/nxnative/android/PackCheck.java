package dev.nxnative.android;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Whether a pack folder fits the picked game folder and this APK, in three parts, each with a sentence the user can act on:
 * <ul>
 * <li>{@link #build}: the pack is of the game build in the folder. pack.tsv's version and its build rows (main=…, sdk=…: the NSO build IDs the code was rewritten
 *     from) against the folder's update of that version, and that build against the builds this APK's host of that game supports (game.tsv build rows, Profile.buildMap).
 *     A mismatch means the rewritten code is of another game version: the game would crash, so the launcher does not start it and offers Prepare again.</li>
 * <li>{@link #shaders}: the pack has this APK's shader library (Pack.libraryProblem).</li>
 * <li>{@link #code} (slower: hashes the modules, so it runs off the main thread): the pack's code was rewritten by this APK's rewriter. The key
 *     scripts/pack_android.py writes as &lt;module&gt;.android.key (rewrite_key: the rewriter's files, the module, the profile's patches.py and game.tsv's build rows) is
 *     computed here (RewriteKey, which also accepts a key made with an earlier rewriter file of identical output, dm/tools/rewriter_equiv, and one of the earlier
 *     formula over the whole game.tsv, the APK's or the pack's copy when its build rows are the APK's) from the tools in the APK (assets/pack/tools.zip) and the folder's module. Another key is not an error: a pack made by an earlier commit
 *     usually runs; the card says so and Prepare again brings the code up to date (the shaders are kept).</li>
 * </ul>
 */
final class PackCheck {
    private PackCheck() {}

    /** A module of the pack's version in a game (BuildCheck.mod) */
    static BuildCheck.Mod mod(File game, String version, String m) { return BuildCheck.mod(game, version, m); }

    /**
     * null when the pack's code can be started with the game folder, else why not: a module whose rewrite did not finish (no key file: Pack.unfinishedModule),
     * a job rewriting this pack's code now or stopped while it did (the job beside the dump, or the one the launcher shows), or code of another game build (build)
     */
    static String blocking(Context c, Pack p, File game) {
        String m = p.unfinishedModule();
        if (m != null) return "The pack's game code is unfinished (" + m + ".android.key is missing: an update of the pack was stopped part way). Prepare again finishes it (a few minutes; shaders are kept).";
        Job j = rewriting(c, p.dir, game, p.titleId);
        if (j != null) return "The pack's game code is being rewritten (" + j.name() + ": " + j.statusText(false) + "). Let preparing finish first.";
        return build(p, game);
    }

    /** The job that is rewriting the code of this pack folder or stopped while it did (its rewrite step is running in job.json), else null */
    static Job rewriting(Context c, File packDir, File game, String titleId) {
        File[] cands = {Job.remembered(c), Job.fileOf(DumpCheck.workOf(game, titleId))};
        for (File f : cands) {
            if (f == null || !f.isFile()) continue;
            try {
                Job j = Job.read(f);
                if (j.pack().getAbsolutePath().equals(packDir.getAbsolutePath()) && !"done".equals(j.state()) && "running".equals(j.stepState("rewrite"))) return j;
            } catch (IOException e) {
                // not a job of this app's format: it rewrites nothing
            }
        }
        return null;
    }

    /** null when the pack's code is of the game build in the folder and of a build this APK supports, else why not (BuildCheck.build; the build IDs that differ are in the message) */
    static String build(Pack p, File game) { return BuildCheck.build(p, game).message; }

    /** null when the pack has this APK's shader library, else why not */
    static String shaders(File packDir, String titleId) { return Pack.libraryProblem(packDir, titleId); }

    // ---- The rewriter's key (slow: off the main thread) ----

    private static final Map<String, Boolean> codeCache = new HashMap<>();
    private static Map<String, byte[]> tools;   // the rewriter's files from the APK's tools.zip, by repository path

    /**
     * True when every module's &lt;module&gt;.android.key in the pack is the key this APK's rewriter makes from the folder's module, false when one differs,
     * null when it cannot be told (no tools in this APK, or no key file). Cached by the files' sizes and times.
     */
    static Boolean code(Context c, Pack p, File game) {
        String[] mods = p.modules;
        String gv = p.gameVersion(game);   // the dump's version with the pack's build (BuildCheck.build)
        StringBuilder id = new StringBuilder(p.dir.getPath()).append('|').append(game.getPath());
        for (String m : mods) {
            File k = new File(p.dir, m + ".android.key");
            id.append('|').append(k.length()).append(':').append(k.lastModified()).append(':').append(mod(game, gv, m).stamp());
        }
        File gt = new File(p.dir, "game.tsv");   // the pack's copy: an earlier key may have been made with it (RewriteKey)
        id.append('|').append(gt.length()).append(':').append(gt.lastModified());
        synchronized (codeCache) {
            if (codeCache.containsKey(id.toString())) return codeCache.get(id.toString());
        }
        Boolean r = codeNow(c, p, game);
        synchronized (codeCache) { codeCache.put(id.toString(), r); }
        return r;
    }

    private static Boolean codeNow(Context c, Pack p, File game) {
        Map<String, byte[]> t = tools(c);
        if (t == null) return null;
        String gd = "dm/games/" + p.titleId + "/";
        if (!t.containsKey(gd + "game.tsv")) return null;   // the APK carries the profiles of its own games only: another game's pack is not this APK's to judge
        byte[] packGt = null;   // the game.tsv the pack was made with (pack_android.py copies it after the rewrite): an earlier formula's key counts with it (RewriteKey)
        File gf = new File(p.dir, "game.tsv");
        try { if (gf.isFile()) packGt = Files.readAllBytes(gf.toPath()); } catch (IOException e) { packGt = null; }
        for (String m : p.modules) {
            File kf = new File(p.dir, m + ".android.key");
            BuildCheck.Mod nso = mod(game, p.gameVersion(game), m);
            if (!kf.isFile()) { android.util.Log.w("nxnative", p.titleId + ": the pack has no " + kf.getName() + ": its rewrite did not finish (or the pack is from before keys); ⋯ › Prepare again remakes it"); return null; }
            if (!nso.exists()) return null;
            try {
                String have = new String(Files.readAllBytes(kf.toPath()), StandardCharsets.UTF_8).trim();
                if (!RewriteKey.keys(t, BuildConfig.NX_REWRITER.split(" "), gd, m, nso::hash, packGt).contains(have)) return false;   // this rewriter's key, an equivalent one's, an earlier formula's (RewriteKey)
            } catch (IOException e) {
                return null;
            }
        }
        return true;
    }

    /** dm/tools/*.py, dm/tools/rewriter_equiv/* and dm/games/&lt;id&gt;/{patches.py,game.tsv} from assets/pack/tools.zip; null without it (-PnxPackEngine=fake) */
    private static synchronized Map<String, byte[]> tools(Context c) {
        if (tools != null) return tools;
        Map<String, byte[]> out = new HashMap<>();
        try (InputStream in = c.getAssets().open("pack/tools.zip"); ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                String n = e.getName();
                boolean want = n.startsWith("dm/tools/") && n.indexOf('/', 9) < 0 || n.startsWith(RewriteKey.EQUIV) && n.indexOf('/', RewriteKey.EQUIV.length()) < 0
                        || n.startsWith("dm/games/") && (n.endsWith("/patches.py") || n.endsWith("/game.tsv"));
                if (want && !e.isDirectory()) out.put(n, z.readAllBytes());
            }
        } catch (IOException e) {
            return null;
        }
        tools = out;
        return tools;
    }
}
