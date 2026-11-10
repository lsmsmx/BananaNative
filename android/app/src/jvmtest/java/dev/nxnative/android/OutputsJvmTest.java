package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Outputs on the JVM: a step's outputs written in place in their folder count as complete only once Outputs.finish wrote the marker, and a new run, a stop
 * at any point before the marker, or a delete never leaves them accepted; a step recorded done whose outputs went runs again (Outputs.redo, BananaNative
 * issue #3). {@code <empty folder>}: prints "ok" lines, exits 1 on the first failed check.
 * Run by dm/tools/tests/test_android_outputs.py.
 */
public final class OutputsJvmTest {
    private static void check(boolean ok, String what) {
        if (!ok) { System.out.println("FAIL " + what); System.exit(1); }
        System.out.println("ok " + what);
    }

    private static void write(File f, String text) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    /** What the split writes at its end: the package folder and the tables */
    private static void tool(File split) throws IOException {
        write(new File(split, "pkg/vs_0.bin"), "vs");
        write(new File(split, "pkg/ps_0.bin"), "ps");
        for (String t : new String[] {"shader_map.tsv", "shader_facts.tsv", "shader_pairs.tsv", "pso_pairs.tsv"}) write(new File(split, t), "# " + t + "\n");
    }

    // Job.STEPS and Job.USES: identify, rewrite, split, shaders, pack; the pack step uses the rewrite's code and the shaders step's library
    private static final int REWRITE = 1, SHADERS = 3, PACK = 4;
    private static final int[][] USES = {null, {PACK}, null, {PACK}, null};

    private static boolean[] flags(int... on) {
        boolean[] f = new boolean[5];
        for (int i : on) f[i] = true;
        return f;
    }

    private static String names(boolean[] r) {
        String[] steps = {"identify", "rewrite", "split", "shaders", "pack"};
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < r.length; i++) if (r[i]) b.append(b.length() > 0 ? "," : "").append(steps[i]);
        return b.toString();
    }

    /** What the rewrite step leaves in the pack folder: each module's code, then its key */
    private static void rewrite(File pack, String... modules) throws IOException {
        for (String m : modules) { write(new File(pack, m + Outputs.CODE), "code"); write(new File(pack, m + Outputs.CODE_KEY), "key\n"); }
    }

    /**
     * BananaNative issue #3: job.json (beside the dump, on shared storage) recorded the rewrite done; the pack folder (the app's own storage) went with an
     * uninstall, so the pack step stopped at "no complete main". A step recorded done whose outputs are gone runs again; finished shaders stay.
     */
    private static void stepsRecordedDone(File root) throws IOException {
        File pack = new File(root, "packs/040074A01BF12000"), lib = new File(root, "ondevice/lib");
        java.util.List<String> mods = java.util.Arrays.asList("main", "sdk");
        rewrite(pack, "main", "sdk");
        write(new File(pack, "rewrite_main.log"), "");
        check(Outputs.codeModules(pack).equals(mods), "codeModules lists the modules with code and key");
        check(Outputs.codeComplete(pack, mods) && Outputs.codeComplete(pack, java.util.Collections.singletonList("main")), "the rewrite's outputs are complete");
        check(!Outputs.codeComplete(pack, java.util.Collections.<String>emptyList()), "no module named is not complete");
        new File(pack, "sdk" + Outputs.CODE_KEY).delete();
        check(!Outputs.codeComplete(pack, mods) && Outputs.codeModules(pack).equals(java.util.Collections.singletonList("main")), "a module without its key is not complete (a rewrite cut short)");
        write(new File(lib, "v-abc/dmsh_vk.lib"), "lib");
        check(Outputs.libraryComplete(lib, "abc") && !Outputs.libraryComplete(lib, "def"), "libraryComplete looks for this chain's library");

        // the issue: identify, rewrite, split and shaders done (shaders finished later, after the reinstall), the pack folder gone
        Outputs.deleteTree(pack);
        boolean[] done = flags(0, REWRITE, 2, SHADERS);
        boolean[] present = flags(0, 2, PACK);
        present[REWRITE] = Outputs.codeComplete(pack, mods);
        present[SHADERS] = Outputs.libraryComplete(lib, "abc");
        check(!present[REWRITE] && present[SHADERS], "the pack folder went, the library is there");
        check(names(Outputs.redo(done, present, USES)).equals("rewrite"), "rewrite recorded done, its code gone: the rewrite runs again, the shaders are kept");

        // the same while the shaders step was still running (paused): only the rewrite again
        check(names(Outputs.redo(flags(0, REWRITE), present, USES)).equals("rewrite"), "a paused job with its code gone: the rewrite runs again");
        // both gone (a code-only job that took its library from the pack, Job.adopt): both run again
        check(names(Outputs.redo(done, flags(0, 2, PACK), USES)).equals("rewrite,shaders"), "code and library gone: both run again");
        // everything there: nothing
        check(names(Outputs.redo(done, flags(0, 1, 2, 3, 4), USES)).isEmpty(), "all outputs there: nothing runs again");
        // a finished job deletes its working files (the library in ondevice/lib): nothing needs them, nothing runs again
        check(names(Outputs.redo(flags(0, 1, 2, 3, PACK), flags(0, 1, 2, PACK), USES)).isEmpty(), "a done job's deleted working files are not missing");
        // a user that is done goes with the step it was made from
        check(names(Outputs.redo(flags(0, 1, 2), flags(0, 2, 3, 4), new int[][] {null, {2}, {3}, null, null})).isEmpty(), "users done: nothing runs again");
        check(names(Outputs.redo(flags(0, 1, 2), flags(0, 2, 3, 4), new int[][] {null, {2, 3}, {4}, null, null})).equals("rewrite,split"), "a done user of a step that runs again runs again");
        // the rewrite runs again: its outputs are complete again
        rewrite(pack, "main", "sdk");
        check(Outputs.codeComplete(pack, Outputs.codeModules(pack)), "after the rewrite the code is complete");
    }

    public static void main(String[] a) throws Exception {
        File root = new File(a[0]), split = new File(root, "split");

        Outputs.begin(split);
        check(split.isDirectory() && !Outputs.complete(split), "begin makes the folder, not complete");
        tool(split);
        check(!Outputs.complete(split), "the tool's outputs alone are not complete (a kill before finish)");
        Outputs.finish(split);
        check(Outputs.complete(split) && new File(split, "shader_map.tsv").isFile() && new File(split, "pkg/ps_0.bin").isFile(), "finish marks it complete, outputs in place");
        check(!new File(split, Outputs.MARKER + ".tmp").exists(), "no temporary marker left");

        // a rerun of the step: the marker goes first, the old outputs with it, then a stop before finish
        Outputs.begin(split);
        check(!Outputs.complete(split) && split.isDirectory() && !new File(split, "pkg").exists(), "begin on a complete folder empties it, marker first");
        write(new File(split, "shader_map.tsv"), "# cut short");
        check(!Outputs.complete(split), "a stopped rerun is not complete");

        // a temporary marker a kill left behind is not the marker
        write(new File(split, Outputs.MARKER + ".tmp"), "complete\n");
        check(!Outputs.complete(split), "a temporary marker is not complete");
        tool(split);
        Outputs.finish(split);
        check(Outputs.complete(split), "finish over a temporary marker");

        // the job's end and a refused profile check delete it
        Outputs.delete(split);
        check(!split.exists() && !Outputs.complete(split), "delete removes the folder and its marker");

        // an older build's split/ (made by renaming split.tmp/, no marker) is not taken: the split runs again
        File old = new File(root, "old-split");
        tool(old);
        check(!Outputs.complete(old), "a folder without the marker is not complete");

        // Job.deleteTree on a folder that holds outputs takes the marker before the rest
        Outputs.begin(split);
        tool(split);
        Outputs.finish(split);
        Outputs.deleteTree(split);
        check(!split.exists(), "deleteTree removes a complete folder");

        stepsRecordedDone(root);
        System.out.println("all ok");
    }
}
