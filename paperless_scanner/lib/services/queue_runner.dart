import 'paperless_service.dart';
import 'task_result.dart';
import 'upload_queue.dart';

class DrainSummary {
  int imported = 0;
  int duplicates = 0;
  int failed = 0;
  int stillWaiting = 0;

  /// Why the run stopped early (server unreachable, bad token...), if it did.
  String? stoppedBecause;

  bool get didAnything => imported + duplicates + failed > 0;
}

/// Uploads every waiting scan, oldest first. Stops at the first sign the
/// server can't be used right now (unreachable, certificate, token), so a
/// phone away from home gives up after one quick try. Returns null if
/// another run already holds the queue.
Future<DrainSummary?> drainQueue(UploadQueue queue, PaperlessService paperless) {
  return queue.withLock(() async {
    final summary = DrainSummary();
    final waiting = await queue.waiting();
    for (var i = 0; i < waiting.length; i++) {
      final item = waiting[i];
      item
        ..attempts += 1
        ..lastTriedAt = DateTime.now();

      final up = await paperless.upload(
        queue.pdfOf(item),
        meta: item.meta,
        filename: item.filename,
      );

      if (!up.isQueued) {
        item.lastError = up.message;
        if (up.status == UploadStatus.serverError) {
          // Paperless answered and said no (bad field, too large...): retrying
          // the same request won't change that.
          item.failed = true;
          await queue.save(item);
          summary.failed++;
          await queue.addReport(QueueReport(
            at: DateTime.now(),
            label: item.label,
            outcome: 'failed',
            message: up.message,
          ));
          continue;
        }
        await queue.save(item);
        summary
          ..stoppedBecause = up.message
          ..stillWaiting = waiting.length - i;
        return summary;
      }

      final taskId = up.taskId;
      final outcome = taskId == null
          ? const TaskOutcome(TaskState.unknown)
          : await paperless.waitForTask(taskId);

      switch (outcome.state) {
        case TaskState.failed:
          item
            ..failed = true
            ..lastError = outcome.message ?? 'Paperless could not import it.';
          await queue.save(item);
          summary.failed++;
          await queue.addReport(QueueReport(
            at: DateTime.now(),
            label: item.label,
            outcome: 'failed',
            message: item.lastError,
          ));
        case TaskState.duplicate:
          await queue.remove(item);
          summary.duplicates++;
          await queue.addReport(QueueReport(
            at: DateTime.now(),
            label: item.label,
            outcome: 'duplicate',
            documentId: outcome.documentId,
          ));
        case TaskState.success || TaskState.processing || TaskState.unknown:
          // Paperless has the file; what's left is on its side.
          await queue.remove(item);
          summary.imported++;
          await queue.addReport(QueueReport(
            at: DateTime.now(),
            label: item.label,
            outcome: outcome.state == TaskState.success ? 'imported' : 'processing',
            documentId: outcome.documentId,
          ));
      }
    }
    return summary;
  });
}
