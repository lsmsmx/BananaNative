package dev.nxnative.android;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.Arrays;
import java.util.Comparator;

/** The run logs (<external files>/logs/*.log) and their digests (*.digest.txt, ExitWatch), read-only, for the Share log button: content://<application ID>.logs/<file name> */
public final class LogProvider extends ContentProvider {
    static final String AUTHORITY = BuildConfig.APPLICATION_ID + ".logs";
    /** The suffix of a run log's digest (<log name without .log>.digest.txt) */
    static final String DIGEST = ".digest.txt";

    static File dir(Context c) {
        File d = new File(c.getExternalFilesDir(null), "logs");
        d.mkdirs();
        return d;
    }

    /** The newest run log, or null */
    static File newest(Context c) { return newest(c, null); }

    /** The newest run log of one game (<title ID>-<time>.log), or of any run when titleId is null; null when there is none */
    static File newest(Context c, String titleId) {
        File[] f = dir(c).listFiles((d, n) -> n.endsWith(".log") && (titleId == null || n.startsWith(titleId + "-")));
        if (f == null || f.length == 0) return null;
        return Arrays.stream(f).max(Comparator.comparingLong(File::lastModified)).get();
    }

    static Uri uri(File log) { return Uri.parse("content://" + AUTHORITY + "/" + Uri.encode(log.getName())); }

    private File file(Uri uri) throws FileNotFoundException {
        String n = uri.getLastPathSegment();
        if (n == null || n.contains("/") || !(n.endsWith(".log") || n.endsWith(DIGEST))) throw new FileNotFoundException(String.valueOf(uri));
        File f = new File(dir(getContext()), n);
        if (!f.isFile()) throw new FileNotFoundException(f.getPath());
        return f;
    }

    @Override public boolean onCreate() { return true; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("logs are read-only");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        File f;
        try { f = file(uri); } catch (FileNotFoundException e) { return null; }
        MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        c.addRow(new Object[] {f.getName(), f.length()});
        return c;
    }

    @Override public String getType(Uri uri) { return "text/plain"; }
    @Override public Uri insert(Uri uri, ContentValues v) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
