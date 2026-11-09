package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;


/**
 * LogDigest on the JVM, driven by dm/tools/tests/test_android_logdigest.py. Commands, one per run (files are UTF-8; "-" for none):
 * <pre>
 * digest &lt;rules&gt; &lt;noise&gt; &lt;log&gt; &lt;record&gt; &lt;reason&gt; &lt;status&gt; &lt;description&gt; &lt;background 0|1&gt; &lt;tail bytes&gt;
 *        → the digest's text, then "== verdict &lt;kind&gt;\t&lt;rule or -&gt;\t&lt;doc URL or -&gt;\t&lt;likely cause for the notice's sentence or -&gt;"
 * table &lt;rules|noise&gt; &lt;tsv&gt;   → "ok &lt;id&gt;" per row, or "bad &lt;message&gt;" (exit 0 either way)
 * keys                         → the two key-line patterns, one a line
 * redact &lt;text file&gt;           → each line redacted, one a line
 * </pre>
 */
public final class LogDigestJvmTest {
    private static String file(String p) throws IOException { return p.equals("-") ? null : new String(Files.readAllBytes(Paths.get(p)), StandardCharsets.UTF_8); }
    private static String or(String s) { return s == null ? "-" : s; }

    public static void main(String[] a) throws IOException {
        switch (a[0]) {
            case "digest": {
                LogDigest d = new LogDigest(LogDigest.rules(file(a[1])), LogDigest.noise(file(a[2])));
                d.readTail(new File(a[3]), Long.parseLong(a[9]));
                LogDigest.Verdict v = d.verdict(ExitNotice.parse(file(a[4])), Integer.parseInt(a[5]), Integer.parseInt(a[6]), a[7].equals("-") ? null : a[7], a[8].equals("1"));
                String notice = ExitNotice.cause(ExitNotice.parse(file(a[4])), Integer.parseInt(a[5]), Integer.parseInt(a[6]), a[7].equals("-") ? null : a[7], a[8].equals("1"),
                        file(a[3]));   // the notice reads the log's last 32 KB; the test logs are smaller
                System.out.print(d.render(v));
                System.out.println("== verdict " + v.kind + "\t" + or(v.rule) + "\t" + or(v.docUrl()) + "\t" + or(LogDigest.likely(v, notice)));
                break;
            }
            case "table":
                try {
                    List<LogDigest.Row> rows = a[1].equals("rules") ? LogDigest.rules(file(a[2])) : LogDigest.noise(file(a[2]));
                    for (LogDigest.Row r : rows) System.out.println("ok " + r.id);
                } catch (IllegalArgumentException e) {   // a PatternSyntaxException too
                    System.out.println("bad " + e.getMessage().replace('\n', ' '));
                }
                break;
            case "keys": System.out.println(LogDigest.KEY_LINE.pattern()); System.out.println(LogDigest.KEY_LINE2.pattern()); break;
            case "redact":
                for (String l : file(a[1]).split("\n", -1)) System.out.println(LogDigest.redact(l));
                break;
            default: throw new IllegalArgumentException(a[0]);
        }
    }
}
