package com.brokis.musicplayer;

import android.media.AudioManager;
import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Register our audio-focus plugin BEFORE super.onCreate so it is exposed
        // to JavaScript on the very first page load (adding it later would only
        // take effect after a reload).
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
    }
}
