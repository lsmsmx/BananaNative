package dev.nxnative.android;

import android.app.AlertDialog;
import android.content.Context;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.function.Consumer;
import java.util.function.IntConsumer;

/**
 * A slider over named steps, on DrsSlider's pattern: the title "<label>: <step's name>", optional words under the two ends. The in-game panel applies each step at once;
 * Game settings picks it in a dialog with "Default (...)" first. Used by the DLSS stand-in's Motion clarity and Temporal sharpness (DlssTune, GameUpscaler)
 */
final class StepSlider {
    private StepSlider() {}

    static String title(String label, String[] names, int index) { return label + ": " + names[index]; }

    /** A slider set to index; the title goes to setTitle at each step, onChange(index) at each step the user makes. left / right: words under the ends, or null */
    static LinearLayout view(Context c, String label, String[] names, int index, String left, String right, Consumer<String> setTitle, IntConsumer onChange) {
        LinearLayout box = new LinearLayout(c);
        box.setOrientation(LinearLayout.VERTICAL);
        setTitle.accept(title(label, names, index));
        SeekBar sb = new SeekBar(c);
        sb.setMax(names.length - 1);
        sb.setProgress(index);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean user) { setTitle.accept(title(label, names, v)); if (user) onChange.accept(v); }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        box.addView(sb);
        if (left != null || right != null) {
            LinearLayout ends = new LinearLayout(c);
            ends.setOrientation(LinearLayout.HORIZONTAL);
            TextView l = new TextView(c), r = new TextView(c);
            l.setText(left == null ? "" : left);
            r.setText(right == null ? "" : right);
            l.setTextSize(12);
            r.setTextSize(12);
            r.setGravity(android.view.Gravity.END);
            ends.addView(l, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            ends.addView(r, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            box.addView(ends);
        }
        return box;
    }

    /** A dialog with the slider; with dflt non-null a "Default (dflt)" switch first (on: set.accept(null)). done after OK */
    static void dialog(Context c, String label, String suffix, String[] names, int index, String left, String right, String dflt, boolean own, Consumer<Integer> set, Runnable done) {
        LinearLayout box = new LinearLayout(c);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(c, 20); box.setPadding(pad, pad / 2, pad, 0);
        int[] cur = {index};
        AlertDialog[] dlg = new AlertDialog[1];
        LinearLayout slider = view(c, label, names, index, left, right, t -> { if (dlg[0] != null) dlg[0].setTitle(t + suffix); }, i -> cur[0] = i);
        CheckBox def = null;
        if (dflt != null) {
            def = new CheckBox(c);
            def.setText("Default (" + dflt + ")");
            def.setChecked(!own);
            slider.setAlpha(own ? 1f : 0.4f);
            def.setOnCheckedChangeListener((b, on) -> slider.setAlpha(on ? 0.4f : 1f));
            box.addView(def);
        }
        box.addView(slider);
        CheckBox d = def;
        dlg[0] = new AlertDialog.Builder(c).setTitle(title(label, names, index) + suffix).setView(box)
                .setPositiveButton("OK", (x, w) -> { set.accept(d != null && d.isChecked() ? null : cur[0]); done.run(); })
                .setNegativeButton("Cancel", null).show();
    }
}
