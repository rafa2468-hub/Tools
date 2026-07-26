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
    private boolean playWhenReady = false;
    private boolean prepared = false;
    private float volume = 1.0f;
    private boolean resumeOnFocusGain = false;

    // The playback queue lives here so tracks auto-advance even while the screen
    // is off (the web layer is suspended then and can't drive the "next track"
    // logic). Sources are http(s) URLs (Jellyfin) or local file paths.
    private final java.util.List<String> queue = new java.util.ArrayList<>();
    private int queueIndex = -1;
    private String repeatMode = "off"; // off | all | one

    private final Handler handler = new Handler(Looper.getMainLooper());
    private Runnable ticker;

    private final AudioManager.OnAudioFocusChangeListener focusListener = focusChange -> {
        switch (focusChange) {
            case AudioManager.AUDIOFOCUS_LOSS:
                resumeOnFocusGain = false;
                internalPause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
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

    private int requestFocus() {
        if (audioManager == null) return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
                focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attrs)
                    .setOnAudioFocusChangeListener(focusListener, handler)
                    .setWillPauseWhenDucked(false)
                    .build();
                return audioManager.requestAudioFocus(focusRequest);
            } else {
                return audioManager.requestAudioFocus(
                    focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
            }
        } catch (Exception e) {
            return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        }
    }

    private void abandonFocus() {
        if (audioManager == null) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (focusRequest != null) audioManager.abandonAudioFocusRequest(focusRequest);
            } else {
                audioManager.abandonAudioFocus(focusListener);
            }
        } catch (Exception e) { /* ignore */ }
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
        releasePlayer();
        // The play intent is decided by the caller (load/loadData) so that a
        // play() arriving during an async local-file load isn't lost.
        playWhenReady = autoplay;
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
            player.setOnErrorListener((mp, what, extra) -> {
                JSObject d = new JSObject();
                d.put("what", what);
                d.put("extra", extra);
                emit("error", d);
                releasePlayer();
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
                emit("pause");
            }
        } catch (Exception e) { /* ignore */ }
    }

    // Called when the current track finishes. Picks the next source from the
    // queue and plays it — all natively, so it works with the screen off. Emits
    // "advanced" (with the new index) so the web layer can catch up its UI when
    // it wakes; at the end of the queue emits "pause" + "ended" instead.
    private void advanceOnCompletion() {
        if ("one".equals(repeatMode) && queueIndex >= 0 && queueIndex < queue.size()) {
            setDataSourceAndPrepare(queue.get(queueIndex), true);
            JSObject d = new JSObject();
            d.put("index", queueIndex);
            emit("advanced", d);
            return;
        }
        int next = queueIndex + 1;
        if (next >= queue.size()) {
            if ("all".equals(repeatMode) && !queue.isEmpty()) {
                next = 0;
            } else {
                emit("pause");
                emit("ended");
                return;
            }
        }
        if (next >= 0 && next < queue.size()) {
            queueIndex = next;
            setDataSourceAndPrepare(queue.get(next), true);
            JSObject d = new JSObject();
            d.put("index", next);
            emit("advanced", d);
        } else {
            emit("pause");
            emit("ended");
        }
    }

    // ---- plugin API ----

    // Replace the playback queue and current index WITHOUT touching the player.
    // The web layer starts the current track itself (via load/loadData); this
    // just tells native what to auto-advance through when a track completes.
    @PluginMethod
    public void setQueue(PluginCall call) {
        com.getcapacitor.JSArray arr = call.getArray("sources");
        final int index = call.getInt("index", -1);
        final String repeat = call.getString("repeat", "off");
        final java.util.List<String> next = new java.util.ArrayList<>();
        if (arr != null) {
            for (int i = 0; i < arr.length(); i++) {
                try { next.add(arr.getString(i)); } catch (Exception e) { next.add(""); }
            }
        }
        getActivity().runOnUiThread(() -> {
            queue.clear();
            queue.addAll(next);
            queueIndex = index;
            repeatMode = repeat;
        });
        call.resolve();
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
        getActivity().runOnUiThread(() -> {
            if (index >= 0 && index < queue.size()) {
                queueIndex = index;
                setDataSourceAndPrepare(queue.get(index), true);
            }
        });
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
        final boolean autoplay = Boolean.TRUE.equals(call.getBoolean("autoplay", false));
        getActivity().runOnUiThread(() -> setDataSourceAndPrepare(url, autoplay));
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
        abandonFocus();
    }
}
