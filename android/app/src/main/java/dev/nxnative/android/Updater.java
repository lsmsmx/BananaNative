package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.BackgroundColorSpan;
import android.text.style.BulletSpan;
import android.text.style.LeadingMarginSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.text.style.TypefaceSpan;
import android.text.style.URLSpan;
import android.util.Log;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.net.ConnectException;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import javax.net.ssl.HttpsURLConnection;

/**
 * The in-app updater (release app only, dev.bananative.android). Once per app start (a new process: not on a resume or a rotation), Home asks every release source
 * at once, with short timeouts (UpdateCheck.SOURCES: GitHub's releases API for moceure/BananaNative, the list, not /releases/latest: every release is a
 * pre-release, which /latest skips; then the same releases on git.eden-emu.dev's Forgejo API), each list through a URL with a fresh cache-busting query
 * parameter and Cache-Control: no-cache, so no shared cache in front of a host can answer with an old list (releases(Source)). A source that fails is left out (a log line); when a release in
 * any answer is newer than this app (UpdateCheck.pickAll: the highest version) a popup offers Update or Later. Update downloads the release's own APK through its
 * browser_download_url (only on that source's own hosts; the source counts the download) into the cache, checks its size and, when the API gives a digest, its
 * SHA-256, and installs it with PackageInstaller (limited to this package; Android itself refuses an update signed with another certificate); Android asks the player to confirm, and first to allow installs from this app when that is off. A check that fails says nothing (a log line).
 * <p>
 * Test apps never check, unless their files folder (Android/data/&lt;app&gt;/files/update.txt, adb) holds KEY=VALUE lines: repo=&lt;owner/name&gt; (a GitHub
 * repository), forgejo=&lt;owner/name&gt; (a repository on git.eden-emu.dev), url=&lt;https URL of a releases list in the APIs' JSON; its assets may be on its
 * own host or GitHub's&gt;, version=&lt;X.Y.Z the app takes as its own&gt; (a test app's versionName is no release version); the sources named are asked, in
 * that order. Settings › Updates › Check for updates checks at once.
 */
final class Updater {
    private Updater() {}

    private static final String TAG = "nxupdate";
    private static final long MAX_APK = 1L << 30;
    private static final String ACTION = ".UPDATE_INSTALL_STATUS";
    private static final Handler UI = new Handler(Looper.getMainLooper());

    private static boolean checkedThisStart;   // one check per app start (process)
    private static File pending;   // a verified download waiting for "install unknown apps" (resume)

    /** The download waiting for its install, or null (Settings › Storage leaves it alone) */
    static File pending() { return pending; }
    private static boolean receiverOn;
    private static WeakReference<Activity> front = new WeakReference<>(null);

    /** A message for the player, without a stack trace */
    private static final class Failure extends IOException { Failure(String m) { super(m); } }

    // ---- Settings ----

    /** The test app's override (update.txt): key → value; empty in the release app and without the file */
    private static java.util.Map<String, String> override(Context c) {
        java.util.Map<String, String> m = new java.util.HashMap<>();
        if (BuildConfig.NX_LIVE) return m;
        File f = new File(c.getExternalFilesDir(null), "update.txt");
        try {
            if (!f.isFile()) return m;
            for (String line : new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).split("\n")) {
                String l = line.trim();
                int eq = l.indexOf('=');
                if (l.isEmpty() || l.startsWith("#") || eq <= 0) continue;
                m.put(l.substring(0, eq).trim().toLowerCase(Locale.ROOT), l.substring(eq + 1).trim());
            }
        } catch (IOException | SecurityException e) { Log.w(TAG, "update.txt unreadable: " + e); }
        return m;
    }

    /** Whether this app checks at all: the release app, or a test app with an update.txt naming a repository or a URL */
    static boolean available(Context c) { return !sources(c).isEmpty(); }

    /** The sources this app asks, in order: UpdateCheck.SOURCES in the release app, those update.txt names in a test app (none without it) */
    static List<UpdateCheck.Source> sources(Context c) {
        if (BuildConfig.NX_LIVE) return UpdateCheck.SOURCES;
        java.util.Map<String, String> o = override(c);
        List<UpdateCheck.Source> l = new ArrayList<>();
        if (o.containsKey("repo")) l.add(UpdateCheck.github(o.get("repo")));
        if (o.containsKey("forgejo")) l.add(UpdateCheck.forgejo(o.get("forgejo")));
        if (o.containsKey("url")) {
            String u = o.get("url"), host = Uri.parse(u).getHost();
            List<String> hosts = new ArrayList<>(UpdateCheck.github(UpdateCheck.REPO).assetHosts);
            if (host != null) hosts.add(host.toLowerCase(Locale.ROOT));
            l.add(new UpdateCheck.Source(host != null ? host : u, u, "application/json", null, hosts.toArray(new String[0])));
        }
        return l;
    }

    /** The version this app compares against: its versionName (a test app's update.txt version= instead) */
    static String version(Context c) { return override(c).getOrDefault("version", BuildConfig.VERSION_NAME); }

    // ---- The check ----

    private static final int API_CONNECT_MS = 6000, API_READ_MS = 8000;   // a check: short; the sources are asked at once and one that is down is left out
    private static final int DL_CONNECT_MS = 15000, DL_READ_MS = 30000;   // a download
    private static final int MAX_LIST = 4 << 20;   // a releases list larger than this is not read

    /**
     * An https GET of url; label (the source's name) names the server in the messages. fresh asks every cache on the way (Cache-Control: no-cache, Pragma:
     * no-cache for the old ones) to revalidate with the origin rather than answer from a stored copy; the releases lists are fetched so, downloads are not.
     */
    private static HttpsURLConnection open(String url, String accept, String label, int connectMs, int readMs, boolean fresh) throws IOException {
        URL u = new URL(url);
        if (!"https".equals(u.getProtocol())) throw new Failure("not an https link: " + url);
        HttpsURLConnection c = (HttpsURLConnection) u.openConnection();
        c.setRequestProperty("User-Agent", "BananaNative-updater");
        c.setRequestProperty("Accept", accept);
        if (fresh) {
            c.setRequestProperty("Cache-Control", "no-cache");
            c.setRequestProperty("Pragma", "no-cache");
            c.setUseCaches(false);
        }
        c.setConnectTimeout(connectMs);
        c.setReadTimeout(readMs);
        c.setInstanceFollowRedirects(true);   // GitHub's release assets redirect to its object storage (https to https: HttpURLConnection never follows a change of protocol)
        int code = c.getResponseCode();
        if (code == 403 || code == 429) { c.disconnect(); throw new Failure(label + " rate limit, try again later"); }
        if (code != 200) { c.disconnect(); throw new Failure(label + " answered HTTP " + code); }
        return c;
    }

    private static String why(Throwable e, String label) {
        if (e instanceof UnknownHostException || e instanceof ConnectException) return "No network (cannot reach " + label + ")";
        if (e instanceof SocketTimeoutException) return "No network (" + label + " did not answer in time)";
        if (e instanceof Failure) return e.getMessage();
        if (e instanceof IllegalArgumentException) return label + "'s answer could not be read: " + e.getMessage();
        return e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
    }

    /**
     * One source's releases (network). The list is asked through a URL that no cache has seen: UpdateCheck.busted appends a fresh query parameter at request
     * time, so each check is its own cache key, and the request says Cache-Control: no-cache. A shared cache in front of git.eden-emu.dev (Cloudflare and/or
     * the server's, cache-control: public, max-age=180, x-cache-enabled: true) kept answering s.api with the list as of two hours before v0.2.3 was published
     * while the same URL with another query gave the current list, so every phone said up to date; the headers alone cannot be relied on (that cache ignored
     * max-age), the parameter can. The APK download is left as it is: an asset's URL is new with each release, and the file is checked by size and, when
     * the API gives a digest, SHA-256 (fetch).
     */
    private static List<UpdateCheck.Release> releases(UpdateCheck.Source s) throws IOException {
        HttpsURLConnection c = open(UpdateCheck.busted(s.api, Long.toString(System.currentTimeMillis())), s.accept, s.name, API_CONNECT_MS, API_READ_MS, true);
        String body;
        try (InputStream in = c.getInputStream()) {
            byte[] b = in.readNBytes(MAX_LIST + 1);
            if (b.length > MAX_LIST) throw new Failure(s.name + "'s answer is too large");
            body = new String(b, StandardCharsets.UTF_8);
        } finally { c.disconnect(); }
        return UpdateCheck.parse(body, s);
    }

    /**
     * Every source's releases, asked at once (network: call off the UI thread): one list per source, in order, null for a source that failed (a log line). Only
     * when every source fails, a Failure that says why for each.
     */
    static List<List<UpdateCheck.Release>> releases(Context ctx) throws IOException {
        List<UpdateCheck.Source> src = sources(ctx);
        int n = src.size();
        List<List<UpdateCheck.Release>> got = new ArrayList<>();
        String[] err = new String[n];
        Thread[] t = new Thread[n];
        for (int k = 0; k < n; k++) {
            got.add(null);
            final int i = k; final UpdateCheck.Source s = src.get(k);
            t[k] = new Thread(() -> {
                List<UpdateCheck.Release> l = null; String e = null;
                try { l = releases(s); }
                catch (Throwable x) { e = why(x, s.name); }
                synchronized (got) { got.set(i, l); err[i] = e; }
            }, "nx-update-" + k);
            t[k].start();
        }
        long end = System.currentTimeMillis() + API_CONNECT_MS + API_READ_MS + 2000;
        for (Thread x : t) {
            try { x.join(Math.max(1, end - System.currentTimeMillis())); } catch (InterruptedException e) { Thread.currentThread().interrupt(); break; }
        }
        List<List<UpdateCheck.Release>> out = new ArrayList<>();
        StringBuilder fails = new StringBuilder();
        synchronized (got) {
            for (int k = 0; k < n; k++) {
                List<UpdateCheck.Release> l = got.get(k);
                if (l == null) {
                    String e = err[k] != null ? err[k] : "No network (" + src.get(k).name + " did not answer in time)";
                    Log.w(TAG, "release source " + src.get(k).name + " failed: " + e);
                    fails.append(fails.length() > 0 ? "; " : "").append(e);
                }
                out.add(l);
            }
        }
        boolean any = false;
        for (List<UpdateCheck.Release> l : out) any |= l != null;
        if (!any) throw new Failure(n == 0 ? "no release source" : fails.toString());
        return out;
    }

    private static boolean gone(Activity a) { return a.isFinishing() || a.isDestroyed(); }

    /** Home's start (MainActivity.onCreate, once per process): old downloads go, then the check; a newer release opens the popup */
    static void onStart(Activity a) {
        if (checkedThisStart) return;
        checkedThisStart = true;
        Context app = a.getApplicationContext();
        new Thread(() -> {
            cleanup(app);
            if (!available(app)) return;
            int[] cur = UpdateCheck.appVersion(version(app));
            if (cur == null) { Log.i(TAG, "check skipped: version " + version(app) + " is not X.Y.Z"); return; }
            UpdateCheck.Update u;
            try { u = UpdateCheck.pickAll(releases(app), cur); }
            catch (Throwable e) {   // silent: a log line only
                Log.w(TAG, "update check failed: " + why(e, "the release sources"));
                return;
            }
            if (u == null) { Log.i(TAG, "up to date (" + version(app) + ")"); return; }
            Log.i(TAG, u.tag() + " is newer than " + version(app) + ": " + u.apk.name);
            final UpdateCheck.Update got = u;
            UI.post(() -> { if (!gone(a)) popup(a, got); });
        }, "nx-update-check").start();
    }

    /** Settings › Updates › Check for updates: the popup when a newer release exists, else "You're up to date" */
    static void checkNow(Activity a) {
        if (!available(a)) {
            msg(a, "Updates", "Only the release app (dev.bananative.android) checks for updates. A test app checks when update.txt in its files folder names repo= or url=.");
            return;
        }
        int[] cur = UpdateCheck.appVersion(version(a));
        if (cur == null) { msg(a, "Updates", "This build's version (" + version(a) + ") is not a release version: nothing to compare."); return; }
        AlertDialog wait = new AlertDialog.Builder(a).setMessage("Checking for updates…").setCancelable(false).create();
        wait.show();
        Context app = a.getApplicationContext();
        new Thread(() -> {
            UpdateCheck.Update u = null; String err = null;
            try { u = UpdateCheck.pickAll(releases(app), cur); }
            catch (Throwable e) { err = why(e, "the release sources"); Log.w(TAG, "update check failed: " + err); }
            final UpdateCheck.Update got = u; final String fail = err;
            UI.post(() -> {
                if (gone(a)) return;
                wait.dismiss();
                if (fail != null) Toast.makeText(a, "Update check failed: " + fail, Toast.LENGTH_LONG).show();
                else if (got == null) Toast.makeText(a, "You're up to date (v" + UpdateCheck.text(cur) + ")", Toast.LENGTH_LONG).show();
                else popup(a, got);
            });
        }, "nx-update-check").start();
    }

    private static void msg(Activity a, String title, String msg) { new AlertDialog.Builder(a).setTitle(title).setMessage(msg).setPositiveButton("OK", null).show(); }

    /** The popup: version, the release notes (styled, scrolling when long), size; Update, Later */
    static void popup(Activity a, UpdateCheck.Update u) {
        int p = Ui.dp(a, 20);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(p, Ui.dp(a, 4), p, Ui.dp(a, 8));
        box.addView(Ui.body(a, "Version " + UpdateCheck.text(u.version) + " is available (you have " + version(a) + ")."));
        CharSequence notes = styled(a, ReleaseNotes.parse(u.release.body));
        if (notes.length() > 0) {
            TextView t = Ui.text(a, notes, 14, R.color.bn_text, 400);
            t.setLineSpacing(Ui.dp(a, 2), 1f);
            t.setMovementMethod(android.text.method.LinkMovementMethod.getInstance());   // links open in the browser
            t.setFocusable(false);   // else the dialog focuses it and the notes open scrolled to their end
            int max = a.getResources().getDisplayMetrics().heightPixels * 45 / 100;   // the notes scroll within this; the version, size and buttons stay in view
            ScrollView sv = new ScrollView(a) {
                @Override protected void onMeasure(int w, int h) { super.onMeasure(w, MeasureSpec.makeMeasureSpec(max, MeasureSpec.AT_MOST)); }
            };
            sv.setBackground(Ui.round(Ui.col(a, R.color.bn_tint), Ui.dp(a, 12), 0, 0));
            sv.setPadding(Ui.dp(a, 12), Ui.dp(a, 8), Ui.dp(a, 12), Ui.dp(a, 8));
            sv.setClipToPadding(false);
            sv.addView(t);
            Ui.add(box, sv, 12);
        }
        Ui.add(box, Ui.small(a, u.apk.name + " · " + UpdateCheck.mb(u.apk.size) + " · from " + u.release.source.name), 10);
        new AlertDialog.Builder(a).setTitle("Update available").setView(box)
                .setPositiveButton("Update", (d, w) -> download(a, u))
                .setNegativeButton("Later", null)
                .show();
    }

    /** ReleaseNotes' blocks as styled text: headings bold and larger, bullets and numbers indented by their level, code monospace, links as their text */
    static CharSequence styled(Context c, List<ReleaseNotes.Block> blocks) {
        SpannableStringBuilder b = new SpannableStringBuilder();
        List<Object[]> spans = new ArrayList<>();   // { span, start, end, flags }, set once the text is complete: a paragraph span set earlier grows over text appended at its end
        int indent = Ui.dp(c, 18), gap = Ui.dp(c, 8), dot = Ui.dp(c, 3);
        int bullet = Ui.col(c, R.color.bn_text_2), codeBg = Ui.col(c, R.color.bn_card_stroke);
        ReleaseNotes.Kind prev = null;
        for (ReleaseNotes.Block x : blocks) {
            boolean item = x.kind == ReleaseNotes.Kind.BULLET || x.kind == ReleaseNotes.Kind.NUMBER;
            boolean prevItem = prev == ReleaseNotes.Kind.BULLET || prev == ReleaseNotes.Kind.NUMBER;
            if (b.length() > 0 && !(item && prevItem)) b.append("\n");   // a blank line between paragraphs, none between a list's items
            int start = b.length();
            String label = x.kind == ReleaseNotes.Kind.NUMBER ? x.label + " " : "";
            b.append(label).append(x.text);
            int off = start + label.length(), end = b.length();
            b.append("\n");   // each block ends its paragraph: its paragraph spans cover [start, end + 1) and never grow over what follows
            for (ReleaseNotes.Span sp : x.spans) {
                int s0 = off + sp.start, s1 = off + sp.end;
                if (s1 <= s0) continue;
                switch (sp.style) {
                    case BOLD: spans.add(new Object[] {new StyleSpan(Typeface.BOLD), s0, s1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE}); break;
                    case ITALIC: spans.add(new Object[] {new StyleSpan(Typeface.ITALIC), s0, s1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE}); break;
                    case CODE:
                        spans.add(new Object[] {new TypefaceSpan("monospace"), s0, s1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE});
                        spans.add(new Object[] {new BackgroundColorSpan(codeBg), s0, s1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE});
                        break;
                    case LINK:
                        if (sp.url != null && sp.url.startsWith("https://")) spans.add(new Object[] {new URLSpan(sp.url), s0, s1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE});
                        break;
                }
            }
            if (end == start) { prev = x.kind; continue; }
            if (x.kind == ReleaseNotes.Kind.HEADING) {
                spans.add(new Object[] {new StyleSpan(Typeface.BOLD), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE});
                spans.add(new Object[] {new RelativeSizeSpan(x.level <= 1 ? 1.3f : x.level == 2 ? 1.2f : 1.1f), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE});
            } else if (x.kind == ReleaseNotes.Kind.BULLET) {
                if (x.level > 0) spans.add(new Object[] {new LeadingMarginSpan.Standard(indent * x.level), start, end + 1, Spanned.SPAN_PARAGRAPH});
                spans.add(new Object[] {new BulletSpan(gap, bullet, dot), start, end + 1, Spanned.SPAN_PARAGRAPH});
            } else if (x.kind == ReleaseNotes.Kind.NUMBER) {
                spans.add(new Object[] {new LeadingMarginSpan.Standard(indent * x.level, indent * x.level + indent + gap), start, end + 1, Spanned.SPAN_PARAGRAPH});   // the item's next lines under its text
            }
            prev = x.kind;
        }
        int len = b.length();
        while (len > 0 && b.charAt(len - 1) == '\n') len--;
        b.delete(len, b.length());
        for (Object[] x : spans) b.setSpan(x[0], (int) x[1], Math.min(len, (int) x[2]), (int) x[3]);
        return b;
    }

    // ---- Download and install ----

    private static File dir(Context c) { return new File(c.getCacheDir(), "updates"); }

    /** Old downloads: everything in the cache's updates/ except a download waiting for its install */
    static void cleanup(Context c) {
        File[] l = dir(c).listFiles();
        if (l == null) return;
        for (File f : l) if (!f.equals(pending) && !f.delete()) Log.w(TAG, "could not delete " + f);
    }

    private static void download(Activity a, UpdateCheck.Update u) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        TextView st = Ui.body(a, "Starting…");
        box.addView(st);
        ProgressBar bar = Ui.progress(a);
        bar.setMax(1000);
        Ui.add(box, bar, 12);
        final boolean[] cancel = {false};
        AlertDialog d = Ui.sheet(a, "Downloading " + u.tag(), box).setCancelable(false).setNegativeButton("Cancel", (x, w) -> cancel[0] = true).show();
        Context app = a.getApplicationContext();
        new Thread(() -> {
            File f = null; String err = null;
            try { f = fetch(app, u, cancel, (n, total) -> UI.post(() -> {
                    bar.setProgress(total > 0 ? (int) (n * 1000 / total) : 0);
                    st.setText(UpdateCheck.mb(n) + " of " + UpdateCheck.mb(total));
                })); }
            catch (Throwable e) { err = why(e, u.release.source.name); Log.w(TAG, "download of " + u.apk.url + " failed: " + err); }
            final File got = f; final String fail = err;
            UI.post(() -> {
                if (d.isShowing()) d.dismiss();
                if (cancel[0]) return;
                if (gone(a)) return;
                if (fail != null) { msg(a, "Update not installed", fail); return; }
                install(a, got);
            });
        }, "nx-update-download").start();
    }

    interface Progress { void at(long n, long total); }

    /** Downloads the APK into the cache and checks it: the size the API gives, and the SHA-256 of its digest when it has one. Only a checked file is returned */
    static File fetch(Context c, UpdateCheck.Update u, boolean[] cancel, Progress p) throws IOException {
        long size = u.apk.size;
        if (size > MAX_APK) throw new Failure("the APK is larger than 1 GB");
        File d = dir(c);
        if (!d.isDirectory() && !d.mkdirs()) throw new Failure("cannot make " + d);
        String name = "banana-native-" + u.tag().replaceAll("[^A-Za-z0-9._-]", "_");
        File part = new File(d, name + ".part"), apk = new File(d, name + ".apk");
        part.delete(); apk.delete();
        MessageDigest md;
        try { md = MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new Failure("no SHA-256"); }
        long n = 0;
        if (!UpdateCheck.allowedAsset(u.apk.url, u.release.source)) throw new Failure("the download link is not on " + u.release.source.name + ": " + u.apk.url);   // UpdateCheck.apk checked it already
        HttpsURLConnection conn = open(u.apk.url, "application/octet-stream", u.release.source.name, DL_CONNECT_MS, DL_READ_MS, false);   // the asset's own browser_download_url, as it is: the source counts the download; no cache-busting (the link is new per release, the file is checked below)
        try (InputStream in = conn.getInputStream(); OutputStream out = new FileOutputStream(part)) {
            byte[] b = new byte[1 << 16]; int k; long shown = 0;
            while ((k = in.read(b)) > 0) {
                if (cancel[0]) throw new Failure("cancelled");
                if ((n += k) > MAX_APK || (size > 0 && n > size)) throw new Failure("the download is larger than the release says");
                out.write(b, 0, k);
                md.update(b, 0, k);
                if (n - shown >= (256 << 10)) { shown = n; p.at(n, size); }
            }
        } catch (IOException e) { part.delete(); throw e; }
        finally { conn.disconnect(); }
        p.at(n, size);
        if (size > 0 && n != size) { part.delete(); throw new Failure("the download is incomplete (" + n + " of " + size + " bytes)"); }
        if (u.sha256 != null) {
            StringBuilder h = new StringBuilder();
            for (byte x : md.digest()) h.append(String.format(Locale.ROOT, "%02x", x));
            if (!h.toString().equals(u.sha256)) { part.delete(); throw new Failure("the download's SHA-256 does not match the release (" + h + ")"); }
            Log.i(TAG, u.apk.name + ": " + n + " bytes, SHA-256 matches");
        } else Log.i(TAG, u.apk.name + ": " + n + " bytes (the release gives no SHA-256)");
        if (!part.renameTo(apk)) { part.delete(); throw new Failure("cannot rename the download"); }
        return apk;
    }

    /** Installs a checked download; when installs from this app are not allowed yet, the system's screen for it first (resume continues) */
    private static void install(Activity a, File apk) {
        if (!a.getPackageManager().canRequestPackageInstalls()) {
            pending = apk;
            new AlertDialog.Builder(a).setTitle("Allow installs")
                    .setMessage("To install the update, allow Banana Native to install apps (\"Install unknown apps\"), then come back.")
                    .setPositiveButton("Open settings", (d, w) -> a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName()))))
                    .setNegativeButton("Cancel", (d, w) -> { pending = null; apk.delete(); })
                    .setCancelable(false).show();
            return;
        }
        pending = null;
        try { commit(a, apk); }
        catch (IOException | RuntimeException e) { Log.w(TAG, "install failed: " + e); msg(a, "Update not installed", why(e, "the installer")); }
    }

    /** MainActivity.onResume: back from "Install unknown apps" with a download waiting */
    static void resume(Activity a) {
        front = new WeakReference<>(a);
        File f = pending;
        if (f == null) return;
        if (!f.isFile()) { pending = null; return; }
        if (a.getPackageManager().canRequestPackageInstalls()) install(a, f);
        else { pending = null; f.delete(); Toast.makeText(a, "Installs from this app are not allowed: the update was not installed", Toast.LENGTH_LONG).show(); }
    }

    private static void commit(Activity a, File apk) throws IOException {
        Context app = a.getApplicationContext();
        receiver(app);
        PackageInstaller pi = app.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        sp.setAppPackageName(app.getPackageName());
        sp.setSize(apk.length());
        int id = pi.createSession(sp);
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("base.apk", 0, apk.length())) {
                byte[] b = new byte[1 << 16]; int k;
                while ((k = in.read(b)) > 0) out.write(b, 0, k);
                s.fsync(out);
            }
            Intent i = new Intent(app.getPackageName() + ACTION).setPackage(app.getPackageName());
            PendingIntent sender = PendingIntent.getBroadcast(app, id, i, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);   // mutable: the installer adds its status
            s.commit(sender.getIntentSender());
        } catch (IOException | RuntimeException e) { pi.abandonSession(id); throw e; }
        apk.delete();   // the session holds its own copy
        Log.i(TAG, "install session " + id + " committed");
    }

    /** The installer's answers: its confirmation screen (the player taps Update there), or why it failed */
    private static synchronized void receiver(Context app) {
        if (receiverOn) return;
        receiverOn = true;
        app.registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                String msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Activity a = front.get();
                if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = i.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
                    if (confirm == null) return;
                    if (a != null && !gone(a)) a.startActivity(confirm);
                    else c.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                } else if (st == PackageInstaller.STATUS_SUCCESS) {
                    Log.i(TAG, "update installed");
                } else {
                    Log.w(TAG, "install failed: status " + st + (msg != null ? ": " + msg : ""));
                    if (st != PackageInstaller.STATUS_FAILURE_ABORTED && a != null && !gone(a))
                        msg(a, "Update not installed", msg != null ? msg : "The installer answered status " + st + ".");
                }
            }
        }, new IntentFilter(app.getPackageName() + ACTION), Context.RECEIVER_NOT_EXPORTED);
    }
}
