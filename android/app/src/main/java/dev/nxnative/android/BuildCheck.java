package dev.nxnative.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Which game build a dump (a folder or a .nup) holds, and whether a pack's code was rewritten from it ({@link #build}, PackCheck's first part), from the NSO
 * build IDs: the pack's build rows against the dump's modules of the pack's version, then that build against the builds this APK's profile supports. Also the
 * key Home's list tells one game from another by ({@link #gameKey}). No Android classes: the JVM test (BuildCheckJvmTest) runs it on the Mac, on a .nup fixture and,
 * when they are there, on the real dumps.
 */
final class BuildCheck {
    private BuildCheck() {}

    /** What {@link #build} found: OK, or the kind of problem, a sentence for the user (message) and the build IDs compared (detail, for the card and the log) */
    enum Kind { OK, OTHER_BUILD, MISSING, UNREADABLE, UNSUPPORTED, ROMFS }

    static final class Result {
        final Kind kind;
        final String message, detail;

        Result(Kind kind, String message, String detail) { this.kind = kind; this.message = message; this.detail = detail; }

        boolean ok() { return kind == Kind.OK; }

        /** The card's short state */
        String label() {
            switch (kind) {
                case OTHER_BUILD: return "Pack is for another build";
                case MISSING: return "Pack is for another update";
                case UNREADABLE: return "Game file cannot be read";
                case UNSUPPORTED: return "Untested game version";
                case ROMFS: return "Game data of another version";
                default: return null;
            }
        }
    }

    static final Result OK = new Result(Kind.OK, null, "");

    /**
     * The folder's module of the pack's version: updates/&lt;version&gt;/exefs/&lt;module&gt;, else the base's (an update without its own ExeFS: DumpLayout), base/exefs
     * for version 0, or a one-version folder's exefs/
     */
    static File module(File game, String version, String m) {
        long v;
        try { v = Long.parseLong(version); } catch (NumberFormatException e) { return new File(game, "updates/" + version + "/exefs/" + m); }
        File f = new File(DumpLayout.path(game, v, "exefs/" + m));
        if (f.isFile()) return f;
        File g = new File(game, "exefs/" + m);
        return g.isFile() ? g : f;
    }

    /**
     * A module of the pack's version in a game: a file of a folder (module), or a file of a .nup's package (update/&lt;version&gt;, base for 0).
     * label names it in messages as the folder's relative path does ("updates/458752/exefs/main", "update/458752/exefs/main" in a .nup)
     */
    static final class Mod {
        final File file;        // a folder's file
        final Nup nup;          // or a .nup's
        final Nup.Node node;
        final String label;
        final String error;     // the .nup could not be read

        private Mod(File file, Nup nup, Nup.Node node, String label, String error) { this.file = file; this.nup = nup; this.node = node; this.label = label; this.error = error; }

        boolean exists() { return file != null ? file.isFile() : node != null && !node.dir; }

        /** The first n bytes (fewer when it is shorter) */
        byte[] head(int n) throws IOException {
            if (error != null) throw new IOException(error);
            if (nup != null) return nup.read(node, n);
            try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(file, "r")) {
                byte[] b = new byte[(int) Math.min(n, r.length())];
                r.readFully(b);
                return b;
            }
        }

        /** Its bytes into the hash */
        void hash(MessageDigest h) throws IOException {
            if (error != null) throw new IOException(error);
            if (nup != null) { h.update(nup.read(node, -1)); return; }
            try (InputStream in = new FileInputStream(file)) {
                byte[] buf = new byte[1 << 16];
                int k;
                while ((k = in.read(buf)) > 0) h.update(buf, 0, k);
            }
        }

        /** What changes when it changes (the code check's cache) */
        String stamp() {
            if (file != null) return file.length() + ":" + file.lastModified();
            return nup == null ? "none" : nup.identity + ":" + label;
        }
    }

    static Mod mod(File game, String version, String m) {
        if (!Nup.isNup(game)) {
            File f = module(game, version, m);
            return new Mod(f, null, null, rel(game, f), null);
        }
        long v;
        try { v = Long.parseLong(version); } catch (NumberFormatException e) { v = -1; }
        String sel = v < 0 ? "update/" + version : DumpLayout.selector(game, v, false), label = sel + "/exefs/" + m;
        try {
            Nup n = Nup.open(game);
            Nup.Pkg p = n.find(sel);
            return new Mod(null, n, p == null ? null : n.lookup(p, "exefs/" + m), label, null);
        } catch (IOException e) {
            return new Mod(null, null, null, label, e.getMessage());
        }
    }

    private static String rel(File game, File f) {
        String g = game.getPath() + "/", p = f.getPath();
        return p.startsWith(g) ? p.substring(g.length()) : p;
    }

    /** An NSO's build ID (64 hex digits, lower case) from its first 0x60 bytes */
    static String nsoBuildId(byte[] h, String name) throws IOException {
        if (h.length < 0x60 || h[0] != 'N' || h[1] != 'S' || h[2] != 'O' || h[3] != '0') throw new IOException(name + " is not an NSO");
        StringBuilder b = new StringBuilder();
        for (int i = 0x40; i < 0x60; i++) b.append(String.format(Locale.ROOT, "%02x", h[i] & 0xff));
        return b.toString();
    }

    /** The pack's build rows: module → build ID (32 hex digits, as pack_android.py writes them) */
    static Map<String, String> builds(Pack p) {
        Map<String, String> out = new LinkedHashMap<>();
        for (String b : p.builds) {
            int eq = b.indexOf('=');
            if (eq > 0) out.put(b.substring(0, eq), b.substring(eq + 1).toLowerCase(Locale.ROOT));
        }
        return out;
    }

    private static String shortId(String id) { return id.length() > 16 ? id.substring(0, 16) : id; }

    /**
     * Whether the pack's code is of the game build in the folder (or .nup) and of a build this APK supports. Every module of the pack's build rows is compared
     * (not only the first that differs), so detail lists all of them: "main: pack e25c90c1c1366388, game a1dd7480bca7b6e8 (update/458752/exefs/main)"
     */
    static Result build(Pack p, File game) {
        Map<String, String> want = builds(p);
        if (want.isEmpty()) return OK;   // a pack from before the build rows: nothing to compare
        String name = p.name.isEmpty() ? p.titleId : p.name;
        StringBuilder diff = new StringBuilder();
        String firstMod = null, firstWant = null, firstHave = null;
        String gv = p.gameVersion(game);   // the dump's version with the pack's build IDs (the same code under another version number plays as it is), else the pack's
        for (Map.Entry<String, String> e : want.entrySet()) {
            Mod nso = mod(game, gv, e.getKey());
            if (nso.error != null) return new Result(Kind.UNREADABLE, nso.error, nso.label + ": " + nso.error);
            if (!nso.exists())
                return new Result(Kind.MISSING, "The pack is for " + name + " " + Profile.versionName(parse(p.version)) + ", but the game " + (nso.nup != null ? ".nup" : "folder")
                        + " has no " + nso.label + ". Pick the dump that has that update, or Prepare again from this one.", "no " + nso.label + " (pack version " + p.version + ")");
            String id;
            try {
                id = nsoBuildId(nso.head(0x60), nso.label);
            } catch (IOException ex) {
                return new Result(Kind.UNREADABLE, nso.label + " cannot be read: " + ex.getMessage(), nso.label + ": " + ex.getMessage());
            }
            if (!id.startsWith(e.getValue())) {
                diff.append(diff.length() == 0 ? "" : "; ").append(e.getKey()).append(": pack ").append(shortId(e.getValue())).append(", game ").append(shortId(id))
                        .append(" (").append(nso.label).append(')');
                if (firstMod == null) { firstMod = e.getKey(); firstWant = e.getValue(); firstHave = id; }
            }
        }
        if (firstMod != null)
            return new Result(Kind.OTHER_BUILD, "The pack was made from another build of " + name + " (" + firstMod + " " + shortId(firstWant) + "…) than the one in the game "
                    + (Nup.isNup(game) ? ".nup" : "folder") + " (" + shortId(firstHave) + "…, " + Profile.versionName(parse(p.version))
                    + "). Its code would crash with this game: Prepare again rewrites the game's code from this dump (a few minutes; shaders are kept).\n\nBuild IDs that differ: " + diff, diff.toString());
        Profile prof = Profile.of(p.titleId);
        Map<Long, Map<String, String>> sup = prof == null ? Map.of() : prof.buildMap();
        if (!sup.isEmpty()) {
            // one of the profile's builds, by its build IDs (not by the version number: a dump may keep that build's code in another package, DumpLayout)
            boolean ok = false;
            for (Map<String, String> row : sup.values()) ok |= sameBuild(row, want);
            if (!ok) {
                Map<String, String> row = sup.get(parse(p.version));
                StringBuilder bad = new StringBuilder();
                if (row != null) for (Map.Entry<String, String> e : row.entrySet()) {
                    String have = want.get(e.getKey());
                    if (have != null && !have.startsWith(e.getValue()) && !e.getValue().startsWith(have))
                        bad.append(bad.length() == 0 ? "" : "; ").append(e.getKey()).append(": pack ").append(shortId(have)).append(", app ").append(shortId(e.getValue()));
                }
                StringBuilder s = new StringBuilder();
                for (Long v : sup.keySet()) s.append(s.length() == 0 ? "" : ", ").append(v);
                // a pack made with Try anyway (or elsewhere) for a build the profile does not list: Play anyway can start it, after a warning (MainActivity.launch)
                return new Result(Kind.UNSUPPORTED, "The pack is for " + name + " (" + Profile.versionName(parse(p.version)) + "), a version this app has not been tested with (it supports "
                        + prof.supportedShort() + ").", (row == null ? "pack version " + p.version + ", app supports " + s : bad.toString()) + "\n" + prof.supported());
            }
        }
        // its RomFS is the data of that version (RomfsCheck: game.tsv romfs_check), else the game takes another version's data and may crash
        String romfs;
        try { romfs = p.romfsPath(game); } catch (IOException e) { romfs = null; }
        String bad = RomfsCheck.check(prof, romfs, want.get("main"), parse(gv));
        if (bad != null) {
            int a = bad.indexOf('('), b = bad.lastIndexOf(").");
            return new Result(Kind.ROMFS, bad, a >= 0 && b > a ? bad.substring(a + 1, b) : bad);
        }
        return OK;
    }

    /** Whether a pack's build rows are a profile build row's (every module both name, by prefix: game.tsv keeps 16 bytes) */
    private static boolean sameBuild(Map<String, String> row, Map<String, String> want) {
        if (row.isEmpty()) return false;
        for (Map.Entry<String, String> e : row.entrySet()) {
            String have = want.get(e.getKey());
            if (have != null && !have.startsWith(e.getValue()) && !e.getValue().startsWith(have)) return false;
        }
        return true;
    }

    private static long parse(String version) {
        try { return Long.parseLong(version); } catch (NumberFormatException e) { return -1; }
    }

    // ---- One game, one card ----

    /**
     * What tells one game dump from another on Home's list: "&lt;title ID&gt;:&lt;version&gt;:&lt;main's build ID&gt;" of the newest version the dump holds (a .nup's
     * newest version with its own exefs/main: DumpLayout). Two dumps with the same key are the same game build
     * (a folder and the .nup made from it, two spellings of one path): one card. null when it cannot be told (no main, unreadable)
     */
    static String gameKey(File game, String titleId) {
        try {
            long v = DumpLayout.newest(game);
            if (v < 0) return null;
            String version = String.valueOf(v);
            Mod m = mod(game, version, "main");
            if (!m.exists()) return null;
            String id = nsoBuildId(m.head(0x60), m.label);
            return titleId.toUpperCase(Locale.ROOT) + ":" + version + ":" + id.substring(0, 32);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }
}
