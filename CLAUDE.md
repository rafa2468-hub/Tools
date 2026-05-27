# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository overview

A collection of independent tools hosted together:

| Tool | Type | Location |
|---|---|---|
| Landing page | Static HTML | `index.html` |
| Gas savings calculator | Single-file HTML/JS | `gas-calculator.html` |
| Music player | PWA (single-file HTML/JS) | `music-player/` |
| Paperless Scanner | Flutter app (Android/iOS) | `paperless_scanner/` |

## Web tools (gas-calculator, music-player)

**No build system.** Pure HTML/CSS/JS — no npm, no bundler, no transpilation. Open the HTML files directly in a browser.

Both tools are self-contained single files. `gas-calculator.html` is entirely standalone. The music player consists of:
- `music-player/index.html` — all logic and UI
- `music-player/sw.js` — service worker (cache-first strategy for the app shell; audio is never intercepted because it comes from IndexedDB blob URLs)
- `music-player/manifest.webmanifest` — PWA manifest
- `music-player/vendor/jsmediatags.min.js` — vendored ID3 tag reader

**Service worker versioning:** bump `CACHE_VERSION` in `sw.js` whenever `index.html` or any app-shell asset changes, otherwise returning users keep the old cached version.

## Music player architecture

All state lives in a single `state` object. The two audio sources (`local` and `jellyfin`) are parallel sub-objects:

```
state.local.tracks / state.local.playlists    ← IndexedDB-backed local files
state.jellyfin.tracks / state.jellyfin.playlists  ← fetched from Jellyfin server
state.tracks / state.playlists                ← active pointer to one of the above
state.source                                  ← 'local' | 'jellyfin'
```

Switching sources via `setActiveSource()` swaps those pointers and re-renders. Queue management (`setQueue`, `reshuffleQueueKeepingCurrent`) operates on indices into `state.tracks`.

**Persistence:**
- Tracks (local mode): IndexedDB (`music-player-db`, stores `tracks` and `playlists`)
- Playback prefs (volume, shuffle, repeat): `localStorage` under key `music-player-prefs`
- Jellyfin credentials: `localStorage` under key `music-player-jf`
- Active source: `localStorage` under key `music-player-source`

**Jellyfin API:** The `JF` object contains all API client methods. Album names are resolved via a two-step fetch (album list first, then audio items) because per-track `Album` fields are empty when the ID3 tag is missing—`AlbumId` → album name mapping is used instead.

## Paperless Scanner (Flutter)

### Commands

```bash
cd paperless_scanner

# Install dependencies
flutter pub get

# Run on a connected device/emulator
flutter run

# Build debug APK locally
flutter build apk --debug

# Run linter
flutter analyze

# Run tests
flutter test
```

### Architecture

Three-screen flow: `HomeScreen` → (camera scan via `CunningDocumentScanner`) → `PreviewScreen` → (upload) → back to `HomeScreen`. Settings are accessible from `HomeScreen` via `SettingsScreen`.

Services are stateless classes instantiated where needed (no DI framework):

- `StorageService` — reads/writes server URL, API token, and `allowInsecure` flag via `flutter_secure_storage` (Keychain on iOS, EncryptedSharedPreferences on Android)
- `PdfService` — assembles scanned image paths into an A4 PDF using the `pdf` package
- `PaperlessService` — POSTs multipart PDF to `/api/documents/post_document/`; also handles `testConnection()`. Respects `allowInsecure` by bypassing TLS certificate validation when set

**Platform notes:**
- Android `minSdkVersion 29` (required by ML Kit Document Scanner)
- iOS uses VisionKit (no Play Services required); ML Kit model downloaded on first Android scan

### CI (GitHub Actions)

The workflow in `.github/workflows/build-apk.yml` uses an unusual scaffold approach: it runs `flutter create` to generate a fresh project in `/tmp/build`, then overlays the repo's `lib/` and `pubspec.yaml` on top. This avoids committing generated Android project files. It also injects a Gradle init script that forces `compileSdk 35` on all subprojects.

The workflow triggers on pushes to `main` and `claude/mobile-document-scanner-EOLSL`, plus manual dispatch. The resulting debug APK is uploaded as a GitHub Actions artifact (14-day retention).
