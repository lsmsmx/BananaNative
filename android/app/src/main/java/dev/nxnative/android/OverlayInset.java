package dev.nxnative.android;

/**
 * How far the overlay's text blocks sit in from the window's top corners. The game window is edge to edge, so a display with rounded corners cuts a block that is flush in
 * its corner (issue #105: the top-right items were half hidden). A corner of radius r is a quarter circle; a block whose top edge is t below the top is clear of it when its
 * side edge is x = r - sqrt(r^2 - (r - t)^2) in. t is 30 % of r, which puts the block's top corner on the curve at x of about 29 % of r, near the corner but not far down the screen. A display
 * cutout on the side (landscape: the camera) takes the larger of the two. Pure Java: checked on the JVM (OverlayInsetJvmTest).
 */
final class OverlayInset {
    private OverlayInset() {}

    static final float DOWN = 0.3f;   // of the corner radius

    /** How far a block must sit below the top for a corner of radius r (px) */
    static int top(int r) { return r <= 0 ? 0 : Math.round(r * DOWN); }

    /** How far a block, top(r) below the top, must sit in from the side for a corner of radius r (px), plus one px against rounding */
    static int side(int r) {
        if (r <= 0) return 0;
        float t = top(r), d = r - t;
        return (int) Math.ceil(r - Math.sqrt(Math.max(0f, (double) r * r - d * d))) + 1;
    }

    /** {left, top} of a block in the window's top-left corner and {right, top} of one in the top-right; radii and cutout insets in px (0 = none) */
    static int[] topLeft(int radius, int cutLeft, int cutTop) { return new int[] {Math.max(side(radius), cutLeft), Math.max(top(radius), cutTop)}; }
    static int[] topRight(int radius, int cutRight, int cutTop) { return new int[] {Math.max(side(radius), cutRight), Math.max(top(radius), cutTop)}; }
}
