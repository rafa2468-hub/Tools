package com.brokis.musicplayer;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Build;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

// Minimal Android audio-focus bridge for the web player.
//
// The @jofr media-session plugin manages the lock-screen session and the
// foreground service but never requests audio focus. Without a media-usage
// audio-focus request Android does not treat our WebView playback as "media",
// so it leaves the sound on the phone speaker instead of routing it to a
// connected Bluetooth / wired headset. Requesting AUDIOFOCUS_GAIN with
// USAGE_MEDIA is exactly what Spotify / YouTube do to claim the media output.
//
// The web layer calls request() right before playback starts and abandon()
// after it stops, and listens for "audioFocusChange" so it can pause on a phone
// call / navigation prompt and resume afterwards.
@CapacitorPlugin(name = "AudioFocus")
public class AudioFocusPlugin extends Plugin {

    private AudioManager audioManager;
    private AudioFocusRequest focusRequest; // API 26+

    private final AudioManager.OnAudioFocusChangeListener focusListener =
        new AudioManager.OnAudioFocusChangeListener() {
            @Override
            public void onAudioFocusChange(int focusChange) {
                JSObject data = new JSObject();
                data.put("change", focusChange);
                notifyListeners("audioFocusChange", data);
            }
        };

    @Override
    public void load() {
        audioManager = (AudioManager) getContext().getSystemService(Context.AUDIO_SERVICE);
    }

    @PluginMethod
    public void request(PluginCall call) {
        int result = AudioManager.AUDIOFOCUS_REQUEST_FAILED;
        try {
            if (audioManager != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    AudioAttributes attrs = new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build();
                    focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(attrs)
                        .setWillPauseWhenDucked(false)
                        .setOnAudioFocusChangeListener(focusListener)
                        .build();
                    result = audioManager.requestAudioFocus(focusRequest);
                } else {
                    result = audioManager.requestAudioFocus(
                        focusListener,
                        AudioManager.STREAM_MUSIC,
                        AudioManager.AUDIOFOCUS_GAIN);
                }
            }
        } catch (Exception e) {
            // Never let an audio-focus hiccup crash playback.
        }
        JSObject ret = new JSObject();
        ret.put("granted", result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED);
        call.resolve(ret);
    }

    @PluginMethod
    public void abandon(PluginCall call) {
        try {
            if (audioManager != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    if (focusRequest != null) {
                        audioManager.abandonAudioFocusRequest(focusRequest);
                    }
                } else {
                    audioManager.abandonAudioFocus(focusListener);
                }
            }
        } catch (Exception e) {
            // ignore
        }
        call.resolve();
    }
}
