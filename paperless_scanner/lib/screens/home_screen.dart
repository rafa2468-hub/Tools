import 'package:cunning_document_scanner/cunning_document_scanner.dart';
import 'package:material_ui/material_ui.dart';
import '../models/scan_session.dart';
import '../services/app_files.dart';
import '../services/background.dart';
import '../services/queue_runner.dart';
import '../services/storage_service.dart';
import '../services/upload_queue.dart';
import 'preview_screen.dart';
import 'queue_screen.dart';
import 'settings_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> with WidgetsBindingObserver {
  final _storage = StorageService();
  bool _isScanning = false;
  bool _isDraining = false;
  List<QueuedItem> _queued = const [];
  List<QueueReport> _reports = const [];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _startUp();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    // Back in the app, maybe back home: try what's waiting.
    if (state == AppLifecycleState.resumed && !_isScanning) _drain(quiet: true);
  }

  Future<void> _startUp() async {
    await AppFiles.cleanUpAtStart();
    await _refreshQueue();
    await _drain(quiet: true);
  }

  Future<void> _refreshQueue() async {
    final queue = await AppFiles.queue();
    final items = await queue.items();
    final reports = await queue.reports();
    if (!mounted) return;
    setState(() {
      _queued = items;
      _reports = reports;
    });
    if (items.any((i) => !i.failed)) await scheduleBackgroundUploads();
  }

  Future<void> _drain({required bool quiet}) async {
    if (_isDraining) return;
    final waiting = _queued.where((i) => !i.failed).length;
    if (waiting == 0 && quiet) return;
    setState(() => _isDraining = true);
    DrainSummary? summary;
    try {
      summary = await drainFromStoredSettings();
    } finally {
      if (mounted) setState(() => _isDraining = false);
    }
    await _refreshQueue();
    if (!mounted || summary == null) return;
    final message = _summaryText(summary);
    if (message != null && (!quiet || summary.didAnything)) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(message), duration: const Duration(seconds: 6)),
      );
    }
  }

  String? _summaryText(DrainSummary s) {
    final parts = <String>[
      if (s.imported > 0) '${s.imported} uploaded',
      if (s.duplicates > 0) '${s.duplicates} already in Paperless',
      if (s.failed > 0) '${s.failed} failed',
    ];
    if (s.stoppedBecause != null) {
      parts.add('${s.stillWaiting} still waiting: ${s.stoppedBecause}');
    }
    return parts.isEmpty ? null : parts.join('. ');
  }

  Future<void> _startScan() async {
    if (_isScanning) return;

    final configured = await _storage.isConfigured();
    if (!configured && mounted) {
      final go = await showDialog<bool>(
        context: context,
        builder: (ctx) => AlertDialog(
          icon: const Icon(Icons.settings_outlined),
          title: const Text('Setup required'),
          content: const Text(
            'Add your Paperless-ngx server URL and API token before scanning.',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Later'),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Open Settings'),
            ),
          ],
        ),
      );
      if (go == true && mounted) {
        await Navigator.push(
          context,
          MaterialPageRoute(builder: (_) => const SettingsScreen()),
        );
      }
      return;
    }

    setState(() => _isScanning = true);
    try {
      final result = await CunningDocumentScanner.scanDocuments(
        noOfPages: maxPages,
        scannerSource: ScannerSource.cameraAndGallery,
      );
      if (result == null || result.images.isEmpty) return; // cancelled

      final session = ScanSession(
        imagePaths: result.images,
        createdAt: DateTime.now(),
      );
      if (!mounted) return;
      if (result.discardedPages > 0) {
        _snack('${result.discardedPages} pages over the $maxPages-page limit were left out.');
      }
      final message = await Navigator.push<String>(
        context,
        MaterialPageRoute(builder: (_) => PreviewScreen(session: session)),
      );
      if (message != null) _snack(message);
      await _refreshQueue();
    } on CunningDocumentScannerException catch (e) {
      _snack('Scanner error: ${e.message}', error: true);
    } on Exception catch (e) {
      _snack('Scan error: $e', error: true);
    } finally {
      if (mounted) setState(() => _isScanning = false);
    }
  }

  void _snack(String message, {bool error = false}) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(message),
        duration: const Duration(seconds: 6),
        backgroundColor: error ? Theme.of(context).colorScheme.error : null,
      ),
    );
  }

  Future<void> _openQueue() async {
    await Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => const QueueScreen()),
    );
    await _refreshQueue();
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final waiting = _queued.where((i) => !i.failed).length;
    final failed = _queued.length - waiting;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Paperless Scanner'),
        actions: [
          IconButton(
            icon: const Icon(Icons.settings_outlined),
            tooltip: 'Settings',
            onPressed: () async {
              await Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const SettingsScreen()),
              );
              await _drain(quiet: true);
            },
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(24, 48, 24, 120),
        children: [
          Center(
            child: Container(
              width: 120,
              height: 120,
              decoration: BoxDecoration(
                color: cs.primaryContainer,
                borderRadius: BorderRadius.circular(32),
              ),
              child: Icon(
                Icons.document_scanner_outlined,
                size: 60,
                color: cs.onPrimaryContainer,
              ),
            ),
          ),
          const SizedBox(height: 32),
          Text('Scan & Upload', style: tt.headlineMedium, textAlign: TextAlign.center),
          const SizedBox(height: 12),
          Text(
            'Tap Scan to capture a document.\nEdges are detected automatically.',
            textAlign: TextAlign.center,
            style: tt.bodyLarge?.copyWith(color: cs.onSurface.withValues(alpha: 0.6)),
          ),
          if (_queued.isNotEmpty) ...[
            const SizedBox(height: 32),
            Card(
              child: ListTile(
                leading: Icon(
                  failed > 0 ? Icons.error_outline : Icons.cloud_queue,
                  color: failed > 0 ? cs.error : cs.primary,
                ),
                title: Text([
                  if (waiting > 0) '$waiting waiting to upload',
                  if (failed > 0) '$failed failed',
                ].join(', ')),
                subtitle: Text(waiting > 0
                    ? 'They upload by themselves on home Wi-Fi.'
                    : 'Open to retry or remove.'),
                trailing: _isDraining
                    ? const SizedBox(
                        width: 20,
                        height: 20,
                        child: CircularProgressIndicator(strokeWidth: 2.5),
                      )
                    : const Icon(Icons.chevron_right),
                onTap: _openQueue,
              ),
            ),
          ],
          if (_reports.isNotEmpty) ...[
            const SizedBox(height: 24),
            Row(
              children: [
                Expanded(child: Text('From the queue', style: tt.titleSmall)),
                TextButton(
                  onPressed: () async {
                    await (await AppFiles.queue()).clearReports();
                    await _refreshQueue();
                  },
                  child: const Text('Clear'),
                ),
              ],
            ),
            for (final r in _reports.take(5))
              ListTile(
                dense: true,
                contentPadding: EdgeInsets.zero,
                leading: Icon(
                  r.outcome == 'failed' ? Icons.error_outline : Icons.check_circle_outline,
                  color: r.outcome == 'failed' ? cs.error : cs.primary,
                ),
                title: Text(r.summary),
                subtitle: Text(_when(r.at)),
              ),
          ],
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _isScanning ? null : _startScan,
        icon: _isScanning
            ? const SizedBox(
                width: 20,
                height: 20,
                child: CircularProgressIndicator(strokeWidth: 2.5),
              )
            : const Icon(Icons.document_scanner),
        label: Text(_isScanning ? 'Opening scanner…' : 'Scan Document'),
      ),
      floatingActionButtonLocation: FloatingActionButtonLocation.centerFloat,
    );
  }
}

/// Up to this many pages per scan.
const maxPages = 20;

String _when(DateTime t) {
  final local = t.toLocal();
  String two(int n) => n.toString().padLeft(2, '0');
  return '${local.year}-${two(local.month)}-${two(local.day)} '
      '${two(local.hour)}:${two(local.minute)}';
}
