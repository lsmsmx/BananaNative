package dev.nxnative.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.List;

/**
 * A simulated engine (-PnxPackEngine=fake, the default until libnxpack.so exists) so that the whole flow (pick, progress, notification, pause, kill, resume, Play
 * gating) runs on a phone today. It takes the tools' argv as PackService gives them, writes their progress lines into the log, sleeps instead of working and writes
 * placeholder outputs where the real tools write theirs; its pack (ondevice/fake-pack, never android-pack) has pack.tsv, empty modules and an empty library, so the
 * launcher's checks pass but it is never started. The translation keeps a count (fake.done, written by rename every 100 shaders), so a kill or a pause resumes it
 * where it was, as the real per-shader results will. NX_FAKE_RATE (shaders a second, default 250) and NX_FAKE_FAIL (a step's tool name: fails halfway) for tests.
 */
final class FakeEngine implements PackEngine {
    static final int DKB_SHADERS = 22852;   // DKB's shader count (ONDEVICE.md), for a game without a count in its profile
    private volatile boolean stop;

    @Override public boolean simulated() { return true; }

    @Override public void interrupt() { stop = true; }

    @Override public int run(String tool, String[] argv, String logPath) {
        stop = false;
        List<String> a = Arrays.asList(argv);
        try (PrintStream log = new PrintStream(new FileOutputStream(logPath, true), true, "UTF-8")) {
            log.println("[fake] " + tool + " " + String.join(" ", argv));
            String name = new File(tool).getName();
            boolean fail = name.equals(System.getenv("NX_FAKE_FAIL"));
            try {
                if (name.equals("pack_android.py") && a.contains("--code-only")) return rewrite(log, new File(arg(a, "--out")), fail);
                if (name.equals("pack_android.py")) return pack(log, new File(arg(a, "--out")), new File(arg(a, "--shader-cache")));
                if (name.equals("shader_split.py")) return split(log, new File(argv[2]).getParentFile(), fail);
                if (name.equals("vk_android_build.py")) {   // the library at its end, as the real one writes it
                    int rc = translate(log, new File(argv[3]), fail);
                    return rc != 0 ? rc : library(log, new File(argv[3]));
                }
                log.println("[RED] fake engine: no simulation for " + tool);
                return 2;
            } catch (InterruptedException e) {
                return INTERRUPTED;
            } catch (IOException e) {
                log.println("[RED] " + e);
                return 1;
            }
        } catch (IOException e) {
            return 1;
        }
    }

    private static String arg(List<String> a, String key) {
        int i = a.indexOf(key);
        return i >= 0 && i + 1 < a.size() ? a.get(i + 1) : ".";
    }

    /** Sleeps ms, in steps, returning early with InterruptedException when interrupted */
    private void work(long ms) throws InterruptedException {
        long end = System.nanoTime() + ms * 1_000_000;
        while (System.nanoTime() < end) {
            if (stop) throw new InterruptedException();
            Thread.sleep(Math.min(50, Math.max(1, (end - System.nanoTime()) / 1_000_000)));
        }
        if (stop) throw new InterruptedException();
    }

    private static void write(File f, String text) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        Files.write(tmp.toPath(), text.getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    private int rewrite(PrintStream log, File out, boolean fail) throws InterruptedException, IOException {
        out.mkdirs();
        String[] modules = {"main", "sdk"};
        for (int i = 0; i < modules.length; i++) {
            log.println("progress " + i + " " + modules.length + " rewrite " + modules[i]);
            work(4000);
            if (fail) { log.println("[RED] simulated failure (NX_FAKE_FAIL)"); return 1; }
            write(new File(out, modules[i] + ".patched.bin"), "");
            write(new File(out, modules[i] + ".android.key"), "simulated\n");   // last, as pack_android.py writes it (the pack step checks both: Outputs.codeComplete)
            log.println("rewrote " + modules[i] + " (simulated)");
        }
        log.println("progress " + modules.length + " " + modules.length + " rewrite");
        return 0;
    }

    private int split(PrintStream log, File out, boolean fail) throws InterruptedException, IOException {
        out.mkdirs();
        int total = 158_000;   // RomFS files
        for (int d = 0; d <= total; d += 2000) {
            log.println("progress: " + d + "/" + total);
            if (fail && d > total / 2) { log.println("[RED] simulated failure (NX_FAKE_FAIL)"); return 1; }
            work(150);
        }
        new File(out, "pkg").mkdirs();
        for (String t : new String[] {"shader_map.tsv", "shader_facts.tsv", "shader_pairs.tsv", "pso_pairs.tsv"}) write(new File(out, t), "# simulated\n");
        return 0;
    }

    private int translate(PrintStream log, File cache, boolean fail) throws InterruptedException, IOException {
        cache.mkdirs();
        final int SHADERS = Profile.of(game()) != null && Profile.of(game()).shaderTotal > 0 ? Profile.of(game()).shaderTotal : DKB_SHADERS;   // the game's shader map, as the real step
        File count = new File(cache, "fake.done");
        int done = 0;
        try {
            done = Integer.parseInt(new String(Files.readAllBytes(count.toPath()), StandardCharsets.UTF_8).trim());
        } catch (IOException | NumberFormatException e) {
            // a fresh translation
        }
        if (done > 0) log.println("resuming: " + done + " of " + SHADERS + " shaders have results");
        int rate = 250;
        try {
            rate = Math.max(1, Integer.parseInt(System.getenv("NX_FAKE_RATE")));
        } catch (NumberFormatException e) {
            // default
        }
        int jobs = 1;
        try {
            jobs = Integer.parseInt(System.getenv("NX_PACK_JOBS"));
        } catch (NumberFormatException e) {
            // default
        }
        log.println("translating " + (SHADERS - done) + " shaders with " + jobs + " workers (simulated, " + rate + " a second)");
        long last = 0;
        try {
            while (done < SHADERS) {
                int n = Math.min(25, SHADERS - done);
                work(Math.max(1, n * 1000L / rate));
                done += n;
                if (fail && done > SHADERS / 2) { log.println("[RED] simulated failure (NX_FAKE_FAIL)"); return 1; }
                if (done % 100 == 0 || done == SHADERS) write(count, String.valueOf(done));
                long now = System.nanoTime();
                if (now - last > 250_000_000L || done == SHADERS) { log.println("progress " + done + " " + SHADERS + " translate"); last = now; }
            }
        } catch (InterruptedException e) {
            log.println("stopped after " + done + " shaders");
            throw e;
        }
        return 0;
    }

    private int library(PrintStream log, File cache) throws InterruptedException, IOException {
        File lib = new File(cache, "v-" + BuildConfig.NX_DMSH_VK_CHAIN);
        Job.deleteTree(lib);   // written in place, as vk_android_build.py does (no folder renamed on shared storage: Outputs)
        lib.mkdirs();
        for (int i = 0; i <= 10; i++) {
            log.println("progress " + i + " 10 library");
            work(600);
        }
        write(new File(lib, "dmsh_vk.lib"), "");
        write(new File(lib, "inputs.tsv"), "game\t" + game() + "\n");
        return 0;
    }

    /** The job's game: DM_GAME, which PackService sets for every tool (the first of the APK's games without it) */
    private static String game() { String g = System.getenv("DM_GAME"); return g != null && !g.isEmpty() ? g : Profile.first().titleId; }

    private int pack(PrintStream log, File out, File cache) throws InterruptedException, IOException {
        log.println("progress 0 1 pack");
        work(1500);
        String rel = "shaders/v-" + BuildConfig.NX_DMSH_VK_CHAIN + "/dmsh_vk.lib";
        File lib = new File(out, rel);
        lib.getParentFile().mkdirs();
        File src = new File(cache, "v-" + BuildConfig.NX_DMSH_VK_CHAIN + "/dmsh_vk.lib");
        if (!src.isFile()) { log.println("[RED] no library in " + cache); return 1; }
        Files.copy(src.toPath(), lib.toPath(), StandardCopyOption.REPLACE_EXISTING);
        for (String m : new String[] {"main", "sdk"}) {   // a shaders-only rebuild skipped the rewrite: placeholders, as the rewrite step writes
            File f = new File(out, m + ".patched.bin");
            if (!f.isFile()) write(f, "");
            File k = new File(out, m + ".android.key");
            if (!k.isFile()) write(k, "simulated\n");
        }
        write(new File(out, "pack.tsv"), "format\t1\ntarget\tandroid\ntitle_id\t" + game() + "\nname\t" + (Profile.of(game()) != null ? Profile.of(game()).name : game()) + " (simulated)\nversion\t"
                + System.getenv("NX_PACK_VERSION") + "\nmodules\tmain sdk\nromfs\t" + ("0".equals(System.getenv("NX_PACK_VERSION")) ? "base" : "updates/" + System.getenv("NX_PACK_VERSION")) + "/romfs\nshaders\t" + rel + "\nsimulated\t1\n");
        log.println("progress 1 1 pack");
        return 0;
    }
}
