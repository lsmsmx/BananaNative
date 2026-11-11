package dev.nxnative.android;

/**
 * SkipNotice on the JVM, driven by dm/tools/tests/test_android_skipnotice.py. Commands, one per run:
 * <pre>
 * percent &lt;status or -&gt;           → "percent &lt;n&gt;"
 * causes &lt;status or -&gt;            → "causes &lt;text&gt;"
 * text &lt;percent&gt; &lt;driver or -&gt;    → "text &lt;the notice&gt;"
 * </pre>
 */
public final class SkipNoticeJvmTest {
    public static void main(String[] a) {
        String s = a.length > 1 && !a[1].equals("-") ? a[1] : null;
        switch (a[0]) {
            case "percent": System.out.println("percent " + SkipNotice.percent(s)); break;
            case "causes": System.out.println("causes " + SkipNotice.causes(s)); break;
            case "nomesh": System.out.println("nomesh " + SkipNotice.noMesh(s) + " " + SkipNotice.textNoMesh(Integer.parseInt(a[2]))); break;
            case "text": System.out.println("text " + SkipNotice.text(Integer.parseInt(a[1]), a.length > 2 && !a[2].equals("-") ? a[2] : null)); break;
            default: throw new IllegalArgumentException(a[0]);
        }
    }
}
