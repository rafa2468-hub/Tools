import 'dart:convert';
import 'dart:io';
import 'dart:math';
import '../models/upload_meta.dart';

/// A scan waiting to go to Paperless: its PDF plus the upload details.
class QueuedItem {
  QueuedItem({
    required this.id,
    required this.filename,
    required this.meta,
    required this.queuedAt,
    this.attempts = 0,
    this.lastError,
    this.lastTriedAt,
    this.failed = false,
  });

  final String id;
  final String filename;
  final UploadMeta meta;
  final DateTime queuedAt;
  int attempts;
  String? lastError;
  DateTime? lastTriedAt;

  /// Paperless took it but refused it, or the server rejected the request.
  /// Retrying won't help on its own; the user decides what to do.
  bool failed;

  String get label => meta.cleanTitle ?? filename;

  Map<String, dynamic> toJson() => {
        'id': id,
        'filename': filename,
        'meta': meta.toJson(),
        'queued_at': queuedAt.toIso8601String(),
        'attempts': attempts,
        'last_error': lastError,
        'last_tried_at': lastTriedAt?.toIso8601String(),
        'failed': failed,
      };

  static QueuedItem? fromJson(Object? j) {
    if (j is! Map<String, dynamic>) return null;
    try {
      final tried = j['last_tried_at'];
      return QueuedItem(
        id: j['id'] as String,
        filename: j['filename'] as String,
        meta: UploadMeta.fromJson(j['meta'] as Map<String, dynamic>),
        queuedAt: DateTime.parse(j['queued_at'] as String),
        attempts: j['attempts'] as int? ?? 0,
        lastError: j['last_error'] as String?,
        lastTriedAt: tried is String ? DateTime.tryParse(tried) : null,
        failed: j['failed'] as bool? ?? false,
      );
    } on Object {
      return null;
    }
  }
}

/// What happened to a queued scan, shown on the home screen.
class QueueReport {
  const QueueReport({
    required this.at,
    required this.label,
    required this.outcome,
    this.documentId,
    this.message,
  });

  final DateTime at;
  final String label;

  /// 'imported', 'duplicate', 'processing' or 'failed'.
  final String outcome;
  final int? documentId;
  final String? message;

  String get summary => switch (outcome) {
        'imported' => documentId == null
            ? '"$label" imported into Paperless.'
            : '"$label" imported as document #$documentId.',
        'duplicate' => '"$label" was already in Paperless (#$documentId).',
        'processing' => '"$label" uploaded; Paperless was still working on it.',
        _ => '"$label" failed: ${message ?? 'unknown error'}',
      };

  Map<String, dynamic> toJson() => {
        'at': at.toIso8601String(),
        'label': label,
        'outcome': outcome,
        'document_id': documentId,
        'message': message,
      };

  static QueueReport? fromJson(Object? j) {
    if (j is! Map<String, dynamic>) return null;
    final at = DateTime.tryParse(j['at'] as String? ?? '');
    if (at == null) return null;
    return QueueReport(
      at: at,
      label: j['label'] as String? ?? '',
      outcome: j['outcome'] as String? ?? 'failed',
      documentId: j['document_id'] as int?,
      message: j['message'] as String?,
    );
  }
}

/// Scans waiting for the server, kept as files in [dir] (app storage, not
/// the cache, so Android doesn't clear them): `<id>.pdf` and `<id>.json`.
class UploadQueue {
  UploadQueue(this.dir);

  final Directory dir;

  static const _reportsFile = 'reports.json';
  static const _lockFile = '.drain.lock';
  static const maxReports = 20;

  /// A lock older than this is left over from a run that died.
  static const staleLock = Duration(minutes: 15);

  File pdfOf(QueuedItem item) => File('${dir.path}/${item.id}.pdf');
  File _jsonOf(String id) => File('${dir.path}/$id.json');

  Future<QueuedItem> add(File pdf, UploadMeta meta, String filename) async {
    await dir.create(recursive: true);
    final id = '${DateTime.now().millisecondsSinceEpoch}-${Random().nextInt(1 << 32).toRadixString(16)}';
    final item = QueuedItem(id: id, filename: filename, meta: meta, queuedAt: DateTime.now());
    await pdf.copy(pdfOf(item).path);
    // JSON last: an item only exists once its PDF is fully in place.
    await save(item);
    return item;
  }

  Future<void> save(QueuedItem item) async {
    final tmp = File('${dir.path}/${item.id}.json.tmp');
    await tmp.writeAsString(jsonEncode(item.toJson()), flush: true);
    await tmp.rename(_jsonOf(item.id).path);
  }

  Future<List<QueuedItem>> items() async {
    if (!await dir.exists()) return [];
    final out = <QueuedItem>[];
    await for (final f in dir.list()) {
      if (f is! File || !f.path.endsWith('.json')) continue;
      if (f.path.endsWith(_reportsFile)) continue;
      try {
        final item = QueuedItem.fromJson(jsonDecode(await f.readAsString()));
        if (item != null && await pdfOf(item).exists()) out.add(item);
      } on FormatException {
        // a half-written file; ignore it
      }
    }
    out.sort((a, b) {
      final byTime = a.queuedAt.compareTo(b.queuedAt);
      return byTime != 0 ? byTime : a.id.compareTo(b.id);
    });
    return out;
  }

  Future<List<QueuedItem>> waiting() async =>
      [for (final i in await items()) if (!i.failed) i];

  Future<void> remove(QueuedItem item) async {
    for (final f in [_jsonOf(item.id), pdfOf(item)]) {
      if (await f.exists()) await f.delete();
    }
  }

  Future<List<QueueReport>> reports() async {
    final f = File('${dir.path}/$_reportsFile');
    if (!await f.exists()) return [];
    try {
      final list = jsonDecode(await f.readAsString());
      if (list is! List) return [];
      return [for (final r in list) if (QueueReport.fromJson(r) case final rep?) rep];
    } on FormatException {
      return [];
    }
  }

  Future<void> addReport(QueueReport report) async {
    final all = [report, ...await reports()].take(maxReports).toList();
    await _writeReports(all);
  }

  Future<void> clearReports() => _writeReports(const []);

  Future<void> _writeReports(List<QueueReport> reports) async {
    await dir.create(recursive: true);
    final tmp = File('${dir.path}/$_reportsFile.tmp');
    await tmp.writeAsString(jsonEncode([for (final r in reports) r.toJson()]), flush: true);
    await tmp.rename('${dir.path}/$_reportsFile');
  }

  /// Runs [body] only if no other run (the app or the background job) holds
  /// the lock; returns null when it's taken. Creating the lock file is
  /// exclusive at the file-system level, so two runs can't both get it.
  Future<T?> withLock<T>(Future<T> Function() body) async {
    await dir.create(recursive: true);
    final lock = File('${dir.path}/$_lockFile');
    try {
      await lock.create(exclusive: true);
    } on FileSystemException {
      final age = DateTime.now().difference(await lock.lastModified());
      if (age < staleLock) return null;
      await lock.delete();
      try {
        await lock.create(exclusive: true);
      } on FileSystemException {
        return null;
      }
    }
    try {
      return await body();
    } finally {
      if (await lock.exists()) await lock.delete();
    }
  }
}
