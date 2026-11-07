package com.brokis.musicplayer;

import android.media.AudioManager;
import android.os.Bundle;
import android.webkit.WebView;
import androidx.activity.OnBackPressedCallback;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Register our plugins BEFORE super.onCreate so they are exposed to
        // JavaScript on the very first page load (adding them later would only
        // take effect after a reload). NativePlayer plays audio through Android's
        // MediaPlayer so it routes to USB-C / Bluetooth / the car; AudioFocus is
        // now dormant (NativePlayer handles focus itself) but kept registered.
        registerPlugin(NativePlayerPlugin.class);
        registerPlugin(AudioFocusPlugin.class);

        super.onCreate(savedInstanceState);

        // Let the player auto-advance to the next track. Without this the WebView
        // requires a fresh user tap for every play() call, so a track ends and
        // nothing follows (tapping a song works because that's a gesture).
        this.bridge.getWebView().getSettings().setMediaPlaybackRequiresUserGesture(false);

        // Route the hardware volume rocker to the music stream so it adjusts
        // playback volume (including on the connected Bluetooth / wired headset),
        // matching how real media apps behave.
        setVolumeControlStream(AudioManager.STREAM_MUSIC);

        // Android's Back button (v2.0.0). Capacitor 6 core has no Back handling,
        // so Back used to leave the app even with the full player open. Ask the
        // page first: window.appBack() closes an open menu, then the player, then
        // goes back a page, and returns true when it did. Otherwise (the main
        // library screen, or the page isn't loaded) leave the app the normal way.
        getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
            @Override
            public void handleOnBackPressed() {
                WebView wv = pageView();
                if (wv == null) {
                    leaveApp(this);
                    return;
                }
                wv.evaluateJavascript(BACK_JS, value -> {
                    if (!"true".equals(value)) leaveApp(this);
                });
            }
        });
    }

    private static final String BACK_JS =
        "(function(){try{return !!(window.appBack && window.appBack());}catch(e){return false;}})()";

    private WebView pageView() {
        return bridge != null ? bridge.getWebView() : null;
    }

    // Turn our callback off for one press so Android's default runs (it closes
    // the screen, as Back always did here), then turn it back on.
    private void leaveApp(OnBackPressedCallback callback) {
        callback.setEnabled(false);
        getOnBackPressedDispatcher().onBackPressed();
        callback.setEnabled(true);
    }
}
