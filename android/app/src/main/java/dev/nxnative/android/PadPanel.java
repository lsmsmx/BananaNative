package dev.nxnative.android;

/**
 * The in-game panel with a controller (SidePanel, GameActivity): the button combination that opens and closes it, the stick and D-pad as directions with repeat,
 * the panel's confirm and back buttons from the game's own Button mapping, and the steps of a radio group and the tabs. Pure Java, no Android classes, so the JVM
 * test (PadPanelJvmTest, dm/tools/tests/test_android_padpanel.py) checks it.
 * <p>
 * The combination is one a game never needs on its own: the Home / guide button (KEYCODE_BUTTON_MODE, which the host never takes), or Select and Start (the Switch's
 * − and +) held together for HOLD_MS. Both keys of the held pair still reach the game while they are held (a game that reads + alone sees it); when the hold fires
 * the panel opens, the game's held buttons are released (GameActivity.releaseGameInput) and the pair's own releases are eaten.
 */
final class PadPanel {
    private PadPanel() {}

    // android.view.KeyEvent's KEYCODE_ numbers
    static final int KEY_DPAD_UP = 19, KEY_DPAD_DOWN = 20, KEY_DPAD_LEFT = 21, KEY_DPAD_RIGHT = 22, KEY_BUTTON_A = 96, KEY_BUTTON_B = 97,
            KEY_L1 = 102, KEY_R1 = 103, KEY_START = 108, KEY_SELECT = 109, KEY_MODE = 110;

    /** Settings › Controller: in-game panel (Prefs.panelPad), for every game */
    static final int OPEN_BOTH = 0, OPEN_HOME = 1, OPEN_MINUS_PLUS = 2, OPEN_OFF = 3;
    static final String[] OPEN_NAMES = {"Home, or hold − and + (half a second)", "Home button only", "Hold − and + only (half a second)", "Off"};
    static final long HOLD_MS = 500;

    static int open(int v) { return v >= 0 && v < OPEN_NAMES.length ? v : OPEN_BOTH; }

    /** What the combination detector says about one key event */
    static final int PASS = 0, EAT = 1, TOGGLE = 2;

    /** The open / close combination, over every connected controller */
    static final class Combo {
        private final int how;
        private boolean select, start, eatSelect, eatStart;
        private long since = -1;

        Combo(int how) { this.how = open(how); }

        private boolean home() { return how == OPEN_BOTH || how == OPEN_HOME; }
        private boolean pair() { return how == OPEN_BOTH || how == OPEN_MINUS_PLUS; }

        /** A key event (time t in ms): PASS goes on to the game or the panel, EAT is consumed, TOGGLE opens or closes the panel */
        int key(int code, boolean down, int repeat, long t) {
            if (code == KEY_MODE) return !home() ? PASS : down ? EAT : TOGGLE;   // on its release, as a press
            if (code != KEY_SELECT && code != KEY_START || !pair()) return PASS;
            boolean sel = code == KEY_SELECT;
            if (!down) {
                boolean eat = sel ? eatSelect : eatStart;
                if (sel) { select = false; eatSelect = false; } else { start = false; eatStart = false; }
                since = -1;
                return eat ? EAT : PASS;
            }
            if (sel ? eatSelect : eatStart) return EAT;   // a repeat of a pair that fired
            if (repeat > 0) return PASS;
            if (sel) select = true; else start = true;
            if (select && start && since < 0) since = t;
            return PASS;
        }

        /** When the held pair is due (ms), -1 when it is not held */
        long due() { return since < 0 ? -1 : since + HOLD_MS; }

        /** A timer at due(): true when the pair has been held long enough; its keys are then eaten until they come up */
        boolean fire(long t) {
            if (since < 0 || !select || !start || t < since + HOLD_MS) return false;
            since = -1;
            eatSelect = eatStart = true;
            return true;
        }

        /** The window lost its focus (a dialog, the background): the keys' releases may never come */
        void reset() { select = start = eatSelect = eatStart = false; since = -1; }
    }

    /** Directions */
    static final int NONE = 0, UP = 1, DOWN = 2, LEFT = 3, RIGHT = 4;
    /** A stick past ON points somewhere; it lets go under OFF (so a stick resting near the threshold does not flicker) */
    static final float ON = 0.6f, OFF = 0.35f;

    static int ofKey(int code) {
        return code == KEY_DPAD_UP ? UP : code == KEY_DPAD_DOWN ? DOWN : code == KEY_DPAD_LEFT ? LEFT : code == KEY_DPAD_RIGHT ? RIGHT : NONE;
    }

    private static float along(int dir, float x, float y) { return dir == UP ? -y : dir == DOWN ? y : dir == LEFT ? -x : dir == RIGHT ? x : 0; }

    /** A stick or hat (x right, y down, -1..1) as a direction, held: the direction it pointed at before; the larger axis wins */
    static int stick(float x, float y, int held) {
        float ax = Math.abs(x), ay = Math.abs(y), m = Math.max(ax, ay);
        int d = ax >= ay ? (x < 0 ? LEFT : RIGHT) : (y < 0 ? UP : DOWN);
        if (held != NONE && along(held, x, y) > OFF && (d == held || m < ON)) return held;
        return m >= ON ? d : NONE;
    }

    /** A held direction moves again after this many ms: the n-th repeat (0: the first, after the press) */
    static long repeatDelay(int n) { return n == 0 ? 400 : 90; }

    /** A radio group's next choice from cur by delta (-1, +1), over the ones that can be chosen; cur when there is none */
    static int step(boolean[] can, int cur, int delta) {
        for (int i = cur + delta; i >= 0 && i < can.length; i += delta) if (can[i]) return i;
        return cur;
    }

    /** The tab delta away from cur among n, round */
    static int tab(int cur, int delta, int n) { return n <= 0 ? 0 : ((cur + delta) % n + n) % n; }

    /**
     * The panel's confirm and back keys: the controller buttons that are the Switch's A and B in this game (DM_PAD_MAP over the default for labelled, as the host
     * reads them), so the panel confirms where the game does (by position, the right button; with Xbox button label swap's "swap buttons A/B", the one labelled A).
     * A D-pad or shoulder source is skipped (they move through the panel); without one, the positions' default.
     */
    static int[] confirmBack(String spec, int labelled) {
        PadMap m = PadMap.parse(spec, labelled);
        int a = -1, b = -1;
        for (int i = 0; i < PadMap.SOURCES.length; i++) {
            int k = PadMap.KEYCODES[i];
            if (ofKey(k) != NONE || k == KEY_L1 || k == KEY_R1) continue;
            if (m.target[i] == 0 && a < 0) a = k;
            if (m.target[i] == 1 && b < 0) b = k;
        }
        if (a < 0) a = b != KEY_BUTTON_B ? KEY_BUTTON_B : KEY_BUTTON_A;
        if (b < 0) b = a != KEY_BUTTON_A ? KEY_BUTTON_A : KEY_BUTTON_B;
        return new int[] {a, b};
    }
}
