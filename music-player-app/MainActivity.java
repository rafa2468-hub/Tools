package com.brokis.musicplayer;

import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Let the player auto-advance to the next track. Without this the WebView
        // requires a fresh user tap for every play() call, so a track ends and
        // nothing follows (tapping a song works because that's a gesture).
        this.bridge.getWebView().getSettings().setMediaPlaybackRequiresUserGesture(false);
    }
}
