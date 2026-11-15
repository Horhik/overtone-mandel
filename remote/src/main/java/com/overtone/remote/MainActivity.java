package com.overtone.remote;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;

// Fullscreen landscape WebView. Start: assets/connect.html lists computers heard on the UDP beacon
// ("OVERTONE <port> <host>" on 8778) and connects to the first one; then the page served by the desktop
// (http://<ip>:<port>/) is the remote itself. Back / "другой компьютер" returns to the list.
public class MainActivity extends Activity {
    static final String CONNECT = "file:///android_asset/connect.html";
    static final int BEACON = 8778;

    WebView web;
    SharedPreferences prefs;
    volatile boolean running, onConnectPage;
    boolean autoConnect = true;   // only at launch: after "back" the list stays until a tap
    Thread listener;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28)
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        prefs = getSharedPreferences("remote", MODE_PRIVATE);

        web = new WebView(this);
        web.setBackgroundColor(Color.rgb(11, 13, 12));
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setUserAgentString(s.getUserAgentString() + " OvertoneRemote/1");
        web.addJavascriptInterface(new Bridge(), "Android");
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView v, String url) {
                onConnectPage = url.startsWith("file:");
            }
            @Override
            public void onReceivedError(WebView v, WebResourceRequest req, WebResourceError err) {
                if (req.isForMainFrame() && !req.getUrl().toString().startsWith("file:"))
                    showConnect("Не удалось открыть " + req.getUrl().getAuthority());
            }
        });
        setContentView(web);
        immersive();

        String host = hostFrom(getIntent());
        if (host != null) { autoConnect = false; open(host); } else showConnect(null);
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        String host = hostFrom(i);
        if (host != null) open(host);
    }

    static String hostFrom(Intent i) {
        Uri u = i == null ? null : i.getData();
        if (u == null || u.getHost() == null) return null;
        return u.getHost() + ":" + (u.getPort() > 0 ? u.getPort() : 8777);
    }

    void open(String hostPort) {
        prefs.edit().putString("last", hostPort).apply();
        onConnectPage = false;
        web.loadUrl("http://" + hostPort + "/");
    }

    void showConnect(String msg) {
        onConnectPage = true;
        web.loadUrl(CONNECT + (msg != null ? "#" + Uri.encode(msg) : ""));
    }

    @Override
    public void onBackPressed() {
        if (onConnectPage) super.onBackPressed();
        else showConnect(null);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) immersive();
    }

    @SuppressWarnings("deprecation")
    void immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    // ---------------------------------------------------------------- discovery: listen for the desktop's beacon
    @Override
    protected void onResume() {
        super.onResume();
        running = true;
        listener = new Thread(this::listen, "beacon");
        listener.start();
    }

    @Override
    protected void onPause() {
        running = false;
        super.onPause();
    }

    void listen() {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        WifiManager.MulticastLock lock = wm != null ? wm.createMulticastLock("overtone") : null;
        if (lock != null) lock.acquire();
        try (DatagramSocket sock = new DatagramSocket(null)) {
            sock.setReuseAddress(true);
            sock.setBroadcast(true);
            sock.bind(new InetSocketAddress(BEACON));
            sock.setSoTimeout(1000);
            byte[] buf = new byte[256];
            while (running) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try { sock.receive(p); } catch (SocketTimeoutException e) { continue; }
                String[] m = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8).split(" ", 3);
                if (m.length < 2 || !m[0].equals("OVERTONE")) continue;
                String hp = p.getAddress().getHostAddress() + ":" + m[1];
                String name = m.length > 2 ? m[2] : "";
                if (!onConnectPage) continue;
                String js = "window.found && found(" + JSONObject.quote(hp) + "," + JSONObject.quote(name) + ")";
                runOnUiThread(() -> { if (onConnectPage) web.evaluateJavascript(js, null); });
            }
        } catch (Exception e) {
            runOnUiThread(() -> { if (onConnectPage) web.evaluateJavascript("window.noBeacon && noBeacon()", null); });
        } finally {
            if (lock != null) lock.release();
        }
    }

    // ---------------------------------------------------------------- called from the pages
    class Bridge {
        @JavascriptInterface public void connect(String hostPort) { runOnUiThread(() -> open(hostPort)); }
        @JavascriptInterface public void disconnect() { runOnUiThread(() -> showConnect(null)); }
        @JavascriptInterface public String last() { return prefs.getString("last", ""); }
        @JavascriptInterface public boolean takeAuto() { boolean a = autoConnect; autoConnect = false; return a; }
    }
}
