# Patched copy of cunning_document_scanner 3.1.0

Upstream: https://pub.dev/packages/cunning_document_scanner, version 3.1.0
(archive sha256 be2745944c19ed94be45f0d7724a02583b73699821f2fd7cbeb18727631dbfbe,
the same hash pub.dev serves). MIT licence, kept in `LICENSE`.

Copied: `android/`, `ios/`, `lib/`, `LICENSE`, `pubspec.yaml`, `README.md`,
`CHANGELOG.md`. Left out: `example/`, `test/`, `tool/`, `analysis_options.yaml`.

## The one change

`android/src/main/kotlin/biz/cunning/cunning_document_scanner/CunningDocumentScannerPlugin.kt`,
`startScan`: when Google's ML Kit scanner fails to start (`getStartScanIntent`
fails or throws), upstream silently opens its fallback scanner: the phone's
plain camera app, then a crop box the user drags by hand, with no edge
detection. This copy fails the call instead, with code `SCANNER_UNAVAILABLE`
and a message naming the exception, ML Kit's error code or the Play services
status, the top six stack frames, and up to three causes. The app shows it (`lib/screens/scanner_error.dart`).

The fallback launcher (`openFallbackScanner`, `createDocumentScanIntent`) is
removed. `DocumentScannerActivity` stays: gallery-only imports still use it.

Why: Paperless Scanner 1.2.0 on a Pixel got the fallback camera on every
scan (2026-11-22), and upstream throws the reason away.

## Updating

Download the new version's archive from
`https://pub.dev/api/archives/cunning_document_scanner-<version>.tar.gz`,
check its sha256 against pub.dev, replace these folders, and reapply the
change above (search the plugin for `openFallbackScanner`).
