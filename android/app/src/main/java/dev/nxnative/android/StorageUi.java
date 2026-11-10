package dev.nxnative.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.Context;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Settings › Storage: lists what the app wrote that can go (StorageScan), grouped by kind with each item's size and the total, and removes only what the player ticks
 * and confirms. A game's pack and shaders go only through its "Remove game data…", which names every folder first. Saves are never listed or removed (StorageScan's
 * guard; the save folders of every game, picked or default, are added to it here). Refused while a game, Prepare, a copy, a save backup or a pack export runs.
 */
final class StorageUi {
    private StorageUi() {}

    /** Why nothing may be scanned now, or null */
    private static String busy(Context c, List<Game> games) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> l = am == null ? null : am.getRunningAppProcesses();
        if (l != null) for (ActivityManager.RunningAppProcessInfo p : l) {
            if (p.processName == null) continue;
            if (p.processName.endsWith(":game")) return "A game is running: stop it first (its in-game panel › Stop the game).";
            if (p.processName.endsWith(":pack")) return "Prepare is running: wait for it, or pause it, and close the app's Prepare first.";
        }
        if (GameCopy.running()) return "A copy into the app is running: wait for it.";
        for (Game g : games) if (SaveUi.busy(g.titleId) || PackUi.busy(g.titleId)) return "A save backup or a pack export of " + g.name + " is running: wait for it.";
        return null;
    }

    /** The app's folders, with every save folder the app knows of protected */
    static StorageScan.Roots roots(Context c, List<Game> games) {
        StorageScan.Roots r = new StorageScan.Roots(AppStorage.root(c), c.getFilesDir(), c.getCacheDir(), c.getNoBackupFilesDir());
        Set<String> ids = new LinkedHashSet<>();
        for (Profile p : Profile.all()) ids.add(p.titleId);
        for (Game g : games) {
            ids.add(g.titleId);
            r.save(Folders.save(c, g));
            r.save(Prefs.oldSaveDefault(new File(g.gameDir), g.titleId));
        }
        for (String id : ids) {
            String picked = Prefs.folder(c, "save_" + id);
            if (picked != null) r.save(new File(picked));
            r.save(AppStorage.save(c, id));
        }
        return r;
    }

    static List<StorageScan.Listed> listed(List<Game> games) {
        List<StorageScan.Listed> l = new ArrayList<>();
        for (Game g : games) l.add(new StorageScan.Listed(g.titleId, g.gameDir, g.packDir));
        return l;
    }

    static void dialog(MainActivity a) {
        List<Game> games = Game.load(a);
        String why = busy(a, games);
        if (why != null) { a.error(why); return; }
        AlertDialog wait = new AlertDialog.Builder(a).setMessage("Looking at the app's storage…").setCancelable(false).show();
        Context app = a.getApplicationContext();
        new Thread(() -> {
            StorageScan.Roots r = roots(app, games);
            List<StorageScan.Item> items = StorageScan.scan(r, listed(games), BuildConfig.NX_DMSH_VK_CHAIN, Updater.pending());
            a.runOnUiThread(() -> {
                wait.dismiss();
                if (!gone(a)) show(a, r, games, items);
            });
        }, "nxstorage-scan").start();
    }

    private static boolean gone(Activity a) { return a.isFinishing() || a.isDestroyed(); }

    private static void show(MainActivity a, StorageScan.Roots r, List<Game> games, List<StorageScan.Item> items) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        TextView total = Ui.body(a, "");
        box.addView(total);
        List<CheckBox> boxes = new ArrayList<>();
        List<StorageScan.Item> order = new ArrayList<>();
        Runnable sum = () -> {
            long n = 0;
            for (int i = 0; i < boxes.size(); i++) if (boxes.get(i).isChecked()) n += order.get(i).bytes;
            total.setText("Found " + StorageScan.bytes(StorageScan.total(items)) + " that can go; " + StorageScan.bytes(n) + " ticked. Saves are never listed or removed.");
        };
        if (items.isEmpty()) Ui.add(box, Ui.small(a, "Nothing to clean up."), 8);
        for (Map.Entry<StorageScan.Kind, List<StorageScan.Item>> e : StorageScan.byKind(items).entrySet()) {
            StorageScan.Kind k = e.getKey();
            Ui.add(box, Ui.section(a, k.title + " · " + StorageScan.bytes(StorageScan.total(e.getValue()))), 16);
            Ui.add(box, Ui.small(a, k.about), 2);
            for (StorageScan.Item it : e.getValue()) {
                CheckBox cb = new CheckBox(a);
                cb.setText(it.label + " · " + StorageScan.bytes(it.bytes));
                cb.setChecked(k.preselected);
                cb.setOnCheckedChangeListener((b, on) -> sum.run());
                Ui.add(box, cb, 2);
                boxes.add(cb);
                order.add(it);
            }
        }
        sum.run();
        if (!games.isEmpty()) {
            Ui.add(box, Ui.section(a, "A game's data"), 16);
            Ui.add(box, Ui.small(a, "A game's pack and shaders are kept while it is in your list. Remove game data… shows what it takes first."), 2);
        }
        AlertDialog[] d = {null};
        for (Game g : games) Ui.add(box, Ui.button(a, "Remove " + g.name + " data…", Ui.Kind.TONAL, v -> { d[0].dismiss(); gameData(a, r, games, g); }), 6);
        AlertDialog.Builder b = Ui.sheet(a, "Storage", box).setNegativeButton("Close", null);
        if (!items.isEmpty()) b.setPositiveButton("Remove ticked", (x, w) -> {
            List<StorageScan.Item> pick = new ArrayList<>();
            for (int i = 0; i < boxes.size(); i++) if (boxes.get(i).isChecked()) pick.add(order.get(i));
            if (pick.isEmpty()) return;
            confirm(a, r, "Remove " + pick.size() + " item" + (pick.size() == 1 ? "" : "s") + "?", pick, null);
        });
        d[0] = b.show();
    }

    private static void gameData(MainActivity a, StorageScan.Roots r, List<Game> games, Game g) {
        List<StorageScan.Item> l = StorageScan.gameData(r, listed(List.of(g)).get(0), listed(games));
        if (l.isEmpty()) { a.error(g.name + " has no data in the app's storage that can go (its pack is elsewhere, or there is none). Saves are never removed."); return; }
        confirm(a, r, "Remove " + g.name + " data?", l, "The game stays in your list; Play then needs Prepare again (or Import pack…). Your saves stay.");
    }

    /** Names exactly what goes, then removes it off the UI thread (each item checked by the guard again) */
    private static void confirm(MainActivity a, StorageScan.Roots r, String title, List<StorageScan.Item> pick, String note) {
        List<StorageScan.Item> l = StorageScan.distinct(pick);
        StringBuilder m = new StringBuilder();
        for (StorageScan.Item it : l) m.append("· ").append(it.label).append(" (").append(StorageScan.bytes(it.bytes)).append(")\n");
        m.append("\n").append(StorageScan.bytes(StorageScan.total(l))).append(" in all. This cannot be undone.");
        if (note != null) m.append(" ").append(note);
        else m.append(" Saves are never removed.");
        Ui.confirmDanger(a, title, m, "Remove", () -> {
            String why = busy(a, Game.load(a));   // something may have started while the list showed
            if (why != null) { a.error(why); return; }
            AlertDialog wait = new AlertDialog.Builder(a).setMessage("Removing…").setCancelable(false).show();
            new Thread(() -> {
                StorageScan.Result res = StorageScan.delete(r, l);
                a.runOnUiThread(() -> {
                    wait.dismiss();
                    if (gone(a)) return;
                    a.refresh();
                    String done = StorageScan.bytes(res.freed) + " freed.";
                    if (res.problems.isEmpty()) new AlertDialog.Builder(a).setMessage(done).setPositiveButton("OK", null).show();
                    else Ui.explain(a, "Not everything was removed", done, String.join("\n", res.problems)).setPositiveButton("OK", null).show();
                });
            }, "nxstorage-remove").start();
        });
    }
}
