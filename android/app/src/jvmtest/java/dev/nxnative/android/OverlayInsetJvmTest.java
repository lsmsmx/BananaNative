package dev.nxnative.android;

/** OverlayInset (the pure rule); prints "ok" */
public final class OverlayInsetJvmTest {
    static void check(boolean c, String what) { if (!c) { System.err.println("FAIL: " + what); System.exit(1); } }

    public static void main(String[] a) {
        check(OverlayInset.top(0) == 0 && OverlayInset.side(0) == 0, "square corner: no inset");
        int r = 100, t = OverlayInset.top(r), s = OverlayInset.side(r);
        check(t == 30, "top " + t);
        // the block's top corner (s in, t down) is visible when it is within r of the corner circle's centre (r, r)
        double dx = r - s, dy = r - t;
        check(Math.sqrt(dx * dx + dy * dy) <= r, "corner of the block is inside the visible quarter circle: " + s);
        check(s > 0 && s < r, "side " + s);
        check(OverlayInset.side(200) > OverlayInset.side(100), "grows with the radius");
        int[] tr = OverlayInset.topRight(100, 150, 5);
        check(tr[0] == 150 && tr[1] == 30, "cutout wins on the side, corner on the top");
        int[] tl = OverlayInset.topLeft(100, 0, 60);
        check(tl[0] == s && tl[1] == 60, "cutout wins on the top");
        System.out.println("ok");
    }
}
