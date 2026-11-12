package dev.nxnative.android;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;

/**
 * The readiness check's decisions on the JVM (Readiness.java): requirement rows, facts, version compare, the host's needs, required against degraded, the log line and
 * the report. Run by dm/tools/tests/test_android_readiness.py with {@code <facts file> <rows>}: the facts of a phone (the Honor PNM-N49's, as nxhost_vk_readiness wrote
 * them, or made-up ones) and a profile's rows as build.gradle hands them over. Prints "ok <n>" and exits 0, or the first failure and exits 1.
 */
public final class ReadinessJvmTest {
    private static int n;

    static void check(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
        n++;
    }

    static Readiness.Check find(List<Readiness.Check> l, String title) {
        for (Readiness.Check c : l) if (c.title.equals(title)) return c;
        return null;
    }

    public static void main(String[] a) throws Exception {
        try {
            run(a);
        } catch (AssertionError e) {
            System.out.println("FAIL: " + e.getMessage());
            System.exit(1);
        }
        System.out.println("ok " + n);
    }

    static void run(String[] a) throws Exception {
        // Version and number compare
        check(Readiness.compare("1.4.295", "1.3") > 0 && Readiness.compare("1.3", "1.3.0") == 0 && Readiness.compare("1.2.199", "1.3") < 0, "dotted versions");
        check(Readiness.compare("49152", "49152") == 0 && Readiness.compare("32768", "49152") < 0 && Readiness.compare("0x10", "15") > 0, "numbers and hex");
        check(Readiness.compare("14.7", "14") > 0 && Readiness.compare("11.2", "14") < 0 && Readiness.compare("yes", "1") < 0, "decimals; text reads as 0");

        // Custom drivers (libadrenotools) only on a GPU whose reported vendor ID is Qualcomm's
        check(Readiness.customDriversFit(Map.of("gpu.vendor", "0x5143")) && !Readiness.customDriversFit(Map.of("gpu.vendor", "0x13b5"))
                && Readiness.customDriversFit(Map.of()) && Readiness.customDriversFit(null) && Readiness.customDriversFit(Map.of("gpu.vendor", "?")), "customDriversFit");

        // Rows: malformed ones skipped, title and why split at ": "
        List<Readiness.Req> rows = Readiness.parseReqs("required|ram_gib|14|Memory: needs a lot;bogus|x|1|y;degraded|sparse.format.bc6h|1|Sparse BC6H;required||1|no fact");
        check(rows.size() == 2 && rows.get(0).required && rows.get(0).title.equals("Memory") && rows.get(0).why.equals("needs a lot") && !rows.get(1).required
                && rows.get(1).title.equals("Sparse BC6H") && rows.get(1).why.isEmpty(), "parseReqs");
        check(Readiness.parseReqs(null).isEmpty() && Readiness.parseReqs("").isEmpty(), "no rows");

        // A phone with everything: every check passes
        String all = "vk.instance=1.4.0\nvk.api=1.4.295\nqueue.graphicsCompute=1\nfeature.timelineSemaphore=1\nfeature.dynamicRendering=1\next.VK_KHR_swapchain=1\n"
                + "ext.VK_KHR_map_memory2=1\next.VK_EXT_map_memory_placed=1\nfeature.memoryMapPlaced=1\nfeature.descriptorBindingSampledImageUpdateAfterBind=1\n"
                + "feature.descriptorBindingPartiallyBound=1\nfeature.runtimeDescriptorArray=1\nlimit.maxComputeSharedMemorySize=49152\nsparse.format.bc6h=1\n"
                + "gpu.name=Test GPU\ndriver.name=Test driver\ndriver.info=v1 = one\ndriver.id=8\ndriver.version=0x1\ndriver.custom=\n";
        Map<String, String> f = Readiness.merge(all, 14.66, 36);
        check(f.get("driver.info").equals("v1 = one") && f.get("ram_gib").equals("14.7") && f.get("android.sdk").equals("36"), "facts: the first '=' splits; the launcher's own");
        List<Readiness.Check> l = Readiness.evaluate(f, rows);
        check(!Readiness.any(l, Readiness.Status.FAIL) && !Readiness.any(l, Readiness.Status.WARNING), "all present: " + l);
        check(Readiness.summary("0123456789ABCDEF", f, l).startsWith("[readiness] 0123456789ABCDEF: PASS; Test GPU, Vulkan 1.4.295; driver Test driver (v1 = one), driver ID 8"), "summary " + Readiness.summary("0123456789ABCDEF", f, l));

        // Missing: a required row fails, a degraded one warns, a degraded row of a failed fact is left out, a host need fails once per title
        f = Readiness.merge(all.replace("limit.maxComputeSharedMemorySize=49152", "limit.maxComputeSharedMemorySize=32768").replace("sparse.format.bc6h=1", "sparse.format.bc6h=0")
                .replace("ext.VK_EXT_map_memory_placed=1\nfeature.memoryMapPlaced=1\n", ""), 11.2, 36);
        List<Readiness.Req> game = Readiness.parseReqs("required|ram_gib|10|Memory: a;degraded|ram_gib|14|Memory: b;required|limit.maxComputeSharedMemorySize|49152|Compute shared memory: big;"
                + "degraded|sparse.format.bc6h|1|Sparse BC6H: coarser");
        l = Readiness.evaluate(f, game);
        Readiness.Check placed = find(l, "Placed memory maps");
        check(placed != null && placed.status == Readiness.Status.FAIL && placed.message.contains("not reported") && placed.message.startsWith("This device lacks it; this game needs it."), "placed maps fail: " + placed);
        check(Readiness.of(l, Readiness.Status.FAIL).stream().filter(c -> c.title.equals("Placed memory maps")).count() == 1, "one failure per title");
        Readiness.Check cs = find(l, "Compute shared memory");
        check(cs != null && cs.status == Readiness.Status.FAIL && cs.message.contains("reports 32768, needs 49152") && cs.message.contains("Big."), "shared memory fails: " + cs);
        Readiness.Check bc6 = find(l, "Sparse BC6H");
        check(bc6 != null && bc6.status == Readiness.Status.WARNING && bc6.message.startsWith("Missing: this game runs without it"), "degraded warns");
        check(l.stream().filter(c -> c.title.equals("Memory")).map(c -> c.status).collect(java.util.stream.Collectors.toList()).equals(List.of(Readiness.Status.PASS, Readiness.Status.WARNING)), "memory 11.2: required 10 passes, degraded 14 warns");
        String s = Readiness.summary("0123456789ABCDEF", f, l);
        check(s.contains(": FAIL;") && s.contains("missing (required): Placed memory maps, Compute shared memory") && s.contains("missing (degraded): Memory, Sparse BC6H"), "summary " + s);
        l = Readiness.evaluate(Readiness.merge(all, 8.0, 36), Readiness.parseReqs("required|ram_gib|10|Memory: a;degraded|ram_gib|14|Memory: b"));
        check(find(l, "Memory").status == Readiness.Status.FAIL && Readiness.of(l, Readiness.Status.WARNING).isEmpty(), "the degraded row of a failed fact is left out");

        // No Vulkan facts (no answer, or a host that cannot report): one warning, nothing called missing; the launcher's own facts still checked
        l = Readiness.evaluate(Readiness.merge(null, 15, 30), game);   // (SDK 30: below the app's 31, so the one required fact that fails here is the version)
        check(Readiness.of(l, Readiness.Status.FAIL).size() == 1 && find(l, "Android 12 or newer").status == Readiness.Status.FAIL && find(l, "GPU features").status == Readiness.Status.WARNING, "no facts: " + l);
        l = Readiness.evaluate(Readiness.merge("unchecked=the GPU check did not answer in 8 s\n", 15, 36), game);
        check(!Readiness.any(l, Readiness.Status.FAIL) && find(l, "GPU features").message.contains("did not answer"), "a timeout is no failure: " + l);
        // The driver answered and failed: one failure
        l = Readiness.evaluate(Readiness.merge("error=vkCreateInstance returned -9\nvk.instance=1.3.0\n", 15, 36), game);
        check(find(l, "GPU features").status == Readiness.Status.FAIL && find(l, "GPU features").message.contains("vkCreateInstance returned -9")
                && find(l, "Vulkan 1.3 loader").status == Readiness.Status.PASS && Readiness.of(l, Readiness.Status.FAIL).size() == 1, "an error line: " + l);

        // Status and the driver's environment
        check(Readiness.status("playable") == null && Readiness.status("") == null && Readiness.status("bring-up").status == Readiness.Status.WARNING, "status");
        String[] de = Readiness.driverEnv(List.of("DM_SAVE=/x", "DM_VK_DRIVER=/d/turnip", "DM_VK_HOOKDIR=/h", "DM_VK_DRIVER=system"));
        check(de.length == 3 && de[0].equals("DM_VK_DRIVER=/d/turnip") && de[2].equals("DM_VK_DRIVER=system"), "driverEnv keeps the order (the last wins)");
        // The game's environment for the check (GitHub #40): every line but the paths start sets, in order; unknown DM_ names and the nearest switch
        java.util.Set<String> paths = new java.util.HashSet<>(List.of("DM_SAVE", "DM_ROMFS", "HOME"));
        String[] ge = Readiness.gameEnv(List.of("DM_VK_DRIVER=/d/turnip", "DM_SAVE=/x", "FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1", "TU_DEBUG=startup", "HOME=/h", "DM_VK_DRIVER=system"), paths);
        check(String.join(",", ge).equals("DM_VK_DRIVER=/d/turnip,FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1,TU_DEBUG=startup,DM_VK_DRIVER=system"), "gameEnv: " + String.join(",", ge));
        java.util.Set<String> known = new java.util.HashSet<>(List.of("DM_VK_PSO_ASYNC", "DM_VK_DRIVER", "DM_LANG", "DM_OVERLAY"));
        check(Readiness.unknownSwitches(List.of("DM_VK_PS0_ASYNC=1", "DM_LANG=fr", "FD_DEV_FEATURES=x", "DM_VK_PS0_ASYNC=2", "DM_NOPE=1"), known).equals(List.of("DM_VK_PS0_ASYNC", "DM_NOPE")),
                "unknownSwitches: only DM_ names, once each");
        check("DM_VK_PSO_ASYNC".equals(Readiness.closest("DM_VK_PS0_ASYNC", known)) && Readiness.closest("DM_NOPE", known) == null, "closest");
        String ul = Readiness.unknownLine("DM_VK_PS0_ASYNC", "the game's settings", known);
        check(ul.startsWith("[RED] DM_VK_PS0_ASYNC (the game's settings) is not a switch of this build") && ul.contains("did you mean DM_VK_PSO_ASYNC?"), "unknownLine: " + ul);

        // The report: header, GPU, the checks, every fact
        String rep = Readiness.report("head", f, Readiness.evaluate(f, game));
        check(rep.startsWith("head\nGPU: Test GPU") && rep.contains("[FAIL] Compute shared memory") && rep.contains("-- facts --\n") && rep.contains("limit.maxComputeSharedMemorySize=32768"), "report");

        // A real phone's facts against a real profile's rows (the test's arguments)
        if (a.length >= 2) {
            Map<String, String> real = Readiness.merge(new String(Files.readAllBytes(new File(a[0]).toPath()), StandardCharsets.UTF_8), 14.66, 36);
            List<Readiness.Req> req = Readiness.parseReqs(a[1]);
            check(!req.isEmpty(), "the profile's rows parse");
            List<Readiness.Check> rl = Readiness.evaluate(real, req);
            System.out.println(Readiness.summary("profile", real, rl));
            for (Readiness.Check c : rl) System.out.println(c);
        }
    }
}
