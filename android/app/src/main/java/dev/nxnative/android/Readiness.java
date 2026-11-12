package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The readiness check's decisions (android/README.md "Readiness check"): what this phone and its GPU driver report, against what the host needs for every game
 * ({@link #HOST}, the checks vka_dev.c makes) and what the game's profile asks for (its req rows, dm/games/README.md "Keys"). Each requirement is "required" (the game
 * will not run, or will draw nothing, without it) or "degraded" (it runs, with something missing or at lower detail). The facts are "key=value" lines: the host's
 * nxhost_vk_readiness (vka_readiness.c, on the driver the game would load: whatever it reports, a custom driver's too) and the launcher's own (ram_gib, android.sdk).
 * Nothing here knows a vendor or a game: the numbers come from the driver and the profile. No Android classes: the JVM test runs it on the Mac.
 * The screen (ReadinessActivity) and the start-up check (GameActivity) show these; the screen's pass / warning / fail cards with a fix button, Run again and the shareable
 * report are adapted from the author's earlier readiness check.
 */
final class Readiness {
    enum Status { PASS, WARNING, FAIL, INFO }

    /** One requirement: the fact (a key of the facts) must be at least min (a number, a dotted version, or 0x hex). what: "title: why" */
    static final class Req {
        final boolean required;
        final String fact, min, title, why;

        Req(boolean required, String fact, String min, String what) {
            this.required = required;
            this.fact = fact;
            this.min = min;
            int c = what.indexOf(": ");
            this.title = c > 0 ? what.substring(0, c) : what;
            this.why = c > 0 ? what.substring(c + 2) : "";
        }
    }

    static final class Check {
        final String title, message, fix, fact;
        final Status status;

        Check(String title, Status status, String message, String fix) { this(title, status, message, fix, null); }

        /** fact: the requirement's fact key (Req.fact) when the check is one, so a caller can tell what failed without matching titles */
        Check(String title, Status status, String message, String fix, String fact) {
            this.fact = fact;
            this.title = title;
            this.status = status;
            this.message = message;
            this.fix = fix;
        }

        @Override public String toString() { return "[" + status + "] " + title + ": " + message; }
    }

    /** What the host needs whatever the game: vka_dev.c stops without the first ones (Vulkan 1.3, the queue, timeline semaphores, dynamic rendering), and without
     *  placed memory maps, update-after-bind texture tables or a swapchain it runs but draws or shows nothing (its [RED] lines say so) */
    static final List<Req> HOST = Collections.unmodifiableList(List.of(
            new Req(true, "android.sdk", "31", "Android 12 or newer: the app is built for it"),
            new Req(true, "vk.instance", "1.3", "Vulkan 1.3 loader: the graphics replay is written for Vulkan 1.3"),
            new Req(true, "vk.api", "1.3", "Vulkan 1.3 GPU driver: the graphics replay is written for Vulkan 1.3"),
            new Req(true, "queue.graphicsCompute", "1", "A graphics and compute queue: everything is submitted to one such queue"),
            new Req(true, "feature.timelineSemaphore", "1", "Timeline semaphores: the game's fences are built on them"),
            new Req(true, "feature.dynamicRendering", "1", "Dynamic rendering: every render pass is recorded with it"),
            new Req(true, "ext.VK_KHR_swapchain", "1", "Swapchain: without it no picture reaches the screen"),
            new Req(true, "ext.VK_KHR_map_memory2", "1", "Memory map 2: placed memory maps need it"),
            new Req(true, "ext.VK_EXT_map_memory_placed", "1", "Placed memory maps: the game's GPU memory is mapped at the game's own addresses; without them nothing is drawn"),
            new Req(true, "feature.memoryMapPlaced", "1", "Placed memory maps: the game's GPU memory is mapped at the game's own addresses; without them nothing is drawn"),
            new Req(true, "feature.descriptorBindingSampledImageUpdateAfterBind", "1", "Texture tables (update after bind): the game's texture pools live in them"),
            new Req(true, "feature.descriptorBindingPartiallyBound", "1", "Texture tables (partially bound): the game's texture pools live in them"),
            new Req(true, "feature.runtimeDescriptorArray", "1", "Texture tables (runtime arrays): the game's texture pools live in them")));

    /** A profile's req rows as build.gradle hands them over (Profile.reqs): "level|fact|min|what" rows, ';' between them; a malformed row is skipped */
    static List<Req> parseReqs(String table) {
        List<Req> out = new ArrayList<>();
        if (table == null) return out;
        for (String row : table.split(";")) {
            String[] c = row.split("\\|", -1);
            if (c.length < 4 || !(c[0].equals("required") || c[0].equals("degraded")) || c[1].isEmpty() || c[2].isEmpty()) continue;
            out.add(new Req(c[0].equals("required"), c[1].trim(), c[2].trim(), c[3].trim().isEmpty() ? c[1].trim() : c[3].trim()));
        }
        return out;
    }

    /** "key=value" lines (the first '=' splits) */
    static Map<String, String> facts(String text) {
        Map<String, String> out = new LinkedHashMap<>();
        if (text == null) return out;
        for (String l : text.split("\n")) {
            int eq = l.indexOf('=');
            if (eq > 0) out.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
        }
        return out;
    }

    /** a compared with b: numbers, 0x hex, or dotted versions part by part (1.3 &lt; 1.3.204 &lt; 1.4); a value that is no number reads as 0 */
    static int compare(String a, String b) {
        String[] x = a.trim().split("\\."), y = b.trim().split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            double p = i < x.length ? num(x[i]) : 0, q = i < y.length ? num(y[i]) : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        return 0;
    }

    private static double num(String s) {
        try {
            return s.startsWith("0x") || s.startsWith("0X") ? Long.parseLong(s.substring(2), 16) : Double.parseDouble(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** The facts this game is checked against: the host's Vulkan facts (null: this host cannot report them) and the launcher's */
    static Map<String, String> merge(String vulkan, double ramGib, int sdk) {
        Map<String, String> f = facts(vulkan);
        if (vulkan == null && !f.containsKey("unchecked")) f.put("unchecked", "this build's host cannot report the GPU's features");
        f.put("ram_gib", String.format(Locale.ROOT, "%.1f", ramGib));
        f.put("android.sdk", String.valueOf(sdk));
        return f;
    }

    /** Checked: the host's needs, then the game's rows. A degraded row of a fact whose required row failed is left out (one message per missing thing) */
    static List<Check> evaluate(Map<String, String> facts, List<Req> game) {
        List<Check> out = new ArrayList<>();
        // error: the driver answered and failed (no Vulkan library, no instance, no GPU): the game cannot run either. unchecked: nobody answered (the check timed out,
        // or the host cannot report): nothing is known, so nothing is called missing; one warning says the GPU was not checked
        String err = facts.get("error"), unchecked = facts.get("unchecked");
        List<Req> all = new ArrayList<>(HOST);
        all.addAll(game);
        List<String> failed = new ArrayList<>(), seen = new ArrayList<>();
        for (Req r : all) {
            String have = facts.get(r.fact);
            boolean vulkan = !r.fact.equals("ram_gib") && !r.fact.startsWith("android.");
            if (have == null && vulkan && (err != null || unchecked != null)) {   // nothing to compare with: said once
                if (!seen.contains("error")) {
                    seen.add("error");
                    out.add(err != null ? new Check("GPU features", Status.FAIL, "The GPU driver could not be used (" + err + "): the game needs Vulkan 1.3.", "driver")
                            : new Check("GPU features", Status.WARNING, "Not checked (" + unchecked + "): whether this GPU has what the game needs is not known.", null));
                }
                continue;
            }
            if (!r.required && failed.contains(r.fact) || failed.contains("title " + r.title)) continue;
            String key = r.fact + " " + r.min + " " + r.title;
            if (seen.contains(key)) continue;   // the same requirement twice (the host's and a profile's)
            seen.add(key);
            if ("1".equals(facts.get("memory.fdAlias")) && compare("1", r.min) >= 0 &&
                    (r.fact.equals("ext.VK_EXT_map_memory_placed") || r.fact.equals("feature.memoryMapPlaced") || r.fact.equals("ext.VK_KHR_map_memory2"))) {
                out.add(new Check(r.title, Status.PASS, "Guest-address memory is available through a tested exported-memory alias (memory.fdAlias=1).", null));
                continue;
            }
            boolean ok = have != null && compare(have, r.min) >= 0;
            String shown = have == null ? "not reported" : "reports " + have;
            if (ok) out.add(new Check(r.title, Status.PASS, "Present (" + label(r.fact) + " " + have + ", needs " + r.min + ").", null));
            else {
                if (r.required) { failed.add(r.fact); failed.add("title " + r.title); }
                String msg = (r.required ? "This device lacks it; this game needs it. " : "Missing: this game runs without it, with less. ")
                        + (r.why.isEmpty() ? "" : r.why.substring(0, 1).toUpperCase(Locale.ROOT) + r.why.substring(1) + ". ")
                        + "(" + label(r.fact) + " " + shown + ", needs " + r.min + ")";
                out.add(new Check(r.title, r.required ? Status.FAIL : Status.WARNING, msg, r.fact.equals("ram_gib") ? "memory" : "driver", r.fact));
            }
        }
        return out;
    }

    /** The game's status on Android (its readiness.tsv status row): a game in bring-up gets a warning of its own, whatever the phone has */
    static Check status(String status) {
        if (status == null || status.isEmpty() || status.equals("playable")) return null;
        if (status.equals("bring-up"))
            return new Check("Status on Android", Status.WARNING, "This game is still being brought up on Android: it may stop or draw wrongly on any phone, this one included.", null);
        return new Check("Status on Android", Status.INFO, status, null);
    }

    /** Of a game's environment (KEY=VALUE, in start's order), the lines that pick its GPU driver: what the readiness facts must be read on */
    /**
     * The game's environment for the check, as start() will set it (KEY=VALUE lines in start's order, the last of a key winning), less the keys in skip (the paths
     * start sets itself). A driver reads some variables once, when its first device is made (Mesa's FD_DEV_FEATURES, TU_DEBUG): the check makes that device in the
     * game's process, so it must see them too (GitHub #40)
     */
    static String[] gameEnv(List<String> env, java.util.Set<String> skip) {
        List<String> out = new ArrayList<>();
        for (String kv : env) {
            int eq = kv.indexOf('=');
            if (eq > 0 && !skip.contains(kv.substring(0, eq).trim())) out.add(kv);
        }
        return out.toArray(new String[0]);
    }

    /** The DM_ names of these lines that are not in known (BuildConfig.NX_ENV_SWITCHES: what this build reads), once each, in order */
    static List<String> unknownSwitches(List<String> lines, java.util.Set<String> known) {
        List<String> out = new ArrayList<>();
        for (String kv : lines) {
            int eq = kv.indexOf('=');
            String k = eq > 0 ? kv.substring(0, eq).trim() : "";
            if (k.startsWith("DM_") && !known.contains(k) && !out.contains(k)) out.add(k);
        }
        return out;
    }

    /** The known switch closest to an unknown name (at most 2 edits, as DM_VK_PS0_ASYNC for DM_VK_PSO_ASYNC), null when none is that close */
    static String closest(String name, java.util.Set<String> known) {
        String best = null;
        int bd = 3;
        for (String k : known) {
            if (Math.abs(k.length() - name.length()) >= bd) continue;
            int d = edits(name, k);
            if (d < bd || d == bd && best != null && k.compareTo(best) < 0) { bd = d; best = k; }
        }
        return bd <= 2 ? best : null;
    }

    private static int edits(String a, String b) {
        int[] prev = new int[b.length() + 1], cur = new int[b.length() + 1];
        for (int j = 0; j <= b.length(); j++) prev[j] = j;
        for (int i = 1; i <= a.length(); i++) {
            cur[0] = i;
            for (int j = 1; j <= b.length(); j++) cur[j] = Math.min(Math.min(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + (a.charAt(i - 1) == b.charAt(j - 1) ? 0 : 1));
            int[] t = prev; prev = cur; cur = t;
        }
        return prev[b.length()];
    }

    /** The [RED] line for an unknown DM_ name of a user line (from: env.txt, the game's settings, env-<id>.txt) */
    static String unknownLine(String name, String from, java.util.Set<String> known) {
        String near = closest(name, known);
        return "[RED] " + name + " (" + from + ") is not a switch of this build: nothing reads it" + (near != null ? "; did you mean " + near + "?" : "") + " (docs/env_switches.tsv lists the switches)";
    }

    static String[] driverEnv(List<String> env) {
        List<String> out = new ArrayList<>();
        for (String kv : env) if (kv.startsWith("DM_VK_DRIVER=") || kv.startsWith("DM_VK_HOOKDIR=")) out.add(kv);
        return out.toArray(new String[0]);
    }

    private static String label(String fact) {
        if (fact.equals("ram_gib")) return "memory in GiB:";
        if (fact.equals("android.sdk")) return "Android SDK";
        return fact + ":";
    }

    static boolean any(List<Check> l, Status s) { for (Check c : l) if (c.status == s) return true; return false; }

    static List<Check> of(List<Check> l, Status s) {
        List<Check> out = new ArrayList<>();
        for (Check c : l) if (c.status == s) out.add(c);
        return out;
    }

    /** Qualcomm's PCI vendor ID, as VkPhysicalDeviceProperties.vendorID reports it (gpu.vendor) */
    static final long VENDOR_QUALCOMM = 0x5143;

    /**
     * Whether the custom drivers the app installs can run on this GPU: they are Adreno drivers (Turnip, Qualcomm's builds) loaded through libadrenotools, so only on
     * a GPU whose reported vendor ID is Qualcomm's. A Mali, Xclipse or PowerVR GPU has its system driver alone. Unknown (no facts): true, as before
     */
    static boolean customDriversFit(Map<String, String> f) {
        String v = f == null ? null : f.get("gpu.vendor");
        if (v == null || v.isEmpty()) return true;
        try { return Long.decode(v.trim()) == VENDOR_QUALCOMM; } catch (NumberFormatException e) { return true; }
    }

    /**
     * The system driver in a few words for a page's state chip (Game settings), from the facts the launcher keeps (driver.id, driver.version): Qualcomm's own driver with
     * the version it shows in its build string (driverVersion 512.842.44 reads 842.44), Mesa Turnip with its Mesa version; null for another driver (the caller says "System driver")
     */
    static String driverWord(Map<String, String> f) {
        if (f == null) return null;
        long v;
        try { v = Long.decode(f.getOrDefault("driver.version", "").trim()); } catch (NumberFormatException e) { return null; }
        switch (f.getOrDefault("driver.id", "").trim()) {
            case "8": return "Qualcomm " + ((v >> 12) & 0x3ff) + "." + (v & 0xfff);   // VK_DRIVER_ID_QUALCOMM_PROPRIETARY
            case "13": return "Turnip " + ((v >> 22) & 0x3ff) + "." + ((v >> 12) & 0x3ff) + "." + (v & 0xfff);   // VK_DRIVER_ID_MESA_TURNIP
            default: return null;
        }
    }

    /** A GPU's name as GLES's renderer string gives it, made short for the chip: "Adreno (TM) 840" reads "Adreno 840"; null or blank: null */
    static String gpuName(String renderer) {
        if (renderer == null) return null;
        String s = renderer.replace("(TM)", "").replace("(R)", "").replaceAll("\\s+", " ").trim();
        return s.isEmpty() ? null : s;
    }

    /** The GPU and driver in one line: name, driver name and info, driver ID and version, and a custom driver's name */
    static String gpu(Map<String, String> f) {
        if (!f.containsKey("gpu.name")) return "GPU not reported" + (f.containsKey("error") ? " (" + f.get("error") + ")" : f.containsKey("unchecked") ? " (" + f.get("unchecked") + ")" : "");
        StringBuilder s = new StringBuilder(f.get("gpu.name"));
        s.append(", Vulkan ").append(f.getOrDefault("vk.api", "?"));
        s.append("; driver ").append(f.getOrDefault("driver.name", "?"));
        String info = f.getOrDefault("driver.info", "");
        if (!info.isEmpty()) s.append(" (").append(info).append(')');
        s.append(", driver ID ").append(f.getOrDefault("driver.id", "?")).append(", version ").append(f.getOrDefault("driver.version", "?"));
        String custom = f.getOrDefault("driver.custom", "");
        s.append(custom.isEmpty() ? ", the system's driver" : ", custom driver " + custom);
        return s.toString();
    }

    /** The log's one line ("[readiness] ..."): every player's log says what the device has and what the check decided */
    static String summary(String titleId, Map<String, String> f, List<Check> l) {
        StringBuilder s = new StringBuilder("[readiness] ").append(titleId).append(": ");
        s.append(any(l, Status.FAIL) ? "FAIL" : any(l, Status.WARNING) ? "WARN" : "PASS");
        s.append("; ").append(gpu(f)).append("; RAM ").append(f.get("ram_gib")).append(" GiB, Android SDK ").append(f.get("android.sdk"));
        for (Status st : new Status[] {Status.FAIL, Status.WARNING}) {
            List<Check> m = of(l, st);
            if (m.isEmpty()) continue;
            s.append("; ").append(st == Status.FAIL ? "missing (required): " : "missing (degraded): ");
            for (int i = 0; i < m.size(); i++) s.append(i == 0 ? "" : ", ").append(m.get(i).title);
        }
        return s.toString();
    }

    /** The report a player copies or shares: the device, the GPU and driver, the checks, then every fact */
    static String report(String header, Map<String, String> f, List<Check> l) {
        StringBuilder s = new StringBuilder(header).append('\n');
        s.append("GPU: ").append(gpu(f)).append('\n');
        s.append("RAM: ").append(f.get("ram_gib")).append(" GiB; Android SDK ").append(f.get("android.sdk")).append('\n');
        for (Check c : l) s.append(c).append('\n');
        s.append("-- facts --\n");
        for (Map.Entry<String, String> e : f.entrySet()) s.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        return s.toString();
    }
}
