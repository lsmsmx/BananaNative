package dev.nxnative.android;

import java.util.Arrays;
import java.util.List;

/**
 * The pure part of the Upscaler row for games with game.tsv dlss_standin (Prefs.dlssGame; GameUpscaler keeps it per game, StepSlider draws the sliders,
 * dm/gfx/vk_android/vka_dlss.c and vka_present.c read the lines live). Such a game's DLSS runs on vka's stand-in, so its Upscaler row picks the stand-in (DM_VK_DLSS:
 * GSR 2, FSR 1 or bilinear, per game as dlss_mode); under GSR 2, Motion clarity (DM_VK_GSR2_CLARITY c0..c4: steadier on the left, less ghosting on the right) and Temporal
 * sharpness (DM_VK_DLSS_SHARP, 0..30 in steps of 5, default 15; under FSR 1 the same lever as "FSR sharpness").
 *
 * The rule, for these games only:
 * <pre>
 *   Upscaler row (dlss_mode)                      DM_VK_DLSS
 *   0 GSR 2 (smooth images); unset or unknown     3
 *   1 FSR 1 (sharp, fast)                         2
 *   2 Bilinear (fastest, softest)                 1
 *
 *   stored ndsr_choice2 (GameUpscaler.MODES,      the present's scaling of the window to the screen
 *   the game's own, else the main screen's)       DM_VK_FSR   DM_VK_PRESENT_NEAREST
 *   0 Nearest neighbor                            0           1
 *   1 Bilinear                                    0           0
 *   2 FSR 1                                       1           0
 *   3 Native                                      0           0      (bilinear: the present has no counterpart)
 *   4 SGSR 1, also the unset default              1           0      (FSR 1: the present has no SGSR path)
 *   >= 5 or < 0 (unknown): as the default (4)     1           0
 * </pre>
 * The stored choice drives the present alone (with DM_VK_FSR_SHARP from the stored ndsr_sharp, as for any game); no DM_VK_NDSR line is sent for these games
 */
final class DlssTune {
    private DlssTune() {}

    static final String[] CLARITY_NAMES = {"Steadiest", "Steady", "Balanced", "Clear", "Clearest"};
    static final String[] CLARITY_NOTES = {"", "", "", "", " (may flicker in combat)"};
    static final int CLARITY_DEFAULT = 2;
    static final int[] SHARPS = {0, 5, 10, 15, 20, 25, 30};
    static final int SHARP_DEFAULT = 3;   // 15

    static String clarityName(int i) { return CLARITY_NAMES[i] + CLARITY_NOTES[i]; }
    static String[] clarityNames() { String[] n = new String[CLARITY_NAMES.length]; for (int i = 0; i < n.length; i++) n[i] = clarityName(i); return n; }
    static String sharpName(int i) { return SHARPS[i] == 0 ? "Off" : String.valueOf(SHARPS[i]); }
    static String[] sharpNames() { String[] n = new String[SHARPS.length]; for (int i = 0; i < n.length; i++) n[i] = sharpName(i); return n; }
    static int clamp(int v, int n, int dflt) { return v >= 0 && v < n ? v : dflt; }

    /** The Upscaler row: the stand-in's modes in the order shown, and each one's DM_VK_DLSS (3 GSR2 3-pass, 2 FSR 1 with the jitter taken out, 1 bilinear) */
    static final String[] MODE_NAMES = {"GSR 2 (smooth images)", "FSR 1 (sharp, fast)", "Bilinear (fastest, softest)"};
    static final int[] MODE_DLSS = {3, 2, 1};
    static final int MODE_DEFAULT = 0, MODE_GSR2 = 0, MODE_FSR1 = 1;
    /** Which sliders the mode shows: GSR 2 both, FSR 1 the sharpness alone (its RCAS), bilinear none */
    static boolean showsClarity(int mode) { return clamp(mode, MODE_NAMES.length, MODE_DEFAULT) == MODE_GSR2; }
    static boolean showsSharp(int mode) { int m = clamp(mode, MODE_NAMES.length, MODE_DEFAULT); return m == MODE_GSR2 || m == MODE_FSR1; }
    static String sharpTitle(int mode) { return clamp(mode, MODE_NAMES.length, MODE_DEFAULT) == MODE_FSR1 ? "FSR sharpness" : "Temporal sharpness"; }

    /** The present's scaling for a stored ndsr_choice2 (the table above): FSR 1, bilinear or nearest */
    static final int PRESENT_FSR = 0, PRESENT_BILINEAR = 1, PRESENT_NEAREST = 2;
    /** GameUpscaler.MODES' indices this rule reads: 0 Nearest neighbor, 1 Bilinear, 2 FSR 1, 3 Native, 4 SGSR 1 (GameUpscaler.MODE_DEFAULT) */
    static final int UPSCALER_COUNT = 5, UPSCALER_DEFAULT = 4;
    static int presentOf(int ndsrChoice) {
        switch (clamp(ndsrChoice, UPSCALER_COUNT, UPSCALER_DEFAULT)) {
            case 0: return PRESENT_NEAREST;
            case 1: case 3: return PRESENT_BILINEAR;
            default: return PRESENT_FSR;   // 2 FSR 1, 4 SGSR 1
        }
    }

    /**
     * Every line of such a game: the stand-in's mode and sliders, then the present's scaling from the stored Upscaler choice (FSR 1 with its RCAS at screenSharp stops,
     * below 0 RCAS at strength 0: GameUpscaler.SHARPS), no DM_VK_NDSR
     */
    static List<String> lines(int mode, int clarity, int sharpIndex, int ndsrChoice, float screenSharp) {
        int m = clamp(mode, MODE_NAMES.length, MODE_DEFAULT), p = presentOf(ndsrChoice);
        List<String> l = new java.util.ArrayList<>();
        l.add("DM_VK_DLSS=" + MODE_DLSS[m]);
        l.addAll(lines(clarity, sharpIndex));
        l.add("DM_VK_FSR=" + (p == PRESENT_FSR ? 1 : 0));
        l.add("DM_VK_FSR_SHARP=" + screenSharp);
        l.add("DM_VK_PRESENT_NEAREST=" + (p == PRESENT_NEAREST ? 1 : 0));
        return l;
    }
    /** The host's lines for a clarity step and a sharpness index */
    static List<String> lines(int clarity, int sharpIndex) {
        return Arrays.asList("DM_VK_GSR2_CLARITY=" + clamp(clarity, CLARITY_NAMES.length, CLARITY_DEFAULT), "DM_VK_DLSS_SHARP=" + SHARPS[clamp(sharpIndex, SHARPS.length, SHARP_DEFAULT)]);
    }
}
