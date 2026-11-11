package dev.nxnative.android;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.os.SystemClock;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.Locale;

/**
 * Preparing a game on the phone. "+ Add game" (MainActivity) on a dump without a usable pack calls {@link #prepare}: the dump is checked (DumpCheck), a pack
 * beside it is checked against it (PackCheck: game build, shader library, rewriter) and what does not fit is redone, else the whole job runs; PackService runs it,
 * and this card shows it on the main screen while there is one: its state (needs preparing, preparing N of 22,852 shaders, paused and why, stopped with an error
 * and what to do), a progress bar, Pause / Resume / Retry, and ⋯ Manage (forget, delete) away from them. When the job is done the game goes into the game list
 * with its pack and the card goes away: Play is on the game's card.
 * <p>
 * The folder is picked with the Storage Access Framework's tree picker and turned into its /storage path (PathUtil); everything after that is read by path with All
 * files access, as the host does. The SAF picker is the familiar system UI and needs no path typed; reading through the SAF (DocumentFile / ContentResolver) instead
 * would be one binder call per file for a RomFS of 158,000 files, and the tools (CPython, the translator) need POSIX paths in any case.
 * <p>
 * Progress comes from the service over a bound Messenger while it runs, otherwise from job.json (read every second while the screen shows).
 */
final class Ondevice {
    static final int PICK_DUMP = 100, ASK_NOTIFY = 101;
    private static Panel panel;

    private Ondevice() {}

    static void attach(MainActivity a, LinearLayout root, int index) {
        if (panel != null) panel.detach();
        panel = new Panel(a);
        root.addView(panel.view, index);
    }

    /** Prepare the dump (MainActivity's Add game, a game card's Prepare again): checked off the main thread, then asked before anything is written */
    static void prepare(MainActivity a, File dump, boolean again) { if (panel != null && panel.a == a) panel.picked(dump, false, null, again); }

    /** Shaders a delta update of this pack translates (the profile's manifest rows for the pack's older libraries, ShaderLibs.toTranslate), -1 when the manifest has none of them */
    static int deltaShaders(File packDir, String titleId) { return Pack.deltaShaders(packDir, titleId); }

    /** What making a pack's shaders does and takes, for Prepare's dialogs: an update of the changed ones when the manifest knows the pack's older library, else all of them */
    static String shadersText(File packDir, String titleId) {
        int n = deltaShaders(packDir, titleId);
        Profile p = Profile.of(titleId);
        if (n >= 0 && p != null) return String.format(Locale.getDefault(), "Updates the shaders from your dump: %,d of the %,d changed in this app version (%s, in the background).",
                n, p.shaderTotal, ShaderLibs.minutesText(n));
        return "Makes the shaders from your dump (" + (p != null ? ShaderLibs.minutesText(p.shaderTotal) : "about 20-35 minutes") + ", in the background).";
    }

    private static final java.util.Set<String> autoTried = new java.util.HashSet<>();

    /**
     * A shader update the user need not ask for: a dump whose pack has an older library the profile's delta manifest knows (deltaShaders) is updated in the background
     * with no question, once per app process (a failed or paused job is resumed once). The game card then shows it, and Play stays on when that library is playable
     * (Pack.playableLibrary); otherwise Play waits for it as before. Only the card's own pack is updated (card = true): never the pack Folders.packFor would pick
     * (a game added with another pack, e.g. the play app's beside the dump), never a whole job, never a job for another pack folder
     */
    static void autoUpdate(MainActivity a, File dump, File packDir, String titleId) {
        if (panel == null || panel.a != a || panel.alive() || deltaShaders(packDir, titleId) < 0 || !autoTried.add(dump.getPath())) return;
        panel.picked(dump, true, packDir, false, true);
    }

    /**
     * A code update the user need not ask for: when the pack is this app's own (ownPack) and its code key no longer matches because the app's tools changed
     * (a profile's patches.py, the rewriter), its code is rewritten in the background, as Prepare again would, keeping the shaders. Only with the dump there
     * (the card's check found no problem with it), its build one the profile knows (checked by picked: a build mismatch stops it), and no game or other job
     * running; once per dump per app process. The rewrite goes through the job's staging and swap, so a stopped update never leaves new code beside old tables
     */
    static boolean autoCodeUpdate(MainActivity a, File dump, File packDir) {
        if (panel == null || panel.a != a || panel.alive() || gameRunning(a) || !autoTried.add("code:" + dump.getPath())) return false;
        panel.picked(dump, true, packDir, true, true);
        return true;
    }

    /**
     * Whether this app made the pack, so the launcher may update its code by itself: its pack.tsv's made_by row (written by the phone's pack step), else, for a pack
     * made before that row, the job beside the dump that says it made this pack folder (job.json own_pack). Without either, the pack is treated as other tools'.
     * Reads files: call it off the main thread
     */
    static boolean ownPack(File dump, File packDir, String titleId) {
        try {
            if (Pack.read(packDir).madeByApp()) return true;
            File jf = Job.fileOf(DumpCheck.workOf(dump, titleId));
            if (!jf.isFile()) return false;
            Job j = Job.read(jf);
            return j.ownsPack() && GameList.samePath(j.pack().getPath(), packDir.getPath());
        } catch (IOException | RuntimeException e) {
            return false;
        }
    }

    /** Whether a game runs now (the :game process): the background code update waits for it, as the pack's code must not change under a running game */
    private static boolean gameRunning(Context c) {
        android.app.ActivityManager am = (android.app.ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        List<android.app.ActivityManager.RunningAppProcessInfo> ps = am == null ? null : am.getRunningAppProcesses();
        if (ps != null) for (android.app.ActivityManager.RunningAppProcessInfo p : ps) if (p.processName.endsWith(":game")) return true;
        return false;
    }

    /** Whether a job (any game's) runs now, so a background shader update of another card waits for it (autoUpdate starts none while one is alive) */
    static boolean busy(MainActivity a) { return panel != null && panel.a == a && panel.alive(); }

    /** The shader update job of this game's card (its dump and pack): the card's job when it has one, else the remembered job.json; null when none is for this game */
    static Job cardJob(MainActivity a, Game g) {
        Job j = panel != null && panel.a == a ? panel.job : null;
        if (j == null) {
            File f = Job.remembered(a);
            try { j = f != null && f.isFile() ? Job.read(f) : null; } catch (IOException e) { j = null; }
        }
        return j != null && GameList.samePath(g.gameDir, j.dump().getPath()) && GameList.samePath(g.packDir, j.pack().getPath()) ? j : null;
    }

    /** Whether a folder or .nup is a dump Prepare can start from: an update, or a base game with its code and RomFS (Pack.isDump; DumpCheck checks it fully) */
    static boolean isDump(File dir) { return Pack.isDump(dir); }
    static void resume(MainActivity a) { if (panel != null && panel.a == a) panel.resume(); }

    static void pause(MainActivity a) { if (panel != null && panel.a == a) panel.pause(); }

    /** Whether Home shows the preparing card now */
    static boolean showing() { return panel != null && panel.view.getVisibility() == View.VISIBLE; }
    /** The progress card when it shows (MainActivity puts it into the game list: at its game's place, or first for a game not listed yet), else null */
    static View cardView() { return panel != null && panel.view.getVisibility() == View.VISIBLE ? panel.view : null; }
    /** The job's own Manage sheet (Forget, the deletes), from the game's ⋯ while its card is the progress card */
    static void manageJob(MainActivity a) { if (panel != null && panel.a == a) panel.manage(); }
    /** Whether the progress card shows this game (its dump and pack): its card in the list is then the progress card, one card per game */
    static boolean shows(Game g) {
        Job j = panel != null && panel.view.getVisibility() == View.VISIBLE ? panel.job : null;
        return j != null && GameList.samePath(g.gameDir, j.dump().getPath()) && GameList.samePath(g.packDir, j.pack().getPath());
    }

    static boolean activityResult(MainActivity a, int request, int result, Intent data) {
        if (request != PICK_DUMP) return false;
        if (result != MainActivity.RESULT_OK || data == null || data.getData() == null || panel == null || panel.a != a) return true;
        String path = PathUtil.treeToPath(data.getData());
        if (path == null) a.error("That folder is not on this phone's storage (" + data.getData() + ").");
        else panel.picked(new File(path), false, null);
        return true;
    }

    /** adb: am start -n dev.nxnative.android/.MainActivity (play app: dev.nxnative.android.play/dev.nxnative.android.MainActivity) --es ondevice_dump <dump folder> [--ez ondevice_start true] [--ez ondevice_pause true] [--ez ondevice_go_on true]
     *  [--es ondevice_pack <pack folder>] [--ez show_when_locked true] (TESTING.md). ondevice_pack: the job writes its pack there instead of the app's
     *  packs/<id> (AppStorage.pack), and a pack already there or beside the dump is not adopted (agents' test runs: <work>/ondevice/test-pack, so the user's pack is never written) */
    static void fromIntent(MainActivity a, Intent i) {
        if (panel != null && panel.a != a) return;   // a panel of an activity gone (the launcher was recreated on its Settings page)
        // Tests over adb on a locked phone: the launcher shows over the lock screen (it stays locked; only this screen shows), so its screen can be checked and captured
        if (i.getBooleanExtra("show_when_locked", false)) { a.setShowWhenLocked(true); a.setTurnScreenOn(true); a.getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); }
        String dump = i.getStringExtra("ondevice_dump");
        String pack = i.getStringExtra("ondevice_pack");
        if (dump != null && panel != null) panel.picked(new File(dump), i.getBooleanExtra("ondevice_start", false), pack == null ? null : new File(pack));
        if (i.getBooleanExtra("ondevice_pause", false) && panel != null) panel.pauseJob("adb");
        if (i.getBooleanExtra("ondevice_go_on", false) && panel != null) panel.goOnAnyway("adb");
    }

    private static final class Panel implements ServiceConnection {
        final MainActivity a;
        final LinearLayout view, card;
        final TextView head, title, status, detail, tile;
        final ProgressBar bar;
        final Button go, goOn, anyway, update;
        final Ui.Details why;   // a failed job's technical part (the tool's error, the folders, the logs), closed by default
        final Ui.Action play;
        final View more;
        /** The job (file and creation time) whose game this app put into the list ("added" in the ondevice prefs): once per job, so a game the user removed stays removed */
        static String addedKey(Job j) { return j.file.getPath() + "@" + j.created(); }
        final Handler h = new Handler(Looper.getMainLooper());
        final Messenger client = new Messenger(new Handler(Looper.getMainLooper(), this::onMessage));
        Messenger service;
        boolean bound, resumed;
        long resumedAt;
        Job job;
        private String placedFor;   // which job (dump and pack) the list was last built for
        private String metaLine;   // the game's version and publisher from its .nup, shown before the state
        boolean jobAlive;   // the service said its worker runs this job

        Panel(MainActivity a) {
            this.a = a;
            // A card while there is a job: its heading, a line on what preparing is, then the job in a soft inner card: its name and ⋯ Manage at the top right,
            // the state, the progress, then Pause / Resume / Retry and Play. Forget and the deletes are in Manage, never beside Play. No job, no card: "+ Add game"
            // is where preparing starts
            view = Ui.card(a);
            card = view;   // one card: the heading and ⋯ Manage, the state, the progress, the buttons
            LinearLayout top = Ui.row(a);
            top.setGravity(android.view.Gravity.TOP);
            // laid out as a game card (MainActivity.gameCard): the game's tile, its name and state line, ⋯; MainActivity puts it into the game list
            tile = Ui.tile(a, "", 52);
            top.addView(tile);
            head = Ui.text(a, "", 17, R.color.bn_text, 700);
            head.setMaxLines(2);
            head.setEllipsize(android.text.TextUtils.TruncateAt.END);
            title = Ui.small(a, "");
            LinearLayout tc = new LinearLayout(a);   // in a column of its own, as the game cards: a bare TextView with weight kept its first (one-line) height
            tc.setOrientation(LinearLayout.VERTICAL);
            tc.setPadding(dp(14), 0, dp(4), 0);
            tc.addView(head);
            tc.addView(title);
            top.addView(tc, Ui.weight());
            // ⋯: a listed game's own Manage (Game settings, saves, packs ... and Preparing, which leads here: manageJob); a game not listed yet: the job's
            more = Ui.more(a, "Manage", v -> { Game g = job != null ? listedGame(job) : null; if (g != null) a.manageGame(g); else manage(); });
            LinearLayout.LayoutParams mp = (LinearLayout.LayoutParams) more.getLayoutParams();
            mp.leftMargin = dp(8);
            top.addView(more, mp);
            card.addView(top);
            status = Ui.body(a, "");
            status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);   // what the job does, said when it changes (step, pause, stop)
            Ui.add(card, status, 10);
            bar = Ui.progress(a);
            bar.setMax(1000);
            Ui.add(card, bar, 10);
            detail = Ui.small(a, "");
            Ui.add(card, detail, 8);
            why = Ui.details(a, "");
            Ui.add(card, why, 4);
            go = Ui.button(a, "", Ui.Kind.TONAL, v -> goOrPause());
            go.setPadding(dp(12), 0, dp(12), 0);   // "Prepare again" fits half a narrow card
            go.setMinHeight(dp(56));
            go.setMinimumHeight(dp(56));
            play = Ui.action(a, R.drawable.ic_play, "Play", Ui.Kind.PRIMARY, v -> play());
            Ui.add(card, Ui.pair(a, go, play), 16);
            // a heat pause (PackPolicy): the phone may report heat it does not have (a thermal HAL that says SEVERE on a cool handheld, GitHub #38)
            goOn = Ui.button(a, "Go on anyway", Ui.Kind.SOFT, v -> goOnAnyway("launcher"));
            goOn.setVisibility(View.GONE);
            Ui.add(card, goOn, 8);
            // a failed job whose check can be relaxed (PrepareText.FIX_ANYWAY): asked first, with what may go wrong
            anyway = Ui.button(a, "Try anyway…", Ui.Kind.SOFT, v -> tryAnyway());
            anyway.setVisibility(View.GONE);
            Ui.add(card, anyway, 8);
            // a failed job that this app build cannot finish (PrepareText.FIX_UPDATE): the release app's updater, where it has one
            update = Ui.button(a, "Check for updates", Ui.Kind.SOFT, v -> Updater.checkNow(a));
            update.setVisibility(View.GONE);
            Ui.add(card, update, 8);
            load();
            show();
        }

        int dp(int v) { return Math.round(v * a.getResources().getDisplayMetrics().density); }

        void detach() {
            keepScreen(false);
            pause();
        }

        /** Settings › Keep the screen on while preparing: the window's FLAG_KEEP_SCREEN_ON while the job runs (or waits to go on by itself) and Home shows its card */
        private boolean screenKept;
        void keepScreen(boolean on) {
            if (on == screenKept) return;
            screenKept = on;
            if (on) a.getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else a.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            android.util.Log.i("nxpack", "keep screen on while preparing: " + (on ? "on" : "off"));
        }

        void resume() {
            resumed = true;
            resumedAt = SystemClock.elapsedRealtime();
            if (!bound) bound = a.bindService(new Intent(a, PackService.class), this, 0);   // 0: only while the service runs; never starts it
            tick();
        }

        void pause() {
            resumed = false;
            h.removeCallbacksAndMessages(null);
            if (bound) {
                if (service != null) {
                    Message m = Message.obtain(null, PackService.MSG_UNREGISTER);
                    m.replyTo = client;
                    try { service.send(m); } catch (RemoteException e) { /* gone */ }
                }
                a.unbindService(this);
                bound = false;
                service = null;
                jobAlive = false;
            }
        }

        /** Every second while shown: job.json when the service is not telling us */
        void tick() {
            if (!resumed) return;
            if (service == null || !jobAlive) load();
            show();
            h.postDelayed(this::tick, 1000);
        }

        void load() {
            File f = Job.remembered(a);
            if (f == null || !f.isFile()) { job = null; return; }
            try {
                job = Job.read(f);
            } catch (IOException e) {
                // being renamed over, or damaged: keep the last one
            }
        }

        @Override public void onServiceConnected(ComponentName n, IBinder b) {
            service = new Messenger(b);
            Message m = Message.obtain(null, PackService.MSG_REGISTER);
            m.replyTo = client;
            try { service.send(m); } catch (RemoteException e) { service = null; }
        }

        @Override public void onServiceDisconnected(ComponentName n) {   // the :pack process died
            service = null;
            jobAlive = false;
            show();
        }

        @Override public void onBindingDied(ComponentName n) {
            service = null;
            jobAlive = false;
            if (bound) { a.unbindService(this); bound = false; }
            if (resumed) bound = a.bindService(new Intent(a, PackService.class), this, 0);
        }

        boolean onMessage(Message m) {
            if (m.what != PackService.MSG_STATE) return false;
            Bundle d = m.getData();
            Job j = Job.of(new File(d.getString("file", "")), d.getString("job", "{}"));
            if (j != null) job = j;
            jobAlive = d.getBoolean("alive");
            show();
            return true;
        }

        /** Whether the service's worker is running the job now (or may be: just resumed, the binding not yet answered) */
        boolean alive() { return jobAlive || (service == null && SystemClock.elapsedRealtime() - resumedAt < 2500 && job != null && "running".equals(job.state())); }

        void show() {
            boolean alive = job != null && alive();
            boolean ready = job != null && ready(job);
            // A finished job: its game goes into the list with its pack (once), and the card goes: Play is on the game's card
            if (ready && !alive && !addedKey(job).equals(Prefs.open(a, "ondevice").getString("added", ""))) addGame(job);
            boolean was = view.getVisibility() == View.VISIBLE;
            if (job == null || ready && listed(job)) { keepScreen(false); view.setVisibility(View.GONE); placedFor = null; if (was) a.refresh(); return; }
            view.setVisibility(View.VISIBLE);
            // The list puts this card where its game's card would be. When the card switches to another game's job, the list has to be rebuilt too.
            // That happens after an APK update: the card first shows another game's finished job, then a background shader update takes it over,
            // and without a rebuild the card stays in the first game's spot, hiding that game's card while the updated game shows twice
            String key = job.dump().getPath() + "\n" + job.pack().getPath();
            if (!was || !key.equals(placedFor)) {
                placedFor = key;
                a.refresh();   // this also hides the "no games yet" card while this one shows
                Ui.tileIcon(tile, null);   // like the game card: the icon from the .nup if it has one, and its version and publisher before the state
                metaLine = null;
                GameMeta.load(job.dump(), m -> {
                    if (!key.equals(placedFor)) return;
                    if (m.icon != null) Ui.tileIcon(tile, m.icon);
                    metaLine = m.line();
                    if (metaLine != null) show();
                });
            }
            String st = job.state();
            String sim = "fake".equals(BuildConfig.NX_PACK_ENGINE) ? "  (simulated engine)" : "";
            head.setText(job.name());
            String n = job.name().trim();
            tile.setText(n.isEmpty() ? "?" : n.substring(0, n.offsetByCodePoints(0, 1)).toUpperCase(Locale.ROOT));
            // A job of a game this app version does not include (made by an APK that had it): one sentence, nothing to press but ⋯ (Forget, the deletes)
            String notIncluded = Profile.missing(job.titleId(), job.name());
            boolean failed = "failed".equals(st);
            title.setText((metaLine != null ? metaLine + " \u00b7 " : "") + (notIncluded != null ? "Not in this app version" : "done".equals(st) ? "Prepared" : "pending".equals(st) ? "Ready to prepare" : failed ? "Could not prepare"
                    : "paused".equals(st) ? "Paused" : "Preparing") + (job.tryAnyway() ? " · tried anyway" : "") + sim);
            status.setText(notIncluded != null ? notIncluded : job.statusText(alive));
            int pct = (int) Math.round(job.overall() * 1000);
            bar.setProgress(pct);
            bar.setStateDescription((pct / 10) + " percent");
            detail.setText(job.detailText());
            bar.setVisibility(notIncluded != null ? View.GONE : View.VISIBLE);
            detail.setVisibility(notIncluded != null ? View.GONE : View.VISIBLE);
            why.set(failed || notIncluded != null ? job.errorDetail() : "");
            boolean running = alive && ("running".equals(st) || "paused".equals(st) && !"user".equals(job.reason()) && !"timeout".equals(job.reason()));
            boolean noFix = notIncluded != null || failed && PrepareText.FIX_NONE.equals(job.fix());   // nothing in this app can make it work: no Retry that cannot succeed
            boolean broken = failed && !alive && job.brokenApk(Job.installed(a));   // this app build cannot do it: no Retry until another install; an older pack may still play
            go.setText(running ? "Pause" : "done".equals(st) && !job.apkChanged() ? "Prepare again" : failed ? "Retry" : "pending".equals(st) ? "Start" : "Resume");
            go.setEnabled(!"done".equals(st) || job.apkChanged());
            go.setVisibility(noFix || broken ? View.GONE : View.VISIBLE);
            update.setVisibility(broken && Updater.available(a) ? View.VISIBLE : View.GONE);
            goOn.setVisibility(alive && job.canGoOn() ? View.VISIBLE : View.GONE);
            anyway.setVisibility(!alive && failed && !noFix && PrepareText.FIX_ANYWAY.equals(job.fix()) && !job.tryAnyway() ? View.VISIBLE : View.GONE);
            keepScreen(running && Prefs.keepScreenPreparing(a));
            play.setEnabled(ready && !running || !ready && playsOld(job));
            play.setVisibility(noFix ? View.GONE : View.VISIBLE);   // Play stays (disabled while it cannot start) so the card's layout does not jump; gone only when it never can
        }

        /** Try anyway…: the warning first, then the job goes on with its checks relaxed (Job.tryAnyway) from where it stopped */
        void tryAnyway() {
            Job j = job;
            if (j == null || alive()) return;
            new AlertDialog.Builder(a).setTitle("Try anyway?").setMessage(PrepareText.ANYWAY_WARNING)
                    .setPositiveButton("Try anyway", (d, w) -> {
                        if (a.needAccess(j.dump())) return;
                        try {
                            j.tryAnyway(true);
                            j.save();
                        } catch (IOException e) {
                            a.error("Could not save the job: " + e.getMessage());
                            return;
                        }
                        show();
                        startService(j);
                    })
                    .setNegativeButton("Cancel", null).show();
        }

        // A listed game whose shaders are being updated plays with its older library of the same ABI meanwhile (Pack.playableLibrary), as its own card did: the list
        // leaves that card out while this one shows (shows). Checked at most every 5 s: show runs every second and both read files
        private long oldAt; private boolean oldOk;
        boolean playsOld(Job j) {
            long now = android.os.SystemClock.uptimeMillis();
            if (now - oldAt > 5000) { oldAt = now; oldOk = listedGame(j) != null && Pack.playableLibrary(j.pack(), j.titleId()) != null; }
            return oldOk;
        }
        Game listedGame(Job j) {
            for (Game g : Game.load(a)) if (GameList.samePath(g.gameDir, j.dump().getPath()) && GameList.samePath(g.packDir, j.pack().getPath())) return g;
            return null;
        }

        /** Whether the list has this job's game with its pack */
        boolean listed(Job j) {
            for (Game g : Game.load(a)) if (GameList.samePath(g.gameDir, j.dump().getPath()) && GameList.samePath(g.packDir, j.pack().getPath())) return true;
            return false;
        }

        /** The finished job's game into the list (its dump and pack); a picked pack folder (Folders) follows to the new pack */
        void addGame(Job j) {
            Prefs.open(a, "ondevice").edit().putString("added", addedKey(j)).apply();
            if (listed(j)) return;
            Pack p;
            try { p = Pack.read(j.pack()); } catch (IOException e) { return; }
            if (p.simulated) return;
            Game g = new Game(j.titleId(), p.name.isEmpty() ? j.name() : p.name, j.dump().getPath(), j.pack().getPath());
            Game.put(a, g);   // one card per game: replaces the entries of the same file or the same title and build (GameList)
            String picked = Folders.pickedPack(a, g.titleId);
            if (picked != null && !picked.equals(g.packDir)) Folders.pickedPack(a, g.titleId, g.packDir);
            a.refresh();
            long[] miss = j.shadersMissing();   // Try anyway on a game version the profile was not made from: say briefly what it lacks and which version has it all
            Profile prof = Profile.of(j.titleId());
            android.widget.Toast.makeText(a, g.name + " is ready to play." + (miss[0] > 0 && prof != null ? " " + PrepareText.missingShadersText(miss[0], miss[1], prof.supportedShort()) : ""),
                    android.widget.Toast.LENGTH_LONG).show();
        }

        /** Play needs the pack (Pack.read: pack.tsv and its modules) and its shader library of this APK's version */
        boolean ready(Job j) {
            if (!"done".equals(j.state()) || j.apkChanged()) return false;
            try {
                Pack p = Pack.read(j.pack());
                return Pack.libraryProblem(j.pack(), j.titleId()) == null && p.unfinishedModule() == null && p.titleId.equals(j.titleId());
            } catch (IOException e) {
                return false;
            }
        }

        /** What a background check found beside a picked dump */
        static final class Found {
            DumpCheck.Result r;
            File existing;   // a job.json beside the dump
            File packDir;    // the pack Folders.packFor finds for it (null: none)
            Pack pack;       // that pack, when it reads as this game's pack
            String build;    // PackCheck.blocking: why its code cannot run with this dump (unfinished, another game build), null when it fits
            Boolean code;    // PackCheck.code: its code is this APK's rewriter's (null: cannot tell)
        }

        /**
         * A picked dump: checked off the main thread (the dump, the pack beside it), then the job: a job in progress for it is resumed; a pack that fits is used
         * as it is; a pack that fits in part has only the rest redone; else the whole job. again: the user asked to prepare again (a game card's Manage): a pack
         * whose code another rewriter made has its code redone too
         */
        void picked(File dump, boolean start, File packOverride) { picked(dump, start, packOverride, false); }

        void picked(File dump, boolean start, File packOverride, boolean again) { picked(dump, start, packOverride, again, false); }

        /** card: an automatic update of a game card's pack (autoUpdate): packOverride is that pack, checked as the pack to keep; nothing else is started or asked */
        void picked(File dump, boolean start, File packOverride, boolean again, boolean card) { picked(dump, start, packOverride, again, card, false); }

        /** anyBuild: Try anyway for a dump of a build this app does not list (asked first: untested); a job beside the dump that was tried anyway keeps it */
        void picked(File dump, boolean start, File packOverride, boolean again, boolean card, boolean anyBuild) {
            if (a.needAccess(dump)) return;
            android.widget.Toast.makeText(a, "Checking " + dump.getName() + "…", android.widget.Toast.LENGTH_SHORT).show();
            new Thread(() -> {
                Found f = new Found();
                boolean any = anyBuild;
                try {
                    String id = Pack.titleIdOf(dump);   // a .nup, a folder's base, or an update-only dump's newest update
                    File jf = Job.fileOf(DumpCheck.workOf(dump, id));
                    if (jf.isFile()) {
                        f.existing = jf;
                        try { any |= Job.read(jf).tryAnyway(); } catch (IOException e) { /* another format: a new job replaces it */ }
                    }
                } catch (IOException e) {
                    // DumpCheck says what is wrong
                }
                f.r = DumpCheck.check(dump, f.existing == null, any);
                if (f.r.ok() && (packOverride == null || card)) {
                    f.packDir = card ? packOverride : Folders.packFor(a, f.r.dump, f.r.titleId);
                    try {
                        Pack p = f.packDir == null ? null : Pack.read(f.packDir);
                        if (p != null && !p.simulated && p.titleId.equals(f.r.titleId)) {
                            f.pack = p;
                            f.build = PackCheck.blocking(a, p, f.r.dump);
                            if (f.build == null) f.code = PackCheck.code(a, p, f.r.dump);
                        }
                    } catch (IOException e) {
                        // no usable pack: the whole job
                    }
                }
                h.post(() -> checked(f, start, packOverride, again, card));
            }, "nxpack-check").start();
        }

        void checked(Found f, boolean start, File packOverride, boolean again, boolean card) {
            DumpCheck.Result r = f.r;
            if (!r.ok()) {
                if (card) return;
                if (r.otherBuild) untested(r, start, packOverride, again);   // a build this app does not list: say so, and offer Try anyway
                else Ui.explain(a, "Cannot prepare this dump", r.error, r.detail).setPositiveButton("OK", null).show();
                show();
                return;
            }
            // A job in progress for this dump is resumed
            if (f.existing != null) {
                try {
                    Job j = Job.read(f.existing);
                    boolean same = j.dump().getPath().equals(r.dump.getPath()) && j.version() == r.version && (packOverride == null || packOverride.equals(j.pack()));
                    String s = j.state();
                    if (same && (s.equals("running") || s.equals("paused") || s.equals("failed") || s.equals("pending"))) {
                        Job.remember(a, f.existing);
                        job = j;
                        show();
                        if (start) startService(j);
                        else confirm("Resume preparing?", j.statusText(false) + "\n\nIt goes on from where it stopped.", "Resume", () -> startService(j));
                        return;
                    }
                    if (!s.equals("done") && !s.isEmpty()) {   // unfinished, for another pack folder: never written over (its job.json is this dump's)
                        if (card) return;   // an automatic update leaves it to the user
                        Ui.explain(a, "Preparing is not finished", "This dump is already being prepared into another pack folder. Resume it from its card, or delete its "
                                + "working files (⋯ › Delete working files) before preparing into another folder.", j.statusText(false) + "\n\n" + j.errorDetail())
                                .setPositiveButton("OK", null).show();
                        Job.remember(a, f.existing);
                        job = j;
                        show();
                        return;
                    }
                } catch (IOException e) {
                    // a damaged job.json or one of another format: a new job replaces it
                }
            }
            // One job at a time: the service runs only the job it has, so a new one is not written or remembered while it runs
            if (alive() && job != null && !"done".equals(job.state())) {
                a.error(job.name() + " is being prepared now (" + job.statusText(true) + "). Pause it first, then try again.");
                return;
            }
            if (card && (f.pack == null || f.build != null)) return;   // an automatic update only updates the shaders of the card's own pack, when its code fits
            // A pack already there (the picked pack folder, else beside the dump): what fits is kept
            if (f.pack != null) {
                File pd = f.packDir;
                String lp = PackCheck.shaders(pd, r.titleId);
                boolean code = f.build != null || again && !Boolean.TRUE.equals(f.code), shaders = lp != null;
                if (!code && !shaders) { adopt(r, pd, false, false); return; }
                Runnable redo = () -> adopt(r, pd, code, shaders);
                if (start) { redo.run(); return; }
                int delta = shaders ? deltaShaders(pd, r.titleId) : -1;
                String what = code && shaders ? (delta >= 0 ? "Rewrites the game code (a few minutes). " + shadersText(pd, r.titleId)
                                : "Makes the game code and the shaders again from your dump (" + ShaderLibs.jobMinutesText(Profile.of(r.titleId).shaderTotal) + ", in the background).")
                        : code ? "Rewrites the game code from your dump (a few minutes, in the background). The shaders are kept."
                        : shadersText(pd, r.titleId);
                String why = (f.build != null ? f.build + "\n\n" : again && code ? "The pack's game code was made by other tools than this app's.\n\n" : "") + (lp != null ? lp + "\n\n" : "");
                Ui.explain(a, code ? "Update the pack?" : "Prepare shaders?", what, why + "Pack: " + pd)
                        .setPositiveButton(code ? "Prepare again" : "Prepare", (d, w) -> redo.run())
                        .setNeutralButton("Pick another pack folder", (d, w) -> Folders.pickPack(a))
                        .setNegativeButton("Cancel", null).show();
                return;
            }
            Profile prof = Profile.of(r.titleId);   // DumpCheck refused a dump without one
            // what it makes, how long, and the space it needs, plainly; where the files go is in Details
            String text = String.format(Locale.getDefault(), "%s, %d DLC.\n\nThe app makes the game's pack and %,d shaders from your dump, once: %s "
                    + "in the background, best on the charger. You can pause any time.\n\nNeeds about 1.5 GB of free space (%.1f GB free now).",
                    (r.version == 0 ? "Base game" : "Update " + r.version) + (r.untested ? " (untested version: Try anyway)" : ""), r.dlc, prof.shaderTotal,
                    ShaderLibs.jobMinutesText(prof.shaderTotal), r.free / 1073741824.0);
            String where = "Dump: " + r.dump + "\nWorking files: " + r.work + "\nPack: " + AppStorage.pack(a, r.titleId)
                    + (r.build >= 0 && r.build != r.version ? "\nIts code is " + prof.buildName(r.build) : "") + (r.buildIds.isEmpty() ? "" : "\nBuild IDs: " + r.buildIds);
            Runnable go = () -> {
                try {
                    Job j = Job.create(r, AppStorage.pack(a, r.titleId));
                    if (packOverride != null) j.pack(packOverride);
                    j.save();
                    Job.remember(a, j.file);
                    job = j;
                    show();
                    startService(j);
                } catch (Exception e) {
                    a.error("Cannot start: " + e.getMessage());
                }
            };
            if (start) go.run();
            else askPrepare(r, prof, text, where, go);
        }

        /**
         * Prepare's question for a dump with no pack that fits: Prepare or Cancel, and the two ways to a pack made elsewhere: "Use an existing pack…" (the folder
         * picker; the picked folder becomes the game's, MainActivity.pickPackFor) and "Import pack…" (a .zip from Export pack… or a pack folder into the app's
         * storage, PackUi; the game gets its card first, so the import has a game to go to)
         */
        void askPrepare(DumpCheck.Result r, Profile prof, String text, String where, Runnable go) {
            LinearLayout box = new LinearLayout(a);
            box.setOrientation(LinearLayout.VERTICAL);
            Ui.add(box, Ui.body(a, text), 0);
            Ui.add(box, Ui.details(a, where), 8);
            Ui.add(box, Ui.small(a, "Have a pack made elsewhere (on a computer, or exported from another phone)?"), 16);
            AlertDialog[] d = new AlertDialog[1];
            Ui.add(box, Ui.button(a, "Use an existing pack…", Ui.Kind.SOFT, v -> { d[0].dismiss(); a.pickPackFor(r.dump, r.titleId); }), 8);
            Ui.add(box, Ui.button(a, "Import pack…", Ui.Kind.SOFT, v -> { d[0].dismiss(); PackUi.importPick(a, a.cardFor(r.dump, r.titleId)); }), 8);
            d[0] = Ui.sheet(a, "Prepare " + prof.name + "?", box).setPositiveButton("Prepare", (x, w) -> go.run()).setNegativeButton("Cancel", null).show();
        }

        /** A pack that is already there (PackCheck): ready (a done job pointing at it, the game added to the list), or a job that redoes its code, its shaders or both */
        void adopt(DumpCheck.Result r, File packDir, boolean code, boolean shaders) {
            try {
                Job j = Job.adopt(r, packDir, code, shaders);
                j.save();
                Job.remember(a, j.file);
                job = j;
                if (code || shaders) { show(); startService(j); return; }
                Prefs.open(a, "ondevice").edit().putString("added", addedKey(j)).apply();
                Pack p = Pack.read(packDir);
                Game g = new Game(r.titleId, p.name.isEmpty() ? j.name() : p.name, r.dump.getPath(), packDir.getPath());
                Game.put(a, g);   // one card per game (GameList)
                a.refresh();
                show();
                new AlertDialog.Builder(a).setTitle("Ready to play").setMessage("Its pack fits: nothing needs preparing.\n\nPack: " + packDir).setPositiveButton("OK", null).show();
            } catch (Exception e) {
                a.error("Cannot use " + packDir + ": " + e.getMessage());
            }
        }

        /**
         * A dump of a build this app does not list (DumpCheck otherBuild): what it holds and what this app supports, plainly, with the build IDs in Details; then
         * Try anyway (the warning says what may go wrong; the job prepares its newest version untested), a pack made elsewhere, or Cancel
         */
        void untested(DumpCheck.Result r, boolean start, File packOverride, boolean again) {
            Profile p = Profile.of(r.titleId);
            Ui.explain(a, "Untested version" + (p != null ? " of " + p.name : ""), r.error + "\n\n" + PrepareText.ANYWAY_WARNING, r.detail)
                    .setPositiveButton("Try anyway", (d, w) -> picked(r.dump, start, packOverride, again, false, true))
                    .setNeutralButton("Use a pack…", (d, w) -> a.pickPackFor(r.dump, r.titleId))
                    .setNegativeButton("Cancel", null).show();
        }

        void confirm(String t, String msg, String yes, Runnable r) {
            new AlertDialog.Builder(a).setTitle(t).setMessage(msg).setPositiveButton(yes, (d, w) -> r.run()).setNegativeButton("Cancel", null).show();
        }

        void startService(Job j) {
            // Android 13+: the progress notification needs the user's yes; the job runs either way (without it the notification shows only in the task manager)
            if (a.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                a.requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, ASK_NOTIFY);
            a.startForegroundService(new Intent(a, PackService.class).setAction(PackService.ACTION_START).putExtra(PackService.EXTRA_JOB, j.file.getPath()));
            if (!bound) bound = a.bindService(new Intent(a, PackService.class), this, 0);
            resumedAt = SystemClock.elapsedRealtime();
        }

        /** from: who asked (PackService.EXTRA_FROM: launcher or adb), for the logs */
        void pauseJob(String from) {
            a.startService(new Intent(a, PackService.class).setAction(PackService.ACTION_PAUSE).putExtra(PackService.EXTRA_FROM, from));
        }

        /** Go on anyway (PackService.ACTION_GO_ON): the heat pause is lifted while the service runs this job; from: launcher or adb, for the logs */
        void goOnAnyway(String from) {
            a.startService(new Intent(a, PackService.class).setAction(PackService.ACTION_GO_ON).putExtra(PackService.EXTRA_FROM, from));
        }

        void goOrPause() {
            if (job == null) return;
            if (go.getText().toString().equals("Pause")) { pauseJob("launcher"); return; }
            String missing = Profile.missing(job.titleId(), job.name());
            if (missing != null) { a.error(missing); return; }   // the button is hidden then; a click queued before it was is answered the same way
            if (a.needAccess(job.dump())) return;
            startService(job);
        }

        void play() {
            if (job == null) return;
            if (!ready(job)) { Game g = playsOld(job) ? listedGame(job) : null; if (g != null) a.launch(g); return; }   // the older library while the update runs
            Pack p;
            try { p = Pack.read(job.pack()); } catch (IOException e) { a.error(e.getMessage()); return; }
            if (p.simulated) {
                a.error("This pack was made by the simulated engine: it is a placeholder, so the game is not started. Play is enabled because the pack and its library are complete.");
                return;
            }
            Game g = new Game(job.titleId(), p.name.isEmpty() ? job.name() : p.name, job.dump().getPath(), job.pack().getPath());
            Game.put(a, g);   // one card per game: replaces the entries of the same file or the same title and build (GameList)
            a.refresh();
            a.launch(g);
        }

        /**
         * The job's Manage sheet (⋯): Forget (nothing deleted), Delete working files (the job's own ondevice/ folder), and Delete the prepared pack as well (only
         * a pack this job made, never one it adopted). Each asks first and names every folder that goes; saves are never deleted. While the job runs, all wait.
         */
        void manage() {
            if (job == null) return;
            Job j = job;
            boolean running = alive() && ("running".equals(j.state()) || "paused".equals(j.state()) && !"user".equals(j.reason()) && !"timeout".equals(j.reason()));
            LinearLayout box = new LinearLayout(a);
            box.setOrientation(LinearLayout.VERTICAL);
            TextView info = Ui.small(a, j.statusText(alive()) + "\n\nDump: " + j.dump() + "\nWorking files: " + j.dir() + "\nPack: " + j.pack() + "\nLogs: " + new File(j.dir(), "logs"));
            info.setTextIsSelectable(true);
            box.addView(info);
            android.app.AlertDialog[] d = new android.app.AlertDialog[1];
            View line = new View(a);
            line.setBackgroundColor(Ui.col(a, R.color.bn_card_stroke));
            Ui.add(box, line, 20, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1)));
            if (running) Ui.add(box, Ui.small(a, "Pause first to forget or delete."), 14);
            Button f = Ui.button(a, "Forget this job…", Ui.Kind.QUIET, v -> { d[0].dismiss(); forget(); });
            f.setEnabled(!running);
            Ui.add(box, f, 10, Ui.wrap());
            Button w = Ui.button(a, "Delete working files…", Ui.Kind.DANGER, v -> { d[0].dismiss(); deleteWork(j, false); });
            w.setEnabled(!running);
            Ui.add(box, w, 8, Ui.wrap());
            if (ownPack(j) != null) {
                Button pk = Ui.button(a, "Delete the prepared pack too…", Ui.Kind.DANGER, v -> { d[0].dismiss(); deleteWork(j, true); });
                pk.setEnabled(!running);
                Ui.add(box, pk, 8, Ui.wrap());
            }
            d[0] = Ui.sheet(a, "Preparing " + j.name(), box).setPositiveButton("Done", null).show();
        }

        void forget() {
            if (job == null) return;
            Ui.confirmDanger(a, "Forget this job?", "Deleted: nothing. The screen stops showing the job.\n\nKept: its files in " + job.dir()
                    + ", the pack, your dump and your saves. Picking the same dump again finds the job.\n\nThe split's kept progress (a copy in the app's own storage) "
                    + "is deleted when a job of another game starts; until then an unfinished split resumes from it.", "Forget", () -> {
                Job.remember(a, null);
                job = null;
                show();
            });
        }

        /** The pack folder this job made itself (the app's packs/<id>, an older job's <work>/android-pack, or the simulated engine's ondevice/fake-pack), else null: an adopted pack is the user's */
        File ownPack(Job j) {
            if (!j.ownsPack()) return null;   // a folder that was there before the job (a Mac pack, an adopted one) is the user's
            File p = j.pack(), w = j.work();
            boolean ours = p.equals(AppStorage.pack(a, j.titleId())) || p.getParentFile() != null && (p.getParentFile().equals(w) || p.getParentFile().equals(j.dir()));
            return ours && new File(p, "pack.tsv").isFile() ? p : null;
        }

        static boolean inside(File f, File dir) {
            String fp = f.getAbsolutePath() + "/", dp = dir.getAbsolutePath() + "/";
            return fp.startsWith(dp) || dp.startsWith(fp);
        }

        /** Delete working files (and, when asked, the pack this job made): asked first, never a folder that holds or is inside the dump or the save folder */
        void deleteWork(Job j, boolean pack) {
            File dir = j.dir(), pk = pack ? ownPack(j) : null;
            File save = Folders.save(a, new Game(j.titleId(), j.name(), j.dump().getPath(), j.pack().getPath()));
            java.util.List<File> gone = new java.util.ArrayList<>();
            gone.add(dir);
            if (pk != null && !inside(pk, dir)) gone.add(pk);
            for (File g : gone) {
                if (inside(g, j.dump()) || save != null && inside(g, save)) {
                    a.error("Not deleted: " + g + " holds or is inside " + (inside(g, j.dump()) ? "your dump" : "your save folder") + ".");
                    return;
                }
            }
            StringBuilder m = new StringBuilder("Deleted:\n· the working files in " + dir + " (rewritten code, split and translated shaders, the job's progress)"
                    + "\n· the split's kept progress, a copy in the app's own storage");
            if (pk != null) m.append(inside(pk, dir) ? " with the pack inside it" : "\n· the prepared pack and its shader library in " + pk);
            m.append("\n\nKept: your dump (" + j.dump() + ") and your saves").append(save != null ? " (" + save + ")" : "")
                    .append(pk == null ? ", and the pack (" + j.pack() + ")" : "").append(". Saves are never deleted here.");
            m.append(pk != null ? "\n\nThe game will not start until it is prepared again (about 30-45 minutes)." : "\n\nA new Prepare starts from the beginning.");
            Ui.confirmDanger(a, pk != null ? "Delete the prepared pack?" : "Delete working files?", m, "Delete", () -> {
                android.app.AlertDialog wait = new android.app.AlertDialog.Builder(a).setMessage("Deleting…").setCancelable(false).show();
                new Thread(() -> {
                    for (File g : gone) Job.deleteTree(g);
                    Job.deleteTree(j.splitScan(a));   // the split's kept scan results, in the app's own storage
                    h.post(() -> {
                        wait.dismiss();
                        Job.remember(a, null);
                        job = null;
                        show();
                        a.refresh();
                        android.widget.Toast.makeText(a, "Deleted. Your dump and saves are untouched.", android.widget.Toast.LENGTH_LONG).show();
                    });
                }, "nxpack-delete").start();
            });
        }
    }
}
