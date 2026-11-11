package dev.nxnative.android;

import android.app.AlertDialog;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * A game's own settings (its Manage sheet › Game settings…): resolution, frame rate cap, GPU driver, GPU timing, language and extra environment lines, stored per title ID (Prefs.gameKey,
 * GpuDriver.choice). Each starts as "Default", which follows the main screen's Settings (global); a value set here wins for this game only. The in-game panel's frame
 * rate cap is this game's too. GameActivity reads them at start.
 */
final class GameSettings {
    private GameSettings() {}

    static void dialog(MainActivity a, Game g) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        AlertDialog[] d = new AlertDialog[1];
        Runnable again = () -> { d[0].dismiss(); dialog(a, g); };
        String id = g.titleId;
        box.addView(Ui.small(a, "For this game only, from its next start. Default follows Settings."));
        Ui.add(box, new Ui.Setting(a, "↩️", "Reset to defaults", v -> new AlertDialog.Builder(a).setTitle("Reset to defaults · " + g.name)
                .setMessage("The game will follow global settings. Extra environment lines are kept.")
                .setPositiveButton("Reset", (x, w) -> { Prefs.resetGame(a, id); again.run(); }).setNegativeButton("Cancel", null).show())
                .value("The game will follow global settings.").chevron(a).view, 8);
        Profile prof = Profile.of(id);
        if (prof != null && prof.hasResolution())   // only where it does something: a profile with window_fn or graphics_scale rows
            Ui.add(box, new Ui.Setting(a, "🖼️", "Game resolution", v -> resolution(a, g, again)).value(windowLabel(a, id)).chevron(a).view, 8);
        if (!Prefs.limited(id) && Prefs.ownRate(a, id)) {   // the host found no limiter it can hold for this game: choices would do nothing (the host's frame limiter holds a limited game)
            Ui.Setting fixed = new Ui.Setting(a, "⏱️", "Frame rate cap", null).value("This game picks its own frame rate");
            fixed.view.setClickable(false);
            fixed.view.setAlpha(0.6f);
            Ui.add(box, fixed.view, 0);
        } else Ui.add(box, new Ui.Setting(a, "⏱️", "Frame rate cap", v -> cap(a, g, again)).value(capLabel(a, id)).chevron(a).view, 0);
        if (Prefs.paced(id)) {   // the game's speed follows its measured frame rate (fps_decouple sead); off: as on the console. The in-game panel changes it live
            Ui.Setting full = new Ui.Setting(a, "🏃", "Full game speed at low frame rates", null).toggle(a, Prefs.decouple(a, id), (b, on) -> Prefs.decouple(a, id, on));
            full.value("The game keeps its normal speed when its frame rate drops or is limited. Off: it slows down with the frame rate, as on the console");
            Ui.add(box, full.view, 0);
        }
        Ui.add(box, new Ui.Setting(a, "📐", "Display", v -> display(a, g, again)).value(Prefs.DISPLAY_NAMES[Prefs.stretch(a, id) ? 1 : 0]).chevron(a).view, 0);
        Ui.add(box, new Ui.Setting(a, "🔍", "Upscaler", v -> upscaler(a, g, again)).value(dflt(GameUpscaler.ownMode(a, id), GameUpscaler.MODES[GameUpscaler.mode(a, id)])).chevron(a).view, 0);
        if (GameUpscaler.mode(a, id) == GameUpscaler.MODE_FSR)   // only with FSR 1 chosen
            Ui.add(box, new Ui.Setting(a, "✨", "FSR sharpness", v -> sharpness(a, g, again)).value(dflt(GameUpscaler.ownSharp(a, id), GameUpscaler.sharpName(a, id))).chevron(a).view, 0);
        if (Prefs.drsGame(id))
            Ui.add(box, new Ui.Setting(a, "📐", "Dynamic resolution", v -> drs(a, g, again)).value(dflt(Prefs.ownDrs(a, id), Prefs.DRS_NAMES[Prefs.drs(a, id)])).chevron(a).view, 0);
        Ui.add(box, new Ui.Setting(a, "🎮", "GPU driver", v -> driver(a, g, again)).value(driverLabel(a, id)).chevron(a).view, 0);
        Ui.add(box, new Ui.Setting(a, "⏲️", "GPU timing", v -> gpuTiming(a, g, again)).value(gpuTsLabel(a, id)).chevron(a).view, 0);
        Ui.add(box, new Ui.Setting(a, "🌐", "Game language", v -> language(a, g, again)).value(GameLanguage.label(Prefs.gameLang(a, id), phoneTag())).chevron(a).view, 0);
        // Speed-ups that pay off most on Qualcomm's driver (fewer draws for the GPU, less CPU a render pass); off by default (with either on, a tester lost the GPU on Adreno 830
        // with Turnip in 1-2 of 5 starts: open), each one can be turned on alone
        Ui.Setting inst = new Ui.Setting(a, "🧩", "Instanced draws", null).toggle(a, Prefs.instancedDraws(a, id), (b, on) -> Prefs.instancedDraws(a, id, on));
        inst.value("Draws that repeat one mesh become one instanced draw (fewer draws for the GPU)");
        Ui.add(box, inst.view, 0);
        Ui.Setting crp = new Ui.Setting(a, "🧱", "Classic render passes", null).toggle(a, Prefs.classicPasses(a, id), (b, on) -> Prefs.classicPasses(a, id, on));
        crp.value("Render passes instead of dynamic rendering (less CPU a pass on Qualcomm's driver)");
        Ui.add(box, crp.view, 0);
        Ui.Setting pso = new Ui.Setting(a, "🧵", "Background pipelines", null).toggle(a, Prefs.psoAsync(a, id), (b, on) -> Prefs.psoAsync(a, id, on));
        pso.value("New pipelines are made in the background: a short pop-in instead of a stutter the first time something is drawn");
        Ui.add(box, pso.view, 0);
        Ui.Setting ram = new Ui.Setting(a, "💾", "RAM saver (uses storage)", null).toggle(a, Prefs.ramSaver(a, id), (b, on) -> Prefs.ramSaver(a, id, on));
        ram.value("Keeps the game's large texture data in a file instead of compressed RAM: more free RAM where the phone runs short, but each area load writes "
                + "that data to storage (up to a few GB) and needs that much free space. Off: as before");
        Ui.add(box, ram.view, 0);
        Ui.Setting ret = new Ui.Setting(a, "⏸️", "Pause on return", null).toggle(a, Prefs.pauseOnReturn(a, id), (b, on) -> Prefs.pauseOnReturn(a, id, on));
        ret.value("Coming back from the background, the game waits paused for a tap");
        Ui.add(box, ret.view, 0);
        Ui.Setting rum = new Ui.Setting(a, "📳", "Rumble", null).toggle(a, Prefs.rumble(a, id), (b, on) -> Prefs.rumble(a, id, on));
        rum.value("On the controller, or the phone without one");
        Ui.add(box, rum.view, 0);
        Ui.Setting fast = new Ui.Setting(a, "🚀", "Use fast cores", null).toggle(a, Prefs.pinThreads(a, id), (b, on) -> Prefs.pinThreads(a, id, on));
        fast.value("Keeps the busiest threads off the small cores");
        Ui.add(box, fast.view, 0);
        Ui.add(box, new Ui.Setting(a, "🅰️", "Xbox button label swap", v -> xbox(a, g, again))
                .value((Prefs.ownXboxButtons(a, id) ? "" : "Default · ") + Prefs.xboxLabel(Prefs.xboxButtons(a, id))).chevron(a).view, 0);
        Ui.add(box, new Ui.Setting(a, "🕹️", "Button mapping", v -> PadMapUi.forGame(a, g, again)).value(PadMapUi.label(a, id)).chevron(a).view, 0);
        String env = Prefs.gameEnv(a, id);
        Ui.add(box, new Ui.Setting(a, "🧪", "Extra environment", v -> env(a, g, again))
                .value(env.isEmpty() ? "None (KEY=VALUE lines for the host, after env.txt)" : env.split("\n").length + " line(s): " + env.replace('\n', ' ')).chevron(a).view, 0);
        d[0] = Ui.sheet(a, "Game settings · " + g.name, box).setPositiveButton("Done", null).show();
    }

    static String windowLabel(MainActivity a, String id) {
        String w = Prefs.gameWindow(a, id);
        String l = Prefs.windowName(w, false);
        return Prefs.ownWindow(a, id) ? l : "Default · " + l;
    }

    static String capLabel(MainActivity a, String id) {
        String l = Prefs.limited(id) ? Prefs.paceLabel(Prefs.paceCap(a, id)) : MainActivity.capLabel(Prefs.fpsCap(a, id));
        return Prefs.ownFpsCap(a, id) ? l : "Default · " + l;
    }

    static String driverLabel(MainActivity a, String id) {
        GpuDriver.Info i = GpuDriver.selected(a, id);
        String l = i == null ? "System (the phone's own driver)" : i.label();
        return GpuDriver.choice(a, id) != null ? l : "Default · " + l;
    }

    static final String[] GPU_TS = {"segment", "submission"};

    private static String gpuTsName(String mode) { return mode.equals("segment") ? "Per segment" : "Per submission"; }

    static String gpuTsLabel(MainActivity a, String id) {
        String m = Prefs.gpuTs(a, id);
        Profile p = Profile.of(id);
        return m != null ? gpuTsName(m) : "Game default · " + gpuTsName(p == null ? "submission" : p.gpuTs);
    }

    /**
     * GPU timing: what the game's GPU timestamps measure (DM_NVN_GPU_TS; game.tsv gpu_timestamps is the default). Per segment: each timestamp gets the GPU time of
     * its place in the submission, so a game's dynamic resolution and frame pacing see the real GPU time; per submission: every timestamp of a submission gets
     * its completion time (the older behaviour)
     */
    private static void gpuTiming(MainActivity a, Game g, Runnable done) {
        Profile p = Profile.of(g.titleId);
        String[] labels = {"Game default (" + gpuTsName(p == null ? "submission" : p.gpuTs) + ")", "Per segment", "Per submission"};
        String m = Prefs.gpuTs(a, g.titleId);
        int checked = m == null ? 0 : 1 + Math.max(0, Arrays.asList(GPU_TS).indexOf(m));
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        AlertDialog[] d = new AlertDialog[1];
        box.addView(Ui.small(a, "Per segment: the game's timestamps see the real GPU time, so its dynamic resolution can adapt. Per submission: one time for the whole "
                + "submission (the older behaviour). A DM_NVN_GPU_TS line in env.txt wins."));
        android.widget.RadioGroup rg = new android.widget.RadioGroup(a);
        for (int k = 0; k < labels.length; k++) {
            android.widget.RadioButton rb = new android.widget.RadioButton(a);
            rb.setText(labels[k]);
            rb.setId(k + 1);
            rg.addView(rb);
        }
        rg.check(checked + 1);
        rg.setOnCheckedChangeListener((x, w) -> {
            Prefs.gpuTs(a, g.titleId, w <= 1 ? null : GPU_TS[w - 2]);
            d[0].dismiss();
            done.run();
        });
        Ui.add(box, rg, 8);
        d[0] = Ui.sheet(a, "GPU timing · " + g.name, box).setNegativeButton("Cancel", null).show();
    }

    static String phoneTag() { return java.util.Locale.getDefault().toLanguageTag(); }

    /** Game language (DM_LANG, GameLanguage): the phone's language first (the default), then the console's languages; a game without the one picked uses one it has */
    static void language(android.app.Activity a, Game g, Runnable done) {   // the in-game panel's shortcut too
        String[] labels = new String[GameLanguage.CODES.length + 1];
        labels[0] = GameLanguage.label(null, phoneTag());
        System.arraycopy(GameLanguage.NAMES, 0, labels, 1, GameLanguage.NAMES.length);
        String m = Prefs.gameLang(a, g.titleId);
        int checked = m == null ? 0 : 1 + Math.max(0, Arrays.asList(GameLanguage.CODES).indexOf(m));
        new AlertDialog.Builder(a).setTitle("Game language · " + g.name).setSingleChoiceItems(labels, checked, (x, w) -> {
            Prefs.gameLang(a, g.titleId, w == 0 ? null : GameLanguage.CODES[w - 1]);
            x.dismiss();
            done.run();
        }).setNegativeButton("Cancel", null).show();
    }

    private static void resolution(MainActivity a, Game g, Runnable done) {
        String[] labels = new String[Prefs.WINDOWS.length + 1];
        labels[0] = "Default (" + Prefs.windowName(Prefs.defaultWindow(a, g.titleId), false) + ")";
        { Profile lp = Profile.of(g.titleId); for (int li = 0; li < Prefs.WINDOWS.length; li++) labels[li + 1] = lp != null ? lp.windowLabel(Prefs.WINDOWS[li], Prefs.WINDOW_NAMES[li]) : Prefs.WINDOW_NAMES[li]; }   // a choice the game draws at another size says so (Profile.windowLabel)
        int checked = Prefs.ownWindow(a, g.titleId) ? 1 + Math.max(0, Arrays.asList(Prefs.WINDOWS).indexOf(Prefs.gameWindow(a, g.titleId))) : 0;
        new AlertDialog.Builder(a).setTitle("Game resolution · " + g.name).setSingleChoiceItems(labels, checked, (x, w) -> {
            Prefs.gameWindow(a, g.titleId, w == 0 ? null : Prefs.WINDOWS[w - 1]);
            x.dismiss();
            done.run();
        }).setNegativeButton("Cancel", null).show();
    }

    /** Display: Fit keeps the game's 16:9 picture with bars on a screen of another shape; Stretch fills the screen (DM_VK_STRETCH), the touch mapping with it */
    private static void display(MainActivity a, Game g, Runnable done) {
        new AlertDialog.Builder(a).setTitle("Display · " + g.name).setSingleChoiceItems(Prefs.DISPLAY_NAMES, Prefs.stretch(a, g.titleId) ? 1 : 0, (x, w) -> {
            Prefs.stretch(a, g.titleId, w == 1);
            x.dismiss();
            done.run();
        }).setNegativeButton("Cancel", null).show();
    }

    private static void xbox(MainActivity a, Game g, Runnable done) {   // Default first: the game follows Settings again
        String[] labels = new String[Prefs.XBOX_NAMES.length + 1];
        labels[0] = "Default (Settings: " + Prefs.xboxLabel(Prefs.xboxButtons(a)) + ")";
        System.arraycopy(Prefs.XBOX_NAMES, 0, labels, 1, Prefs.XBOX_NAMES.length);
        int checked = Prefs.ownXboxButtons(a, g.titleId) ? Prefs.xboxButtons(a, g.titleId) + 1 : 0;
        new AlertDialog.Builder(a).setTitle("Xbox button label swap · " + g.name).setSingleChoiceItems(labels, checked, (x, w) -> {
            Prefs.xboxButtons(a, g.titleId, w == 0 ? null : w - 1);
            x.dismiss();
            done.run();
        }).setNegativeButton("Cancel", null).show();
    }

    private static void cap(MainActivity a, Game g, Runnable done) {
        if (Prefs.limited(g.titleId)) {   // a frame limit, no locking (Prefs.PACE_CAPS)
            String[] labels = new String[Prefs.PACE_CAPS.length + 1];
            int pl = Prefs.profileLimit(g.titleId);   // the profile's own default (fps_limit), else the main screen's
            labels[0] = pl > 0 ? "Default (this game's: " + Prefs.paceLabel(Prefs.paceOf(pl)) + ")" : "Default (Settings: " + Prefs.paceLabel(Prefs.paceOf(Prefs.fpsCap(a))) + ")";
            int checked = 0, cur = Prefs.paceCap(a, g.titleId);
            for (int k = 0; k < Prefs.PACE_CAPS.length; k++) { labels[k + 1] = Prefs.paceLabel(Prefs.PACE_CAPS[k]); if (Prefs.ownFpsCap(a, g.titleId) && Prefs.PACE_CAPS[k] == cur) checked = k + 1; }
            new AlertDialog.Builder(a).setTitle("Frame rate cap · " + g.name).setSingleChoiceItems(labels, checked, (x, w) -> {
                Prefs.fpsCap(a, g.titleId, w == 0 ? null : Prefs.PACE_CAPS[w - 1]);
                x.dismiss(); done.run();
            }).setNegativeButton("Cancel", null).show();
            return;
        }
        int n = Prefs.FPS_CAPS.length;
        String[] labels = new String[n + 2];
        labels[0] = "Default (Settings: " + MainActivity.capLabel(Prefs.fpsCap(a)) + ")";
        labels[1] = MainActivity.capLabel(-1);
        int checked = 0;
        boolean own = Prefs.ownFpsCap(a, g.titleId);
        int cur = Prefs.fpsCap(a, g.titleId);
        if (own && cur < 0) checked = 1;
        for (int k = 0; k < n; k++) {
            labels[k + 2] = MainActivity.capLabel(Prefs.FPS_CAPS[k]);
            if (own && Prefs.FPS_CAPS[k] == cur) checked = k + 2;
        }
        new AlertDialog.Builder(a).setTitle("Frame rate cap · " + g.name).setSingleChoiceItems(labels, checked, (x, w) -> {
            Prefs.fpsCap(a, g.titleId, w == 0 ? null : w == 1 ? -1 : Prefs.FPS_CAPS[w - 2]);
            x.dismiss();
            done.run();
        }).setNegativeButton("Cancel", null).show();
    }

    private static void driver(MainActivity a, Game g, Runnable done) {
        List<GpuDriver.Info> inst = GpuDriver.installed(a);
        GpuDriver.Info def = GpuDriver.selected(a);
        List<String> labels = new ArrayList<>();
        labels.add("Default (the main screen's: " + (def == null ? "System" : def.label()) + ")");
        labels.add("System (the phone's own driver)");
        String c = GpuDriver.choice(a, g.titleId);
        int checked = c == null ? 0 : c.equals(GpuDriver.SYSTEM) ? 1 : 0;
        for (int k = 0; k < inst.size(); k++) {
            labels.add(inst.get(k).label());
            if (c != null && inst.get(k).dir.getName().equals(c)) checked = k + 2;
        }
        new AlertDialog.Builder(a).setTitle("GPU driver · " + g.name).setSingleChoiceItems(labels.toArray(new String[0]), checked, (x, w) -> {
            GpuDriver.choice(a, g.titleId, w == 0 ? null : w == 1 ? GpuDriver.SYSTEM : inst.get(w - 2).dir.getName());
            x.dismiss();
            done.run();
        }).setNegativeButton("Cancel", null).show();
    }

    private static void env(MainActivity a, Game g, Runnable done) {
        EditText t = new EditText(a);
        t.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        t.setMinLines(4);
        t.setTypeface(android.graphics.Typeface.MONOSPACE);
        t.setTextSize(13);
        t.setHint("DM_WATCH_STALL=25");
        t.setText(Prefs.gameEnv(a, g.titleId));
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.addView(Ui.small(a, "KEY=VALUE lines for the host, one a line (# starts a comment). They come after env.txt and win over it, for " + g.name + " only."));
        Ui.add(box, t, 8);
        Ui.sheet(a, "Extra environment · " + g.name, box).setPositiveButton("Save", (x, w) -> { Prefs.gameEnv(a, g.titleId, t.getText().toString()); done.run(); })
                .setNeutralButton("Clear", (x, w) -> { Prefs.gameEnv(a, g.titleId, null); done.run(); }).setNegativeButton("Cancel", null).show();
    }

    static String dflt(boolean own, String v) { return own ? v : "Default · " + v; }

    /** A choice with "Default (Settings: ...)" first: picking it removes the game's own value (set(null)) */
    private static void choose(MainActivity a, String title, String[] items, String settingsValue, boolean own, int cur, java.util.function.Consumer<Integer> set, Runnable done) {
        String[] labels = new String[items.length + 1];
        labels[0] = "Default (Settings: " + settingsValue + ")";
        System.arraycopy(items, 0, labels, 1, items.length);
        new AlertDialog.Builder(a).setTitle(title).setSingleChoiceItems(labels, own ? cur + 1 : 0, (x, w) -> { set.accept(w == 0 ? null : w - 1); x.dismiss(); done.run(); })
                .setNegativeButton("Cancel", null).show();
    }

    private static void upscaler(MainActivity a, Game g, Runnable done) {   // the Upscaler (GameUpscaler: DM_VK_NDSR and the present's scaling); the in-game panel changes it live
        choose(a, "Upscaler · " + g.name, GameUpscaler.MODES, GameUpscaler.MODES[GameUpscaler.mode(a)], GameUpscaler.ownMode(a, g.titleId), GameUpscaler.mode(a, g.titleId),
               v -> GameUpscaler.mode(a, g.titleId, v), done);
    }

    private static void sharpness(MainActivity a, Game g, Runnable done) {   // FSR's RCAS sharpening (GameUpscaler, DM_VK_NDSR_SHARP)
        choose(a, "FSR sharpness · " + g.name, GameUpscaler.SHARP_NAMES, GameUpscaler.sharpName(GameUpscaler.sharp(a)), GameUpscaler.ownSharp(a, g.titleId),
               GameUpscaler.sharpIndex(GameUpscaler.sharp(a, g.titleId)), v -> GameUpscaler.sharp(a, g.titleId, v == null ? null : GameUpscaler.SHARPS[v]), done);
    }

    private static void drs(MainActivity a, Game g, Runnable done) {   // the render size's range (Prefs.DRS), from the next start; the in-game panel changes it live
        DrsSlider.dialog(a, " · " + g.name, Prefs.drs(a, g.titleId), Prefs.DRS_NAMES[Prefs.drs(a)], Prefs.ownDrs(a, g.titleId), v -> Prefs.drs(a, g.titleId, v), done);
    }
}
