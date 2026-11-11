package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
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
 * The in-game panel: slides in from the right over the running game, which goes on behind it, in four tabs (TAB_NAMES: Performance, Display, Controls, General;
 * a tab bar at its top with a close button, switched by a tap or a controller's L / R) so that each fits the screen of a phone held in landscape. It holds the frame rate cap (Native.fpsCapSet), frame generation
 * (Lsfg.rows, Native.frameGenSet), the Upscaler and the dynamic resolution (GameUpscaler.rows, DrsSlider), Game resolution (resolutionRow, in Display: live for a game with graphics_scale rows), the picture (Fit / Stretch, its size and position: GameActivity.picture, TouchControls.positionRows),
 * the overlay's items (Overlay.rows), which gamepad is Player 1 (PadOrder), the on-screen controls' rows (TouchControls.controlRows), the panel's own size and text size (PanelLook), a shortcut to this
 * game's Button mapping (which applies at the next start) and Stop. Every other change applies at once, without going back to the launcher, and is
 * kept for the next start. Opened by Back (the phone's button or its edge swipe), a long press on the on-screen controls'
 * show / hide button, a swipe in from the right edge, or a controller (PadPanel: Home, or − and + held; Settings › Controller: in-game panel); closed the same
 * ways, by a tap beside it or its ✕, or by the controller's B. With a controller (padKey, padMotion) the D-pad and the left stick move the focus (held: repeated),
 * left / right also step a radio group or a slider, A presses, L / R switch tabs; A and B are the buttons that are the Switch's A and B in this game
 * (PadPanel.confirmBack, the game's Button mapping). The focused row has a yellow frame (focusMoved): the theme's own focus highlight is hard to see on the panel.
 */
final class SidePanel extends FrameLayout {
    private final Activity act;
    private final LinearLayout box, tabs;
    private final View sheet;
    private final ScrollView scroll;
    private final Rows rows;
    private TextView fpsNow, lsfgNow;
    private RadioGroup caps;
    private boolean open;
    private final Runnable tick = this::refresh;

    // The tabs: each a builder of its own (build); Performance's rows refresh twice a second while it shows
    static final String[] TAB_NAMES = {"Performance", "Display", "Controls", "General"};
    private static final int TAB_PERF = 0, TAB_DISPLAY = 1, TAB_CONTROLS = 2, TAB_GENERAL = 3;
    private int tab = TAB_PERF;
    private static final int FOCUS = 0xFFFFD54F;   // the tab bar's selected tab and the focus frame: a banana yellow, dark text on it

    // A controller: its confirm and back keys (padButtons), the held direction and its repeat
    private int confirm = KeyEvent.KEYCODE_BUTTON_B, back = KeyEvent.KEYCODE_BUTTON_A;
    private int heldDir = PadPanel.NONE, stickHeld = PadPanel.NONE, repeats;
    private final Runnable repeat = this::repeatTick;
    private View lit;   // the view with the focus frame
    /** GameActivity: the game's held buttons released when the panel opens (releaseGameInput) */
    Runnable onOpen;

    /** The rows GameActivity adds from its on-screen controls (TouchControls) */
    interface Rows {
        void controls(LinearLayout box);   // TouchControls.controlRows
        void position(LinearLayout box);   // TouchControls.positionRows: the picture's position on this screen
    }

    // The ids of the choices a rebuild gives the focus back to (restyle)
    private static final int ID_SIZE = 5000, ID_TEXT = 5100, ID_FIT = 5200, ID_MAP = 5300, ID_ONE = 5400, ID_RES = 5500;

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
        scroll = new ScrollView(a);
        scroll.addView(box);
        LinearLayout sh = new LinearLayout(a);   // the tab bar fixed at the top, the tab's rows scrolling under it
        sh.setOrientation(LinearLayout.VERTICAL);
        sh.setBackgroundColor(0xF0202124);
        tabs = new LinearLayout(a);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabs.setBackgroundColor(0xFF2D2E31);
        sh.addView(tabs, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        sh.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        addView(sh, new LayoutParams(PanelLook.widthPx(Prefs.panelSize(a), dp, a.getResources().getDisplayMetrics().widthPixels), LayoutParams.MATCH_PARENT, Gravity.END));
        sheet = sh;
        getViewTreeObserver().addOnGlobalFocusChangeListener(this::focusMoved);
        setVisibility(GONE);
    }

    /** The controller's confirm and back keys (PadPanel.confirmBack: the buttons that are the Switch's A and B in this game) */
    void padButtons(int[] cb) { confirm = cb[0]; back = cb[1]; }

    boolean isOpen() { return open; }
    void toggle() { if (open) close(); else open(); }
    /** Opened or closed by a controller (GameActivity: Home, or − and + held): opened, the focus on its first row, out of touch mode */
    void padToggle() { if (open) close(); else { open(); post(this::ensureFocus); } }

    void open() {
        if (open) return;
        open = true;
        if (onOpen != null) onOpen.run();
        box.removeAllViews();
        build();
        look();
        setVisibility(VISIBLE);
        sheet.setTranslationX(sheet.getLayoutParams().width);
        sheet.animate().translationX(0).setDuration(180).start();
        post(() -> { if (open && !isInTouchMode()) focusFirst(); });
        removeCallbacks(tick);
        refresh();
    }

    void close() {
        if (!open) return;
        open = false;
        heldDir = stickHeld = PadPanel.NONE;
        removeCallbacks(repeat);
        sheet.animate().translationX(sheet.getLayoutParams().width).setDuration(150).withEndAction(() -> { if (!open) setVisibility(GONE); }).start();
    }

    /**
     * A controller key while the panel is open (GameActivity.dispatchKeyEvent): back closes it, confirm presses what has the focus, the D-pad moves the focus (held:
     * repeated; left / right step a radio group or a slider), L / R switch tabs. Each acts on its press. Every key but Back is the panel's: false lets Back through
     */
    boolean padKey(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BACK) return false;   // the activity's Back closes the panel
        boolean press = e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0, up = e.getAction() == KeyEvent.ACTION_UP;
        int dir = PadPanel.ofKey(k);
        if (dir != PadPanel.NONE) { if (press) press(dir); else if (up) release(dir); return true; }
        if (!press) return true;
        boolean lost = findFocus() == null || isInTouchMode();
        if (k == back) close();
        else if (k == KeyEvent.KEYCODE_BUTTON_L1 || k == KeyEvent.KEYCODE_BUTTON_R1) { if (lost) ensureFocus(); showTab(PadPanel.tab(tab, k == KeyEvent.KEYCODE_BUTTON_L1 ? -1 : 1, TAB_NAMES.length)); }
        else if (k == confirm || k == KeyEvent.KEYCODE_DPAD_CENTER || k == KeyEvent.KEYCODE_ENTER) { if (lost) ensureFocus(); else activate(); }   // the first press only shows where the focus is
        return true;
    }

    /** A controller's sticks and hat while the panel is open: the hat or the left stick as a direction (PadPanel.stick), held like a D-pad key. The game gets none */
    boolean padMotion(MotionEvent e) {
        if (e.getAction() != MotionEvent.ACTION_MOVE) return true;
        int d = PadPanel.stick(e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y), stickHeld);
        if (d == PadPanel.NONE) d = PadPanel.stick(e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y), stickHeld);
        if (d != stickHeld) {
            if (stickHeld != PadPanel.NONE) release(stickHeld);
            stickHeld = d;
            if (d != PadPanel.NONE) press(d);
        }
        return true;
    }

    private void press(int dir) {
        if (dir == heldDir) return;   // the same direction from the D-pad's keys and its hat
        heldDir = dir;
        repeats = 0;
        move(dir);
        removeCallbacks(repeat);
        postDelayed(repeat, PadPanel.repeatDelay(0));
    }
    private void release(int dir) { if (dir == heldDir) { heldDir = PadPanel.NONE; removeCallbacks(repeat); } }
    private void repeatTick() {
        if (!open || heldDir == PadPanel.NONE) return;
        move(heldDir);
        postDelayed(repeat, PadPanel.repeatDelay(++repeats));
    }

    /** One step: left / right on a slider or a radio group change it, otherwise the focus moves to the nearest row that way (the ScrollView keeps it in sight) */
    private void move(int dir) {
        View f = findFocus();
        if (f == null || isInTouchMode()) { ensureFocus(); return; }
        if (dir == PadPanel.LEFT || dir == PadPanel.RIGHT) {
            int code = dir == PadPanel.LEFT ? KeyEvent.KEYCODE_DPAD_LEFT : KeyEvent.KEYCODE_DPAD_RIGHT;
            if (f instanceof SeekBar) { f.onKeyDown(code, new KeyEvent(KeyEvent.ACTION_DOWN, code)); return; }   // its own step, as the user's change (keyProgressIncrement)
            if (f instanceof RadioButton && f.getParent() instanceof RadioGroup && stepRadio((RadioGroup) f.getParent(), f, dir == PadPanel.RIGHT ? 1 : -1)) return;
        }
        int fd = dir == PadPanel.UP ? View.FOCUS_UP : dir == PadPanel.DOWN ? View.FOCUS_DOWN : dir == PadPanel.LEFT ? View.FOCUS_LEFT : View.FOCUS_RIGHT;
        View n = f.focusSearch(fd);
        if (n != null && inside(n)) n.requestFocus();
        else if (fd == View.FOCUS_UP || fd == View.FOCUS_DOWN) scroll.smoothScrollBy(0, (fd == View.FOCUS_UP ? -1 : 1) * scroll.getHeight() / 3);   // the notes past the last row
    }

    /** Left / right on a radio group: the next choice that can be chosen, from the checked one in a list (vertical), from the focused one in a row */
    private boolean stepRadio(RadioGroup g, View f, int delta) {
        boolean[] can = new boolean[g.getChildCount()];
        for (int i = 0; i < can.length; i++) { View c = g.getChildAt(i); can[i] = c instanceof RadioButton && c.isEnabled() && c.getVisibility() == VISIBLE; }
        View checked = g.getCheckedRadioButtonId() != View.NO_ID ? g.findViewById(g.getCheckedRadioButtonId()) : null;
        int cur = g.getOrientation() == LinearLayout.VERTICAL && checked != null ? g.indexOfChild(checked) : g.indexOfChild(f);
        int t = PadPanel.step(can, cur, delta);
        if (t == cur) return g.getOrientation() == LinearLayout.VERTICAL;   // a list's ends: stay; a row's ends: on to what is beside it
        View c = g.getChildAt(t);
        c.requestFocus();
        c.performClick();
        return true;
    }

    /** Confirm: presses what has the focus (a slider takes left / right instead) */
    private void activate() {
        View f = findFocus();
        if (f != null && !(f instanceof SeekBar)) f.performClick();
    }

    private boolean inside(View v) {
        for (Object p = v; p instanceof View; p = ((View) p).getParent()) if (p == sheet) return true;
        return false;
    }

    /** Out of touch mode (a controller is in use: requestFocusFromTouch), the focus on the panel: kept where it is, else on the tab's first row */
    private void ensureFocus() {
        if (!open) return;
        View f = findFocus();
        if (f != null && !isInTouchMode()) return;
        View t = tabs.getChildAt(tab);
        if (t != null) t.requestFocusFromTouch();
        focusFirst();
    }
    private void focusFirst() {
        java.util.ArrayList<View> l = box.getFocusables(View.FOCUS_FORWARD);
        View f = l.isEmpty() ? tabs.getChildAt(tab) : l.get(0);
        if (f != null) f.requestFocus();
    }

    /** The focus frame: a yellow outline on the focused row of the panel (not in touch mode, where nothing has the focus) */
    private void focusMoved(View old, View now) {
        if (lit != null) { lit.setForeground(null); lit = null; }
        if (now == null || !inside(now) || isInTouchMode()) return;
        float dp = getResources().getDisplayMetrics().density;
        android.graphics.drawable.GradientDrawable d = new android.graphics.drawable.GradientDrawable();
        d.setColor(0x26FFD54F);
        d.setStroke(Math.round(2.5f * dp), FOCUS);
        d.setCornerRadius(6 * dp);
        now.setForeground(d);
        lit = now;
    }

    /**
     * A dialog opened from the panel with a controller's confirm and back keys (on their press), as in the panel: Android's own fallback would take the bottom
     * button as confirm and the right one as back. Its focus starts on the safe choice (negative), so a confirm press does nothing that cannot be undone
     */
    private AlertDialog padDialog(AlertDialog d) {
        d.setOnKeyListener((di, k, e) -> {
            if (k != confirm && k != back) return false;
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) {
                if (k == back) di.cancel();
                else { d.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_CENTER)); d.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DPAD_CENTER)); }
            }
            return true;
        });
        Button no = d.getButton(AlertDialog.BUTTON_NEGATIVE);
        if (no != null && !isInTouchMode()) no.requestFocus();
        return d;
    }

    /** The tab bar: one button per tab (the selected one filled), then ✕; large enough to tap (the panel's row height, 48 dp at least), named for TalkBack */
    private void tabBar() {
        tabs.removeAllViews();
        float dp = getResources().getDisplayMetrics().density;
        int h = Math.max(Math.round(48 * dp), PanelLook.rowPx(Prefs.panelSize(act), dp)), pad = Math.round(4 * dp);
        for (int i = 0; i < TAB_NAMES.length; i++) {
            int k = i;
            boolean on = i == tab;
            TextView t = new TextView(act);
            t.setText(TAB_NAMES[i]);
            t.setGravity(Gravity.CENTER);
            t.setMaxLines(1);
            t.setIncludeFontPadding(false);
            t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);   // one size for every tab, set once the bar is laid out (tabTextSize)
            t.setPadding(pad, 0, pad, 0);
            t.setTypeface(on ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            t.setTextColor(on ? 0xFF000000 : 0xFFE8EAED);
            t.setBackgroundColor(on ? FOCUS : 0);
            t.setSelected(on);
            t.setFocusable(true);
            t.setOnClickListener(v -> showTab(k));
            t.setContentDescription(TAB_NAMES[i] + " tab, " + (i + 1) + " of " + TAB_NAMES.length + (on ? ", selected" : ""));
            tabs.addView(t, new LinearLayout.LayoutParams(0, h, 1f));
        }
        TextView x = new TextView(act);
        x.setText("✕");
        x.setGravity(Gravity.CENTER);
        x.setTextSize(18);
        x.setTextColor(0xFFE8EAED);
        x.setFocusable(true);
        x.setOnClickListener(v -> close());
        x.setContentDescription("Close the panel, back to the game");
        tabs.addView(x, new LinearLayout.LayoutParams(h, h));
        tabs.post(this::tabTextSize);
    }

    /** One text size for all tab labels: the largest (16 sp at most, 10 at least) at which the widest label, in bold as when selected, fits its tab.
     *  Auto-sizing each label on its own shrank "Performance" alone and set it lower than the others */
    private void tabTextSize() {
        if (tabs.getChildCount() <= TAB_NAMES.length) return;
        float sp = getResources().getDisplayMetrics().scaledDensity, fit = 16;
        android.graphics.Paint p = new android.graphics.Paint();
        p.setTypeface(Typeface.DEFAULT_BOLD);
        p.setTextSize(16 * sp);
        for (int i = 0; i < TAB_NAMES.length; i++) {
            TextView t = (TextView) tabs.getChildAt(i);
            int room = t.getWidth() - t.getPaddingLeft() - t.getPaddingRight();
            float w = p.measureText(TAB_NAMES[i]);
            if (room > 0 && w > room) fit = Math.min(fit, 16 * room / w);
        }
        float size = Math.max(10, (float) Math.floor(fit * 2) / 2);
        for (int i = 0; i < TAB_NAMES.length; i++) ((TextView) tabs.getChildAt(i)).setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
    }

    /** Another tab: its rows built, scrolled to its top; with a controller the focus on its first row */
    private void showTab(int k) {
        if (!open || k == tab) return;
        tab = k;
        box.removeAllViews();
        build();
        look();
        scroll.scrollTo(0, 0);
        if (!isInTouchMode()) focusFirst();
        refresh();
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
            int y = scroll.getScrollY();
            box.removeAllViews();
            build();
            look();
            scroll.post(() -> {
                scroll.scrollTo(0, y);
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

    /** The tab's rows (TAB_NAMES); Performance's here: the frame rate cap, the Upscaler, the dynamic resolution and frame generation */
    private void build() {
        tabBar();
        fpsNow = lsfgNow = null;
        caps = null;
        if (tab == TAB_DISPLAY) { display(); return; }
        if (tab == TAB_CONTROLS) { controls(); return; }
        if (tab == TAB_GENERAL) { general(); return; }

        head("Frame rate cap");   // the game's own frame rate; frame generation (below, its own heading) adds frames on top
        caps = new RadioGroup(act);
        if (Prefs.limited(game())) {   // a frame limit, no locking (Prefs.PACE_CAPS)
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
                Native.fpsLimitSet(v > 0 ? v : Prefs.paced(game()) ? 60 : 0);   // 60 fps: a paced game's fastest rate; for another, no limit (the game's own interval)
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
        if (Prefs.paced(game())) {   // Full game speed at low frame rates: sead's controller at the measured rate (Game settings has the same), live in the host
            android.widget.CheckBox full = new android.widget.CheckBox(act);
            full.setText("Full game speed at low frame rates");
            full.setChecked(Prefs.decouple(act, game()));
            full.setOnCheckedChangeListener((b, on) -> { Prefs.decouple(act, game(), on); Native.decoupleSet(on); });
            box.addView(full);
        }
        fpsNow = note("");
        if (GameUpscaler.shown(act, game())) {   // right under the frame rate cap: the Upscaler (the game's own upscaler and the window's scaling to the screen), for games whose network it replaces
            GameUpscaler.rows(act, game(), box);
            note(act instanceof GameActivity && ((GameActivity) act).live() ? "Applies from the next frame, kept for this game (the same setting as Game settings › Upscaler)."
                    : "Applies at the next start (this run takes no live changes: DM_CTL is off).");
        }
        if (Prefs.drsGame(game())) {
            TextView drsHead = head("Dynamic resolution");   // the render size's range (Prefs.DRS), applied at each step; the heading shows it
            box.addView(DrsSlider.view(act, Prefs.drs(act, game()), drsHead::setText, i -> { Prefs.drs(act, game(), i); Native.drsSet(Prefs.DRS[i][0], Prefs.DRS[i][1]); }));
        }

        lsfgNow = Lsfg.rows(act, box, "", () -> { Lsfg.push(act); refresh(); });
    }

    /** Display: the picture (Fit / Stretch, size, position) and the overlay's items */
    private void display() {
        resolutionRow();

        picture();

        head("Overlay");
        Overlay.rows(act, box, null);
    }

    /** Controls: Player 1, the on-screen controls, rumble and the Button mapping shortcut */
    private void controls() {
        playerOneRow();

        head("On-screen controls");
        rows.controls(box);
        // Rumble: the game's vibration on the controller or the phone, this game's setting (Game settings › Rumble), applied at once (GameActivity.rumble)
        android.widget.CheckBox rum = new android.widget.CheckBox(act);
        rum.setText("Rumble (the game's vibration)");
        rum.setChecked(Prefs.rumble(act, game()));
        rum.setOnCheckedChangeListener((b, on) -> { if (act instanceof GameActivity) ((GameActivity) act).rumble(on); else Prefs.rumble(act, game(), on); });
        box.addView(rum);

        nextStartRows();
    }

    /** General: the panel's own look, Pause, Stop and Back to the game */
    private void general() {
        note("The game runs on behind this panel; every change applies now and is kept, except those marked for the next start.");
        panelRows();

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
        stop.setOnClickListener(v -> padDialog(new AlertDialog.Builder(act).setTitle("Stop the game?").setMessage("The game ends; unsaved progress is lost. The log stays for Share log.")
                .setPositiveButton("Stop", (d, w) -> { Native.note("stopped by the user"); ExitWatch.stopped(); if (act instanceof GameActivity) ((GameActivity) act).backToFrontend(); android.os.Process.killProcess(android.os.Process.myPid()); })
                .setNegativeButton("Keep running", null).show()));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = Math.round(24 * getResources().getDisplayMetrics().density);
        box.addView(stop, lp);
        Button done = new Button(act);
        done.setText("Back to the game");
        done.setOnClickListener(v -> close());
        box.addView(done, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    /**
     * Resolution: this game's Game resolution (the same choices and the same setting as Game settings › Game resolution), only for a game whose profile has a lever
     * (Profile.hasResolution). A game with graphics_scale rows changes now: the host takes the choice's graphics scale and operation mode and posts the console's change
     * notifications, which the game answers by querying them again and resizing its scenes (Native.graphicsModeSet, hle_oe.c nxhost_graphics_mode_set). A window_fn or res_levels
     * game takes it at the next start.
     */
    private void resolutionRow() {
        Profile p = Profile.of(game());
        if (p == null || !p.hasResolution()) return;
        head("Resolution");
        boolean now = p.resolutionLive() && act instanceof GameActivity;
        String cur = Prefs.gameWindow(act, game());
        Button r = row(ID_RES, "Game resolution", (Prefs.ownWindow(act, game()) ? "" : "Default · ") + Prefs.windowName(cur, false));
        r.setOnClickListener(v -> {
            String[] labels = new String[Prefs.WINDOWS.length + 1];
            labels[0] = "Default (" + Prefs.windowName(Prefs.defaultWindow(act, game()), false) + ")";
            { Profile lp = Profile.of(game()); for (int li = 0; li < Prefs.WINDOWS.length; li++) labels[li + 1] = lp != null ? lp.windowLabel(Prefs.WINDOWS[li], Prefs.WINDOW_NAMES[li]) : Prefs.WINDOW_NAMES[li]; }   // a choice the game draws at another size says so (Profile.windowLabel)
            int checked = Prefs.ownWindow(act, game()) ? 1 + Math.max(0, java.util.Arrays.asList(Prefs.WINDOWS).indexOf(cur)) : 0;
            new AlertDialog.Builder(act).setTitle("Game resolution").setSingleChoiceItems(labels, checked, (d, w) -> {
                Prefs.gameWindow(act, game(), w == 0 ? null : Prefs.WINDOWS[w - 1]);   // the game's own setting: the next start takes it too
                if (now) {
                    int[] m = p.resolutionLive(Prefs.gameWindow(act, game()));
                    if (m != null && !Native.graphicsModeSet(m[0], m[1])) Native.note("Game resolution: this host cannot change it live; it applies at the next start");
                }
                d.dismiss();
                restyle(ID_RES);
            }).setNegativeButton("Cancel", null).show();
        });
        note(now ? "Applies now: the game resizes its picture within a few frames. Kept for this game (the same setting as Game settings › Game resolution)."
                + (p.resolution.contains("/console") ? " The larger sizes run the game as on a docked console." : "")
                : "Applies at the next start (the same setting as Game settings › Game resolution).");
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
            padDialog(new AlertDialog.Builder(act).setTitle("Player 1").setSingleChoiceItems(names, checked, (d, w) -> {
                ga.choosePlayerOne(w == 0 ? "" : pads.get(w - 1).key);
                d.dismiss();
                restyle(ID_ONE);
            }).setNegativeButton("Cancel", null).show());
        });
    }

    /** Twice a second while open: what the game runs at, which caps it can hold, and the frame generation's state */
    private void refresh() {
        if (!open || tab != TAB_PERF) return;   // only Performance has live lines (showTab calls it again)
        String[] f = Native.fpsCapStatus().split(" ");
        int cap = Integer.parseInt(f[0]), rates = Integer.parseInt(f[1]), now = Integer.parseInt(f[2]);
        if (Prefs.limited(game())) {   // the frame limiter's caps are always available
            // The overlay shows the frame rate, the cap is checked above; one line only when the limit holds another rate: a game whose frame step moves in whole
            // vsyncs (hle_fpslock.c dm_fps_limit_step) runs 45 and 40 at 30, at full speed
            int chosen = Prefs.paceCap(act, game()), held = f.length > 3 ? Integer.parseInt(f[3]) : 0;
            boolean other = now > 0 && chosen > 0 && held > 0 && 60 % chosen != 0 && held < chosen;
            fpsNow.setVisibility(other ? VISIBLE : GONE);
            if (other) fpsNow.setText("This game moves in whole frames: " + chosen + " fps runs at " + held + " fps, at full game speed.");
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
