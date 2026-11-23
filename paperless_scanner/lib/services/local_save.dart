import 'dart:io';
import 'package:file_picker/file_picker.dart';

/// Saves a finished PDF on the phone, for scans that don't belong in
/// Paperless, through Android's own Save dialog.
///
/// No starting folder is passed on purpose: Android's Save dialog then
/// opens in the folder this app saved to last (it keeps that per app), and
/// Android's default location the first time.
///
/// Returns true when the file was saved, false when the dialog was
/// cancelled. A failed save throws, so the caller keeps the scan.
Future<bool> saveToPhone(File pdf, String fileName) async {
  final bytes = await pdf.readAsBytes();
  final saved = await FilePicker.saveFile(
    fileName: fileName,
    bytes: bytes,
    mimeType: 'application/pdf',
  );
  return saved != null;
}
