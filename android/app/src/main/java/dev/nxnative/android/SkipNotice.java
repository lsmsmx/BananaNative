package dev.nxnative.android;

/**
 * The in-game notice for skipped work (android/README.md, In-game notices; pure Java: dm/tools/tests/test_android_skipnotice.py runs it on the JVM). The host counts the draws
 * and dispatches the Vulkan replay cannot run for causes of the GPU or its driver (refused pipelines and shaders, a missing mesh shader or other feature) and, once a run,
 * when they are a large share two windows in a row, answers Native.skipStatus() with "&lt;percent&gt; &lt;causes&gt;" (vka_exec.c nxhost_skip_status; DM_VK_SKIP_WARN is the
 * threshold, 0 = off). GameActivity polls it and shows this text once, small and dismissible.
 */
final class SkipNotice {
    private SkipNotice() {}

    /** The host's share of skipped draws in percent, 0 for no warning (an empty, missing or garbled status) */
    static int percent(String status) {
        if (status == null) return 0;
        String s = status.trim();
        int sp = s.indexOf(' ');
        try {
            int p = Integer.parseInt(sp < 0 ? s : s.substring(0, sp));
            return p >= 1 && p <= 100 ? p : 0;
        } catch (NumberFormatException e) { return 0; }
    }

    /** What the host named as the causes (for the log); empty when it named none */
    static String causes(String status) {
        if (status == null) return "";
        String s = status.trim();
        int sp = s.indexOf(' ');
        return sp < 0 ? "" : s.substring(sp + 1).trim();
    }

    /** Whether the largest cause is a device without mesh shading (the host names it first, vka_skipwarn.h): no other driver is known to help, so the notice only says what is missing */
    static boolean noMesh(String status) { return causes(status).startsWith("mesh draws without VK_EXT_mesh_shader"); }

    /** The notice for a device without mesh shading: honest about the cause, no driver advice */
    static String textNoMesh(int percent) {
        return "This GPU has no mesh shading, so some models are missing from the picture (" + percent + " % of draws skipped). Another GPU driver may not change this.";
    }

    /** The notice: what is wrong, where to change it, and the driver in use now (null or empty = the system's) */
    static String text(int percent, String driver) {
        String d = driver == null || driver.isEmpty() ? "System" : driver;
        return "Part of the picture is not drawn on this GPU driver (" + percent + " % of draws skipped). Another driver may draw it: Settings › GPU driver, or this game's Game settings. Driver now: " + d + ".";
    }
}
