package com.brokis.musicplayer;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Base64;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.io.File;
import java.io.FileOutputStream;

// Native audio playback for the music player.
//
// The player used to play through the WebView's <audio> element, but the WebView
// controls its own audio output and would not route to USB-C / Bluetooth / the
// car the way real media apps do. This plugin plays the same sources through
// Android's MediaPlayer, whose output routes correctly to the active media
// device, and it requests media audio focus itself (safe now that nothing plays
// in the WebView), so phone calls / navigation prompts pause and resume cleanly.
//
// The web layer drives it through a thin adapter that mimics the <audio>
// element, so the rest of the app is unchanged. Events mirror the media element:
// loadstart, loadedmetadata, play, playing, pause, timeupdate, ended, error.
@CapacitorPlugin(name = "NativePlayer")
public class NativePlayerPlugin extends Plugin {

    private MediaPlayer player;
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest; // API 26+
    private boolean hasFocus = false;
    private boolean playWhenReady = false;
    private boolean prepared = false;
    private float volume = 1.0f;
    private boolean resumeOnFocusGain = false;
    private android.net.wifi.WifiManager.WifiLock wifiLock;
    private String currentSource = "";

    // The playback queue lives here so tracks auto-advance even while the screen
    // is off (the web layer is suspended then and can't drive the "next track"
    // logic). Sources are http(s) URLs (Jellyfin) or local file paths. The
    // parallel metadata lists let us update the lock screen / car display
    // natively on advance — the web layer can't do it while suspended.
    private final java.util.List<String> queue = new java.util.ArrayList<>();
    // Per-track alternate source, used when the primary fails (e.g. a direct
    // Jellyfin file the device can't decode falls back to a transcode). Retried
    // natively so it still works while the screen is off and JS is frozen.
    private final java.util.List<String> fallbacks = new java.util.ArrayList<>();
    private String currentFallback = "";
    private boolean triedFallback = false;
    // Counters surfaced to the UI so playback problems can be identified on the
    // device without a debugger attached.
    private int bufferingCount = 0, focusLossCount = 0, errorCount = 0;
    private final java.util.List<String> titles = new java.util.ArrayList<>();
    private final java.util.List<String> artists = new java.util.ArrayList<>();
    private final java.util.List<String> albums = new java.util.ArrayList<>();
    private int queueIndex = -1;
    private String repeatMode = "off"; // off | all | one
    // Consecutive failures while auto-advancing; caps how far we'll skip through
    // a broken queue (e.g. server unreachable) before giving up.
    private int errorStreak = 0;
    private static final int MAX_ERROR_SKIPS = 3;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private Runnable ticker;

    private final AudioManager.OnAudioFocusChangeListener focusListener = focusChange -> {
        switch (focusChange) {
            case AudioManager.AUDIOFOCUS_LOSS:
                focusLossCount++; emitDiag();
                resumeOnFocusGain = false;
                internalPause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                focusLossCount++; emitDiag();
                resumeOnFocusGain = isPlaying();
                internalPause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                // Keep playing at full volume. Self-ducking here could leave the
                // output stuck quiet if the matching GAIN never arrived, and it
                // is not needed for a music player.
                break;
            case AudioManager.AUDIOFOCUS_GAIN:
                if (resumeOnFocusGain) { resumeOnFocusGain = false; internalPlay(); }
                break;
            default:
                break;
        }
    };

    @Override
    public void load() {
        audioManager = (AudioManager) getContext().getSystemService(Context.AUDIO_SERVICE);
    }

    // ---- helpers ----

    private void emit(String event) {
        notifyListeners(event, new JSObject());
    }

    private void emit(String event, JSObject data) {
        notifyListeners(event, data);
    }

    private void emitDiag() {
        JSObject d = new JSObject();
        d.put("buffering", bufferingCount);
        d.put("focusLoss", focusLossCount);
        d.put("errors", errorCount);
        emit("diag", d);
    }

    private boolean isPlaying() {
        try { return player != null && player.isPlaying(); } catch (Exception e) { return false; }
    }

    private void startTicker() {
        stopTicker();
        ticker = new Runnable() {
            @Override
            public void run() {
                if (player != null && prepared) {
                    try {
                        JSObject d = new JSObject();
                        d.put("position", player.getCurrentPosition() / 1000.0);
                        int dur = player.getDuration();
                        d.put("duration", dur > 0 ? dur / 1000.0 : 0);
                        emit("timeupdate", d);
                    } catch (Exception e) { /* ignore */ }
                }
                handler.postDelayed(this, 500);
            }
        };
        handler.postDelayed(ticker, 500);
    }

    private void stopTicker() {
        if (ticker != null) { handler.removeCallbacks(ticker); ticker = null; }
    }

    // Request audio focus ONCE and hold it for the whole listening session.
    // Rebuilding an AudioFocusRequest on every track (as this used to do)
    // orphans the previous request, and the stale request can deliver a focus
    // LOSS to our listener mid-playback — which showed up as random pauses.
    private int requestFocus() {
        if (audioManager == null) return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        if (hasFocus) return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        try {
            int res;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (focusRequest == null) {
                    AudioAttributes attrs = new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build();
                    focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(attrs)
                        .setOnAudioFocusChangeListener(focusListener, handler)
                        .setWillPauseWhenDucked(false)
                        .build();
                }
                res = audioManager.requestAudioFocus(focusRequest);
            } else {
                res = audioManager.requestAudioFocus(
                    focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
            }
            if (res == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) hasFocus = true;
            return res;
        } catch (Exception e) {
            return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        }
    }

    private void abandonFocus() {
        if (audioManager == null || !hasFocus) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (focusRequest != null) audioManager.abandonAudioFocusRequest(focusRequest);
            } else {
                audioManager.abandonAudioFocus(focusListener);
            }
        } catch (Exception e) { /* ignore */ }
        hasFocus = false;
    }

    // Hold a Wi-Fi lock while streaming. With the screen off Android puts Wi-Fi
    // into power save, which stalls a Jellyfin stream mid-track and reads as a
    // random pause. Only needed for network sources, not local files.
    private void acquireWifiLock() {
        try {
            if (currentSource == null || !currentSource.startsWith("http")) return;
            if (wifiLock == null) {
                android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager)
                    getContext().getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wm == null) return;
                wifiLock = wm.createWifiLock(
                    android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF, "musicplayer:stream");
                wifiLock.setReferenceCounted(false);
            }
            if (!wifiLock.isHeld()) wifiLock.acquire();
        } catch (Exception e) { /* ignore */ }
    }

    private void releaseWifiLock() {
        try { if (wifiLock != null && wifiLock.isHeld()) wifiLock.release(); } catch (Exception e) { /* ignore */ }
    }

    // Push the current track's metadata straight to the media session that the
    // @jofr plugin owns, so the lock screen / car display updates on a native
    // auto-advance (the web layer is frozen with the screen off and can't do
    // it). Fully reflective and best-effort: if the plugin's internals change,
    // the title just stops updating natively instead of crashing.
    private void updateSessionMetadata(int index) {
        try {
            if (index < 0 || index >= titles.size()) return;
            com.getcapacitor.PluginHandle handle = getBridge().getPlugin("MediaSession");
            if (handle == null) return;
            Object plugin = handle.getInstance();
            if (plugin == null) return;
            java.lang.reflect.Field f = plugin.getClass().getDeclaredField("service");
            f.setAccessible(true);
            Object svc = f.get(plugin);
            if (svc == null) return;
            svc.getClass().getMethod("setTitle", String.class).invoke(svc, titles.get(index));
            svc.getClass().getMethod("setArtist", String.class).invoke(svc, artists.get(index));
            svc.getClass().getMethod("setAlbum", String.class).invoke(svc, albums.get(index));
            try {
                int dur = (player != null && prepared) ? player.getDuration() : 0;
                svc.getClass().getMethod("setDuration", long.class).invoke(svc, (long) Math.max(0, dur));
                svc.getClass().getMethod("setPosition", long.class).invoke(svc, 0L);
            } catch (Throwable ignored) { /* optional */ }
            svc.getClass().getMethod("update").invoke(svc);
        } catch (Throwable t) { /* best effort only */ }
    }

    // Release the current player without emitting playback events.
    private void releasePlayer() {
        stopTicker();
        prepared = false;
        if (player != null) {
            try { player.reset(); } catch (Exception e) { /* ignore */ }
            try { player.release(); } catch (Exception e) { /* ignore */ }
            player = null;
        }
    }

    private void setDataSourceAndPrepare(String path, boolean autoplay) {
        setDataSourceAndPrepare(path, autoplay, "");
    }

    private void setDataSourceAndPrepare(String path, boolean autoplay, String fallback) {
        releasePlayer();
        currentFallback = (fallback == null) ? "" : fallback;
        triedFallback = false;
        // The play intent is decided by the caller (load/loadData) so that a
        // play() arriving during an async local-file load isn't lost.
        playWhenReady = autoplay;
        currentSource = (path == null) ? "" : path;
        emit("loadstart");
        try {
            player = new MediaPlayer();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            } else {
                player.setAudioStreamType(AudioManager.STREAM_MUSIC);
            }
            // Keep the CPU alive so playback continues with the screen off / in
            // the car. (WAKE_LOCK permission is declared in the manifest.)
            try { player.setWakeMode(getContext(), PowerManager.PARTIAL_WAKE_LOCK); } catch (Exception e) { /* ignore */ }
            player.setVolume(volume, volume);
            player.setDataSource(path);
            player.setOnPreparedListener(mp -> {
                prepared = true;
                JSObject d = new JSObject();
                int dur = 0;
                try { dur = mp.getDuration(); } catch (Exception e) { /* ignore */ }
                d.put("duration", dur > 0 ? dur / 1000.0 : 0);
                emit("loadedmetadata", d);
                if (playWhenReady) internalPlay();
            });
            player.setOnCompletionListener(mp -> {
                stopTicker();
                advanceOnCompletion();
            });
            // A stalled network read (Jellyfin stream) surfaces here, not as an
            // error. Counting it distinguishes "the stream ran dry" from a real
            // pause when diagnosing playback complaints.
            player.setOnInfoListener((mp, what, extra) -> {
                if (what == MediaPlayer.MEDIA_INFO_BUFFERING_START) {
                    bufferingCount++;
                    emitDiag();
                }
                return false;
            });
            player.setOnErrorListener((mp, what, extra) -> {
                JSObject d = new JSObject();
                d.put("what", what);
                d.put("extra", extra);
                emit("error", d);
                errorCount++;
                emitDiag();
                boolean wanted = playWhenReady;
                // Before giving up on this track, try its alternate source (a
                // direct file that won't decode falls back to a transcode).
                if (wanted && !triedFallback && currentFallback.length() > 0) {
                    final String alt = currentFallback;
                    triedFallback = true;
                    releasePlayer();
                    handler.postDelayed(() -> {
                        setDataSourceAndPrepare(alt, true, "");
                        triedFallback = true;
                    }, 250);
                    return true;
                }
                releasePlayer();
                // A single bad track (transient network drop, missing file) used
                // to stop playback dead until the user intervened. Skip to the
                // next one instead, capped so a wholly unreachable queue can't
                // spin forever.
                if (wanted && !queue.isEmpty() && errorStreak < MAX_ERROR_SKIPS) {
                    errorStreak++;
                    handler.postDelayed(() -> advanceOnCompletion(), 500);
                }
                return true;
            });
            player.prepareAsync();
        } catch (Exception e) {
            JSObject d = new JSObject();
            d.put("message", String.valueOf(e.getMessage()));
            emit("error", d);
            releasePlayer();
        }
    }

    private void internalPlay() {
        if (player == null || !prepared) { playWhenReady = true; return; }
        try {
            if (requestFocus() != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
                // Some devices return DELAYED/FAILED; try to play anyway rather
                // than silently doing nothing.
            }
            player.setVolume(volume, volume);
            player.start();
            acquireWifiLock();
            // Real playback started — the queue is healthy again.
            errorStreak = 0;
            emit("play");
            emit("playing");
            startTicker();
        } catch (Exception e) { /* ignore */ }
    }

    private void internalPause() {
        try {
            if (player != null && player.isPlaying()) {
                player.pause();
                stopTicker();
                releaseWifiLock();
                emit("pause");
            }
        } catch (Exception e) { /* ignore */ }
    }

    // Called when the current track finishes. Picks the next source from the
    // queue and plays it — all natively, so it works with the screen off. Emits
    // "advanced" (with the new index) so the web layer can catch up its UI when
    // it wakes; at the end of the queue emits "pause" + "ended" instead.
    private String at(java.util.List<String> list, int i) {
        if (i < 0 || i >= list.size()) return "";
        String s = list.get(i);
        return s == null ? "" : s;
    }

    // An entry is playable if it has either a primary source (a downloaded
    // file) or a fallback (the live stream) — so a track that hasn't finished
    // downloading still plays instead of being skipped.
    private boolean playable(int i) {
        if (i < 0 || i >= queue.size()) return false;
        return at(queue, i).length() > 0 || at(fallbacks, i).length() > 0;
    }

    private void playIndex(int i) {
        queueIndex = i;
        String primary = at(queue, i);
        String alt = at(fallbacks, i);
        // Prefer the downloaded file; otherwise stream, with no further retry.
        String src = primary.length() > 0 ? primary : alt;
        String fb = primary.length() > 0 ? alt : "";
        setDataSourceAndPrepare(src, true, fb);
        // Update the lock screen / car display right away — the web layer can't
        // while the screen is off.
        updateSessionMetadata(i);
        JSObject d = new JSObject();
        d.put("index", i);
        emit("advanced", d);
    }

    private void emitStopped() {
        emit("pause");
        emit("ended");
    }

    private void advanceOnCompletion() {
        int n = queue.size();
        if (n == 0) { emitStopped(); return; }
        if ("one".equals(repeatMode) && playable(queueIndex)) { playIndex(queueIndex); return; }
        // Walk forward to the next PLAYABLE entry. Entries can be empty when a
        // local file hasn't been written to disk yet; playing "" would throw and
        // kill playback, so skip over them instead of stopping.
        int idx = queueIndex;
        for (int step = 0; step < n; step++) {
            idx++;
            if (idx >= n) {
                if ("all".equals(repeatMode)) idx = 0;
                else { emitStopped(); return; }
            }
            if (playable(idx)) { playIndex(idx); return; }
        }
        emitStopped();
    }

    // ---- plugin API ----

    // Replace the playback queue and current index WITHOUT touching the player.
    // The web layer starts the current track itself (via load/loadData); this
    // just tells native what to auto-advance through when a track completes.
    private static java.util.List<String> toList(com.getcapacitor.JSArray arr) {
        java.util.List<String> out = new java.util.ArrayList<>();
        if (arr == null) return out;
        for (int i = 0; i < arr.length(); i++) {
            try {
                String s = arr.getString(i);
                out.add(s == null ? "" : s);
            } catch (Exception e) { out.add(""); }
        }
        return out;
    }

    @PluginMethod
    public void setQueue(PluginCall call) {
        final java.util.List<String> nextSources = toList(call.getArray("sources"));
        final java.util.List<String> nextFallbacks = toList(call.getArray("fallbacks"));
        final java.util.List<String> nextTitles = toList(call.getArray("titles"));
        final java.util.List<String> nextArtists = toList(call.getArray("artists"));
        final java.util.List<String> nextAlbums = toList(call.getArray("albums"));
        final int index = call.getInt("index", -1);
        final String repeat = call.getString("repeat", "off");
        getActivity().runOnUiThread(() -> {
            queue.clear();
            queue.addAll(nextSources);
            fallbacks.clear();
            fallbacks.addAll(nextFallbacks);
            while (fallbacks.size() < queue.size()) fallbacks.add("");
            titles.clear();
            titles.addAll(nextTitles);
            artists.clear();
            artists.addAll(nextArtists);
            albums.clear();
            albums.addAll(nextAlbums);
            // Pad metadata so index lookups are always in range.
            while (titles.size() < queue.size()) titles.add("");
            while (artists.size() < queue.size()) artists.add("");
            while (albums.size() < queue.size()) albums.add("");
            queueIndex = index;
            repeatMode = repeat;
            errorStreak = 0;
        });
        call.resolve();
    }

    // Fill in one queue entry's source after the web layer has written that
    // local file to disk. Cheap: no re-sending of the whole queue.
    @PluginMethod
    public void setSource(PluginCall call) {
        final int index = call.getInt("index", -1);
        final String source = call.getString("source", "");
        getActivity().runOnUiThread(() -> {
            if (index >= 0 && index < queue.size()) queue.set(index, source);
        });
        call.resolve();
    }

    // ---- Jellyfin download cache ----
    // Streaming a Jellyfin track straight into MediaPlayer proved unreliable:
    // the connection runs dry mid-song (buffering underruns) and a stream that
    // ends early looks like "track finished", which skips. Downloading the file
    // first and playing it from disk removes the network from playback
    // entirely — the same thing that made local files reliable.
    private static final long MAX_CACHE_BYTES = 1536L * 1024L * 1024L; // ~1.5 GB

    private File streamCacheDir() {
        File dir = new File(getContext().getCacheDir(), "stream");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    // Keep the cache bounded: drop the least recently modified files first.
    private void trimCache() {
        try {
            File[] files = streamCacheDir().listFiles();
            if (files == null) return;
            long total = 0;
            for (File f : files) total += f.length();
            if (total <= MAX_CACHE_BYTES) return;
            java.util.Arrays.sort(files, (a, b) -> Long.compare(a.lastModified(), b.lastModified()));
            for (File f : files) {
                if (total <= MAX_CACHE_BYTES) break;
                long len = f.length();
                if (f.delete()) total -= len;
            }
        } catch (Exception e) { /* ignore */ }
    }

    @PluginMethod
    public void cachedPath(PluginCall call) {
        final String name = call.getString("name", "");
        JSObject ret = new JSObject();
        try {
            File f = new File(streamCacheDir(), name);
            boolean ok = f.exists() && f.length() > 0;
            if (ok) f.setLastModified(System.currentTimeMillis()); // LRU touch
            ret.put("path", ok ? f.getAbsolutePath() : "");
        } catch (Exception e) {
            ret.put("path", "");
        }
        call.resolve(ret);
    }

    // Download a track to the cache and return its path. Rejects (so the caller
    // falls back to streaming) when the server won't tell us the length, since
    // without it a truncated download can't be told from a complete one — that
    // is exactly the failure we're trying to eliminate.
    @PluginMethod
    public void download(PluginCall call) {
        final String url = call.getString("url", "");
        final String name = call.getString("name", "");
        if (url.length() == 0 || name.length() == 0) { call.reject("bad args"); return; }
        new Thread(() -> {
            java.net.HttpURLConnection conn = null;
            File part = null;
            try {
                File out = new File(streamCacheDir(), name);
                if (out.exists() && out.length() > 0) {
                    JSObject ret = new JSObject();
                    ret.put("path", out.getAbsolutePath());
                    call.resolve(ret);
                    return;
                }
                conn = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
                conn.setConnectTimeout(20000);
                conn.setReadTimeout(30000);
                conn.setInstanceFollowRedirects(true);
                int code = conn.getResponseCode();
                if (code != 200) { call.reject("http " + code); return; }
                long expected = -1;
                try {
                    String cl = conn.getHeaderField("Content-Length");
                    if (cl != null) expected = Long.parseLong(cl.trim());
                } catch (Exception e) { expected = -1; }
                if (expected <= 0) { call.reject("no length"); return; }
                part = new File(streamCacheDir(), name + ".part");
                java.io.InputStream in = conn.getInputStream();
                FileOutputStream fos = new FileOutputStream(part);
                byte[] buf = new byte[65536];
                long total = 0;
                int n;
                while ((n = in.read(buf)) > 0) {
                    fos.write(buf, 0, n);
                    total += n;
                }
                fos.close();
                in.close();
                if (total < expected) { part.delete(); call.reject("incomplete"); return; }
                if (!part.renameTo(out)) { part.delete(); call.reject("rename failed"); return; }
                trimCache();
                JSObject ret = new JSObject();
                ret.put("path", out.getAbsolutePath());
                call.resolve(ret);
            } catch (Exception e) {
                try { if (part != null && part.exists()) part.delete(); } catch (Exception ig) { /* ignore */ }
                call.reject("download failed: " + e.getMessage());
            } finally {
                try { if (conn != null) conn.disconnect(); } catch (Exception ig) { /* ignore */ }
            }
        }).start();
    }

    // Report whether a local track has already been written to disk, so the web
    // layer can reuse it instead of shipping the bytes across the bridge again
    // (e.g. after an app restart). Returns an empty path when not present.
    @PluginMethod
    public void resolvePersisted(PluginCall call) {
        final String name = call.getString("name", "");
        JSObject ret = new JSObject();
        try {
            File f = new File(new File(getContext().getFilesDir(), "tracks"), name);
            ret.put("path", (f.exists() && f.length() > 0) ? f.getAbsolutePath() : "");
        } catch (Exception e) {
            ret.put("path", "");
        }
        call.resolve(ret);
    }

    // Current playback-health counters, so the UI can show them on demand even
    // if some change events were missed while the web layer was suspended.
    @PluginMethod
    public void getDiag(PluginCall call) {
        JSObject ret = new JSObject();
        ret.put("buffering", bufferingCount);
        ret.put("focusLoss", focusLossCount);
        ret.put("errors", errorCount);
        call.resolve(ret);
    }

    @PluginMethod
    public void setRepeat(PluginCall call) {
        repeatMode = call.getString("mode", "off");
        call.resolve();
    }

    // Jump to a queue index and play it (used by Next/Previous while awake).
    @PluginMethod
    public void skipTo(PluginCall call) {
        final int index = call.getInt("index", -1);
        getActivity().runOnUiThread(() -> { if (playable(index)) playIndex(index); });
        call.resolve();
    }

    // Persist a local track's bytes to a stable file so MediaPlayer can play it
    // by path (needed for the native auto-advance queue — it can't reach a
    // JavaScript blob). Returns the absolute path. Writes off the UI thread.
    @PluginMethod
    public void persist(PluginCall call) {
        final String data = call.getString("data", "");
        final String name = call.getString("name", "track.dat");
        new Thread(() -> {
            try {
                File dir = new File(getContext().getFilesDir(), "tracks");
                if (!dir.exists()) dir.mkdirs();
                File f = new File(dir, name);
                if (!f.exists() || f.length() == 0) {
                    byte[] bytes = Base64.decode(data, Base64.DEFAULT);
                    FileOutputStream fos = new FileOutputStream(f);
                    fos.write(bytes);
                    fos.close();
                }
                JSObject ret = new JSObject();
                ret.put("path", f.getAbsolutePath());
                call.resolve(ret);
            } catch (Exception e) {
                call.reject("persist failed: " + e.getMessage());
            }
        }).start();
    }

    // Load a source that MediaPlayer can open directly: an http(s) stream
    // (Jellyfin) or a file path. Prepares but does not start until play().
    @PluginMethod
    public void load(PluginCall call) {
        final String url = call.getString("url", "");
        final String fallback = call.getString("fallback", "");
        final boolean autoplay = Boolean.TRUE.equals(call.getBoolean("autoplay", false));
        getActivity().runOnUiThread(() -> setDataSourceAndPrepare(url, autoplay, fallback));
        call.resolve();
    }

    // Load a local track whose bytes come from the WebView (a blob). We write
    // them to the cache dir and play the file, since MediaPlayer can't open a
    // JavaScript blob: URL.
    @PluginMethod
    public void loadData(PluginCall call) {
        final String data = call.getString("data", "");
        final String ext = call.getString("ext", "dat");
        final boolean autoplay = Boolean.TRUE.equals(call.getBoolean("autoplay", false));
        getActivity().runOnUiThread(() -> {
            try {
                // Release any current player first so it isn't holding the temp
                // file we're about to overwrite.
                releasePlayer();
                byte[] bytes = Base64.decode(data, Base64.DEFAULT);
                File f = new File(getContext().getCacheDir(), "np_current." + ext);
                FileOutputStream fos = new FileOutputStream(f);
                fos.write(bytes);
                fos.close();
                setDataSourceAndPrepare(f.getAbsolutePath(), autoplay);
            } catch (Exception e) {
                JSObject d = new JSObject();
                d.put("message", String.valueOf(e.getMessage()));
                emit("error", d);
            }
        });
        call.resolve();
    }

    @PluginMethod
    public void play(PluginCall call) {
        getActivity().runOnUiThread(() -> { playWhenReady = true; internalPlay(); });
        call.resolve();
    }

    @PluginMethod
    public void pause(PluginCall call) {
        getActivity().runOnUiThread(() -> { playWhenReady = false; internalPause(); });
        call.resolve();
    }

    @PluginMethod
    public void seek(PluginCall call) {
        final double seconds = call.getDouble("seconds", 0.0);
        getActivity().runOnUiThread(() -> {
            try { if (player != null && prepared) player.seekTo((int) (seconds * 1000)); } catch (Exception e) { /* ignore */ }
        });
        call.resolve();
    }

    @PluginMethod
    public void stop(PluginCall call) {
        getActivity().runOnUiThread(() -> {
            playWhenReady = false;
            releasePlayer();
            releaseWifiLock();
            abandonFocus();
        });
        call.resolve();
    }

    @PluginMethod
    public void setVolume(PluginCall call) {
        final double v = call.getDouble("value", 1.0);
        volume = (float) Math.max(0.0, Math.min(1.0, v));
        getActivity().runOnUiThread(() -> {
            try { if (player != null) player.setVolume(volume, volume); } catch (Exception e) { /* ignore */ }
        });
        call.resolve();
    }

    @PluginMethod
    public void setRate(PluginCall call) {
        final double rate = call.getDouble("value", 1.0);
        getActivity().runOnUiThread(() -> {
            try {
                if (player != null && prepared && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    boolean wasPlaying = isPlaying();
                    player.setPlaybackParams(player.getPlaybackParams().setSpeed((float) rate));
                    if (!wasPlaying) player.pause();
                }
            } catch (Exception e) { /* ignore */ }
        });
        call.resolve();
    }

    @Override
    protected void handleOnDestroy() {
        super.handleOnDestroy();
        releasePlayer();
        releaseWifiLock();
        abandonFocus();
    }
}
