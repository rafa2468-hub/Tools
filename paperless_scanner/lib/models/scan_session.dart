/// One scanned page: the image the scanner wrote, plus how the user turned it.
class ScanPage {
  ScanPage(this.path, {this.quarterTurns = 0});

  final String path;

  /// Clockwise quarter turns (0-3), applied when the PDF is built.
  int quarterTurns;

  int get rotationDegrees => (quarterTurns % 4) * 90;

  void rotateClockwise() => quarterTurns = (quarterTurns + 1) % 4;
}

class ScanSession {
  ScanSession({required List<String> imagePaths, required this.createdAt}) {
    addImages(imagePaths);
  }

  /// In document order; the preview screen reorders, rotates and deletes.
  final List<ScanPage> pages = [];
  final DateTime createdAt;

  /// Every image this session ever held, deleted pages included, so all of
  /// them can be removed from storage when the session ends.
  final Set<String> allPaths = {};

  int get pageCount => pages.length;

  void addImages(List<String> paths) {
    pages.addAll(paths.map(ScanPage.new));
    allPaths.addAll(paths);
  }
}
