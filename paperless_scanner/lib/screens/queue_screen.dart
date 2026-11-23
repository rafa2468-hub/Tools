import 'package:material_ui/material_ui.dart';
import 'package:share_plus/share_plus.dart';
import '../services/app_files.dart';
import '../services/background.dart';
import '../services/upload_queue.dart';

/// Scans that haven't reached Paperless yet.
class QueueScreen extends StatefulWidget {
  const QueueScreen({super.key});

  @override
  State<QueueScreen> createState() => _QueueScreenState();
}

class _QueueScreenState extends State<QueueScreen> {
  UploadQueue? _queue;
  List<QueuedItem> _items = const [];
  bool _busy = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final q = _queue ??= await AppFiles.queue();
    final items = await q.items();
    if (mounted) setState(() => _items = items);
  }

  Future<void> _uploadNow() async {
    setState(() => _busy = true);
    try {
      final s = await drainFromStoredSettings();
      if (!mounted) return;
      final text = s == null
          ? 'Another upload is already running, or Settings are incomplete.'
          : s.stoppedBecause != null
              ? 'Not reachable right now: ${s.stoppedBecause}'
              : 'Done: ${s.imported} uploaded, ${s.duplicates} already there, ${s.failed} failed.';
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(text), duration: const Duration(seconds: 6)),
      );
    } finally {
      if (mounted) setState(() => _busy = false);
      await _load();
    }
  }

  Future<void> _retry(QueuedItem item) async {
    item
      ..failed = false
      ..lastError = null;
    await _queue!.save(item);
    await scheduleBackgroundUploads();
    await _uploadNow();
  }

  Future<void> _delete(QueuedItem item) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Remove this scan?'),
        content: Text('"${item.label}" will be deleted from the phone and '
            'never reach Paperless. Share it first if you want a copy.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Keep')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Remove')),
        ],
      ),
    );
    if (ok != true) return;
    await _queue!.remove(item);
    await _load();
  }

  Future<void> _share(QueuedItem item) async {
    await SharePlus.instance.share(ShareParams(
      files: [XFile(_queue!.pdfOf(item).path, mimeType: 'application/pdf')],
      fileNameOverrides: [item.filename],
      subject: item.label,
    ));
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Waiting to upload'),
        actions: [
          IconButton(
            tooltip: 'Upload now',
            icon: _busy
                ? const SizedBox(
                    width: 20,
                    height: 20,
                    child: CircularProgressIndicator(strokeWidth: 2.5),
                  )
                : const Icon(Icons.cloud_upload_outlined),
            onPressed: _busy || _items.isEmpty ? null : _uploadNow,
          ),
        ],
      ),
      body: _items.isEmpty
          ? const Center(child: Text('Nothing waiting.'))
          : ListView.separated(
              itemCount: _items.length,
              separatorBuilder: (_, _) => const Divider(height: 1),
              itemBuilder: (_, i) {
                final item = _items[i];
                return ListTile(
                  leading: Icon(
                    item.failed ? Icons.error_outline : Icons.schedule,
                    color: item.failed ? cs.error : cs.primary,
                  ),
                  title: Text(item.label),
                  subtitle: Text([
                    'Scanned ${_when(item.queuedAt)}',
                    if (item.attempts > 0) 'tries: ${item.attempts}',
                    if (item.lastError != null) item.lastError!,
                  ].join('\n')),
                  isThreeLine: item.lastError != null,
                  trailing: PopupMenuButton<String>(
                    onSelected: (v) => switch (v) {
                      'retry' => _retry(item),
                      'share' => _share(item),
                      _ => _delete(item),
                    },
                    itemBuilder: (_) => [
                      if (item.failed)
                        const PopupMenuItem(value: 'retry', child: Text('Try again')),
                      const PopupMenuItem(value: 'share', child: Text('Share PDF')),
                      const PopupMenuItem(value: 'delete', child: Text('Remove')),
                    ],
                  ),
                );
              },
            ),
    );
  }
}

String _when(DateTime t) {
  final l = t.toLocal();
  String two(int n) => n.toString().padLeft(2, '0');
  return '${l.year}-${two(l.month)}-${two(l.day)} ${two(l.hour)}:${two(l.minute)}';
}
