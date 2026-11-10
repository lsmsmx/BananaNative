package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * The in-game panel: slides in from the right over the running game, which goes on behind it. It holds the frame rate cap (Native.fpsCapSet), frame generation
 * (Lsfg.rows, Native.frameGenSet), the Upscaler and the dynamic resolution (GameUpscaler.rows, DrsSlider), the picture (Fit / Stretch, its size and position: GameActivity.picture, TouchControls.positionRows),
 * the overlay's items (Overlay.rows), which gamepad is Player 1 (PadOrder), the on-screen controls' rows (TouchControls.controlRows), the panel's own size and text size (PanelLook), a shortcut to this
 * game's Button mapping (which applies at the next start) and Stop. Every other change applies at once, without going back to the launcher, and is
 * kept for the next start. Opened by Back (the phone's button or its edge swipe), a long press on the on-screen controls'
 * show / hide button, or a gamepad's Home / guide button (KEYCODE_BUTTON_MODE); closed the same ways, by a tap beside it, or by B on a gamepad, whose D-pad and A move
 * through it meanwhile (GameActivity.dispatchKeyEvent).
 */
final class SidePanel extends FrameLayout {
    private final Activity act;
    private final LinearLayout box;
    private final View sheet;
    private final Rows rows;
    private TextView fpsNow, lsfgNow;
    private RadioGroup caps;
    private boolean open;
    private final Runnable tick = this::refresh;

    /** The rows GameActivity adds from its on-screen controls (TouchControls) */
    interface Rows {
        void controls(LinearLayout box);   // TouchControls.controlRows
        void position(LinearLayout box);   // TouchControls.positionRows: the picture's position on this screen
    }

    // The ids of the choices a rebuild gives the focus back to (restyle)
    private static final int ID_SIZE = 5000, ID_TEXT = 5100, ID_FIT = 5200, ID_MAP = 5300, ID_ONE = 5400;

    SidePanel(Activity a, Rows rows) {
        super(a);
        act = a;
        this.rows = rows;
        float dp = getResources().getDisplayMetrics().density;
        View scrim = new View(a);
        scrim.setBackgroundColor(0x66000000);
        scrim.setOnClickListener(v -> close());
        addView(scrim, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int p = Math.round(16 * dp);
        box.setPadding(p, p, p, p);
        ScrollView sv = new ScrollView(a);
        sv.setBackgroundColor(0xF0202124);
        sv.addView(box);
        addView(sv, new LayoutParams(PanelLook.widthPx(Prefs.panelSize(a), dp, a.getResources().getDisplayMetrics().widthPixels), LayoutParams.MATCH_PARENT, Gravity.END));
        sheet = sv;
        setVisibility(GONE);
    }

    boolean isOpen() { return open; }
    void toggle() { if (open) close(); else open(); }

    void open() {
        if (open) return;
        open = true;
        box.removeAllViews();
        build();
        look();
        setVisibility(VISIBLE);
        sheet.setTranslationX(sheet.getLayoutParams().width);
        sheet.animate().translationX(0).setDuration(180).start();
        box.post(() -> { View f = box.focusSearch(View.FOCUS_DOWN); if (f != null) f.requestFocus(); else box.requestFocus(); });
        removeCallbacks(tick);
        refresh();
    }

    void close() {
        if (!open) return;
        open = false;
        sheet.animate().translationX(sheet.getLayoutParams().width).setDuration(150).withEndAction(() -> { if (!open) setVisibility(GONE); }).start();
    }

    /** A gamepad key while the panel is open: B closes it, A presses what has focus, the D-pad moves the focus (the view system's own handling) */
    boolean padKey(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BUTTON_B) { if (e.getAction() == KeyEvent.ACTION_UP) close(); return true; }
        if (k == KeyEvent.KEYCODE_BUTTON_A) {
            View f = findFocus();
            if (e.getAction() == KeyEvent.ACTION_UP && f != null) f.performClick();
            return true;
        }
        return false;
    }

    /** The panel's size and text size (Prefs.panelSize / panelText): its width, and every row's least height and text size, over what build() made */
    private void look() {
        float dp = getResources().getDisplayMetrics().density;
        int size = Prefs.panelSize(act), w = PanelLook.widthPx(size, dp, getResources().getDisplayMetrics().widthPixels);
        if (sheet.getLayoutParams().width != w) { sheet.getLayoutParams().width = w; sheet.requestLayout(); }
        look(box, PanelLook.textScale(Prefs.panelText(act)), PanelLook.rowPx(size, dp));
    }
    private static void look(View v, float text, int row) {
        if (v instanceof TextView) {
            TextView t = (TextView) v;
            if (text != 1f) t.setTextSize(TypedValue.COMPLEX_UNIT_PX, t.getTextSize() * text);
            if (v instanceof Button || v instanceof CompoundButton) t.setMinHeight(row);
        } else if (v instanceof SeekBar) v.setMinimumHeight(row);
        if (v instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) v).getChildCount(); i++) look(((ViewGroup) v).getChildAt(i), text, row);
    }

    /** Built again with the new look or labels, at once (from a choice's listener: after it returns), the focus back on the choice that was pressed */
    private void restyle(int focusId) {
        post(() -> {
            if (!open) return;
            int y = sheet.getScrollY();
            box.removeAllViews();
            build();
            look();
            sheet.post(() -> {
                sheet.scrollTo(0, y);
                View f = focusId != 0 ? box.findViewById(focusId) : null;
                if (f != null) f.requestFocus();
            });
            refresh();
        });
    }

    private TextView head(String t) {
        TextView h = new TextView(act);
        h.setText(t);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        h.setTextSize(15);
        h.setPadding(0, Math.round(18 * getResources().getDisplayMetrics().density), 0, 0);
        box.addView(h);
        return h;
    }

    private TextView note(String t) {
        TextView n = new TextView(act);
        n.setText(t);
        n.setTextSize(12);
        n.setTextColor(Color.LTGRAY);
        box.addView(n);
        return n;
    }

    /** The running game's title ID (GameActivity's intent): the panel's settings are that game's own */
    private String game() { return String.valueOf(act.getIntent().getStringExtra("title_id")); }

    private void build() {
        TextView title = new TextView(act);
        title.setText("Banana Native");
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextSize(20);
        box.addView(title);
        note("The game runs on behind this panel; every change applies now and is kept, except those marked for the next start.");

        head("Frame rate cap");   // the game's own frame rate; frame generation (below, its own heading) adds frames on top
        caps = new RadioGroup(act);
        if (Prefs.paced(game())) {   // a frame limit, no locking (Prefs.PACE_CAPS)
            int cur = Prefs.paceCap(act, game());
            for (int i = 0; i < Prefs.PACE_CAPS.length; i++) {
                RadioButton b = new RadioButton(act);
                b.setId(3000 + i); b.setText(Prefs.paceLabel(Prefs.PACE_CAPS[i]));
                if (Prefs.PACE_CAPS[i] == cur) b.setChecked(true);
                caps.addView(b);
            }
            caps.setOnCheckedChangeListener((g, id) -> {
                int v = Prefs.PACE_CAPS[id - 3000];
                Prefs.fpsCap(act, game(), v);
                Native.fpsLimitSet(v > 0 ? v : 60);
                refresh();
            });
        } else {
            int live = Integer.parseInt(Native.fpsCapStatus().split(" ")[0]), cur = live >= 0 ? live : Prefs.fpsCap(act, game());   // the cap in effect (env.txt or DM_CTL may have set another)
            for (int i = 0; i < Prefs.FPS_CAPS.length; i++) {
                int v = Prefs.FPS_CAPS[i];
                RadioButton b = new RadioButton(act);
                b.setId(2000 + i);
                b.setText(v == 0 ? "The game's own" : v + " fps");
                if (v == cur || v == 0 && cur < 0) b.setChecked(true);   // a cap the launcher does not offer (40, 45 from env.txt or DM_CTL): none checked, the line below says it
                caps.addView(b);
            }
            caps.setOnCheckedChangeListener((g, id) -> {
                int v = Prefs.FPS_CAPS[id - 2000];
                Prefs.fpsCap(act, game(), v);   // the running game's own cap from now on (the main screen's stays the default of the others)
                Native.fpsCapSet(v);
                refresh();
            });
        }
        box.addView(caps);
        fpsNow = note("");
        GameUpscaler.rows(act, game(), box);   // right under the frame rate cap: the Upscaler (the game's own upscaler and the window's scaling to the screen
        if (Prefs.drsGame(game())) {
            TextView drsHead = head("Dynamic resolution");   // the render size's range (Prefs.DRS), applied at each step; the heading shows it
            box.addView(DrsSlider.view(act, Prefs.drs(act, game()), drsHead::setText, i -> { Prefs.drs(act, game(), i); Native.drsSet(Prefs.DRS[i][0], Prefs.DRS[i][1]); }));
        }

        lsfgNow = Lsfg.rows(act, box, "", () -> { Lsfg.push(act); refresh(); });


        picture();

        head("Overlay");
        Overlay.rows(act, box, null);

        playerOneRow();

        head("On-screen controls");
        rows.controls(box);
        // Rumble: the game's vibration on the controller or the phone, this game's setting (Game settings › Rumble), applied at once (GameActivity.rumble)
        android.widget.CheckBox rum = new android.widget.CheckBox(act);
        rum.setText("Rumble (the game's vibration)");
        rum.setChecked(Prefs.rumble(act, game()));
        rum.setOnCheckedChangeListener((b, on) -> { if (act instanceof GameActivity) ((GameActivity) act).rumble(on); else Prefs.rumble(act, game(), on); });
        box.addView(rum);

        panelRows();

        nextStartRows();

        Button pause = new Button(act);   // the game suspended (clock, graphics, threads, audio) until a tap on the screen, or A / Start: GameActivity.pause
        pause.setText("Pause the game");
        pause.setOnClickListener(v -> { close(); if (act instanceof GameActivity) ((GameActivity) act).pause(); });
        LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pl.topMargin = Math.round(24 * getResources().getDisplayMetrics().density);
        box.addView(pause, pl);
        android.widget.CheckBox ret = new android.widget.CheckBox(act);
        ret.setText("Pause when the game comes back from the background");
        ret.setChecked(Prefs.pauseOnReturn(act, game()));
        ret.setOnCheckedChangeListener((b, on) -> Prefs.pauseOnReturn(act, game(), on));
        box.addView(ret);
        Button stop = new Button(act);
        stop.setText("Stop the game");
        stop.setOnClickListener(v -> new AlertDialog.Builder(act).setTitle("Stop the game?").setMessage("The game ends; unsaved progress is lost. The log stays for Share log.")
                .setPositiveButton("Stop", (d, w) -> { Native.note("stopped by the user"); ExitWatch.stopped(); if (act instanceof GameActivity) ((GameActivity) act).backToFrontend(); android.os.Process.killProcess(android.os.Process.myPid()); })
                .setNegativeButton("Keep running", null).show());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = Math.round(24 * getResources().getDisplayMetrics().density);
        box.addView(stop, lp);
        Button done = new Button(act);
        done.setText("Back to the game");
        done.setOnClickListener(v -> close());
        box.addView(done, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    /** Picture: Fit / Stretch and the size (GameActivity.picture: live through DM_CTL, kept for this game), then the position on this screen (TouchControls) */
    private void picture() {
        head("Picture");
        GameActivity ga = act instanceof GameActivity ? (GameActivity) act : null;
        boolean live = ga != null && ga.live();
        boolean st = live ? ga.runStretch() : Prefs.stretch(act, game());   // what the game shows now (an env.txt line may have set it), else what the next start uses
        int sz = live ? ga.runPictureSize() : Prefs.pictureSize(act, game());
        RadioGroup fit = new RadioGroup(act);
        fit.setOrientation(RadioGroup.HORIZONTAL);
        String[] names = {"Fit", "Stretch"};
        for (int i = 0; i < names.length; i++) { RadioButton b = new RadioButton(act); b.setId(ID_FIT + i); b.setText(names[i]); fit.addView(b); }
        fit.check(ID_FIT + (st ? 1 : 0));
        box.addView(fit);
        TextView sl = new TextView(act);
        SeekBar ss = new SeekBar(act);
        ss.setMax(PictureFit.steps());
        ss.setProgress(PictureFit.stepOfSize(sz));
        ss.setKeyProgressIncrement(1);   // the D-pad's left / right: 5 % a press
        Runnable label = () -> sl.setText("Size " + PictureFit.sizeOfStep(ss.getProgress()) + " %" + (ss.getProgress() == PictureFit.steps() ? " (full)" : ""));
        label.run();
        Runnable send = () -> { if (ga != null) ga.picture(fit.getCheckedRadioButtonId() == ID_FIT + 1, PictureFit.sizeOfStep(ss.getProgress())); };
        fit.setOnCheckedChangeListener((g, id) -> send.run());
        ss.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean user) { label.run(); send.run(); }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        box.addView(sl);
        box.addView(ss);
        note(live ? "Fit keeps the game's shape, Stretch fills the screen; below 100 % the picture is smaller, with black around it. Applies now, kept for this game."
                : "Applies at the next start (this run takes no live changes: DM_CTL is off).");
        rows.position(box);
    }

    /** The panel itself: its size (width, row height) and its text size, for every game, applied at once */
    private void panelRows() {
        head("Panel");
        String[] what = {"Size", "Text size"};
        int[] ids = {ID_SIZE, ID_TEXT}, cur = {Prefs.panelSize(act), Prefs.panelText(act)};
        for (int k = 0; k < 2; k++) {
            TextView l = new TextView(act); l.setText(what[k]); box.addView(l);
            RadioGroup g = new RadioGroup(act);
            g.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < PanelLook.NAMES.length; i++) { RadioButton b = new RadioButton(act); b.setId(ids[k] + i); b.setText(PanelLook.NAMES[i]); g.addView(b); }
            g.check(ids[k] + cur[k]);
            int base = ids[k];
            g.setOnCheckedChangeListener((gr, id) -> {
                if (base == ID_SIZE) Prefs.panelSize(act, id - base); else Prefs.panelText(act, id - base);
                restyle(id);
            });
            box.addView(g);
        }
    }

    /**
     * A shortcut to this game's Button mapping (DM_PAD_MAP), which the host reads only at its start: one compact row under the "Next start" heading, "Button mapping"
     * then the mapping's summary in grey and a chevron. A borderless Button, so that look() gives it the panel's row height and text size and the D-pad can focus it.
     * Game language is set in the launcher only (Game settings › Game language).
     */
    private void nextStartRows() {
        if (!(act instanceof GameActivity)) return;
        GameActivity ga = (GameActivity) act;
        head("Next start");
        String sum = PadMapUi.label(act, game());
        Button map = row(ID_MAP, "Button mapping", sum);
        map.setContentDescription("Button mapping: " + sum + ", applies at the next start");
        map.setOnClickListener(v -> PadMapUi.forGame(act, ga.game(), () -> restyle(ID_MAP)));
    }

    /**
     * One compact row: the label, then the value in grey and a chevron. A borderless Button, so that look() gives it the panel's row height and text size and the
     * D-pad can focus it.
     */
    private Button row(int id, String label, String value) {
        Button map = new Button(act);
        map.setId(id);
        map.setAllCaps(false);
        TypedValue bg = new TypedValue();
        act.getTheme().resolveAttribute(android.R.attr.selectableItemBackground, bg, true);
        map.setBackgroundResource(bg.resourceId);   // a list row with the focus and press highlight, not a raised default Button
        map.setStateListAnimator(null);
        map.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        map.setTextColor(new TextView(act).getTextColors());   // the colour of the panel's other rows
        map.setTextSize(14);
        map.setSingleLine(true);
        map.setEllipsize(android.text.TextUtils.TruncateAt.END);
        map.setMinWidth(0);
        map.setMinimumWidth(0);
        int pv = Math.round(4 * getResources().getDisplayMetrics().density);
        map.setPadding(0, pv, 0, pv);
        android.text.SpannableStringBuilder t = new android.text.SpannableStringBuilder(label + "  ");
        int from = t.length();
        t.append(value).append("  ›");
        t.setSpan(new android.text.style.ForegroundColorSpan(Color.LTGRAY), from, t.length(), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        map.setText(t);
        box.addView(map, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return map;
    }

    /**
     * Player 1 (PadOrder): the gamepad on player 1, and a choice of the gamepads connected or Automatic (an external controller before a phone's own gamepad-like
     * device). Applies at once and is kept for every game, by the device's descriptor.
     */
    private void playerOneRow() {
        if (!(act instanceof GameActivity)) return;
        GameActivity ga = (GameActivity) act;
        java.util.List<PadOrder.Pad> pads = ga.gamepads();
        if (pads.isEmpty()) return;
        head("Controllers");
        PadOrder.Pad one = ga.playerOne();
        String chosen = Prefs.padFirst(act);
        Button b = row(ID_ONE, "Player 1", (one != null ? one.name : "none") + (chosen.isEmpty() ? " (automatic)" : ""));
        b.setOnClickListener(v -> {
            String[] names = new String[pads.size() + 1];
            names[0] = "Automatic";
            int checked = 0;
            for (int i = 0; i < pads.size(); i++) { names[i + 1] = pads.get(i).name; if (!chosen.isEmpty() && chosen.equals(pads.get(i).key)) checked = i + 1; }
            new AlertDialog.Builder(act).setTitle("Player 1").setSingleChoiceItems(names, checked, (d, w) -> {
                ga.choosePlayerOne(w == 0 ? "" : pads.get(w - 1).key);
                d.dismiss();
                restyle(ID_ONE);
            }).setNegativeButton("Cancel", null).show();
        });
    }

    /** Twice a second while open: what the game runs at, which caps it can hold, and the frame generation's state */
    private void refresh() {
        if (!open) return;
        String[] f = Native.fpsCapStatus().split(" ");
        int cap = Integer.parseInt(f[0]), rates = Integer.parseInt(f[1]), now = Integer.parseInt(f[2]);
        if (Prefs.paced(game())) {   // the frame limiter's caps are always available
            fpsNow.setVisibility(GONE);   // the overlay shows the frame rate, the cap is checked above
        } else {
            for (int i = 0; i < Prefs.FPS_CAPS.length; i++) {
                int v = Prefs.FPS_CAPS[i], bit = v == 15 ? 0 : v == 20 ? 1 : v == 30 ? 2 : 5;   // the host's bits: 15, 20, 30, (40, 45,) 60
                caps.getChildAt(i).setEnabled(v == 0 || cap >= 0 && (rates & 1 << bit) != 0);
            }
            // No cap in this build, or a game that picks its own frame rate: no choices (they would do nothing), one line instead
            // (rates is 0 too until the game's main is loaded: only a game that presents, now > 0, is known to pick its own)
            boolean own = cap >= 0 && rates == 0 && now > 0, none = cap < 0 || own;
            caps.setVisibility(none ? GONE : VISIBLE);
            if (cap >= 0 && now > 0) Prefs.ownRate(act, game(), own);   // its Game settings hide the cap too
            String s = cap < 0 ? "This build has no frame rate cap."
                    : now <= 0 ? "Starting…"
                    : own ? "This game picks its own frame rate."
                    : "Now " + now + " fps" + (cap > 0 ? ", capped at " + cap : ", the game's own") + ".";
            fpsNow.setText(s);
        }
        if (lsfgNow != null) { String st = Native.frameGenStatus(); int sp = st.indexOf(' '); lsfgNow.setText("Now: " + (sp > 0 ? st.substring(sp + 1) : st)); }
        removeCallbacks(tick);   // a change calls refresh too: one loop, not one more per change
        postDelayed(tick, 500);
    }
}
