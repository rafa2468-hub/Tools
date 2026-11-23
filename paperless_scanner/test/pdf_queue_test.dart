// Page images, PDF pages, upload details and the on-phone queue: no server.
import 'dart:convert';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as img;
import 'package:paperless_scanner/models/choices.dart';
import 'package:paperless_scanner/models/scan_session.dart';
import 'package:paperless_scanner/models/upload_meta.dart';
import 'package:paperless_scanner/services/image_prep.dart';
import 'package:paperless_scanner/services/pdf_service.dart';
import 'package:paperless_scanner/services/upload_queue.dart';

late Directory _tmp;

String _jpeg(String name, int w, int h) {
  final path = '${_tmp.path}/$name';
  File(path).writeAsBytesSync(img.encodeJpg(img.Image(width: w, height: h)));
  return path;
}

/// Stands in for the native compressor: copies the file and remembers the
/// rotation it was asked for.
class _FakeShrinker implements ImageShrinker {
  final calls = <(String, int)>[];
  @override
  Future<void> shrink(String source, String target, {required int rotateDegrees}) async {
    calls.add((source, rotateDegrees));
    await File(source).copy(target);
  }
}

List<List<double>> _mediaBoxes(File pdf) {
  final text = latin1.decode(pdf.readAsBytesSync());
  return [
    for (final m in RegExp(r'/MediaBox\s*\[([^\]]+)\]').allMatches(text))
      m.group(1)!.trim().split(RegExp(r'\s+')).map(double.parse).toList(),
  ];
}

void main() {
  setUpAll(() async {
    _tmp = await Directory.systemTemp.createTemp('pdf_queue_test');
  });
  tearDownAll(() async => _tmp.delete(recursive: true));

  group('imageSize', () {
    test('reads a JPEG', () {
      final p = _jpeg('size.jpg', 640, 480);
      expect(imageSize(File(p).readAsBytesSync()), (640, 480));
    });
    test('reads a PNG', () {
      final bytes = img.encodePng(img.Image(width: 30, height: 70));
      expect(imageSize(bytes), (30, 70));
    });
    test('null for anything else', () {
      expect(imageSize(utf8.encode('not an image')), isNull);
    });
  });

  group('shrinkBound (what flutter_image_compress gets)', () {
    // The plugin scales by max(1, min(w/b, h/b)); check the result directly.
    (int, int) scaled(int w, int h) {
      final b = shrinkBound(w, h, maxLongSidePx);
      final s = [1.0, [w / b, h / b].reduce((a, c) => a < c ? a : c)]
          .reduce((a, c) => a > c ? a : c);
      return ((w / s).round(), (h / s).round());
    }

    test('a 12 MP portrait page lands on 2200 px tall', () {
      final (w, h) = scaled(3000, 4000);
      expect(h, closeTo(2200, 2));
      expect(w, closeTo(1650, 2));
    });
    test('landscape works the same way', () {
      final (w, h) = scaled(4000, 3000);
      expect(w, closeTo(2200, 2));
      expect(h, closeTo(1650, 2));
    });
    test('a small image is left alone', () {
      expect(scaled(1200, 1600), (1200, 1600));
    });
    test('a long receipt keeps its shape', () {
      final (w, h) = scaled(1000, 6000);
      expect(h, closeTo(2200, 3));
      expect(w, closeTo(367, 3));
    });
  });

  group('pageFormatFor', () {
    test('portrait: 11 inches tall, width by the image shape', () {
      final f = pageFormatFor(1700, 2200);
      expect(f.height, closeTo(792, 0.01));
      expect(f.width, closeTo(612, 0.01)); // Letter, from a Letter-shaped scan
    });
    test('landscape: 11 inches wide', () {
      final f = pageFormatFor(2200, 1100);
      expect(f.width, closeTo(792, 0.01));
      expect(f.height, closeTo(396, 0.01));
    });
  });

  group('PdfService', () {
    test('one page per image, each sized to its image', () async {
      final a = _jpeg('a.jpg', 170, 220);
      final b = _jpeg('b.jpg', 220, 110);
      final out = await PdfService.buildPdfFromJpegs(
        [a, b],
        out: File('${_tmp.path}/two.pdf'),
        compress: false,
      );
      final boxes = _mediaBoxes(out);
      expect(boxes, hasLength(2));
      expect(boxes[0][2], closeTo(612, 0.5));
      expect(boxes[0][3], closeTo(792, 0.5));
      expect(boxes[1][2], closeTo(792, 0.5));
      expect(boxes[1][3], closeTo(396, 0.5));
    });

    test('keeps page order and passes each rotation to the shrinker', () async {
      final fake = _FakeShrinker();
      final p1 = _jpeg('p1.jpg', 100, 200);
      final p2 = _jpeg('p2.jpg', 100, 200);
      final pages = [ScanPage(p2, quarterTurns: 1), ScanPage(p1)];
      final pdf = await PdfService(shrinker: fake).buildPdf(
        pages,
        workDir: Directory('${_tmp.path}/work1'),
      );
      expect(fake.calls, [(p2, 90), (p1, 0)]);
      expect(pdf.existsSync(), isTrue);
      expect(latin1.decode(pdf.readAsBytesSync().take(5).toList()), '%PDF-');
    });
  });

  test('ScanSession tracks every image, deleted pages included', () {
    final s = ScanSession(imagePaths: ['a', 'b'], createdAt: DateTime(2026));
    s.pages.removeAt(0);
    s.addImages(['c']);
    expect(s.pages.map((p) => p.path), ['b', 'c']);
    expect(s.allPaths, {'a', 'b', 'c'});
    s.pages.first
      ..rotateClockwise()
      ..rotateClockwise()
      ..rotateClockwise()
      ..rotateClockwise();
    expect(s.pages.first.rotationDegrees, 0);
  });

  group('UploadMeta', () {
    test('form fields repeat tags and send the date at noon', () {
      final m = UploadMeta(
        title: '  Rachunek  ',
        tagIds: const [3, 5],
        correspondentId: 7,
        documentTypeId: 2,
        created: DateTime(2026, 3, 9, 23, 59),
      );
      expect(m.formFields.map((e) => '${e.key}=${e.value}'), [
        'title=Rachunek',
        'tags=3',
        'tags=5',
        'correspondent=7',
        'document_type=2',
        'created=2026-03-09T12:00:00',
      ]);
    });
    test('empty meta sends nothing', () {
      expect(const UploadMeta(title: ' ').formFields, isEmpty);
    });
    test('JSON round trip', () {
      final m = UploadMeta(
        title: 'X',
        tagIds: const [1],
        correspondentId: 4,
        created: DateTime(2026, 1, 2),
      );
      final back = UploadMeta.fromJson(jsonDecode(jsonEncode(m.toJson())) as Map<String, dynamic>);
      expect(back.formFields.map((e) => e.value), m.formFields.map((e) => e.value));
    });
  });

  test('Choices JSON round trip skips bad entries', () {
    final c = Choices.fromJson({
      'tags': [
        {'id': 1, 'name': 'Tax'},
        {'id': 'x', 'name': 'bad'},
      ],
      'correspondents': [
        {'id': 2, 'name': 'CRA'},
      ],
      'fetched_at': '2026-11-22T20:00:00.000',
    });
    final back = Choices.fromJson(jsonDecode(jsonEncode(c.toJson())) as Map<String, dynamic>);
    expect(back.tags.map((t) => t.name), ['Tax']);
    expect(back.correspondents.single.id, 2);
    expect(back.fetchedAt, DateTime(2026, 11, 22, 20));
    expect(Choices.nameOf(back.tags, 1), 'Tax');
    expect(Choices.nameOf(back.tags, 99), '#99');
  });

  group('UploadQueue', () {
    late UploadQueue q;
    late File pdf;
    setUp(() {
      q = UploadQueue(Directory('${_tmp.path}/queue_${DateTime.now().microsecondsSinceEpoch}'));
      pdf = File('${_tmp.path}/q.pdf')..writeAsStringSync('%PDF-1.4 q');
    });

    test('add, list in order, save, remove', () async {
      final a = await q.add(pdf, const UploadMeta(title: 'A'), 'A.pdf');
      await Future<void>.delayed(const Duration(milliseconds: 5));
      final b = await q.add(pdf, const UploadMeta(tagIds: [1]), 'b.pdf');
      expect((await q.items()).map((i) => i.id), [a.id, b.id]);
      expect(q.pdfOf(a).readAsStringSync(), '%PDF-1.4 q');
      expect(b.label, 'b.pdf');

      b
        ..failed = true
        ..lastError = 'nope';
      await q.save(b);
      expect((await q.waiting()).map((i) => i.id), [a.id]);
      expect((await q.items()).last.lastError, 'nope');

      await q.remove(a);
      expect((await q.items()).map((i) => i.id), [b.id]);
      expect(q.pdfOf(a).existsSync(), isFalse);
    });

    test('an entry without its PDF is ignored', () async {
      final a = await q.add(pdf, const UploadMeta(), 'a.pdf');
      q.pdfOf(a).deleteSync();
      expect(await q.items(), isEmpty);
    });

    test('reports: newest first, capped', () async {
      for (var i = 0; i < UploadQueue.maxReports + 3; i++) {
        await q.addReport(QueueReport(at: DateTime(2026, 1, 1, 0, i), label: '$i', outcome: 'imported'));
      }
      final r = await q.reports();
      expect(r, hasLength(UploadQueue.maxReports));
      expect(r.first.label, '${UploadQueue.maxReports + 2}');
      await q.clearReports();
      expect(await q.reports(), isEmpty);
    });

    test('the lock keeps a second run out, and frees up afterwards', () async {
      int? inner;
      final outer = await q.withLock(() async {
        inner = await q.withLock(() async => 2);
        return 1;
      });
      expect(outer, 1);
      expect(inner, isNull);
      expect(await q.withLock(() async => 3), 3);
    });

    test('a stale lock is taken over', () async {
      await q.dir.create(recursive: true);
      final lock = File('${q.dir.path}/.drain.lock')..createSync();
      lock.setLastModifiedSync(DateTime.now().subtract(const Duration(hours: 1)));
      expect(await q.withLock(() async => 'ran'), 'ran');
    });
  });
}
