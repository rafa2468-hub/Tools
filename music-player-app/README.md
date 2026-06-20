# Music Player — Android wrapper (Capacitor)

Wraps the `../music-player` PWA into a native Android APK. The web assets are
bundled inside the APK, so the app shell works offline and launches instantly;
Jellyfin streaming still needs the network like any other client.

Nothing here is committed except the config — the `www/` copy, the generated
`android/` project, icons and `node_modules/` are all produced at build time by
`.github/workflows/build-music-player-apk.yml`.

## Build a debug APK

Push to the `claude/music-player-apk` branch or run the **Build Music Player
APK** workflow manually (Actions → Run workflow). Download the
`music-player-apk` artifact from the run and sideload `music-player-debug.apk`.

## Build locally (needs Node 18+, JDK 17, Android SDK)

```sh
cd music-player-app
npm install
rm -rf www && mkdir www && cp -r ../music-player/. www/
npx cap add android
npx cap sync android
cd android && ./gradlew assembleDebug
# APK at android/app/build/outputs/apk/debug/app-debug.apk
```

App id: `com.brokis.musicplayer` · name: **Music Player** (edit in
`capacitor.config.json`).
