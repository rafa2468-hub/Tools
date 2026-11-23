import 'dart:io';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import '../models/scan_session.dart';
import 'image_prep.dart';

/// Every page is as tall (or, for a landscape page, as wide) as a Letter
/// page, 11 inches, and the other side follows the scan's own shape, so a
/// receipt stays a narrow strip instead of floating on a white sheet.
const pageLongSidePt = 11 * PdfPageFormat.inch;

PdfPageFormat pageFormatFor(int widthPx, int heightPx) {
  if (widthPx <= 0 || heightPx <= 0) return PdfPageFormat.letter;
  if (widthPx >= heightPx) {
    return PdfPageFormat(pageLongSidePt, pageLongSidePt * heightPx / widthPx);
  }
  return PdfPageFormat(pageLongSidePt * widthPx / heightPx, pageLongSidePt);
}

class PdfService {
  PdfService({this.shrinker = const NativeImageShrinker()});

  final ImageShrinker shrinker;

  /// Builds the PDF for [pages] into [workDir] (created if missing):
  /// each page rotated and shrunk, then one PDF page per image.
  Future<File> buildPdf(
    List<ScanPage> pages, {
    required Directory workDir,
    String? title,
  }) async {
    await workDir.create(recursive: true);
    final prepared = <String>[];
    for (var i = 0; i < pages.length; i++) {
      final target = '${workDir.path}/page_${i + 1}.jpg';
      await shrinker.shrink(pages[i].path, target,
          rotateDegrees: pages[i].rotationDegrees);
      prepared.add(target);
    }
    return buildPdfFromJpegs(prepared, out: File('${workDir.path}/scan.pdf'), title: title);
  }

  /// One page per image, each page sized by [pageFormatFor].
  static Future<File> buildPdfFromJpegs(
    List<String> jpegs, {
    required File out,
    String? title,
    bool compress = true, // tests turn it off to read the page sizes back
  }) async {
    final doc = pw.Document(
      title: title ?? 'Scanned document',
      creator: 'Paperless Scanner',
      compress: compress,
    );
    for (final path in jpegs) {
      final bytes = await File(path).readAsBytes();
      final size = imageSize(bytes);
      final format = size == null ? PdfPageFormat.letter : pageFormatFor(size.$1, size.$2);
      final image = pw.MemoryImage(bytes);
      doc.addPage(
        pw.Page(
          pageFormat: format,
          margin: pw.EdgeInsets.zero,
          build: (_) => pw.Image(image, fit: pw.BoxFit.contain),
        ),
      );
    }
    await out.writeAsBytes(await doc.save());
    return out;
  }
}
