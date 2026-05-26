class ScanSession {
  final List<String> imagePaths;
  final DateTime createdAt;
  String? title;

  ScanSession({
    required this.imagePaths,
    required this.createdAt,
    this.title,
  });

  int get pageCount => imagePaths.length;
}
