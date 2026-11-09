package dev.nxnative.android;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

/**
 * ExitNotice on the JVM, driven by dm/tools/tests/test_android_exitnotice.py. Commands, one per run (files are UTF-8; "-" for none):
 * <pre>
 * cause &lt;record&gt; &lt;reason&gt; &lt;status&gt; &lt;description&gt; &lt;background 0|1&gt; &lt;log&gt;  → "cause &lt;the sentence, or null&gt;"
 * pending &lt;record&gt;                                                     → "pending true|false"
 * message &lt;name&gt; &lt;cause&gt; [&lt;likely cause&gt;]                             → "message &lt;the notice's text, \n as \\n&gt;"
 * block &lt;thread&gt; &lt;trace file&gt; &lt;max lines&gt;                              → "block &lt;the log block, \n as \\n&gt;"
 * line &lt;class&gt; &lt;message or -&gt;                                         → "line &lt;the record's exception line&gt;"
 * </pre>
 */
public final class ExitNoticeJvmTest {
    private static String file(String p) throws IOException { return p.equals("-") ? null : new String(Files.readAllBytes(Paths.get(p)), StandardCharsets.UTF_8); }
    private static String esc(String s) { return s.replace("\n", "\\n"); }

    public static void main(String[] a) throws IOException {
        switch (a[0]) {
            case "cause": System.out.println("cause " + ExitNotice.cause(ExitNotice.parse(file(a[1])), Integer.parseInt(a[2]), Integer.parseInt(a[3]), a[4].equals("-") ? null : a[4],
                    a[5].equals("1"), file(a[6]))); break;
            case "pending": System.out.println("pending " + ExitNotice.pending(ExitNotice.parse(file(a[1])))); break;
            case "message": System.out.println("message " + esc(a.length > 3 ? ExitNotice.message(a[1], a[2], a[3]) : ExitNotice.message(a[1], a[2]))); break;
            case "block": System.out.println("block " + esc(ExitNotice.crashBlock(a[1], file(a[2]), Integer.parseInt(a[3])))); break;
            case "line": System.out.println("line " + ExitNotice.exceptionLine(a[1], a[2].equals("-") ? null : a[2])); break;
            default: throw new IllegalArgumentException(a[0]);
        }
    }
}
