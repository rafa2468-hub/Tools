import 'dart:convert';
import 'dart:io';
import 'dart:math';

/// A multipart/form-data body that can repeat a field name (Paperless wants
/// one `tags` field per tag; package:http's MultipartRequest can't do that)
/// and streams the file instead of holding it in memory.
class MultipartBody {
  MultipartBody({
    required this.fields,
    required this.fileField,
    required this.file,
    required this.filename,
    this.fileContentType = 'application/pdf',
    String? boundary,
  }) : boundary = boundary ?? _randomBoundary();

  final List<MapEntry<String, String>> fields;
  final String fileField;
  final File file;
  final String filename;
  final String fileContentType;
  final String boundary;

  String get contentType => 'multipart/form-data; boundary=$boundary';

  List<int> get _head {
    final b = StringBuffer();
    for (final f in fields) {
      b
        ..write('--$boundary\r\n')
        ..write('Content-Disposition: form-data; name="${_quote(f.key)}"\r\n')
        ..write('\r\n')
        ..write(f.value)
        ..write('\r\n');
    }
    b
      ..write('--$boundary\r\n')
      ..write('Content-Disposition: form-data; name="${_quote(fileField)}"; '
          'filename="${_quote(filename)}"\r\n')
      ..write('Content-Type: $fileContentType\r\n')
      ..write('\r\n');
    return utf8.encode(b.toString());
  }

  List<int> get _tail => utf8.encode('\r\n--$boundary--\r\n');

  Future<int> get length async =>
      _head.length + await file.length() + _tail.length;

  Stream<List<int>> stream() async* {
    yield _head;
    yield* file.openRead();
    yield _tail;
  }

  // Same escaping as package:http: no raw line breaks or quotes in a header.
  static String _quote(String s) => s
      .replaceAll('\r\n', '%0D%0A')
      .replaceAll('\r', '%0D')
      .replaceAll('\n', '%0A')
      .replaceAll('"', '%22');

  static String _randomBoundary() {
    final r = Random.secure();
    final hex = List.generate(16, (_) => r.nextInt(256))
        .map((b) => b.toRadixString(16).padLeft(2, '0'))
        .join();
    return 'paperless-scanner-$hex';
  }
}
