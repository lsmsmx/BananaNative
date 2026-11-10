package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Random;

/**
 * The launcher's .nup reading on the JVM (no Android): Nup (superblock, TOC SHA-256, packages, tree, file reads), Zstd (the frame decoder), the selector names,
 * and Pack's DM_ROMFS and DM_AOC_DIR links for a .nup game and a folder game. Compiled with Nup.java, Zstd.java, Pack.java, Profile.java and a stub BuildConfig
 * (NX_PROFILES, NX_DMSH_VK_CHAIN). The fixtures are made by dm/tools/nup_pack.py, a second implementation written from the spec:
 * <ul>
 * <li>{@code <repo> <scratch folder> [<python with zstandard>]}: makes a small dump folder in the scratch folder, packs it raw (python3 -I nup_pack.py --raw)
 *     and, given that python, with zstd too, then every case; prints "ok <n>" and exits 0, or the first failure and exits 1</li>
 * <li>{@code real <file.nup> <dump folder>}: reads a real .nup made from that dump folder (read only): its packages, each exefs file of each package whole
 *     (checked against the folder's file and its SHA-256) and the first files of each RomFS</li>
 * </ul>
 */
public final class NupJvmTest {
    private static int n;

    static void check(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
        n++;
    }

    static void write(File f, byte[] b) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), b);
    }

    static final String TID = "040074A01BF12000", AOC = "040074a01bf13001";

    /** A main.npdm: META with its ACI0 at 0x80, the program ID at ACI0+0x10 (what Pack.programId and nup_pack.py read) */
    static byte[] npdm(long id, int extra) {
        byte[] b = new byte[0x100 + extra];
        b[0] = 'M'; b[1] = 'E'; b[2] = 'T'; b[3] = 'A';
        b[0x70] = (byte) 0x80;
        b[0x80] = 'A'; b[0x81] = 'C'; b[0x82] = 'I'; b[0x83] = '0';
        for (int i = 0; i < 8; i++) b[0x90 + i] = (byte) (id >>> (8 * i));
        return b;
    }

    /** An NSO-like file: NSO0, a build ID at 0x40, then a body that compresses (text with repeats, some noise) */
    static byte[] nso(int seed, int size) {
        byte[] b = new byte[size];
        b[0] = 'N'; b[1] = 'S'; b[2] = 'O'; b[3] = '0';
        for (int i = 0; i < 32; i++) b[0x40 + i] = (byte) (seed * 31 + i);
        fill(b, 0x100, seed);
        return b;
    }

    static void fill(byte[] b, int from, int seed) {
        Random r = new Random(seed);
        String[] words = {"bananza ", "kong ", "dk ", "pauline ", "\0\0\0\0", "ldr x0, [x1, #8]\n", "été ", "{\"key\": 12}, "};
        int i = from;
        while (i < b.length) {
            int kind = r.nextInt(10);
            if (kind < 6) { byte[] w = words[r.nextInt(words.length)].getBytes(StandardCharsets.UTF_8); for (int k = 0; k < w.length && i < b.length; k++) b[i++] = w[k]; }
            else if (kind < 8) { int len = 1 + r.nextInt(40); for (int k = 0; k < len && i < b.length; k++) b[i++] = (byte) r.nextInt(256); }
            else { int back = 1 + r.nextInt(Math.max(1, Math.min(i - from, 3000)) ), len = 4 + r.nextInt(300); for (int k = 0; k < len && i < b.length; k++, i++) b[i] = b[Math.max(from, i - back)]; }
        }
    }

    static byte[] random(int seed, int size) { byte[] b = new byte[size]; new Random(seed).nextBytes(b); return b; }

    /** base/ (version 0), updates/458752/, dlc/<aoc>/0/: exefs and RomFS files of several kinds (empty, random, repeating, equal files shared, nested folders) */
    static void dump(File d) throws IOException {
        long id = Long.parseUnsignedLong(TID, 16);
        String[] vers = {"base", "updates/458752"};
        for (int v = 0; v < vers.length; v++) {
            File p = new File(d, vers[v]);
            write(new File(p, "exefs/main"), nso(v + 1, 300_000 + v * 7777));
            write(new File(p, "exefs/sdk"), nso(v + 11, 90_000));
            write(new File(p, "exefs/main.npdm"), npdm(id, v * 64));
            write(new File(p, "exefs/rtld"), nso(99, 5000));   // equal in both: one extent
            write(new File(p, ".listing"), "listing".getBytes(StandardCharsets.UTF_8));
            write(new File(p, "romfs/empty.bin"), new byte[0]);
            write(new File(p, "romfs/Pack/Actor/a.szs"), random(v, 70_000));
            write(new File(p, "romfs/Pack/Actor/b.txt"), "hello romfs\n".repeat(5000 + v).getBytes(StandardCharsets.UTF_8));
            byte[] rle = new byte[200_000];
            Arrays.fill(rle, (byte) 7);
            write(new File(p, "romfs/Pack/zeros.bin"), rle);
            byte[] mixed = new byte[1_500_000];   // several blocks: repeat offsets and tables across blocks
            fill(mixed, 0, 1234 + v);
            write(new File(p, "romfs/System/mixed.bin"), mixed);
            write(new File(p, "romfs/" + "日本.dat"), "utf-8 name".getBytes(StandardCharsets.UTF_8));
        }
        write(new File(d, "dlc/" + AOC + "/0/romfs/dlc.bin"), nso(5, 40_000));
    }

    static File pack(File repo, File dumpDir, File out, String python, boolean raw, String... more) throws IOException, InterruptedException {
        List<String> cmd = new ArrayList<>(List.of(python, "-I", new File(repo, "dm/tools/nup_pack.py").getPath(), dumpDir.getPath(), out.getPath()));
        if (raw) cmd.add("--raw");
        cmd.addAll(List.of(more));
        Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        String o = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (p.waitFor() != 0 || !out.isFile()) throw new AssertionError("nup_pack.py failed: " + o);
        return out;
    }

    static byte[] bytes(File f) throws IOException { return Files.readAllBytes(f.toPath()); }

    /** Builds a meta block from type and data pairs */
    static byte[] tlvs(Object... tp) throws IOException {
        java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
        for (int i = 0; i < tp.length; i += 2) {
            byte[] p = tp[i + 1] instanceof String ? ((String) tp[i + 1]).getBytes(StandardCharsets.UTF_8) : (byte[]) tp[i + 1];
            int t = (Integer) tp[i];
            o.write(new byte[] {(byte) t, (byte) (t >> 8), 0, 0, (byte) p.length, (byte) (p.length >> 8), (byte) (p.length >> 16), (byte) (p.length >>> 24)});
            o.write(p);
        }
        return o.toByteArray();
    }

    /** Meta blocks with the spec's numbers and with the packer's shifted ones, an "icon" that isn't an image, and a record cut off at the end */
    static void meta() throws IOException {
        byte[] jpg = {(byte) 0xFF, (byte) 0xD8, (byte) 0xFF, (byte) 0xE0, 1, 2, 3}, npdm = "META".getBytes(StandardCharsets.UTF_8);
        Nup.Meta m = Nup.Meta.parse(tlvs(1, npdm, 3, jpg, 4, "1.8.0", 5, "Mario Kart World", 6, "Nintendo", 0x8001, "private"));
        check(m.icon != null && m.icon.length == jpg.length && "1.8.0".equals(m.version) && "Mario Kart World".equals(m.title) && "Nintendo".equals(m.publisher), "meta: the spec's types");
        m = Nup.Meta.parse(tlvs(0, npdm, 2, jpg, 3, "3.0.0", 4, "Donkey Kong Bananza", 5, "Nintendo", 6, new byte[32]));
        check(m.icon != null && "3.0.0".equals(m.version) && "Donkey Kong Bananza".equals(m.title) && "Nintendo".equals(m.publisher), "meta: the packer's types, one lower (a type 0 record)");
        m = Nup.Meta.parse(tlvs(1, npdm, 3, "not an image", 5, "Zelda"));
        check(m.icon == null && "Zelda".equals(m.title) && m.version == null, "meta: an icon that is no JPEG or PNG is not taken");
        m = Nup.Meta.parse(java.util.Arrays.copyOf(tlvs(5, "Cut"), 10));
        check(m.title == null, "meta: a record past the block's end is not read");
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("real")) { real(new File(args[1]), new File(args[2])); System.out.println("ok " + n); return; }
        File repo = new File(args[0]), t = new File(args[1]);
        names();
        meta();
        Zstd.contentSize(new byte[] {(byte) 0x28, (byte) 0xB5, 0x2F, (byte) 0xFD, 0x20, 0x05, 0x01, 0, 0});   // a header alone parses
        File d = new File(t, "Game");
        dump(d);
        File raw = pack(repo, d, new File(t, "game-raw.nup"), "python3", true);
        reader(raw, d, true);
        damage(raw, t);
        if (args.length > 2) {
            File z = pack(repo, d, new File(t, "game-zstd.nup"), args[2], false);
            reader(z, d, false);
            packRows(z, t);
        }
        packRows(raw, t);
        picked(raw, d, t);
        // A .nup without the add-on: its aoc row's own .nup (beside it, with the add-on) is linked instead
        File nodlc = pack(repo, d, new File(t, "NoDlc.nup"), "python3", true, "--only", "base,update");
        Pack p = Pack.read(packDir(t, "pack-nodlc", raw.getName() + ":update/458752/romfs", "1	" + raw.getName() + ":addon/" + AOC + "/romfs"));
        check(Nup.open(nodlc).of(Nup.ADDON).isEmpty() && raw.getAbsolutePath().equals(p.aocLinks(nodlc).get("1.nup")), "no add-on in the game's .nup: the row's .nup " + p.aocLinks(nodlc));
        check(p.romfsPath(nodlc).equals(nodlc.getAbsolutePath() + ":update/458752/romfs"), "the game's own RomFS all the same");
        // Forgejo #30: an aoc row whose add-on neither the game's .nup nor the row's .nup has is no link (the host reported it owned and stopped at its mount), and a line names it
        for (String row : new String[] {"1\tdlc/" + AOC + "/0/romfs", "1\tGone.nup:addon/" + AOC + "/romfs", "1\t" + nodlc.getName() + ":addon/" + AOC + "/romfs"}) {
            List<String> skipped = new ArrayList<>();
            Map<String, String> l = Pack.read(packDir(t, "pack-nodlc2", nodlc.getName() + ":update/458752/romfs", row)).aocLinks(nodlc, skipped);
            check(l.isEmpty() && skipped.size() == 1 && skipped.get(0).contains("add-on 1") && skipped.get(0).contains("runs without it"), "no add-on anywhere (" + row + "): no link, one line " + l + " " + skipped);
        }
        folderRows(t);
        dumps(repo, d, t);
        System.out.println("ok " + n);
    }

    // ---- Dumps Prepare starts from (Pack.isDump) ----

    /** A whole dump: an update, the base game alone with its code and RomFS (a game whose supported build is its base, Mario Kart World 1.8.0), or an update alone that carries the whole game (DumpLayout); never one version's folder */
    static void dumps(File repo, File d, File t) throws IOException, InterruptedException {
        check(Pack.isDump(d), "base/ and updates/: a dump");
        check(!Pack.isDump(new File(d, "base")) && !Pack.isDump(new File(d, "updates/458752")), "one version's folder is no dump");
        File b = new File(t, "BaseOnly");
        write(new File(b, "base/exefs/main.npdm"), bytes(new File(d, "base/exefs/main.npdm")));
        check(!Pack.isDump(b), "base/exefs/main.npdm alone: no version to prepare");
        write(new File(b, "base/exefs/main"), bytes(new File(d, "base/exefs/main")));
        check(!Pack.isDump(b), "the base's code without its RomFS: no version to prepare");
        new File(b, "base/romfs").mkdirs();
        check(!Pack.isDump(b), "an empty base/romfs: no version to prepare");
        write(new File(b, "base/romfs/Pack/zeros.bin"), new byte[16]);
        check(Pack.isDump(b), "the base game alone, with its code and RomFS: a dump (version 0)");
        new File(b, "updates").mkdirs();
        check(Pack.isDump(b), "with an updates/ folder too");
        File nb = pack(repo, d, new File(t, "BaseOnly.nup"), "python3", true, "--only", "base");
        check(Nup.open(nb).of(Nup.UPDATE).isEmpty() && Pack.isDump(nb), "a .nup of the base game alone: a dump (version 0)");
        File nu = pack(repo, d, new File(t, "UpdateOnly.nup"), "python3", true, "--only", "update");
        check(Nup.open(nu).base() == null && Pack.isDump(nu) && DumpLayout.versions(nu).equals(List.of(458752L)), "an update-only .nup that carries the whole game (its exefs and RomFS): a dump of that update (DumpLayout)");
        check(Pack.isDump(new File(t, "game-raw.nup")), "a .nup with the base and an update: a dump");
        check(!Pack.isDump(new File(t, "missing.nup")), "a .nup that is not there");
    }

    // ---- Names ----

    static void names() {
        Nup.Ref r = Nup.parse("/s/Game.nup:update/458752/romfs");
        check(r.path.equals("/s/Game.nup") && r.selector.equals("update/458752") && r.sub.equals("romfs"), "update/romfs: " + r);
        r = Nup.parse("/s/Game.nup:update/458752/exefs/main");
        check(r.selector.equals("update/458752") && r.sub.equals("exefs/main"), "exefs/main");
        r = Nup.parse("Game.nup:base");
        check(r.path.equals("Game.nup") && r.selector.equals("base") && r.sub.isEmpty(), "base");
        r = Nup.parse("Game.nup:addon/040074A01BF13001/romfs");
        check(r.selector.equals("addon/040074a01bf13001") && r.sub.equals("romfs"), "addon lower-cased: " + r.selector);
        r = Nup.parse("/a.nup:dir/b.nup:update/007/romfs");   // split at the last ".nup:"
        check(r.path.equals("/a.nup:dir/b.nup") && r.selector.equals("update/7"), "last .nup: " + r.path + " " + r.selector);
        r = Nup.parse("/s/Game.nup");
        check(r.selector == null && r.path.equals("/s/Game.nup"), "no selector");
        r = Nup.parse("/s/Game.nup:");
        check(r.selector == null, "empty selector");
        check(Nup.parse("updates/458752/romfs") == null, "a folder path is no .nup");
        check(Nup.parse(".nup") == null, "only the extension");
        for (String bad : new String[] {"G.nup:update", "G.nup:update/x", "G.nup:addon/123", "G.nup:base2", "G.nup:romfs", "G.nup:addon/040074a01bf13001x"}) {
            boolean threw = false;
            try { Nup.parse(bad); } catch (IllegalArgumentException e) { threw = true; }
            check(threw, "refused: " + bad);
        }
        check(Nup.versionSelector(0).equals("base") && Nup.versionSelector(458752).equals("update/458752"), "versionSelector");
        check(Nup.spec(new File("/s/G.nup"), "update/1", "romfs").equals("/s/G.nup:update/1/romfs") && Nup.spec(new File("/s/G.nup"), "base", "").equals("/s/G.nup:base"), "spec");
        check(Nup.isNup(new File("/nonexistent/G.nup")) && !Nup.isNup(new File("/nonexistent/G.NUP")) && !Nup.isNup(new File("/nonexistent/G")), "isNup by name");
    }

    // ---- The reader ----

    static void reader(File nup, File d, boolean raw) throws IOException {
        String tag = nup.getName() + ": ";
        Nup x = Nup.open(nup);
        check(Nup.open(nup) == x, tag + "cached");
        check(x.packages.size() == 3, tag + "3 packages, not " + x.packages.size());
        check(x.base() != null && x.base().version == 0 && x.base().selector().equals("base"), tag + "base");
        Nup.Pkg up = x.find("update/458752");
        check(up != null && up.kind == Nup.UPDATE && up.flags == 0, tag + "update");
        check(x.newest() == up && x.resolve(null) == up && x.resolve("base") == x.base(), tag + "newest is the update");
        Nup.Pkg ao = x.find("addon/" + AOC);
        check(ao != null && ao.kind == Nup.ADDON && ao.exefsDir == Nup.NONE && ao.romfsDir != Nup.NONE, tag + "addon");
        check(x.find("update/1") == null && x.of(Nup.ADDON).size() == 1 && x.of(Nup.UPDATE).size() == 1, tag + "no other packages");
        check(x.programId().equals(TID), tag + "program ID " + x.programId());
        check(Pack.titleIdOf(nup).equals(TID) && Pack.titleIdOf(d).equals(TID), tag + "titleIdOf .nup and folder");
        String[] vers = {"base", "updates/458752"};
        for (int v = 0; v < 2; v++) {
            Nup.Pkg p = v == 0 ? x.base() : up;
            File pd = new File(d, vers[v]);
            for (String f : new String[] {"exefs/main", "exefs/sdk", "exefs/main.npdm", "exefs/rtld", "romfs/empty.bin", "romfs/Pack/Actor/a.szs", "romfs/Pack/Actor/b.txt",
                    "romfs/Pack/zeros.bin", "romfs/System/mixed.bin", "romfs/日本.dat"}) {
                Nup.Node node = x.lookup(p, f);
                check(node != null && !node.dir, tag + p.selector() + "/" + f + " found");
                byte[] want = bytes(new File(pd, f));
                check(node.size == want.length, tag + f + " size");
                check(raw ? node.codec == 0 : node.codec == 1 || want.length < 4096 || f.endsWith("a.szs"), tag + f + " codec " + node.codec);
                check(Arrays.equals(x.read(node, -1), want), tag + p.selector() + "/" + f + " reads as the folder's file");
                byte[] head = x.read(node, 0x60);
                check(head.length >= Math.min(0x60, want.length) && Arrays.equals(Arrays.copyOf(head, Math.min(0x60, want.length)), Arrays.copyOf(want, Math.min(0x60, want.length))), tag + f + " head");
            }
            check(x.lookup(p, ".listing") == null && x.lookup(p, "romfs/nothing") == null && x.lookup(p, "exefs/main/x") == null, tag + "no .listing, missing → null");
            Nup.Node rd = x.lookup(p, "romfs");
            check(rd != null && rd.dir && rd.childCount == 4, tag + "romfs has 4 entries");
            List<String> names = new ArrayList<>();
            for (Nup.Node c : x.children(rd)) names.add(c.name);
            check(names.equals(List.of("Pack", "System", "empty.bin", "日本.dat")), tag + "romfs entries sorted bytewise: " + names);
            check(x.lookup(p, "") != null && x.lookup(p, "").dir, tag + "root");
            check(DumpBuildId.of(x.read(x.lookup(p, "exefs/main"), 0x60)).equals(DumpBuildId.of(nso(v + 1, 0x100))), tag + "build ID from the head");
        }
        check(x.lookup(x.base(), "exefs/rtld").extentOffset == x.lookup(up, "exefs/rtld").extentOffset, tag + "equal files share an extent");
        check(Arrays.equals(x.read(x.lookup(ao, "romfs/dlc.bin"), -1), bytes(new File(d, "dlc/" + AOC + "/0/romfs/dlc.bin"))), tag + "addon file");
        check(x.lookup(ao, "exefs/main") == null, tag + "addon has no exefs");
        Nup q = Nup.peek(nup);
        check(q.packages.size() == 3 && q.base() != null, tag + "peek");
    }

    /** An NSO's build ID as DumpCheck.nsoBuildId writes it (DumpCheck is not in the JVM test: it uses android.os.StatFs) */
    static final class DumpBuildId {
        static String of(byte[] h) {
            StringBuilder b = new StringBuilder();
            for (int i = 0x40; i < 0x60; i++) b.append(String.format("%02x", h[i] & 0xff));
            return b.toString();
        }
    }

    static void expectBad(File f, String part, String what) {
        try {
            Nup.open(f);
        } catch (IOException e) {
            check(e.getMessage().contains(part), what + ": " + e.getMessage());
            return;
        }
        throw new AssertionError(what + ": opened");
    }

    /** A damaged copy is refused: a TOC byte (its SHA-256), the size, the magic; a damaged file is refused when read whole */
    static void damage(File raw, File t) throws IOException {
        byte[] b = bytes(raw);
        File c = new File(t, "damaged.nup");
        byte[] x = b.clone();
        x[0x80 + 0x30 + 3] ^= 1;   // in the first package record
        Files.write(c.toPath(), x);
        expectBad(c, "SHA-256", "a TOC byte");
        Files.write(c.toPath(), Arrays.copyOf(b, b.length - 1));
        expectBad(c, "incomplete", "one byte short");
        x = b.clone();
        x[1] = 'X';
        Files.write(c.toPath(), x);
        expectBad(c, "magic", "magic");
        x = b.clone();
        x[0x4C] = 1;
        Files.write(c.toPath(), x);
        expectBad(c, "flags", "flags");
        Nup good = Nup.open(raw);
        Nup.Node main = good.lookup(good.base(), "exefs/main");
        x = b.clone();
        x[(int) main.extentOffset + 0x200] ^= 0x55;
        File c2 = new File(t, "damaged-blob.nup");
        Files.write(c2.toPath(), x);
        Nup dn = Nup.open(c2);   // the TOC is fine
        try {
            dn.read(dn.lookup(dn.base(), "exefs/main"), -1);
            throw new AssertionError("a damaged file read whole");
        } catch (IOException e) {
            check(e.getMessage().contains("SHA-256"), "damaged file: " + e.getMessage());
        }
        check(Arrays.equals(Arrays.copyOf(dn.read(dn.lookup(dn.base(), "exefs/main"), 0x60), 0x60), Arrays.copyOf(nso(1, 0x100), 0x60)), "its head still reads");
        c.delete();
        c2.delete();
    }

    // ---- Pack: DM_ROMFS and the aoc links ----

    static File packDir(File t, String name, String romfs, String aoc) throws IOException {
        File p = new File(t, name);
        write(new File(p, "main.patched.bin"), new byte[1]);
        write(new File(p, "pack.tsv"), ("format\t1\ntarget\tandroid\ntitle_id\t" + TID + "\nname\tGame\nversion\t458752\nromfs\t" + romfs + "\nmodules\tmain\n"
                + (aoc == null ? "" : "aoc\t" + aoc + "\n")).getBytes(StandardCharsets.UTF_8));
        return p;
    }

    static void packRows(File nup, File t) throws IOException {
        String abs = nup.getAbsolutePath(), tag = nup.getName() + ": ";
        Pack p = Pack.read(packDir(t, "pack-nup", nup.getName() + ":update/458752/romfs", "1\t" + nup.getName() + ":addon/" + AOC + "/romfs"));
        check(p.romfsPath(nup).equals(abs + ":update/458752/romfs"), tag + "DM_ROMFS " + p.romfsPath(nup));
        Map<String, String> l = p.aocLinks(nup);
        check(l.size() == 1 && abs.equals(l.get("1.nup")), tag + "aoc link 1.nup → the .nup: " + l);
        // A renamed .nup: the selector of pack.tsv on the game's own file; the aoc row's .nup is gone, so the game's own .nup
        File other = new File(t, "Renamed.nup");
        Files.copy(nup.toPath(), other.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        check(p.romfsPath(other).equals(other.getAbsolutePath() + ":update/458752/romfs"), tag + "renamed: " + p.romfsPath(other));
        check(other.getAbsolutePath().equals(p.aocLinks(other).get("1.nup")), tag + "renamed aoc link");
        other.delete();
        // pack.tsv's base selector
        p = Pack.read(packDir(t, "pack-base", nup.getName() + ":base/romfs", null));
        check(p.romfsPath(nup).equals(abs + ":base/romfs") && p.aocLinks(nup).isEmpty(), tag + "base selector");
        // a pack made from a folder, used with the .nup: the pack's version's package
        p = Pack.read(packDir(t, "pack-folder", "updates/458752/romfs", "1\tdlc/" + AOC + "/0/romfs"));
        check(p.romfsPath(nup).equals(abs + ":update/458752/romfs"), tag + "folder pack's version: " + p.romfsPath(nup));
        check(abs.equals(p.aocLinks(nup).get("1.nup")), tag + "folder pack's aoc row → the game's .nup");
        // a selector the .nup does not have: no RomFS
        p = Pack.read(packDir(t, "pack-missing", nup.getName() + ":update/1/romfs", null));
        check(p.romfsPath(nup) == null, tag + "a missing package: null");
        p = Pack.read(packDir(t, "pack-addon", nup.getName() + ":addon/" + AOC + "/romfs", null));
        check(p.romfsPath(nup).equals(abs + ":addon/" + AOC + "/romfs"), tag + "addon selector");
    }

    /**
     * A .nup used where it is (PickedNup, PickedPath): named by a stand-in path in app storage where nothing is, read through Nup.opener (here: the fixture file by
     * its channel, as the phone reads the provider's descriptor); the host's and the packer's forms of its paths; a file the opener can no longer open
     */
    static void picked(File raw, File d, File t) throws IOException {
        File app = new File(t, "app");
        String uri = "content://com.android.externalstorage.documents/document/primary%3AGames%2FGame%20Name.nup";
        String key = PickedPath.key(uri);
        check(key.matches("[0-9a-f]{16}") && key.equals(PickedPath.key(uri)) && !key.equals(PickedPath.key(uri + "x")), "key: 16 hex digits of the URI's SHA-256, stable");
        File stand = PickedPath.standIn(app, key, "Game Name.nup");
        check(stand.getPath().equals(new File(app, "picked/" + key + "/Game Name.nup").getPath()) && key.equals(PickedPath.keyOf(app, stand)), "stand-in path and its key");
        check(PickedPath.keyOf(app, new File(app, "games/Game.nup")) == null && PickedPath.keyOf(app, new File(app, "picked/xyz/Game.nup")) == null
                && PickedPath.keyOf(app, new File(app, "picked/" + key + "/Game.txt")) == null && PickedPath.keyOf(new File(t, "other"), stand) == null, "keyOf: stand-in paths only");
        check(PickedPath.nameProblem("Game.nup") == null && PickedPath.nameProblem("Game.NUP").contains("lower case") && PickedPath.nameProblem("Game.zip").contains("not a .nup")
                && PickedPath.nameProblem(null) != null && PickedPath.nameProblem("a/b.nup") != null && PickedPath.nameProblem(".nup") != null
                && PickedPath.nameProblem("x.nup:y.nup") != null, "names a .nup in place may have");
        check(PickedPath.fdSpec(7, "Game Name.nup").equals("fd:7/Game Name.nup"), "fd spec");
        boolean[] lost = {false};
        Nup.opener = new Nup.Opener() {
            public boolean owns(File f) { return key.equals(PickedPath.keyOf(app, f)); }
            public Nup.Source open(File f) throws IOException {
                if (lost[0]) throw new IOException("The app can no longer open " + f.getName() + ".");
                java.io.RandomAccessFile r = new java.io.RandomAccessFile(raw, "r");
                return new Nup.Source(r.getChannel(), r.length(), uri + "|" + r.length(), r);
            }
        };
        try {
            check(!stand.exists() && Nup.picked(stand) && !Nup.picked(raw) && Nup.isNup(stand), "the stand-in: nothing there, a picked .nup");
            Nup x = Nup.open(stand);
            check(x.programId().equals(TID) && x.packages.size() == 3 && Nup.open(stand) == x, "read through the opener, cached");
            byte[] main = x.read(x.lookup(x.find("update/458752"), "exefs/main"), -1);
            check(Arrays.equals(main, bytes(new File(d, "updates/458752/exefs/main"))), "a file read through the opener, its SHA-256 checked");
            check(Pack.titleIdOf(stand).equals(TID) && Pack.isDump(stand) && DumpLayout.versions(stand).equals(DumpLayout.versions(raw))
                    && String.valueOf(DumpLayout.find(stand).error).equals(String.valueOf(DumpLayout.find(raw).error).replace(raw.getName(), stand.getName())), "Add game's checks on the stand-in as on the file: " + DumpLayout.find(stand).error);
            check(BuildCheck.gameKey(stand, TID) != null && BuildCheck.gameKey(stand, TID).equals(BuildCheck.gameKey(raw, TID)), "the same game key as the file itself");
            Pack p = Pack.read(packDir(t, "pack-picked", "Game Name.nup:update/458752/romfs", "1\tGame Name.nup:addon/" + AOC + "/romfs"));
            String romfs = p.romfsPath(stand);
            check(romfs.equals(stand.getAbsolutePath() + ":update/458752/romfs"), "DM_ROMFS of the stand-in " + romfs);
            check(PickedPath.forHost(romfs, stand, 7).equals("fd:7/Game Name.nup:update/458752/romfs"), "the host's DM_ROMFS: the descriptor " + PickedPath.forHost(romfs, stand, 7));
            String link = p.aocLinks(stand).get("1.nup");
            check(PickedPath.forHost(link, stand, 7).equals("fd:7/Game Name.nup"), "the add-on link's target: the descriptor " + link);
            check(PickedPath.forHost("/x/romfs", stand, 7).equals("/x/romfs"), "another path as it is");
            lost[0] = true;
            try { Nup.open(stand); check(false, "a lost file opens"); } catch (IOException e) { check(e.getMessage().contains("no longer open"), "a lost file: " + e.getMessage()); }
            check(String.valueOf(DumpLayout.find(stand).error).contains("no longer open"), "a lost file says so: " + DumpLayout.find(stand).error);
        } finally {
            Nup.opener = null;
        }
        check(!Nup.picked(stand), "no opener: no picked file");
    }

    /** A folder game: as before (Pack.romfs, aoc links to the folders that are there) */
    static void folderRows(File t) throws IOException {
        File g = new File(t, "Game");
        Pack p = Pack.read(packDir(t, "pack-fold", "updates/458752/romfs", "1\tdlc/" + AOC + "/0/romfs\n" + "aoc\t2\tdlc/missing/0/romfs"));
        check(p.romfsPath(g).equals(new File(g, "updates/458752/romfs").getPath()) && p.romfsPath(g).equals(p.romfs(g).getPath()), "folder DM_ROMFS " + p.romfsPath(g));
        Map<String, String> l = p.aocLinks(g);
        check(l.size() == 1 && l.get("1").equals(new File(g, "dlc/" + AOC + "/0/romfs").getPath()), "folder aoc links: " + l);
        List<String> skipped = new ArrayList<>();
        check(p.aocLinks(g, skipped).equals(l) && skipped.size() == 1 && skipped.get(0).contains("add-on 2"), "a missing DLC folder is a line: " + skipped);
        aocLinkDir(t, l);
        // a pack made from a .nup, used with the folder: the folder's own RomFS (Pack.romfs falls back), no aoc link
        p = Pack.read(packDir(t, "pack-fold2", "game.nup:update/458752/romfs", "1\tgame.nup:addon/" + AOC + "/romfs"));
        check(p.romfsPath(g).equals(new File(g, "updates/458752/romfs").getPath()) && p.aocLinks(g).isEmpty(), "a .nup pack with a folder: " + p.romfsPath(g));
    }

    /** Pack.makeAocLinks: an earlier run's entries go, each link is made (the target mapped), a link that cannot be made is a line, never dropped */
    static void aocLinkDir(File t, Map<String, String> l) throws IOException {
        File dir = new File(t, "aoc-links");
        Pack.removeDir(dir);
        dir.mkdirs();
        write(new File(dir, "7"), new byte[1]);   // left from an earlier run
        Pack.Linker real = (target, path) -> Files.createSymbolicLink(new File(path).toPath(), new File(target).toPath());
        List<String> bad = Pack.makeAocLinks(dir, l, x -> x, real);
        File one = new File(dir, "1");
        check(bad.isEmpty() && !new File(dir, "7").exists() && Files.isSymbolicLink(one.toPath()) && one.isDirectory(), "aoc link 1 → the DLC folder: " + bad);
        bad = Pack.makeAocLinks(dir, Map.of("1.nup", "x.nup"), x -> "fd:9/" + x, real);
        check(bad.isEmpty() && Files.readSymbolicLink(new File(dir, "1.nup").toPath()).toString().equals("fd:9/x.nup") && !one.exists(), "a mapped target (fd:<n>/<name>) and the old link gone");
        bad = Pack.makeAocLinks(dir, Map.of("1", "/x", "2", "/y"), x -> x, (target, path) -> { throw new IOException("EPERM (symlink failed)"); });
        check(bad.size() == 2 && bad.get(0).contains("EPERM") && bad.get(0).contains(" -> /"), "each refused link is a line: " + bad);
        Pack.removeDir(dir);
        check(!dir.exists(), "removeDir");
    }

    // ---- A real .nup ----

    static void real(File nup, File d) throws IOException {
        long t0 = System.nanoTime();
        Nup x = Nup.open(nup);
        System.out.printf("opened in %.2f s: %d packages, %d dirs, %d files, TOC %.1f MB%n", (System.nanoTime() - t0) / 1e9, x.packages.size(), x.dirCount, x.fileCount, x.tocSize / 1e6);
        for (Nup.Pkg p : x.packages) {
            String folder = p.kind == Nup.BASE ? "base" : p.kind == Nup.UPDATE ? "updates/" + p.version : null;
            System.out.println("  " + p.selector() + String.format(" title %016X", p.titleId));
            if (p.kind == Nup.ADDON) {
                File[] l = new File(d, "dlc/" + String.format("%016x", p.titleId)).listFiles(File::isDirectory);
                if (l == null) l = new File(d, "dlc/" + String.format("%016X", p.titleId)).listFiles(File::isDirectory);
                check(l != null && l.length > 0, "the folder has the addon " + p.selector());
                folder = d.toPath().relativize(l[0].toPath()).toString();
            }
            Nup.Node ex = x.dir(p.exefsDir);
            if (ex != null) for (Nup.Node f : x.children(ex)) {
                long t1 = System.nanoTime();
                byte[] b = x.read(f, -1);
                check(Arrays.equals(b, bytes(new File(d, folder + "/exefs/" + f.name))), p.selector() + "/exefs/" + f.name + " equals the folder's");
                System.out.printf("    exefs/%s %d bytes, codec %d, %.2f s%n", f.name, f.size, f.codec, (System.nanoTime() - t1) / 1e9);
            }
            Nup.Node rd = x.dir(p.romfsDir);
            int k = 0;
            List<Nup.Node> q = new ArrayList<>(x.children(rd));
            List<String> path = new ArrayList<>();
            for (Nup.Node c : q) path.add(c.name);
            for (int i = 0; i < q.size() && k < 6; i++) {
                Nup.Node c = q.get(i);
                if (c.dir) { for (Nup.Node cc : x.children(c)) { q.add(cc); path.add(path.get(i) + "/" + cc.name); } continue; }
                if (c.size > (64 << 20)) continue;
                check(Arrays.equals(x.read(c, -1), bytes(new File(d, folder + "/romfs/" + path.get(i)))), p.selector() + "/romfs/" + path.get(i));
                k++;
            }
            if (p.exefsDir != Nup.NONE) check(x.programId().length() == 16, "program ID");
        }
        System.out.println("  program ID " + x.programId());
        try (RandomAccessFile r = new RandomAccessFile(nup, "r")) { check(r.length() == x.fileSize, "size"); }
    }
}
