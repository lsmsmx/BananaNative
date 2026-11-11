package dev.nxnative.android;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * scripts/pack_android.py's rewrite_key and rewrite_keys, plain Java (PackCheck.code; checked on the JVM by dm/tools/tests/test_android_rewritekey.py with
 * RewriteKeyJvmTest): SHA-256 over name\0bytes of the REWRITER files, the module, the profile's patches.py (when there is one) and game.tsv's build and
 * fps_decouple rows, the only part of game.tsv the rewriter reads (buildRows, hashed under the name ROWS; the rest is the host's, and a change there must not make packs stale). The keys
 * that count as the current rewriter's are its own and those with an earlier equivalent file in place of a REWRITER file: dm/tools/rewriter_equiv/&lt;file&gt;.&lt;commit&gt;,
 * which holds a file there ONLY when the rewrite output with it was verified byte-identical (pack_android.py EQUIV). Each also in the earlier (v1) formula, which
 * hashed the whole game.tsv: with the APK's, and with the pack's copy (&lt;pack&gt;/game.tsv) when its build rows are the APK's: the key proves the copy is the file
 * it was made with, and the rows the rewrite read are the same.
 */
final class RewriteKey {
    private RewriteKey() {}

    static final String EQUIV = "dm/tools/rewriter_equiv/";
    /** The name the build rows are hashed under (pack_android.py ROWS) */
    static final String ROWS = "game.tsv build rows";

    /**
     * game.tsv's build and fps_decouple rows as patch_nso.py reads them (rows split at tabs, each cell stripped, first cell "build" or "fps_decouple"), one a line (pack_android.py build_rows;
     * bytes kept as they are). A narrower strip and split than the rewriter's own: two tables that differ for it never come out the same here.
     */
    static byte[] buildRows(byte[] tsv) {
        StringBuilder out = new StringBuilder();
        for (String row : new String(tsv, StandardCharsets.ISO_8859_1).split("\n", -1)) {
            String[] c = row.split("\t", -1);
            for (int i = 0; i < c.length; i++) c[i] = strip(c[i]);
            if (c[0].equals("build") || c[0].equals("fps_decouple")) out.append(String.join("\t", c)).append('\n');
        }
        return out.toString().getBytes(StandardCharsets.ISO_8859_1);
    }

    private static String strip(String s) {   // Python's bytes.strip(b' \r\x0b\x0c')
        int a = 0, b = s.length();
        while (a < b && " \r\u000b\u000c".indexOf(s.charAt(a)) >= 0) a++;
        while (b > a && " \r\u000b\u000c".indexOf(s.charAt(b - 1)) >= 0) b--;
        return s.substring(a, b);
    }

    /** Feeds the module's bytes (BuildCheck.Mod.hash) */
    interface Module { void hash(MessageDigest h) throws IOException; }

    /**
     * Every key that counts as the current rewriter's, its own first. tools: the APK's tools by path (dm/tools/&lt;file&gt;, dm/tools/rewriter_equiv/&lt;file&gt;.&lt;commit&gt;,
     * &lt;gameDir&gt;patches.py and game.tsv); packGameTsv: the pack's copy of game.tsv, or null; the module is read once for all of them.
     */
    static List<String> keys(Map<String, byte[]> tools, String[] rewriter, String gameDir, String module, Module nso, byte[] packGameTsv) throws IOException {
        List<List<Object[]>> variants = new ArrayList<>();
        variants.add(new ArrayList<>());
        for (String f : rewriter) {
            byte[] cur = tools.get("dm/tools/" + f);
            if (cur == null) throw new IOException("the APK's tools have no dm/tools/" + f);
            List<byte[]> alt = new ArrayList<>();
            alt.add(cur);
            String pre = EQUIV + f + ".";   // <file>.<commit>
            for (Map.Entry<String, byte[]> e : new java.util.TreeMap<>(tools).entrySet()) {
                String n = e.getKey();
                if (n.startsWith(pre) && n.length() > pre.length() && n.indexOf('.', pre.length()) < 0) alt.add(e.getValue());
            }
            List<List<Object[]>> next = new ArrayList<>();
            for (List<Object[]> v : variants) for (byte[] b : alt) { List<Object[]> w = new ArrayList<>(v); w.add(new Object[] {f, b}); next.add(w); }
            variants = next;
        }
        List<MessageDigest> hs = new ArrayList<>();
        for (List<Object[]> v : variants) {
            MessageDigest h = sha256();
            for (Object[] p : v) part(h, (String) p[0], (byte[]) p[1]);
            hs.add(h);
        }
        MessageDigest all = new Tee(hs);
        all.update((module + "\0").getBytes(StandardCharsets.UTF_8));
        nso.hash(all);
        byte[] pp = tools.get(gameDir + "patches.py");
        if (pp != null) part(all, "patches.py", pp);
        byte[] gt = tools.get(gameDir + "game.tsv");
        if (gt == null) throw new IOException("the APK's tools have no " + gameDir + "game.tsv");
        List<Object[]> tails = new ArrayList<>();
        tails.add(new Object[] {ROWS, buildRows(gt)});
        tails.add(new Object[] {"game.tsv", gt});
        if (packGameTsv != null && !Arrays.equals(packGameTsv, gt) && Arrays.equals(buildRows(packGameTsv), buildRows(gt))) tails.add(new Object[] {"game.tsv", packGameTsv});
        List<String> out = new ArrayList<>();
        for (Object[] t : tails) for (MessageDigest h : hs) {
            MessageDigest d;
            try { d = (MessageDigest) h.clone(); } catch (CloneNotSupportedException e) { throw new IOException(e); }
            part(d, (String) t[0], (byte[]) t[1]);
            StringBuilder s = new StringBuilder();
            for (byte b : d.digest()) s.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            out.add(s + " target=android");
        }
        return out;
    }

    private static MessageDigest sha256() throws IOException {
        try { return MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    private static void part(MessageDigest h, String name, byte[] b) {
        h.update((name + "\0").getBytes(StandardCharsets.UTF_8));
        h.update(b);
    }

    /** One stream into several digests (the module is read once) */
    private static final class Tee extends MessageDigest {
        private final List<MessageDigest> ds;
        Tee(List<MessageDigest> ds) { super("tee"); this.ds = ds; }
        @Override protected void engineUpdate(byte b) { for (MessageDigest d : ds) d.update(b); }
        @Override protected void engineUpdate(byte[] b, int off, int len) { for (MessageDigest d : ds) d.update(b, off, len); }
        @Override protected byte[] engineDigest() { throw new UnsupportedOperationException(); }
        @Override protected void engineReset() { for (MessageDigest d : ds) d.reset(); }
    }
}
