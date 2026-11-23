/// A tag, correspondent or document type as Paperless lists it.
class NamedItem {
  const NamedItem(this.id, this.name);

  final int id;
  final String name;

  Map<String, dynamic> toJson() => {'id': id, 'name': name};

  static NamedItem? fromJson(Object? j) {
    if (j is! Map) return null;
    final id = j['id'];
    final name = j['name'];
    if (id is! int || name is! String) return null;
    return NamedItem(id, name);
  }
}

/// The pick-lists for the upload details, as last fetched from Paperless.
class Choices {
  const Choices({
    this.tags = const [],
    this.correspondents = const [],
    this.documentTypes = const [],
    this.fetchedAt,
  });

  final List<NamedItem> tags;
  final List<NamedItem> correspondents;
  final List<NamedItem> documentTypes;
  final DateTime? fetchedAt;

  bool get isEmpty =>
      tags.isEmpty && correspondents.isEmpty && documentTypes.isEmpty;

  Map<String, dynamic> toJson() => {
        'tags': [for (final t in tags) t.toJson()],
        'correspondents': [for (final c in correspondents) c.toJson()],
        'document_types': [for (final d in documentTypes) d.toJson()],
        'fetched_at': fetchedAt?.toIso8601String(),
      };

  factory Choices.fromJson(Map<String, dynamic> j) {
    List<NamedItem> list(String key) => [
          for (final e in (j[key] as List? ?? const []))
            ?NamedItem.fromJson(e),
        ];
    final at = j['fetched_at'];
    return Choices(
      tags: list('tags'),
      correspondents: list('correspondents'),
      documentTypes: list('document_types'),
      fetchedAt: at is String ? DateTime.tryParse(at) : null,
    );
  }

  static String? nameOf(List<NamedItem> items, int? id) {
    if (id == null) return null;
    for (final i in items) {
      if (i.id == id) return i.name;
    }
    return '#$id';
  }
}
