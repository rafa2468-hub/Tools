import 'dart:io';
import 'package:flutter/material.dart';
import 'package:share_plus/share_plus.dart';
import '../models/scan_session.dart';
import '../services/paperless_service.dart';
import '../services/pdf_service.dart';

class PreviewScreen extends StatefulWidget {
  const PreviewScreen({super.key, required this.session});

  final ScanSession session;

  @override
  State<PreviewScreen> createState() => _PreviewScreenState();
}

class _PreviewScreenState extends State<PreviewScreen> {
  final _titleController = TextEditingController();
  final _paperless = PaperlessService();
  final _pdfService = PdfService();

  bool _isUploading = false;
  String _statusMessage = '';

  @override
  void dispose() {
    _titleController.dispose();
    super.dispose();
  }

  Future<File> _buildPdf() => _pdfService.buildPdf(
        widget.session.imagePaths,
        title: _titleController.text.trim().isNotEmpty
            ? _titleController.text.trim()
            : null,
      );

  Future<void> _upload() async {
    setState(() {
      _isUploading = true;
      _statusMessage = 'Building PDF…';
    });

    try {
      final pdf = await _buildPdf();

      setState(() => _statusMessage = 'Uploading to Paperless-ngx…');

      final result = await _paperless.uploadDocument(
        pdf,
        title: _titleController.text.trim().isNotEmpty
            ? _titleController.text.trim()
            : null,
      );

      if (!mounted) return;

      if (result.isSuccess) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Uploaded successfully!'),
            backgroundColor: Colors.green,
          ),
        );
        Navigator.pop(context);
      } else {
        _showError(result.message ?? 'Upload failed.');
      }
    } on Exception catch (e) {
      _showError(e.toString());
    } finally {
      if (mounted) setState(() => _isUploading = false);
    }
  }

  Future<void> _sharePdf() async {
    setState(() {
      _isUploading = true;
      _statusMessage = 'Building PDF…';
    });
    try {
      final pdf = await _buildPdf();
      if (!mounted) return;
      await Share.shareXFiles(
        [XFile(pdf.path, mimeType: 'application/pdf')],
        subject: _titleController.text.trim().isNotEmpty
            ? _titleController.text.trim()
            : 'Scanned Document',
      );
    } on Exception catch (e) {
      _showError(e.toString());
    } finally {
      if (mounted) setState(() => _isUploading = false);
    }
  }

  void _showError(String message) {
    if (!mounted) return;
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        icon: Icon(Icons.error_outline,
            color: Theme.of(ctx).colorScheme.error),
        title: const Text('Upload failed'),
        content: Text(message),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('OK'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final pages = widget.session.imagePaths;

    return Scaffold(
      appBar: AppBar(
        title: Text(
          '${pages.length} ${pages.length == 1 ? 'Page' : 'Pages'} scanned',
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.share_outlined),
            tooltip: 'Share as PDF',
            onPressed: _isUploading ? null : _sharePdf,
          ),
        ],
      ),
      body: Column(
        children: [
          // Page thumbnails
          Expanded(
            child: GridView.builder(
              padding: const EdgeInsets.all(12),
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 3,
                crossAxisSpacing: 8,
                mainAxisSpacing: 8,
                childAspectRatio: 0.75, // portrait ratio
              ),
              itemCount: pages.length,
              itemBuilder: (_, index) => _PageThumbnail(
                imagePath: pages[index],
                pageNumber: index + 1,
              ),
            ),
          ),

          // Title + upload controls
          Container(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
            decoration: BoxDecoration(
              color: cs.surfaceContainerLow,
              border: Border(top: BorderSide(color: cs.outlineVariant)),
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                TextField(
                  controller: _titleController,
                  decoration: const InputDecoration(
                    labelText: 'Document title (optional)',
                    prefixIcon: Icon(Icons.title),
                    border: OutlineInputBorder(),
                  ),
                  textCapitalization: TextCapitalization.words,
                  enabled: !_isUploading,
                ),
                const SizedBox(height: 12),
                if (_isUploading) ...[
                  LinearProgressIndicator(borderRadius: BorderRadius.circular(4)),
                  const SizedBox(height: 8),
                  Text(
                    _statusMessage,
                    textAlign: TextAlign.center,
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                ] else
                  FilledButton.icon(
                    onPressed: _upload,
                    icon: const Icon(Icons.cloud_upload_outlined),
                    label: const Text('Upload to Paperless-ngx'),
                    style: FilledButton.styleFrom(
                      padding: const EdgeInsets.symmetric(vertical: 14),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _PageThumbnail extends StatelessWidget {
  const _PageThumbnail({required this.imagePath, required this.pageNumber});

  final String imagePath;
  final int pageNumber;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Stack(
      fit: StackFit.expand,
      children: [
        ClipRRect(
          borderRadius: BorderRadius.circular(8),
          child: Image.file(
            File(imagePath),
            fit: BoxFit.cover,
            errorBuilder: (_, __, ___) => Container(
              color: cs.surfaceContainerHigh,
              child: const Icon(Icons.broken_image_outlined),
            ),
          ),
        ),
        Positioned(
          bottom: 4,
          right: 4,
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
            decoration: BoxDecoration(
              color: cs.inverseSurface.withOpacity(0.75),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Text(
              '$pageNumber',
              style: TextStyle(
                color: cs.onInverseSurface,
                fontSize: 11,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
        ),
      ],
    );
  }
}
