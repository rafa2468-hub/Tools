# Paperless Scanner

Flutter app for Android and iOS: scan documents with automatic edge detection, generate a PDF, and upload directly to a [Paperless-ngx](https://docs.paperless-ngx.com) server.

## Features

- ML Kit Document Scanner — live edge detection, perspective correction, brightness enhancement
- Multi-page sessions (up to 20 pages per PDF)
- Direct upload to Paperless-ngx REST API
- Share PDF to any app via the OS share sheet
- API token stored in the device Keychain / EncryptedSharedPreferences

## Setup

### Prerequisites

- Flutter SDK ≥ 3.3: https://docs.flutter.dev/get-started/install
- Android: device / emulator with Google Play Services (required by ML Kit)
- iOS: device or simulator running iOS 13+

### Install & run

```bash
cd paperless_scanner
flutter pub get
flutter run
```

### Configure the app

1. Open the app and tap the **Settings** icon (top-right)
2. Enter your Paperless-ngx server URL, e.g. `https://paperless.home.example.com`
3. Paste your API token (Paperless → Settings → Users → your user → API Token)
4. Tap **Test Connection** to verify, then **Save Settings**

## Tech stack

| Concern | Package |
|---|---|
| Document scanning | `google_mlkit_document_scanner` |
| PDF generation | `pdf` + `printing` |
| HTTP client | `http` |
| Secure storage | `flutter_secure_storage` |
| Share sheet | `share_plus` |

## Project layout

```
lib/
  main.dart                  # App entry point & theme
  models/
    scan_session.dart        # In-memory scan result holder
  services/
    storage_service.dart     # Read/write server URL + token (Keychain)
    pdf_service.dart         # Assemble scanned images into A4 PDF
    paperless_service.dart   # POST document to Paperless-ngx API
  screens/
    home_screen.dart         # Launch scanner, entry point
    preview_screen.dart      # Page thumbnails, title, upload
    settings_screen.dart     # Server URL + token config
```

## Android notes

- `minSdkVersion 29` (Android 10) required by ML Kit Document Scanner
- The ML Kit model is downloaded on first scan (requires internet)

## iOS notes

- Add camera and photo library usage descriptions are already in `Info.plist`
- ML Kit runs fully on-device on iOS (no Play Services dependency)
