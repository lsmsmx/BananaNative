package dev.nxnative.android;

import android.content.Context;
import android.content.SharedPreferences;

/** The launcher's own settings (the on-screen controls keep theirs in TouchControls): the overlay over the game, frame generation. */
final class Prefs {
    private Prefs() {}

    /**
     * A settings file, read again whenever another process has written it. The launcher and the game (GameActivity, in its own process :game) each keep the file in memory:
     * with MODE_PRIVATE a change made in the game's panel stayed unseen by the launcher, whose next write put its old copy back over it, so in-game settings came back
     * as they were. MODE_MULTI_PROCESS reloads the file on each getSharedPreferences when it changed on disk (deprecated, still honoured), so every access goes through
     * here, and writes use commit(): written before the call returns, so Stop (which ends the game's process) cannot drop one still queued by apply().
     */
    @SuppressWarnings("deprecation")
    static SharedPreferences open(Context c, String name) { return c.getSharedPreferences(name, Context.MODE_MULTI_PROCESS); }
    static SharedPreferences of(Context c) { return open(c, "launcher"); }

    /** The overlay over the game (master switch: the frame rate and the other items top right, Overlay; the build tag top left is always shown); on by default. DM_OVERLAY=0 / 1 in env.txt overrides it for a run. */
    static boolean overlay(Context c) { return of(c).getBoolean("overlay", true); }
    static void overlay(Context c, boolean on) { of(c).edit().putBoolean("overlay", on).commit(); }

    /** One item of the overlay (Overlay.ITEMS); all on by default, shown only while the overlay is on */
    static boolean overlayItem(Context c, String key) { return of(c).getBoolean("overlay_" + key, true); }
    static void overlayItem(Context c, String key, boolean on) { of(c).edit().putBoolean("overlay_" + key, on).commit(); }

    /** The game's window size (Game resolution): DM_WINDOW_SIZE for the host (loader.c window_apply), "1920x1080" (Phone 1080p, the default), "original" (the game's own, 2880x1620 in Donkey Kong Bananza), "1280x720" (Phone 720p: less GPU time a frame), "2560x1440" (Phone 1440p: for QHD screens), "960x540" (Phone 540p: for weaker GPUs) or "848x480" (Phone 480p). DM_WINDOW_SIZE in env.txt overrides it for a run.
     *  A game whose profile has graphics_scale rows gets the graphics scale and operation mode that give the size instead (Profile.resolutionEnv).
     *  Listed from the largest. The stored value is the size itself, so the order can change; the default is WINDOW_DEFAULT. */
    static final String[] WINDOWS = {"original", "2560x1440", "1920x1080", "1280x720", "960x540", "848x480"},
            WINDOW_NAMES = {"Original (the game's own size)", "Phone 1440p (2560x1440)", "Phone 1080p (1920x1080)", "Phone 720p (1280x720)", "Phone 540p (960x540)", "Phone 480p (848x480)"},
            WINDOW_SHORT = {"Original", "1440p", "1080p", "720p", "540p", "480p"};
    static final String WINDOW_DEFAULT = "1920x1080";   // Phone 1080p
    /** A window size's name in the lists, its short name for a settings row (short); the value itself when it is none of WINDOWS (an env.txt-style size) */
    static String windowName(String v, boolean shortName) {
        int i = java.util.Arrays.asList(WINDOWS).indexOf(v);
        return i < 0 ? v : (shortName ? WINDOW_SHORT : WINDOW_NAMES)[i];
    }
    static String window(Context c) { return of(c).getString("window", WINDOW_DEFAULT); }
    static void window(Context c, String v) { of(c).edit().putString("window", v).commit(); }

    // ---- A game's own settings (its Manage sheet › Game settings, the in-game panel), stored per title ID as "<key>@<title ID>"; a game without its own value
    // takes the default (the main screen's Settings, global). DKB with nothing of its own set runs exactly as before ----
    static String gameKey(String key, String titleId) { return key + "@" + titleId; }
    /** Rumble (Rumble.java): the main screen's default (on), then each game's own */
    static boolean rumble(Context c) { return of(c).getBoolean("rumble", true); }
    static void rumbleDefault(Context c, boolean on) { of(c).edit().putBoolean("rumble", on).commit(); }
    static boolean rumble(Context c, String titleId) { return of(c).getBoolean(gameKey("rumble", titleId), rumble(c)); }
    static void rumble(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("rumble", titleId), on).commit(); }
    /** Use fast cores (Native.pinThreads): the main screen's default (off: not measured yet, and it may work against the ADPF hint session of vka_perf.c), then each game's own */
    static boolean pinThreads(Context c) { return of(c).getBoolean("pin_threads", false); }
    static void pinThreadsDefault(Context c, boolean on) { of(c).edit().putBoolean("pin_threads", on).commit(); }
    static boolean pinThreads(Context c, String titleId) { return of(c).getBoolean(gameKey("pin_threads", titleId), pinThreads(c)); }
    static void pinThreads(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("pin_threads", titleId), on).commit(); }
    /** Xbox button label swap: 0 off; 1 labels (the prompts' A/B and X/Y drawn the Xbox way: DM_XBOX_LABELS, hle_fs.c); 2 labels + confirm with A (A and B by label,
     *  DM_PAD_LABELLED=2, so only X/Y are redrawn). The main screen's default (off), then each game's own */
    static final String[] XBOX_NAMES = {"Off", "Swap labels for X/Y and A/B", "Swap labels for X/Y, swap buttons A/B"};
    static int xboxButtons(Context c) { int v = of(c).getInt("xbox_buttons", 0); return v >= 0 && v < XBOX_NAMES.length ? v : 0; }
    static void xboxButtonsDefault(Context c, int v) { of(c).edit().putInt("xbox_buttons", v).commit(); }
    static int xboxButtons(Context c, String titleId) { int v = of(c).getInt(gameKey("xbox_buttons", titleId), xboxButtons(c)); return v >= 0 && v < XBOX_NAMES.length ? v : 0; }
    static boolean ownXboxButtons(Context c, String titleId) { return of(c).contains(gameKey("xbox_buttons", titleId)); }
    /** null: back to Default (follows Settings) */
    static void xboxButtons(Context c, String titleId, Integer v) {
        if (v == null) of(c).edit().remove(gameKey("xbox_buttons", titleId)).commit(); else of(c).edit().putInt(gameKey("xbox_buttons", titleId), v).commit();
    }
    static String xboxLabel(int v) { return XBOX_NAMES[v]; }
    /** The in-game panel's Player 1 (PadOrder): the chosen gamepad's InputDevice descriptor, for every game; "" = automatic */
    static String padFirst(Context c) { return of(c).getString("pad_first", ""); }
    static void padFirst(Context c, String key) { of(c).edit().putString("pad_first", key == null ? "" : key).commit(); }

    /** Button mapping (PadMap, DM_PAD_MAP): Settings' default ("" = none: the host's default), then each game's own (unset = Settings'; "" = none for this game) */
    static String padMap(Context c) { return of(c).getString("pad_map", ""); }
    static void padMapDefault(Context c, String spec) { of(c).edit().putString("pad_map", spec == null ? "" : spec).commit(); }
    static String padMap(Context c, String titleId) { return of(c).getString(gameKey("pad_map", titleId), padMap(c)); }
    static boolean ownPadMap(Context c, String titleId) { return of(c).contains(gameKey("pad_map", titleId)); }
    /** null: back to Default (follows Settings) */
    static void padMap(Context c, String titleId, String spec) { put(c, gameKey("pad_map", titleId), spec); }

    /** Game settings › Display (per game): false = Fit (keep aspect, letterboxed; the default), true = Stretch to screen (DM_VK_STRETCH=1: the picture fills the display) */
    static final String[] DISPLAY_NAMES = {"Fit (keep aspect)", "Stretch to screen"};
    static boolean stretch(Context c, String titleId) { return of(c).getBoolean(gameKey("stretch", titleId), false); }
    static void stretch(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("stretch", titleId), on).commit(); }
    /** The in-game panel's Picture size (per game): the picture's share of its fitted or stretched size, PictureFit.SIZE_MIN..100 % (DM_VK_PICTURE_SIZE; 100, the default, is not passed) */
    static int pictureSize(Context c, String titleId) { return PictureFit.size(of(c).getInt(gameKey("picture_size", titleId), PictureFit.SIZE_MAX)); }
    static void pictureSize(Context c, String titleId, int pct) { of(c).edit().putInt(gameKey("picture_size", titleId), PictureFit.size(pct)).commit(); }

    /** The in-game panel's own size and text size (PanelLook: 0 Small, 1 Normal, 2 Large), for every game */
    static int panelSize(Context c) { return PanelLook.of(of(c).getInt("panel_size", PanelLook.NORMAL)); }
    static void panelSize(Context c, int v) { of(c).edit().putInt("panel_size", PanelLook.of(v)).commit(); }
    static int panelText(Context c) { return PanelLook.of(of(c).getInt("panel_text", PanelLook.NORMAL)); }
    static void panelText(Context c, int v) { of(c).edit().putInt("panel_text", PanelLook.of(v)).commit(); }
    /** Games whose speed follows the measured frame rate (their profile has fps_decouple rows: the host's sead_decouple) get a frame limit instead of 60 / 30 / 20 locking.
     *  The limit is one of PACE_CAPS: 0 = 60 fps (the game's fastest rate, where the measured rate is clamped), or at most that many frames a second (paceOf) */
    static final int[] PACE_CAPS = {0, 45, 40, 30};
    static boolean paced(String titleId) { Profile p = Profile.of(titleId); return p != null && p.decoupled; }
    static int paceOf(int v) { return v <= 0 || v >= 60 ? 0 : v >= 45 ? 45 : v >= 40 ? 40 : 30; }
    static String paceLabel(int v) { return (v <= 0 ? 60 : v) + " fps"; }   // 0 is 60: the game's own fastest rate, where the decoupling's rate is clamped
    /** A paced game's cap as saved, without capOf (which turns 45 and 40 into 30 for the other games): its own, else the default, as one of PACE_CAPS */
    static int paceCap(Context c, String titleId) { return paceOf(of(c).getInt(gameKey("fps_cap", titleId), of(c).getInt("fps_cap", -1))); }
    /** Game settings › Reset to defaults: every choice of the game's own back to the main screen's default (Extra environment's lines stay) */
    static final String[] GAME_SETTINGS = {"window", "fps_cap", "gpu_driver", "gpu_ts", "pause_return", "ndsr_choice2", "ndsr_sharp", "drs2", "ram_saver"};
    static void resetGame(Context c, String titleId) {
        SharedPreferences.Editor e = of(c).edit();
        for (String k : GAME_SETTINGS) e.remove(gameKey(k, titleId));
        e.commit();
    }

    /** Dynamic resolution, for games whose profile has drs_ctl rows (the host steers them: probe.c drs_install): the range the render size moves in, as % of the window,
     *  always up to 100 %; {100, 100} is off: the full size and no upscaling. Stored as an index into DRS, the main screen's default then each game's own */
    static boolean drsGame(String titleId) { Profile p = Profile.of(titleId); return p != null && p.drs; }
    static final int[][] DRS = {{100, 100}, {80, 100}, {70, 100}, {60, 100}, {50, 100}, {40, 100}, {30, 100}};   // 30 %: the game's rate table stops at 3x (640x360 at 1080p)
    static final String[] DRS_NAMES = {"Off", "80%", "70%", "60%", "50%", "40%", "30%"};   // the lowest render size, as % of the window (always up to 100 %)
    static final int DRS_DEFAULT = 5;   // 40-100 %
    static int drs(Context c) { int v = of(c).getInt("drs2", DRS_DEFAULT); return v >= 0 && v < DRS.length ? v : DRS_DEFAULT; }
    static void drsDefault(Context c, int v) { of(c).edit().putInt("drs2", v).commit(); }
    static boolean ownDrs(Context c, String titleId) { return of(c).contains(gameKey("drs2", titleId)); }
    static int drs(Context c, String titleId) { int v = of(c).getInt(gameKey("drs2", titleId), drs(c)); return v >= 0 && v < DRS.length ? v : drs(c); }
    /** null removes the game's own choice */
    static void drs(Context c, String titleId, Integer v) {
        if (v == null) of(c).edit().remove(gameKey("drs2", titleId)).commit(); else of(c).edit().putInt(gameKey("drs2", titleId), v).commit();
    }

    /** The game's window size: its own choice, else the default (defaultWindow) */
    static String gameWindow(Context c, String titleId) { return of(c).getString(gameKey("window", titleId), defaultWindow(c, titleId)); }
    /** A game's default window size: the main screen's when the player chose one there, else its profile's android_window (a workaround for one game, game.tsv), else Phone 1080p */
    static String defaultWindow(Context c, String titleId) {
        if (of(c).contains("window")) return window(c);
        Profile p = Profile.of(titleId);
        return p != null && java.util.Arrays.asList(WINDOWS).contains(p.window) ? p.window : WINDOW_DEFAULT;
    }
    /** null: back to the default */
    static void gameWindow(Context c, String titleId, String v) { put(c, gameKey("window", titleId), v); }
    static boolean ownWindow(Context c, String titleId) { return of(c).contains(gameKey("window", titleId)); }

    /**
     * The frame rate cap (the in-game panel): -1 = not set (the game profile's fps_target), 0 = the game's own, else 15, 20, 30 or 60. DM_FPS_TARGET for the host at
     * start, and Native.fpsCapSet while the game runs. A DM_FPS_TARGET line in env.txt overrides it for a run. 40 and 45 are no longer offered (they did not hold):
     * a stored 40 or 45 reads as the next lower cap (capOf), so an older install keeps a working choice.
     */
    static final int[] FPS_CAPS = {0, 60, 30, 20, 15};
    /** A stored cap as one of FPS_CAPS: -1 and 0 as they are, else the highest offered cap at or below it (45 → 30), the game's own when none is */
    static int capOf(int v) {
        if (v <= 0) return v;
        int best = 0;
        for (int k : FPS_CAPS) if (k > 0 && k <= v && k > best) best = k;
        return best;
    }
    static int fpsCap(Context c) { return capOf(of(c).getInt("fps_cap", -1)); }
    static void fpsCap(Context c, int v) { of(c).edit().putInt("fps_cap", v).commit(); }
    /** The game's cap: its own (set in its Game settings or its in-game panel), else the default */
    static int fpsCap(Context c, String titleId) { return capOf(of(c).getInt(gameKey("fps_cap", titleId), fpsCap(c))); }

    /**
     * Whether the host found that this game picks its own frame rate (its cap status reports no rate it can hold: Native.fpsCapStatus rates 0), as the last run
     * saw it: its Game settings and its in-game panel then show no cap choices, which would do nothing. Written by GameActivity and the panel while the game runs.
     */
    static boolean ownRate(Context c, String titleId) { return of(c).getBoolean(gameKey("own_rate", titleId), false); }
    static void ownRate(Context c, String titleId, boolean v) { if (ownRate(c, titleId) != v) of(c).edit().putBoolean(gameKey("own_rate", titleId), v).commit(); }
    /** null: back to the default */
    static void fpsCap(Context c, String titleId, Integer v) {
        if (v == null) of(c).edit().remove(gameKey("fps_cap", titleId)).commit();
        else of(c).edit().putInt(gameKey("fps_cap", titleId), v).commit();
    }
    static boolean ownFpsCap(Context c, String titleId) { return of(c).contains(gameKey("fps_cap", titleId)); }

    /** Pause on return (per game, on by default): the game's screen coming back from the background shows it paused, suspended until a tap (GameActivity.onStart) */
    static boolean pauseOnReturn(Context c, String titleId) { return of(c).getBoolean(gameKey("pause_return", titleId), true); }
    static void pauseOnReturn(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("pause_return", titleId), on).commit(); }
    /** Game settings › Instanced draws (DM_VK_INSTANCE): runs of draws that repeat one mesh drawn as one instanced draw. Off by default (a GPU loss on Adreno 830 with Turnip, open) */
    static boolean instancedDraws(Context c, String titleId) { return of(c).getBoolean(gameKey("vk_instance", titleId), false); }
    static void instancedDraws(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("vk_instance", titleId), on).commit(); }
    /** Game settings › Background pipelines (DM_VK_PSO_ASYNC): a pipeline the cache doesn't have yet is made on two background threads instead of on the frame,
     *  so a cold cache gives a short pop-in instead of a freeze (Bananza's heaviest area with an empty cache: worst frame 8.1 s → 3.7 s). On by default */
    static boolean psoAsync(Context c, String titleId) { return of(c).getBoolean(gameKey("vk_pso_async", titleId), true); }
    static void psoAsync(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("vk_pso_async", titleId), on).commit(); }
    /**
     * RAM saver (per game, off by default; from Anka Native, NOTICE.md): DM_MEM_FILEBACK=1 with DM_MEM_FILEBACK_KINDS=shader,texture for the host (shader-code pools are
     * file-backed for every game by default; this adds the texture kind), so the game's large write-once texture pools live in a file in the app's cache folder instead of anonymous memory: clean file pages the kernel drops instead of compressing them into
     * zram. For phones short of RAM with free storage; it writes the pools' data to storage at each area load. Lines in env.txt or the game's own lines win
     */
    static boolean ramSaver(Context c, String titleId) { return of(c).getBoolean(gameKey("ram_saver", titleId), false); }
    static void ramSaver(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("ram_saver", titleId), on).commit(); }
    /** Game settings › Classic render passes (DM_VK_CLASSIC_RP): VkRenderPasses with imageless framebuffers instead of dynamic rendering. Off by default (a GPU loss on Adreno 830 with Turnip, open) */
    static boolean classicPasses(Context c, String titleId) { return of(c).getBoolean(gameKey("vk_classic_rp", titleId), false); }
    static void classicPasses(Context c, String titleId, boolean on) { of(c).edit().putBoolean(gameKey("vk_classic_rp", titleId), on).commit(); }

    /**
     * The game's own environment lines (KEY=VALUE, one a line; its Game settings), given to the host after the launcher's settings and env.txt, so they win for
     * this game; env-&lt;title ID&gt;.txt in the app's folder (adb) comes after them. "" = none
     */
    /** GPU timing (Game settings): "segment" or "submission" passed to the host as DM_NVN_GPU_TS, null for the game's default (its profile's gpu_timestamps) */
    static String gpuTs(Context c, String titleId) { return of(c).getString(gameKey("gpu_ts", titleId), null); }
    static void gpuTs(Context c, String titleId, String mode) { put(c, gameKey("gpu_ts", titleId), mode); }

    /** Whether the launcher takes the adb control commands of the intents that start it (AdbControl: add a game, start Prepare, install or pick a GPU driver, export or
     *  import a pack; android/TESTING.md): a test build always; a play or release build only while Settings › Help › ADB control (debug) is on, off by default */
    static boolean adbControl(Context c) { return AdbControl.allowed(BuildConfig.NX_PLAY, of(c).getBoolean("adb_control", false)); }
    static void adbControl(Context c, boolean on) { of(c).edit().putBoolean("adb_control", on).commit(); }

    /** Keep the screen on while a game is prepared (Settings › System, on by default): Home keeps it on while the job runs and its card shows (Ondevice) */
    static boolean keepScreenPreparing(Context c) { return of(c).getBoolean("keep_screen_preparing", true); }
    static void keepScreenPreparing(Context c, boolean on) { of(c).edit().putBoolean("keep_screen_preparing", on).commit(); }

    /** Game language (Game settings): a console language code (GameLanguage.CODES) passed as DM_LANG, null for the phone's language (the default) */
    static String gameLang(Context c, String titleId) { return of(c).getString(gameKey("lang", titleId), null); }
    static void gameLang(Context c, String titleId, String code) { put(c, gameKey("lang", titleId), code); }

    static String gameEnv(Context c, String titleId) { return of(c).getString(gameKey("env", titleId), ""); }
    static void gameEnv(Context c, String titleId, String text) { put(c, gameKey("env", titleId), text == null || text.trim().isEmpty() ? null : text.trim()); }

    private static void put(Context c, String key, String v) {
        if (v == null) of(c).edit().remove(key).commit();
        else of(c).edit().putString(key, v).commit();
    }

    /**
     * Folders the user picked (main screen, Folders…), as /storage paths; null = the default. Kept in the app's preferences, so an APK update keeps them (an
     * uninstall does not). "pack_<title ID>": the game's pack folder (pack.tsv and shaders/; default the app's packs/<id> when it holds one, else
     * <game>/../<id>-work/android-pack, Pack.find; the older single-game "pack" key is still read when it holds a pack of that title, Folders.packFor);
     * "save_<title ID>": the game's save folder, given to the host as DM_SAVE (default the app's saves/<id>, AppStorage.save).
     */
    static String folder(Context c, String key) { return of(c).getString("folder_" + key, null); }
    static void folder(Context c, String key, String path) { of(c).edit().putString("folder_" + key, path).commit(); }

    /**
     * The default save folder of versions up to 0.1.0, on shared storage: <Switch2 root>/saves/<id> for the play app, saves-test/<id> for the others, the root
     * being the folder that holds the game folder or the .nup. The default is now the app's own saves/<id> (AppStorage.save); a save found here is copied in once
     * (SaveMigrate, GameActivity.saveDir), and this folder is never changed
     */
    static java.io.File oldSaveDefault(java.io.File game, String id) {
        return new java.io.File(game.getAbsoluteFile().getParentFile(), (BuildConfig.NX_PLAY ? "saves/" : "saves-test/") + id);
    }

    /** The old default save folder whose save was copied into app storage (SaveMigrate), null when none was. Written by the game's process (GameActivity) */
    static String migratedFrom(Context c, String titleId) { return of(c).getString(gameKey("save_migrated", titleId), null); }
    static void migratedFrom(Context c, String titleId, String path) { put(c, gameKey("save_migrated", titleId), path); }

    /** Where a game's save backups go (SaveStore): <Switch2 root>/save-backups/<id>, the root being the game folder's parent, for the play and the test app alike
     *  (each backup's manifest says which save folder it came from) */
    static java.io.File backupsDir(java.io.File game, String id) {
        return new java.io.File(game.getAbsoluteFile().getParentFile(), "save-backups/" + id);
    }

    /**
     * Which build a log or a screenshot came from. Play app: "Banana Native 0.1 · 6a6448d" (the version up to its "-", the commit);
     * test app: "Banana Native test · int@6a6448d" (the branch without "android-", the commit). "+dirty" follows the commit when the
     * tree had changes. The play app adds " · official" when the release key signed the APK, else " · unofficial build" (Signer). The main screen shows it and it
     * is the first line of every game log ("[nxlaunch] build ...").
     */
    static String buildTag(Context ctx) { return Signer.tag(buildTag(), BuildConfig.NX_PLAY, signers(ctx)); }

    private static String[] signers;

    /** The SHA-256 of each certificate that signed this APK (its current signers, not past ones of a rotated key), read once; empty when it cannot be read */
    static synchronized String[] signers(Context ctx) {
        if (signers != null) return signers;
        try {
            android.content.pm.SigningInfo si = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES).signingInfo;
            android.content.pm.Signature[] sig = si == null ? new android.content.pm.Signature[0] : si.getApkContentsSigners();
            String[] h = new String[sig.length];
            for (int i = 0; i < sig.length; i++) h[i] = Signer.sha256(sig[i].toByteArray());
            signers = h;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            signers = new String[0];
        }
        return signers;
    }

    /** Which key signed this APK, for logs and reports: the signer's SHA-256, first 16 hex digits */
    static String signerId(Context ctx) { return Signer.shortId(signers(ctx)); }

    /** The build identifier alone (Signer.buildId): "launcher-2@4a15067", "0.1 · 6a6448d". Home shows it under its title (the build tag would not fit a phone's width) */
    static String buildId() {
        return Signer.buildId(BuildConfig.NX_PLAY, BuildConfig.VERSION_NAME, BuildConfig.NX_COMMIT, BuildConfig.NX_DIRTY, BuildConfig.NX_BRANCH);
    }

    private static String buildTag() { return Signer.buildTag(BuildConfig.NX_PLAY, buildId()); }
}
