package dev.nxnative.android;

import android.view.Surface;

/**
 * libnxlaunch.so (src/main/cpp/nxlaunch.c), which forwards to the C interface of libnxhost.so (dm/host/android/input_android.h, dm/gfx/vk_android/vk_android.h).
 * Loaded only in the game and selftest processes, and only one game's pair per process: {@link #load} before any other call (each game has its own host,
 * libnxhost_&lt;title ID&gt;.so, with its profile compiled in, and its own shim libnxlaunch_&lt;title ID&gt;.so; the JNI names are the same in each).
 * Each call does nothing (false, -1) in a host without that entry point.
 */
final class Native {
    private static String loaded;

    private Native() {}

    /** Loads the host of a game this APK runs (Profile). Throws IllegalStateException with a sentence for the user when it has none, or another game's is loaded */
    static synchronized void load(String titleId) {
        if (titleId.equals(loaded)) return;
        if (loaded != null) throw new IllegalStateException("this process runs the host of " + loaded + ", not " + titleId);
        String why = Profile.missing(titleId, null);
        if (why != null) throw new IllegalStateException(why);
        try {
            System.loadLibrary("nxhost_" + titleId);
            System.loadLibrary("nxlaunch_" + titleId);
        } catch (UnsatisfiedLinkError e) {
            throw new IllegalStateException("The host of " + titleId + " cannot be loaded: " + e.getMessage());
        }
        loaded = titleId;
    }

    /** Starts nxhost_main(args) on a thread with an 8 MB stack after setting env (KEY=VALUE) and sending stdout / stderr to logPath. 0 = started. */
    static native int start(String[] args, String[] env, String logPath, String cwd);

    /** A launcher line in the host's log. */
    static native void note(String line);

    /** The game screen's surface for the host (nxhost_set_window), null when it goes. */
    static native void setSurface(Surface surface);

    /** Whether this host takes input events (nxhost_input_*). */
    static native boolean hasInput();

    /** A gamepad connected (player 0..7, -1 when every player is taken) or gone. */
    static native int deviceAdded(int device, String name, boolean labelled);

    static native void deviceRemoved(int device);

    /** A gamepad / D-pad key; true when it is a pad button (the event is consumed). */
    static native boolean key(int device, int keyCode, boolean down);

    /** One joystick MotionEvent: the axes the device has (MotionEvent.AXIS_*) and their values. True when any is used. */
    static native boolean axes(int device, int[] axes, float[] values);

    /** One pointer: MotionEvent.getActionMasked(), the pointer ID, the position divided by the view's width and height. */
    static native void touch(int action, int pointer, float x, float y);

    /** The view's size in pixels and the game picture's rectangle in it, normalized. */
    static native void touchLayout(float viewW, float viewH, float picX, float picY, float picW, float picH);

    static native void touchControlsEnable(boolean on);

    /** The on-screen controls' layout, six floats per control: kind (0 button, 1 stick, 2 D-pad), buttons (DM_BTN_* bits), stick (0 left, 1 right), x, y (normalized
     *  to the view), r (in units of the view's height). The number the host kept; -1 when this host has no such entry point (no controls are drawn). */
    static native int touchControlsSet(float[] controls);

    /** What each control holds now (DM_BTN_* bits) and the sticks (lx, ly, rx, ry; -1..1, y up). */
    static native void touchControlsState(long[] pressed, float[] stick);

    static native boolean motionStart();

    static native void motionStop();

    /** Display.getRotation(). */
    static native void motionRotation(int rotation);

    /** 0 onPause, 1 onStop, 2 onStart, 3 onResume, 4 pause (the game suspended while its screen shows), 5 continue (input_android.h). */
    static native void lifecycle(int event);

    /** Whether the host is paused (4 until 5; nxhost_paused): 1 or 0, -1 when this host has no such entry point. */
    static native int paused();

    static native void audioInterrupt(boolean on);

    static native void audioRouteChanged();

    /** The overlay's frame rate (nxhost_fps): out[0] game frames a second, out[1] images presented a second, out[2] the game's frame time in ms, out[3] frame generation
     *  (0 off, 1 generating, 2 on and not generating, -1 unavailable), out[4] images per game frame, out[5] GPU ms a game frame (-1: none). False when stale (no present for 2.5 s) or not in this host. */
    static native boolean fps(float[] out);

    /** The overlay's block for the present to draw into the game's images (nxhost_overlay_set): slot 0 top left (the build tag), 1 top right (the frame rate and the other items);
     *  px a direct buffer with an ARGB_8888 bitmap's pixels (stride bytes a row), null to empty the slot. False: the present does not draw it, the launcher's views show it */
    static native boolean overlaySet(int slot, java.nio.ByteBuffer px, int w, int h, int stride);

    /** 1 = the present has drawn the overlay (the launcher's views hide), 0 = not yet, -1 = the present does not draw it (DM_VK_OVERLAY=0, a failure, or not in this host) */
    static native int overlayState();

    /** Frame generation's settings for the next frames (nxhost_framegen_set): on, images per game frame 2..4, target rate (0 = fixed), flow scale % (0 = auto). */
    static native void frameGenSet(boolean on, int multiplier, int targetRate, int flowPct);

    /** "<state> <reason>": state 0 off, 1 generating, 2 on and not generating, -1 unavailable, -2 not in this host. */
    static native String frameGenStatus();

    /** Where the letterboxed picture sits in the window's free space (nxhost_picture_place), never scaled: 0 = left / top, 0.5 = centred, 1 = right / bottom. */
    static native void picturePlace(float x, float y);

    /** The frame rate cap while the game runs: 0 = the game's own, else 15, 20, 30 or 60, the launcher's choices (Prefs.FPS_CAPS; nxhost_fps_cap_set) */
    static native void fpsCapSet(int fps);

    /** "<cap> <rates> <game fps>": the cap in effect, a bit per cap this game can hold (the host's bits: 15, 20, 30, 40, 45, 60), the game's frame rate now; "-1 0 0" without a cap in the host. rates 0: the game picks its own frame rate */
    static native String fpsCapStatus();

    /** The readiness check's Vulkan facts ("key=value" lines, nxhost_vk_readiness) on the driver env names (DM_VK_DRIVER / DM_VK_HOOKDIR; the other keys are not set);
     *  null when this host has no such entry point. Before start: the driver is opened once per process, and the host's replay gets the same one */
    static native String readiness(String[] env);

    /** Sends stdout / stderr to logPath (and logcat), without starting the host: for the selftest. 0 = done. */
    static native int openLog(String logPath);

    /** Whether this host has nxhost_vk_selftest. */
    static native boolean hasSelftest();

    /** nxhost_vk_selftest(window, frames): blocks for about frames / 60 s. 0 = passed, -1 = not in this host. */
    static native int selftest(Surface surface, int frames);

    /** The player a gamepad drives (0..7), -1 when it has none (yet). */
    static native int devicePlayer(int device);

    /** The gamepad becomes player 1 (PadOrder); player 1's device moves to its old player. 0, or -1 (every player taken, or not in this host). */
    static native int deviceFirst(int device, String name);

    /** Rumble (Rumble.java): the listener's onRumble(player, intensity, sharpness) gets every change of a player's vibration; set once, before start. False without rumble in this host. */
    static native boolean setRumbleListener(Object listener);

    /** Whether a player has a vibrator: the host sends that player's vibration only while it has one. */
    static native void rumbleRoute(int player, boolean on);

    /** Places the replay thread on the phone's fastest cores and the encoder and the game's threads off the small ones (nxlaunch.c); the number placed by this call, -1 when the phone reports no clocks */
    static native int pinThreads();
    /** The frame limit while the game runs (games whose speed follows the measured rate: Prefs.paced); 0 = none */
    static native void fpsLimitSet(int fps);
    /** Donkey Kong Bananza's dynamic resolution range while it runs, % of the window (probe.c nxhost_drs_set); 100, 100 = off (the native size) */
    static native void drsSet(int minPct, int maxPct);
}
