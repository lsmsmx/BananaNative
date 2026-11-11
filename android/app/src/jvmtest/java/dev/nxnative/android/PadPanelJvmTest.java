package dev.nxnative.android;

/** PadPanel on the JVM (dm/tools/tests/test_android_padpanel.py): the open / close combination, the stick's directions, the repeat, the steps and the confirm / back keys. Prints "ok" or throws */
public final class PadPanelJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }

    public static void main(String[] args) {
        final int SEL = PadPanel.KEY_SELECT, START = PadPanel.KEY_START, MODE = PadPanel.KEY_MODE;

        // Home: eaten on its press, toggles on its release; passed on when the setting leaves it out
        PadPanel.Combo c = new PadPanel.Combo(PadPanel.OPEN_BOTH);
        check(c.key(MODE, true, 0, 0) == PadPanel.EAT && c.key(MODE, false, 0, 10) == PadPanel.TOGGLE, "Home toggles on release");
        check(new PadPanel.Combo(PadPanel.OPEN_MINUS_PLUS).key(MODE, false, 0, 0) == PadPanel.PASS, "Home off: passed on");
        check(new PadPanel.Combo(PadPanel.OPEN_OFF).key(MODE, false, 0, 0) == PadPanel.PASS, "Off: Home passed on");
        check(new PadPanel.Combo(99).key(MODE, false, 0, 0) == PadPanel.TOGGLE, "a stored value out of range reads as the default");

        // − and + held: both reach the game while held; after HOLD_MS the timer fires and their releases are eaten
        c = new PadPanel.Combo(PadPanel.OPEN_BOTH);
        check(c.key(SEL, true, 0, 1000) == PadPanel.PASS && c.due() < 0, "one key alone: no timer");
        check(c.key(START, true, 0, 1100) == PadPanel.PASS && c.due() == 1100 + PadPanel.HOLD_MS, "the pair held: due HOLD_MS after the second key");
        check(!c.fire(1100 + PadPanel.HOLD_MS - 1), "not before the hold");
        check(c.key(START, true, 3, 1300) == PadPanel.PASS && c.due() == 1100 + PadPanel.HOLD_MS, "a repeat does not restart the hold");
        check(c.fire(1100 + PadPanel.HOLD_MS), "fires at the hold");
        check(!c.fire(5000), "fires once");
        check(c.key(START, true, 4, 1700) == PadPanel.EAT, "a repeat after it fired is eaten");
        check(c.key(SEL, false, 0, 1800) == PadPanel.EAT && c.key(START, false, 0, 1810) == PadPanel.EAT, "the pair's releases are eaten");
        check(c.key(START, true, 0, 2000) == PadPanel.PASS && c.key(START, false, 0, 2100) == PadPanel.PASS, "then + alone is the game's again");

        // Let go early: no toggle
        c = new PadPanel.Combo(PadPanel.OPEN_MINUS_PLUS);
        c.key(SEL, true, 0, 0); c.key(START, true, 0, 10);
        check(c.key(SEL, false, 0, 200) == PadPanel.PASS && c.due() < 0 && !c.fire(10 + PadPanel.HOLD_MS), "released before the hold: nothing");
        c.key(SEL, true, 0, 300);
        check(c.due() == 300 + PadPanel.HOLD_MS, "pressed again: a new hold from then");
        c.reset();
        check(c.due() < 0 && !c.fire(10000), "reset drops the hold");
        check(new PadPanel.Combo(PadPanel.OPEN_HOME).key(SEL, true, 0, 0) == PadPanel.PASS, "Home only: the pair is the game's");
        PadPanel.Combo h = new PadPanel.Combo(PadPanel.OPEN_HOME);
        h.key(SEL, true, 0, 0); h.key(START, true, 0, 0);
        check(h.due() < 0 && !h.fire(10000), "Home only: no hold");
        check(c.key(PadPanel.KEY_BUTTON_A, true, 0, 0) == PadPanel.PASS, "other keys pass");

        // The stick: past ON points, lets go under OFF, the larger axis wins
        check(PadPanel.stick(0, 0, PadPanel.NONE) == PadPanel.NONE, "centred");
        check(PadPanel.stick(0.5f, 0, PadPanel.NONE) == PadPanel.NONE, "under ON");
        check(PadPanel.stick(0.7f, 0.1f, PadPanel.NONE) == PadPanel.RIGHT && PadPanel.stick(-0.7f, 0, PadPanel.NONE) == PadPanel.LEFT, "left / right");
        check(PadPanel.stick(0.1f, -0.9f, PadPanel.NONE) == PadPanel.UP && PadPanel.stick(0.2f, 0.8f, PadPanel.NONE) == PadPanel.DOWN, "up / down (y down)");
        check(PadPanel.stick(0, 0.4f, PadPanel.DOWN) == PadPanel.DOWN, "held over OFF");
        check(PadPanel.stick(0, 0.3f, PadPanel.DOWN) == PadPanel.NONE, "let go under OFF");
        check(PadPanel.stick(0.9f, 0.5f, PadPanel.DOWN) == PadPanel.RIGHT, "turned to another direction past ON");
        check(PadPanel.stick(0.55f, 0.5f, PadPanel.DOWN) == PadPanel.DOWN, "a diagonal under ON keeps the held one");
        check(PadPanel.stick(0, -1, PadPanel.NONE) == PadPanel.UP && PadPanel.stick(1, 0, PadPanel.NONE) == PadPanel.RIGHT, "the hat");
        check(PadPanel.ofKey(PadPanel.KEY_DPAD_UP) == PadPanel.UP && PadPanel.ofKey(PadPanel.KEY_DPAD_RIGHT) == PadPanel.RIGHT && PadPanel.ofKey(96) == PadPanel.NONE, "keys");

        // Repeat: a pause, then quick steps
        check(PadPanel.repeatDelay(0) > PadPanel.repeatDelay(1) && PadPanel.repeatDelay(1) == PadPanel.repeatDelay(7) && PadPanel.repeatDelay(1) >= 50, "repeat");

        // Radio steps skip what cannot be chosen and stop at the ends
        boolean[] can = {true, false, true, true, false};
        check(PadPanel.step(can, 0, 1) == 2 && PadPanel.step(can, 2, -1) == 0 && PadPanel.step(can, 3, 1) == 3 && PadPanel.step(can, 0, -1) == 0, "steps");
        check(PadPanel.step(new boolean[] {false, true}, 1, -1) == 1, "nothing to step to");
        check(PadPanel.tab(0, -1, 4) == 3 && PadPanel.tab(3, 1, 4) == 0 && PadPanel.tab(1, 1, 4) == 2 && PadPanel.tab(0, 1, 0) == 0, "tabs go round");

        // Confirm / back follow the game's mapping: by position the right button (BUTTON_B) is the Switch's A
        int[] cb = PadPanel.confirmBack("", 0);
        check(cb[0] == PadPanel.KEY_BUTTON_B && cb[1] == PadPanel.KEY_BUTTON_A, "by position: confirm right, back bottom");
        cb = PadPanel.confirmBack("", 2);
        check(cb[0] == PadPanel.KEY_BUTTON_A && cb[1] == PadPanel.KEY_BUTTON_B, "swap buttons A/B: by label");
        cb = PadPanel.confirmBack("BUTTON_X=A,BUTTON_B=Y", 0);
        check(cb[0] == 99 && cb[1] == PadPanel.KEY_BUTTON_A, "a mapped A: " + cb[0] + " " + cb[1]);
        cb = PadPanel.confirmBack("DPAD_UP=A,BUTTON_B=UP", 0);
        check(cb[0] == PadPanel.KEY_BUTTON_B && cb[1] == PadPanel.KEY_BUTTON_A, "A on the D-pad: the default instead: " + cb[0] + " " + cb[1]);
        check(cb[0] != cb[1], "two keys");
        System.out.println("ok");
    }
}
