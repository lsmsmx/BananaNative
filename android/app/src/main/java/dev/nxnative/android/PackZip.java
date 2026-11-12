package dev.nxnative.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.TreeMap;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * A game's Export pack… and Import pack… (its Manage sheet, PackUi). No Android classes: the JVM test (PackZipJvmTest, run by
 * dm/tools/tests/test_android_packzip.py) runs it on the Mac.
 * <ul>
 * <li><b>Export</b> writes the pack folder (pack.tsv at the zip's root, the modules, shaders/…) as one zip, STORED (the library and the code do not compress, and a
 *     stored zip reads back fast), named banana-pack-&lt;title ID&gt;-&lt;version&gt;.zip, into &lt;name&gt;.partial first, which is read back (every entry's size and CRC
 *     against the pack's file) and only then gets its name. The pack is only read.</li>
 * <li><b>Import</b> takes such a zip (pack.tsv at its root, or in its one top folder) or a pack folder. The title in pack.tsv must be a game of this APK (Profile)
 *     and, from a game's sheet, that game's. The pack is unpacked (or copied) into &lt;packs&gt;/&lt;title ID&gt;.partial; an entry whose name would land outside it
 *     (an absolute path, "..", a backslash: zip slip) refuses the whole zip. The copy is checked: Pack.read (its format, every module's patched.bin), its code
 *     finished (Pack.unfinishedModule) and a shader library of some version in it (an older one goes through the usual restamp / shader update at Play). Only then
 *     is it swapped in (swapIn): the pack already in &lt;packs&gt;/&lt;title ID&gt; is kept as &lt;title ID&gt;.replaced until the new one has its name, then removed.
 *     A swap that stopped between the two renames is put back by recover.</li>
 * </ul>
 */
final class PackZip {
    static final String PARTIAL = ".partial", REPLACED = ".replaced";

    private PackZip() {}

    /** banana-pack-&lt;title ID&gt;-&lt;version&gt;.zip */
    static String exportName(Pack p) { return "banana-pack-" + p.titleId + "-" + p.version.replaceAll("[^0-9A-Za-z._-]", "_") + ".zip"; }

    /** Every file under a folder by its relative path ("/" between names), sorted; a link or anything but files and folders is refused */
    static TreeMap<String, File> files(File dir) throws IOException {
        TreeMap<String, File> out = new TreeMap<>();
        files(dir, "", out);
        return out;
    }

    private static void files(File dir, String rel, TreeMap<String, File> out) throws IOException {
        String[] names = dir.list();
        if (names == null) throw new IOException("cannot list " + dir);
        for (String n : names) {
            File f = new File(dir, n);
            String r = rel.isEmpty() ? n : rel + "/" + n;
            if (Files.isSymbolicLink(f.toPath())) throw new IOException(r + " is a link: a pack holds files and folders only");
            if (Files.isDirectory(f.toPath(), LinkOption.NOFOLLOW_LINKS)) files(f, r, out);
            else if (Files.isRegularFile(f.toPath(), LinkOption.NOFOLLOW_LINKS)) out.put(r, f);
            else throw new IOException(r + " is neither a file nor a folder");
        }
    }

    private static long crc(File f) throws IOException {
        CRC32 c = new CRC32();
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[1 << 16];
            int n;
            while ((n = in.read(b)) > 0) c.update(b, 0, n);
        }
        return c.getValue();
    }

    /** A name in dir not taken yet: name, else name-2.zip, name-3.zip… */
    static File freeFile(File dir, String name) {
        File f = new File(dir, name);
        String base = name.endsWith(".zip") ? name.substring(0, name.length() - 4) : name;
        for (int k = 2; f.exists() || new File(f.getPath() + PARTIAL).exists(); k++) f = new File(dir, base + "-" + k + ".zip");
        return f;
    }

    /** Exports the pack folder as a stored zip into outDir (see the class comment); returns the zip */
    static File export(File packDir, File outDir) throws IOException {
        Pack p = Pack.read(packDir);
        if (SaveStore.inside(outDir, packDir)) throw new IOException("the zip cannot go inside the pack folder " + packDir);
        if (!outDir.isDirectory() && !outDir.mkdirs()) throw new IOException("cannot create " + outDir);
        TreeMap<String, File> fl = files(packDir);
        File zip = freeFile(outDir, exportName(p)), tmp = new File(zip.getPath() + PARTIAL);
        TreeMap<String, long[]> want = new TreeMap<>();
        try {
            try (FileOutputStream fo = new FileOutputStream(tmp); ZipOutputStream z = new ZipOutputStream(fo)) {
                z.setMethod(ZipOutputStream.STORED);
                byte[] b = new byte[1 << 16];
                for (java.util.Map.Entry<String, File> e : fl.entrySet()) {
                    File f = e.getValue();
                    long size = f.length(), c = crc(f);
                    ZipEntry ze = new ZipEntry(e.getKey());
                    ze.setMethod(ZipEntry.STORED);
                    ze.setSize(size);
                    ze.setCompressedSize(size);
                    ze.setCrc(c);
                    ze.setTime(f.lastModified());
                    z.putNextEntry(ze);
                    long n = 0;
                    try (InputStream in = new FileInputStream(f)) {
                        int k;
                        while ((k = in.read(b)) > 0) { z.write(b, 0, k); n += k; }
                    }
                    if (n != size) throw new IOException(e.getKey() + " changed while it was written");   // ZipOutputStream checks the CRC too
                    z.closeEntry();
                    want.put(e.getKey(), new long[] {size, c});
                }
                z.finish();
                fo.getFD().sync();
            }
            // read back: the same entries, sizes and CRCs as the pack's files
            try (ZipFile zf = new ZipFile(tmp)) {
                int count = 0;
                for (Enumeration<? extends ZipEntry> en = zf.entries(); en.hasMoreElements(); ) {
                    ZipEntry ze = en.nextElement();
                    long[] w = want.get(ze.getName());
                    if (w == null || w[0] != ze.getSize() || w[1] != ze.getCrc()) throw new IOException("the zip does not match the pack at " + ze.getName());
                    count++;
                }
                if (count != want.size()) throw new IOException("the zip has " + count + " files, the pack " + want.size());
            }
            if (!tmp.renameTo(zip)) throw new IOException("cannot name the zip " + zip);
        } finally {
            if (tmp.exists()) tmp.delete();
        }
        return zip;
    }

    /** A zip entry's name as a path inside the pack (the top folder stripped when the zip has one); null for the top folder itself. Refuses zip slip */
    static String safeName(String name, String prefix) throws IOException {
        if (name.indexOf('\\') >= 0 || name.startsWith("/") || name.indexOf('\0') >= 0 || name.matches("^[A-Za-z]:.*"))
            throw new IOException("the zip has an entry outside the pack (" + name + "): it is not imported");
        for (String s : name.split("/", -1))
            if (s.equals("..") || s.equals(".")) throw new IOException("the zip has an entry outside the pack (" + name + "): it is not imported");
        if (!prefix.isEmpty()) {
            if (!name.startsWith(prefix)) throw new IOException("the zip has files beside its top folder (" + name + ")");
            name = name.substring(prefix.length());
        }
        while (name.endsWith("/")) name = name.substring(0, name.length() - 1);
        return name.isEmpty() ? null : name;
    }

    /** "" when pack.tsv is at the zip's root, "&lt;folder&gt;/" when it is in its one top folder; IOException when the zip has no pack.tsv there */
    static String prefix(ZipFile zf) throws IOException {
        if (zf.getEntry("pack.tsv") != null) return "";
        String top = null;
        for (Enumeration<? extends ZipEntry> en = zf.entries(); en.hasMoreElements(); ) {
            String n = en.nextElement().getName();
            int s = n.indexOf('/');
            String t = s < 0 ? null : n.substring(0, s + 1);
            if (t == null || top != null && !top.equals(t)) throw new IOException("the zip has no pack.tsv at its root");
            top = t;
        }
        if (top == null || zf.getEntry(top + "pack.tsv") == null) throw new IOException("the zip has no pack.tsv (it is not a pack made by Export pack…)");
        return top;
    }

    /** The title ID pack.tsv names (upper case), of a zip or a pack folder */
    static String titleOf(File source) throws IOException {
        byte[] tsv;
        if (source.isDirectory()) {
            File f = new File(source, "pack.tsv");
            if (!f.isFile()) throw new IOException(source + " has no pack.tsv: not a pack folder");
            tsv = Files.readAllBytes(f.toPath());
        } else {
            try (ZipFile zf = new ZipFile(source)) {
                ZipEntry e = zf.getEntry(prefix(zf) + "pack.tsv");
                try (InputStream in = zf.getInputStream(e)) { tsv = Io.all(in); }
            } catch (java.util.zip.ZipException e) {
                throw new IOException(source.getName() + " is not a zip file (" + e.getMessage() + ")");
            }
        }
        for (String l : new String(tsv, StandardCharsets.UTF_8).split("\n")) {
            if (l.startsWith("title_id\t")) return l.substring(9).trim().toUpperCase(Locale.ROOT);
        }
        throw new IOException("pack.tsv has no title_id");
    }

    /** Unpacks a zip into to (which must not exist), every entry checked by safeName */
    static void extract(File zip, File to) throws IOException {
        try (ZipFile zf = new ZipFile(zip)) {
            String prefix = prefix(zf);
            List<ZipEntry> all = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> en = zf.entries(); en.hasMoreElements(); ) all.add(en.nextElement());
            for (ZipEntry e : all) safeName(e.getName(), prefix);   // the whole zip first: nothing is written from a zip with one bad name
            if (!to.mkdirs()) throw new IOException("cannot create " + to);
            String root = to.getCanonicalPath() + File.separator;
            byte[] b = new byte[1 << 16];
            for (ZipEntry e : all) {
                String n = safeName(e.getName(), prefix);
                if (n == null) continue;
                File f = new File(to, n);
                if (!f.getCanonicalPath().startsWith(root)) throw new IOException("the zip has an entry outside the pack (" + e.getName() + "): it is not imported");
                if (e.isDirectory()) { if (!f.isDirectory() && !f.mkdirs()) throw new IOException("cannot create " + f); continue; }
                File parent = f.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
                if (f.exists()) throw new IOException("the zip has " + n + " twice");
                try (InputStream in = zf.getInputStream(e); OutputStream out = new FileOutputStream(f)) {   // ZipFile checks each entry's CRC as it reads
                    int k;
                    while ((k = in.read(b)) > 0) out.write(b, 0, k);
                    ((FileOutputStream) out).getFD().sync();
                }
            }
        } catch (java.util.zip.ZipException e) {
            throw new IOException(zip.getName() + " is not a whole zip file (" + e.getMessage() + ")");
        }
    }

    /**
     * Unpacks or copies a pack (a zip or a pack folder) into &lt;packs&gt;/&lt;title ID&gt;.partial and checks it (see the class comment). want: the title it must be,
     * null for any game of this APK. Returns the checked pack (its dir the .partial folder); nothing is left behind when it throws
     */
    static Pack stage(File source, File packs, String want) throws IOException {
        String id = titleOf(source);
        if (!id.matches("[0-9A-F]{16}")) throw new IOException("pack.tsv names the title " + id + ", not a title ID");
        String why = Profile.missing(id, null);
        if (why != null) throw new IOException(why);
        if (want != null && !want.equals(id)) throw new IOException("that pack is for " + id + ", not " + want);
        File target = new File(packs, id), part = new File(packs, id + PARTIAL);
        if (SaveStore.inside(source, packs) || SaveStore.inside(packs, source)) throw new IOException(source + " is the app's own pack folder (or holds it): nothing to import");
        if (part.exists()) SaveStore.deleteTree(part);   // an import that stopped: only this code writes there
        if (part.exists()) throw new IOException("cannot clear " + part);
        try {
            if (source.isDirectory()) {
                if (!part.mkdirs()) throw new IOException("cannot create " + part);
                for (java.util.Map.Entry<String, File> e : files(source).entrySet()) {
                    File f = new File(part, e.getKey());
                    if (!f.getParentFile().isDirectory() && !f.getParentFile().mkdirs()) throw new IOException("cannot create " + f.getParentFile());
                    SaveStore.copyTree(e.getValue(), f);
                }
            } else extract(source, part);
            Pack p = Pack.read(part);
            if (!p.titleId.equals(id)) throw new IOException("pack.tsv names " + p.titleId);
            if (p.simulated) throw new IOException("that pack is a placeholder of the simulated engine, not a pack to play");
            String m = p.unfinishedModule();
            if (m != null) throw new IOException("the pack's code is unfinished (" + m + " has no .android.key): prepare it again where it was made");
            if (Pack.libraryOf(part) == null && Pack.libraryVersions(part).isEmpty()) throw new IOException("the pack has no shader library (shaders/v-<version>/dmsh_vk.lib)");
            return p;
        } catch (IOException | RuntimeException e) {
            SaveStore.deleteTree(part);
            throw e instanceof IOException ? (IOException) e : new IOException(e.getMessage(), e);
        }
    }

    /** Puts a checked .partial folder in place of target; the pack that was there is kept until the new one has its name, then removed */
    static void swapIn(File part, File target) throws IOException {
        File old = new File(target.getPath() + REPLACED);
        recover(target);
        if (old.exists()) SaveStore.deleteTree(old);   // target is there (recover): a removal that stopped
        if (target.exists() && !target.renameTo(old)) throw new IOException("cannot move the current pack " + target + " aside");
        if (!part.renameTo(target)) {
            if (old.exists() && !old.renameTo(target)) throw new IOException("cannot name the new pack " + target + ", and the old one stays as " + old);
            throw new IOException("cannot name the new pack " + target + "; the old one is kept");
        }
        SaveStore.deleteTree(old);
    }

    /** A swap that stopped after the current pack went aside and before the new one had its name: the current one comes back */
    static void recover(File target) {
        File old = new File(target.getPath() + REPLACED);
        if (!target.exists() && old.isDirectory()) old.renameTo(target);
    }
}
