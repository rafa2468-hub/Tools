import 'dart:io';
import 'dart:typed_data';
import 'package:flutter_image_compress/flutter_image_compress.dart';

/// Longest side of a page image in the PDF. 2200 px over an 11-inch page
/// is 200 DPI: sharp for text, a fraction of a raw camera frame's size.
const maxLongSidePx = 2200;

/// JPEG quality for page images.
const jpegQuality = 75;

/// Turns a scanner image into the page image that goes into the PDF:
/// rotated as the user chose, shrunk, re-encoded as JPEG.
abstract class ImageShrinker {
  Future<void> shrink(String source, String target, {required int rotateDegrees});
}

class NativeImageShrinker implements ImageShrinker {
  const NativeImageShrinker();

  @override
  Future<void> shrink(String source, String target, {required int rotateDegrees}) async {
    final size = imageSize(await File(source).readAsBytes());
    final bound = size == null
        ? maxLongSidePx
        : shrinkBound(size.$1, size.$2, maxLongSidePx);
    final out = await FlutterImageCompress.compressAndGetFile(
      source,
      target,
      // The plugin scales so both sides stay at or above these minimums;
      // a square bound makes that "short side = bound", see [shrinkBound].
      minWidth: bound,
      minHeight: bound,
      quality: jpegQuality,
      rotate: rotateDegrees,
      format: CompressFormat.jpeg,
    );
    if (out == null) throw FileSystemException('Could not shrink the page', source);
  }
}

/// flutter_image_compress scales by max(1, min(w / minW, h / minH)). With
/// minW = minH = this bound, that is shortSide / bound, which comes out as
/// longSide / maxLong: the long side lands on [maxLong], and smaller
/// images are left at their size.
int shrinkBound(int width, int height, int maxLong) {
  final long = width > height ? width : height;
  final short = width > height ? height : width;
  if (long <= maxLong) return short < 1 ? 1 : short;
  final b = (short * maxLong / long).round();
  return b < 1 ? 1 : b;
}

/// (width, height) from a JPEG or PNG header, or null.
(int, int)? imageSize(Uint8List bytes) {
  try {
    // JPEG: FF D8, size in a start-of-frame marker (C0-CF, except C4/C8/CC).
    if (bytes.length > 4 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
      var i = 2;
      while (i + 9 < bytes.length) {
        if (bytes[i] != 0xFF) return null;
        final marker = bytes[i + 1];
        if (marker == 0xFF) {
          i++; // fill byte
          continue;
        }
        final len = (bytes[i + 2] << 8) | bytes[i + 3];
        final isFrame = marker >= 0xC0 &&
            marker <= 0xCF &&
            marker != 0xC4 &&
            marker != 0xC8 &&
            marker != 0xCC;
        if (isFrame) {
          final h = (bytes[i + 5] << 8) | bytes[i + 6];
          final w = (bytes[i + 7] << 8) | bytes[i + 8];
          return (w, h);
        }
        i += 2 + len;
      }
    }
    // PNG: signature, then IHDR with width at 16 and height at 20.
    if (bytes.length > 24 &&
        bytes[0] == 0x89 &&
        bytes[1] == 0x50 &&
        bytes[2] == 0x4E &&
        bytes[3] == 0x47) {
      final w = (bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19];
      final h = (bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23];
      return (w, h);
    }
  } on RangeError {
    return null;
  }
  return null;
}
