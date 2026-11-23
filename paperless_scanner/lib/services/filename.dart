String _two(int n) => n.toString().padLeft(2, '0');

/// File name for the uploaded PDF. Keeps letters and digits in any language
/// (Polish included), spaces, dashes and underscores; drops everything else.
/// Without a usable title it's `scan-YYYY-MM-DD-HHMM.pdf`, which Paperless
/// then also uses as the document's title.
String pdfFilename(String? title, DateTime when) {
  final cleaned = (title ?? '')
      .replaceAll(RegExp(r'[^\p{L}\p{N} _-]', unicode: true), '')
      .replaceAll(RegExp(r'\s+'), ' ')
      .trim();
  final base = cleaned.isEmpty
      ? 'scan-${when.year}-${_two(when.month)}-${_two(when.day)}-'
          '${_two(when.hour)}${_two(when.minute)}'
      : cleaned;
  final cut = base.length > 80 ? base.substring(0, 80).trim() : base;
  return '$cut.pdf';
}
