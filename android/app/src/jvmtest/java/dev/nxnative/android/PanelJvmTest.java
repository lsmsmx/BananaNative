package dev.nxnative.android;

/**
 * PanelLook, PictureFit and DlssTune on the JVM (dm/tools/tests/test_android_panel.py): the panel's size presets, the picture size slider, the picture's rectangle and the DLSS stand-in's Upscaler row.
 * Prints "ok" or throws; with the argument "grid", prints the rectangles in pixels for the test to compare with the present's vka_fit_rect
 */
public final class PanelJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }
    private static boolean near(float a, float b) { return Math.abs(a - b) < 1e-4f; }

    static final int[][] VIEWS = {{2400, 1080}, {1920, 1080}, {1200, 900}, {2208, 1840}, {1080, 2400}};
    static final int[] SIZES = {100, 95, 80, 50, 10, 0};
    static final float[] PLACES = {0, 0.5f, 1};

    public static void main(String[] args) {
        if (args.length > 0 && args[0].equals("grid")) {
            for (int[] v : VIEWS) for (int st = 0; st < 2; st++) for (int s : SIZES) for (float p : PLACES) {
                float[] r = PictureFit.rect(v[0], v[1], p, p, st == 1, s);
                System.out.println(v[0] + " " + v[1] + " " + st + " " + s + " " + Math.round(p * 10000) + " "
                        + Math.round(r[0] * v[0]) + " " + Math.round(r[1] * v[1]) + " " + Math.round(r[2] * v[0]) + " " + Math.round(r[3] * v[1]));
            }
            return;
        }
        // The DLSS stand-in's sliders (DlssTune): five clarity steps, Balanced (2) the default and the last one marked; sharpness 0..30 in 5s, 15 the default; the host's lines
        check(DlssTune.clarityNames().length == 5 && DlssTune.clarityName(DlssTune.CLARITY_DEFAULT).equals("Balanced")
                && DlssTune.clarityName(4).equals("Clearest (may flicker in combat)") && DlssTune.clarityName(0).equals("Steadiest"), "clarity names");
        check(DlssTune.SHARPS.length == 7 && DlssTune.SHARPS[DlssTune.SHARP_DEFAULT] == 15 && DlssTune.SHARPS[6] == 30 && DlssTune.sharpName(0).equals("Off"), "sharpness steps");
        check(DlssTune.lines(2, DlssTune.SHARP_DEFAULT).equals(java.util.List.of("DM_VK_GSR2_CLARITY=2", "DM_VK_DLSS_SHARP=15")), "default lines");
        check(DlssTune.lines(4, 0).equals(java.util.List.of("DM_VK_GSR2_CLARITY=4", "DM_VK_DLSS_SHARP=0")) && DlssTune.lines(9, -1).equals(DlssTune.lines(2, 3)), "lines, out of range");
        // Its Upscaler row (dlss_mode): GSR 2 the default, then FSR 1, bilinear -> DM_VK_DLSS 3 / 2 / 1; unset or unknown -> 3; the sliders shown by mode
        check(DlssTune.MODE_NAMES.length == 3 && DlssTune.MODE_DEFAULT == 0 && DlssTune.MODE_DLSS[0] == 3 && DlssTune.MODE_DLSS[1] == 2 && DlssTune.MODE_DLSS[2] == 1, "upscaler modes");
        for (int m : new int[] {-1, 3, 7}) check(DlssTune.lines(m, 2, 3, 4, 0.25f).get(0).equals("DM_VK_DLSS=3"), "dlss_mode unknown " + m + ": GSR 2");
        check(DlssTune.lines(1, 2, 3, 4, 0.25f).get(0).equals("DM_VK_DLSS=2") && DlssTune.lines(2, 2, 3, 4, 0.25f).get(0).equals("DM_VK_DLSS=1"), "dlss_mode FSR 1, bilinear");
        check(DlssTune.showsClarity(0) && !DlssTune.showsClarity(1) && !DlssTune.showsClarity(2) && DlssTune.showsClarity(7), "clarity only with GSR 2 (out of range reads as GSR 2)");
        check(DlssTune.showsSharp(0) && DlssTune.showsSharp(1) && !DlssTune.showsSharp(2), "sharpness with GSR 2 and FSR 1");
        check(DlssTune.sharpTitle(0).equals("Temporal sharpness") && DlssTune.sharpTitle(1).equals("FSR sharpness"), "the sharpness row's name by mode");
        // The stored Upscaler choice (ndsr_choice2: 0 Nearest, 1 Bilinear, 2 FSR 1, 3 Native, 4 SGSR 1 the default) drives the present alone: SGSR 1, FSR 1 and unknown -> FSR 1,
        // Bilinear and Native -> bilinear, Nearest -> nearest
        String[][] present = {{"DM_VK_FSR=0", "DM_VK_PRESENT_NEAREST=1"}, {"DM_VK_FSR=0", "DM_VK_PRESENT_NEAREST=0"}, {"DM_VK_FSR=1", "DM_VK_PRESENT_NEAREST=0"},
                              {"DM_VK_FSR=0", "DM_VK_PRESENT_NEAREST=0"}, {"DM_VK_FSR=1", "DM_VK_PRESENT_NEAREST=0"}};
        for (int c = 0; c < 5; c++) {
            java.util.List<String> l = DlssTune.lines(0, 2, 3, c, 0.25f);
            check(l.get(3).equals(present[c][0]) && l.get(5).equals(present[c][1]), "stored choice " + c + ": " + l);
        }
        check(DlssTune.presentOf(4) == DlssTune.PRESENT_FSR && DlssTune.presentOf(2) == DlssTune.PRESENT_FSR && DlssTune.presentOf(1) == DlssTune.PRESENT_BILINEAR
                && DlssTune.presentOf(3) == DlssTune.PRESENT_BILINEAR && DlssTune.presentOf(0) == DlssTune.PRESENT_NEAREST, "presentOf");
        for (int c : new int[] {5, 9, -1}) check(DlssTune.presentOf(c) == DlssTune.PRESENT_FSR && DlssTune.lines(0, 2, 3, c, 0.25f).equals(DlssTune.lines(0, 2, 3, 4, 0.25f)), "unknown stored choice " + c + ": as the default, FSR 1");
        check(DlssTune.lines(0, 2, 3, DlssTune.UPSCALER_DEFAULT, 0.25f).equals(java.util.List.of("DM_VK_DLSS=3", "DM_VK_GSR2_CLARITY=2", "DM_VK_DLSS_SHARP=15", "DM_VK_FSR=1", "DM_VK_FSR_SHARP=0.25", "DM_VK_PRESENT_NEAREST=0")),
              "unset (the default SGSR 1): GSR 2 c2 / 15, present FSR 1: " + DlssTune.lines(0, 2, 3, 4, 0.25f));
        check(DlssTune.lines(1, 2, 3, 1, 0.25f).equals(java.util.List.of("DM_VK_DLSS=2", "DM_VK_GSR2_CLARITY=2", "DM_VK_DLSS_SHARP=15", "DM_VK_FSR=0", "DM_VK_FSR_SHARP=0.25", "DM_VK_PRESENT_NEAREST=0")), "FSR 1, present bilinear");
        check(DlssTune.lines(2, 0, 0, 0, -1f).equals(java.util.List.of("DM_VK_DLSS=1", "DM_VK_GSR2_CLARITY=0", "DM_VK_DLSS_SHARP=0", "DM_VK_FSR=0", "DM_VK_FSR_SHARP=-1.0", "DM_VK_PRESENT_NEAREST=1")), "bilinear, present nearest");
        check(DlssTune.lines(9, 9, 9, 9, 0.25f).equals(DlssTune.lines(0, 2, 3, 4, 0.25f)), "out of range: the defaults");
        for (int c = -1; c < 7; c++) for (String l : DlssTune.lines(0, 2, 3, c, 0.25f)) check(!l.startsWith("DM_VK_NDSR"), "no NDSR line");
        // The panel's presets: Normal is the panel as it was (420 dp, at most 55 % of the width), each larger one wider, taller rows and larger text
        check(PanelLook.widthPx(1, 3f, 2400) == 1260, "normal: 420 dp: " + PanelLook.widthPx(1, 3f, 2400));
        check(PanelLook.widthPx(1, 3f, 1600) == 880, "normal: 55 % of a narrow screen: " + PanelLook.widthPx(1, 3f, 1600));
        check(PanelLook.widthPx(0, 3f, 2400) < PanelLook.widthPx(1, 3f, 2400) && PanelLook.widthPx(1, 3f, 2400) < PanelLook.widthPx(2, 3f, 2400), "widths grow");
        check(PanelLook.widthPx(2, 3f, 1000) <= 700, "large: at most 70 % of the width");
        check(PanelLook.rowPx(0, 3f) < PanelLook.rowPx(1, 3f) && PanelLook.rowPx(1, 3f) < PanelLook.rowPx(2, 3f), "rows grow");
        check(PanelLook.textScale(1) == 1f && PanelLook.textScale(0) < 1f && PanelLook.textScale(2) > 1f, "text scale");
        check(PanelLook.of(-1) == 1 && PanelLook.of(7) == 1 && PanelLook.of(2) == 2, "a stored value out of range reads as Normal");
        check(PanelLook.widthPx(9, 3f, 2400) == PanelLook.widthPx(1, 3f, 2400), "an unknown size is Normal");

        // The picture size slider: 50..100 % in steps of 5
        check(PictureFit.steps() == 10, "ten steps");
        check(PictureFit.sizeOfStep(0) == 50 && PictureFit.sizeOfStep(10) == 100 && PictureFit.sizeOfStep(7) == 85, "step to size");
        check(PictureFit.sizeOfStep(-3) == 50 && PictureFit.sizeOfStep(99) == 100, "steps clamped");
        for (int k = 0; k <= PictureFit.steps(); k++) check(PictureFit.stepOfSize(PictureFit.sizeOfStep(k)) == k, "round trip " + k);
        check(PictureFit.size(0) == 100 && PictureFit.size(130) == 100 && PictureFit.size(20) == 50 && PictureFit.size(73) == 73, "size as the host reads it");

        // The rectangle: fit, stretch, a share of either, placed in the free space
        float[] r = PictureFit.rect(2400, 1080, 0.5f, 0.5f, false, 100);
        check(near(r[0], 0.1f) && near(r[1], 0) && near(r[2], 0.8f) && near(r[3], 1), "fit centred on 20:9");
        r = PictureFit.rect(2400, 1080, 0.5f, 0.5f, false, 80);
        check(near(r[2], 0.64f) && near(r[3], 0.8f) && near(r[0], 0.18f) && near(r[1], 0.1f), "80 % centred, bars around");
        r = PictureFit.rect(2400, 1080, 0.5f, 0f, false, 80);
        check(near(r[1], 0) && near(r[0], 0.18f), "80 % at the top");
        r = PictureFit.rect(1200, 900, 0.5f, 0.5f, true, 100);
        check(near(r[0], 0) && near(r[1], 0) && near(r[2], 1) && near(r[3], 1), "stretch fills");
        r = PictureFit.rect(1200, 900, 0.5f, 0.5f, true, 50);
        check(near(r[0], 0.25f) && near(r[1], 0.25f) && near(r[2], 0.5f) && near(r[3], 0.5f), "stretch at 50 %");
        r = PictureFit.rect(1200, 900, 3f, -1f, false, 100);
        check(near(r[0], 0) && near(r[1], 0), "the place clamped: " + r[0] + " " + r[1]);
        System.out.println("ok");
    }
}
