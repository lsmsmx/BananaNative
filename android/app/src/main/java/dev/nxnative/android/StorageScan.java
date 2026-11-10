package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Settings › Storage (StorageUi): what the app wrote that can go, found by name under the app's own folders only, and the deletion of what the player picked.
 * <ul>
 * <li>Only the kinds below are ever listed (an allow-list of names under known roots: the external files folder, the internal files folder, the cache, the
 *     no-backup folder). Nothing else in those folders is listed, whatever it is.</li>
 * <li>Saves are never listed or deleted: {@link #refuse} turns down any path that is a save root, lies inside one, holds one, or sits beside one with its name as a
 *     prefix (the save's working siblings: &lt;save&gt;.lock, _device, .restore, .pend1 …). The save roots are the fixed ones of {@link Roots} (saves/, save/,
 *     save-backups/, save-import/) and every save folder the caller adds (picked ones, the old defaults). It is checked when an item is listed and again right before it
 *     is deleted.</li>
 * <li>The pack of a listed game and the shader library it plays are not offered; {@link #gameData} lists them for one game when the player asks for exactly that.</li>
 * </ul>
 * Deletion does not follow symbolic links (a link is removed, never what it names). The caller refuses the scan while a game or Prepare runs.
 * No Android classes: the JVM test (StorageScanJvmTest, dm/tools/tests/test_android_storage.py) runs it on the Mac.
 */
final class StorageScan {
    private StorageScan() {}

    enum Kind {
        OLD_SHADERS("Old shader library versions", true, "Libraries of earlier app versions in packs that already have this version's library."),
        UNFINISHED("Unfinished copies and imports", true, "Left by a copy, pack import, driver install or restamp that was stopped part way."),
        DOWNLOADS("Downloads", true, "Update and driver downloads left in the cache."),
        FILEBACK("RAM saver files", true, "Memory files a game left behind when it was stopped."),
        OLD_LOGS("Old logs", true, "Run logs, except the newest of each game."),
        GAME_TEMP("Game temporary files", false, "Each game's temporary folder and pipeline cache. They come back as you play; the first minutes may stutter again."),
        UNUSED_PACKS("Packs of games not in your list", false, "Adding such a game again would need Prepare again."),
        UNUSED_COPIES("Game copies not in your list", false, "Copies made with Copy into the app that no game in the list uses."),
        GAME_DATA("Game data", false, "The game's pack with its shaders, temporary files, logs and DLC links. Play then needs Prepare again; saves stay.");

        final String title, about;
        final boolean preselected;
        Kind(String title, boolean preselected, String about) { this.title = title; this.preselected = preselected; this.about = about; }
    }

    /** One file or folder that can go */
    static final class Item {
        final Kind kind;
        final File file;
        final String label;
        final long bytes;
        Item(Kind kind, File file, String label, long bytes) { this.kind = kind; this.file = file; this.label = label; this.bytes = bytes; }
        @Override public String toString() { return kind + " " + label + " " + bytes; }
    }

    /** A game of the list: its title ID, game file or folder, pack folder */
    static final class Listed {
        final String titleId, gameDir, packDir;
        Listed(String titleId, String gameDir, String packDir) { this.titleId = titleId; this.gameDir = gameDir; this.packDir = packDir; }
    }

    /** Where the app writes, and the save folders that are never touched */
    static final class Roots {
        final File ext, files, cache, noBackup;
        final List<File> saves = new ArrayList<>();
        /** ext: AppStorage.root; files: getFilesDir; cache: getCacheDir; noBackup: getNoBackupFilesDir (null for none) */
        Roots(File ext, File files, File cache, File noBackup) {
            this.ext = ext; this.files = files; this.cache = cache; this.noBackup = noBackup;
            for (File r : new File[] {ext, files}) {
                if (r == null) continue;
                for (String n : new String[] {"saves", "save", "save-backups", "save-import"}) saves.add(new File(r, n));
            }
        }
        /** A save folder elsewhere (picked in Folders, an older default): it, its insides and its siblings named after it are protected too */
        Roots save(File f) { if (f != null) saves.add(f); return this; }
        List<File> all() {
            List<File> l = new ArrayList<>();
            for (File f : new File[] {ext, files, cache, noBackup}) if (f != null) l.add(f);
            return l;
        }
    }

    // ---- The guard ----

    private static String canon(File f) throws IOException { return f.getCanonicalPath(); }

    private static boolean inside(String child, String parent) { return child.startsWith(parent.endsWith(File.separator) ? parent : parent + File.separator); }

    /**
     * null when f may be deleted, else why not: outside the app's roots (or a root itself), or a save (the folder, inside it, holding it, or a working sibling named
     * after it). Compared by canonical paths; a path that cannot be resolved is refused.
     */
    static String refuse(Roots r, File f) {
        if (f == null) return "no path";
        String c;
        try { c = canon(f); } catch (IOException e) { return "cannot resolve " + f; }
        boolean in = false;
        for (File root : r.all()) {
            try {
                String rc = canon(root);
                if (c.equals(rc)) return "an app folder itself: " + f;
                if (inside(c, rc)) in = true;
            } catch (IOException ignored) { /* a root that cannot be resolved holds nothing */ }
        }
        if (!in) return "outside the app's folders: " + f;
        String name = new File(c).getName(), parent = new File(c).getParent();
        for (File s : r.saves) {
            String sc;
            try { sc = canon(s); } catch (IOException e) { return "a save folder cannot be resolved: " + s; }
            if (c.equals(sc) || inside(c, sc)) return "a save: " + f;
            if (inside(sc, c)) return "holds a save: " + f;
            File sf = new File(sc);
            if (parent != null && parent.equals(sf.getParent()) && name.startsWith(sf.getName())) return "beside a save, named after it: " + f;
        }
        return null;
    }

    // ---- Sizes and deletion (no link followed) ----

    static long size(File f) {
        Path p = f.toPath();
        if (Files.isSymbolicLink(p)) return 0;
        if (!Files.isDirectory(p)) return f.length();
        long[] n = {0};
        try {
            Files.walkFileTree(p, new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path x, BasicFileAttributes a) { if (a.isRegularFile()) n[0] += a.size(); return FileVisitResult.CONTINUE; }
                @Override public FileVisitResult visitFileFailed(Path x, IOException e) { return FileVisitResult.CONTINUE; }
            });
        } catch (IOException ignored) { /* what was counted */ }
        return n[0];
    }

    private static void deleteTree(Path p) throws IOException {
        if (Files.isSymbolicLink(p) || !Files.isDirectory(p)) { Files.deleteIfExists(p); return; }
        Files.walkFileTree(p, new SimpleFileVisitor<Path>() {   // walkFileTree does not follow links: a link inside is visited as a file and removed itself
            @Override public FileVisitResult visitFile(Path x, BasicFileAttributes a) throws IOException { Files.deleteIfExists(x); return FileVisitResult.CONTINUE; }
            @Override public FileVisitResult postVisitDirectory(Path d, IOException e) throws IOException { if (e != null) throw e; Files.deleteIfExists(d); return FileVisitResult.CONTINUE; }
        });
    }

    /** The result of a deletion: bytes freed, and a line for each item that was refused or not fully deleted */
    static final class Result {
        long freed;
        final List<String> problems = new ArrayList<>();
    }

    /** Deletes the items, each checked by the guard again first */
    static Result delete(Roots r, List<Item> items) {
        Result res = new Result();
        for (Item it : items) {
            String why = refuse(r, it.file);
            if (why != null) { res.problems.add("not removed (" + why + ")"); continue; }
            try {
                deleteTree(it.file.toPath());
                res.freed += it.bytes;
            } catch (IOException | SecurityException e) {
                res.problems.add("not all of " + it.label + " could be removed: " + e.getMessage());
            }
        }
        return res;
    }

    // ---- The scan ----

    static final String TITLE = "[0-9A-Fa-f]{16}";
    // The names other classes give their files, repeated so this class compiles alone (no Android class): PACK_PARTIAL / REPLACED, DRIVER_INSTALLING / REPLACED,
    // LogProvider.DIGEST; StorageScanJvmTest checks they still match
    static final String PACK_PARTIAL = ".partial", PACK_REPLACED = ".replaced", DRIVER_INSTALLING = ".installing", DRIVER_REPLACED = ".replaced-", DIGEST = ".digest.txt";

    private static File[] list(File d) {
        File[] l = d == null ? null : d.listFiles();
        if (l == null) return new File[0];
        Arrays.sort(l);
        return l;
    }

    private static void add(Roots r, List<Item> out, Kind k, File f, File root) {
        if (!f.exists() && !Files.isSymbolicLink(f.toPath())) return;
        if (refuse(r, f) != null) return;   // never listed: the deletion would refuse it anyway
        String label = root != null && f.getPath().startsWith(root.getPath() + File.separator) ? f.getPath().substring(root.getPath().length() + 1) : f.getPath();
        out.add(new Item(k, f, label, size(f)));
    }

    private static boolean same(File a, String b) {
        if (b == null) return false;
        try { return a.getCanonicalFile().equals(new File(b).getCanonicalFile()); } catch (IOException e) { return a.getAbsoluteFile().equals(new File(b).getAbsoluteFile()); }
    }

    private static boolean within(String path, File dir) {
        if (path == null) return false;
        try { String p = canon(new File(path)), d = canon(dir); return p.equals(d) || inside(p, d); } catch (IOException e) { return false; }
    }

    /**
     * What can go. current: this APK's DMSH_VK_CHAIN; keep: an update download waiting for its install (not listed), or null; listed: the game list (their packs and
     * copies are not offered)
     */
    static List<Item> scan(Roots r, List<Listed> listed, String current, File keep) {
        List<Item> out = new ArrayList<>();
        File packs = r.ext == null ? null : new File(r.ext, "packs");
        // Packs in app storage: unfinished imports, a replaced pack whose successor is in place, old library versions and restamp leftovers, packs of no listed game
        for (File p : list(packs)) {
            String n = p.getName();
            if (n.endsWith(PACK_PARTIAL) && n.substring(0, n.length() - PACK_PARTIAL.length()).matches(TITLE)) { add(r, out, Kind.UNFINISHED, p, r.ext); continue; }
            if (n.endsWith(PACK_REPLACED)) {
                String id = n.substring(0, n.length() - PACK_REPLACED.length());   // put back by PackZip.recover when the pack itself is missing: only then is it still needed
                if (id.matches(TITLE) && new File(new File(packs, id), "pack.tsv").isFile()) add(r, out, Kind.UNFINISHED, p, r.ext);
                continue;
            }
            if (!n.matches(TITLE) || !p.isDirectory()) continue;
            boolean used = false;
            for (Listed g : listed) used |= g.titleId.equalsIgnoreCase(n) || within(g.packDir, p);
            if (!used) { add(r, out, Kind.UNUSED_PACKS, p, r.ext); continue; }
            File sh = new File(p, "shaders");
            boolean haveCurrent = current != null && !current.isEmpty() && new File(new File(sh, "v-" + current), "dmsh_vk.lib").isFile();
            for (File v : list(sh)) {
                String vn = v.getName();
                if (vn.startsWith(".restamp-")) add(r, out, Kind.UNFINISHED, v, r.ext);
                // only once this version's library is there: until then an older one is what the game plays, and what a delta update starts from
                else if (haveCurrent && vn.startsWith("v-") && !vn.equals("v-" + current) && v.isDirectory()) add(r, out, Kind.OLD_SHADERS, v, r.ext);
            }
        }
        // Game copies: a cut copy (<name>.part), and copies no listed game uses
        for (File g : list(r.ext == null ? null : CopyPlan.games(r.ext))) {
            if (g.getName().endsWith(".part")) { add(r, out, Kind.UNFINISHED, g, r.ext); continue; }
            boolean used = false;
            for (Listed l : listed) used |= within(l.gameDir, g);
            if (!used) add(r, out, Kind.UNUSED_COPIES, g, r.ext);
        }
        // Logs: all but the newest of each game (names sort by time: <title ID>-yyyyMMdd-HHmmss.log), each with its digest
        Map<String, List<File>> logs = new HashMap<>();
        for (File f : list(r.ext == null ? null : new File(r.ext, "logs"))) {
            String n = f.getName();
            if (!n.endsWith(".log") || n.indexOf('-') <= 0) continue;
            logs.computeIfAbsent(n.substring(0, n.indexOf('-')), x -> new ArrayList<>()).add(f);
        }
        for (List<File> l : logs.values()) {
            l.sort(null);
            for (File f : l.subList(0, l.size() - 1)) {
                add(r, out, Kind.OLD_LOGS, f, r.ext);
                add(r, out, Kind.OLD_LOGS, new File(f.getParentFile(), f.getName().replaceFirst("\\.log$", "") + DIGEST), r.ext);
            }
        }
        // The games' temporary folders (DM_TEMP: temp/<title ID>); never temp/<id>_cache* (the game's cache storage) or anything else there
        for (File t : list(r.ext == null ? null : new File(r.ext, "temp"))) if (t.getName().matches(TITLE) && t.isDirectory()) add(r, out, Kind.GAME_TEMP, t, r.ext);
        // GPU drivers: a stopped install's leftovers (DriverZip.recover's rule: a .replaced-<name> is still needed while <name> is missing)
        File drivers = r.files == null ? null : new File(r.files, "gpu_drivers");
        for (File d : list(drivers)) {
            String n = d.getName();
            if (n.equals(DRIVER_INSTALLING)) add(r, out, Kind.UNFINISHED, d, r.files);
            else if (n.startsWith(DRIVER_REPLACED) && new File(drivers, n.substring(DRIVER_REPLACED.length())).exists()) add(r, out, Kind.UNFINISHED, d, r.files);
        }
        // The packer's unpacked tools: a stopped unpack (PackEngine: pack/<version>.tmp)
        for (File d : list(r.files == null ? null : new File(r.files, "pack"))) if (d.getName().endsWith(".tmp")) add(r, out, Kind.UNFINISHED, d, r.files);
        // The cache: update downloads (Updater: cache/updates/, not the one waiting for its install), driver downloads (DriverFetch: cache/driver-*.zip),
        // RAM saver files a stopped game left (the host's file-backed pools: cache/dm_fileback_*, unlinked at once while it runs)
        for (File f : list(r.cache == null ? null : new File(r.cache, "updates"))) if (keep == null || !same(f, keep.getPath())) add(r, out, Kind.DOWNLOADS, f, r.cache);
        for (File f : list(r.cache)) {
            String n = f.getName();
            if (n.startsWith("driver-") && n.endsWith(".zip")) add(r, out, Kind.DOWNLOADS, f, r.cache);
            else if (n.startsWith("dm_fileback_")) add(r, out, Kind.FILEBACK, f, r.cache);
        }
        return out;
    }

    /**
     * What "Remove game data" takes for one listed game: its pack when it is in the app's storage (with every shader library in it), its temporary folder, its logs and
     * its DLC links. Never its saves (the guard), never the game itself (Remove on its card takes a copy), never a pack another listed entry uses too.
     */
    static List<Item> gameData(Roots r, Listed g, List<Listed> listed) {
        List<Item> out = new ArrayList<>();
        File packs = r.ext == null ? null : new File(r.ext, "packs");
        if (packs != null && g.packDir != null && within(g.packDir, packs)) {
            boolean shared = false;
            for (Listed o : listed) if (o != g && !o.gameDir.equals(g.gameDir) && same(new File(g.packDir), o.packDir)) shared = true;
            if (!shared) add(r, out, Kind.GAME_DATA, new File(g.packDir), r.ext);
        }
        if (r.ext != null) {
            add(r, out, Kind.GAME_DATA, new File(r.ext, "temp/" + g.titleId), r.ext);
            for (File f : list(new File(r.ext, "logs"))) if (f.getName().startsWith(g.titleId + "-")) add(r, out, Kind.GAME_DATA, f, r.ext);
        }
        if (r.files != null) add(r, out, Kind.GAME_DATA, new File(r.files, "aoc/" + g.titleId), r.files);
        return out;
    }

    // ---- Words ----

    static long total(List<Item> l) { long n = 0; for (Item i : l) n += i.bytes; return n; }

    static String bytes(long b) {
        if (b < 1024) return b + " B";
        if (b < 1L << 20) return String.format(Locale.ROOT, "%.0f KB", b / 1024.0);
        if (b < 1L << 30) return String.format(Locale.ROOT, "%.1f MB", b / (double) (1L << 20));
        return String.format(Locale.ROOT, "%.2f GB", b / (double) (1L << 30));
    }

    /** The items grouped by kind, in the kinds' order */
    static Map<Kind, List<Item>> byKind(List<Item> l) {
        Map<Kind, List<Item>> m = new java.util.EnumMap<>(Kind.class);
        for (Item i : l) m.computeIfAbsent(i.kind, x -> new ArrayList<>()).add(i);
        return m;
    }

    /** Distinct files (an item listed twice is deleted once) */
    static List<Item> distinct(List<Item> l) {
        Set<String> seen = new HashSet<>();
        List<Item> out = new ArrayList<>();
        for (Item i : l) if (seen.add(i.file.getAbsolutePath())) out.add(i);
        return out;
    }
}
