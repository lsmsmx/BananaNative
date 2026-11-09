package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import javax.net.ssl.HttpsURLConnection;

/**
 * Driver download (GpuDriver's "Install…" → "Download…"): lists the .zip assets of the public GitHub releases of the repositories below and, only when the user taps
 * one, downloads it into the cache, installs it through GpuDriver.install and picks it. Nothing is fetched in the background or without a tap; no driver is bundled.
 * The repositories' own licences apply to what they publish (android/NOTICE.md).
 */
final class DriverFetch {
    private DriverFetch() {}

    /** The one list of driver repositories: { GitHub owner/name, label } */
    static final String[][] REPOS = {
        {"MrPurple666/purple-turnip", "Mr. Purple Turnip"},
        {"K11MCH1/AdrenoToolsDrivers", "KIMCHI Turnip"},
        {"whitebelyash/freedreno_turnip-CI", "whitebelyash Turnip CI"},
        {"Weab-chan/freedreno_turnip-CI", "Weab-chan Freedreno CI"},
        {"crueter/GameHub-8Elite-Drivers", "GameHub Adreno 8xx"},
        {"StevenMXZ/Adreno-Tools-Drivers", "StevenMXZ Turnip and Qualcomm"},
    };

    private static final long MAX_ZIP = 512L << 20;
    private static final Handler UI = new Handler(Looper.getMainLooper());

    static final class Asset {
        final String repo, tag, name, url; final long size;
        Asset(String repo, String tag, String name, String url, long size) { this.repo = repo; this.tag = tag; this.name = name; this.url = url; this.size = size; }
        String label() { return tag + "\n  " + name + String.format(Locale.US, "  (%.1f MB)", size / 1048576.0); }
    }

    /** A message for the user, without a stack trace */
    private static final class Failure extends IOException { Failure(String m) { super(m); } }

    private static HttpsURLConnection open(String url, String accept) throws IOException {
        URL u = new URL(url);
        if (!"https".equals(u.getProtocol())) throw new Failure("not an https link: " + url);
        HttpsURLConnection c = (HttpsURLConnection) u.openConnection();
        c.setRequestProperty("User-Agent", "BananaNative-driver-fetch");
        c.setRequestProperty("Accept", accept);
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(true);   // release assets redirect to GitHub's object storage (https to https)
        int code = c.getResponseCode();
        if (code == 403 || code == 429) { c.disconnect(); throw new Failure("GitHub rate limit, try again later"); }
        if (code != 200) { c.disconnect(); throw new Failure("GitHub answered HTTP " + code); }
        return c;
    }

    private static String why(Throwable e) {
        if (e instanceof UnknownHostException || e instanceof ConnectException) return "No network (cannot reach GitHub)";
        if (e instanceof SocketTimeoutException) return "No network (GitHub did not answer in time)";
        if (e instanceof Failure) return e.getMessage();
        if (e instanceof JSONException) return "GitHub's answer could not be read: " + e.getMessage();
        return e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();   // GpuDriver.install's reasons (no meta.json, …)
    }

    /** The .zip assets of a repository's releases, newest release first (network: call off the UI thread) */
    static List<Asset> list(String repo) throws IOException, JSONException {
        HttpsURLConnection c = open("https://api.github.com/repos/" + repo + "/releases?per_page=30", "application/vnd.github+json");
        String body;
        try (InputStream in = c.getInputStream()) { body = new String(in.readAllBytes(), StandardCharsets.UTF_8); }
        finally { c.disconnect(); }
        List<Asset> out = new ArrayList<>();
        JSONArray rel = new JSONArray(body);
        for (int k = 0; k < rel.length(); k++) {
            JSONObject r = rel.getJSONObject(k);
            if (r.optBoolean("draft")) continue;
            String tag = r.optString("tag_name", r.optString("name", "?"));
            JSONArray as = r.optJSONArray("assets");
            if (as == null) continue;
            for (int j = 0; j < as.length(); j++) {
                JSONObject a = as.getJSONObject(j);
                String n = a.optString("name", ""), url = a.optString("browser_download_url", "");
                if (n.toLowerCase(Locale.ROOT).endsWith(".zip") && url.startsWith("https://")) out.add(new Asset(repo, tag, n, url, a.optLong("size", 0)));
            }
        }
        return out;
    }

    /** Downloads the asset into the cache, installs it and returns the driver (network: call off the UI thread) */
    static GpuDriver.Info fetch(Activity a, Asset as) throws IOException {
        if (as.size > MAX_ZIP) throw new Failure("the zip is larger than 512 MB: not a driver package");
        File tmp = File.createTempFile("driver-", ".zip", a.getCacheDir());
        try {
            HttpsURLConnection c = open(as.url, "application/octet-stream");
            try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(tmp)) {
                byte[] b = new byte[1 << 16]; long n = 0; int k;
                while ((k = in.read(b)) > 0) { if ((n += k) > MAX_ZIP) throw new Failure("the download is larger than 512 MB: not a driver package"); out.write(b, 0, k); }
            } finally { c.disconnect(); }
            try (InputStream in = new FileInputStream(tmp)) { return GpuDriver.install(a, in, as.name.replaceAll("(?i)(\\.adpkg)?\\.zip$", "")); }
        } finally { tmp.delete(); }
    }

    private static boolean gone(Activity a) { return a.isFinishing() || a.isDestroyed(); }

    private static AlertDialog busy(Activity a, String msg) {
        AlertDialog d = new AlertDialog.Builder(a).setMessage(msg).setCancelable(false).create();
        d.show();
        return d;
    }

    /** "Download…": the repositories, then a repository's zips; a tap on a zip downloads, installs and picks it, then reopens the driver dialog (request = its picker's request code) */
    static void dialog(Activity a, int request) {
        String[] labels = new String[REPOS.length];
        for (int k = 0; k < REPOS.length; k++) labels[k] = REPOS[k][1] + "\n  github.com/" + REPOS[k][0];
        new AlertDialog.Builder(a).setTitle("Download a driver (GitHub releases)")
                .setItems(labels, (d, w) -> releases(a, REPOS[w][0], request))
                .setNegativeButton("Cancel", null).show();
    }

    private static void releases(Activity a, String repo, int request) {
        AlertDialog wait = busy(a, "Reading the releases of " + repo + "…");
        new Thread(() -> {
            List<Asset> l = null; String err = null;
            try { l = list(repo); } catch (Throwable e) { err = why(e); }   // nothing on this thread may end the app
            final List<Asset> got = l; final String fail = err;
            UI.post(() -> {
                if (gone(a)) return;
                wait.dismiss();
                if (fail != null) { Toast.makeText(a, repo + ": " + fail, Toast.LENGTH_LONG).show(); return; }
                if (got.isEmpty()) { Toast.makeText(a, repo + ": no .zip assets in its releases", Toast.LENGTH_LONG).show(); return; }
                String[] labels = new String[got.size()];
                for (int k = 0; k < got.size(); k++) labels[k] = got.get(k).label();
                new AlertDialog.Builder(a).setTitle(repo)
                        .setItems(labels, (d, w) -> download(a, got.get(w), request))
                        .setNegativeButton("Back", (d, w) -> dialog(a, request)).show();
            });
        }, "driver-list").start();
    }

    private static void download(Activity a, Asset as, int request) {
        AlertDialog wait = busy(a, "Downloading " + as.name + "…");
        new Thread(() -> {
            GpuDriver.Info i = null; String err = null;
            try { i = fetch(a, as); } catch (Throwable e) { err = why(e); }   // a damaged zip throws unchecked exceptions too (DriverZip turns them into reasons)
            final GpuDriver.Info got = i; final String fail = err;
            if (got != null) GpuDriver.select(a, got);
            UI.post(() -> {
                if (gone(a)) return;
                wait.dismiss();
                if (fail != null) { Toast.makeText(a, "Not installed: " + fail, Toast.LENGTH_LONG).show(); return; }
                Toast.makeText(a, "Installed and picked " + got.label(), Toast.LENGTH_LONG).show();
                GpuDriver.dialog(a, request);
            });
        }, "driver-download").start();
    }
}
