import 'dart:io';
import 'package:cunning_document_scanner/cunning_document_scanner.dart';
import 'package:material_ui/material_ui.dart';
import 'package:share_plus/share_plus.dart';
import '../models/choices.dart';
import '../models/scan_session.dart';
import '../models/upload_meta.dart';
import '../services/app_files.dart';
import '../services/background.dart';
import '../services/filename.dart';
import '../services/paperless_service.dart';
import '../services/pdf_service.dart';
import '../services/storage_service.dart';
import '../services/task_result.dart';
import 'home_screen.dart' show maxPages;
import 'scanner_error.dart';

/// Review the pages, add details, upload. Pops with a message for the home
/// screen when the scan is done with (uploaded, queued or discarded).
class PreviewScreen extends StatefulWidget {
  const PreviewScreen({super.key, required this.session});

  final ScanSession session;

  @override
  State<PreviewScreen> createState() => _PreviewScreenState();
}

class _PreviewScreenState extends State<PreviewScreen> {
  final _titleController = TextEditingController();
  final _pdfService = PdfService();

  ScanSession get _session => widget.session;

  bool _busy = false;
  bool _closing = false;
  String _status = '';

  Choices _choices = const Choices();
  bool _choicesLive = false;
  final Set<int> _tagIds = {};
  int? _correspondentId;
  int? _documentTypeId;
  DateTime? _created;

  @override
  void initState() {
    super.initState();
    _loadChoices();
  }

  @override
  void dispose() {
    _titleController.dispose();
    super.dispose();
  }

  // ---------------------------------------------------------------- choices

  Future<void> _loadChoices() async {
    final cached = await AppFiles.loadChoices();
    if (mounted) setState(() => _choices = cached);
    try {
      final config = await StorageService().load();
      if (!config.isComplete) return;
      final fresh = await PaperlessService(config).fetchChoices();
      await AppFiles.saveChoices(fresh);
      if (mounted) {
        setState(() {
          _choices = fresh;
          _choicesLive = true;
        });
      }
    } on Object {
      // Away from home: the cached lists (if any) stay.
    }
  }

  UploadMeta get _meta => UploadMeta(
        title: _titleController.text,
        tagIds: _tagIds.toList()..sort(),
        correspondentId: _correspondentId,
        documentTypeId: _documentTypeId,
        created: _created,
      );

  // ------------------------------------------------------------------ pages

  Future<void> _addPages() async {
    final room = maxPages - _session.pageCount;
    if (room <= 0) {
      _snack('This scan already has $maxPages pages, the most one upload takes.');
      return;
    }
    try {
      final result = await CunningDocumentScanner.scanDocuments(
        noOfPages: room,
        scannerSource: ScannerSource.cameraAndGallery,
      );
      if (result == null || result.images.isEmpty) return;
      setState(() => _session.addImages(result.images));
      if (result.discardedPages > 0) {
        _snack('${result.discardedPages} pages over the $maxPages-page limit were left out.');
      }
    } on Object catch (e) {
      if (mounted) await showScannerError(context, e);
    }
  }

  Future<void> _deletePage(int index) async {
    if (_session.pageCount == 1) {
      await _confirmDiscard();
      return;
    }
    setState(() => _session.pages.removeAt(index));
  }

  void _viewPage(ScanPage page) {
    showDialog<void>(
      context: context,
      builder: (ctx) => Dialog.fullscreen(
        child: Stack(
          children: [
            Positioned.fill(
              child: InteractiveViewer(
                maxScale: 6,
                child: Center(
                  child: RotatedBox(
                    quarterTurns: page.quarterTurns,
                    child: Image.file(File(page.path)),
                  ),
                ),
              ),
            ),
            Positioned(
              top: 8,
              right: 8,
              child: SafeArea(
                child: IconButton.filledTonal(
                  icon: const Icon(Icons.close),
                  onPressed: () => Navigator.pop(ctx),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  // ----------------------------------------------------- finishing the scan

  /// Removes everything this scan put on the phone and leaves the screen.
  Future<void> _close(String? message) async {
    _closing = true;
    await AppFiles.deleteFiles(_session.allPaths);
    if (mounted) Navigator.pop(context, message);
  }

  Future<void> _confirmDiscard() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Discard this scan?'),
        content: const Text('The pages will be deleted from the phone.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Keep')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Discard')),
        ],
      ),
    );
    if (ok == true) await _close(null);
  }

  void _setStatus(String s) {
    if (mounted) setState(() => _status = s);
  }

  Future<void> _upload() async {
    final meta = _meta;
    final filename = pdfFilename(meta.cleanTitle, _session.createdAt);
    setState(() {
      _busy = true;
      _status = 'Building PDF…';
    });
    Directory? work;
    try {
      final config = await StorageService().load();
      if (!config.isComplete) {
        _showMessage('Setup needed', 'Open Settings and save the server URL and API token first.');
        return;
      }
      work = await AppFiles.newWorkDir();
      final pdf = await _pdfService.buildPdf(_session.pages, workDir: work, title: meta.cleanTitle);

      _setStatus('Uploading to Paperless-ngx…');
      final paperless = PaperlessService(config);
      final up = await paperless.upload(pdf, meta: meta, filename: filename);
      if (!mounted) return;

      if (up.isUnreachable) {
        final queue = await AppFiles.queue();
        await queue.add(pdf, meta, filename);
        await scheduleBackgroundUploads();
        await _close("Can't reach Paperless right now, so the scan is saved on "
            'the phone. It uploads by itself on home Wi-Fi.');
        return;
      }
      if (!up.isQueued) {
        _showMessage('Upload failed', up.message ?? 'Upload failed.');
        return;
      }
      final taskId = up.taskId;
      if (taskId == null) {
        await _close("Uploaded. Paperless sent no task ID, so the app can't "
            'confirm the import. Check Paperless.');
        return;
      }

      _setStatus('Uploaded. Waiting for Paperless to import it…');
      final outcome = await paperless.waitForTask(taskId);
      if (!mounted) return;

      switch (outcome.state) {
        case TaskState.success:
          await _close(outcome.documentId == null
              ? 'Imported into Paperless.'
              : 'Imported into Paperless as document #${outcome.documentId}.');
        case TaskState.duplicate:
          _showMessage(
            'Already in Paperless',
            'Paperless says this is a duplicate of document '
                '#${outcome.documentId}${outcome.inTrash ? ' (in its trash)' : ''}. '
                'Nothing new was added. Go back to discard this scan.',
          );
        case TaskState.failed:
          _showMessage("Paperless couldn't import it", outcome.message ?? 'The import failed.');
        case TaskState.processing:
          await _close('Uploaded. Paperless is still working on it; check its '
              'Tasks page in a minute.');
        case TaskState.unknown:
          await _close(outcome.message ?? 'Uploaded, but the result is unknown.');
      }
    } on Exception catch (e) {
      _showMessage('Upload failed', e.toString());
    } finally {
      await AppFiles.deleteDir(work);
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _sharePdf() async {
    setState(() {
      _busy = true;
      _status = 'Building PDF…';
    });
    try {
      final meta = _meta;
      // The work folder stays until the next app start: the app receiving
      // the share may still be reading the file.
      final work = await AppFiles.newWorkDir();
      final pdf = await _pdfService.buildPdf(_session.pages, workDir: work, title: meta.cleanTitle);
      if (!mounted) return;
      await SharePlus.instance.share(ShareParams(
        files: [XFile(pdf.path, mimeType: 'application/pdf')],
        fileNameOverrides: [pdfFilename(meta.cleanTitle, _session.createdAt)],
        subject: meta.cleanTitle ?? 'Scanned document',
      ));
    } on Exception catch (e) {
      _showMessage('Share failed', e.toString());
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  void _snack(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
  }

  /// A dialog that keeps the scan on screen, so nothing is lost.
  void _showMessage(String title, String message) {
    if (!mounted) return;
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        icon: Icon(Icons.error_outline, color: Theme.of(ctx).colorScheme.error),
        title: Text(title),
        content: SingleChildScrollView(child: Text(message)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('OK')),
        ],
      ),
    );
  }

  // ---------------------------------------------------------------- details

  Future<void> _pickTags() async {
    final picked = await showModalBottomSheet<Set<int>>(
      context: context,
      isScrollControlled: true,
      showDragHandle: true,
      builder: (_) => _TagPicker(tags: _choices.tags, selected: _tagIds),
    );
    if (picked != null) {
      setState(() {
        _tagIds
          ..clear()
          ..addAll(picked);
      });
    }
  }

  Future<void> _pickDate() async {
    final now = DateTime.now();
    final d = await showDatePicker(
      context: context,
      initialDate: _created ?? now,
      firstDate: DateTime(1900),
      lastDate: DateTime(now.year + 1, 12, 31),
      helpText: "Document's date",
    );
    if (d != null) setState(() => _created = d);
  }

  Widget _details(BuildContext context) {
    final tt = Theme.of(context).textTheme;
    final cs = Theme.of(context).colorScheme;
    final noLists = _choices.isEmpty;

    DropdownButtonFormField<int?> dropdown({
      required String label,
      required IconData icon,
      required List<NamedItem> items,
      required int? value,
      required ValueChanged<int?> onChanged,
    }) {
      // A saved choice the server no longer has still shows, as its number.
      final known = value == null || items.any((i) => i.id == value);
      return DropdownButtonFormField<int?>(
        initialValue: value,
        isExpanded: true,
        decoration: InputDecoration(
          labelText: label,
          prefixIcon: Icon(icon),
          border: const OutlineInputBorder(),
        ),
        items: [
          const DropdownMenuItem<int?>(value: null, child: Text('Let Paperless decide')),
          for (final i in items) DropdownMenuItem<int?>(value: i.id, child: Text(i.name)),
          if (!known) DropdownMenuItem<int?>(value: value, child: Text('#$value')),
        ],
        onChanged: _busy ? null : onChanged,
      );
    }

    return ExpansionTile(
      tilePadding: EdgeInsets.zero,
      leading: const Icon(Icons.tune),
      title: const Text('Details'),
      subtitle: Text(
        [
          if (_correspondentId != null)
            Choices.nameOf(_choices.correspondents, _correspondentId),
          if (_documentTypeId != null) Choices.nameOf(_choices.documentTypes, _documentTypeId),
          if (_tagIds.isNotEmpty) '${_tagIds.length} tags',
          if (_created != null) UploadMeta.dateOnly(_created!),
        ].whereType<String>().join(' · ').ifEmpty('Optional: tags, correspondent, type, date'),
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
      ),
      childrenPadding: const EdgeInsets.only(bottom: 8),
      children: [
        if (noLists)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: Text(
              "Tags, correspondents and types load from Paperless; they'll show "
              'here once the phone has reached it once.',
              style: tt.bodySmall?.copyWith(color: cs.onSurfaceVariant),
            ),
          )
        else if (!_choicesLive && _choices.fetchedAt != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: Text(
              'Lists as of ${UploadMeta.dateOnly(_choices.fetchedAt!)} (Paperless not reachable now).',
              style: tt.bodySmall?.copyWith(color: cs.onSurfaceVariant),
            ),
          ),
        dropdown(
          label: 'Correspondent',
          icon: Icons.person_outline,
          items: _choices.correspondents,
          value: _correspondentId,
          onChanged: (v) => setState(() => _correspondentId = v),
        ),
        const SizedBox(height: 12),
        dropdown(
          label: 'Document type',
          icon: Icons.description_outlined,
          items: _choices.documentTypes,
          value: _documentTypeId,
          onChanged: (v) => setState(() => _documentTypeId = v),
        ),
        const SizedBox(height: 4),
        ListTile(
          contentPadding: EdgeInsets.zero,
          leading: const Icon(Icons.sell_outlined),
          title: _tagIds.isEmpty
              ? const Text('Tags: let Paperless decide')
              : Wrap(
                  spacing: 6,
                  runSpacing: 6,
                  children: [
                    for (final id in _tagIds)
                      InputChip(
                        label: Text(Choices.nameOf(_choices.tags, id)!),
                        onDeleted: _busy ? null : () => setState(() => _tagIds.remove(id)),
                      ),
                  ],
                ),
          trailing: const Icon(Icons.edit_outlined),
          onTap: _busy || _choices.tags.isEmpty ? null : _pickTags,
        ),
        ListTile(
          contentPadding: EdgeInsets.zero,
          leading: const Icon(Icons.event_outlined),
          title: Text(_created == null
              ? 'Date: let Paperless decide'
              : 'Date: ${UploadMeta.dateOnly(_created!)}'),
          trailing: _created == null
              ? const Icon(Icons.edit_outlined)
              : IconButton(
                  tooltip: 'Clear the date',
                  icon: const Icon(Icons.clear),
                  onPressed: _busy ? null : () => setState(() => _created = null),
                ),
          onTap: _busy ? null : _pickDate,
        ),
      ],
    );
  }

  // ------------------------------------------------------------------- build

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final pages = _session.pages;

    return PopScope(
      canPop: _closing,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop && !_busy) _confirmDiscard();
      },
      child: Scaffold(
        appBar: AppBar(
          title: Text('${pages.length} ${pages.length == 1 ? 'page' : 'pages'}'),
          actions: [
            IconButton(
              icon: const Icon(Icons.share_outlined),
              tooltip: 'Share as PDF',
              onPressed: _busy ? null : _sharePdf,
            ),
          ],
        ),
        body: ReorderableListView.builder(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
          buildDefaultDragHandles: false,
          itemCount: pages.length,
          // onReorderItem's target index already allows for the removed item.
          onReorderItem: (from, to) {
            if (_busy) return;
            setState(() => pages.insert(to, pages.removeAt(from)));
          },
          itemBuilder: (context, i) {
            final page = pages[i];
            return Card(
              key: ObjectKey(page),
              margin: const EdgeInsets.symmetric(vertical: 4),
              child: ListTile(
                contentPadding: const EdgeInsets.fromLTRB(8, 4, 0, 4),
                leading: GestureDetector(
                  onTap: () => _viewPage(page),
                  child: SizedBox(
                    width: 48,
                    height: 64,
                    child: ClipRRect(
                      borderRadius: BorderRadius.circular(4),
                      child: RotatedBox(
                        quarterTurns: page.quarterTurns,
                        child: Image.file(
                          File(page.path),
                          fit: BoxFit.cover,
                          cacheWidth: 200,
                          errorBuilder: (_, _, _) => Container(
                            color: cs.surfaceContainerHigh,
                            child: const Icon(Icons.broken_image_outlined),
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
                title: Text('Page ${i + 1}'),
                onTap: () => _viewPage(page),
                trailing: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    IconButton(
                      tooltip: 'Rotate',
                      icon: const Icon(Icons.rotate_right),
                      onPressed: _busy ? null : () => setState(page.rotateClockwise),
                    ),
                    IconButton(
                      tooltip: 'Delete page',
                      icon: const Icon(Icons.delete_outline),
                      onPressed: _busy ? null : () => _deletePage(i),
                    ),
                    ReorderableDragStartListener(
                      index: i,
                      enabled: !_busy,
                      child: const Padding(
                        padding: EdgeInsets.all(12),
                        child: Icon(Icons.drag_handle),
                      ),
                    ),
                  ],
                ),
              ),
            );
          },
          footer: Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                OutlinedButton.icon(
                  onPressed: _busy || pages.length >= maxPages ? null : _addPages,
                  icon: const Icon(Icons.add_a_photo_outlined),
                  label: const Text('Add pages'),
                ),
                const SizedBox(height: 16),
                TextField(
                  controller: _titleController,
                  decoration: const InputDecoration(
                    labelText: 'Document title (optional)',
                    prefixIcon: Icon(Icons.title),
                    border: OutlineInputBorder(),
                  ),
                  textCapitalization: TextCapitalization.sentences,
                  enabled: !_busy,
                ),
                const SizedBox(height: 8),
                _details(context),
              ],
            ),
          ),
        ),
        bottomNavigationBar: SafeArea(
          child: Container(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 12),
            decoration: BoxDecoration(
              color: cs.surfaceContainerLow,
              border: Border(top: BorderSide(color: cs.outlineVariant)),
            ),
            child: _busy
                ? Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      LinearProgressIndicator(borderRadius: BorderRadius.circular(4)),
                      const SizedBox(height: 8),
                      Text(_status, style: Theme.of(context).textTheme.bodySmall),
                    ],
                  )
                : FilledButton.icon(
                    onPressed: _upload,
                    icon: const Icon(Icons.cloud_upload_outlined),
                    label: const Text('Upload to Paperless-ngx'),
                    style: FilledButton.styleFrom(
                      padding: const EdgeInsets.symmetric(vertical: 14),
                    ),
                  ),
          ),
        ),
      ),
    );
  }
}

extension on String {
  String ifEmpty(String other) => isEmpty ? other : this;
}

/// Multi-select of tags with a search box.
class _TagPicker extends StatefulWidget {
  const _TagPicker({required this.tags, required this.selected});

  final List<NamedItem> tags;
  final Set<int> selected;

  @override
  State<_TagPicker> createState() => _TagPickerState();
}

class _TagPickerState extends State<_TagPicker> {
  late final Set<int> _picked = {...widget.selected};
  String _filter = '';

  @override
  Widget build(BuildContext context) {
    final f = _filter.toLowerCase();
    final shown = [
      for (final t in widget.tags)
        if (f.isEmpty || t.name.toLowerCase().contains(f)) t,
    ];
    return Padding(
      padding: EdgeInsets.only(bottom: MediaQuery.viewInsetsOf(context).bottom),
      child: SizedBox(
        height: MediaQuery.sizeOf(context).height * 0.7,
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
              child: TextField(
                autofocus: false,
                decoration: const InputDecoration(
                  hintText: 'Search tags',
                  prefixIcon: Icon(Icons.search),
                  border: OutlineInputBorder(),
                ),
                onChanged: (v) => setState(() => _filter = v),
              ),
            ),
            Expanded(
              child: ListView(
                children: [
                  for (final t in shown)
                    CheckboxListTile(
                      value: _picked.contains(t.id),
                      title: Text(t.name),
                      onChanged: (on) => setState(() {
                        if (on == true) {
                          _picked.add(t.id);
                        } else {
                          _picked.remove(t.id);
                        }
                      }),
                    ),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.all(16),
              child: Row(
                children: [
                  TextButton(
                    onPressed: () => setState(_picked.clear),
                    child: const Text('Clear'),
                  ),
                  const Spacer(),
                  FilledButton(
                    onPressed: () => Navigator.pop(context, _picked),
                    child: Text('Use ${_picked.length} tags'),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
