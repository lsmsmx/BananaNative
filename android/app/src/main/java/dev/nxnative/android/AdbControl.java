package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.function.Predicate;

/**
 * The adb control commands of the launcher (android/TESTING.md): the extras of an intent that starts MainActivity and add a game, start Prepare, install or pick a
 * GPU driver, or export or import a pack. MainActivity is exported, as every launcher activity is, so any app on the phone may start it with them. A test build
 * takes them; a play or release build only while Settings › ADB control (debug) is on (Prefs.adbControl, off by default). Starting a game from a frontend
 * (the .Launch and .Open aliases, FrontendLaunch) is not a control command and is unchanged.
 * Ported from Anka Native 0.2.2 (EddizEge, MIT; its issue #7). No Android types: android/app/src/jvmtest's AdbControlJvmTest runs it on the JVM.
 */
final class AdbControl {
    private AdbControl() {}

    /** Every extra a control command reads: MainActivity.fromIntent and start, Ondevice, GpuDriver and PackUi (dm/tools/tests/test_android_adbcontrol.py checks the list
     *  against the sources). frontend_game is not one: FrontendLaunch sends it, and it does only what the exported .Launch alias already does for any app */
    static final List<String> EXTRAS = Collections.unmodifiableList(java.util.Arrays.asList(
            "game", "add_game", "pack", "save", "run", "no_pause",
            "ondevice_dump", "ondevice_start", "ondevice_pause", "ondevice_go_on", "ondevice_pack", "show_when_locked",
            "gpu_driver_zip", "gpu_driver",
            "pack_title", "pack_game", "pack_export", "pack_import"));

    /** Whether control commands are taken: always in a test build, in a play or release build only with the setting on */
    static boolean allowed(boolean playBuild, boolean settingOn) { return !playBuild || settingOn; }

    /** The control extras an intent carries (has: Intent::hasExtra), in EXTRAS' order; empty when it carries none */
    static List<String> asked(Predicate<String> has) {
        List<String> out = new ArrayList<>();
        for (String k : EXTRAS) if (has.test(k)) out.add(k);
        return out;
    }
}
