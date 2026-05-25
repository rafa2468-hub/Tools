import 'package:flutter/material.dart';
import 'package:google_mlkit_document_scanner/google_mlkit_document_scanner.dart';
import '../models/scan_session.dart';
import '../services/storage_service.dart';
import 'preview_screen.dart';
import 'settings_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _storage = StorageService();
  bool _isScanning = false;
  DocumentScanner? _scanner;

  @override
  void dispose() {
    _scanner?.close();
    super.dispose();
  }

  Future<void> _startScan() async {
    if (_isScanning) return;

    // Prompt to configure if not yet set up
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
      _scanner?.close();
      _scanner = DocumentScanner(
        options: DocumentScannerOptions(
          documentFormat: DocumentFormat.jpeg,
          mode: ScannerMode.full,   // ML-enhanced: dewarping, enhancement
          pageLimit: 20,
          isGalleryImport: true,    // also allow picking from gallery
        ),
      );

      final result = await _scanner!.scanDocument();

      if (result.images.isEmpty) return; // user cancelled

      final session = ScanSession(
        imagePaths: result.images,
        createdAt: DateTime.now(),
      );

      if (mounted) {
        await Navigator.push(
          context,
          MaterialPageRoute(builder: (_) => PreviewScreen(session: session)),
        );
      }
    } on Exception catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Scan error: $e'),
            backgroundColor: Theme.of(context).colorScheme.error,
          ),
        );
      }
    } finally {
      if (mounted) setState(() => _isScanning = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Paperless Scanner'),
        actions: [
          IconButton(
            icon: const Icon(Icons.settings_outlined),
            tooltip: 'Settings',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(builder: (_) => const SettingsScreen()),
            ),
          ),
        ],
      ),
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Container(
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
              const SizedBox(height: 32),
              Text('Scan & Upload', style: tt.headlineMedium),
              const SizedBox(height: 12),
              Text(
                'Tap Scan to capture a document.\nEdges are detected automatically.',
                textAlign: TextAlign.center,
                style: tt.bodyLarge?.copyWith(
                  color: cs.onSurface.withOpacity(0.6),
                ),
              ),
            ],
          ),
        ),
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
