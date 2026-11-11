package dev.nxnative.android;

import android.content.Context;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.List;

/**
 * The Upscaler: what runs in place of a game's own neural upscaler, and how the window is scaled to the screen (Nintendo's NDSR, emulated and slow on phones: DM_VK_NDSR, DM_VK_NDSR_SHARP in dm/gfx/vk_android/vka_draw.c), per game:
 * a nearest-neighbour or bilinear stretch, FSR 1 (the default, with RCAS at AMD's default sharpness), Qualcomm's Snapdragon Game Super Resolution 1 (one pass, its own sharpening), or the game's network itself ("Native"). Set in Game settings and live in the in-game panel, sent to the
 * running game as lines of its live control file (DM_CTL) as the in-game panel's other live settings do. For a game whose DLSS runs on the host's GSR2 stand-in (Prefs.dlssGame),
 * the Upscaler row offers the stand-in's modes instead (dlss_mode: DM_VK_DLSS), with its Motion clarity and Temporal sharpness sliders (DlssTune, StepSlider; DM_VK_GSR2_CLARITY,
 * DM_VK_DLSS_SHARP); the stored ndsr_choice2 then drives the present's scaling alone, without DM_VK_NDSR (DlssTune's rule)
 */
final class GameUpscaler {
    private GameUpscaler() {}

    /** The choices in the order shown, and each one's DM_VK_NDSR value (3 nearest-neighbour blit, 1 bilinear blit, 2 FSR 1, 0 the game's network, 4 Qualcomm's SGSR 1).
     *  New choices go at the end: the stored choice is an index into this list */
    static final String[] MODES = {"Nearest neighbor (fastest, blocky)", "Bilinear (fastest, softest)", "FSR 1 (sharp, fast; too bright on Qualcomm's driver in some games)", "Native (sharpest, slowest)", "SGSR 1 (sharp, fast, one pass)"};
    static final int[] NDSR = {3, 1, 2, 0, 4};
    static final int MODE_DEFAULT = 4;   // SGSR 1 (FSR 1 is too bright on Qualcomm's driver in Ocarina of Time: open, docs/reference/shaders.md)
    static final int MODE_FSR = 2;
    /** RCAS sharpness in stops (0 the strongest, each stop halves it); below 0: no RCAS */
    static final float[] SHARPS = {-1f, 1f, 0.5f, 0.25f, 0f};
    static final String[] SHARP_NAMES = {"Off", "Softer", "Soft", "Default", "Max"};

    /** The choice's index in MODES (stored as ndsr_choice2: the earlier key's indices meant another order): the main screen's default, then each game's own */
    static int mode(Context c) { int m = Prefs.of(c).getInt("ndsr_choice2", MODE_DEFAULT); return m >= 0 && m < MODES.length ? m : MODE_DEFAULT; }
    static void modeDefault(Context c, int m) { Prefs.of(c).edit().putInt("ndsr_choice2", m).commit(); }
    static boolean ownMode(Context c, String titleId) { return Prefs.of(c).contains(Prefs.gameKey("ndsr_choice2", titleId)); }
    static int mode(Context c, String titleId) { int m = Prefs.of(c).getInt(Prefs.gameKey("ndsr_choice2", titleId), mode(c)); return m >= 0 && m < MODES.length ? m : mode(c); }
    static void mode(Context c, String titleId, Integer m) {
        if (m == null) Prefs.of(c).edit().remove(Prefs.gameKey("ndsr_choice2", titleId)).commit(); else Prefs.of(c).edit().putInt(Prefs.gameKey("ndsr_choice2", titleId), m).commit();
        push(c, titleId);
    }
    static float sharp(Context c) { return Prefs.of(c).getFloat("ndsr_sharp", 0.25f); }   // AMD's default
    static void sharpDefault(Context c, float s) { Prefs.of(c).edit().putFloat("ndsr_sharp", s).commit(); }
    static boolean ownSharp(Context c, String titleId) { return Prefs.of(c).contains(Prefs.gameKey("ndsr_sharp", titleId)); }
    static float sharp(Context c, String titleId) { return Prefs.of(c).getFloat(Prefs.gameKey("ndsr_sharp", titleId), sharp(c)); }
    static void sharp(Context c, String titleId, Float s) {
        if (s == null) Prefs.of(c).edit().remove(Prefs.gameKey("ndsr_sharp", titleId)).commit(); else Prefs.of(c).edit().putFloat(Prefs.gameKey("ndsr_sharp", titleId), s).commit();
        push(c, titleId);
    }
    static String sharpName(float s) { for (int i = 0; i < SHARPS.length; i++) if (SHARPS[i] == s) return SHARP_NAMES[i]; return SHARP_NAMES[0]; }
    static String sharpName(Context c, String titleId) { return sharpName(sharp(c, titleId)); }
    static int sharpIndex(float s) { for (int i = 0; i < SHARPS.length; i++) if (SHARPS[i] == s) return i; return 0; }

    // A game whose DLSS runs on the host's GSR2 stand-in (Prefs.dlssGame): Motion clarity and Temporal sharpness (DlssTune), each game's own or the default, sent live like the rest
    static boolean ownClarity(Context c, String titleId) { return Prefs.of(c).contains(Prefs.gameKey("gsr2_clarity", titleId)); }
    static int clarity(Context c, String titleId) { return DlssTune.clamp(Prefs.of(c).getInt(Prefs.gameKey("gsr2_clarity", titleId), DlssTune.CLARITY_DEFAULT), DlssTune.CLARITY_NAMES.length, DlssTune.CLARITY_DEFAULT); }
    static void clarity(Context c, String titleId, Integer v) {
        if (v == null) Prefs.of(c).edit().remove(Prefs.gameKey("gsr2_clarity", titleId)).commit(); else Prefs.of(c).edit().putInt(Prefs.gameKey("gsr2_clarity", titleId), v).commit();
        push(c, titleId);
    }
    static boolean ownDlssSharp(Context c, String titleId) { return Prefs.of(c).contains(Prefs.gameKey("dlss_sharp", titleId)); }
    /** The index into DlssTune.SHARPS */
    static int dlssSharp(Context c, String titleId) { return DlssTune.clamp(Prefs.of(c).getInt(Prefs.gameKey("dlss_sharp", titleId), DlssTune.SHARP_DEFAULT), DlssTune.SHARPS.length, DlssTune.SHARP_DEFAULT); }
    static void dlssSharp(Context c, String titleId, Integer v) {
        if (v == null) Prefs.of(c).edit().remove(Prefs.gameKey("dlss_sharp", titleId)).commit(); else Prefs.of(c).edit().putInt(Prefs.gameKey("dlss_sharp", titleId), v).commit();
        push(c, titleId);
    }

    // A game on the DLSS stand-in: its Upscaler row is the stand-in's mode (DlssTune.MODE_NAMES), stored per game only (no main-screen default: no other game has the stand-in)
    static boolean ownDlssMode(Context c, String titleId) { return Prefs.of(c).contains(Prefs.gameKey("dlss_mode", titleId)); }
    static int dlssMode(Context c, String titleId) { return DlssTune.clamp(Prefs.of(c).getInt(Prefs.gameKey("dlss_mode", titleId), DlssTune.MODE_DEFAULT), DlssTune.MODE_NAMES.length, DlssTune.MODE_DEFAULT); }
    static void dlssMode(Context c, String titleId, Integer v) {
        if (v == null) Prefs.of(c).edit().remove(Prefs.gameKey("dlss_mode", titleId)).commit(); else Prefs.of(c).edit().putInt(Prefs.gameKey("dlss_mode", titleId), v).commit();
        push(c, titleId);
    }

    /**
     * The host's switches for a run (env.txt lines come after these and win). One choice for every scaling step: the game's internal upscaler (DM_VK_NDSR) and the
     * present's scaling of the window to the screen (only when the window is smaller): FSR 1 there too with the same sharpness (DM_VK_FSR; its RCAS pass also places and turns
     * the picture, so "Off" is RCAS at strength 0: no sharpening), nearest neighbor as nearest filtering (DM_VK_PRESENT_NEAREST), bilinear otherwise (Native has no counterpart there)
     */
    static List<String> lines(Context c, String titleId) {
        int m = mode(c, titleId); float s = sharp(c, titleId);
        if (Prefs.dlssGame(titleId)) return DlssTune.lines(dlssMode(c, titleId), clarity(c, titleId), dlssSharp(c, titleId), m, s);   // the stand-in's row; the stored choice: the present alone
        return java.util.Arrays.asList("DM_VK_NDSR=" + NDSR[m], "DM_VK_NDSR_SHARP=" + s, "DM_VK_FSR=" + (m == MODE_FSR ? 1 : 0), "DM_VK_FSR_SHARP=" + s,
                                       "DM_VK_PRESENT_NEAREST=" + (NDSR[m] == 3 ? 1 : 0));
    }
    static void env(Context c, String titleId, List<String> env) { env.addAll(lines(c, titleId)); }

    private static void push(Context c, String titleId) {
        File ext = c.getExternalFilesDir(null);   // GameActivity's DM_CTL: <app files>/ctl.txt (only the running game reads it)
        if (ext == null) return;
        try (FileWriter w = new FileWriter(new File(ext, "ctl.txt"), true)) {
            StringBuilder b = new StringBuilder(); for (String l : lines(c, titleId)) b.append("env ").append(l).append('\n');
            w.write(b.toString());
        } catch (IOException e) { Native.note("[RED] game upscaler: the setting could not be sent to the game (" + e.getMessage() + "); it applies at the next start"); }
    }

    /**
     * Whether the in-game panel shows the rows: only for a game whose neural upscaler network the host can replace (it says so once the network has run: Native.ndsrStatus),
     * remembered per game (ndsr_seen) so that the rows are there from the next start's first screens on, before the network's first frame. A host that cannot say: always
     */
    static boolean shown(Context c, String titleId) {
        int st = Native.ndsrStatus();
        String k = Prefs.gameKey("ndsr_seen", titleId);
        if (st > 0 && !Prefs.of(c).getBoolean(k, false)) Prefs.of(c).edit().putBoolean(k, true).apply();
        return st != 0 || Prefs.of(c).getBoolean(k, false);
    }

    /** The in-game panel's rows: the upscaler, and FSR's sharpness while FSR 1 is the choice; each applied at once (the same per-game choice as Game settings › Upscaler). A game on the DLSS stand-in gets dlssRows instead */
    static void rows(Context c, String titleId, LinearLayout box) {
        if (Prefs.dlssGame(titleId)) { dlssRows(c, titleId, box); return; }
        TextView head = new TextView(c);
        head.setText("Upscaler");
        head.setPadding(0, 24, 0, 0);
        box.addView(head);
        RadioGroup g = new RadioGroup(c);
        for (int i = 0; i < MODES.length; i++) { RadioButton b = new RadioButton(c); b.setText(MODES[i]); b.setId(3100 + i); g.addView(b); if (i == mode(c, titleId)) b.setChecked(true); }
        box.addView(g);
        TextView l = new TextView(c); l.setText("FSR sharpness"); box.addView(l);
        RadioGroup s = new RadioGroup(c);
        s.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < SHARPS.length; i++) { RadioButton b = new RadioButton(c); b.setText(SHARP_NAMES[i]); b.setId(3200 + i); s.addView(b); if (SHARPS[i] == sharp(c, titleId)) b.setChecked(true); }
        s.setOnCheckedChangeListener((gr, id) -> sharp(c, titleId, SHARPS[id - 3200]));
        box.addView(s);
        Runnable shown = () -> { int v = mode(c, titleId) == MODE_FSR ? android.view.View.VISIBLE : android.view.View.GONE; l.setVisibility(v); s.setVisibility(v); };
        shown.run();
        g.setOnCheckedChangeListener((gr, id) -> { mode(c, titleId, id - 3100); shown.run(); });
    }

    /** A game on the DLSS stand-in: "Upscaler" (the stand-in's mode) with Motion clarity and Temporal sharpness under GSR 2, the sharpness alone ("FSR sharpness") under FSR 1. Each applied at once */
    private static void dlssRows(Context c, String titleId, LinearLayout box) {
        TextView head = new TextView(c);
        head.setText("Upscaler");
        head.setPadding(0, 24, 0, 0);
        box.addView(head);
        RadioGroup g = new RadioGroup(c);
        for (int i = 0; i < DlssTune.MODE_NAMES.length; i++) { RadioButton b = new RadioButton(c); b.setText(DlssTune.MODE_NAMES[i]); b.setId(3300 + i); g.addView(b); if (i == dlssMode(c, titleId)) b.setChecked(true); }
        box.addView(g);
        TextView clHead = new TextView(c); box.addView(clHead);
        android.view.View cl = StepSlider.view(c, "Motion clarity", DlssTune.clarityNames(), clarity(c, titleId), GameSettings.CLARITY_LEFT, GameSettings.CLARITY_RIGHT,
                                               clHead::setText, i -> clarity(c, titleId, i));
        box.addView(cl);
        TextView shHead = new TextView(c); box.addView(shHead);
        android.view.View sh = StepSlider.view(c, DlssTune.sharpTitle(dlssMode(c, titleId)), DlssTune.sharpNames(), dlssSharp(c, titleId), "Softer", "Sharper",
                                               t -> shHead.setText(DlssTune.sharpTitle(dlssMode(c, titleId)) + t.substring(t.indexOf(':'))), i -> dlssSharp(c, titleId, i));
        box.addView(sh);
        Runnable shown = () -> {
            int m = dlssMode(c, titleId), vc = DlssTune.showsClarity(m) ? android.view.View.VISIBLE : android.view.View.GONE, vs = DlssTune.showsSharp(m) ? android.view.View.VISIBLE : android.view.View.GONE;
            clHead.setVisibility(vc); cl.setVisibility(vc); shHead.setVisibility(vs); sh.setVisibility(vs);
            shHead.setText(StepSlider.title(DlssTune.sharpTitle(m), DlssTune.sharpNames(), dlssSharp(c, titleId)));
        };
        shown.run();
        g.setOnCheckedChangeListener((gr, id) -> { dlssMode(c, titleId, id - 3300); shown.run(); });
    }
}
