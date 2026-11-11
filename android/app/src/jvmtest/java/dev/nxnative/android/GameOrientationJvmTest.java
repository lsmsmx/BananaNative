package dev.nxnative.android;

/** GameOrientation (the pure rule); prints "ok" */
public final class GameOrientationJvmTest {
    static void check(boolean c, String what) { if (!c) { System.err.println("FAIL: " + what); System.exit(1); } }

    public static void main(String[] a) {
        // Honor PNM-N49: outer screen is a phone, inner screen (2172x2352, 480 dpi = 3.0 density) a large screen, in either reading of width and height
        check(GameOrientation.fixedLandscape(1080, 2448, 2.8f), "phone keeps sensorLandscape");
        check(!GameOrientation.fixedLandscape(2172, 2352, 3.0f), "inner screen: no request (724 dp)");
        check(!GameOrientation.fixedLandscape(2352, 2172, 3.0f), "inner screen turned");
        check(GameOrientation.fixedLandscape(1600, 2560, 2.0f), "tablet 16:10 keeps sensorLandscape");
        check(!GameOrientation.fixedLandscape(1840, 2208, 3.0f), "near-square large screen (613 dp, 1.2:1): no request");
        check(GameOrientation.fixedLandscape(1536, 2048, 2.0f), "tablet 4:3 keeps sensorLandscape");
        check(GameOrientation.fixedLandscape(1198, 2400, 2.0f), "599 dp is a phone");
        check(GameOrientation.fixedLandscape(0, 0, 0f), "unknown size keeps the manifest");

        System.out.println("ok");
    }
}
