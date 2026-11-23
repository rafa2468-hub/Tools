# Paperless Scanner

Android app: scan a document with automatic edge detection, tidy the pages, turn them into a PDF, upload it to a [Paperless-ngx](https://docs.paperless-ngx.com) server, and show what Paperless actually did with it (imported as document #N, duplicate of #N, or the error). Away from the server, scans wait on the phone and upload by themselves on Wi-Fi.

## Features

- Document scanner with edge detection and perspective correction (`cunning_document_scanner`, Google's ML Kit scanner), up to 20 pages per PDF, gallery import allowed
- Before uploading: reorder, rotate and delete pages, add more pages, view a page full screen
- Optional details sent with the upload: title, tags, correspondent, document type, the document's date (lists come from Paperless and are kept on the phone for use away from home)
- Pages shrunk to 200 DPI JPEGs (longest side 2200 px), each PDF page shaped like its scan, 11 inches on the long side
- Upload through the Paperless-ngx REST API, then follow the import task to its real result
- Offline queue: if the server can't be reached, the PDF is kept on the phone and uploads when the app is opened or, in the background, about every 15 minutes on Wi-Fi
- Trust a home-lab certificate authority (import its public root certificate); no "accept any certificate" switch
- Share the PDF to any app through the Android share sheet
- Server URL, API token and imported CA kept in the plugin's encrypted storage (Android Keystore)
- Cleans up after itself: scanned images and work files are deleted once a scan is uploaded, queued or discarded, and leftovers at the next start

## Get the app

GitHub Actions builds the APK (`.github/workflows/build-apk.yml`) whenever `paperless_scanner/` changes on `main` or in a pull request: Actions → *Build Paperless Scanner APK* → the run → artifact `paperless-scanner`. Every build is signed with the same key (kept in the repo secrets `PAPERLESS_KEYSTORE_B64` and `PAPERLESS_KEYSTORE_PASSWORD`), so a new APK installs over the old one and keeps the settings.

Version 1.0.0 builds were signed with a throwaway key per build. Uninstall 1.0.0 once before installing 1.1.0 or later; after that, updates install in place.

## Configure

1. Open the app, tap **Settings** (top right).
2. **Server URL**, e.g. `https://paperless.home.lab`.
3. **API token**: in Paperless, click your name (top right) → **My Profile** → **API Auth Token**.
4. **Home certificate authority** (only if the server's certificate comes from your own CA, e.g. mkcert): copy the CA's **public** certificate (`rootCA.pem`, PEM or DER) to the phone and tap **Import CA file**. The app refuses a file that contains a private key. It adds this CA to the phone's built-in trusted roots, for this app only; the hostname is still checked.
5. **Test Connection** checks the URL, certificate and token without saving anything. **Save Settings** saves.

## How an upload works

1. Each page is rotated as set in the preview and shrunk; the pages become one PDF.
2. `POST /api/documents/post_document/` with the PDF and the details that were set (`title`, one `tags` field per tag, `correspondent`, `document_type`, `created` sent as that day at 12:00 so no time zone moves it). Paperless answers with a task ID; that only means the file was queued.
3. The app polls `GET /api/tasks/?task_id=…` (API version 10) every 2 seconds for up to 2 minutes and reads the result the way Paperless's own web UI does: `document_id` (imported), `duplicate_of` (duplicate), `error_message` / `reason` (failed). If it's still running after 2 minutes, the app says so; check Paperless's Tasks page.
4. If the server can't be reached at all, the PDF and its details go into the queue instead (app storage, not the cache). Each queue run tries the oldest scan first and stops at the first sign the server is out of reach. A scan Paperless refuses is marked failed and stays until it's retried or removed (Home → the queue card).

The PDF's file name keeps letters in any language; without a title it's `scan-YYYY-MM-DD-HHMM.pdf`, which Paperless also uses as the title.

## Build details

The repo keeps only the app's own files. CI runs `flutter create` (Android only) in `/tmp/build`, then copies in:

- `lib/`, `test/`, `analysis_options.yaml`, `third_party/` (the patched scanner plugin), `pubspec.yaml` and `pubspec.lock` (resolved with `--enforce-lockfile`, so builds are repeatable; to change a package, update `pubspec.yaml`, delete `pubspec.lock`, take the new one from the run's `pubspec-lock` artifact and commit it)
- `android/build.gradle.kts` as the app-level Gradle file (Flutter's own template, with minSdk 29)

It adds the INTERNET and CAMERA permissions to the generated manifest and sets `shrink=false` in `android/gradle.properties` (Flutter's default R8 shrinking of release builds broke Google's scanner: it failed to start with a NullPointerException), then runs `flutter analyze`, `flutter test`, builds a release APK and re-signs it with the app's key. Flutter is pinned in the workflow (`flutter-version`); bump it on purpose.

The scanner plugin is a patched copy of `cunning_document_scanner` 3.1.0 in `third_party/` (MIT licence kept). The stock plugin silently opens a plain camera with a manual crop box whenever Google's scanner fails to start; the copy reports Google's error instead, and the app shows it in a dialog. Details and how to update it: `third_party/cunning_document_scanner/PATCHES.md`.

`flutter_secure_storage` stays on 10.x on purpose: 10.x moves what 9.x saved to its new format on first launch, while 11.x can't read 9.x data at all. Moving to 11.x is safe once every installed copy has run 1.2.0 or later.

`test/server_test.dart` runs a fake Paperless server on localhost behind a throwaway CA made with `openssl`. It checks the certificate trust (with and without the CA, and a wrong hostname), the token check, the upload and its form fields, the tag lists, the task results and the queue. `test/pdf_queue_test.dart` covers page sizes, shrinking, the upload details and the queue on disk.

## Project layout

```
lib/
  main.dart                  # App entry point & theme
  models/
    scan_session.dart        # Pages of the scan being reviewed (order, rotation)
    server_config.dart       # URL, token, imported CA
    upload_meta.dart         # Title, tags, correspondent, type, date
    choices.dart             # Tag / correspondent / type lists
  services/
    storage_service.dart     # Load/save ServerConfig (encrypted storage)
    tls.dart                 # CA import checks, TLS context
    paperless_service.dart   # Test connection, upload, follow the task, lists
    multipart.dart           # Upload body (repeated fields, streamed file)
    task_result.dart         # Read a Paperless task's outcome
    filename.dart            # PDF file name from the title
    image_prep.dart          # Rotate and shrink a page image
    pdf_service.dart         # Page images → PDF
    upload_queue.dart        # Scans waiting for the server, on disk
    queue_runner.dart        # Upload what's waiting
    background.dart          # The Wi-Fi background job (workmanager)
    app_files.dart           # Where files live, clean-up
  screens/
    home_screen.dart         # Start a scan, queue status
    preview_screen.dart      # Pages, details, upload, share
    queue_screen.dart        # Waiting / failed scans
    settings_screen.dart     # Server, token, CA, test, save
    scanner_error.dart       # Dialog for a scanner that won't start
test/
  pure_test.dart             # File names, task parsing
  pdf_queue_test.dart        # Images, PDF pages, details, queue
  server_test.dart           # TLS, token, upload, lists, queue against a fake server
  scanner_error_test.dart    # Scanner error text and dialog
third_party/
  cunning_document_scanner/  # Patched scanner plugin (see its PATCHES.md)
```

## iOS

Not built. The code would need iOS-specific work: on iOS, Dart's `SecurityContext.setTrustedCertificatesBytes` takes a single DER certificate only, the app needs `NSCameraUsageDescription` and `NSPhotoLibraryUsageDescription` in `Info.plist`, and background uploads need iOS's own background-task setup.
