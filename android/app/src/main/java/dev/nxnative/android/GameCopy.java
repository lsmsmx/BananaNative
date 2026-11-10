package dev.nxnative.android;

import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.WindowManager;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Add game › Copy into the app: the dump folder (a Storage Access Framework tree) the user picks, or a .nup the app cannot use where it is (a provider that only
 * streams it: PickedNup.Streamed; a .nup on the phone's storage is used in place, PickedNup), is copied into app storage
 * (&lt;app storage&gt;/games/&lt;its name&gt;, CopyPlan), which the app, the packer and the host read by plain paths without All files access. The permission on the
 * picked folder or file is kept (takePersistableUriPermission), so it shows in the system's list of the app's folder access and a later copy can read it again.
 * <p>
 * The cost: the copy takes the dump's size a second time (the original can then be deleted), and Android deletes app storage with the app unless the user keeps
 * its data on uninstall (hasFragileUserData). The copy is written as &lt;name&gt;.part and renamed when complete; a cut copy is deleted at the next copy of that name.
 * Remove from list deletes a copy on request (MainActivity.confirmRemove). All files access stays the way to read a dump in place.
 */
final class GameCopy {
    private GameCopy() {}

    static final int PICK_TREE = 50;
    private static volatile boolean running;

    /** Whether a copy is being written (Settings › Storage waits for it) */
    static boolean running() { return running; }

    static void pickTree(MainActivity a) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        a.startActivityForResult(i, PICK_TREE);
    }

    /** A .nup the app cannot use where it is (a provider that only streams it: PickedNup.Streamed) copied into the app, on the picker's grant */
    static void copyNup(MainActivity a, Uri u) { start(a, u, false); }

    /** MainActivity.onActivityResult: true when the request was one of these */
    static boolean activityResult(MainActivity a, int request, Intent data) {
        if (request != PICK_TREE) return false;
        Uri u = data.getData();
        try {
            a.getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) { /* a provider that grants no lasting access: the copy below still runs on this grant */ }
        start(a, u, true);
        return true;
    }

    private static final class Item { final Uri uri; final String rel; final long size; Item(Uri u, String r, long s) { uri = u; rel = r; size = s; } }

    private static void start(MainActivity a, Uri u, boolean tree) {
        if (running) { a.error("A copy is already running."); return; }
        ContentResolver cr = a.getContentResolver();
        File games = CopyPlan.games(AppStorage.root(a));
        TextView t = Ui.small(a, "Reading the list of files…");
        t.setTextSize(15);
        boolean[] cancel = {false};
        AlertDialog d = Ui.sheet(a, "Copy into the app", t).setNegativeButton("Stop", (x, w) -> cancel[0] = true).setCancelable(false).show();
        a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        running = true;
        new Thread(() -> {
            String err = null; File done = null, partF = null;
            try {
                List<Item> items = new ArrayList<>();
                String name;
                if (tree) {
                    String root = DocumentsContract.getTreeDocumentId(u);
                    name = CopyPlan.safeName(displayName(cr, DocumentsContract.buildDocumentUriUsingTree(u, root)));
                    if (name == null) throw new IOException("the folder has no name the app can use");
                    list(cr, u, root, "", items, cancel);
                } else {
                    name = CopyPlan.safeName(displayName(cr, u));
                    if (name == null) throw new IOException("the file has no name the app can use");
                    if (!name.endsWith(".nup")) throw new IOException(name + " is not a .nup file (its name must end in \".nup\", lower case)");
                    items.add(new Item(u, "", size(cr, u)));
                }
                File dest = new File(games, name), part = CopyPlan.partOf(dest);
                partF = part;
                if (dest.exists()) throw new IOException("a copy named " + name + " is already in the app (" + dest + "): remove that game with its copy first, or pick it there");
                long total = 0; for (Item i : items) total += Math.max(0, i.size);
                if (!games.isDirectory() && !games.mkdirs()) throw new IOException("cannot make " + games);
                String fit = CopyPlan.fits(total, games.getUsableSpace());
                if (fit != null) throw new IOException(fit);
                deleteTree(part);   // a copy of this name cut before
                long copied = 0; byte[] buf = new byte[1 << 20]; long shown = 0;
                for (Item i : items) {
                    File out = tree ? new File(part, i.rel) : part;
                    File parent = out.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot make " + parent);
                    try (InputStream in = cr.openInputStream(i.uri); OutputStream o = new FileOutputStream(out)) {
                        if (in == null) throw new IOException("cannot open " + i.rel);
                        for (int n; (n = in.read(buf)) > 0;) {
                            if (cancel[0]) throw new IOException("stopped");
                            o.write(buf, 0, n);
                            copied += n;
                            if (copied - shown > (64L << 20)) { shown = copied; String s = progress(name, copied, total, i.rel); a.runOnUiThread(() -> t.setText(s)); }
                        }
                    }
                }
                if (!part.renameTo(dest)) throw new IOException("cannot rename " + part + " to " + dest);
                done = dest;
                Prefs.of(a).edit().putString("copied_from_" + dest.getName(), u.toString()).commit();   // where the copy came from (a later copy can read it again)
            } catch (IOException | RuntimeException e) {
                err = e.getMessage();
                if (partF != null) deleteTree(partF);
            } finally {
                running = false;
            }
            File game = done; String why = err;
            a.runOnUiThread(() -> {
                a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (a.isDestroyed()) return;
                d.dismiss();
                if (game != null) a.copied(game);
                else a.error("Not copied: " + why + ". Nothing of the copy is kept.");
            });
        }, "game-copy").start();
    }

    private static String progress(String name, long copied, long total, String rel) {
        int pct = total > 0 ? (int) (copied * 100 / total) : 0;
        return "Copying " + name + ": " + CopyPlan.gib(copied) + " of " + CopyPlan.gib(total) + " GiB (" + pct + " %)\n" + rel
                + "\n\nKeep the app open. The copy goes to the app's own storage, so it needs no All files access.";
    }

    /** The tree's files below doc, with their paths relative to the tree's root */
    private static void list(ContentResolver cr, Uri tree, String doc, String rel, List<Item> out, boolean[] cancel) throws IOException {
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, doc);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = cr.query(kids, cols, null, null, null)) {
            if (c == null) throw new IOException("cannot list " + (rel.isEmpty() ? "the folder" : rel));
            while (c.moveToNext()) {
                if (cancel[0]) throw new IOException("stopped");
                String id = c.getString(0), n = CopyPlan.safeName(c.getString(1)), mime = c.getString(2);
                if (n == null) throw new IOException("a file in " + (rel.isEmpty() ? "the folder" : rel) + " has a name the app cannot use: " + c.getString(1));
                String r = rel.isEmpty() ? n : rel + "/" + n;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) list(cr, tree, id, r, out, cancel);
                else out.add(new Item(DocumentsContract.buildDocumentUriUsingTree(tree, id), r, c.isNull(3) ? 0 : c.getLong(3)));
            }
        }
    }

    private static String displayName(ContentResolver cr, Uri u) {
        try (Cursor c = cr.query(u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            return c != null && c.moveToFirst() ? c.getString(0) : null;
        }
    }

    private static long size(ContentResolver cr, Uri u) {
        try (Cursor c = cr.query(u, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            return c != null && c.moveToFirst() && !c.isNull(0) ? c.getLong(0) : 0;
        }
    }

    /** Deletes a copy (or a cut one) inside app storage's games folder; anything else is left alone */
    static boolean deleteCopy(android.content.Context c, File f) {
        if (!CopyPlan.isCopy(AppStorage.root(c), f.getPath())) return false;
        deleteTree(f);
        return !f.exists();
    }

    private static void deleteTree(File f) {
        File[] k = f.isDirectory() ? f.listFiles() : null;
        if (k != null) for (File x : k) deleteTree(x);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
