package dev.nxnative.android;

import java.util.Locale;

/**
 * Prepare's words for the player (PrepareText): short main texts with no file path or tool output in them (those go to Details), the right offer after a failure
 * (Retry, Try anyway, nothing), no "0 of 0" in the progress line, and an older job's long error moved to Details. Also Profile.missing's one sentence. Run by
 * dm/tools/tests/test_android_preparetext.py, which compiles this with PrepareText.java, Profile.java and a stub BuildConfig.
 */
public final class PrepareTextJvmTest {
    static int n;

    static void check(boolean ok, String what) {
        if (!ok) { System.out.println("FAIL: " + what); System.exit(1); }
        n++;
    }

    /** A main text the player reads: no path, no exit code, no [RED], at most three sentences */
    static void plain(String s, String what) {
        check(!s.contains("/") && !s.contains("[RED]") && !s.contains("exit code") && !s.contains("log:"), what + ": no technical part in the main text: " + s);
        check(s.split("\\. ").length <= 3, what + ": short: " + s);
    }

    public static void main(String[] a) {
        Locale.setDefault(Locale.US);
        String log = "/storage/emulated/0/Switch2/0400A3A01EE4C000-work/ondevice/logs/shaders.log";

        PrepareText.Failure f = PrepareText.toolStopped("Translating shaders", 1, "[RED] 12 of 8157 shaders did not build: no library written (DMSH_VK_REQUIRE_ALL)", log);
        plain(f.text, "shaders that did not build");
        check(f.fix.equals(PrepareText.FIX_ANYWAY) && f.detail.contains(log) && f.detail.contains("12 of 8157") && f.detail.contains("exit code 1"), "Try anyway, the tool's line in Details: " + f.detail);

        f = PrepareText.toolStopped("Finding the shaders", 1, "[RED] MemoryError in 0x38f972af.fdata", log);
        plain(f.text, "memory");
        check(f.fix.equals(PrepareText.FIX_RETRY) && f.text.contains("ran out of memory") && f.text.contains("Retry"), "memory: close apps, Retry: " + f.text);

        f = PrepareText.toolStopped("Translating shaders", 1, "[RED] 3 of the 8157 shaders to build are not in the package /x/pkg: it is not a split", log);
        plain(f.text, "not in the package");
        check(f.fix.equals(PrepareText.FIX_RETRY) && f.text.contains("dumped the game again"), "a dump of other shaders: dump again: " + f.text);

        // Try anyway on another game version: vk_android_build.py open_packs' line (DMSH_VK_REQUIRE_ALL=0), and the stop without it is not taken for it
        long[] miss = PrepareText.missingShaders("[RED] 120 of the 8161 shaders to build are not in the package /x/pkg (a game version the profile's shader_map.tsv was not made from): "
                + "the library is written without them (DMSH_VK_REQUIRE_ALL=0). abc: not in the package");
        check(miss != null && miss[0] == 120 && miss[1] == 8161, "missing shaders counted");
        check(PrepareText.missingShaders("[RED] 3 of the 8157 shaders to build are not in the package /x/pkg: it is not a split") == null, "the stop is not a count");
        String mt = PrepareText.missingShadersText(120, 8161, "update 655360");
        plain(mt, "missing shaders");
        check(mt.contains("120 of its 8,161") && mt.endsWith("All of them are in update 655360."), "missing shaders text: " + mt);

        f = PrepareText.toolStopped("Rewriting the game code", 2, null, log);
        plain(f.text, "any other stop");
        check(f.fix.equals(PrepareText.FIX_RETRY) && f.text.startsWith("Rewriting the game code stopped.") && f.detail.endsWith("Log: " + log), "any other stop: Retry, Details has the log: " + f.detail);

        // A tool that stops on an uncaught exception: the card had "Finding the shaders: exit code 1" and no reason (a player's split of Ocarina of Time from a .nup).
        // The runner's [RED] line (dm_pack.c) is the reason; without one, the traceback's last line; an earlier red line gives way to a later traceback
        String[] traced = {"targeted split: 1,442 of 186,583 RomFS files hold the shaders", "[RED] /Shader/a.bfsha: bad header (its shaders are left out)",
                "progress: 16/1442", "Traceback (most recent call last):", "File \"/x/repo/dm/gfx/tools/shader_split.py\", line 3150, in main",
                "want, why = profile_files(files, main_v)", "~~~~~~~~~~~~~^^^^^^^^^^^^^^^^", "raise KeyError(s)", "KeyError: 'Shader/b.bfsha'"};
        PrepareText.StopReason sr = new PrepareText.StopReason();
        for (String l : traced) sr.line(l);
        check("KeyError: 'Shader/b.bfsha'".equals(sr.reason), "the traceback's last line, after an earlier red line: " + sr.reason);
        sr.line("[RED] KeyError: 'Shader/b.bfsha' (shader_split.py:3150)");
        check("[RED] KeyError: 'Shader/b.bfsha' (shader_split.py:3150)".equals(sr.reason), "the runner's red line: " + sr.reason);
        f = PrepareText.toolStopped("Finding the shaders", 1, sr.reason, log);
        plain(f.text, "an uncaught exception");
        check(f.fix.equals(PrepareText.FIX_RETRY) && f.detail.startsWith("Finding the shaders: exit code 1\nKeyError: 'Shader/b.bfsha' (shader_split.py:3150)\nLog: "), "its reason in Details: " + f.detail);
        sr = new PrepareText.StopReason();
        for (String l : new String[] {"Traceback (most recent call last):", "File \"t.py\", line 1, in <module>", "MemoryError"}) sr.line(l);
        f = PrepareText.toolStopped("Finding the shaders", 1, sr.reason, log);
        check(f.text.contains("ran out of memory"), "a MemoryError traceback: the memory text: " + f.text);
        sr = new PrepareText.StopReason();
        for (String l : new String[] {"KeyError: 'x'", "shaders: 42848 (compute 4125)", "SystemExit"}) sr.line(l);
        check(sr.reason == null, "no traceback, no reason: " + sr.reason);
        check(sr.line("[RED] the dump has no RomFS") && !sr.line("progress: 1/2"), "red lines are said so (the Tail's missing-shader count)");

        f = PrepareText.crashed("Translating shaders", "java.io.IOException: /data/user/0/x: EACCES");
        plain(f.text, "an exception");
        check(f.detail.contains("EACCES") && f.fix.equals(PrepareText.FIX_RETRY), "the exception in Details");

        f = PrepareText.killed("Rewriting the game code", 2, "2.7");
        plain(f.text, "killed");
        check(f.text.contains("2 times") && f.text.contains("2.7 GB"), "killed: " + f.text);

        f = PrepareText.shadersDiffer("Pokemon Pokopia", "update 655360", "The split's shader_map.tsv differs from the app's dm/games/0400A3A01EE4C000/shader_map.tsv.");
        plain(f.text, "shaders differ");
        check(f.fix.equals(PrepareText.FIX_ANYWAY) && f.text.contains("Try anyway"), "shaders differ: Try anyway");

        // The trigger: a job of a game this APK does not include gets one sentence and no Retry
        String missing = Profile.missing("0400A3A01EE4C000", "Pokemon Pokopia");
        check(missing.equals("This app version does not include Pokemon Pokopia (0400A3A01EE4C000). Install a build that includes it."), "Profile.missing: " + missing);
        check(Profile.missing("0400A3A01EE4C000", null).startsWith("This app version does not include this game (0400A3A01EE4C000)."), "Profile.missing without a name");
        check(Profile.missing("040074A01BF12000", "Donkey Kong Bananza") == null, "a game the APK has");
        f = PrepareText.notIncluded(missing);
        check(f.fix.equals(PrepareText.FIX_NONE) && f.text.equals(missing), "not included: nothing to retry");

        // An older job (no "fix"): its long error goes to Details, the card says where it stopped
        String old = "cannot compare the dump's shaders with the app's list: /data/user/0/dev.nxnative.android/files/pack/d8f39dfec5d562c3/repo/dm/games/0400A3A01EE4C000/shader_map.tsv";
        String text = PrepareText.failedText("Translating shaders", old, false);
        plain(text, "an older job's card");
        check(text.equals("Preparing stopped while translating shaders. Tap Retry. Details says why."), "older job: " + text);
        check(PrepareText.failedDetail(old, "", false).equals(old), "older job: its error in Details");
        check(PrepareText.failedText("Translating shaders", "Some text.", true).equals("Some text.") && PrepareText.failedDetail("Some text.", "d", true).equals("d"), "a job of this version");

        // The progress line never says "0 of 0"
        check(PrepareText.progressLine(18, false, 0, 0, "", 0).equals("18 %"), "no total: no shader count: " + PrepareText.progressLine(18, false, 0, 0, "", 0));
        check(PrepareText.progressLine(5, false, 0, 22852, "", 0).equals("5 %  ·  22,852 shaders to make"), PrepareText.progressLine(5, false, 0, 22852, "", 0));
        check(PrepareText.progressLine(37, true, 1304, 22852, "about 35 min left", 70).equals("37 %  ·  21,548 shaders left  ·  about 35 min left  ·  70 unchanged kept"),
                PrepareText.progressLine(37, true, 1304, 22852, "about 35 min left", 70));
        check(PrepareText.progressLine(40, false, 1234, 22852, "about 1 min left", 0).equals("40 %  ·  1,234 of 22,852 shaders"), "outside the step: no time left");

        // The split's count is of files, not shaders (the card read "Finding the shaders 186,583" while the game has 42,898 shaders); the other steps keep theirs
        check(PrepareText.stepCountText("Finding the shaders", true, 1203, 1890, "").equals("Finding the shaders: 1,203 of 1,890 files"), PrepareText.stepCountText("Finding the shaders", true, 1203, 1890, ""));
        check(PrepareText.stepCountText("Finding the shaders", true, 0, 1, "").equals("Finding the shaders: 0 of 1 file"), "one file");
        check(PrepareText.stepCountText("Finding the shaders", true, 0, 0, "").equals("Finding the shaders"), "no count yet: no 0 of 0");
        check(PrepareText.stepCountText("Finding the shaders", true, 2, 5, "40%").equals("Finding the shaders: 2 of 5 files (40 % of a large file)"), PrepareText.stepCountText("Finding the shaders", true, 2, 5, "40%"));
        check(PrepareText.stepCountText("Rewriting the game code", false, 3, 12, "").equals("Rewriting the game code 3 / 12"), "other steps as before");
        check(PrepareText.stepCountText("Checking the dump", false, 1, 1, "identify").equals("Checking the dump"), "a step of one");

        // Needs preparing says why
        check(PrepareText.needsPreparing(false, false, true).contains("Continue"), "a stopped Prepare continues");
        check(PrepareText.needsPreparing(true, true, false).contains("unfinished"), "unfinished code");
        check(PrepareText.needsPreparing(true, false, false).contains("cannot be read"), "unreadable pack");
        check(PrepareText.needsPreparing(false, false, false).startsWith("No pack yet"), "no pack");

        System.out.println("ok " + n);
    }
}
