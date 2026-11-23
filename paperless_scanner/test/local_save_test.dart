import 'dart:io';
import 'dart:typed_data';
import 'package:file_picker/file_picker.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:paperless_scanner/services/local_save.dart';

/// Stands in for Android's Save dialog and records what it was asked.
class _FakeSaveDialog extends FilePickerPlatform {
  _FakeSaveDialog(this.answer);

  /// What the "user" does: a Uri saves, null cancels, an error fails.
  final Future<Uri?> Function() answer;

  String? fileName;
  Uint8List? bytes;
  String? mimeType;
  String? initialDirectory;
  int calls = 0;

  @override
  Future<Uri?> saveFile({
    required String fileName,
    required Uint8List bytes,
    required String mimeType,
    String? dialogTitle,
    String? initialDirectory,
    Function(FilePickerStatus)? onFileSaving,
    WindowsOptions windowsOptions = const WindowsOptions(),
    LinuxOptions linuxOptions = const LinuxOptions(),
    WebOptions webOptions = const WebOptions(),
  }) {
    calls++;
    this.fileName = fileName;
    this.bytes = bytes;
    this.mimeType = mimeType;
    this.initialDirectory = initialDirectory;
    return answer();
  }
}

void main() {
  late Directory dir;
  late File pdf;
  final content = Uint8List.fromList('%PDF-1.4 test'.codeUnits);

  setUp(() async {
    dir = await Directory.systemTemp.createTemp('local_save_test');
    pdf = File('${dir.path}/doc.pdf');
    await pdf.writeAsBytes(content);
  });

  tearDown(() async {
    await dir.delete(recursive: true);
  });

  test('saved: hands over the PDF and its name, no starting folder', () async {
    final fake = _FakeSaveDialog(() async => Uri.parse('/document/primary:Download/Rachunek.pdf'));
    FilePickerPlatform.instance = fake;

    expect(await saveToPhone(pdf, 'Rachunek Łódź.pdf'), isTrue);
    expect(fake.calls, 1);
    expect(fake.fileName, 'Rachunek Łódź.pdf');
    expect(fake.bytes, content);
    expect(fake.mimeType, 'application/pdf');
    // No starting folder, so Android opens the last folder this app used.
    expect(fake.initialDirectory, isNull);
  });

  test('cancelled: reports not saved', () async {
    FilePickerPlatform.instance = _FakeSaveDialog(() async => null);
    expect(await saveToPhone(pdf, 'scan.pdf'), isFalse);
  });

  test('failed: the error reaches the caller, so the scan is kept', () async {
    FilePickerPlatform.instance = _FakeSaveDialog(
      () async => throw PlatformException(code: 'Error while saving file', message: 'disk full'),
    );
    await expectLater(saveToPhone(pdf, 'scan.pdf'), throwsA(isA<PlatformException>()));
  });
}
