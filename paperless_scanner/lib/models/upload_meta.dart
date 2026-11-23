/// What goes to Paperless with the PDF. Every field is optional: whatever is
/// left out, Paperless fills in itself (matching rules, date from the text).
class UploadMeta {
  const UploadMeta({
    this.title,
    this.tagIds = const [],
    this.correspondentId,
    this.documentTypeId,
    this.created,
  });

  final String? title;
  final List<int> tagIds;
  final int? correspondentId;
  final int? documentTypeId;

  /// The document's own date. Only the day is used.
  final DateTime? created;

  String? get cleanTitle {
    final t = title?.trim();
    return (t == null || t.isEmpty) ? null : t;
  }

  /// The post_document form fields, in order. `tags` repeats, once per tag.
  List<MapEntry<String, String>> get formFields => [
        if (cleanTitle != null) MapEntry('title', cleanTitle!),
        for (final id in tagIds) MapEntry('tags', '$id'),
        if (correspondentId != null)
          MapEntry('correspondent', '$correspondentId'),
        if (documentTypeId != null)
          MapEntry('document_type', '$documentTypeId'),
        if (created != null) MapEntry('created', createdField(created!)),
      ];

  /// Paperless takes `created` as a date-time. Noon, with no time zone,
  /// keeps the day from shifting whatever zone the server runs in.
  static String createdField(DateTime d) => '${dateOnly(d)}T12:00:00';

  static String dateOnly(DateTime d) =>
      '${d.year.toString().padLeft(4, '0')}-'
      '${d.month.toString().padLeft(2, '0')}-'
      '${d.day.toString().padLeft(2, '0')}';

  Map<String, dynamic> toJson() => {
        'title': cleanTitle,
        'tags': tagIds,
        'correspondent': correspondentId,
        'document_type': documentTypeId,
        'created': created == null ? null : dateOnly(created!),
      };

  factory UploadMeta.fromJson(Map<String, dynamic> j) {
    final created = j['created'];
    return UploadMeta(
      title: j['title'] as String?,
      tagIds: [for (final t in (j['tags'] as List? ?? const [])) t as int],
      correspondentId: j['correspondent'] as int?,
      documentTypeId: j['document_type'] as int?,
      created: created is String ? DateTime.tryParse(created) : null,
    );
  }
}
