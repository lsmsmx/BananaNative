package dev.nxnative.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.TypedValue;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.Arrays;

/**
 * The on-screen controls over the game's SurfaceView: a virtual Pro Controller drawn by the launcher and driven by the host. The view lays the controls out in dp around
 * the letterboxed 16:9 picture, hands the layout to the host (nxhost_touch_controls_set) and passes every touch on (nxhost_input_touch); the host decides what each touch
 * drives, a control (its buttons and sticks go to the game as the handheld controller, dm_input_set) or the game's touchscreen, so a touch on a control never reaches the
 * touchscreen. The view reads back what each control holds to draw it and to give a haptic tick on each press.
 * The small button at the top centre (faint, its touch area larger than its drawing) shows or hides the controls; holding it opens the in-game panel. Settings ›
 * Show / hide button (and the panel's controls rows) turns it off: the controls then always show (in Auto, while no gamepad is connected). They hide by themselves
 * while a gamepad is connected and come back when the last one goes.
 * The "On-screen controls" setting (Settings, the in-game panel; for every game): Auto (the default) draws nothing while a gamepad is connected, not even the show /
 * hide button or the panel's handle (Back or the gamepad's Home button opens the panel), and everything comes back when the last one goes; On keeps them with a gamepad
 * too; Off never draws them, and every touch is the game's touchscreen.
 */
final class TouchControls extends View {
    // DM_BTN_* (dm/host/dm_input.h, nn::hid::NpadButton order)
    private static final long A = 1, B = 1 << 1, X = 1 << 2, Y = 1 << 3, LS = 1 << 4, RS = 1 << 5, L = 1 << 6, R = 1 << 7, ZL = 1 << 8, ZR = 1 << 9,
            PLUS = 1 << 10, MINUS = 1 << 11, LEFT = 1 << 12, UP = 1 << 13, RIGHT = 1 << 14, DOWN = 1 << 15;
    private static final int BUTTON = 0, STICK = 1, DPAD = 2, MAX = 24;   // input_map.h NXT_*, NXT_MAX
    private static final int SHAPE_ROUND = 0, SHAPE_SHOULDER = 1;

    private final boolean input;   // the host takes touches (nxhost_input_touch)
    private SharedPreferences prefs() { return Prefs.open(getContext(), "touch_controls"); }   // read again when the other process wrote it (Prefs.open)
    private boolean available;     // the host takes a layout (nxhost_touch_controls_set): the controls can be drawn
    private boolean shown = true, gamepad;
    private boolean button = true;   // the show / hide button is there (toggleButton): without it the controls are never hidden by it
    private int mode = MODE_AUTO;   // the "On-screen controls" setting (setMode)
    private float opacity, scale;
    private boolean haptics;

    // The layout in pixels, as the host has it
    private int n;
    private final int[] kind = new int[MAX], stick = new int[MAX], shape = new int[MAX];
    private final long[] buttons = new long[MAX];
    private final float[] cx = new float[MAX], cy = new float[MAX], r = new float[MAX];
    private final String[] label = new String[MAX];
    private float tx, ty, tr;      // the show / hide button
    private float picX, picY, picW = 1, picH = 1;   // the game's picture in the view, normalized

    // The user's layout, one per screen shape (prof: the view's size in steps of 40 units, so a foldable's two screens keep one each): each group (the left stick with L3,
    // the D-pad, the face buttons, the right stick with R3, the left and the right shoulders with − / +) moved by (gdx, gdy) units and scaled by gsc around its centre,
    // and where the picture sits in the free space (placeX, placeY: 0 left / top, 0.5 centred, 1 right / bottom; nxhost_picture_place)
    private static final int GROUPS = 6;
    private static final String[] GROUP_NAME = {"left stick", "D-pad", "A B X Y", "right stick", "ZL L −", "ZR R +"};
    private String prof;
    private final float[] gdx = new float[GROUPS], gdy = new float[GROUPS], gsc = new float[GROUPS];
    private float placeX = 0.5f, placeY = 0.5f;
    private boolean stretch;   // the replay stretches the picture over the whole view (DM_VK_STRETCH)
    private int pictureSize = PictureFit.SIZE_MAX;   // the picture's share of its fitted or stretched size (DM_VK_PICTURE_SIZE), %
    /** Stretch to screen and the picture size (GameActivity: from the game's environment at start, and the in-game panel's Picture rows while it runs, which send the
     *  host the same values): touches map to the rectangle the present draws (PictureFit.rect) */
    void setPicture(boolean on, int sizePct) {
        int sz = PictureFit.size(sizePct);
        if (stretch == on && pictureSize == sz) return;
        stretch = on; pictureSize = sz;
        layout(getWidth(), getHeight()); invalidate();
    }
    private final RectF[] gbox = new RectF[GROUPS];
    // The layout editor (startEditLayout): touches move the groups and the picture instead of going to the host
    private boolean editing;
    private int dragG = -1, dragId = -1, pinchId = -1;   // dragG: a group, GROUPS for the picture
    private float lastX, lastY, pinchD0, pinchS0;
    private final RectF[] tool = new RectF[5];   // the editor's buttons: Top, Centre, Bottom, Reset, Done
    private static final String[] TOOL = {"Top", "Centre", "Bottom", "Reset", "Done"};

    private final long[] pressed = new long[MAX], prev = new long[MAX];
    private final float[] sticks = new float[4];
    private int toggleId = -1;
    private boolean toggleLong;
    private final Runnable longPress = () -> { toggleLong = true; performHapticFeedback(HapticFeedbackConstants.LONG_PRESS); settings(); };

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF rect = new RectF();

    TouchControls(Context c, boolean input) {
        super(c);
        this.input = input;
        opacity = prefs().getFloat("opacity", 0.6f);
        scale = prefs().getFloat("scale", 1f);
        haptics = prefs().getBoolean("haptics", true);
        shown = prefs().getBoolean("shown", true);
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setHapticFeedbackEnabled(true);
    }


    /** A gamepad connected (the controls hide) or the last one gone (they come back, unless the user hid them) */
    void setGamepad(boolean connected) {
        if (gamepad == connected) return;
        gamepad = connected;
        Native.note("on-screen controls " + (active() ? "shown" : "hidden") + (connected ? ": a gamepad is connected" : ": no gamepad"));
        tell();
        apply();
    }

    private boolean active() { return visible() && (shown || !button) && (mode == MODE_ON || !gamepad); }

    static final int MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2;
    static final String[] MODE_NAMES = {"Auto", "On", "Off"};
    /** The "On-screen controls" setting, kept for every game: Auto (hidden while a gamepad is connected), On or Off */
    static int mode(Context c) { int m = Prefs.open(c, "touch_controls").getInt("mode", MODE_AUTO); return m >= 0 && m < MODE_NAMES.length ? m : MODE_AUTO; }
    static void mode(Context c, int m) { Prefs.open(c, "touch_controls").edit().putInt("mode", m).commit(); }
    /** Settings › Show / hide button: the small button at the top centre that shows or hides the controls (on by default), for every game */
    static boolean toggleButton(Context c) { return Prefs.open(c, "touch_controls").getBoolean("button", true); }
    static void toggleButton(Context c, boolean on) { Prefs.open(c, "touch_controls").edit().putBoolean("button", on).commit(); }
    /** The in-game panel's switch: kept, and applied at once */
    void setToggleButton(boolean on) {
        toggleButton(getContext(), on);
        button = on;
        Native.note("on-screen controls: show / hide button " + (on ? "on" : "off"));
        apply();
    }

    /** Whether anything is drawn, the show / hide button included; onSwitch gets each change (GameActivity: the panel's handle) */
    private boolean visible() { return available && (mode == MODE_ON || mode == MODE_AUTO && !gamepad); }
    java.util.function.Consumer<Boolean> onSwitch;
    private boolean told = true;
    private void tell() { boolean v = mode == MODE_ON || mode == MODE_AUTO && !gamepad; if (v != told) { told = v; if (onSwitch != null) onSwitch.accept(v); } }

    void setMode(int m) {
        mode = m;
        Native.note("on-screen controls: " + MODE_NAMES[m] + " (" + (active() ? "shown" : "hidden") + ")");
        tell();
        apply();
    }

    private void apply() {
        if (input) Native.touchControlsEnable(active() && !editing);   // off: every touch in the picture is the game's touchscreen
        Arrays.fill(pressed, 0); Arrays.fill(prev, 0); Arrays.fill(sticks, 0);
        invalidate();
    }

    private void toggle() {
        shown = !shown;
        prefs().edit().putBoolean("shown", shown).commit();
        Native.note("on-screen controls " + (active() ? "shown" : "hidden") + " by the user");
        apply();
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        layout(w, h);
    }

    // ---- Layout: in units of 1/135 inch, scaled by the user's size, anchored to the screen's sides and its bottom, so the controls sit in the letterbox bars as far as they fit ----
    private int add(int k, long b, int s, int sh, float x, float y, float rad, String lb) {
        if (n >= MAX) return -1;
        kind[n] = k; buttons[n] = b; stick[n] = s; shape[n] = sh; cx[n] = x; cy[n] = y; r[n] = rad; label[n] = lb;
        return n++;
    }

    /** Pixels per layout unit: 1/135 inch from the screen's physical dpi, which is a dp on the phone the layout was tuned on (Honor PNM-N49's outer screen, 405 dpi at
     *  density 3), so the controls keep their physical size whatever the display size setting; the dp when the screen reports a dpi far from its density */
    private float unit() {
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        float ppi = (dm.xdpi + dm.ydpi) / 2, u = ppi / 135f;
        return u > dm.density * 0.6f && u < dm.density * 1.6f ? u : dm.density;
    }

    private void layout(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float dp = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 1, getResources().getDisplayMetrics()), u = unit();
        String key = Math.round(w / u / 40) + "x" + Math.round(h / u / 40);
        if (!key.equals(prof)) { prof = key; load(); }   // a new screen shape (a fold, an unfold): its own layout
        float[] p = PictureFit.rect(w, h, placeX, placeY, stretch, pictureSize);
        picX = p[0]; picY = p[1]; picW = p[2]; picH = p[3];
        if (input) { Native.picturePlace(placeX, placeY); Native.touchLayout(w, h, picX, picY, picW, picH); }
        // On a short screen (under 340 units high) everything shrinks with it
        float fit = Math.min(1f, h / u / 340f), d = u * scale * fit, m = 14 * u;
        // The controls take a band along the bottom no taller than a phone held in landscape (380 units, 2.8 inches): on a taller screen (a foldable's inner screen,
        // a tablet) they stay where the thumbs are, in the bottom corners, instead of spreading over its height
        float y0 = Math.max(0, h - 380 * d), bh = h - y0;
        float bar = picX * w;   // the width of each letterbox bar
        float rStick = 60 * d, rRight = 54 * d, rPad = 50 * d, rFace = 25 * d, rSh = 25 * d, rSmall = 17 * d, rClick = 17 * d, off = 1.65f * rFace;
        n = 0;
        // Shoulders in the band's top corners, ZL / ZR outside, L / R inside, minus / plus next to them
        float shY = y0 + m + 0.7f * rSh;
        add(BUTTON, ZL, 0, SHAPE_SHOULDER, m + rSh, shY, rSh, "ZL");
        add(BUTTON, L, 0, SHAPE_SHOULDER, m + 3 * rSh + 10 * d, shY, rSh, "L");
        add(BUTTON, ZR, 0, SHAPE_SHOULDER, w - m - rSh, shY, rSh, "ZR");
        add(BUTTON, R, 0, SHAPE_SHOULDER, w - m - 3 * rSh - 10 * d, shY, rSh, "R");
        add(BUTTON, MINUS, 0, SHAPE_ROUND, m + 4 * rSh + 22 * d + rSmall, shY, rSmall, "−");
        add(BUTTON, PLUS, 0, SHAPE_ROUND, w - m - 4 * rSh - 22 * d - rSmall, shY, rSmall, "+");
        // Left: the stick high, the D-pad low, centred in the bar when it is wide enough
        float top = shY + rSh + 10 * d;
        float lx = Math.max(m + rStick, bar / 2), ly = Math.max(y0 + bh * 0.45f, top + rStick);
        int ls = add(STICK, 0, 0, SHAPE_ROUND, lx, ly, rStick, "");
        add(DPAD, UP | DOWN | LEFT | RIGHT, 0, SHAPE_ROUND, Math.max(m + rPad, bar / 2), h - m - rPad, rPad, "");
        float diag = (rStick + rClick + 6 * d) * 0.7071f;
        add(BUTTON, LS, 0, SHAPE_ROUND, lx + diag, ly + diag, rClick, "L3");
        // Right: the face buttons high (A right, B bottom, X top, Y left, as on a Switch), the right stick low
        float fx = Math.min(w - m - rFace - off, w - bar / 2), fy = ly;
        add(BUTTON, A, 0, SHAPE_ROUND, fx + off, fy, rFace, "A");
        add(BUTTON, B, 0, SHAPE_ROUND, fx, fy + off, rFace, "B");
        add(BUTTON, X, 0, SHAPE_ROUND, fx, fy - off, rFace, "X");
        add(BUTTON, Y, 0, SHAPE_ROUND, fx - off, fy, rFace, "Y");
        float rx = Math.min(w - m - rRight, w - bar / 2), ry = h - m - rRight;
        add(STICK, 0, 1, SHAPE_ROUND, rx, ry, rRight, "");
        float diagR = (rRight + rClick + 6 * d) * 0.7071f;
        add(BUTTON, RS, 0, SHAPE_ROUND, rx - diagR, ry - diagR, rClick, "R3");
        // Push apart what still overlaps (a large size on a small screen): the later control moves towards the middle
        for (int pass = 0; pass < 4; pass++)
            for (int j = 0; j < n; j++)
                for (int i = 0; i < j; i++) {
                    float dx = cx[j] - cx[i], dy = cy[j] - cy[i], need = r[i] + r[j] + 2 * d;
                    if (dx * dx + dy * dy >= need * need) continue;
                    float move = (float) Math.sqrt(Math.max(0, need * need - dy * dy)) - Math.abs(dx);
                    cx[j] += cx[j] < w / 2f ? move : -move;
                }
        groups(w, h, u);
        tr = 10 * u; tx = w / 2f; ty = m + tr;   // small and faint (onDraw); its touch area is larger (onToggle)
        // The host's units: centres normalized to the view, radii in units of its height; a stick's grab ring is its own (input_map.c)
        float[] f = new float[n * 6];
        for (int i = 0; i < n; i++) {
            f[6 * i] = kind[i]; f[6 * i + 1] = buttons[i]; f[6 * i + 2] = stick[i];
            f[6 * i + 3] = cx[i] / w; f[6 * i + 4] = cy[i] / h; f[6 * i + 5] = r[i] / h;
        }
        int kept = input ? Native.touchControlsSet(f) : -1;
        boolean was = available;
        available = kept == n;
        if (!available && was != available) Native.note("on-screen controls: this host takes no layout (" + kept + "): not drawn");
        if (!editing) Native.note(String.format(java.util.Locale.ROOT, "on-screen controls: %d in a %dx%d view (%.1f dp high, %.2f px a unit, band from y %.0f, screen %s%s), picture x %.3f y %.3f, size %.2f, opacity %.2f, %s",
                n, w, h, h / dp, u, y0, prof, custom() ? ", the user's layout" : "", picX, picY, scale, opacity, active() ? "shown" : "hidden")
                + (stretch || pictureSize < PictureFit.SIZE_MAX ? ", picture " + (stretch ? "stretched" : "fitted") + " at " + pictureSize + " %" : ""));
        apply();
    }

    // ---- The user's layout: groups moved and scaled, the picture placed, kept per screen shape ----
    private int groupOf(int i) {
        long b = buttons[i];
        if (kind[i] == STICK) return stick[i] == 0 ? 0 : 3;
        if (kind[i] == DPAD) return 1;
        if (b == LS) return 0;
        if (b == RS) return 3;
        if ((b & (A | B | X | Y)) != 0) return 2;
        if ((b & (ZL | L | MINUS)) != 0) return 4;
        return 5;
    }

    /** Moves and scales each group as the user left it, around the centre of where the automatic layout put it, kept inside the view; gbox gets each group's bounds */
    private void groups(int w, int h, float u) {
        for (int g = 0; g < GROUPS; g++) {
            float sx = 0, sy = 0; int k = 0;
            for (int i = 0; i < n; i++) if (groupOf(i) == g) { sx += cx[i]; sy += cy[i]; k++; }
            if (k == 0) { gbox[g] = null; continue; }
            float mx = sx / k, my = sy / k, sc = gsc[g];
            float x0 = Float.MAX_VALUE, y0 = Float.MAX_VALUE, x1 = -Float.MAX_VALUE, y1 = -Float.MAX_VALUE;
            for (int i = 0; i < n; i++) {
                if (groupOf(i) != g) continue;
                cx[i] = mx + (cx[i] - mx) * sc + gdx[g] * u; cy[i] = my + (cy[i] - my) * sc + gdy[g] * u; r[i] *= sc;
                x0 = Math.min(x0, cx[i] - r[i]); y0 = Math.min(y0, cy[i] - r[i]); x1 = Math.max(x1, cx[i] + r[i]); y1 = Math.max(y1, cy[i] + r[i]);
            }
            float ox = x0 < 0 ? -x0 : x1 > w ? w - x1 : 0, oy = y0 < 0 ? -y0 : y1 > h ? h - y1 : 0;   // off the view: back inside
            if (ox != 0 || oy != 0) for (int i = 0; i < n; i++) if (groupOf(i) == g) { cx[i] += ox; cy[i] += oy; }
            gbox[g] = new RectF(x0 + ox, y0 + oy, x1 + ox, y1 + oy);
        }
    }

    private boolean custom() {
        for (int g = 0; g < GROUPS; g++) if (gdx[g] != 0 || gdy[g] != 0 || gsc[g] != 1) return true;
        return placeX != 0.5f || placeY != 0.5f;
    }

    private void load() {
        button = toggleButton(getContext());
        for (int g = 0; g < GROUPS; g++) {
            String[] v = prefs().getString("group" + g + "@" + prof, "0 0 1").split(" ");
            try { gdx[g] = Float.parseFloat(v[0]); gdy[g] = Float.parseFloat(v[1]); gsc[g] = Float.parseFloat(v[2]); }
            catch (RuntimeException e) { gdx[g] = 0; gdy[g] = 0; gsc[g] = 1; }
        }
        String[] v = prefs().getString("picture@" + prof, "0.5 0.5").split(" ");
        try { placeX = Float.parseFloat(v[0]); placeY = Float.parseFloat(v[1]); } catch (RuntimeException e) { placeX = placeY = 0.5f; }
    }

    private void saveLayout() {
        SharedPreferences.Editor e = prefs().edit();
        for (int g = 0; g < GROUPS; g++) e.putString("group" + g + "@" + prof, gdx[g] + " " + gdy[g] + " " + gsc[g]);
        e.putString("picture@" + prof, placeX + " " + placeY).commit();   // commit: the game process may end right after (Stop)
    }

    private void relayout() { layout(getWidth(), getHeight()); }

    /** Where the picture sits vertically on this screen: 0 top, 0.5 centred, 1 bottom (the panel's Picture row); never scaled or cropped */
    float picturePosition() { return placeY; }
    void setPicturePosition(float y) {
        if (prof == null) return;   // not laid out yet
        placeY = Math.max(0, Math.min(1, y));
        relayout(); saveLayout();
        Native.note(String.format(java.util.Locale.ROOT, "picture placed at y %.2f of the free space on screen %s", placeY, prof));
    }

    /** Run when the editor opens, before it draws: closes what opened it (the in-game panel sets it to its close) and hides what lies over the controls (Paused) */
    Runnable onEditStart;
    /** Run when the editor closes (Done): shows again what onEditStart hid */
    Runnable onEditEnd;

    boolean editing() { return editing; }

    /** The layout editor: drag a group of controls to move it, pinch it to resize it, drag the picture to move it into the black bars; Done keeps it for this screen shape */
    void startEditLayout() {
        if (editing || prof == null) return;
        if (onEditStart != null) onEditStart.run();
        editing = true; shown = true; gamepad = false;
        Native.note("on-screen controls: the layout editor is open (screen " + prof + ")");
        apply();
    }

    private void endEditLayout() {
        editing = false; dragG = -1; dragId = -1; pinchId = -1;
        saveLayout(); relayout();
        if (onEditEnd != null) onEditEnd.run();
    }

    private void resetLayout() {
        Arrays.fill(gdx, 0); Arrays.fill(gdy, 0); Arrays.fill(gsc, 1); placeX = placeY = 0.5f;
        relayout(); saveLayout();
    }

    /** The picture's position on this screen (the in-game panel's Picture rows): Top / Centre / Bottom of the free space */
    void positionRows(Context ctx, LinearLayout box) {
        TextView t = new TextView(ctx);
        t.setText("Position (this screen)");
        android.widget.RadioGroup rg = new android.widget.RadioGroup(ctx);
        rg.setOrientation(LinearLayout.HORIZONTAL);
        String[] names = {"Top", "Centre", "Bottom"};
        float[] at = {0, 0.5f, 1};
        for (int i = 0; i < 3; i++) {
            android.widget.RadioButton b = new android.widget.RadioButton(ctx);
            b.setText(names[i]); b.setId(4100 + i);   // fixed: the panel is built again on each open and each look change (generateViewId would climb into the other rows' ids)
            rg.addView(b);
            if (Math.abs(placeY - at[i]) < 0.01f) b.setChecked(true);
            float y = at[i];
            b.setOnCheckedChangeListener((v, on) -> { if (on) setPicturePosition(y); });
        }
        box.addView(t); box.addView(rg);
    }

    // ---- The editor's touches ----
    private boolean editTouch(MotionEvent e) {
        int act = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        switch (act) {
            case MotionEvent.ACTION_DOWN: {
                for (int k = 0; k < tool.length; k++) if (tool[k] != null && tool[k].contains(x, y)) { toolTap(k); return true; }
                dragG = -1;
                float pad = 10 * unit();
                for (int g = 0; g < GROUPS; g++)
                    if (gbox[g] != null && x >= gbox[g].left - pad && x <= gbox[g].right + pad && y >= gbox[g].top - pad && y <= gbox[g].bottom + pad) { dragG = g; break; }
                if (dragG < 0 && x >= picX * getWidth() && x <= (picX + picW) * getWidth() && y >= picY * getHeight() && y <= (picY + picH) * getHeight()) dragG = GROUPS;
                dragId = id; lastX = x; lastY = y;
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_POINTER_DOWN:
                if (dragG >= 0 && dragG < GROUPS && pinchId < 0) {
                    int j = e.findPointerIndex(dragId);
                    if (j < 0) return true;
                    pinchId = id; pinchD0 = (float) Math.hypot(x - e.getX(j), y - e.getY(j)); pinchS0 = gsc[dragG];
                }
                return true;
            case MotionEvent.ACTION_MOVE: {
                int j = e.findPointerIndex(dragId);
                if (dragG < 0 || j < 0) return true;
                float mx = e.getX(j), my = e.getY(j), u = unit();
                if (pinchId >= 0) {
                    int q = e.findPointerIndex(pinchId);
                    if (q >= 0 && pinchD0 > 0) gsc[dragG] = Math.max(0.5f, Math.min(2f, pinchS0 * (float) Math.hypot(mx - e.getX(q), my - e.getY(q)) / pinchD0));
                } else if (dragG < GROUPS) {
                    gdx[dragG] += (mx - lastX) / u; gdy[dragG] += (my - lastY) / u;
                } else {
                    float fx = getWidth() * (1 - picW), fy = getHeight() * (1 - picH);
                    if (fx > 1) placeX = snap(placeX + (mx - lastX) / fx);
                    if (fy > 1) placeY = snap(placeY + (my - lastY) / fy);
                }
                lastX = mx; lastY = my;
                relayout();
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP:
                if (id == pinchId) pinchId = -1;
                else if (id == dragId) { dragG = -1; dragId = -1; pinchId = -1; }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (dragG == GROUPS) { placeX = snapEnd(placeX); placeY = snapEnd(placeY); relayout(); }
                dragG = -1; dragId = -1; pinchId = -1;
                saveLayout(); invalidate();
                return true;
            default:
                return true;
        }
    }

    // The picture's position: free while dragged (clamped), pulled to the top, the centre and the bottom near them
    private static float snap(float v) { return Math.max(0, Math.min(1, v)); }
    private static float snapEnd(float v) {
        for (float t : new float[] {0, 0.5f, 1}) if (Math.abs(v - t) < 0.06f) return t;
        return v;
    }

    private void toolTap(int k) {
        if (haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (k <= 2) { placeY = k * 0.5f; relayout(); saveLayout(); }
        else if (k == 3) resetLayout();
        else { endEditLayout(); Native.note("on-screen controls: layout kept for screen " + prof + (custom() ? "" : " (the automatic one)")); }
        invalidate();
    }

    // ---- Touches: the show / hide button is the launcher's; every other pointer goes to the host ----
    private void send(int action, int id, float x, float y) { if (input) Native.touch(action, id, x / getWidth(), y / getHeight()); }

    private boolean onToggle(float x, float y) { float dx = x - tx, dy = y - ty, g = tr * 2.4f; return button && visible() && dx * dx + dy * dy <= g * g; }   // 24 units: a 48 dp target

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (editing) return editTouch(e);
        int act = e.getActionMasked();
        switch (act) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex(), id = e.getPointerId(i);
                if (toggleId < 0 && onToggle(e.getX(i), e.getY(i))) {
                    toggleId = id; toggleLong = false;
                    postDelayed(longPress, 600);
                    if (haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    return true;
                }
                send(act, id, e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) if (e.getPointerId(i) != toggleId) send(act, e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex(), id = e.getPointerId(i);
                if (id == toggleId) {
                    toggleId = -1;
                    removeCallbacks(longPress);
                    if (!toggleLong && onToggle(e.getX(i), e.getY(i))) toggle();
                    return true;
                }
                send(act, id, e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                toggleId = -1;
                removeCallbacks(longPress);
                send(act, 0, 0, 0);
                break;
            default:
                return true;
        }
        poll();
        return true;
    }

    /** What the controls hold after a touch: a tick for each new press, a redraw when anything changed */
    private void poll() {
        if (!active()) return;
        float s0 = sticks[0], s1 = sticks[1], s2 = sticks[2], s3 = sticks[3];
        Native.touchControlsState(pressed, sticks);
        boolean changed = s0 != sticks[0] || s1 != sticks[1] || s2 != sticks[2] || s3 != sticks[3], tick = false;
        for (int i = 0; i < n; i++) {
            if (pressed[i] != prev[i]) changed = true;
            if ((pressed[i] & ~prev[i]) != 0) tick = true;
            prev[i] = pressed[i];
        }
        if (tick && haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (changed) invalidate();
    }

    // ---- Drawing ----
    private int alpha(float a) { return Math.max(0, Math.min(255, Math.round(255 * a * opacity))); }

    @Override protected void onDraw(Canvas c) {
        if (!available || !visible() && !editing) return;
        float dp = getResources().getDisplayMetrics().density;
        line.setStrokeWidth(Math.max(1.5f, 1.6f * dp));
        if (editing) { drawControls(c); drawEditor(c, dp); return; }
        // The show / hide button (when it is on): a small pad outline, mostly transparent over the game, fainter still while the controls are hidden
        boolean on = active();
        if (button) {
            float ta = on ? 1f : 0.7f;
            fill.setColor(Color.argb(Math.round(255 * 0.15f * ta), 0, 0, 0));
            c.drawCircle(tx, ty, tr, fill);
            line.setColor(Color.argb(Math.round(255 * 0.4f * ta), 255, 255, 255));
            c.drawCircle(tx, ty, tr, line);
            rect.set(tx - tr * 0.6f, ty - tr * 0.3f, tx + tr * 0.6f, ty + tr * 0.35f);
            c.drawRoundRect(rect, tr * 0.3f, tr * 0.3f, line);
            if (!on) c.drawLine(tx - tr * 0.65f, ty + tr * 0.65f, tx + tr * 0.65f, ty - tr * 0.65f, line);
        }
        if (!on) return;
        drawControls(c);
    }

    private void drawControls(Canvas c) {
        for (int i = 0; i < n; i++) {
            boolean held = pressed[i] != 0;
            fill.setColor(held ? Color.argb(alpha(0.55f), 255, 255, 255) : Color.argb(alpha(0.35f), 0, 0, 0));
            line.setColor(Color.argb(alpha(0.9f), 255, 255, 255));
            if (kind[i] == STICK) drawStick(c, i);
            else if (kind[i] == DPAD) drawDpad(c, i);
            else {
                if (shape[i] == SHAPE_SHOULDER) {
                    rect.set(cx[i] - r[i], cy[i] - r[i] * 0.62f, cx[i] + r[i], cy[i] + r[i] * 0.62f);
                    c.drawRoundRect(rect, r[i] * 0.5f, r[i] * 0.5f, fill);
                    c.drawRoundRect(rect, r[i] * 0.5f, r[i] * 0.5f, line);
                } else {
                    c.drawCircle(cx[i], cy[i], r[i], fill);
                    c.drawCircle(cx[i], cy[i], r[i], line);
                }
                text.setColor(held ? Color.argb(alpha(1f), 0, 0, 0) : Color.argb(alpha(1f), 255, 255, 255));
                text.setTextSize(r[i] * (label[i].length() > 1 ? 0.72f : 0.95f));
                c.drawText(label[i], cx[i], cy[i] - (text.descent() + text.ascent()) / 2, text);
            }
        }
    }

    /** The editor: the picture's outline, each group's frame (the one held filled), the buttons along the top and what to do */
    private void drawEditor(Canvas c, float dp) {
        int w = getWidth(), h = getHeight();
        float u = unit();
        line.setColor(Color.argb(200, 255, 210, 60));
        rect.set(picX * w, picY * h, (picX + picW) * w, (picY + picH) * h);
        if (dragG == GROUPS) { fill.setColor(Color.argb(50, 255, 210, 60)); c.drawRect(rect, fill); }
        c.drawRect(rect, line);
        for (int g = 0; g < GROUPS; g++) {
            if (gbox[g] == null) continue;
            line.setColor(g == dragG ? Color.argb(255, 255, 210, 60) : Color.argb(160, 255, 255, 255));
            rect.set(gbox[g]); rect.inset(-4 * u, -4 * u);
            if (g == dragG) { fill.setColor(Color.argb(60, 255, 210, 60)); c.drawRoundRect(rect, 8 * u, 8 * u, fill); }
            c.drawRoundRect(rect, 8 * u, 8 * u, line);
        }
        float bw = 74 * u, bh = 34 * u, gap = 8 * u, x = (w - (TOOL.length * bw + (TOOL.length - 1) * gap)) / 2, y = 14 * u;
        text.setTextSize(15 * u);
        for (int k = 0; k < TOOL.length; k++, x += bw + gap) {
            if (tool[k] == null) tool[k] = new RectF();
            tool[k].set(x, y, x + bw, y + bh);
            boolean cur = k <= 2 && Math.abs(placeY - k * 0.5f) < 0.01f;
            fill.setColor(k == 4 ? Color.argb(230, 255, 210, 60) : cur ? Color.argb(230, 255, 255, 255) : Color.argb(200, 40, 40, 40));
            c.drawRoundRect(tool[k], 8 * u, 8 * u, fill);
            line.setColor(Color.argb(220, 255, 255, 255));
            c.drawRoundRect(tool[k], 8 * u, 8 * u, line);
            text.setColor(k == 4 || cur ? Color.BLACK : Color.WHITE);
            c.drawText(TOOL[k], tool[k].centerX(), tool[k].centerY() - (text.descent() + text.ascent()) / 2, text);
        }
        text.setTextSize(13 * u);
        text.setColor(Color.argb(230, 255, 255, 255));
        String hint = dragG >= 0 && dragG < GROUPS ? GROUP_NAME[dragG] + ": drag to move, a second finger to resize (" + Math.round(gsc[dragG] * 100) + " %)"
                : dragG == GROUPS ? "picture: drag into the black bars" : "Drag a control or the picture; pinch a control to resize it";
        c.drawText(hint, w / 2f, y + bh + 22 * u, text);
    }

    private void drawStick(Canvas c, int i) {
        float sx = sticks[2 * stick[i]], sy = sticks[2 * stick[i] + 1];
        boolean held = sx != 0 || sy != 0;
        fill.setColor(Color.argb(alpha(0.25f), 0, 0, 0));
        c.drawCircle(cx[i], cy[i], r[i], fill);
        c.drawCircle(cx[i], cy[i], r[i], line);
        float k = r[i] * 0.55f, kx = cx[i] + sx * k, ky = cy[i] - sy * k;
        fill.setColor(held ? Color.argb(alpha(0.6f), 255, 255, 255) : Color.argb(alpha(0.4f), 90, 90, 90));
        c.drawCircle(kx, ky, r[i] * 0.45f, fill);
        c.drawCircle(kx, ky, r[i] * 0.45f, line);
    }

    private void drawDpad(Canvas c, int i) {
        float x = cx[i], y = cy[i], rr = r[i], w = rr * 0.36f;
        fill.setColor(Color.argb(alpha(0.25f), 0, 0, 0));
        c.drawCircle(x, y, rr, fill);
        c.drawCircle(x, y, rr, line);
        long[] dirs = {UP, RIGHT, DOWN, LEFT};
        for (int k = 0; k < 4; k++) {
            boolean held = (pressed[i] & dirs[k]) != 0;
            path.reset();
            // a triangle pointing away from the centre, rotated a quarter turn per direction
            float[] pts = {0, -rr * 0.82f, w, -rr * 0.38f, -w, -rr * 0.38f};
            for (int j = 0; j < 6; j += 2) {
                float px = pts[j], py = pts[j + 1];
                for (int t = 0; t < k; t++) { float q = px; px = -py; py = q; }
                if (j == 0) path.moveTo(x + px, y + py); else path.lineTo(x + px, y + py);
            }
            path.close();
            fill.setColor(held ? Color.argb(alpha(0.9f), 255, 255, 255) : Color.argb(alpha(0.35f), 255, 255, 255));
            c.drawPath(path, fill);
        }
    }

    // ---- Settings: opacity, size and the haptic tick, kept in the app's preferences ----
    /** What a long press on the show / hide button opens: GameActivity's in-game panel (SidePanel), which holds these rows (controlRows) */
    Runnable openSettings;
    private void settings() { if (openSettings != null) openSettings.run(); }

    /** The on-screen controls' rows of the in-game panel: Auto / On / Off, opacity, size, the haptic tick, the log over the game, Reset and the layout editor; each applies at once (kept for every game) */
    void controlRows(Context ctx, LinearLayout box) {
        RadioGroup mg = new RadioGroup(ctx);
        mg.setOrientation(RadioGroup.HORIZONTAL);
        for (int i = 0; i < MODE_NAMES.length; i++) { RadioButton b = new RadioButton(ctx); b.setId(4000 + i); b.setText(MODE_NAMES[i]); mg.addView(b); }
        mg.check(4000 + mode);
        mg.setOnCheckedChangeListener((g, id) -> { mode(ctx, id - 4000); setMode(id - 4000); });
        box.addView(mg);
        TextView ol = new TextView(ctx), sl = new TextView(ctx);
        SeekBar os = new SeekBar(ctx), ss = new SeekBar(ctx);
        CheckBox hb = new CheckBox(ctx), lb = new CheckBox(ctx), tb = new CheckBox(ctx);
        os.setMax(90); os.setProgress(Math.round(opacity * 100) - 10);   // 10..100 %
        ss.setMax(90); ss.setProgress(Math.round(scale * 100) - 60);     // 60..150 %
        os.setKeyProgressIncrement(5); ss.setKeyProgressIncrement(5);   // the D-pad's left / right: 5 % a press
        hb.setText("Vibrate on press"); hb.setChecked(haptics);
        lb.setText("Show the log over the game"); lb.setChecked(showLog(ctx));
        tb.setText("Show / hide button (top centre)"); tb.setChecked(button);
        tb.setOnCheckedChangeListener((b, v) -> setToggleButton(v));
        Runnable labels = () -> { ol.setText("Opacity " + (os.getProgress() + 10) + " %"); sl.setText("Size " + (ss.getProgress() + 60) + " %"); };
        labels.run();
        SeekBar.OnSeekBarChangeListener live = new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean user) {
                labels.run();
                opacity = (os.getProgress() + 10) / 100f;
                float ns = (ss.getProgress() + 60) / 100f;
                if (ns != scale) { scale = ns; layout(getWidth(), getHeight()); }
                invalidate();
                save();
            }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        };
        os.setOnSeekBarChangeListener(live);
        ss.setOnSeekBarChangeListener(live);
        hb.setOnCheckedChangeListener((b, v) -> { haptics = v; save(); });
        lb.setOnCheckedChangeListener((b, v) -> prefs().edit().putBoolean("log", v).commit());
        android.widget.Button reset = new android.widget.Button(ctx);
        reset.setText("Reset the controls");
        reset.setOnClickListener(v -> { os.setProgress(50); ss.setProgress(40); hb.setChecked(true); });   // 60 % opacity, 100 % size, the tick on
        box.addView(ol); box.addView(os); box.addView(sl); box.addView(ss); box.addView(hb); box.addView(lb); box.addView(tb); box.addView(reset);
        android.widget.Button ed = new android.widget.Button(ctx);   // the layout editor (runs onEditStart); the picture's position is in the panel's Picture rows (positionRows)
        ed.setText("Edit the controls layout…");
        ed.setOnClickListener(v -> startEditLayout());
        box.addView(ed);
    }

    /** Whether GameActivity keeps the newest log lines over the picture once the game draws (off by default; the settings dialog's last box) */
    static boolean showLog(Context c) { return Prefs.open(c, "touch_controls").getBoolean("log", false); }

    private void save() { prefs().edit().putFloat("opacity", opacity).putFloat("scale", scale).putBoolean("haptics", haptics).commit(); }
}
