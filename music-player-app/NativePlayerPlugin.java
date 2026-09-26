package com.brokis.musicplayer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.MediaPlayer;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Base64;
import android.view.KeyEvent;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

// Native audio playback for the music player — and, since v1.7.30, the owner of
// everything the car / lock screen talks to.
//
// Audio plays through Android's MediaPlayer (the WebView's <audio> element would
// not route to USB-C / Bluetooth / the car). The web layer drives it through a
// thin adapter that mimics the <audio> element; events mirror the media element:
// loadstart, loadedmetadata, play, playing, pause, timeupdate, ended, error.
//
// With the screen off the web layer is frozen, so anything that must keep
// working in the car lives here, not in JavaScript:
//  - the queue and the current position in it (tracks advance natively);
//  - the media session and its notification: car / Bluetooth / lock-screen
//    buttons land directly in this class, and the play state, title and length
//    the car shows are published from the player itself, never from JS;
//  - pausing when the audio output disconnects (car turned off);
//  - recovering a Jellyfin stream that drops, at the same position;
//  - downloading the next few Jellyfin tracks to disk ahead of playback.
//
// Every JS→native call and every native callback runs on the main thread, so
// the state below needs no locking (downloads run on their own thread and hand
// results back through the main-thread handler).
@CapacitorPlugin(name = "NativePlayer")
public class NativePlayerPlugin extends Plugin {

    // The live plugin, so PlaybackService (notification buttons, swipe-away)
    // can reach it.
    static NativePlayerPlugin instance;

    private MediaPlayer player;
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest; // API 26+
    private boolean hasFocus = false;
    // A call (or another app) has the audio for now and Android hasn't handed
    // it back yet. hasFocus stays true through this, so without the flag a play
    // press during a call started the music over the call.
    private boolean focusSuspended = false;
    // Polls for the end of a call while a play press is being held for it.
    private Runnable callWatch;
    private long callWatchSince = 0;
    private static final long CALL_WATCH_MS = 2000;
    // A held play press is dropped after this long, rather than starting the
    // music hours later.
    private static final long CALL_WATCH_MAX_MS = 2 * 60 * 60 * 1000L;
    // What the listener wants: true = playing, or trying to (loading, or
    // recovering a dropped stream). Every play/pause path sets it; recovery and
    // the car's play/pause toggle read it.
    private boolean playWhenReady = false;
    private boolean prepared = false;
    private float volume = 1.0f;
    private boolean resumeOnFocusGain = false;
    private android.net.wifi.WifiManager.WifiLock wifiLock;
    private String currentSource = "";
    // The other sources for the current track, best first (cached file →
    // original file streamed → server transcode). When one can't play, the
    // next is tried at the same position — natively, so it works screen-off.
    private final List<String> alternates = new ArrayList<>();
    // Counters surfaced to the UI so playback problems can be identified on the
    // device without a debugger attached.
    private int bufferingCount = 0, focusLossCount = 0, errorCount = 0, downloadCount = 0;
    private int truncatedCount = 0, dlFailCount = 0, shortFileCount = 0;
    private int noisyCount = 0, retryCount = 0;
    private String lastDlError = "";
    // Progress tracking, used to tell a real end-of-track from a stream that
    // simply stopped early (which MediaPlayer reports identically), and to
    // resume at the right spot.
    private int lastPositionMs = 0, durationMs = 0, pendingSeekMs = 0, lastResumeAtMs = 0;
    // The player was torn down while paused (a stream dropped, or the track
    // gave up); play() rebuilds it at lastPositionMs.
    private boolean needsReload = false;

    // The playback queue. Entries are identified by the web layer's track id,
    // never just by position, so a queue re-sent from JS (shuffle, library
    // refresh) can't make native and JS disagree about what's playing.
    // A local track plays from `sources` (a file the web layer wrote to disk);
    // a Jellyfin track has an itemId, and its URLs are built from the templates.
    private final List<String> ids = new ArrayList<>();
    private final List<String> sources = new ArrayList<>();
    private final List<String> itemIds = new ArrayList<>();
    private final List<String> containers = new ArrayList<>();
    private final List<String> titles = new ArrayList<>();
    private final List<String> artists = new ArrayList<>();
    private final List<String> albums = new ArrayList<>();
    // Per-track duration from the server, for when the player can't report one
    // (a transcode arrives without a length) and to spot a truncated track.
    private final List<Integer> durationsSec = new ArrayList<>();
    // Disc * 1000 + track number from the server (0 when unknown), so the fade
    // can tell when the next song simply continues the same album.
    private final List<Integer> trackNos = new ArrayList<>();
    // Jellyfin URL templates with an "{id}" placeholder for the item id.
    private String directTemplate = "", transcodeTemplate = "";
    private int queueIndex = -1;
    private String currentId = "";
    private String repeatMode = "off"; // off | all | one
    // Consecutive failures while auto-advancing; caps how far we'll skip through
    // a broken queue (e.g. server unreachable) before giving up.
    private int errorStreak = 0;
    private static final int MAX_ERROR_SKIPS = 3;
    // Bumped whenever a different track starts, so a delayed skip scheduled for
    // an earlier track can tell it has been overtaken.
    private int trackGen = 0;

    // A dropped stream is retried at the same position with growing gaps
    // (about a minute in total) — long enough to ride out a dead zone or the
    // Wi-Fi → mobile handover, instead of skipping the song.
    private static final long[] RETRY_DELAYS_MS = {1000, 2000, 4000, 8000, 15000, 30000};
    private int retryAttempt = 0;
    private Runnable retryRunnable;

    // Media session: what the car, Bluetooth buttons and lock screen talk to.
    private MediaSession session;
    private boolean noisyRegistered = false;
    // After this long paused, drop the notification and foreground service.
    private Runnable idleStop;
    private static final long IDLE_STOP_MS = 10 * 60 * 1000L;
    private static final String CHANNEL_ID = "playback";
    private static final long SESSION_ACTIONS = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
        | PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_SKIP_TO_NEXT
        | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_STOP;

    // Jellyfin download cache. Tracks play from disk when they've been fetched,
    // and stream (then finish from disk if the stream drops) when they haven't.
    private static final long MAX_CACHE_BYTES = 1536L * 1024L * 1024L; // ~1.5 GB
    // How many tracks after the current one to keep downloaded.
    private static final int PREFETCH_AHEAD = 5;
    private final ExecutorService downloader = Executors.newSingleThreadExecutor();
    private final Set<String> inFlight = Collections.synchronizedSet(new HashSet<>());
    // Cache files for the current track and the ones just ahead. Never trimmed,
    // and a queued download whose track has left this window is skipped.
    private volatile Set<String> protectedNames = new HashSet<>();

    private final Handler handler = new Handler(Looper.getMainLooper());
    private Runnable ticker;

    // Fade-out over the last few seconds of each track. Many downloaded songs
    // are cut at the source and stop dead at full volume; a short fade makes
    // that sound deliberate, and a natural ending is already quiet there. Not
    // applied when the next song continues the same album, so albums that run
    // track into track stay seamless.
    private static final int FADE_MS = 3000;
    private static final long FADE_STEP_MS = 50;
    // Tracks shorter than this (jingles, intros) are left alone.
    private static final int FADE_MIN_TRACK_MS = 15000;
    private Runnable fader;
    private boolean fadeLogged = false;

    // Playback log: a timestamped line for everything that starts, stops or
    // changes playback, and what caused it (app, car, notification, unplug,
    // call, error, app restart). The counters say how often; this says what
    // and when. Kept in a file so it survives the app being killed, and copied
    // out from Settings.
    private static final int LOG_KEEP = 1500;
    private final ArrayDeque<String> logLines = new ArrayDeque<>();
    private final ExecutorService logWriter = Executors.newSingleThreadExecutor();
    private int logAppends = 0;
    private final SimpleDateFormat logTime = new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US);

    private final AudioManager.OnAudioFocusChangeListener focusListener = focusChange -> {
        switch (focusChange) {
            case AudioManager.AUDIOFOCUS_LOSS:
                // Another app took the audio for good; give it up so the next
                // play() asks for it again instead of playing over them.
                focusLossCount++; emitDiag();
                plog("focus lost for good (another app took the audio)" + modeNote());
                hasFocus = false;
                focusSuspended = false;
                userPause("focus lost");
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                // A call or a spoken prompt. Pause, and resume after it if we
                // were playing (or a play press is already waiting for it).
                focusLossCount++; emitDiag();
                focusSuspended = true;
                resumeOnFocusGain = resumeOnFocusGain || playWhenReady;
                plog("focus lost for now (call or prompt)" + modeNote()
                    + (resumeOnFocusGain ? ", will resume after it" : ""));
                if (playWhenReady) {
                    playWhenReady = false;
                    if (retryRunnable != null) { cancelRetry(); needsReload = true; }
                    internalPause();
                    publishState();
                }
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                // Keep playing at full volume. Self-ducking here could leave the
                // output stuck quiet if the matching GAIN never arrived, and it
                // is not needed for a music player.
                plog("focus: duck request ignored, kept playing");
                break;
            case AudioManager.AUDIOFOCUS_GAIN:
                // Also how a request made during a call is granted, once the
                // call ends.
                hasFocus = true;
                focusSuspended = false;
                plog("focus back" + (resumeOnFocusGain ? ", resuming" : ""));
                if (resumeOnFocusGain) { resumeOnFocusGain = false; userPlay("focus back"); }
                break;
            default:
                break;
        }
    };

    // The audio output is about to switch to the phone speaker: the car was
    // turned off, Bluetooth dropped, or headphones were pulled. Every media app
    // pauses here; without it the music carried on out of the phone.
    private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null || !AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(intent.getAction())) return;
            handler.post(() -> {
                noisyCount++;
                emitDiag();
                // Also cancels a pending resume after a call — otherwise the
                // call ending would restart the music on the speaker.
                userPause("unplug: audio output disconnected");
            });
        }
    };

    @Override
    public void load() {
        instance = this;
        loadLog();
        plog("--- app process started ---");
        audioManager = (AudioManager) getContext().getSystemService(Context.AUDIO_SERVICE);
        try {
            session = new MediaSession(getContext(), "MusicPlayer");
            session.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
            session.setCallback(sessionCallback, handler);
            PendingIntent open = openAppIntent();
            if (open != null) session.setSessionActivity(open);
        } catch (Throwable t) {
            session = null;
        }
    }

    // ---- media session ----

    private final MediaSession.Callback sessionCallback = new MediaSession.Callback() {
        @Override
        public boolean onMediaButtonEvent(Intent mediaButtonIntent) {
            // Toggle play/pause on what the listener wants, not on the state the
            // car last saw — a stale "playing" state is what made the car's
            // play button do nothing.
            KeyEvent ev = mediaButtonIntent == null ? null
                : (KeyEvent) mediaButtonIntent.getParcelableExtra(Intent.EXTRA_KEY_EVENT);
            if (ev != null && ev.getAction() == KeyEvent.ACTION_DOWN && ev.getRepeatCount() == 0) {
                plog("button from car/Bluetooth: " + KeyEvent.keyCodeToString(ev.getKeyCode()));
            }
            if (ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE) {
                if (ev.getAction() == KeyEvent.ACTION_DOWN && ev.getRepeatCount() == 0) {
                    if (playWhenReady) userPause(FROM_SESSION); else userPlay(FROM_SESSION);
                }
                return true;
            }
            return super.onMediaButtonEvent(mediaButtonIntent);
        }
        @Override public void onPlay() { userPlay(FROM_SESSION); }
        @Override public void onPause() { userPause(FROM_SESSION); }
        @Override public void onSkipToNext() { skipNext(FROM_SESSION); }
        @Override public void onSkipToPrevious() { skipPrevious(FROM_SESSION); }
        @Override public void onSeekTo(long pos) { seekToMs((int) pos, FROM_SESSION); }
        @Override public void onStop() { userPause(FROM_SESSION + " stop"); stopService(); }
    };

    // Where a command came from, for the playback log. Car and Bluetooth
    // buttons, the lock screen and the media controls Android draws in the
    // notification shade all reach the session the same way, so they can't be
    // told apart. The notification's own buttons come through PlaybackService.
    private static final String FROM_APP = "app";
    private static final String FROM_SESSION = "car/lock screen";
    private static final String FROM_NOTIFICATION = "notification";

    private void activateSession() {
        try { if (session != null && !session.isActive()) session.setActive(true); } catch (Exception e) { /* ignore */ }
        registerNoisy();
    }

    private void registerNoisy() {
        if (noisyRegistered) return;
        try {
            IntentFilter f = new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
            if (Build.VERSION.SDK_INT >= 33) {
                getContext().registerReceiver(noisyReceiver, f, Context.RECEIVER_EXPORTED);
            } else {
                getContext().registerReceiver(noisyReceiver, f);
            }
            noisyRegistered = true;
        } catch (Exception e) { /* ignore */ }
    }

    private void unregisterNoisy() {
        if (!noisyRegistered) return;
        try { getContext().unregisterReceiver(noisyReceiver); } catch (Exception e) { /* ignore */ }
        noisyRegistered = false;
    }

    private int currentPositionMs() {
        try { if (player != null && prepared) return player.getCurrentPosition(); } catch (Exception e) { /* ignore */ }
        return lastPositionMs;
    }

    // Tell the car / lock screen the real state, straight from the player.
    private void publishState() {
        if (session != null) {
            int st;
            if (retryRunnable != null) st = PlaybackState.STATE_BUFFERING;
            else if (isPlaying()) st = PlaybackState.STATE_PLAYING;
            else if (playWhenReady && player != null && !prepared) st = PlaybackState.STATE_BUFFERING;
            else if (queueIndex >= 0 || player != null) st = PlaybackState.STATE_PAUSED;
            else st = PlaybackState.STATE_STOPPED;
            try {
                session.setPlaybackState(new PlaybackState.Builder()
                    .setActions(SESSION_ACTIONS)
                    .setState(st, currentPositionMs(), 1.0f, SystemClock.elapsedRealtime())
                    .build());
            } catch (Exception e) { /* ignore */ }
        }
        updateNotification();
    }

    // Title / artist / album / length of the loaded track, for the car and the
    // lock screen. Skipped when the queue doesn't describe the loaded track (it
    // was re-sent without it), so the display never names a different song.
    private void publishMetadata() {
        if (session == null) return;
        if (queueIndex < 0 || queueIndex >= ids.size() || !currentId.equals(ids.get(queueIndex))) return;
        long dur = durationsSec.get(queueIndex) * 1000L;
        if (dur <= 0 && durationMs > 0) dur = durationMs;
        try {
            session.setMetadata(new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_MEDIA_ID, currentId)
                .putString(MediaMetadata.METADATA_KEY_TITLE, at(titles, queueIndex))
                .putString(MediaMetadata.METADATA_KEY_ARTIST, at(artists, queueIndex))
                .putString(MediaMetadata.METADATA_KEY_ALBUM, at(albums, queueIndex))
                .putLong(MediaMetadata.METADATA_KEY_DURATION, Math.max(0, dur))
                .build());
        } catch (Exception e) { /* ignore */ }
        updateNotification();
    }

    // ---- notification + foreground service ----

    private PendingIntent openAppIntent() {
        try {
            Context ctx = getContext();
            Intent launch = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
            if (launch == null) return null;
            return PendingIntent.getActivity(ctx, 0, launch, pendingFlags());
        } catch (Exception e) {
            return null;
        }
    }

    private static int pendingFlags() {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) flags |= PendingIntent.FLAG_IMMUTABLE;
        return flags;
    }

    private PendingIntent serviceIntent(String action) {
        Context ctx = getContext();
        Intent i = new Intent(ctx, PlaybackService.class).setAction(action);
        return PendingIntent.getService(ctx, action.hashCode(), i, pendingFlags());
    }

    Notification buildNotification() {
        Context ctx = getContext();
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
                nm.createNotificationChannel(new NotificationChannel(CHANNEL_ID, "Playback", NotificationManager.IMPORTANCE_LOW));
            }
        }
        Notification.Builder b = (Build.VERSION.SDK_INT >= 26)
            ? new Notification.Builder(ctx, CHANNEL_ID)
            : new Notification.Builder(ctx);
        boolean known = queueIndex >= 0 && queueIndex < ids.size() && currentId.equals(ids.get(queueIndex));
        String title = known ? at(titles, queueIndex) : "";
        b.setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(title.length() > 0 ? title : "Music Player")
            .setContentText(known ? at(artists, queueIndex) : "")
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setShowWhen(false)
            .setOnlyAlertOnce(true)
            .setOngoing(playWhenReady);
        PendingIntent open = openAppIntent();
        if (open != null) b.setContentIntent(open);
        b.addAction(new Notification.Action.Builder(android.R.drawable.ic_media_previous, "Previous",
            serviceIntent(PlaybackService.ACTION_PREV)).build());
        if (playWhenReady) {
            b.addAction(new Notification.Action.Builder(android.R.drawable.ic_media_pause, "Pause",
                serviceIntent(PlaybackService.ACTION_PAUSE)).build());
        } else {
            b.addAction(new Notification.Action.Builder(android.R.drawable.ic_media_play, "Play",
                serviceIntent(PlaybackService.ACTION_PLAY)).build());
        }
        b.addAction(new Notification.Action.Builder(android.R.drawable.ic_media_next, "Next",
            serviceIntent(PlaybackService.ACTION_NEXT)).build());
        Notification.MediaStyle style = new Notification.MediaStyle().setShowActionsInCompactView(0, 1, 2);
        if (session != null) style.setMediaSession(session.getSessionToken());
        b.setStyle(style);
        return b.build();
    }

    private void updateNotification() {
        PlaybackService svc = PlaybackService.instance;
        if (svc == null) return;
        try { svc.update(buildNotification()); } catch (Exception e) { /* ignore */ }
    }

    // Keep the process alive as a media app while there's something to play.
    // Starting the service is only allowed while the app is in the foreground;
    // it then stays foreground through pauses (so the car can resume it from the
    // background) until it has been idle for IDLE_STOP_MS. A refused start is
    // caught — playback carries on without it rather than crashing, which is how
    // the June 2026 media-session builds died.
    private void ensureForeground() {
        cancelIdleStop();
        PlaybackService svc = PlaybackService.instance;
        if (svc != null) {
            try { svc.promote(buildNotification()); } catch (Throwable t) { /* ignore */ }
            return;
        }
        try {
            Intent i = new Intent(getContext(), PlaybackService.class).setAction(PlaybackService.ACTION_START);
            getContext().startService(i);
        } catch (Throwable t) {
            // Background start refused: play on without it.
            plog("foreground service start refused: " + t.getClass().getSimpleName());
        }
    }

    private void stopService() {
        cancelIdleStop();
        PlaybackService svc = PlaybackService.instance;
        if (svc != null) svc.shutdown();
    }

    private void scheduleIdleStop() {
        cancelIdleStop();
        // Paused for a call: the service must still be there when it ends.
        if (resumeOnFocusGain) return;
        idleStop = () -> {
            idleStop = null;
            if (!playWhenReady && !isPlaying()) {
                plog("paused 10 min: notification removed, service stopped");
                stopService();
            }
        };
        handler.postDelayed(idleStop, IDLE_STOP_MS);
    }

    private void cancelIdleStop() {
        if (idleStop != null) { handler.removeCallbacks(idleStop); idleStop = null; }
    }

    // Notification buttons, relayed by PlaybackService.
    void handleServiceAction(String action) {
        if (PlaybackService.ACTION_PLAY.equals(action)) userPlay(FROM_NOTIFICATION);
        else if (PlaybackService.ACTION_PAUSE.equals(action)) userPause(FROM_NOTIFICATION);
        else if (PlaybackService.ACTION_NEXT.equals(action)) skipNext(FROM_NOTIFICATION);
        else if (PlaybackService.ACTION_PREV.equals(action)) skipPrevious(FROM_NOTIFICATION);
        else if (PlaybackService.ACTION_STOP.equals(action)) { userPause(FROM_NOTIFICATION + " stop"); stopService(); }
    }

    // The app was swiped away from recents. Keep playing if it's playing;
    // otherwise clear the notification.
    void onTaskRemoved() {
        plog("app swiped away from recents" + (playWhenReady || isPlaying() ? ", still playing" : ""));
        if (!playWhenReady && !isPlaying()) stopService();
    }

    // ---- helpers ----

    private void emit(String event) {
        notifyListeners(event, new JSObject());
    }

    private void emit(String event, JSObject data) {
        notifyListeners(event, data);
    }

    private JSObject diagObject() {
        JSObject d = new JSObject();
        d.put("buffering", bufferingCount);
        d.put("focusLoss", focusLossCount);
        d.put("errors", errorCount);
        d.put("downloads", downloadCount);
        d.put("dlFails", dlFailCount);
        d.put("lastDlError", lastDlError);
        d.put("truncated", truncatedCount);
        d.put("shortFiles", shortFileCount);
        d.put("noisy", noisyCount);
        d.put("retries", retryCount);
        // Whether the current track is playing from disk or off the network —
        // the quickest way to tell whether caching is actually working.
        d.put("mode", currentSource.startsWith("http") ? "stream" : "file");
        return d;
    }

    private void emitDiag() {
        emit("diag", diagObject());
    }

    // ---- playback log ----

    private File logFile() {
        return new File(getContext().getFilesDir(), "playback-log.txt");
    }

    // Pick up the log from before this process started (it's how an app
    // restart shows up), trimmed to the newest LOG_KEEP lines.
    private void loadLog() {
        synchronized (logLines) {
            try (BufferedReader r = new BufferedReader(new FileReader(logFile()))) {
                String line;
                while ((line = r.readLine()) != null) {
                    logLines.addLast(line);
                    if (logLines.size() > LOG_KEEP) logLines.removeFirst();
                }
            } catch (Exception e) { /* no log yet */ }
        }
        rewriteLog();
    }

    private void rewriteLog() {
        final List<String> snapshot;
        synchronized (logLines) {
            snapshot = new ArrayList<>(logLines);
            logAppends = 0;
        }
        final File f = logFile();
        try {
            logWriter.execute(() -> {
                try (FileOutputStream out = new FileOutputStream(f, false)) {
                    StringBuilder sb = new StringBuilder();
                    for (String l : snapshot) sb.append(l).append('\n');
                    out.write(sb.toString().getBytes(StandardCharsets.UTF_8));
                } catch (Exception e) { /* ignore */ }
            });
        } catch (Exception e) { /* writer shut down */ }
    }

    // Add one line. The file is appended to (off the main thread) and rewritten
    // from the newest lines once it has doubled, so it stays small.
    void plog(String msg) {
        final String line = logTime.format(new Date()) + "  " + msg;
        boolean trim;
        synchronized (logLines) {
            logLines.addLast(line);
            while (logLines.size() > LOG_KEEP) logLines.removeFirst();
            trim = ++logAppends >= LOG_KEEP;
        }
        if (trim) { rewriteLog(); return; }
        final File f = logFile();
        try {
            logWriter.execute(() -> {
                try (FileOutputStream out = new FileOutputStream(f, true)) {
                    out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
                } catch (Exception e) { /* ignore */ }
            });
        } catch (Exception e) { /* writer shut down */ }
    }

    private static String fmt(int ms) {
        int s = Math.max(0, ms) / 1000;
        return (s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60);
    }

    // Queue entry i as the log names it: position and title.
    private String trackLabel(int i) {
        if (i < 0 || i >= ids.size()) return "(no track)";
        String t = at(titles, i);
        if (t.length() > 40) t = t.substring(0, 40) + "...";
        return "#" + i + " \"" + t + "\"";
    }

    // What kind of source a path is: a downloaded Jellyfin file, one of the
    // listener's own files, the original file streamed, or a transcode.
    private String srcKind(String path) {
        if (path == null || path.length() == 0) return "nothing";
        if (!path.startsWith("http")) return isStreamCacheFile(path) ? "downloaded file" : "local file";
        String trans = jellyfinUrl(transcodeTemplate, queueIndex);
        return (trans.length() > 0 && trans.equals(path)) ? "transcode" : "stream";
    }

    private String modeNote() {
        int m = AudioManager.MODE_NORMAL;
        try { if (audioManager != null) m = audioManager.getMode(); } catch (Exception e) { /* ignore */ }
        if (m == AudioManager.MODE_NORMAL) return "";
        if (m == AudioManager.MODE_RINGTONE) return " [phone ringing]";
        if (m == AudioManager.MODE_IN_CALL) return " [phone call]";
        if (m == AudioManager.MODE_IN_COMMUNICATION) return " [voice/video call]";
        return " [phone mode " + m + "]";
    }

    // ---- calls ----

    // A phone or voice/video call is ringing or in progress (WhatsApp-style
    // calls set MODE_IN_COMMUNICATION).
    private boolean inCall() {
        try {
            if (audioManager == null) return false;
            int m = audioManager.getMode();
            return m == AudioManager.MODE_RINGTONE || m == AudioManager.MODE_IN_CALL
                || m == AudioManager.MODE_IN_COMMUNICATION || m == AudioManager.MODE_CALL_SCREENING
                || m == AudioManager.MODE_CALL_REDIRECT || m == AudioManager.MODE_COMMUNICATION_REDIRECT;
        } catch (Exception e) {
            return false;
        }
    }

    // Decide whether a play request may start the music now. A car,
    // Bluetooth, lock-screen or notification press during a call is held and
    // starts the music when the call ends. A tap in the app itself always goes
    // ahead (it's deliberate, and covers a phone that leaves its call mode
    // stuck); Android's own focus answer still keeps it off a real phone call.
    private boolean gatePlay(String from) {
        if (!FROM_APP.equals(from) && inCall()) {
            holdForCall(from);
            return false;
        }
        if (focusSuspended) {
            // Paused for something that never handed the audio back, and the
            // listener has pressed play: ask for it again.
            focusSuspended = false;
            hasFocus = false;
        }
        return true;
    }

    // Remember a play press until the call ends. Android's focus GAIN normally
    // resumes it; the watch covers a call that never took focus from us.
    private void holdForCall(String why) {
        playWhenReady = false;
        resumeOnFocusGain = true;
        cancelIdleStop();
        plog("play (" + why + ") held: call in progress" + modeNote() + ", starts when it ends");
        startCallWatch();
        publishState();
    }

    private void startCallWatch() {
        stopCallWatch();
        callWatchSince = SystemClock.elapsedRealtime();
        callWatch = new Runnable() {
            @Override
            public void run() {
                if (!resumeOnFocusGain) { callWatch = null; return; }
                if (SystemClock.elapsedRealtime() - callWatchSince > CALL_WATCH_MAX_MS) {
                    callWatch = null;
                    resumeOnFocusGain = false;
                    plog("held play dropped: still in a call after 2 hours");
                    publishState();
                    scheduleIdleStop();
                    return;
                }
                if (!inCall() && !focusSuspended) {
                    callWatch = null;
                    resumeOnFocusGain = false;
                    userPlay("call ended");
                    return;
                }
                handler.postDelayed(this, CALL_WATCH_MS);
            }
        };
        handler.postDelayed(callWatch, CALL_WATCH_MS);
    }

    private void stopCallWatch() {
        if (callWatch != null) { handler.removeCallbacks(callWatch); callWatch = null; }
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
                        lastPositionMs = player.getCurrentPosition();
                        JSObject d = new JSObject();
                        d.put("position", lastPositionMs / 1000.0);
                        int dur = player.getDuration();
                        d.put("duration", dur > 0 ? dur / 1000.0 : 0);
                        emit("timeupdate", d);
                        maybeStartFade();
                        // Ten seconds of steady playback since the last recovery:
                        // the connection is healthy again, so a later drop gets
                        // the full set of retries.
                        if (retryAttempt > 0 && isPlaying() && lastPositionMs > lastResumeAtMs + 10000) retryAttempt = 0;
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

    // ---- fade-out ----

    // Volume factor with leftMs of the track remaining: 1 until the fade
    // starts, then a squared ramp to 0 (a linear ramp sounds like it drops
    // late and suddenly).
    static float fadeGain(int leftMs, int fadeMs) {
        if (leftMs >= fadeMs) return 1f;
        if (leftMs <= 0) return 0f;
        float g = leftMs / (float) fadeMs;
        return g * g;
    }

    // Whether track `next` follows track `cur` on the same album: the next
    // track number, or track 1 of the next disc. Numbers are disc * 1000 +
    // track; 0 means unknown, which never counts as continuing.
    static boolean continuesAlbum(String albumCur, String albumNext, int cur, int next) {
        if (albumCur == null || albumCur.length() == 0 || !albumCur.equals(albumNext)) return false;
        if (cur <= 0 || next <= 0) return false;
        if (next == cur + 1) return true;
        return next / 1000 == cur / 1000 + 1 && next % 1000 == 1;
    }

    private int trackNoAt(int i) {
        return (i >= 0 && i < trackNos.size()) ? trackNos.get(i) : 0;
    }

    private boolean nextContinuesAlbum() {
        int i = queueIndex;
        if (i < 0 || i + 1 >= ids.size() || !currentId.equals(ids.get(i))) return false;
        return continuesAlbum(at(albums, i), at(albums, i + 1), trackNoAt(i), trackNoAt(i + 1));
    }

    // Length the fade counts down to: the file's own length, else the server's.
    private int fadeTrackMs() {
        if (durationMs > 0) return durationMs;
        if (queueIndex >= 0 && queueIndex < durationsSec.size() && currentId.equals(at(ids, queueIndex))) {
            return durationsSec.get(queueIndex) * 1000;
        }
        return 0;
    }

    // Called from the ticker (every 500 ms). Hands over to a finer 50 ms loop
    // for the last stretch of the track.
    private void maybeStartFade() {
        if (fader != null || player == null || !prepared || !isPlaying()) return;
        final int total = fadeTrackMs();
        if (total < FADE_MIN_TRACK_MS) return;
        if (total - lastPositionMs > FADE_MS + 600) return;
        if (nextContinuesAlbum()) {
            if (!fadeLogged) { fadeLogged = true; plog("  no fade: the next song continues the album"); }
            return;
        }
        if (!fadeLogged) { fadeLogged = true; plog("  fading out the last " + (FADE_MS / 1000) + "s"); }
        fader = new Runnable() {
            @Override
            public void run() {
                if (player == null || !prepared || !isPlaying()) { fader = null; return; }
                try {
                    int left = total - player.getCurrentPosition();
                    if (left > FADE_MS + 1000) {
                        // Sought back out of the fade: full volume, and let the
                        // ticker start it again near the end.
                        player.setVolume(volume, volume);
                        fader = null;
                        return;
                    }
                    float g = volume * fadeGain(left, FADE_MS);
                    player.setVolume(g, g);
                } catch (Exception e) { fader = null; return; }
                handler.postDelayed(this, FADE_STEP_MS);
            }
        };
        handler.post(fader);
    }

    private void stopFade() {
        if (fader != null) { handler.removeCallbacks(fader); fader = null; }
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
                    // Delayed gain: a request made during a call is answered
                    // "later" and granted (focus GAIN) when the call ends,
                    // instead of being refused outright.
                    focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(attrs)
                        .setOnAudioFocusChangeListener(focusListener, handler)
                        .setWillPauseWhenDucked(false)
                        .setAcceptsDelayedFocusGain(true)
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

    // Hold a Wi-Fi lock while playing. With the screen off Android puts Wi-Fi
    // into power save, which stalls a Jellyfin stream mid-track and slows the
    // downloads running ahead of playback.
    private void acquireWifiLock() {
        try {
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

    // Release the current player without emitting playback events.
    private void releasePlayer() {
        stopTicker();
        stopFade();
        prepared = false;
        if (player != null) {
            try { player.reset(); } catch (Exception e) { /* ignore */ }
            try { player.release(); } catch (Exception e) { /* ignore */ }
            player = null;
        }
    }

    private void cancelRetry() {
        if (retryRunnable != null) { handler.removeCallbacks(retryRunnable); retryRunnable = null; }
    }

    // Open one source and (optionally) start it at seekMs.
    private void prepareSource(String path, int seekMs, boolean autoplay) {
        releasePlayer();
        needsReload = false;
        fadeLogged = false;
        currentSource = (path == null) ? "" : path;
        pendingSeekMs = Math.max(0, seekMs);
        lastResumeAtMs = pendingSeekMs;
        lastPositionMs = pendingSeekMs;
        durationMs = 0;
        // The play intent is decided by the caller so that a play() arriving
        // during an async load isn't lost.
        playWhenReady = autoplay;
        emit("loadstart");
        plog("  open " + srcKind(currentSource) + (pendingSeekMs > 0 ? " at " + fmt(pendingSeekMs) : ""));
        if (currentSource.length() == 0) { onSourceFailed(lastPositionMs); return; }
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
            player.setDataSource(currentSource);
            player.setOnPreparedListener(mp -> {
                prepared = true;
                JSObject d = new JSObject();
                int dur = 0;
                try { dur = mp.getDuration(); } catch (Exception e) { /* ignore */ }
                durationMs = dur;
                d.put("duration", dur > 0 ? dur / 1000.0 : 0);
                emit("loadedmetadata", d);
                // A file whose own length differs from the server's by more
                // than the 5 s tolerance gets judged "cut short" when it ends.
                if (queueIndex >= 0 && queueIndex < durationsSec.size() && currentId.equals(at(ids, queueIndex))) {
                    int server = durationsSec.get(queueIndex) * 1000;
                    if (server > 0 && dur > 0 && Math.abs(server - dur) > 5000) {
                        plog("  note: " + srcKind(currentSource) + " is " + fmt(dur) + " long, server says " + fmt(server));
                    }
                }
                // Resuming (a dropped stream, a switch to another source): pick
                // up where it stopped.
                if (pendingSeekMs > 0) {
                    try { mp.seekTo(pendingSeekMs); } catch (Exception e) { /* ignore */ }
                    lastPositionMs = pendingSeekMs;
                    pendingSeekMs = 0;
                }
                publishMetadata();
                if (playWhenReady) internalPlay(); else publishState();
            });
            player.setOnCompletionListener(mp -> {
                stopTicker();
                handleCompletion();
            });
            // A stalled network read surfaces here, not as an error. Counting it
            // distinguishes "the stream ran dry" from a real pause.
            player.setOnInfoListener((mp, what, extra) -> {
                if (what == MediaPlayer.MEDIA_INFO_BUFFERING_START) {
                    bufferingCount++;
                    emitDiag();
                }
                return false;
            });
            player.setOnErrorListener((mp, what, extra) -> {
                onPlayerError(what, extra);
                return true;
            });
            player.prepareAsync();
            publishState();
        } catch (Exception e) {
            // setDataSource throws for a missing / unreadable file.
            releasePlayer();
            onSourceFailed(lastPositionMs);
        }
    }

    private void onPlayerError(int what, int extra) {
        errorCount++;
        emitDiag();
        int pos = lastPositionMs;
        boolean fromNetwork = currentSource.startsWith("http");
        boolean undecodable = extra == MediaPlayer.MEDIA_ERROR_UNSUPPORTED || extra == MediaPlayer.MEDIA_ERROR_MALFORMED;
        plog("error " + what + "/" + extra + " on " + srcKind(currentSource) + " at " + fmt(pos) + " " + trackLabel(queueIndex));
        releasePlayer();
        if (fromNetwork && !undecodable) onNetworkDrop(pos);
        else onSourceFailed(pos);
    }

    // The current source can't be played at all (a file that won't open or
    // decode, a format the phone can't handle). Move to the next source for the
    // same track, at the same position.
    private void onSourceFailed(int pos) {
        // A cached copy that won't play is useless: delete it so it's fetched again.
        if (isStreamCacheFile(currentSource)) deleteCacheFile(currentSource);
        if (!alternates.isEmpty()) {
            String next = alternates.remove(0);
            plog("  can't play " + srcKind(currentSource) + ", trying " + srcKind(next));
            retryAttempt = 0;
            prepareSource(next, pos, playWhenReady);
            return;
        }
        giveUpOnTrack();
    }

    // The network dropped under a stream. Retry the same track at the same
    // position with backoff, switching to the downloaded copy if it has
    // finished meanwhile. Only after that fails does it try the next source.
    private void onNetworkDrop(final int pos) {
        lastPositionMs = pos;
        if (!playWhenReady) {
            // Nobody is listening right now; rebuild on the next play().
            plog("  stream dropped while paused at " + fmt(pos) + ", reloads on play");
            needsReload = true;
            publishState();
            return;
        }
        if (retryAttempt < RETRY_DELAYS_MS.length) {
            long delay = RETRY_DELAYS_MS[retryAttempt++];
            retryCount++;
            emitDiag();
            plog("  stream dropped at " + fmt(pos) + ", retry " + retryAttempt + "/" + RETRY_DELAYS_MS.length + " in " + (delay / 1000) + "s");
            final String src = currentSource;
            retryRunnable = () -> {
                retryRunnable = null;
                if (!playWhenReady) { needsReload = true; publishState(); return; }
                File f = (queueIndex >= 0 && queueIndex < ids.size() && currentId.equals(ids.get(queueIndex)))
                    ? cachedFile(queueIndex) : null;
                if (f != null && !f.getAbsolutePath().equals(src)) {
                    if (!alternates.contains(src)) alternates.add(0, src);
                    prepareSource(f.getAbsolutePath(), pos, true);
                } else {
                    prepareSource(src, pos, true);
                }
            };
            handler.postDelayed(retryRunnable, delay);
            publishState();
            return;
        }
        plog("  stream still down after " + RETRY_DELAYS_MS.length + " retries");
        onSourceFailed(pos);
    }

    // Every source for this track has failed. Tell the web layer, and move on
    // (capped, so an unreachable server can't make it skip forever).
    private void giveUpOnTrack() {
        boolean skipping = playWhenReady && !ids.isEmpty() && errorStreak < MAX_ERROR_SKIPS;
        plog("gave up on " + trackLabel(queueIndex) + (skipping ? ", skipping to the next" : ", stopped"));
        JSObject d = new JSObject();
        d.put("index", queueIndex);
        d.put("skipping", skipping);
        emit("error", d);
        if (skipping) {
            errorStreak++;
            final int gen = trackGen;
            handler.postDelayed(() -> { if (gen == trackGen) advanceOnCompletion(true); }, 500);
            return;
        }
        playWhenReady = false;
        needsReload = true;
        emit("pause");
        publishState();
        scheduleIdleStop();
    }

    private void internalPlay() {
        if (player == null || !prepared) { playWhenReady = true; publishState(); return; }
        int focus = requestFocus();
        if (focus == AudioManager.AUDIOFOCUS_REQUEST_DELAYED
                || (focus != AudioManager.AUDIOFOCUS_REQUEST_GRANTED && inCall())) {
            // A call has the audio: start when Android hands it back.
            holdForCall("audio busy");
            return;
        }
        // Refused outside a call: some devices refuse spuriously, so play
        // anyway rather than silently doing nothing.
        try {
            player.setVolume(volume, volume);
            player.start();
            acquireWifiLock();
            // Real playback started — the queue is healthy again.
            errorStreak = 0;
            plog("  playing " + srcKind(currentSource) + " at " + fmt(currentPositionMs()));
            emit("play");
            emit("playing");
            startTicker();
            // Resumed inside the last seconds: fade from here, no blip.
            maybeStartFade();
        } catch (Exception e) { /* ignore */ }
        activateSession();
        ensureForeground();
        publishState();
        schedulePrefetch();
    }

    private void internalPause() {
        try {
            if (player != null && player.isPlaying()) {
                player.pause();
                lastPositionMs = player.getCurrentPosition();
                stopTicker();
                stopFade();
                releaseWifiLock();
                emit("pause");
            }
        } catch (Exception e) { /* ignore */ }
    }

    // Play, from the app, the car, or a notification button.
    private void userPlay(String from) {
        cancelIdleStop();
        plog("play (" + from + ")" + (queueIndex >= 0 ? " " + trackLabel(queueIndex) + " at " + fmt(currentPositionMs()) : ""));
        if (!gatePlay(from)) return;
        stopCallWatch();
        playWhenReady = true;
        resumeOnFocusGain = false;
        activateSession();
        if (player == null || needsReload) {
            if (retryRunnable != null) { publishState(); return; } // a retry is already on its way
            // The player was torn down (a dropped stream while paused, or the
            // track gave up): rebuild it where it stopped.
            List<String> chain = (queueIndex >= 0 && queueIndex < ids.size() && currentId.equals(ids.get(queueIndex)))
                ? sourceChain(queueIndex) : new ArrayList<>();
            if (chain.isEmpty() && currentSource.length() > 0) chain.add(currentSource);
            if (chain.isEmpty()) { publishState(); return; }
            ensureForeground();
            alternates.clear();
            String first = chain.remove(0);
            alternates.addAll(chain);
            retryAttempt = 0;
            prepareSource(first, lastPositionMs, true);
            return;
        }
        internalPlay();
    }

    // Pause, from the app, the car, a notification button, or the audio output
    // disconnecting.
    private void userPause(String from) {
        boolean wasPlaying = isPlaying();
        plog("pause (" + from + ")" + (queueIndex >= 0 ? " " + trackLabel(queueIndex) + " at " + fmt(currentPositionMs()) : "")
            + (wasPlaying ? "" : ", wasn't playing") + (resumeOnFocusGain ? ", cancels resume after call" : ""));
        playWhenReady = false;
        resumeOnFocusGain = false;
        stopCallWatch();
        if (retryRunnable != null) { cancelRetry(); needsReload = true; }
        internalPause();
        if (!wasPlaying) emit("pause");
        publishState();
        scheduleIdleStop();
    }

    private void seekToMs(int ms, String from) {
        int target = Math.max(0, ms);
        plog("seek to " + fmt(target) + " (" + from + ")");
        try {
            if (player != null && prepared) {
                player.seekTo(target);
                // Back to full volume; the ticker restarts the fade if the
                // seek landed inside it.
                stopFade();
                player.setVolume(volume, volume);
            } else {
                pendingSeekMs = target;
            }
        } catch (Exception e) { /* ignore */ }
        lastPositionMs = target;
        maybeStartFade();
        publishState();
    }

    // ---- queue ----

    private String at(List<String> list, int i) {
        if (i < 0 || i >= list.size()) return "";
        String s = list.get(i);
        return s == null ? "" : s;
    }

    private boolean isJellyfin(int i) {
        return at(itemIds, i).length() > 0;
    }

    private String jellyfinUrl(String template, int i) {
        if (template.length() == 0 || !isJellyfin(i)) return "";
        return template.replace("{id}", at(itemIds, i));
    }

    // Same name the web layer used when it managed the cache, so files
    // downloaded by earlier versions are still found.
    private String cacheName(int i) {
        String id = at(itemIds, i).replaceAll("[^A-Za-z0-9_-]", "");
        String c = at(containers, i);
        return "jf_" + id + "." + (c.length() > 0 ? c : "mp3");
    }

    private File cachedFile(int i) {
        if (!isJellyfin(i)) return null;
        File f = new File(streamCacheDir(), cacheName(i));
        return (f.exists() && f.length() > 0) ? f : null;
    }

    // Every way to play entry i, best first: a local file; or for Jellyfin the
    // downloaded copy, then the original file streamed, then a transcode.
    private List<String> sourceChain(int i) {
        List<String> out = new ArrayList<>();
        if (i < 0 || i >= ids.size()) return out;
        String local = at(sources, i);
        if (local.length() > 0) { out.add(local); return out; }
        if (!isJellyfin(i)) return out;
        File f = cachedFile(i);
        if (f != null) {
            f.setLastModified(System.currentTimeMillis()); // LRU touch
            out.add(f.getAbsolutePath());
        }
        String direct = jellyfinUrl(directTemplate, i);
        String trans = jellyfinUrl(transcodeTemplate, i);
        if (direct.length() > 0) out.add(direct);
        if (trans.length() > 0) out.add(trans);
        return out;
    }

    // Local entries are empty until the web layer has written the file to disk.
    private boolean playable(int i) {
        return !sourceChain(i).isEmpty();
    }

    // Start entry i. Plays straight away — from disk if it's downloaded,
    // otherwise streaming while the download catches up — instead of waiting
    // for a whole file to download first. notifyJs is set when the change
    // didn't come from the web layer (auto-advance, car / notification buttons).
    private void playIndex(int i, boolean notifyJs, boolean autoplay, String from) {
        plog("track " + trackLabel(i) + " (" + from + ")" + (autoplay ? "" : ", not playing yet"));
        cancelRetry();
        retryAttempt = 0;
        trackGen++;
        queueIndex = i;
        currentId = at(ids, i);
        alternates.clear();
        List<String> chain = sourceChain(i);
        // Update the car / lock screen right away, before the track loads.
        durationMs = 0;
        publishMetadata();
        if (notifyJs) {
            JSObject d = new JSObject();
            d.put("index", i);
            d.put("id", currentId);
            emit("advanced", d);
        }
        if (chain.isEmpty()) {
            playWhenReady = autoplay;
            giveUpOnTrack();
            return;
        }
        String first = chain.remove(0);
        alternates.addAll(chain);
        prepareSource(first, 0, autoplay);
        if (autoplay) schedulePrefetch();
    }

    private void emitStopped() {
        plog("stopped: end of the queue");
        playWhenReady = false;
        emit("pause");
        emit("ended");
        publishState();
        scheduleIdleStop();
    }

    // How long the current track really is: the LONGER of what the player
    // reports and what the server said. A half-written cache file makes the
    // player report the short file's own length, so position reaches "the end"
    // and a truncated track looks like a clean finish.
    private int expectedDurationMs() {
        int fromServer = 0;
        if (queueIndex >= 0 && queueIndex < durationsSec.size() && currentId.equals(at(ids, queueIndex))) {
            int s = durationsSec.get(queueIndex);
            if (s > 0) fromServer = s * 1000;
        }
        return Math.max(durationMs > 0 ? durationMs : 0, fromServer);
    }

    private boolean isStreamCacheFile(String path) {
        return path != null && path.length() > 0 && !path.startsWith("http")
            && path.startsWith(streamCacheDir().getAbsolutePath());
    }

    private void deleteCacheFile(String path) {
        try {
            if (path == null || path.length() == 0 || path.startsWith("http")) return;
            File f = new File(path);
            if (f.exists()) f.delete();
        } catch (Exception e) { /* ignore */ }
    }

    // MediaPlayer reports a stream that died mid-track exactly the same way as a
    // track that finished. Telling them apart by position is what stops a
    // dropped connection from being heard as "song cut short, next song".
    private void handleCompletion() {
        int expected = expectedDurationMs();
        boolean short_ = expected > 0 && lastPositionMs > 0 && lastPositionMs < expected - 5000;
        String ended = "end of " + trackLabel(queueIndex) + " at " + fmt(lastPositionMs) + " of " + fmt(expected)
            + " (" + srcKind(currentSource) + ")";
        if (short_) {
            int resumeAt = lastPositionMs;
            if (!currentSource.startsWith("http")) {
                // The file on disk is incomplete. Bin it so it gets fetched
                // again, and finish this track from the next source.
                shortFileCount++;
                deleteCacheFile(currentSource);
                JSObject bad = new JSObject();
                bad.put("index", queueIndex);
                emit("badcache", bad);
                emitDiag();
                if (!alternates.isEmpty()) {
                    plog(ended + ": file ended early, deleted it, finishing from the next source");
                    releasePlayer();
                    prepareSource(alternates.remove(0), resumeAt, true);
                    return;
                }
                plog(ended + ": file ended early and there's no other source, moving on");
            } else if (lastPositionMs > lastResumeAtMs + 1000) {
                // The stream stopped early while still making progress: the
                // connection dropped. Resume where it cut out. (No progress
                // since the last resume means the source itself is short.)
                truncatedCount++;
                emitDiag();
                plog(ended + ": stream ended early, reconnecting");
                releasePlayer();
                onNetworkDrop(resumeAt);
                return;
            } else {
                plog(ended + ": ended early with no progress since the last resume, moving on");
            }
        } else {
            plog(ended + ": finished");
        }
        advanceOnCompletion(false);
    }

    // Moves to the next playable entry when a track ends. skipRepeatOne is set
    // when the current track failed, so "repeat one" can't loop on a broken track.
    private void advanceOnCompletion(boolean skipRepeatOne) {
        int n = ids.size();
        if (n == 0) { emitStopped(); return; }
        if (!skipRepeatOne && "one".equals(repeatMode) && playable(queueIndex)) { playIndex(queueIndex, true, true, "repeat one"); return; }
        int idx = queueIndex;
        for (int step = 0; step < n; step++) {
            idx++;
            if (idx >= n) {
                if ("all".equals(repeatMode)) idx = 0;
                else { emitStopped(); return; }
            }
            if (playable(idx)) { playIndex(idx, true, true, skipRepeatOne ? "skip after error" : "next in queue"); return; }
        }
        emitStopped();
    }

    // Next / Previous from the car or the notification (the in-app buttons go
    // through the web layer). Next wraps at the end, like the in-app button.
    // During a call the track still changes, but the music is held until the
    // call ends.
    private void skipNext(String from) {
        int n = ids.size();
        if (n == 0) return;
        errorStreak = 0;
        int idx = queueIndex;
        for (int step = 0; step < n; step++) {
            idx = (idx + 1 >= n) ? 0 : idx + 1;
            if (playable(idx)) { playIndex(idx, true, gatePlay(from), "next from " + from); return; }
        }
    }

    private void skipPrevious(String from) {
        int n = ids.size();
        if (n == 0) return;
        errorStreak = 0;
        // More than 3 s in: restart the song, like the in-app button.
        if (player != null && currentPositionMs() > 3000) { seekToMs(0, "previous from " + from); return; }
        int idx = queueIndex;
        for (int step = 0; step < n; step++) {
            idx = (idx - 1 < 0) ? n - 1 : idx - 1;
            if (playable(idx)) { playIndex(idx, true, gatePlay(from), "previous from " + from); return; }
        }
    }

    // ---- prefetch ----

    private File streamCacheDir() {
        File dir = new File(getContext().getCacheDir(), "stream");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    // Download the current track and the next few in the background, so the
    // car keeps playing from disk with the screen off (the web layer is frozen
    // then and can't fetch anything). Idempotent — safe to call on every play.
    private void schedulePrefetch() {
        int n = ids.size();
        if (queueIndex < 0 || queueIndex >= n) return;
        if (directTemplate.length() == 0 && transcodeTemplate.length() == 0) return;
        List<Integer> window = new ArrayList<>();
        Set<String> keep = new HashSet<>();
        for (int k = 0; k <= PREFETCH_AHEAD && k < n; k++) {
            int i = queueIndex + k;
            if (i >= n) {
                if (!"all".equals(repeatMode)) break;
                i -= n;
            }
            if (!isJellyfin(i)) continue;
            window.add(i);
            keep.add(cacheName(i));
        }
        protectedNames = keep;
        for (int i : window) {
            final String name = cacheName(i);
            if (inFlight.contains(name) || new File(streamCacheDir(), name).exists()) continue;
            final String direct = jellyfinUrl(directTemplate, i);
            final String trans = jellyfinUrl(transcodeTemplate, i);
            // Completeness floor for downloads the server sends without a length
            // (transcodes). The stream is requested at up to 320 kbps, so ~24 kB/s
            // is comfortably below a real file yet rejects a half-finished one.
            final long minBytes = durationsSec.get(i) * 24000L;
            final String label = trackLabel(i);
            inFlight.add(name);
            downloader.execute(() -> {
                try {
                    // Skip a track that has left the upcoming window meanwhile.
                    if (!protectedNames.contains(name)) return;
                    String err = "no source";
                    // Prefer the original file; fall back to caching a transcode.
                    for (String u : new String[]{direct, trans}) {
                        if (u.length() == 0) continue;
                        err = downloadToCache(u, name, minBytes);
                        if (err == null) break;
                    }
                    final String result = err;
                    handler.post(() -> {
                        if (result == null) downloadCount++;
                        else { dlFailCount++; lastDlError = result; }
                        plog("  download " + label + (result == null ? " done" : " failed: " + result));
                        emitDiag();
                    });
                } finally {
                    inFlight.remove(name);
                }
            });
        }
    }

    // Download one track into the cache. Returns null on success, or why it
    // failed. Rejects a download it can't verify as complete (no length and no
    // size floor), since a truncated file would otherwise be trusted.
    private String downloadToCache(String url, String name, long minBytes) {
        java.net.HttpURLConnection conn = null;
        File part = null;
        try {
            File dir = streamCacheDir();
            File out = new File(dir, name);
            if (out.exists() && out.length() > 0) return null;
            conn = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            conn.setConnectTimeout(20000);
            // Generous: a server-side transcode can pause between chunks, and on
            // mobile data a large file streams in slowly.
            conn.setReadTimeout(60000);
            conn.setInstanceFollowRedirects(true);
            int code = conn.getResponseCode();
            if (code != 200) return "http " + code;
            long expected = -1;
            try {
                String cl = conn.getHeaderField("Content-Length");
                if (cl != null) expected = Long.parseLong(cl.trim());
            } catch (Exception e) { expected = -1; }
            if (expected <= 0 && minBytes <= 0) return "no length";
            part = new File(dir, name + ".part");
            long total = 0;
            try (InputStream in = conn.getInputStream(); FileOutputStream fos = new FileOutputStream(part)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) {
                    fos.write(buf, 0, n);
                    total += n;
                }
            }
            long required = (expected > 0) ? expected : minBytes;
            if (total < required) { part.delete(); return "incomplete " + total + "/" + required; }
            if (!part.renameTo(out)) { part.delete(); return "rename failed"; }
            trimCache();
            return null;
        } catch (Exception e) {
            try { if (part != null && part.exists()) part.delete(); } catch (Exception ig) { /* ignore */ }
            return e.getClass().getSimpleName() + ": " + e.getMessage();
        } finally {
            try { if (conn != null) conn.disconnect(); } catch (Exception ig) { /* ignore */ }
        }
    }

    // Keep the cache bounded: drop the least recently used files first, but
    // never the current track, the ones just ahead, or a download in progress.
    private void trimCache() {
        try {
            File[] files = streamCacheDir().listFiles();
            if (files == null) return;
            long total = 0;
            for (File f : files) total += f.length();
            if (total <= MAX_CACHE_BYTES) return;
            Set<String> keep = protectedNames;
            java.util.Arrays.sort(files, (a, b) -> Long.compare(a.lastModified(), b.lastModified()));
            for (File f : files) {
                if (total <= MAX_CACHE_BYTES) break;
                String nm = f.getName();
                if (nm.endsWith(".part") || keep.contains(nm)) continue;
                long len = f.length();
                if (f.delete()) total -= len;
            }
        } catch (Exception e) { /* ignore */ }
    }

    // ---- plugin API ----

    private static List<String> toList(com.getcapacitor.JSArray arr) {
        List<String> out = new ArrayList<>();
        if (arr == null) return out;
        for (int i = 0; i < arr.length(); i++) {
            try {
                String s = arr.getString(i);
                out.add(s == null ? "" : s);
            } catch (Exception e) { out.add(""); }
        }
        return out;
    }

    private static void replace(List<String> target, List<String> next, int size) {
        target.clear();
        target.addAll(next);
        while (target.size() < size) target.add("");
    }

    // Replace the queue. Doesn't restart anything: whatever is loaded keeps
    // playing, and native keeps pointing at it by id even if it moved (a
    // reshuffle) — the web layer can lag behind native while the screen is off,
    // so its index alone can't be trusted. If the index had to be corrected,
    // "advanced" tells the web layer which track is really current.
    @PluginMethod
    public void setQueue(PluginCall call) {
        final List<String> nIds = toList(call.getArray("ids"));
        final List<String> nSources = toList(call.getArray("sources"));
        final List<String> nItemIds = toList(call.getArray("itemIds"));
        final List<String> nContainers = toList(call.getArray("containers"));
        final List<String> nTitles = toList(call.getArray("titles"));
        final List<String> nArtists = toList(call.getArray("artists"));
        final List<String> nAlbums = toList(call.getArray("albums"));
        final List<String> nDurations = toList(call.getArray("durations"));
        final List<String> nTrackNos = toList(call.getArray("trackNos"));
        final int index = call.getInt("index", -1);
        final String repeat = call.getString("repeat", "off");
        final String direct = call.getString("directTemplate", "");
        final String transcode = call.getString("transcodeTemplate", "");
        getActivity().runOnUiThread(() -> {
            ids.clear();
            ids.addAll(nIds);
            int n = ids.size();
            replace(sources, nSources, n);
            replace(itemIds, nItemIds, n);
            replace(containers, nContainers, n);
            replace(titles, nTitles, n);
            replace(artists, nArtists, n);
            replace(albums, nAlbums, n);
            durationsSec.clear();
            for (String s : nDurations) {
                int v;
                try { v = (int) Double.parseDouble(s); } catch (Exception e) { v = 0; }
                durationsSec.add(v);
            }
            while (durationsSec.size() < n) durationsSec.add(0);
            trackNos.clear();
            for (String s : nTrackNos) {
                int v;
                try { v = Integer.parseInt(s.trim()); } catch (Exception e) { v = 0; }
                trackNos.add(v);
            }
            while (trackNos.size() < n) trackNos.add(0);
            directTemplate = direct == null ? "" : direct;
            transcodeTemplate = transcode == null ? "" : transcode;
            repeatMode = repeat;
            errorStreak = 0;
            int newIndex = index;
            boolean loaded = player != null || retryRunnable != null || needsReload;
            if (loaded && currentId.length() > 0
                && !(index >= 0 && index < n && currentId.equals(ids.get(index)))) {
                int found = ids.indexOf(currentId);
                if (found >= 0) newIndex = found;
            }
            queueIndex = newIndex;
            plog("queue from app: " + n + " tracks, at " + trackLabel(newIndex)
                + (newIndex != index ? " (app said #" + index + ", kept the playing track)" : ""));
            if (newIndex != index && newIndex >= 0) {
                JSObject d = new JSObject();
                d.put("index", newIndex);
                d.put("id", currentId);
                emit("advanced", d);
            }
            publishMetadata();
            if (playWhenReady) schedulePrefetch();
        });
        call.resolve();
    }

    // Fill in one local entry's file path after the web layer has written it to
    // disk. Cheap: no re-sending of the whole queue.
    @PluginMethod
    public void setSource(PluginCall call) {
        final int index = call.getInt("index", -1);
        final String source = call.getString("source", "");
        getActivity().runOnUiThread(() -> {
            if (index >= 0 && index < sources.size()) sources.set(index, source);
        });
        call.resolve();
    }

    // Play queue entry `index` (the web layer's Play / Next / Previous / tap on
    // a song). Starts at once; see playIndex.
    @PluginMethod
    public void playEntry(PluginCall call) {
        final int index = call.getInt("index", -1);
        final boolean autoplay = Boolean.TRUE.equals(call.getBoolean("autoplay", false));
        getActivity().runOnUiThread(() -> {
            if (index < 0 || index >= ids.size()) return;
            errorStreak = 0;
            if (autoplay) cancelIdleStop();
            playIndex(index, false, autoplay, FROM_APP);
        });
        call.resolve();
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
        call.resolve(diagObject());
    }

    // The playback log, oldest line first, for Settings to show and copy.
    @PluginMethod
    public void getLog(PluginCall call) {
        StringBuilder sb = new StringBuilder();
        synchronized (logLines) {
            for (String l : logLines) sb.append(l).append('\n');
        }
        JSObject ret = new JSObject();
        ret.put("text", sb.toString());
        call.resolve(ret);
    }

    // A line from the web layer (its own Next / Previous, sleep timer, the app
    // coming to the front), so the log has both sides.
    @PluginMethod
    public void log(PluginCall call) {
        final String msg = call.getString("message", "");
        if (msg != null && msg.length() > 0) {
            final String line = "app: " + (msg.length() > 200 ? msg.substring(0, 200) : msg);
            getActivity().runOnUiThread(() -> plog(line));
        }
        call.resolve();
    }

    @PluginMethod
    public void setRepeat(PluginCall call) {
        final String mode = call.getString("mode", "off");
        getActivity().runOnUiThread(() -> {
            repeatMode = mode;
            if (playWhenReady) schedulePrefetch();
        });
        call.resolve();
    }

    // Persist a local track's bytes to a stable file so MediaPlayer can play it
    // by path (needed for the native queue — it can't reach a JavaScript blob).
    // Written to a .part file and renamed when complete, so an app killed
    // mid-write can't leave a short file that is then trusted.
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
                    File part = new File(dir, name + ".part");
                    try (FileOutputStream fos = new FileOutputStream(part)) {
                        fos.write(bytes);
                    }
                    if (!part.renameTo(f)) {
                        part.delete();
                        throw new java.io.IOException("rename failed");
                    }
                }
                JSObject ret = new JSObject();
                ret.put("path", f.getAbsolutePath());
                call.resolve(ret);
            } catch (Exception e) {
                call.reject("persist failed: " + e.getMessage());
            }
        }).start();
    }

    // Load a source directly (a local file path, or a URL for a track that isn't
    // in the queue). `index`, when given, is the queue entry it belongs to, so
    // the car display and auto-advance know where we are.
    @PluginMethod
    public void load(PluginCall call) {
        final String url = call.getString("url", "");
        final String fallback = call.getString("fallback", "");
        final boolean autoplay = Boolean.TRUE.equals(call.getBoolean("autoplay", false));
        final int index = call.getInt("index", -1);
        getActivity().runOnUiThread(() -> {
            startDirect(index);
            plog("load from app: " + trackLabel(index) + (autoplay ? "" : ", not playing yet"));
            if (fallback.length() > 0) alternates.add(fallback);
            prepareSource(url, 0, autoplay);
        });
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
        final int index = call.getInt("index", -1);
        getActivity().runOnUiThread(() -> {
            try {
                // Release any current player first so it isn't holding the temp
                // file we're about to overwrite.
                releasePlayer();
                startDirect(index);
                plog("load from app (bytes): " + trackLabel(index) + (autoplay ? "" : ", not playing yet"));
                byte[] bytes = Base64.decode(data, Base64.DEFAULT);
                File f = new File(getContext().getCacheDir(), "np_current." + ext);
                FileOutputStream fos = new FileOutputStream(f);
                fos.write(bytes);
                fos.close();
                prepareSource(f.getAbsolutePath(), 0, autoplay);
            } catch (Exception e) {
                JSObject d = new JSObject();
                d.put("message", String.valueOf(e.getMessage()));
                emit("error", d);
            }
        });
        call.resolve();
    }

    // Shared set-up for load/loadData: a new track, pointed at its queue entry.
    private void startDirect(int index) {
        cancelRetry();
        retryAttempt = 0;
        trackGen++;
        alternates.clear();
        if (index >= 0 && index < ids.size()) {
            queueIndex = index;
            currentId = ids.get(index);
        } else {
            currentId = "";
        }
        durationMs = 0;
        publishMetadata();
    }

    @PluginMethod
    public void play(PluginCall call) {
        getActivity().runOnUiThread(() -> userPlay(FROM_APP));
        call.resolve();
    }

    @PluginMethod
    public void pause(PluginCall call) {
        getActivity().runOnUiThread(() -> userPause(FROM_APP));
        call.resolve();
    }

    @PluginMethod
    public void seek(PluginCall call) {
        final double seconds = call.getDouble("seconds", 0.0);
        getActivity().runOnUiThread(() -> seekToMs((int) (seconds * 1000), FROM_APP));
        call.resolve();
    }

    @PluginMethod
    public void stop(PluginCall call) {
        getActivity().runOnUiThread(() -> {
            plog("stop (app)");
            cancelRetry();
            stopCallWatch();
            playWhenReady = false;
            resumeOnFocusGain = false;
            needsReload = false;
            releasePlayer();
            currentSource = "";
            alternates.clear();
            queueIndex = -1;
            currentId = "";
            releaseWifiLock();
            abandonFocus();
            publishState();
            stopService();
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
        plog("--- app closing ---");
        super.handleOnDestroy();
        cancelRetry();
        stopCallWatch();
        cancelIdleStop();
        releasePlayer();
        releaseWifiLock();
        abandonFocus();
        unregisterNoisy();
        stopService();
        if (session != null) {
            try { session.setActive(false); session.release(); } catch (Exception e) { /* ignore */ }
            session = null;
        }
        downloader.shutdownNow();
        // Let the last lines reach the file, then stop the writer.
        logWriter.shutdown();
        if (instance == this) instance = null;
    }
}
