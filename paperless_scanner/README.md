# Paperless Scanner

Android app: scan a document with automatic edge detection, turn it into a PDF, upload it to a [Paperless-ngx](https://docs.paperless-ngx.com) server, and show what Paperless actually did with it (imported as document #N, duplicate of #N, or the error).

## Features

- Document scanner with edge detection and perspective correction (`cunning_document_scanner`), up to 20 pages per PDF, gallery import allowed
- Upload through the Paperless-ngx REST API, then follow the import task to its real result
- Trust a home-lab certificate authority (import its public root certificate); no "accept any certificate" switch
- Share the PDF to any app through the Android share sheet
- Server URL, API token and imported CA kept in Android EncryptedSharedPreferences

## Get the app

GitHub Actions builds the APK (`.github/workflows/build-apk.yml`) whenever `paperless_scanner/` changes on `main` or in a pull request: Actions → *Build Paperless Scanner APK* → the run → artifact `paperless-scanner`. Every build is signed with the same key (kept in the repo secrets `PAPERLESS_KEYSTORE_B64` and `PAPERLESS_KEYSTORE_PASSWORD`), so a new APK installs over the old one and keeps the settings.

Version 1.0.0 builds were signed with a throwaway key per build. Uninstall 1.0.0 once before installing 1.1.0; after that, updates install in place.

## Configure

1. Open the app, tap **Settings** (top right).
2. **Server URL**, e.g. `https://paperless.home.lab`.
3. **API token**: in Paperless, click your name (top right) → **My Profile** → **API Auth Token**.
4. **Home certificate authority** (only if the server's certificate comes from your own CA, e.g. mkcert): copy the CA's **public** certificate (`rootCA.pem`, PEM or DER) to the phone and tap **Import CA file**. The app refuses a file that contains a private key. It adds this CA to the phone's built-in trusted roots, for this app only; the hostname is still checked.
5. **Test Connection** checks the URL, certificate and token without saving anything. **Save Settings** saves.

## How an upload works

1. The pages become one PDF (A4, image fitted to the page).
2. `POST /api/documents/post_document/` with the PDF and the optional title. Paperless answers with a task ID; that only means the file was queued.
3. The app polls `GET /api/tasks/?task_id=…` (API version 10) every 2 seconds for up to 2 minutes and reads the result the way Paperless's own web UI does: `document_id` (imported), `duplicate_of` (duplicate), `error_message` / `reason` (failed). If it's still running after 2 minutes, the app says so; check Paperless's Tasks page.

The PDF's file name keeps letters in any language; without a title it's `scan-YYYY-MM-DD-HHMM.pdf`, which Paperless also uses as the title.

## Build details

The repo keeps only the app's own files. CI runs `flutter create` (Android only) in `/tmp/build`, then copies in:

- `lib/`, `test/`, `pubspec.yaml` and `pubspec.lock` (resolved with `--enforce-lockfile`, so builds are repeatable; to change a package, update `pubspec.yaml`, take the new `pubspec.lock` from the run's `pubspec-lock` artifact and commit it)
- `android/build.gradle.kts` as the app-level Gradle file (minSdk 29, compile/target SDK 35)

It adds the INTERNET and CAMERA permissions to the generated manifest, then runs `flutter analyze`, `flutter test`, builds a debug APK and re-signs it with the app's key. Flutter is pinned in the workflow (`flutter-version`); bump it on purpose.

`test/server_test.dart` runs a fake Paperless server on localhost behind a throwaway CA made with `openssl`. It checks the certificate trust (with and without the CA, and a wrong hostname), the token check, the upload and the task results.

## Project layout

```
lib/
  main.dart                  # App entry point & theme
  models/
    scan_session.dart        # Scanned pages held in memory
    server_config.dart       # URL, token, imported CA
  services/
    storage_service.dart     # Load/save ServerConfig (EncryptedSharedPreferences)
    tls.dart                 # CA import checks, TLS context
    paperless_service.dart   # Test connection, upload, follow the task
    task_result.dart         # Read a Paperless task's outcome
    filename.dart            # PDF file name from the title
    pdf_service.dart         # Scanned images → PDF
  screens/
    home_screen.dart         # Start a scan
    preview_screen.dart      # Thumbnails, title, upload, share
    settings_screen.dart     # Server, token, CA, test, save
test/
  pure_test.dart             # File names, task parsing
  server_test.dart           # TLS, token, upload, task polling against a fake server
```

## iOS

Not built. The code would need iOS-specific work: on iOS, Dart's `SecurityContext.setTrustedCertificatesBytes` takes a single DER certificate only, and the app needs `NSCameraUsageDescription` and `NSPhotoLibraryUsageDescription` in `Info.plist`.
