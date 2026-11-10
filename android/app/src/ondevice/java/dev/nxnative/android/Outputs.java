package dev.nxnative.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * A folder of a step's outputs that the tool writes in place, on shared storage, and that counts as complete only once its marker file is there
 * (ONDEVICE.md "The job model"). The marker is written last, after every output is synced; it is deleted first, before anything else in the folder is
 * deleted or written again. So a kill or a reboot at any point leaves either a folder without the marker (redone) or every output on the disk with it.
 *
 * Why not a temporary folder renamed into place: on Android 11 and later a folder renamed under /storage/emulated goes through MediaProvider
 * (renameForFuse, renameDirectoryUncheckedForFuse), which moves the database row of every indexed file in it in one transaction and answers EPERM when
 * one of them does not move exactly one row; a file's rename falls back for an app with all files access. A user's device refused the split's
 * split.tmp -> split with EPERM (BananaNative issue #1; which row did not move is not known). Only files are renamed here (the marker), as job.json is.
 */
final class Outputs {
    static final String MARKER = "complete";

    private Outputs() {}

    /** Whether the folder's outputs were all written and synced (its marker is there) */
    static boolean complete(File dir) { return new File(dir, MARKER).isFile(); }

    /** Empties the folder for a new run of its tool (the marker goes first), and makes it */
    static void begin(File dir) throws IOException {
        delete(dir);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
    }

    /** Syncs every output to the disk, then writes the marker: to a temporary name, synced, then renamed (a file's rename) */
    static void finish(File dir) throws IOException {
        syncTree(dir);
        File m = new File(dir, MARKER), tmp = new File(dir, MARKER + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write("complete\n".getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        Files.move(tmp.toPath(), m.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    /** Deletes the folder: its marker first (an error when it stays), then the rest */
    static void delete(File dir) throws IOException {
        File m = new File(dir, MARKER);
        if (m.exists() && !m.delete()) throw new IOException("cannot delete " + m);
        deleteTree(dir);
    }

    /** Syncs every file under f to the disk */
    static void syncTree(File f) throws IOException {
        File[] kids = Files.isSymbolicLink(f.toPath()) ? null : f.listFiles();
        if (kids != null) for (File k : kids) {
            if (k.isDirectory()) syncTree(k);
            else try (java.nio.channels.FileChannel ch = java.nio.channels.FileChannel.open(k.toPath(), java.nio.file.StandardOpenOption.WRITE)) { ch.force(true); }
        }
    }

    // ---- Outputs of a step recorded done (ONDEVICE.md "The job model", Outputs that went) ----
    // job.json lives beside the dump on shared storage and outlives the app; the pack it writes (the app's files/packs/<id>) goes when the app is uninstalled.
    // So a step recorded done is checked against its outputs before a later step uses them, and run again when they went.

    /** The rewrite step's outputs in the pack folder, per module: its rewritten code and its key, which pack_android.py writes last */
    static final String CODE = ".patched.bin", CODE_KEY = ".android.key";

    /** The modules whose rewrite finished in the pack folder (each with its code and its key), sorted; empty when none */
    static java.util.List<String> codeModules(File pack) {
        java.util.List<String> out = new java.util.ArrayList<>();
        String[] l = pack.list();
        if (l != null) for (String f : l)
            if (f.endsWith(CODE_KEY) && new File(pack, f.substring(0, f.length() - CODE_KEY.length()) + CODE).isFile()) out.add(f.substring(0, f.length() - CODE_KEY.length()));
        java.util.Collections.sort(out);
        return out;
    }

    /** Whether the pack folder holds the rewritten code of every module named (pack_android.py --keep-code needs each one's code and key); false for none named */
    static boolean codeComplete(File pack, java.util.List<String> modules) {
        if (modules == null || modules.isEmpty()) return false;
        for (String m : modules) if (!new File(pack, m + CODE).isFile() || !new File(pack, m + CODE_KEY).isFile()) return false;
        return true;
    }

    /** Whether a folder of shader libraries (the shaders step's ondevice/lib, or a pack's shaders/) holds the library of chain version v (v-&lt;V&gt;/dmsh_vk.lib) */
    static boolean libraryComplete(File cache, String v) { return new File(new File(cache, "v-" + v), "dmsh_vk.lib").isFile(); }

    /**
     * The steps to run again because outputs went: step i is when it is recorded done, its outputs are not there (present[i] false), and a step that uses
     * them (uses[i]) is not done yet; then every step that uses it goes too (it was made from what went). A step whose users are all done needs nothing more of
     * its outputs (a finished job deletes its working files).
     */
    static boolean[] redo(boolean[] done, boolean[] present, int[][] uses) {
        int n = done.length;
        boolean[] r = new boolean[n];
        for (int i = 0; i < n; i++) {
            if (!done[i] || present[i] || uses[i] == null) continue;
            for (int u : uses[i]) if (!done[u]) { r[i] = true; break; }
        }
        for (boolean more = true; more; ) {   // the users of a step run again, and theirs
            more = false;
            for (int i = 0; i < n; i++) if (r[i] && uses[i] != null) for (int u : uses[i]) if (done[u] && !r[u]) { r[u] = true; more = true; }
        }
        return r;
    }

    /** Deletes a folder and what is in it (a link goes, never what it points to); a marker of these outputs in a folder goes before the rest of it */
    static void deleteTree(File f) {
        if (!Files.isSymbolicLink(f.toPath())) new File(f, MARKER).delete();
        File[] kids = Files.isSymbolicLink(f.toPath()) ? null : f.listFiles();
        if (kids != null) for (File k : kids) {
            if (k.isDirectory()) deleteTree(k);
            else k.delete();
        }
        f.delete();
    }
}
