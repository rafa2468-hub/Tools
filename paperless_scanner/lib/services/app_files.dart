import 'dart:convert';
import 'dart:io';
import 'package:cunning_document_scanner/cunning_document_scanner.dart';
import 'package:path_provider/path_provider.dart';
import '../models/choices.dart';
import 'upload_queue.dart';

/// Where the app keeps its files, and the clean-up that keeps them small.
class AppFiles {
  /// Kept until uploaded: app storage, which Android doesn't clear.
  static Future<UploadQueue> queue() async =>
      UploadQueue(Directory('${(await getApplicationSupportDirectory()).path}/queue'));

  /// Shrunk pages and the PDF for one upload; removed when it's done.
  static Future<Directory> newWorkDir() async {
    final tmp = await getTemporaryDirectory();
    return Directory('${tmp.path}/work_${DateTime.now().microsecondsSinceEpoch}');
  }

  static Future<File> _choicesFile() async =>
      File('${(await getApplicationSupportDirectory()).path}/choices.json');

  /// The last tag/correspondent/type lists fetched, so the pickers work
  /// away from home too.
  static Future<Choices> loadChoices() async {
    try {
      final f = await _choicesFile();
      if (!await f.exists()) return const Choices();
      return Choices.fromJson(jsonDecode(await f.readAsString()) as Map<String, dynamic>);
    } on Object {
      return const Choices();
    }
  }

  static Future<void> saveChoices(Choices choices) async {
    final f = await _choicesFile();
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(choices.toJson()));
  }

  /// Deletes the images and work files of a finished scan.
  static Future<void> deleteFiles(Iterable<String> paths) async {
    for (final p in paths) {
      try {
        final f = File(p);
        if (await f.exists()) await f.delete();
      } on FileSystemException {
        // best effort
      }
    }
  }

  static Future<void> deleteDir(Directory? dir) async {
    if (dir == null) return;
    try {
      if (await dir.exists()) await dir.delete(recursive: true);
    } on FileSystemException {
      // best effort
    }
  }

  /// At app start, with no scan open: drops everything a scan can leave
  /// behind (the scanner's own copies, old work folders). Queued scans live
  /// elsewhere and are not touched.
  static Future<void> cleanUpAtStart() async {
    try {
      await CunningDocumentScanner.cleanCache();
    } on Object {
      // not fatal
    }
    try {
      final tmp = await getTemporaryDirectory();
      await for (final e in tmp.list()) {
        final name = e.uri.pathSegments.where((s) => s.isNotEmpty).last;
        if (e is Directory && name.startsWith('work_')) {
          await deleteDir(e);
        } else if (e is File && name.startsWith('scan_') && name.endsWith('.pdf')) {
          await e.delete(); // left by 1.1.0
        }
      }
    } on Object {
      // not fatal
    }
  }
}
