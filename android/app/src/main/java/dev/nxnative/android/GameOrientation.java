package dev.nxnative.android;

/**
 * Whether the game window asks for a fixed landscape orientation. GameActivity's manifest entry says sensorLandscape, right on a phone: the display turns and the window
 * fills it. On a large screen (smallest width 600 dp and up: a tablet, a foldable's inner screen) the system does not turn the display for an app's orientation request
 * (it ignores the request there, so apps built for phones keep their place), it keeps the display's own orientation and letterboxes the landscape-locked window inside it:
 * on a portrait-shaped 2172x2352 inner screen the window was 2172x2006 with the wallpaper below. The present fits the game's 16:9 picture into any window shape
 * (vka_present_fit.h), so a near-square large screen gets no request and the window the whole display; a phone keeps sensorLandscape. The size is the display's own
 * (WindowManager.getMaximumWindowMetrics), never the window's, which the letterbox itself shrinks. Pure Java: checked on the JVM (GameOrientationJvmTest).
 */
final class GameOrientation {
    private GameOrientation() {}

    static final int LARGE_SW_DP = 600;   // the platform's own line between a phone and a large screen
    // Only a near-square large screen drops the request (a foldable's inner screen, ~1.08:1): there a portrait or a landscape window fits the 16:9 picture about equally.
    // A tablet (1.33:1 and longer) keeps sensorLandscape: where its system honours the request the display turns and the window fills it; without the request a tablet held
    // upright would give the game a portrait window with a small picture
    static final float NEAR_SQUARE = 1.25f;

    /** True = keep the manifest's sensorLandscape; false = no orientation request (the window fills the display). widthPx x heightPx: the display area, density: px per dp */
    static boolean fixedLandscape(int widthPx, int heightPx, float density) {
        if (widthPx <= 0 || heightPx <= 0 || density <= 0) return true;
        int lo = Math.min(widthPx, heightPx), hi = Math.max(widthPx, heightPx);
        return lo / density < LARGE_SW_DP || (float)hi / lo >= NEAR_SQUARE;
    }
}
