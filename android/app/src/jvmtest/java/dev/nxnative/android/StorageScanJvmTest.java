package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * StorageScan on the JVM (dm/tools/tests/test_android_storage.py): the save guard (a save, inside it, holding it, its working siblings, through "..", through a link,
 * outside the app's folders), what the scan lists and what it keeps (the current shader library, a listed game's pack, the newest log, the game's cache storage,
 * a replaced pack still needed), "Remove game data", and a deletion that leaves every save and a link's target as they were. Prints "ok" or throws.
 */
public final class StorageScanJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }

    private static File put(File f, String text) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
        return f;
    }

    private static boolean listed(List<StorageScan.Item> l, File f) {
        for (StorageScan.Item i : l) if (i.file.getAbsolutePath().equals(f.getAbsolutePath())) return true;
        return false;
    }

    private static StorageScan.Kind kind(List<StorageScan.Item> l, File f) {
        for (StorageScan.Item i : l) if (i.file.getAbsolutePath().equals(f.getAbsolutePath())) return i.kind;
        return null;
    }

    public static void main(String[] args) throws IOException {
        File base = new File(args[0]).getCanonicalFile();
        File ext = new File(base, "ext"), files = new File(base, "files"), cache = new File(base, "cache"), nb = new File(base, "nobackup"), outside = new File(base, "outside");
        for (File f : new File[] {ext, files, cache, nb, outside}) f.mkdirs();
        String A = "040074A01BF12000", B = "0400C3F00006E000", C = "0100000000001000", D = "0100000000002000", CUR = "c0ffee000001", OLD = "c0ffee000000";

        // Saves: the app's default, a backup, a legacy private one, a save picked inside the app's temp folder, a picked one beside its device save and working files
        File saveA = put(new File(ext, "saves/" + A + "/save.bin"), "SAVE").getParentFile();
        put(new File(ext, "saves/" + A + "_device/d.bin"), "DEV");
        put(new File(ext, "save-backups/" + A + "/20260101-000000/main/save.bin"), "BK");
        put(new File(files, "save/" + A + "/save.bin"), "OLD");
        File pickedInTemp = put(new File(ext, "temp/" + D + "/mysave/s.bin"), "PICK").getParentFile();
        File picked = put(new File(ext, "custom/mysave/s.bin"), "PICK2").getParentFile();
        File pickedDev = put(new File(ext, "custom/mysave_device/d.bin"), "PICK2D").getParentFile();
        File pickedLock = put(new File(ext, "custom/mysave.lock"), "");
        put(new File(ext, "custom/other.txt"), "x");
        StorageScan.Roots r = new StorageScan.Roots(ext, files, cache, nb).save(saveA).save(pickedInTemp).save(picked);

        // The guard
        check(StorageScan.refuse(r, saveA) != null, "the save itself");
        check(StorageScan.refuse(r, new File(saveA, "save.bin")) != null, "inside the save");
        check(StorageScan.refuse(r, new File(ext, "saves")) != null, "the saves folder");
        check(StorageScan.refuse(r, new File(ext, "save-backups/" + A)) != null, "a backup");
        check(StorageScan.refuse(r, new File(files, "save")) != null, "the legacy private save");
        check(StorageScan.refuse(r, new File(ext, "save-import/x")) != null, "a save being imported");
        check(StorageScan.refuse(r, new File(ext, "saves/" + A + "_device")) != null, "the device save");
        check(StorageScan.refuse(r, new File(ext, "temp/" + D)) != null, "a folder holding a picked save");
        check(StorageScan.refuse(r, new File(ext, "custom")) != null, "the folder holding a picked save");
        check(StorageScan.refuse(r, pickedDev) != null && StorageScan.refuse(r, pickedLock) != null, "a picked save's siblings");
        check(StorageScan.refuse(r, new File(ext, "custom/other.txt")) == null, "a file beside a picked save not named after it");
        check(StorageScan.refuse(r, new File(ext, "packs/../saves/" + A)) != null, "through ..");
        check(StorageScan.refuse(r, ext) != null && StorageScan.refuse(r, cache) != null, "an app folder itself");
        check(StorageScan.refuse(r, new File(outside, "x")) != null && StorageScan.refuse(r, base) != null, "outside the app's folders");
        check(StorageScan.refuse(r, null) != null, "null");
        File toSave = new File(ext, "link-to-save");
        Files.createSymbolicLink(toSave.toPath(), saveA.toPath());
        check(StorageScan.refuse(r, toSave) != null, "a link that names a save");

        // Packs: A listed with this version's library, an old one and a restamp leftover; E listed with only an old library; B not listed
        String E = "0100000000003000";
        File packA = put(new File(ext, "packs/" + A + "/pack.tsv"), "p").getParentFile();
        File curLib = put(new File(packA, "shaders/v-" + CUR + "/dmsh_vk.lib"), "CURRENT");
        File oldLib = put(new File(packA, "shaders/v-" + OLD + "/dmsh_vk.lib"), "OLDER-LIB").getParentFile();
        File restamp = put(new File(packA, "shaders/.restamp-" + CUR + "/dmsh_vk.lib"), "half").getParentFile();
        File packE = put(new File(ext, "packs/" + E + "/pack.tsv"), "p").getParentFile();
        File onlyOld = put(new File(packE, "shaders/v-" + OLD + "/dmsh_vk.lib"), "PLAYED").getParentFile();
        File packB = put(new File(ext, "packs/" + B + "/pack.tsv"), "p").getParentFile();
        File partial = put(new File(ext, "packs/" + A + ".partial/pack.tsv"), "p").getParentFile();
        File replacedA = put(new File(ext, "packs/" + A + ".replaced/pack.tsv"), "p").getParentFile();
        File replacedC = put(new File(ext, "packs/" + C + ".replaced/pack.tsv"), "p").getParentFile();   // C's pack missing: recover puts it back
        // Copies, logs, temp, drivers, the packer, the cache, DLC links
        File part = put(new File(ext, "games/x.nup.part"), "cut");
        File copyA = put(new File(ext, "games/dkb/base/main"), "g").getParentFile().getParentFile();
        File orphan = put(new File(ext, "games/orphan.nup"), "nup");
        File log1 = put(new File(ext, "logs/" + A + "-20260101-000000.log"), "l1"), dig1 = put(new File(ext, "logs/" + A + "-20260101-000000.digest.txt"), "d1");
        File log2 = put(new File(ext, "logs/" + A + "-20260102-000000.log"), "l2"), logB = put(new File(ext, "logs/" + B + "-20260101-000000.log"), "lb");
        File tempA = put(new File(ext, "temp/" + A + "/vk_pipelines-fp0.bin"), "pc").getParentFile();
        File cacheA = put(new File(ext, "temp/" + A + "_cache0/c.bin"), "cs").getParentFile();
        File root = put(new File(ext, "root/cheats/x"), "c").getParentFile();
        File inst = put(new File(files, "gpu_drivers/.installing/x"), "i").getParentFile();
        put(new File(files, "gpu_drivers/foo/meta.json"), "{}");
        File repFoo = put(new File(files, "gpu_drivers/.replaced-foo/meta.json"), "{}").getParentFile();
        File repBar = put(new File(files, "gpu_drivers/.replaced-bar/meta.json"), "{}").getParentFile();
        File packTmp = put(new File(files, "pack/0123456789abcdef.tmp/x"), "t").getParentFile();
        File packCur = put(new File(files, "pack/0123456789abcdef/x"), "t").getParentFile();
        File upOld = put(new File(cache, "updates/old.apk"), "apk"), upKeep = put(new File(cache, "updates/keep.apk"), "apk2");
        File drvZip = put(new File(cache, "driver-123.zip"), "z"), fb = put(new File(cache, "dm_fileback_AbC123"), "fb"), other = put(new File(cache, "other.bin"), "o");
        File target = put(new File(outside, "dlc.nca"), "DLC");
        File aocA = new File(files, "aoc/" + A);
        aocA.mkdirs();
        Files.createSymbolicLink(new File(aocA, "dlc.nca").toPath(), target.toPath());

        List<StorageScan.Listed> games = new ArrayList<>();
        games.add(new StorageScan.Listed(A, copyA.getPath(), packA.getPath()));
        games.add(new StorageScan.Listed(E, "/sdcard/Switch2/e", packE.getPath()));
        games.add(new StorageScan.Listed(D, "/sdcard/Switch2/d", "/sdcard/Switch2/d-work/android-pack"));
        List<StorageScan.Item> l = StorageScan.scan(r, games, CUR, upKeep);

        check(kind(l, oldLib) == StorageScan.Kind.OLD_SHADERS, "an old library beside the current one: " + l);
        check(!listed(l, curLib.getParentFile()) && !listed(l, packA), "never the current library or a listed game's pack");
        check(!listed(l, onlyOld), "an old library the game still plays (no current one)");
        check(kind(l, restamp) == StorageScan.Kind.UNFINISHED && kind(l, partial) == StorageScan.Kind.UNFINISHED && kind(l, replacedA) == StorageScan.Kind.UNFINISHED, "pack leftovers");
        check(!listed(l, replacedC), "a replaced pack whose pack is missing");
        check(kind(l, packB) == StorageScan.Kind.UNUSED_PACKS && !listed(l, packE), "packs of no listed game");
        check(kind(l, part) == StorageScan.Kind.UNFINISHED && kind(l, orphan) == StorageScan.Kind.UNUSED_COPIES && !listed(l, copyA), "copies");
        check(kind(l, log1) == StorageScan.Kind.OLD_LOGS && kind(l, dig1) == StorageScan.Kind.OLD_LOGS && !listed(l, log2) && !listed(l, logB), "old logs, the newest kept");
        check(kind(l, tempA) == StorageScan.Kind.GAME_TEMP && !listed(l, cacheA) && !listed(l, new File(ext, "temp/" + D)), "temp: not cache storage, not a folder holding a save");
        check(!listed(l, root), "never the host's root folder");
        check(kind(l, inst) == StorageScan.Kind.UNFINISHED && kind(l, repFoo) == StorageScan.Kind.UNFINISHED && !listed(l, repBar), "driver leftovers");
        check(kind(l, packTmp) == StorageScan.Kind.UNFINISHED && !listed(l, packCur), "the packer's stopped unpack");
        check(kind(l, upOld) == StorageScan.Kind.DOWNLOADS && !listed(l, upKeep) && kind(l, drvZip) == StorageScan.Kind.DOWNLOADS, "downloads");
        check(kind(l, fb) == StorageScan.Kind.FILEBACK && !listed(l, other), "RAM saver files; nothing unknown");
        for (StorageScan.Item i : l) check(StorageScan.refuse(r, i.file) == null, "every listed item passes the guard: " + i);
        check(StorageScan.total(l) > 0 && StorageScan.bytes(1536).equals("2 KB") && StorageScan.bytes(3L << 30).equals("3.00 GB"), "sizes");

        // Remove game data: A's pack (with its libraries), temp, logs, DLC links; for D (pack elsewhere, save picked in its temp) nothing of the temp folder
        List<StorageScan.Item> ga = StorageScan.gameData(r, games.get(0), games);
        check(listed(ga, packA) && listed(ga, tempA) && listed(ga, log2) && listed(ga, aocA) && !listed(ga, copyA), "game data of A: " + ga);
        for (StorageScan.Item i : ga) check(i.kind == StorageScan.Kind.GAME_DATA, "game data kind");
        check(StorageScan.gameData(r, games.get(2), games).isEmpty(), "game data of D: its temp holds a save, its pack is outside");
        List<StorageScan.Listed> twice = new ArrayList<>(games);
        twice.add(new StorageScan.Listed(A, "/sdcard/Switch2/a2", packA.getPath()));
        check(!listed(StorageScan.gameData(r, games.get(0), twice), packA), "a pack another entry uses is not taken");

        // Deletion: everything listed and A's data; an item pointing at a save is refused; every save and the link's target stay
        List<StorageScan.Item> all = new ArrayList<>(l);
        all.addAll(ga);
        all.add(new StorageScan.Item(StorageScan.Kind.OLD_LOGS, saveA, "forged", 4));
        all.add(new StorageScan.Item(StorageScan.Kind.OLD_LOGS, new File(ext, "custom"), "forged", 4));
        StorageScan.Result res = StorageScan.delete(r, StorageScan.distinct(all));
        check(res.problems.size() == 2, "two refused: " + res.problems);
        check(!packA.exists() && !oldLib.exists() && !packB.exists() && !fb.exists() && !log1.exists() && !aocA.exists() && !Files.exists(new File(aocA, "dlc.nca").toPath()), "removed");
        check(new File(saveA, "save.bin").isFile() && new File(ext, "saves/" + A + "_device/d.bin").isFile() && new File(ext, "save-backups/" + A + "/20260101-000000/main/save.bin").isFile()
                && new File(files, "save/" + A + "/save.bin").isFile() && new File(pickedInTemp, "s.bin").isFile() && new File(picked, "s.bin").isFile()
                && new File(pickedDev, "d.bin").isFile() && pickedLock.isFile(), "every save stays");
        check(target.isFile() && cacheA.isDirectory() && root.isDirectory() && other.isFile() && upKeep.isFile() && packCur.isDirectory() && onlyOld.isDirectory(), "what was not listed stays");
        System.out.println("ok");
    }
}
