package dev.nxnative.android;

import java.io.File;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * ShaderLibs on the JVM, driven by dm/tools/tests/test_android_shader_delta.py (which writes the libraries with the tools' own header layout). Commands, one per run:
 * <pre>
 * header &lt;dmsh_vk.lib&gt;                                → "header chain vk_chain game inputs abi" (fields tab-separated; "null" when not a library)
 * playable &lt;pack&gt; &lt;title ID&gt; &lt;current&gt; &lt;abi&gt; [compat]   → "playable &lt;the v-V folder's name&gt;" or "playable none" (compat: older ABIs, space-separated)
 * legacy &lt;pack&gt; &lt;title ID&gt; &lt;current&gt;                   → "legacy &lt;the v-V folder's name&gt;" or "legacy none"
 * older &lt;pack&gt; &lt;title ID&gt; &lt;current&gt;                    → "older &lt;versions, sorted, comma-separated&gt;"
 * translate &lt;delta rows&gt; &lt;versions, comma-separated&gt;   → "translate &lt;n&gt;"
 * updating &lt;done&gt; &lt;total&gt; &lt;eta or -&gt; &lt;paused 0|1&gt;   → "updating &lt;the game card's state&gt;" (Locale.ROOT)
 * minutes &lt;n&gt;                                           → "minutes &lt;lo&gt; &lt;hi&gt; &lt;text&gt;"; job &lt;n&gt; → "job &lt;a whole Prepare's text&gt;"
 * restamp &lt;pack&gt; &lt;title ID&gt; &lt;current&gt; &lt;abi&gt; &lt;stamps&gt;  → "restamp &lt;the older version taken over&gt;" or "restamp none"
 * selection &lt;profile dir&gt;                               → "selection &lt;keys, comma-separated&gt;" or "selection none" (no shader_seen.tsv)
 * absent &lt;profile dir&gt; &lt;split's shader_map.tsv&gt;         → "absent &lt;keys, comma-separated&gt;" or "absent none" (ShaderLibs.absentOptional)
 * </pre>
 */
public final class ShaderLibsJvmTest {
    public static void main(String[] a) {
        switch (a[0]) {
            case "updating":
                System.out.println("updating " + ShaderLibs.updatingText(java.util.Locale.ROOT, Long.parseLong(a[1]), Long.parseLong(a[2]), a[3].equals("-") ? "" : a[3], a[4].equals("1")));
                break;
            case "header": {
                ShaderLibs.Header h = ShaderLibs.header(new File(a[1]));
                System.out.println(h == null ? "header null" : "header " + String.join("\t", h.chain, h.vkChain, h.game, h.inputs, h.abi));
                break;
            }
            case "playable": {
                File f = a.length > 5 ? ShaderLibs.playable(new File(a[1]), a[2], a[3], a[4], a[5]) : ShaderLibs.playable(new File(a[1]), a[2], a[3], a[4]);
                System.out.println("playable " + (f == null ? "none" : f.getParentFile().getName()));
                break;
            }
            case "legacy": {
                File f = ShaderLibs.legacy(new File(a[1]), a[2], a[3]);
                System.out.println("legacy " + (f == null ? "none" : f.getParentFile().getName()));
                break;
            }
            case "older": {
                List<String> v = ShaderLibs.older(new File(a[1]), a[2], a[3]);
                Collections.sort(v);
                System.out.println("older " + String.join(",", v));
                break;
            }
            case "translate":
                System.out.println("translate " + ShaderLibs.toTranslate(a[1], Arrays.asList(a[2].split(","))));
                break;
            case "job":
                System.out.println("job " + ShaderLibs.jobMinutesText(Integer.parseInt(a[1])));
                break;
            case "minutes": {
                int[] m = ShaderLibs.minutes(Integer.parseInt(a[1]));
                System.out.println("minutes " + m[0] + " " + m[1] + " " + ShaderLibs.minutesText(Integer.parseInt(a[1])));
                break;
            }
            case "restamp": {
                String v = ShaderLibs.restamp(new File(a[1]), a[2], a[3], a[4], a[5]);
                System.out.println("restamp " + (v == null ? "none" : v));
                break;
            }
            case "selection": {
                List<String> k;
                try {
                    k = ShaderLibs.selection(new File(a[1]));
                } catch (java.io.IOException e) {
                    throw new RuntimeException(e);
                }
                System.out.println("selection " + (k == null ? "none" : String.join(",", k)));
                break;
            }
            case "absent": {
                List<String> k;
                try {
                    k = ShaderLibs.absentOptional(new File(a[1]), new File(a[2]));
                } catch (java.io.IOException e) {
                    throw new RuntimeException(e);
                }
                System.out.println("absent " + (k == null ? "none" : String.join(",", k)));
                break;
            }
            default:
                throw new IllegalArgumentException(a[0]);
        }
    }
}
