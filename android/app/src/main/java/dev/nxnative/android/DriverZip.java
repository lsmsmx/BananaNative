package dev.nxnative.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Unpacking a GPU driver zip (GpuDriver.install) into &lt;root&gt;/&lt;name&gt;/. No Android classes: the JVM test (DriverZipJvmTest, run by
 * dm/tools/tests/test_android_driverzip.py) runs it on the Mac. GpuDriver passes the meta.json parser (org.json on the phone).
 * <ul>
 * <li>The zip is unpacked into &lt;root&gt;/.installing first, flat (a folder in the zip is dropped, so no entry can leave it), at most 256 files and 512 MB.</li>
 * <li>Anything the zip reader throws, IOException or not (ZipInputStream throws IllegalArgumentException for an entry name that is not UTF-8), becomes an
 *     IOException with the reason: an install fails, it never crashes the app.</li>
 * <li>Only a checked copy (meta.json, the library it names) replaces an installed driver of the same name: the old folder is kept as .replaced-&lt;name&gt; until the
 *     new one has its name, and put back when that rename fails. A failed install leaves no .installing folder and the installed drivers as they were, so the
 *     picked driver (a folder name in the app's settings) stays valid. Leftovers of a killed install are cleaned at the next one (recover).</li>
 * <li>One install at a time (a lock): the picker, Download… and adb may overlap.</li>
 * </ul>
 */
final class DriverZip {
    static final int MAX_FILES = 256, MAX_META = 64 * 1024;
    static final long MAX_BYTES = 512L << 20;
    static final String INSTALLING = ".installing", REPLACED = ".replaced-";
    private static final Object LOCK = new Object();

    private DriverZip() {}

    /** meta.json's fields: { name (may be empty), libraryName }, or null when it is not a driver's meta.json */
    interface MetaReader { String[] read(byte[] json); }

    static void deleteTree(File f) {
        File[] l = f.listFiles();
        if (l != null) for (File x : l) deleteTree(x);
        f.delete();
    }

    /** A folder name made of [A-Za-z0-9._-], never starting with "." or "_", at most 64 characters */
    static String safe(String s) {
        String t = s.replaceAll("[^A-Za-z0-9._-]+", "_").replaceAll("^[._]+", "");
        return t.length() > 64 ? t.substring(0, 64) : t;
    }

    /** A killed install's leftovers: .installing goes; a .replaced-&lt;name&gt; comes back when &lt;name&gt; is missing, else goes */
    static void recover(File root) {
        File[] l = root.listFiles();
        if (l == null) return;
        for (File f : l) {
            String n = f.getName();
            if (n.equals(INSTALLING)) deleteTree(f);
            else if (n.startsWith(REPLACED)) {
                File dir = new File(root, n.substring(REPLACED.length()));
                if (dir.exists() || !f.renameTo(dir)) deleteTree(f);
            }
        }
    }

    static byte[] readMeta(File f) {
        if (!f.isFile() || f.length() > MAX_META) return null;
        try (InputStream in = new FileInputStream(f)) { return Io.all(in); }
        catch (IOException | SecurityException e) { return null; }
    }

    /** Unpacks the zip into root/&lt;name from its meta.json, else fallbackName&gt;/ and returns that folder; throws with the reason, leaving root as it was */
    static File install(File root, InputStream zin, String fallbackName, MetaReader meta) throws IOException {
        synchronized (LOCK) {
            if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot create " + root);
            recover(root);
            File tmp = new File(root, INSTALLING);
            if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
            try {
                return unpack(root, tmp, zin, fallbackName, meta);
            } catch (IOException e) {
                if (e.getMessage() != null && !e.getMessage().isEmpty()) throw e;
                throw new IOException("the zip cannot be read (damaged or cut short: " + e.getClass().getSimpleName() + ")", e);   // EOFException has no message
            } catch (RuntimeException e) {   // the zip reader's IllegalArgumentException ("MALFORMED" names), a damaged zip's other surprises
                throw new IOException("the zip cannot be read (damaged, or a file name in it is not UTF-8): " + (e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName()), e);
            } finally { deleteTree(tmp); }
        }
    }

    private static File unpack(File root, File tmp, InputStream zin, String fallbackName, MetaReader meta) throws IOException {
        try (ZipInputStream z = new ZipInputStream(zin)) {
            ZipEntry e; byte[] b = new byte[1 << 16]; int files = 0; long total = 0;
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String n = new File(e.getName().replace('\\', '/')).getName();   // drivers are flat; a folder in the zip is dropped, and no entry can leave the target folder
                if (n.isEmpty() || n.startsWith(".")) continue;
                if (++files > MAX_FILES) throw new IOException("more than " + MAX_FILES + " files: not a driver zip");
                try (OutputStream out = new FileOutputStream(new File(tmp, n))) {
                    int k;
                    while ((k = z.read(b)) > 0) {
                        if ((total += k) > MAX_BYTES) throw new IOException("more than 512 MB unpacked: not a driver zip");
                        out.write(b, 0, k);
                    }
                }
            }
        }
        byte[] mj = readMeta(new File(tmp, "meta.json"));
        String[] m = mj == null ? null : meta.read(mj);
        if (m == null) throw new IOException("no meta.json in the zip: not a driver package (or not a zip)");
        String lib = m[1] == null ? "" : m[1];
        if (lib.isEmpty() || lib.contains("/") || lib.contains("\\") || !new File(tmp, lib).isFile())
            throw new IOException("meta.json names " + (lib.isEmpty() ? "no library" : lib + ", which is not in the zip"));
        String name = safe(m[0] == null ? "" : m[0]);
        if (name.isEmpty()) name = safe(fallbackName);
        if (name.isEmpty()) name = "driver";
        File dir = new File(root, name), old = new File(root, REPLACED + name);
        deleteTree(old);
        if (dir.exists() && !dir.renameTo(old)) throw new IOException("cannot replace the installed " + name);
        if (!tmp.renameTo(dir)) {
            if (old.exists() && !old.renameTo(dir)) throw new IOException("cannot move the driver to " + dir + " (the earlier one is kept as " + old + ")");
            throw new IOException("cannot move the driver to " + dir);
        }
        deleteTree(old);
        return dir;
    }
}
