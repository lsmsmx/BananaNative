package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** A pack folder made by scripts/pack_android.py (its pack.tsv) and what it says about a game folder. */
final class Pack {
    static final int FORMAT = 1;

    final File dir;
    final String titleId, name, version;
    final String romfsRel;
    final String shadersRel;   // the shader library's path in the pack (its "shaders" row), null when the pack has none
    final boolean simulated;   // made by the on-device flow's simulated engine (FakeEngine): a placeholder, never started
    final Map<Integer, String> aoc = new LinkedHashMap<>();   // DLC index → its RomFS folder inside the game folder (a .nup game: <name>.nup:addon/<tid>/romfs beside it)
    final String[] modules;   // the profile's modules, rewritten in the pack
    final List<String> builds;   // "main=<build ID>" rows: the NSO builds the code was rewritten from (PackCheck.build)
    final String packer;   // the commit of the tools that made it ("" when not recorded)
    final String madeBy;   // its made_by row: "app <version>" when this app's pack step made it, "" for a pack from other tools (or from before the row)

    private Pack(File dir, Map<String, List<String>> rows) throws IOException {
        this.dir = dir;
        String format = one(rows, "format");
        if (!String.valueOf(FORMAT).equals(format)) throw new IOException("pack format " + format + ", this launcher reads " + FORMAT + ": pack again with this commit's scripts/pack_android.py");
        if (!"android".equals(one(rows, "target"))) throw new IOException("not an Android pack (target " + one(rows, "target") + ")");
        titleId = one(rows, "title_id").toUpperCase(Locale.ROOT);
        name = one(rows, "name");
        version = one(rows, "version");
        romfsRel = one(rows, "romfs");
        List<String> sh = rows.get("shaders");
        shadersRel = sh == null || sh.isEmpty() || sh.get(0).isEmpty() ? null : sh.get(0);
        simulated = rows.containsKey("simulated");
        for (String v : rows.getOrDefault("aoc", List.of())) {
            String[] c = v.split("\t", 2);
            if (c.length == 2) aoc.put(Integer.parseInt(c[0]), c[1]);
        }
        modules = one(rows, "modules").split(" ");
        for (String m : modules)
            if (!new File(dir, m + ".patched.bin").isFile()) throw new IOException("the pack has no " + m + ".patched.bin");
        builds = rows.getOrDefault("build", List.of());
        List<String> pk = rows.get("packer");
        packer = pk == null || pk.isEmpty() ? "" : pk.get(0);
        List<String> mb = rows.get("made_by");
        madeBy = mb == null || mb.isEmpty() ? "" : mb.get(0);
    }

    /** Whether this app made the pack (its made_by row), so its code can be updated without asking when only the tools changed */
    boolean madeByApp() { return madeBy.startsWith("app "); }

    /** The shader library this APK's host loads from the pack: shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib (vka_shader.c makes the path itself; pack.tsv's row is not read), null when missing */
    File library() { return libraryOf(dir); }

    static File libraryOf(File dir) {
        File f = new File(dir, "shaders/v-" + BuildConfig.NX_DMSH_VK_CHAIN + "/dmsh_vk.lib");
        return f.isFile() ? f : null;
    }

    /** This version's library made from an older one by its header alone, when the profile's manifest has a "stamp" row for it (ShaderLibs.restamp; seconds, no job) */
    static String restamp(File dir, String titleId) {
        Profile p = Profile.of(titleId);
        return p == null ? null : ShaderLibs.restamp(dir, titleId, BuildConfig.NX_DMSH_VK_CHAIN, BuildConfig.NX_DMSH_VK_ABI, p.stamp);
    }

    /** Shaders a delta update of this pack translates (the profile's manifest rows for the pack's older libraries, ShaderLibs.toTranslate), -1 when the manifest has none of them */
    static int deltaShaders(File dir, String titleId) {
        Profile p = Profile.of(titleId);
        return p == null ? -1 : ShaderLibs.toTranslate(p.delta, ShaderLibs.older(dir, titleId, BuildConfig.NX_DMSH_VK_CHAIN));
    }

    /**
     * A library of another version the host plays while the shader update makes this APK's (ShaderLibs.playable: the game's, with this APK's DMSH_VK_ABI, else with one of
     * the older ABIs its host also plays, DMSH_VK_ABI_COMPAT), null when none; only asked when {@link #libraryProblem} finds this version's missing
     */
    static File playableLibrary(File dir, String titleId) {
        return ShaderLibs.playable(dir, titleId, BuildConfig.NX_DMSH_VK_CHAIN, BuildConfig.NX_DMSH_VK_ABI, BuildConfig.NX_DMSH_VK_ABI_COMPAT);
    }

    /** A library of another version made before versioned shader updates (no ABI): played with a warning when no playable one is there (ShaderLibs.legacy), null when none */
    static File legacyLibrary(File dir, String titleId) { return ShaderLibs.legacy(dir, titleId, BuildConfig.NX_DMSH_VK_CHAIN); }

    /**
     * The first module whose code is unfinished: its &lt;module&gt;.android.key is missing (scripts/pack_android.py removes it before a rewrite and writes it last,
     * after the module's files went in by rename), so its patched.bin and tables may be of two rewrites. null when every module has its key
     */
    String unfinishedModule() {
        for (String m : modules) if (!new File(dir, m + ".android.key").isFile()) return m;
        return null;
    }

    /** The library versions a pack folder holds (shaders/v-<V>/dmsh_vk.lib) */
    static List<String> libraryVersions(File dir) {
        List<String> out = new ArrayList<>();
        File[] l = new File(dir, "shaders").listFiles(f -> f.getName().startsWith("v-") && new File(f, "dmsh_vk.lib").isFile());
        if (l != null) for (File f : l) out.add(f.getName().substring(2));
        return out;
    }

    /** Why the game (titleId) would draw nothing with this pack folder, in one sentence; null when it has this APK's shader library built from that game's profile */
    static String libraryProblem(File dir, String titleId) {
        Profile prof = Profile.of(titleId);
        String inputs = prof == null ? "unknown" : prof.shaderInputs;
        File lib = libraryOf(dir);
        if (lib != null) {
            // Current only with the stamp of this APK's profile inputs, as pack_android.py's library_current asks: a library built from other shader tables
            // loads but lacks or misplaces shaders, and the pack step would refuse to take it
            if ("unknown".equals(inputs)) return null;
            Map<String, String> st = new LinkedHashMap<>();
            try {
                for (String l : Files.readAllLines(new File(lib.getParentFile(), "inputs.tsv").toPath(), StandardCharsets.UTF_8)) {
                    int t = l.indexOf('\t');
                    if (t > 0 && !l.startsWith("#")) st.put(l.substring(0, t), l.substring(t + 1).trim());
                }
            } catch (IOException e) {
                // no inputs.tsv (a library copied by hand): the library's own header carries the game and the inputs (dmsh_abi_vk.h: 16 bytes at 40 and 56)
                try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(lib, "r")) {
                    byte[] h = new byte[72];
                    r.readFully(h);
                    st.put("inputs", new String(h, 56, 16, StandardCharsets.US_ASCII).replace("\0", "").trim());
                } catch (IOException e2) {
                    return "The pack's shader library (" + lib + ") cannot be read: " + e2.getMessage();
                }
            }
            if (inputs.equals(st.get("inputs"))) return null;
            return "The pack's shader library was built from other shader tables (inputs " + st.get("inputs") + ") than this app's (" + inputs
                    + "): its shaders need making again.";
        }
        List<String> have = libraryVersions(dir);
        String need = BuildConfig.NX_DMSH_VK_CHAIN;
        if (have.isEmpty()) return "The pack folder " + dir + " has no shader library: this app needs shaders/v-" + need + "/dmsh_vk.lib in it.";
        return "The pack folder " + dir + " has the shader library version " + String.join(", ", have) + ", but this app needs version " + need + " (shaders/v-" + need + "/dmsh_vk.lib).";
    }

    private static String one(Map<String, List<String>> rows, String key) throws IOException {
        List<String> v = rows.get(key);
        if (v == null || v.isEmpty()) throw new IOException("pack.tsv has no " + key);
        return v.get(0);
    }

    static Pack read(File dir) throws IOException {
        File f = new File(dir, "pack.tsv");
        if (!f.isFile()) throw new IOException(dir + " has no pack.tsv");
        Map<String, List<String>> rows = new LinkedHashMap<>();
        for (String line : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
            int t = line.indexOf('\t');
            if (t > 0) rows.computeIfAbsent(line.substring(0, t), k -> new ArrayList<>()).add(line.substring(t + 1));
        }
        return new Pack(dir, rows);
    }

    /**
     * Whether a game folder or .nup is a whole dump Prepare can start from (DumpCheck then checks it fully against the profile's builds): a version with its own
     * exefs/main and main.npdm and a RomFS with files (its own or the base's), in any of the three layouts (DumpLayout: the base alone, such as Mario Kart World
     * 1.8.0, an update alone that carries the whole game, or both). One version's folder (exefs/, romfs/ at its top) is not one: it needs a pack made elsewhere.
     */
    static boolean isDump(File dir) { return DumpLayout.preparable(dir); }

    /** The ACI0 program ID of a main.npdm, as 16 upper-case hex digits (scripts/pack_dump.py program_id). */
    static String programId(File npdm) throws IOException {
        try (RandomAccessFile r = new RandomAccessFile(npdm, "r")) {
            byte[] h = new byte[0x80];
            r.readFully(h);
            if (h[0] != 'M' || h[1] != 'E' || h[2] != 'T' || h[3] != 'A') throw new IOException(npdm + ": not an NPDM");
            long off = le32(h, 0x70);
            byte[] aci = new byte[0x18];
            r.seek(off);
            r.readFully(aci);
            return aciProgramId(aci, npdm.toString());
        }
    }

    /** The same, of a main.npdm's bytes (a .nup's: Nup.programId); name says which in an error */
    static String programId(byte[] npdm, String name) throws IOException {
        if (npdm.length < 0x80 || npdm[0] != 'M' || npdm[1] != 'E' || npdm[2] != 'T' || npdm[3] != 'A') throw new IOException(name + ": not an NPDM");
        long off = le32(npdm, 0x70);
        if (off + 0x18 > npdm.length) throw new IOException(name + ": no ACI0 section");
        return aciProgramId(java.util.Arrays.copyOfRange(npdm, (int) off, (int) off + 0x18), name);
    }

    private static String aciProgramId(byte[] aci, String name) throws IOException {
        if (aci[0] != 'A' || aci[1] != 'C' || aci[2] != 'I' || aci[3] != '0') throw new IOException(name + ": no ACI0 section");
        long id = le32(aci, 0x10) | (le32(aci, 0x14) << 32);
        return String.format(Locale.ROOT, "%016X", id);
    }

    /** The title ID of a game: a folder's main.npdm (npdmOf), or a .nup's (Nup.programId). The IOException's message says what is wrong in a sentence */
    static String titleIdOf(File game) throws IOException {
        if (Nup.isNup(game)) {
            try {
                return Nup.open(game).programId();
            } catch (IOException e) {
                throw new IOException(game.getName() + " cannot be used: " + e.getMessage(), e);
            }
        }
        File npdm = npdmOf(game);
        if (npdm == null) throw new IOException(game + " has no main.npdm (base/exefs/, updates/<version>/exefs/ or exefs/).");
        return programId(npdm);
    }

    private static long le32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }

    /** The game folder's main.npdm: a dump's (base/exefs, else the newest update's: an update-only dump) or one version's files' (exefs/). null when it is none of these */
    static File npdmOf(File game) {
        for (String p : new String[] {"base/exefs/main.npdm", "exefs/main.npdm"}) {
            File f = new File(game, p);
            if (f.isFile()) return f;
        }
        long v = DumpLayout.newest(game);   // an update-only dump: the newest update's (an update's NPDM carries the base's program ID)
        File f = v > 0 ? new File(game, "updates/" + v + "/exefs/main.npdm") : null;
        return f != null && f.isFile() ? f : null;
    }

    /** Where a pack for this game folder usually is: <game>/../<id>-work/android-pack, or a pack folder inside the game folder. null when none is found */
    static File find(File game, String titleId) {
        List<File> c = new ArrayList<>();
        File parent = game.getParentFile();
        if (parent != null) c.add(new File(parent, titleId + "-work/android-pack"));
        c.add(new File(game, "android-pack"));
        c.add(new File(game, titleId + "-work/android-pack"));
        File[] subs = game.listFiles(File::isDirectory);
        if (subs != null) for (File s : subs) c.add(s);
        for (File d : c) if (new File(d, "pack.tsv").isFile()) return d;
        return null;
    }

    /** The RomFS folder the host mounts: the one pack.tsv names, else the game folder's romfs/, else the newest updates/<version>/romfs */
    File romfs(File game) {
        File f = new File(game, romfsRel);
        if (f.isDirectory()) return f;
        f = new File(game, "romfs");
        if (f.isDirectory()) return f;
        File[] ups = new File(game, "updates").listFiles(File::isDirectory);
        File best = null;
        if (ups != null) for (File u : ups) {
            if (!u.getName().matches("\\d+") || !new File(u, "romfs").isDirectory()) continue;
            if (best == null || Long.parseLong(u.getName()) > Long.parseLong(best.getName())) best = u;
        }
        if (best != null) return new File(best, "romfs");
        f = new File(game, "base/romfs");   // an update without a RomFS of its own: the base's (DumpLayout)
        return f.isDirectory() ? f : null;
    }

    /**
     * The host's DM_ROMFS for a game: a folder's RomFS folder (romfs), or for a .nup the package pack.tsv's romfs names, applied to this .nup:
     * &lt;the .nup's absolute path&gt;:&lt;selector&gt;/romfs (NUP.md). The selector is pack.tsv's ({@code <name>.nup:update/<v>/romfs}; its file name is not used,
     * so a renamed .nup still plays), or, for a pack made from a folder, the pack's version's package (update/&lt;version&gt;, base for 0). null when the .nup has
     * no such package or it has no RomFS; IOException when the .nup cannot be read
     */
    String romfsPath(File game) throws IOException {
        String gv = gameVersion(game);
        if (!gv.equals(version)) {   // the pack's build under another version of this dump: that version's RomFS (its own, else the base's), as Prepare would pack it
            long v = Long.parseLong(gv);
            if (!DumpLayout.hasRomfs(game, v)) return null;
            return Nup.isNup(game) ? Nup.spec(game, DumpLayout.selector(game, v, true), "romfs") : DumpLayout.path(game, v, "romfs");
        }
        if (!Nup.isNup(game)) {
            File f = romfs(game);
            return f == null ? null : f.getPath();
        }
        String sel = romfsSelector();
        Nup n = Nup.open(game);
        Nup.Pkg p = n.find(sel);
        if (p != null && p.romfsDir == Nup.NONE && sel.startsWith("update/")) {   // an update without a RomFS of its own: the base's (DumpLayout)
            sel = DumpLayout.selector(game, p.version, true);
            p = n.find(sel);
        }
        if (p == null || p.romfsDir == Nup.NONE) return null;
        return Nup.spec(game, sel, "romfs");
    }

    /**
     * The version of a game (folder or .nup) that holds this pack's build: the newest whose modules have the pack's build IDs (DumpLayout.versionWith), else the
     * pack's own version. A dump may keep the same build under another version number (Mario Kart World "1.8.1" with 1.8.0's code): identical build IDs are
     * identical code, so the pack plays it as it is; another build is not found here, and BuildCheck refuses it
     */
    String gameVersion(File game) {
        long v = DumpLayout.versionWith(game, BuildCheck.builds(this));
        return v >= 0 ? String.valueOf(v) : version;
    }

    /** The package of a .nup game's RomFS: pack.tsv's romfs selector when it names a .nup package, else the pack's version's */
    String romfsSelector() {
        Nup.Ref r;
        try {
            r = Nup.parse(romfsRel);
        } catch (IllegalArgumentException e) {
            r = null;
        }
        if (r != null && r.selector != null) return r.selector;
        long v;
        try { v = Long.parseLong(version); } catch (NumberFormatException e) { v = 0; }
        return Nup.versionSelector(v);
    }

    /** Whether a .nup has the addon the host would pick for an index: the low 12 bits of its title ID (NUP.md), with a RomFS (nup_image.h skips one without) */
    static boolean hasAddon(File nup, int index) {
        try {
            for (Nup.Pkg p : Nup.open(nup).of(Nup.ADDON)) if ((p.titleId & 0xFFF) == index && p.romfsDir != Nup.NONE) return true;
        } catch (IOException e) {
            // unreadable: not this .nup
        }
        return false;
    }

    /**
     * The links GameActivity makes in DM_AOC_DIR (hle_fs.c aoc_dir), link name → target. A folder game: &lt;index&gt; → its DLC's RomFS folder in the game
     * folder, for each aoc row whose folder is there (as before). A .nup game: &lt;index&gt;.nup → a .nup file (the host picks the addon package by the index,
     * NUP.md), for each aoc row: the game's own .nup when it has that addon, else the .nup the row names (relative to the folder holding the game) when it is
     * there and has it, else no link: the game runs without that DLC, and skipped (when not null) gets a line naming it. The game's own .nup used to be linked
     * for a row it had no addon for (a pack made from a folder, or from another .nup with the DLC): the host reported the DLC as owned and stopped at its
     * mount (Forgejo #30, docs/reference/packing.md)
     */
    /** Makes a symbolic link (GameActivity: Os.symlink, its errno in the message; the JVM tests: Files.createSymbolicLink) */
    interface Linker { void link(String target, String path) throws IOException; }

    /**
     * Makes the links of aocLinks in dir, after removing what an earlier run left there. Each target goes through target (a .nup used where it is: fd:&lt;n&gt;/&lt;name&gt;).
     * Returns one line for each link that could not be made (the game then runs without that DLC): the launcher logs each one as a [RED] line
     */
    static List<String> makeAocLinks(File dir, Map<String, String> links, java.util.function.UnaryOperator<String> target, Linker linker) {
        List<String> failed = new ArrayList<>();
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) if (!f.delete()) failed.add("cannot remove " + f + " (left from an earlier run)");
        for (Map.Entry<String, String> e : links.entrySet()) {
            String to = target.apply(e.getValue()), at = new File(dir, e.getKey()).getPath();
            try { linker.link(to, at); } catch (IOException ex) { failed.add("cannot link " + at + " -> " + to + ": " + ex.getMessage()); }
        }
        return failed;
    }

    /** Removes a folder of links (its entries, never what they point to; it holds no subfolders), then the folder; nothing when it is not there */
    static void removeDir(File dir) {
        File[] l = dir.listFiles();
        if (l != null) for (File f : l) f.delete();
        dir.delete();
    }

    Map<String, String> aocLinks(File game) { return aocLinks(game, null); }

    Map<String, String> aocLinks(File game, List<String> skipped) {
        Map<String, String> out = new LinkedHashMap<>();
        boolean nup = Nup.isNup(game);
        for (Map.Entry<Integer, String> e : aoc.entrySet()) {
            if (!nup) {
                File target = new File(game, e.getValue());
                if (target.isDirectory()) out.put(String.valueOf(e.getKey()), target.getPath());
                else if (skipped != null) skipped.add("add-on " + e.getKey() + ": no folder " + e.getValue() + " in " + game.getName() + ": the game runs without it");
                continue;
            }
            File target = hasAddon(game, e.getKey()) ? game.getAbsoluteFile() : null;
            if (target == null) {
                try {
                    Nup.Ref r = Nup.parse(e.getValue());
                    File parent = game.getAbsoluteFile().getParentFile(), other = r == null || parent == null ? null : new File(parent, r.path);
                    if (other != null && Nup.isNup(other) && other.isFile() && hasAddon(other, e.getKey())) target = other;
                } catch (IllegalArgumentException ex) {
                    // not a .nup name: only the game's own, which has not got it
                }
            }
            if (target != null) out.put(e.getKey() + ".nup", target.getPath());
            else if (skipped != null) skipped.add("add-on " + e.getKey() + " (pack.tsv aoc row " + e.getValue() + ") is in neither " + game.getName() + " nor the .nup the row names: the game runs without it");
        }
        return out;
    }
}
