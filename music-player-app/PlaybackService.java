package com.brokis.musicplayer;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

// Foreground service that keeps the app alive as a media app while the queue is
// in use, and relays the playback notification's buttons to NativePlayerPlugin.
// It holds no playback state of its own — the plugin owns the player, the queue
// and the media session, and builds the notification.
//
// Started only with startService() while the app is in the foreground (never
// startForegroundService(), whose "must call startForeground within 5 s"
// contract is an uncatchable crash if anything goes wrong), and every
// startForeground() call is guarded — a refused start means playing on without
// the service, never a crash.
public class PlaybackService extends Service {

    static final String ACTION_START = "com.brokis.musicplayer.action.START";
    static final String ACTION_PLAY = "com.brokis.musicplayer.action.PLAY";
    static final String ACTION_PAUSE = "com.brokis.musicplayer.action.PAUSE";
    static final String ACTION_NEXT = "com.brokis.musicplayer.action.NEXT";
    static final String ACTION_PREV = "com.brokis.musicplayer.action.PREV";
    static final String ACTION_STOP = "com.brokis.musicplayer.action.STOP";

    static final int NOTIFICATION_ID = 1001;

    // Main-thread only.
    static PlaybackService instance;

    private boolean foreground = false;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        NativePlayerPlugin plugin = NativePlayerPlugin.instance;
        if (plugin == null) {
            // The app side is gone; nothing to show or control.
            shutdown();
            return START_NOT_STICKY;
        }
        if (action == null || ACTION_START.equals(action)) {
            promote(plugin.buildNotification());
        } else {
            plugin.handleServiceAction(action);
        }
        return START_NOT_STICKY;
    }

    // Become (or stay) a foreground media service showing this notification.
    void promote(Notification n) {
        if (n == null) return;
        if (foreground) { update(n); return; }
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
            foreground = true;
        } catch (Throwable t) {
            // Refused (app in the background on Android 12+): carry on without it.
        }
    }

    void update(Notification n) {
        if (!foreground || n == null) return;
        try {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm != null) nm.notify(NOTIFICATION_ID, n);
        } catch (Exception e) { /* ignore */ }
    }

    // Drop the notification and stop.
    void shutdown() {
        try {
            if (Build.VERSION.SDK_INT >= 24) stopForeground(Service.STOP_FOREGROUND_REMOVE);
            else stopForeground(true);
        } catch (Exception e) { /* ignore */ }
        foreground = false;
        stopSelf();
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        NativePlayerPlugin plugin = NativePlayerPlugin.instance;
        if (plugin != null) plugin.onTaskRemoved();
        else shutdown();
        super.onTaskRemoved(rootIntent);
    }

    @Override
    public void onDestroy() {
        foreground = false;
        if (instance == this) instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
