package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The launcher. Two pages in one activity (so every picker's result comes back here): Home, the short path to playing (pick your dump, prepare it, Play), and Settings
 * (the defaults of every game, GPU driver, folders, the Readiness check, the self-test, logs, the build), opened with the gear in Home's header; Back returns to Home.
 * <p>
 * Home: the All files access card while it is missing, a game being prepared (Ondevice: shown while there is a job), then the games, each a card with its state and
 * one big button: Play, or Prepare when its pack does not fit yet; ⋯ (or a long press) opens Manage (its folders, its own settings, saves, Prepare again, removal),
 * kept away from Play. With no game, a single card says what to do and has Add game. "Add game" takes a dump folder or .nup of any game this APK has a profile for
 * (Profile: one host per game), or a folder that holds several (all added, one summary: addAll): the best pack PackFind finds (the picked pack folder, the app's,
 * beside the game, &lt;id&gt;-work/&lt;any&gt; up to three folders above it) is used when it fits or only needs the automatic shader update, else the app offers to
 * prepare one on the phone (or a card with Prepare, from a folder of games). A game without a profile in this APK is refused with a sentence (Profile.missing),
 * never started.
 * <p>
 * Each page sits in one ScrollView over the drifting bananas, so it scrolls on any screen (the folded phone's outer display, landscape, a split window), at a
 * readable width, centered on a wide window. The activity handles rotation itself (configChanges), so a size change builds the page again (onConfigurationChanged).
 */
public final class MainActivity extends Activity {
    private static final int PICK_GAME = 1, PICK_PACK = 2, PICK_DLL = 3, PICK_DRIVER = 4, PICK_NUP = 5, PICK_NUP_HERE = 6;

    private final List<Game> games = new ArrayList<>();
    private LinearLayout gameBox;   // the game cards (refresh)
    private View accessCard;
    private BananaBackgroundView bananas;   // the drifting bananas behind the screen
    private Ui.Setting overlayRow, resRow, capRow, upsRow, sharpRow, drsRow, lsfgRow, gpuRow, updateRow;
    private boolean resumed, settingsPage;
    private boolean merged;   // the list's duplicates were merged (Game.collapse) since this activity started
    private File pendingGame;   // a game folder (or .nup) waiting for its pack folder
    private String pendingId;
    private File pendingPackPick;   // the pack folder the user just picked (remembered in Prefs once it is accepted)

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) settingsPage = saved.getBoolean("settings");
        FrameLayout frame = new FrameLayout(this);
        bananas = new BananaBackgroundView(this);
        frame.addView(bananas, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(frame);
        build();
        if (saved == null) fromIntent(getIntent());
        Updater.onStart(this);   // the release app: a newer release on GitHub opens the update popup (once per process start: not on a resume or a rotation)
    }

    @Override protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean("settings", settingsPage);
    }

    /** Rotation, a window resize: the same page again */
    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        build();
        if (resumed) { onResumeViews(); if (!settingsPage) Ondevice.resume(this); }
    }

    /** Home ↔ Settings */
    private void page(boolean settings) {
        if (settingsPage == settings) return;
        settingsPage = settings;
        if (settings) Ondevice.pause(this);   // its card is on Home only: no updates while Settings shows
        build();
        if (resumed) { onResumeViews(); if (!settings) Ondevice.resume(this); }
    }

    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() {
        if (settingsPage) page(false);
        else super.onBackPressed();
    }

    /** (Re)builds the current page above the bananas (Ui.page: the same gutter, width and header spacing as every screen) */
    private void build() {
        FrameLayout frame = (FrameLayout) bananas.getParent();
        while (frame.getChildCount() > 1) frame.removeViewAt(1);
        LinearLayout content = Ui.page(this, frame, 640);
        if (settingsPage) buildSettings(content);
        else buildHome(content);
    }

    private void buildHome(LinearLayout content) {
        overlayRow = null;
        content.addView(Ui.header(this, Ui.badge(this, 44), "Banana Native", Prefs.buildId(), Ui.iconButton(this, R.drawable.ic_settings, "Settings", Ui.Kind.TONAL, v -> page(true))));   // the build identifier under the title (Prefs.buildId, the part of the overlay's build tag after the app's name), so a screenshot of Home names its build; Settings › About has the full tag
        accessCard = accessCard();
        Ui.add(content, accessCard, 16);
        Ondevice.attach(this, content, content.getChildCount());   // a game being prepared and its progress (hidden when there is none)
        fixGap(content, Ui.GAP);
        gameBox = new LinearLayout(this);
        gameBox.setOrientation(LinearLayout.VERTICAL);
        Ui.add(content, gameBox, 16);
        refresh();
    }

    /** The Ondevice card is added without a margin: give it the cards' gap */
    private void fixGap(LinearLayout col, int gapDp) {
        for (int k = 1; k < col.getChildCount(); k++) {
            View v = col.getChildAt(k);
            ViewGroup.LayoutParams p = v.getLayoutParams();
            if (p instanceof LinearLayout.LayoutParams && ((LinearLayout.LayoutParams) p).topMargin == 0) ((LinearLayout.LayoutParams) p).topMargin = dp(gapDp);
        }
    }

    private View accessCard() {
        LinearLayout c = Ui.card(this);
        c.setBackground(Ui.round(Ui.col(this, R.color.bn_warn_tint), dp(20), Ui.col(this, R.color.bn_banana_deep), dp(1)));
        c.addView(Ui.heading(this, "Allow file access"));
        Ui.add(c, Ui.body(this, "Needed to read a dump folder where it is. A .nup file works without it, and a dump folder can be copied into the app instead."), 4);
        Ui.add(c, Ui.button(this, "Allow", Ui.Kind.PRIMARY, v -> openAccessSettings()), 14, Ui.wrap());
        return c;
    }

    // ---- Settings page ----

    private void buildSettings(LinearLayout content) {
        accessCard = null;
        gameBox = null;
        content.addView(Ui.header(this, Ui.back(this, v -> page(false)), "Settings", null));

        // Updates first, so the version and the check are easy to find (Updater: the release app also checks at every fresh start)
        Ui.add(content, Ui.section(this, "Updates"), 16);
        LinearLayout upd = settingsCard(content);
        updateRow = new Ui.Setting(this, "🔄", "Check for updates", v -> Updater.checkNow(this)).chevron(this);
        upd.addView(updateRow.view);

        Ui.add(content, Ui.section(this, "Game defaults"), Ui.SECTION_GAP);
        LinearLayout play = settingsCard(content);
        overlayRow = new Ui.Setting(this, "👀", "Overlay", v -> overlayItems())
                .toggle(this, Prefs.overlay(this), (b, on) -> { Prefs.overlay(this, on); refreshSettings(); });
        play.addView(overlayRow.view);
        Ui.Setting touch = new Ui.Setting(this, "🎮", "On-screen controls", null).chevron(this);
        touch.view.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("On-screen controls")
                .setSingleChoiceItems(TouchControls.MODE_NAMES, TouchControls.mode(this), (d, w) -> { TouchControls.mode(this, w); touch.value(touchLabel(w)); d.dismiss(); })
                .setNegativeButton("Cancel", null).show());
        touch.value(touchLabel(TouchControls.mode(this)));
        play.addView(touch.view);
        resRow = new Ui.Setting(this, "🖼️", "Resolution", v -> gameRes()).chevron(this);
        play.addView(resRow.view);
        if (!Profile.anyResolution()) resRow.view.setVisibility(View.GONE);   // no game of this APK has a window_fn or graphics_scale lever: the default would do nothing
        capRow = new Ui.Setting(this, "⏱️", "Frame rate cap", v -> fpsCap()).chevron(this);
        play.addView(capRow.view);
        play.addView(new Ui.Setting(this, "📳", "Rumble", null).toggle(this, Prefs.rumble(this), (b, on) -> Prefs.rumbleDefault(this, on))
                .value("On the controller, or the phone without one").view);
        play.addView(new Ui.Setting(this, "🚀", "Use fast cores", null).toggle(this, Prefs.pinThreads(this), (b, on) -> Prefs.pinThreadsDefault(this, on))
                .value("Keeps the busiest threads off the small cores").view);
        Ui.Setting xbox = new Ui.Setting(this, "🅰️", "Xbox button label swap", null).chevron(this);
        xbox.view.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Xbox button label swap")
                .setSingleChoiceItems(Prefs.XBOX_NAMES, Prefs.xboxButtons(this), (d, w) -> { Prefs.xboxButtonsDefault(this, w); xbox.value(Prefs.xboxLabel(w)); d.dismiss(); })
                .setNegativeButton("Cancel", null).show());
        xbox.value(Prefs.xboxLabel(Prefs.xboxButtons(this)));
        play.addView(xbox.view);
        Ui.Setting padMap = new Ui.Setting(this, "🕹️", "Button mapping", null).chevron(this);
        padMap.view.setOnClickListener(v -> PadMapUi.forDefault(this, () -> padMap.value(PadMapUi.label(this, null))));
        padMap.value(PadMapUi.label(this, null));
        play.addView(padMap.view);
        upsRow = new Ui.Setting(this, "🔍", "Upscaler", v -> pick("Upscaler", GameUpscaler.MODES, GameUpscaler.mode(this), w -> GameUpscaler.modeDefault(this, w))).chevron(this);
        play.addView(upsRow.view);
        sharpRow = new Ui.Setting(this, "✨", "FSR sharpness", v -> pick("FSR sharpness", GameUpscaler.SHARP_NAMES, GameUpscaler.sharpIndex(GameUpscaler.sharp(this)),
                w -> GameUpscaler.sharpDefault(this, GameUpscaler.SHARPS[w]))).chevron(this);
        play.addView(sharpRow.view);
        drsRow = new Ui.Setting(this, "📐", "Dynamic resolution", v -> DrsSlider.dialog(this, "", Prefs.drs(this), null, true, w -> Prefs.drsDefault(this, w),
                this::refreshSettings)).chevron(this);
        play.addView(drsRow.view);
        lsfgRow = new Ui.Setting(this, "✨", "Frame generation", v -> frameGen()).chevron(this);
        play.addView(lsfgRow.view);
        TextView hint = Ui.small(this, "A game's own settings (⋯ on its card) override these.");
        hint.setPadding(dp(4), 0, dp(4), 0);
        Ui.add(content, hint, 8);

        Ui.add(content, Ui.section(this, "System"), Ui.SECTION_GAP);
        LinearLayout sys = settingsCard(content);
        gpuRow = new Ui.Setting(this, "🎮", "GPU driver", v -> GpuDriver.dialog(this, PICK_DRIVER)).chevron(this);   // custom GPU drivers (GpuDriver.java)
        sys.addView(gpuRow.view);
        sys.addView(new Ui.Setting(this, "📁", "Folders", v -> Folders.dialog(this)).value("Game, pack and save folders").chevron(this).view);
        // some phones stop or slow a background job once the screen is off (GitHub discussion 58): Home keeps the screen on while preparing (Ondevice), the service its wake lock
        sys.addView(new Ui.Setting(this, "🔆", "Keep the screen on while preparing", null).toggle(this, Prefs.keepScreenPreparing(this), (b, on) -> Prefs.keepScreenPreparing(this, on))
                .value("While Home shows a game being prepared").view);   // the game, pack and save folders, kept across updates (Folders.java)

        Ui.add(content, Ui.section(this, "Help"), Ui.SECTION_GAP);
        LinearLayout help = settingsCard(content);
        help.addView(new Ui.Setting(this, "📋", "Readiness check", v -> startActivityForResult(new Intent(this, ReadinessActivity.class), ReadinessActivity.REQUEST))
                .value("Can this phone run your games?").chevron(this).view);   // (ReadinessActivity.java)
        help.addView(new Ui.Setting(this, "🧪", "Graphics self-test", v -> startActivity(new Intent(this, SelftestActivity.class))).value("Needs no game").chevron(this).view);
        help.addView(new Ui.Setting(this, "📨", "Share log", v -> shareLog()).value("The newest log").chevron(this).view);

        Ui.add(content, Ui.section(this, "About"), Ui.SECTION_GAP);
        LinearLayout about = Ui.add(content, Ui.card(this), 8);
        TextView tag = Ui.chip(this, Prefs.buildTag(this));   // which build this is (also the first line of every log)
        tag.setTextIsSelectable(true);
        about.addView(tag, Ui.wrap());
        StringBuilder ids = new StringBuilder();   // the games this build includes, so a player can tell whether it has theirs (Profile.missing points here)
        for (Profile p : Profile.all()) ids.append("\n· ").append(p.name).append(" (").append(p.titleId).append(")");
        TextView t = Ui.small(this, "Games in this app:" + ids + "\n\nBased on nxnative by seedds. LSFG from Eden (GPL-3.0)." + (BuildConfig.NX_LSFG ? "" : " Built without LSFG."));
        t.setTextIsSelectable(true);
        Ui.add(about, t, 10);
        refreshSettings();
    }

    /** A settings group's card of rows, under its section label */
    private LinearLayout settingsCard(LinearLayout content) { return Ui.add(content, Ui.listCard(this), 8); }

    /** The settings rows' current values (after a dialog, on resume) */
    private void refreshSettings() {
        if (overlayRow == null) return;
        updateRow.value("Banana Native " + Updater.version(this) + (Updater.available(this) ? "" : " · release app only"));
        boolean ov = Prefs.overlay(this);
        if (overlayRow.toggle.isChecked() != ov) overlayRow.toggle.setChecked(ov);
        int n = 0;
        for (String[] it : Overlay.ITEMS) if (Prefs.overlayItem(this, it[0])) n++;
        overlayRow.value(ov ? "On · " + n + " of " + Overlay.ITEMS.length + " items" : "Off");
        resRow.value(resLabel());
        capRow.value(capLabel(Prefs.fpsCap(this)));
        upsRow.value(GameUpscaler.MODES[GameUpscaler.mode(this)]);
        sharpRow.value(GameUpscaler.sharpName(GameUpscaler.sharp(this)));
        sharpRow.view.setVisibility(GameUpscaler.mode(this) == GameUpscaler.MODE_FSR ? View.VISIBLE : View.GONE);
        drsRow.value(Prefs.DRS_NAMES[Prefs.drs(this)]);
        lsfgRow.value(!BuildConfig.NX_LSFG ? "Not in this build" : Lsfg.on(this) ? "On · " + Lsfg.multiplier(this) + "x" + (Lsfg.find(this) == null ? " · Lossless.dll missing" : "") : "Off");
        GpuDriver.Info d = GpuDriver.selected(this);
        gpuRow.value(d == null ? "System" : d.label());
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) refreshSettings();   // back from a dialog (GPU driver, Folders, …)
    }

    @Override protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        fromIntent(i);
    }

    /** adb: am start -n dev.nxnative.android/.MainActivity --es game <game folder> [--es pack <pack folder>] [--es save <save folder>] [--ez run true] adds the game without
     *  the pickers (android/TESTING.md); --es add_game <folder or .nup> takes Add game's own path instead (gamePicked: all of a folder's games added, Prepare asked); save is the save folder Folders would pick for it (test runs of a play or release app keep away from the user's saves/<id>) */
    private void fromIntent(Intent i) {
        if (settingsPage && (i.hasExtra("game") || i.hasExtra("add_game") || i.hasExtra("ondevice_dump") || i.hasExtra("ondevice_pause"))) page(false);   // these act on Home's cards
        Ondevice.fromIntent(this, i);   // --es ondevice_dump <dump folder> [--ez ondevice_start true] (Ondevice)
        GpuDriver.fromIntent(this, i);   // --es gpu_driver_zip <zip> / --es gpu_driver <name | system>
        PackUi.fromIntent(this, i);   // test app: --es pack_title <id> --es pack_export <folder> | --es pack_import <zip or folder>
        String fg = i.getStringExtra("frontend_game");   // FrontendLaunch: a game it could not start at once
        if (fg != null) { fromFrontend(new File(fg)); return; }
        String ag = i.getStringExtra("add_game");   // Add game's own path for a picked folder or .nup (gamePicked), without the picker
        if (ag != null) { if (!needAccess(new File(ag))) gamePicked(new File(ag)); return; }
        String game = i.getStringExtra("game");
        if (game == null) return;
        File dir = new File(game);   // a game folder or a .nup (NUP.md)
        String id;
        try { id = Pack.titleIdOf(dir); } catch (IOException e) { error(e.getMessage()); return; }
        String p = i.getStringExtra("pack");
        File pack = p != null ? new File(p) : Folders.packFor(this, dir, id);
        if (pack == null) { error("No pack found for " + id + " next to " + game + "."); return; }
        if (!addWithPack(dir, id, pack)) return;
        String save = i.getStringExtra("save");
        if (save != null) Prefs.folder(this, "save_" + id, new File(save).getPath());
        if (i.getBooleanExtra("run", false)) for (Game g : games) if (GameList.samePath(g.gameDir, dir.getPath())) { launch(g); break; }
    }

    /** A game a frontend sent that needs the user (FrontendLaunch.ready): its card's Play says what is missing and offers the fix; a new game takes the steps of a picked one */
    private void fromFrontend(File game) {
        if (settingsPage) page(false);
        refresh();   // FrontendLaunch may have added its card
        if (needAccess(game)) return;
        for (Game g : games) if (GameList.samePath(g.gameDir, game.getPath())) { launch(g); return; }
        gamePicked(game);
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        if (gameBox != null && Game.load(this).size() != games.size()) refresh();   // a card FrontendLaunch added while Home was in the background
        onResumeViews();
        Ondevice.resume(this);
        Updater.resume(this);   // back from "Install unknown apps" with an update download waiting
        ExitWatch.resume(this);
        updateTick();   // a game that ended unexpectedly: "The game stopped unexpectedly", once (and every 2 s while a game runs and Home shows)
    }

    private void onResumeViews() {
        if (accessCard != null) accessCard.setVisibility(Environment.isExternalStorageManager() || !listNeedsAccess() ? View.GONE : View.VISIBLE);
        bananas.setRunning(true);
        refreshSettings();
    }

    /** The bananas stop while the screen is covered (a game, the self-test, settings) or off */
    @Override protected void onPause() {
        resumed = false;
        bananas.setRunning(false);
        Ondevice.pause(this);
        ExitWatch.pause();
        updateUi.removeCallbacks(updateRun);
        super.onPause();
    }

    // ---- A game card whose shaders are updated in the background (Ondevice.autoUpdate) while it stays playable: its state line carries the update's count and time left,
    // every 2 s while Home shows ("Ready · updating shaders 1,234 / 22,852 · about 12 min left"); when the job is done the cards are checked again ----
    private final java.util.Map<TextView, Game> updating = new java.util.HashMap<>();
    private final android.os.Handler updateUi = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable updateRun = this::updateTick;
    private String updateDone;

    private void updateTick() {
        updateUi.removeCallbacks(updateRun);
        if (!resumed || updating.isEmpty()) return;
        boolean done = false;
        for (java.util.Map.Entry<TextView, Game> e : updating.entrySet()) {
            if (e.getKey().getParent() == null) continue;   // a card of an older list
            Game g = e.getValue();
            Job j = Ondevice.cardJob(this, g);
            if (j == null) {   // no update runs for this card: say why instead of "updating" with no count
                String why = Ondevice.busy(this) ? "Ready · shader update waits for the other game's preparing"
                        : Ondevice.deltaShaders(new File(g.packDir), g.titleId) < 0 ? "Ready · new shaders: \u22ef \u203a Prepare to update them" : "Ready · shader update starting";
                Ui.status(e.getKey(), why, R.color.bn_leaf);
                continue;
            }
            String s = j.state();
            if (s.equals("done")) {   // once per finished job: a card that still reads as updating after the check is not checked again and again
                String key = j.file.getPath() + ":" + j.file.lastModified();
                if (!key.equals(updateDone)) { updateDone = key; done = true; }
                continue;
            }
            if (j.currentStep().equals("rewrite")) { Ui.status(e.getKey(), s.equals("running") ? "Ready · updating game code" : "Ready · game code update paused", R.color.bn_leaf); continue; }   // (its progress once the rewrite step reports one)
            Ui.status(e.getKey(), ShaderLibs.updatingText(java.util.Locale.getDefault(), j.shadersDone(), j.shadersTotal(), Job.etaText(j.shadersEta()), !s.equals("running")), R.color.bn_leaf);
        }
        if (done) { refresh(); return; }   // the new library is in: the cards' states are checked again ("Ready")
        updateUi.postDelayed(updateRun, 2000);
    }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    /** Home's games: a heading with Add game over one card per game (also over no card while a game is being prepared); with none, a single card that says what to do */
    void refresh() {
        updating.clear();
        games.clear();
        games.addAll(Game.load(this));
        if (gameBox == null) return;
        if (!merged && games.size() > 1) {   // a list an older version left with two entries of one game: one card each (GameList.collapse; reads the dumps: off the main thread)
            merged = true;
            new Thread(() -> { if (Game.collapse(this)) runOnUiThread(() -> { if (!isFinishing()) refresh(); }); }, "nx-list-merge").start();
        }
        gameBox.removeAllViews();
        if (games.isEmpty() && !Ondevice.showing()) { gameBox.addView(emptyCard()); return; }
        // while a game is being prepared its card says what happens (the steps card would only repeat it), and the heading's Add game still adds another game
        LinearLayout top = Ui.row(this);
        TextView h = Ui.section(this, "Your games");
        top.addView(h, Ui.weight());
        Button add = Ui.button(this, "Add game", Ui.Kind.QUIET, v -> addGame());
        add.setCompoundDrawablesRelativeWithIntrinsicBounds(Ui.icon(this, R.drawable.ic_add, R.color.bn_accent), null, null, null);
        add.setCompoundDrawablePadding(dp(6));
        top.addView(add);
        gameBox.addView(top);
        // One card per game: while a game is being prepared its card is the progress card (Ondevice), at the game's place, or first for a game not listed yet
        View pc = Ondevice.cardView();
        if (pc != null && pc.getParent() instanceof ViewGroup) ((ViewGroup) pc.getParent()).removeView(pc);
        boolean placed = false;
        if (pc != null) { for (Game g : games) placed |= Ondevice.shows(g); if (!placed) { Ui.add(gameBox, pc, 4); placed = true; pc = null; } }
        for (Game g : games) {
            if (pc != null && Ondevice.shows(g)) { Ui.add(gameBox, pc, gameBox.getChildCount() == 1 ? 4 : Ui.GAP); pc = null; continue; }
            Ui.add(gameBox, gameCard(g), gameBox.getChildCount() == 1 ? 4 : Ui.GAP);
        }
    }

    /** No game yet: the banana, the three steps, and Add game */
    private View emptyCard() {
        LinearLayout c = Ui.card(this);
        c.setGravity(Gravity.CENTER_HORIZONTAL);
        c.setPadding(dp(20), dp(28), dp(20), dp(20));
        ImageView im = new ImageView(this);
        im.setImageResource(R.drawable.ic_banana_happy);
        im.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        c.addView(im, new LinearLayout.LayoutParams(dp(80), dp(80)));
        TextView h = Ui.heading(this, "Add your game");
        h.setGravity(Gravity.CENTER);
        Ui.add(c, h, 12, Ui.wrap());
        TextView t = Ui.small(this, "Pick your dump folder, the one with base/ (and updates/), or your game's .nup file.");
        t.setGravity(Gravity.CENTER);
        Ui.add(c, t, 6, Ui.wrap());
        LinearLayout steps = Ui.row(this);
        steps.setGravity(Gravity.CENTER);
        String[] s = {"1  Pick dump", "2  Prepare", "3  Play"};
        for (int k = 0; k < s.length; k++) {
            LinearLayout.LayoutParams lp = Ui.wrap();
            if (k > 0) lp.leftMargin = dp(6);
            TextView chip = Ui.chip(this, s[k]);
            chip.setSingleLine(true);
            steps.addView(chip, lp);
        }
        Ui.add(c, steps, 16, Ui.wrap());
        Ui.add(c, Ui.action(this, R.drawable.ic_add, "Add game", Ui.Kind.PRIMARY, v -> addGame()), 20);
        return c;
    }

    /** A game's card: its tile, name and state, ⋯ Manage at the top right (far from the big button), and one big button: Play, or Prepare when the pack does not fit */
    private View gameCard(Game g) {
        LinearLayout c = Ui.card(this);
        c.setPadding(dp(16), dp(16), dp(16), dp(16));
        LinearLayout top = Ui.row(this);
        TextView tile = Ui.tile(this, g.name, 52);
        top.addView(tile);
        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(14), 0, dp(4), 0);
        TextView name = Ui.text(this, g.name, 17, R.color.bn_text, 700);
        name.setMaxLines(2);
        name.setEllipsize(TextUtils.TruncateAt.END);
        info.addView(name);
        TextView about = Ui.small(this, "");   // the version and publisher from the .nup; its icon goes in the tile
        about.setVisibility(View.GONE);
        info.addView(about);
        GameMeta.load(new File(g.gameDir), m -> {
            if (m.icon != null) Ui.tileIcon(tile, m.icon);
            if (m.line() != null) { about.setText(m.line()); about.setVisibility(View.VISIBLE); }
        });
        boolean other = Profile.of(g.titleId) == null;   // no profile in this APK: it cannot run here (Profile.missing says why)
        TextView st = other ? Ui.status(this, "Not in this app version", R.color.bn_danger) : Ui.status(this, "Checking…", R.color.bn_text_2);
        LinearLayout.LayoutParams sp = Ui.wrap();
        sp.topMargin = dp(2);
        info.addView(st, sp);
        top.addView(info, Ui.weight());
        top.addView(Ui.more(this, "Manage " + g.name, v -> manage(g)));
        c.addView(top);
        Ui.Action play = Ui.action(this, R.drawable.ic_play, "Play", Ui.Kind.PRIMARY, v -> launch(g));
        if (other) {   // nothing on this card can work in this app: the sentence, and no button (⋯ still has its folders, saves and Remove)
            Ui.add(c, Ui.body(this, Profile.missing(g.titleId, g.name)), 12);
            c.setOnLongClickListener(v -> { manage(g); return true; });
            return c;
        }
        Ui.add(c, play, 16);
        check(g, st, play, info);
        c.setOnLongClickListener(v -> { manage(g); return true; });
        return c;
    }

    /** Why a game cannot start as it is, in a few words (unfinished or busy pack code, another game build, no current shader library), and whether Prepare fixes it */
    private static final class Problem {
        final String label;
        final boolean prepare;   // Prepare (on the phone, from the dump) makes what is missing
        boolean shadersOnly;   // only the shader library is missing
        boolean playable;      // ... and an older library of the same ABI plays while the update makes it (Pack.playableLibrary)
        boolean legacy;        // ... or only one made before versioned updates, played with a warning (Pack.legacyLibrary)
        boolean anyway;        // Play still starts it after a warning (an untested game version, another version's RomFS: MainActivity.launch)
        boolean resume;        // a Prepare beside the dump stopped part way: the button continues it
        boolean repick;        // the game's file is gone (GameFile): the button picks it again
        File saveGone;         // the picked save folder is gone: the button asks (the app's save, or pick the folder again)
        String reason = "";    // one short line under the state: why, and what the button does
        String detail = "";    // the technical part (what was compared: build IDs, the pack's error, its folder), in the card's Details and the log

        Problem(String label, boolean prepare) { this.label = label; this.prepare = prepare; }
    }

    /** The card's short state when the game cannot start as it is; null when it can */
    private Problem problem(Game g) {
        File pd = new File(g.packDir), gd = new File(g.gameDir);
        File saveGone = Folders.goneSave(this, g);
        if (saveGone != null) {   // a picked save folder that is gone: the start would make a new empty save there, so it asks first
            Problem s = new Problem("Save folder gone", false);
            s.saveGone = saveGone;
            s.reason = GameFile.saveGoneText(saveGone.getName());
            s.detail = saveGone.getPath();
            return s;
        }
        String gone = gone(g);
        if (gone != null) {   // deleted, moved, renamed (or, for a .nup used where it is, its permission taken back): nothing else can be told
            Problem s = new Problem("Game file gone", false);
            s.repick = true;
            s.reason = GameFile.goneText(gd.getName());
            s.detail = gone;
            return s;
        }
        boolean dump = Ondevice.isDump(gd);
        Pack p;
        String readError = null;
        try { p = Pack.read(pd); } catch (IOException e) { p = null; readError = e.getMessage(); }
        String unfinished = p == null ? null : p.unfinishedModule();
        if (p == null || unfinished != null) {
            // Why it needs preparing, so the card does not only say "Needs preparing": no pack yet, one that cannot be read, code left unfinished, or a Prepare that
            // stopped part way (its job beside the dump: the button continues it rather than asking to start over)
            Job j = stoppedJob(gd, g.titleId);
            Problem s = new Problem(j != null ? "Preparing not finished" : dump ? "Needs preparing" : p == null ? "No pack" : "Pack unfinished", dump || j != null);
            s.resume = j != null;
            s.reason = dump || j != null ? PrepareText.needsPreparing(pd.isDirectory() && readError != null && new File(pd, "pack.tsv").isFile(), unfinished != null, j != null)
                    : "This game has no pack that fits, and it is not a dump Prepare can start from. ⋯ › Folders picks a pack.";
            s.detail = (j != null ? j.statusText(false) + "\n\n" : "") + (readError != null ? readError : unfinished + ".android.key is missing: the rewrite of " + unfinished + " did not finish.")
                    + "\nPack: " + pd + (j != null ? "\n" + j.errorDetail() : "");
            return s;
        }
        if (PackCheck.rewriting(this, pd, gd, p.titleId) != null) return new Problem("Preparing…", false);
        BuildCheck.Result b = BuildCheck.build(p, gd);
        if (!b.ok()) {
            android.util.Log.w("nxnative", g.titleId + " " + g.gameDir + " with pack " + g.packDir + ": " + b.label() + ": " + b.detail);
            Problem s = new Problem(b.label(), dump && (b.kind == BuildCheck.Kind.OTHER_BUILD || b.kind == BuildCheck.Kind.MISSING));   // Prepare makes a pack of the dump's build; it cannot fix unreadable files, an unsupported build or another version's data
            s.anyway = b.kind == BuildCheck.Kind.UNSUPPORTED || b.kind == BuildCheck.Kind.ROMFS;
            s.reason = s.anyway ? "Untested: it may not start or may crash. Play asks first." : s.prepare ? "The pack does not fit this dump. Prepare makes a new one." : "";
            s.detail = b.message + "\n\n" + b.detail + "\nPack: " + pd;
            return s;
        }
        if (Pack.libraryProblem(pd, g.titleId) != null) {
            Problem s = new Problem("Shaders needed", dump);
            s.shadersOnly = true;
            s.playable = Pack.playableLibrary(pd, g.titleId) != null;
            s.legacy = !s.playable && Pack.legacyLibrary(pd, g.titleId) != null;
            if (!s.playable && !s.legacy) {
                s.reason = dump ? "This app version needs new shaders. Prepare shaders makes them from your dump." : "The pack has no shaders for this app version.";
                s.detail = Pack.libraryProblem(pd, g.titleId);
            }
            return s;
        }
        return null;
    }

    /** Why a game's file or folder is gone, for Details; null when it is there (or cannot be told: no All files access for a game read by its path) */
    String gone(Game g) {
        File gd = new File(g.gameDir);
        if (PickedNup.is(this, g.gameDir)) return PickedNup.problem(this, gd);
        boolean readable = Environment.isExternalStorageManager() || CopyPlan.inApp(AppStorage.root(this), g.gameDir);
        return GameFile.gone(gd.exists(), readable) ? gd.getPath() + " is not there." : null;
    }

    /** A Prepare of this dump that stopped part way (its job.json beside the dump, not done), else null */
    private static Job stoppedJob(File dump, String titleId) {
        File f = Job.fileOf(DumpCheck.workOf(dump, titleId));
        try {
            Job j = f.isFile() ? Job.read(f) : null;
            return j != null && !"done".equals(j.state()) && !j.state().isEmpty() ? j : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** The card's state, checked off the main thread (NSO headers, the stamp, and for a ready pack the rewriter's key, which hashes the modules) */
    private void check(Game card, TextView st, Ui.Action play, LinearLayout info) {
        new Thread(() -> {
            Game g = Folders.repack(this, card);   // a remembered pack folder that is gone: the usual search instead (app storage first)
            // An APK update whose shaders did not change: the pack's older library becomes this version's by its header (Pack.restamp), before anything else is decided
            String restamped = Pack.restamp(new File(g.packDir), g.titleId);
            if (restamped != null) android.util.Log.i("nxnative", g.titleId + ": shader library v-" + restamped + " taken over as v-" + BuildConfig.NX_DMSH_VK_CHAIN + " (restamp)");
            Problem problem = problem(g);
            Boolean ok = null;
            if (problem == null) try { ok = PackCheck.code(this, Pack.read(new File(g.packDir)), new File(g.gameDir)); } catch (IOException e) { ok = null; }
            Boolean code = ok;
            boolean own = Boolean.FALSE.equals(ok) && Ondevice.ownPack(new File(g.gameDir), new File(g.packDir), g.titleId);   // the app's own pack, its code from older tools
            runOnUiThread(() -> {
                if (info.getParent() == null) return;
                // An APK update's new shaders: made in the background with no question when the pack's older library is one the delta manifest knows (Ondevice.autoUpdate)
                if (problem != null && problem.shadersOnly && problem.prepare) Ondevice.autoUpdate(this, new File(g.gameDir), new File(g.packDir), g.titleId);
                if (problem == null) Ui.status(st, "Ready", R.color.bn_leaf);
                else if (problem.playable) { Ui.status(st, "Ready · updating shaders", R.color.bn_leaf); updating.put(st, g); updateTick(); }   // the count follows (updateTick)
                else if (problem.legacy) Ui.status(st, "Ready · shaders need one update", R.color.bn_accent);
                else Ui.status(st, problem.label, problem.prepare || problem.anyway ? R.color.bn_accent : R.color.bn_danger);
                if (problem != null && !problem.playable && !problem.legacy && !problem.reason.isEmpty()) {   // why, and what the button does, in a line
                    LinearLayout.LayoutParams rl = Ui.fill();
                    rl.topMargin = dp(4);
                    info.addView(Ui.small(this, problem.reason), rl);
                }
                if (problem != null && !problem.detail.isEmpty()) {   // which build IDs (or which file) the state is about: closed under Details, copyable
                    LinearLayout.LayoutParams dl = Ui.fill();
                    dl.topMargin = dp(2);
                    info.addView(Ui.details(this, problem.detail), dl);
                }
                if (problem != null && problem.anyway) play.setText("Play (untested)");
                if (problem != null && problem.saveGone != null) {
                    play.setText("Fix the save folder");
                    play.setOnClickListener(v -> Folders.saveGoneDialog(this, g, problem.saveGone));
                }
                if (problem != null && problem.repick) {
                    play.setText("Pick it again");
                    play.setIcon(R.drawable.ic_add);
                    play.setOnClickListener(v -> relink(g));
                }
                if (problem != null && problem.legacy) {   // Play stays Play; a chip says why the shaders want one more update
                    TextView n = Ui.warnChip(this, "Shaders need one update", "Shaders need one update", ShaderLibs.LEGACY_WARNING);
                    LinearLayout.LayoutParams wl = Ui.wrap();
                    wl.topMargin = dp(6);
                    info.addView(n, wl);
                }
                if (problem != null && problem.prepare && !problem.playable && !problem.legacy) {   // the big button prepares (Ondevice asks first and says what it will make)
                    play.setText(problem.resume ? "Continue preparing" : problem.shadersOnly ? "Prepare shaders" : "Prepare");
                    play.setIcon(R.drawable.ic_build);
                    play.setOnClickListener(v -> { if (!needAccess(new File(g.gameDir))) Ondevice.prepare(this, new File(g.gameDir), false); });
                }
                if (!Boolean.FALSE.equals(code)) return;
                // The app's own pack whose code older tools made (an APK update changed the rewriter or the profile's patches): updated in the background, no question;
                // the card says so like a shader update. Play stays on: the new code is swapped in only when the rewrite is complete
                if (own) {
                    if (Ondevice.autoCodeUpdate(this, new File(g.gameDir), new File(g.packDir))) {
                        Ui.status(st, "Ready · updating game code", R.color.bn_leaf);
                        updating.put(st, g); updateTick();
                    } else Ui.status(st, "Ready · game code update waits", R.color.bn_leaf);   // a game or another job runs (or it was tried this run): the next check starts it
                    return;   // the app's own pack: no "other tools" chip
                }
                // A pack whose code other tools made: a small chip; its tap says what that means
                TextView n = Ui.warnChip(this, "Pack from other tools", "Pack from other tools",
                        "This game's pack was made by other tools than this app's. It usually still runs. If the game misbehaves, ⋯ › Prepare again remakes it from your dump.");
                LinearLayout.LayoutParams lp = Ui.wrap();
                lp.topMargin = dp(6);
                info.addView(n, lp);
            });
        }, "nx-pack-check").start();
    }

    /** The game's Manage sheet, also from its progress card's ⋯ while it is being prepared (Ondevice) */
    void manageGame(Game g) { manage(g); }

    /**
     * A game's Manage sheet (⋯ on its card, or a long press): a list of actions (its folders, its settings, its log, its saves, Prepare again for a dump), its facts
     * (title ID, folders) below, and, apart at the bottom, Remove from the list (asked first; it deletes nothing on the storage). Play never sits next to it.
     */
    private void manage(Game g) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        File save = Folders.save(this, g);
        String lp = Pack.libraryProblem(new File(g.packDir), g.titleId);
        String noProfile = Profile.missing(g.titleId, g.name);
        AlertDialog[] d = new AlertDialog[1];
        if (noProfile != null) Ui.add(box, Ui.small(this, noProfile), 0);
        // Saves: backups in <Switch2>/save-backups/<id>/, verified; a restore backs up the current save first (SaveUi, SaveStore)
        if (noProfile == null) Ui.add(box, new Ui.Setting(this, "⚙️", "Game settings", v -> { d[0].dismiss(); GameSettings.dialog(this, g); }).chevron(this).view, 0);
        if (noProfile == null && Shortcuts.supported(this))
            Ui.add(box, new Ui.Setting(this, "🏠", "Add to home screen", v -> { d[0].dismiss(); Shortcuts.pin(this, g); }).value("A shortcut that starts this game").chevron(this).view, 0);
        Ui.add(box, new Ui.Setting(this, "📁", "Folders", v -> { d[0].dismiss(); Folders.dialog(this, g); }).chevron(this).view, 0);
        Ui.add(box, new Ui.Setting(this, "💾", "Back up saves", v -> { d[0].dismiss(); SaveUi.backup(this, g); }).value(SaveUi.backupLine(this, g)).chevron(this).view, 0);
        Ui.add(box, new Ui.Setting(this, "↩️", "Restore saves", v -> { d[0].dismiss(); SaveUi.restoreList(this, g); }).chevron(this).view, 0);
        Ui.add(box, new Ui.Setting(this, "📥", "Import save", v -> { d[0].dismiss(); SaveUi.importPick(this, g); }).chevron(this).view, 0);
        if (Ondevice.isDump(new File(g.gameDir)) && noProfile == null)
            Ui.add(box, new Ui.Setting(this, "🔁", "Prepare again", v -> { d[0].dismiss(); Ondevice.prepare(this, new File(g.gameDir), true); }).value("Remakes what does not fit").chevron(this).view, 0);
        // The pack as one .zip, out and in (PackUi, PackZip): an import goes into the app's packs/<id> and is checked before it replaces anything
        Ui.add(box, new Ui.Setting(this, "📦", "Export pack…", v -> { d[0].dismiss(); PackUi.exportPick(this, g); }).value("One .zip, into a folder you pick").chevron(this).view, 0);
        if (noProfile == null)
            Ui.add(box, new Ui.Setting(this, "📲", "Import pack…", v -> { d[0].dismiss(); PackUi.importPick(this, g); }).value("A .zip or a pack folder").chevron(this).view, 0);
        if (Ondevice.shows(g))   // its card is the progress card: the job's own Manage (Forget, the deletes)
            Ui.add(box, new Ui.Setting(this, "⏳", "Preparing", v -> { d[0].dismiss(); Ondevice.manageJob(this); }).value("Forget the job or delete its working files").chevron(this).view, 0);
        Ui.add(box, new Ui.Setting(this, "📨", "Share log", v -> { d[0].dismiss(); shareLog(g.titleId); }).chevron(this).view, 0);
        TextView info = Ui.small(this, "Title ID: " + g.titleId + "\nGame: " + (PickedNup.is(this, g.gameDir) ? new File(g.gameDir).getName() + " (.nup used where it is, picked with the file picker)"
                : g.gameDir + (Nup.isNup(new File(g.gameDir)) ? " (.nup)" : "")) + "\nPack: " + g.packDir
                + "\nShaders: " + (lp == null ? "present (" + BuildConfig.NX_DMSH_VK_CHAIN + ")" : lp) + "\nSaves: " + (save != null ? save.getPath() : "not set"));
        info.setTextIsSelectable(true);
        info.setTextSize(12);
        info.setPadding(dp(4), 0, dp(4), 0);
        Ui.add(box, info, 16);
        Ui.add(box, Ui.divider(this), 16, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
        Ui.add(box, Ui.button(this, "Remove from list", Ui.Kind.DANGER, v -> { d[0].dismiss(); confirmRemove(g); }), 16);
        d[0] = Ui.sheet(this, g.name, box).setPositiveButton("Done", null).show();
    }

    private void openAccessSettings() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    /** A game in app storage (Copy into the app, GameCopy) or a .nup picked in place (PickedNup: its stand-in path is in app storage) is read without All files access */
    boolean needAccess(File game) { return (game == null || !CopyPlan.inApp(AppStorage.root(this), game.getPath())) && needAccess(); }

    /** Whether Home's access card is worth showing: no game yet, or one that is read where it is by its path */
    private boolean listNeedsAccess() {
        List<Game> l = Game.load(this);
        if (l.isEmpty()) return true;
        for (Game g : l) if (!CopyPlan.inApp(AppStorage.root(this), g.gameDir)) return true;
        return false;
    }

    /** A game copied into the app (Add game › Copy into the app): it is added as a picked one */
    void copied(File game) {
        Toast.makeText(this, "Copied into the app: " + game.getName(), Toast.LENGTH_LONG).show();
        gamePicked(game);
    }

    boolean needAccess() {
        if (Environment.isExternalStorageManager()) return false;
        new AlertDialog.Builder(this).setTitle("Allow file access").setMessage("The app needs All files access to read your game dump. Allow it, then come back.")
                .setPositiveButton("Open settings", (d, w) -> openAccessSettings()).setNegativeButton("Cancel", null).show();
        return true;
    }

    void pickFolder(int request, String title) {
        Toast.makeText(this, title, Toast.LENGTH_LONG).show();
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Uri.parse("content://com.android.externalstorage.documents/document/primary%3A"));
        startActivityForResult(i, request);
    }

    /** Add game: the dump is a folder (base/, updates/) or one .nup file (NUP.md); the user says which, then the system picker opens for it */
    void addGame() {
        if (!Environment.isExternalStorageManager()) { addWithoutAccess(); return; }
        new AlertDialog.Builder(this).setTitle("Add game: your dump is…")
                .setItems(new String[] {"A folder (with base/, and updates/ if any)", "A .nup file"}, (d, w) -> {
                    if (w == 0) pickFolder(PICK_GAME, "Pick your dump folder (with base/, and updates/ if any)");
                    else pickNup();
                })
                .setNegativeButton("Cancel", null).show();
    }

    /**
     * Add game without All files access: a .nup is used where it is (PickedNup: the picker's read permission, kept; nothing copied); a dump folder is copied into the
     * app's own storage (GameCopy: the game opens its RomFS files by path, and the picker gives no path); or the user turns All files access on to read dumps where
     * they are
     */
    private void addWithoutAccess() {
        new AlertDialog.Builder(this).setTitle("Add game: your dump is…")
                .setItems(new String[] {"A .nup file (used where it is)", "A dump folder (copied into the app: a folder needs All files access to be read where it is)",
                        "Allow All files access"}, (d, w) -> {
                    if (w == 0) pickNupHere();
                    else if (w == 1) { Toast.makeText(this, "Pick your dump folder (with base/, and updates/ if any)", Toast.LENGTH_LONG).show(); GameCopy.pickTree(this); }
                    else openAccessSettings();
                })
                .setNegativeButton("Cancel", null).show();
    }

    /** The file picker for a .nup used where it is (PickedNup) */
    void pickNupHere() {
        Toast.makeText(this, "Pick your game's .nup file", Toast.LENGTH_LONG).show();
        startActivityForResult(PickedNup.pickIntent(), PICK_NUP_HERE);
    }

    /** The card whose gone file is being picked again (relink): the next picked game goes to it */
    private Game relinkFor;

    /**
     * Pick the file again for a card whose file is gone (GameFile): a .nup with the file picker (used where it is without All files access, by its path with it), a
     * dump folder with the folder picker (All files access). The picked file takes the card's place when it holds the same game (relinkTo)
     */
    void relink(Game g) {
        boolean nup = g.gameDir.endsWith(Nup.EXT);
        if (!nup && needAccess()) return;
        relinkFor = g;
        if (!nup) pickFolder(PICK_GAME, "Pick " + g.name + "'s dump folder again");
        else if (Environment.isExternalStorageManager()) pickNup();
        else pickNupHere();
    }

    /** The picked file in the card's place (its pack, settings and saves stay: they belong to the card and its title ID), when it holds the same game */
    private void relinkTo(Game g, File game) {
        new Thread(() -> {
            String id = null, err;
            try { id = Pack.titleIdOf(game); err = GameFile.relinkProblem(g.titleId, g.name, id, game.getName()); } catch (IOException e) { err = e.getMessage(); }
            if (err == null) {
                synchronized (Game.class) {
                    List<Game> list = Game.load(this);
                    int i = GameFile.index(list, g.gameDir);
                    if (i >= 0) list.set(i, new Game(g.titleId, g.name, game.getPath(), g.packDir, list.get(i).used));
                    else list.add(new Game(g.titleId, g.name, game.getPath(), g.packDir, g.used));
                    Game.save(this, list);
                }
                if (!game.getPath().equals(g.gameDir)) PickedNup.forget(this, new File(g.gameDir));   // the old .nup's permission, when it was one used where it is
            } else if (!game.getPath().equals(g.gameDir)) PickedNup.forget(this, game);   // a picked file of another game: not kept
            String fe = err;
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (fe != null) { error(fe); return; }
                Toast.makeText(this, g.name + " now uses " + game.getName() + ".", Toast.LENGTH_LONG).show();
                refresh();
            });
        }, "nx-relink").start();
    }

    /** A .nup picked to use where it is: its permission kept and the file opened off the main thread, then added as any picked game */
    private void nupHere(Uri u) {
        new Thread(() -> {
            File f = null;
            String err = null;
            boolean streamed = false;
            try { f = PickedNup.picked(this, u); } catch (PickedNup.Streamed e) { err = e.getMessage(); streamed = true; } catch (IOException e) { err = e.getMessage(); }
            File ff = f;
            String fe = err;
            boolean fs = streamed;
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (ff == null) {
                    relinkFor = null;
                    AlertDialog.Builder b = Ui.explain(this, "Cannot use this file", fe, u.toString()).setNegativeButton("OK", null);
                    if (fs) b.setPositiveButton("Copy into the app", (d, w) -> GameCopy.copyNup(this, u));
                    b.show();
                    return;
                }
                if (relinkFor != null) { Game g = relinkFor; relinkFor = null; relinkTo(g, ff); return; }
                addAll(List.of(ff), true);
            });
        }, "nx-pick-nup").start();
    }

    /**
     * The system file picker for a .nup; its document is turned into its /storage path (PathUtil.documentToPath), as a picked folder is. A document without such a path
     * (Recent, Downloads) is used where it is (PickedNup), so the picker asks for a lasting read permission too
     */
    private void pickNup() {
        Toast.makeText(this, "Pick your game's .nup file", Toast.LENGTH_LONG).show();
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Uri.parse("content://com.android.externalstorage.documents/document/primary%3A"));
        startActivityForResult(i, PICK_NUP);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (Ondevice.activityResult(this, request, result, data)) return;
        if (request == ReadinessActivity.REQUEST) { if (result == ReadinessActivity.FIX_DRIVER) GpuDriver.dialog(this, PICK_DRIVER); return; }   // its "Pick a GPU driver"
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == PICK_DRIVER) { GpuDriver.picked(this, data.getData()); return; }
        if (GameCopy.activityResult(this, request, data)) return;
        if (PackUi.documentResult(this, request, data.getData())) return;
        if (SaveUi.documentResult(this, request, data.getData())) return;
        if (request == PICK_NUP_HERE) { nupHere(data.getData()); return; }
        if (request == PICK_NUP) {
            String p = PathUtil.documentToPath(data.getData());
            if (p == null) { nupHere(data.getData()); return; }   // Recent, Downloads: no path, but the file can be used where it is (PickedNup)
            gamePicked(new File(p));
            return;
        }
        if (request == PICK_DLL) {
            String err = Lsfg.install(this, data.getData());
            Toast.makeText(this, err == null ? "Lossless.dll installed" : "Lossless.dll not installed: " + err, Toast.LENGTH_LONG).show();
            return;
        }
        String path = PathUtil.treeToPath(data.getData());
        if (path == null) { error("That folder is not on this phone's storage (" + data.getData() + ")."); return; }
        File dir = new File(path);
        if (Folders.activityResult(this, request, dir)) return;
        if (PackUi.activityResult(this, request, dir)) return;
        if (request == PICK_GAME) gamePicked(dir);
        else if (request == PICK_PACK && pendingGame != null) { pendingPackPick = dir; addWithPack(pendingGame, pendingId, dir); }
    }

    /**
     * A picked game: one .nup, one dump folder (base/exefs/main.npdm or exefs/main.npdm), or a folder that holds games (PackFind.gamesIn: its .nup files and
     * dump subfolders), all of which are added (addAll); a folder with one game is that game
     */
    private void gamePicked(File game) {
        if (relinkFor != null) { Game g = relinkFor; relinkFor = null; relinkTo(g, game); return; }
        if (Nup.isNup(game) || Nup.wrongCase(game)) { nupPicked(game); return; }
        if (Pack.npdmOf(game) != null) { addAll(List.of(game), true); return; }
        List<File> found = PackFind.gamesIn(game);
        if (found.size() == 1) { gamePicked(found.get(0)); return; }
        if (found.isEmpty()) {
            error(game + " has no main.npdm (base/exefs/, updates/<version>/exefs/ or exefs/), no .nup file and no game folder in it. Pick the game's own folder, its .nup file, or the folder that holds your games.");
            return;
        }
        addAll(found, false);
    }

    /** A single .nup (NUP.md): its name and access checked here, the rest as every game (addAll) */
    private void nupPicked(File nup) {
        if (Nup.wrongCase(nup)) { error(nup.getName() + ": rename it so that its name ends in \".nup\" (lower case); the game and the tools read only that name."); return; }
        if (!Nup.picked(nup) && !nup.canRead()) { error(nup + " cannot be read. Check that All files access is on."); return; }
        addAll(List.of(nup), true);
    }

    /** What the background check of Add game found for one game */
    private static final class Pick {
        final File game;
        String id, name, error;   // error: why it cannot be added (unreadable, no profile in this APK), in a sentence
        boolean unsupported;      // no profile in this APK
        boolean dump;             // a dump Prepare starts from (Pack.isDump)
        PackFind.Found pack;      // a pack that plays now or after the automatic shader update (Folders.find), null: none
        DumpLayout.Found found;   // without a pack: the version Prepare would start from, or why it cannot (DumpLayout.find)
        String key;               // its game build (Game.key: title, version, main's build ID), null when it cannot be told
        final List<Game> others = new ArrayList<>();   // Home's entries of the same title that are another dump (GameList.others): the user decides

        Pick(File game) { this.game = game; name = game.getName(); }
    }

    /**
     * Add game for one or more games: each is identified off the main thread (a .nup's TOC is hashed) with its best pack (Folders.find: the picked folder,
     * the app's, beside the game, &lt;id&gt;-work/&lt;any&gt; above it) and checked (PackCheck.blocking), then added on the main thread. A game with a pack that plays
     * now or after the automatic shader update is added with it, with no question. single: one picked game, as before: a dump without such a pack goes to Prepare
     * (Ondevice asks first; it offers an existing pack folder or Import pack… too), a one-version folder to the pack folder picker, and a problem is one message.
     * Several (a picked folder's games): a dump without a usable pack gets a card with its Prepare button (one job at a time is the user's to start), and one
     * message sums up what was added, what this app cannot run and what needs Prepare.
     * One card per game (GameList): the same file or the same title and build replaces its entry; another dump of a title on the list is asked about (single:
     * replace it or keep both) or kept beside it (several, said in the summary)
     */
    private void addAll(List<File> picked, boolean single) {
        Toast.makeText(this, single ? "Checking " + picked.get(0).getName() + "…" : "Checking " + picked.size() + " games…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            List<Pick> out = new ArrayList<>();
            for (File g : picked) {
                Pick k = new Pick(g);
                out.add(k);
                if (Nup.wrongCase(g)) { k.error = g.getName() + ": rename it so that its name ends in \".nup\" (lower case)."; continue; }
                try { k.id = Pack.titleIdOf(g); } catch (IOException e) { k.error = e.getMessage(); continue; }
                String why = Profile.missing(k.id, single ? null : g.getName());
                if (why != null) { k.unsupported = true; k.error = why; continue; }
                k.name = Profile.of(k.id).name;
                k.dump = Pack.isDump(g);
                String pp = Folders.pickedPack(this, k.id);
                File pd = pp == null ? null : new File(pp);
                try { if (pd != null && !Pack.read(pd).titleId.equals(k.id)) pd = null; } catch (IOException e) { pd = null; }
                k.pack = Folders.find(this, pd, g, k.id);
                if (k.pack == null) k.found = DumpLayout.find(g);   // no pack: whether Prepare can start from it (any layout), or why not (another build, incomplete)
                else if (!k.pack.pack.gameVersion(g).equals(k.pack.pack.version))   // the pack's build under another version number of this dump: the same code, played as it is
                    android.util.Log.i("nxnative", "Add game: " + g.getName() + " keeps the pack's build (version " + k.pack.pack.version + ") as version " + k.pack.pack.gameVersion(g) + ": same build IDs, same code");
                k.key = Game.key(g.getPath(), k.id);   // read here, off the main thread: the add takes the keys from the cache
                List<Game> list = Game.load(this);
                for (int i : GameList.others(list, k.id, g.getPath(), k.key, Game::key)) k.others.add(list.get(i));
                android.util.Log.i("nxnative", "Add game: " + g + " (" + k.id + ", " + k.key + "): " + (k.pack == null ? "no usable pack" : "pack " + k.pack.dir + (k.pack.tier == PackFind.READY ? " (ready)" : " (shader update)"))
                        + (k.others.isEmpty() ? "" : "; " + k.others.size() + " other dump(s) of it on the list"));
            }
            runOnUiThread(() -> { if (!isFinishing()) added(out, single); });
        }, "nx-add-game").start();
    }

    private void added(List<Pick> picks, boolean single) {
        if (single) {
            Pick k = picks.get(0);
            if (k.error != null) { error(k.error); return; }
            askReplace(k.name, k.game, k.others, replace -> {
                if (k.pack != null) { addWithPack(k.game, k.id, k.pack.dir, replace); return; }
                if (replace != null) removeEntries(replace);   // the user chose this dump: the other's card goes now (nothing is deleted), this one's comes with its pack
                if (k.found != null && k.found.error == null) {
                    if (k.found.build >= 0 && k.found.build != k.found.version)
                        android.util.Log.i("nxnative", "Add game: " + k.game.getName() + " " + DumpLayout.name(k.found.version) + " is " + Profile.of(k.id).buildName(k.found.build) + " by its build IDs");
                    Ondevice.prepare(this, k.game, false);
                    return;
                }   // a dump of a supported build: Prepare makes its pack
                if (k.found != null && k.found.otherBuild) { Ondevice.prepare(this, k.game, false); return; }   // a build this app does not list: Prepare says so and offers Try anyway
                pendingGame = k.game;
                pendingId = k.id;
                String what = Nup.isNup(k.game) ? ".nup" : "folder";
                // Why Prepare cannot start from it (another build than this app's, an incomplete dump), plainly; a pack made elsewhere can still be picked
                String why = k.found != null && k.found.error != null ? k.found.error
                        : "No pack found for this " + what + ", and it is not a dump Prepare can start from.";
                Ui.explain(this, "No pack for this " + what, why + "\n\nIf you have a pack folder made for exactly this " + what + " (with pack.tsv), you can pick it.",
                                k.found != null ? k.found.detail : "")
                        .setPositiveButton("OK", null)
                        .setNeutralButton("Pick a pack folder", (d, w) -> pickFolder(PICK_PACK, "Pick the pack folder (android-pack, with pack.tsv)")).show();
            });
            return;
        }
        List<String> ok = new ArrayList<>(), unsupported = new ArrayList<>(), prepare = new ArrayList<>(), noPack = new ArrayList<>(), failed = new ArrayList<>(), twice = new ArrayList<>();
        for (Pick k : picks) {
            if (k.unsupported) { unsupported.add(k.game.getName() + " (" + k.id + ")"); continue; }
            if (k.error != null) { failed.add(k.error); continue; }
            if (k.pack != null) {
                String err = add(k.game, k.id, k.pack.dir, null);
                if (err == null) ok.add(k.pack.pack.name.isEmpty() ? k.name : k.pack.pack.name);
                else failed.add(k.game.getName() + ": " + err);
            } else if (k.dump) {
                cardFor(k.game, k.id);
                prepare.add(k.name);
            } else noPack.add(k.game.getName());
            if (!k.others.isEmpty() && (k.pack != null || k.dump)) twice.add(k.game.getName());
        }
        refresh();
        StringBuilder m = new StringBuilder();
        if (!ok.isEmpty()) m.append("Added: ").append(TextUtils.join(", ", ok)).append(".");
        if (!prepare.isEmpty()) m.append(m.length() > 0 ? "\n\n" : "").append("Needs Prepare: ").append(TextUtils.join(", ", prepare))
                .append(". No pack that fits was found: its card's Prepare makes one on this phone (one game at a time).");
        if (!unsupported.isEmpty()) m.append(m.length() > 0 ? "\n\n" : "").append("Not in this app version: ").append(TextUtils.join(", ", unsupported))
                .append(". Install a build that includes them (Settings › About lists the games this one has).");
        if (!noPack.isEmpty()) m.append(m.length() > 0 ? "\n\n" : "").append("No pack found: ").append(TextUtils.join(", ", noPack))
                .append(". Add it on its own to pick its pack folder.");
        if (!failed.isEmpty()) m.append(m.length() > 0 ? "\n\n" : "").append("Not added: ").append(TextUtils.join(" ", failed));
        if (!twice.isEmpty()) m.append(m.length() > 0 ? "\n\n" : "").append("Another dump of the same game was on the list already, so both cards are kept: ")
                .append(TextUtils.join(", ", twice)).append(". ⋯ › Remove from list takes one away (nothing is deleted).");
        android.util.Log.i("nxnative", "Add game: " + m.toString().replace("\n\n", " | "));
        new AlertDialog.Builder(this).setTitle(ok.isEmpty() && prepare.isEmpty() ? "No game added" : "Games added").setMessage(m).setPositiveButton("OK", null).show();
    }

    /**
     * Another dump of a game that is on the list (another file, another build or one that cannot be told): asks whether this one replaces it (its list entry
     * only; nothing is deleted) or both are kept. then gets the entries' game paths to replace, or null to keep them; nothing to ask: null at once
     */
    private void askReplace(String name, File game, List<Game> others, java.util.function.Consumer<List<String>> then) {
        if (others.isEmpty()) { then.accept(null); return; }
        List<String> dirs = new ArrayList<>();
        StringBuilder s = new StringBuilder();
        for (Game o : others) { dirs.add(o.gameDir); s.append("\n• ").append(o.gameDir); }
        new AlertDialog.Builder(this).setTitle("Replace " + name + "?")
                .setMessage("Another dump of " + name + " is on your list:" + s + "\n\nUse " + game.getName() + " instead? Only the list entry changes: no dump, pack or save is deleted.")
                .setPositiveButton("Replace", (d, w) -> then.accept(dirs))
                .setNeutralButton("Keep both", (d, w) -> then.accept(null))
                .setNegativeButton("Cancel", null).show();
    }

    /** These game paths' list entries go (exact paths, as the list keeps them); nothing on the storage changes */
    private void removeEntries(List<String> gameDirs) {
        synchronized (Game.class) {
            List<Game> list = Game.load(this);
            list.removeIf(x -> gameDirs.contains(x.gameDir));
            Game.save(this, list);
        }
        refresh();
    }

    /**
     * The list entry of a game, made when it has none: a game that still needs its pack gets a card whose pack folder is the app's packs/&lt;id&gt;, where Prepare
     * and Import pack… put it (the card says "Needs preparing" and its button prepares). An existing entry of the same game (the same file, or the same title and
     * build: GameList.matches) is kept as it is
     */
    Game cardFor(File game, String id) {
        List<Game> list = Game.load(this);
        List<Integer> same = GameList.matches(list, id, game.getPath(), Game.key(game.getPath(), id), Game::key);
        if (!same.isEmpty()) return list.get(same.get(0));
        Profile p = Profile.of(id);
        Game g = Game.put(this, new Game(id, p == null ? id : p.name, game.getPath(), AppStorage.pack(this, id).getPath()));
        refresh();
        return g;
    }

    /** Prepare's "Use an existing pack…": the picked pack folder becomes the game's (addWithPack; remembered for this title like a pack picked in Folders) */
    void pickPackFor(File game, String id) {
        pendingGame = game;
        pendingId = id;
        pickFolder(PICK_PACK, "Pick the pack folder (the one with pack.tsv and shaders/)");
    }

    private boolean addWithPack(File game, String id, File packDir) { return addWithPack(game, id, packDir, null); }

    private boolean addWithPack(File game, String id, File packDir, List<String> replace) {
        String err = add(game, id, packDir, replace);
        if (err != null) { error(err); return false; }
        refresh();
        if (packDir.equals(pendingPackPick)) Folders.pickedPack(this, id, packDir.getPath());   // a pack folder the user picked is remembered for this game's next pick and updates
        String note = "";
        String lp = Pack.libraryProblem(packDir, id);
        if (lp != null) note += " " + lp;
        String name;
        try { name = Pack.read(packDir).name; } catch (IOException e) { name = id; }
        Toast.makeText(this, "Added " + (name.isEmpty() ? id : name) + "." + note, Toast.LENGTH_LONG).show();
        return true;
    }

    /**
     * The game into the list with this pack folder (Game.put: replacing the entries of the same file or the same title and build, and those in replace, which
     * the user chose to replace); null when added, else why not in a sentence
     */
    private String add(File game, String id, File packDir, List<String> replace) {
        Pack pack;
        try { pack = Pack.read(packDir); } catch (IOException e) { return e.getMessage(); }
        if (!pack.titleId.equals(id)) return "The pack is for " + pack.titleId + ", the game " + (Nup.isNup(game) ? ".nup" : "folder") + " holds " + id + ".";
        String why = Profile.missing(id, pack.name.isEmpty() ? null : pack.name);
        if (why != null) return why;
        if (Nup.isNup(game)) {
            String romfs;
            try { romfs = pack.romfsPath(game); } catch (IOException e) { return game.getName() + " cannot be used: " + e.getMessage(); }
            if (romfs == null) return game.getName() + " has no " + pack.romfsSelector() + " with a RomFS (the pack is for version " + pack.version + ").";
        } else if (pack.romfs(game) == null) return "No RomFS in " + game + " (looked for " + pack.romfsRel + ", romfs/ and updates/<version>/romfs).";
        Game.put(this, new Game(id, pack.name.isEmpty() ? id : pack.name, game.getPath(), packDir.getPath()), replace);
        return null;
    }

    /** Remove from the list (the Manage sheet): says exactly what goes (the list entry) and what stays (the folders and the saves) */
    private void confirmRemove(Game g) {
        File save = Folders.save(this, g);
        if (CopyPlan.isCopy(AppStorage.root(this), g.gameDir)) { confirmRemoveCopy(g, save); return; }
        Ui.confirmDanger(this, "Remove " + g.name + "?",
                "Only the list entry goes. Your dump, its pack and your saves" + (save != null ? " (" + save.getPath() + ")" : "") + " stay on the phone; Add game brings it back.",
                "Remove", () -> {
                    List<Game> list = Game.load(this);
                    list.removeIf(x -> x.gameDir.equals(g.gameDir));
                    Game.save(this, list);
                    PickedNup.forget(this, new File(g.gameDir));   // a .nup used where it is: its read permission goes back (another entry of it keeps it)
                    refresh();
                });
    }

    /** A game copied into the app: Remove can take the copy too (it is the app's own, and only Remove can free its space short of uninstalling) */
    private void confirmRemoveCopy(Game g, File save) {
        Runnable drop = () -> { List<Game> list = Game.load(this); list.removeIf(x -> x.gameDir.equals(g.gameDir)); Game.save(this, list); refresh(); };
        new AlertDialog.Builder(this).setTitle("Remove " + g.name + "?")
                .setMessage("This game is a copy in the app's storage (" + g.gameDir + "). Remove the list entry only, or delete the copy as well (it cannot be undone; "
                        + "your original dump, the pack and your saves" + (save != null ? " (" + save.getPath() + ")" : "") + " stay).")
                .setPositiveButton("Remove only", (d, w) -> drop.run())
                .setNeutralButton("Delete the copy", (d, w) -> Ui.confirmDanger(this, "Delete the copy?", "Deleted: " + g.gameDir + ". This cannot be undone.", "Delete", () -> {
                    drop.run();
                    if (!GameCopy.deleteCopy(this, new File(g.gameDir))) error("Not all of " + g.gameDir + " could be deleted.");
                }))
                .setNegativeButton("Cancel", null).show();
    }

    void launch(Game card) {
        if (needAccess(new File(card.gameDir))) return;
        Game g = Folders.repack(this, card);   // a remembered pack folder that is gone: the usual search instead
        File saveGone = Folders.goneSave(this, g);
        if (saveGone != null) { Folders.saveGoneDialog(this, g, saveGone); return; }
        String gone = gone(g);
        if (gone != null) {
            Ui.explain(this, "Game file gone", GameFile.goneText(new File(g.gameDir).getName()), gone)
                    .setPositiveButton("Pick it again", (d, w) -> relink(g)).setNeutralButton("Remove", (d, w) -> confirmRemove(g)).setNegativeButton("Cancel", null).show();
            return;
        }
        String why = Profile.missing(g.titleId, g.name);
        if (why != null) { error(why); return; }
        if (SaveUi.busy(g.titleId)) { error("A save backup or restore of " + g.name + " is still working: Play waits for it."); return; }
        if (PackUi.busy(g.titleId)) { error("An export or import of " + g.name + "'s pack is still working: Play waits for it."); return; }
        File gd = new File(g.gameDir);
        boolean dump = Ondevice.isDump(gd);
        Runnable pickPack = () -> { pendingGame = gd; pendingId = g.titleId; pickFolder(PICK_PACK, "Pick the pack folder (the one with pack.tsv and shaders/)"); };
        Pack p;
        try {
            p = Pack.read(new File(g.packDir));
        } catch (IOException e) {
            if (dump) packProblem(g, "This game has no pack that can be used yet.", e.getMessage(), pickPack);
            else Ui.explain(this, "No pack for this game", "This game has no pack that can be used. ⋯ › Folders picks one.", e.getMessage() + "\nPack: " + g.packDir)
                    .setPositiveButton("OK", null).show();
            return;
        }
        // The pack's code must be of the game build in the folder (else the game crashes at once): a plain message, and Prepare again from the dump, instead of a
        // start that cannot work. A build this app has not been tested with (a pack made with Try anyway, or elsewhere) or another version's RomFS can still start:
        // Play anyway, after a warning
        String bad = PackCheck.blocking(this, p, gd);
        if (bad != null) {
            BuildCheck.Result b = BuildCheck.build(p, gd);
            boolean untested = p.unfinishedModule() == null && PackCheck.rewriting(this, p.dir, gd, p.titleId) == null
                    && (b.kind == BuildCheck.Kind.UNSUPPORTED || b.kind == BuildCheck.Kind.ROMFS);
            if (!untested) {
                int cut = bad.indexOf("\n\n");   // BuildCheck: the build IDs that differ follow the sentence
                packProblem(g, cut > 0 ? bad.substring(0, cut) : bad, cut > 0 ? bad.substring(cut + 2) : "", pickPack);
                return;
            }
            if (!playAnyway.contains(g.gameDir)) {
                Ui.explain(this, "Untested game version", b.message + "\n\n" + PrepareText.ANYWAY_WARNING, b.detail + "\nPack: " + g.packDir)
                        .setPositiveButton("Play anyway", (d, w) -> { playAnyway.add(g.gameDir); launch(g); })
                        .setNegativeButton("Cancel", null).show();
                return;
            }
            android.util.Log.w("nxnative", g.titleId + ": Play anyway: " + b.kind + ": " + b.detail);
        }
        // The host loads <pack>/shaders/v-<its DMSH_VK_CHAIN>/dmsh_vk.lib: without it the game runs but draws nothing. Say so, and offer to prepare the shaders
        // (a dump) or another pack folder
        String lp = Pack.libraryProblem(new File(g.packDir), g.titleId);
        if (lp != null && Pack.playableLibrary(new File(g.packDir), g.titleId) != null) {   // the older library plays while the update makes this version's (vka_shader.c lib_load)
            if (dump) Ondevice.autoUpdate(this, gd, new File(g.packDir), g.titleId);
            start(g);
            return;
        }
        if (lp != null && Pack.legacyLibrary(new File(g.packDir), g.titleId) != null) {   // made before versioned updates: never blocks Play, says so (the host warns in its log too)
            if (dump) Ondevice.autoUpdate(this, gd, new File(g.packDir), g.titleId);
            Toast.makeText(this, ShaderLibs.LEGACY_WARNING, Toast.LENGTH_LONG).show();
            start(g);
            return;
        }
        if (lp != null) {
            AlertDialog.Builder b = new AlertDialog.Builder(this).setTitle("Shaders missing")
                    .setMessage((dump ? "Prepare " + Ondevice.shadersText(new File(g.packDir), g.titleId).replaceFirst("^Updates", "updates").replaceFirst("^Makes", "makes") : "Without them the game draws nothing.")
                            + "\n\n" + lp)
                    .setNeutralButton("Pick pack folder", (d, w) -> pickPack.run()).setNegativeButton("Cancel", null);
            if (dump) b.setPositiveButton("Prepare", (d, w) -> Ondevice.prepare(this, gd, false));
            else b.setPositiveButton("Start anyway", (d, w) -> start(g));
            b.show();
            return;
        }
        start(g);
    }

    /** The games Play anyway was chosen for in this process (an untested game version): asked once per launcher start, not at every Play */
    private static final java.util.Set<String> playAnyway = new java.util.HashSet<>();

    /** A pack that cannot run with this game folder: why (its technical part in Details), then Prepare again from the dump (the default), or another pack folder */
    private void packProblem(Game g, String why, String detail, Runnable pickPack) {
        File gd = new File(g.gameDir);
        boolean dump = Ondevice.isDump(gd);
        AlertDialog.Builder b = Ui.explain(this, "The pack cannot start this game", why + (dump ? "\n\nPrepare again makes a pack from your dump, on this phone." : ""),
                        (detail.isEmpty() ? "" : detail + "\n") + "Pack: " + g.packDir)
                .setNegativeButton("Cancel", null).setNeutralButton("Pick pack folder", (d, w) -> pickPack.run());
        if (dump) b.setPositiveButton("Prepare again", (d, w) -> Ondevice.prepare(this, gd, false));
        b.show();
    }

    private void start(Game g) {
        Game.touch(this, g);   // the entry played last is the one kept if the list ever holds this game twice
        Intent intent = new Intent(this, GameActivity.class).putExtra("title_id", g.titleId).putExtra("name", g.name).putExtra("game", g.gameDir).putExtra("pack", g.packDir);
        if (getIntent().getBooleanExtra("no_pause", false)) intent.putExtra("no_pause", true);
        startActivity(intent);
    }

    /** The overlay's master switch and its items (Overlay.rows); the main screen's switch follows the master switch */
    private void overlayItems() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        Overlay.rows(this, box, null);
        Ui.sheet(this, "Overlay", box).setPositiveButton("Done", null).setOnDismissListener(d -> refreshSettings()).show();
    }

    private String resLabel() { return Prefs.windowName(Prefs.window(this), true); }

    /** Game resolution: the window size the host gives the game (DM_WINDOW), from the next start */
    private void gameRes() {
        int i = Math.max(0, java.util.Arrays.asList(Prefs.WINDOWS).indexOf(Prefs.window(this)));
        new AlertDialog.Builder(this).setTitle("Game resolution").setSingleChoiceItems(Prefs.WINDOW_NAMES, i, (d, w) -> { Prefs.window(this, Prefs.WINDOWS[w]); refreshSettings(); d.dismiss(); })
                .setNegativeButton("Cancel", null).show();
    }

    static String capLabel(int v) { return v < 0 ? "Default" : v == 0 ? "The game's own" : v + " fps"; }

    /** The frame rate cap the next start uses (Prefs.fpsCap, DM_FPS_TARGET); the in-game panel changes it while the game runs */
    private void fpsCap() {
        int cur = Prefs.fpsCap(this), n = Prefs.FPS_CAPS.length;
        String[] labels = new String[n + 1];
        labels[0] = capLabel(-1);
        int checked = 0;
        for (int k = 0; k < n; k++) {
            int v = Prefs.FPS_CAPS[k];
            labels[k + 1] = capLabel(v);
            if (v == cur) checked = k + 1;
        }
        new AlertDialog.Builder(this).setTitle("Frame rate cap")
                .setSingleChoiceItems(labels, checked, (d, w) -> { Prefs.fpsCap(this, w == 0 ? -1 : Prefs.FPS_CAPS[w - 1]); refreshSettings(); d.dismiss(); })
                .setNegativeButton("Cancel", null).show();
    }

    /** Frame generation's settings and the Lossless.dll found (or a manual pick when it is in none of the known places) */
    private void frameGen() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        File dll = Lsfg.find(this);
        Lsfg.rows(this, box, Lsfg.note + (dll != null ? "" : ". Put it in Documents/Emulation/bios or Switch2, or pick it."), () -> {});
        Ui.add(box, Ui.small(this, "Ported from Eden (GPL-3.0-or-later). Lossless.dll is yours; it is never part of the app."), 10);
        Ui.sheet(this, "Frame generation", box).setPositiveButton("Done", null).setOnDismissListener(d -> refreshSettings())
                .setNeutralButton("Pick Lossless.dll", (d, w) -> {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
                    startActivityForResult(i, PICK_DLL);
                }).show();
    }

    private void shareLog() { shareLog((String) null); }

    /** The newest log of a game (titleId), or of any run (null) */
    private void shareLog(String titleId) {
        File log = LogProvider.newest(this, titleId);
        if (log == null) { error(titleId == null ? "No log yet." : "No log of this game yet."); return; }
        shareLog(log);
    }

    /** One log file */
    void shareLog(File log) { shareLog(log, null); }

    /** One log file, with its digest beside it when there is one (ExitWatch's notice: the log of the run that ended and LogDigest's text of it) */
    void shareLog(File log, File digest) {
        Uri uri = LogProvider.uri(log);
        Intent send;
        if (digest == null) send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, uri);
        else {
            Uri du = LogProvider.uri(digest);
            send = new Intent(Intent.ACTION_SEND_MULTIPLE).setType("text/plain").putParcelableArrayListExtra(Intent.EXTRA_STREAM, new ArrayList<>(List.of(uri, du)));
        }
        send.putExtra(Intent.EXTRA_SUBJECT, "nxnative log " + log.getName());
        ClipData clip = ClipData.newRawUri(log.getName(), uri);
        if (digest != null) clip.addItem(new ClipData.Item(LogProvider.uri(digest)));
        send.setClipData(clip);
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(send, "Share " + log.getName()));
    }

    /** A web page (a rule's doc from ExitWatch's notice) in the phone's browser */
    void openLink(String url) {
        try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); }
        catch (android.content.ActivityNotFoundException e) { error("No app can open " + url); }
    }

    void error(String msg) { new AlertDialog.Builder(this).setMessage(msg).setPositiveButton("OK", null).show(); }

    private static String touchLabel(int m) { return m == TouchControls.MODE_AUTO ? "Auto: hidden while a controller is connected" : TouchControls.MODE_NAMES[m]; }
    /** A default's choice in Settings (no "Default" entry: this is the default) */
    private void pick(String title, String[] items, int cur, java.util.function.IntConsumer set) {
        new AlertDialog.Builder(this).setTitle(title).setSingleChoiceItems(items, cur, (d, w) -> { set.accept(w); refreshSettings(); d.dismiss(); }).setNegativeButton("Cancel", null).show();
    }
}
