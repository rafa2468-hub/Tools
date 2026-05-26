import 'dart:io';
import 'dart:typed_data';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';

class PdfService {
  /// Combines one or more scanned image files into a single PDF.
  /// Each image becomes one page, sized to A4 with the image fitted inside.
  Future<File> buildPdf(List<String> imagePaths, {String? title}) async {
    final doc = pw.Document(title: title ?? 'Scanned Document');

    for (final path in imagePaths) {
      final bytes = await File(path).readAsBytes();
      final pdfImage = pw.MemoryImage(bytes);

      // Decode dimensions to choose portrait vs landscape
      final decoded = await _imageSize(bytes);
      final isLandscape = decoded != null && decoded.$1 > decoded.$2;
      final pageFormat =
          isLandscape ? PdfPageFormat.a4.landscape : PdfPageFormat.a4;

      doc.addPage(
        pw.Page(
          pageFormat: pageFormat,
          margin: pw.EdgeInsets.zero,
          build: (_) => pw.Center(
            child: pw.Image(pdfImage, fit: pw.BoxFit.contain),
          ),
        ),
      );
    }

    final dir = await getTemporaryDirectory();
    final timestamp = DateTime.now().millisecondsSinceEpoch;
    final file = File('${dir.path}/scan_$timestamp.pdf');
    await file.writeAsBytes(await doc.save());
    return file;
  }

  /// Returns (width, height) by reading the JPEG/PNG header bytes.
  Future<(int, int)?> _imageSize(Uint8List bytes) async {
    try {
      // JPEG: FF D8, dimensions in SOF0 (FF C0) or SOF2 (FF C2) marker
      if (bytes.length > 4 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        int i = 2;
        while (i < bytes.length - 8) {
          if (bytes[i] != 0xFF) break;
          final marker = bytes[i + 1];
          final len = (bytes[i + 2] << 8) | bytes[i + 3];
          if (marker == 0xC0 || marker == 0xC2) {
            final h = (bytes[i + 5] << 8) | bytes[i + 6];
            final w = (bytes[i + 7] << 8) | bytes[i + 8];
            return (w, h);
          }
          i += 2 + len;
        }
      }
      // PNG: 8-byte sig + IHDR chunk, width at offset 16, height at 20
      if (bytes.length > 24 &&
          bytes[0] == 0x89 &&
          bytes[1] == 0x50 &&
          bytes[2] == 0x4E &&
          bytes[3] == 0x47) {
        final w = (bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19];
        final h = (bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23];
        return (w, h);
      }
    } catch (_) {}
    return null;
  }
}
