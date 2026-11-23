/// What Paperless did with an uploaded document.
enum TaskState { processing, success, duplicate, failed, unknown }

class TaskOutcome {
  const TaskOutcome(
    this.state, {
    this.documentId,
    this.inTrash = false,
    this.message,
  });

  final TaskState state;

  /// The new document (success) or the existing one (duplicate).
  final int? documentId;

  /// Duplicate only: the existing document is in Paperless's trash.
  final bool inTrash;

  final String? message;
}

/// Picks the task out of a `/api/tasks/?task_id=` response: paginated
/// (API v10) or a bare list (older API versions).
Map<String, dynamic>? taskFromResponse(Object? body) {
  List<dynamic>? items;
  if (body is Map && body['results'] is List) {
    items = body['results'] as List<dynamic>;
  } else if (body is List) {
    items = body;
  }
  if (items == null || items.isEmpty) return null;
  final first = items.first;
  return first is Map ? Map<String, dynamic>.from(first) : null;
}

int? _asInt(Object? v) =>
    v is int ? v : (v is String ? int.tryParse(v) : null);

/// Reads a task the way Paperless's own web UI does (tasks.component.ts,
/// v3.5.1): `document_id` = imported, `duplicate_of` = duplicate,
/// `error_message` / `reason` = why it failed. Also accepts the older
/// API v9 shape (`SUCCESS`, `result`, `related_document`).
TaskOutcome interpretTask(Map<String, dynamic> task) {
  final status = (task['status'] ?? '').toString().toLowerCase();
  final rawData = task['result_data'];
  final data = rawData is Map
      ? Map<String, dynamic>.from(rawData)
      : const <String, dynamic>{};
  final legacyResult = task['result'] is String ? task['result'] as String : null;

  int? relatedId() {
    final ids = task['related_document_ids'];
    if (ids is List && ids.isNotEmpty) return _asInt(ids.first);
    return _asInt(task['related_document']);
  }

  switch (status) {
    case 'pending' || 'started' || 'retry' || 'received':
      return const TaskOutcome(TaskState.processing);
    case 'success':
      return TaskOutcome(
        TaskState.success,
        documentId: _asInt(data['document_id']) ?? relatedId(),
      );
    default:
      final duplicateOf = _asInt(data['duplicate_of']);
      if (duplicateOf != null) {
        return TaskOutcome(
          TaskState.duplicate,
          documentId: duplicateOf,
          inTrash: data['duplicate_in_trash'] == true,
        );
      }
      final message = data['error_message'] ?? data['reason'] ?? legacyResult;
      return TaskOutcome(
        TaskState.failed,
        message: message?.toString() ?? 'Paperless reported status "$status".',
      );
  }
}
