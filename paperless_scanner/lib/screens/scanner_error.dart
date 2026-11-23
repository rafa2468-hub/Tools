import 'package:cunning_document_scanner/cunning_document_scanner.dart';
import 'package:material_ui/material_ui.dart';
import 'package:share_plus/share_plus.dart';

/// Error code the patched scanner plugin (third_party/cunning_document_scanner)
/// returns when Google's document scanner cannot start. The stock plugin
/// silently opens a manual camera instead, which hides the reason.
const scannerUnavailableCode = 'SCANNER_UNAVAILABLE';

/// The text shown for a scanner failure: what went wrong in plain words,
/// then the exact error so it can be diagnosed.
String scannerErrorText(Object error) {
  if (error is CunningDocumentScannerException) {
    if (error.code == scannerUnavailableCode) {
      return "Google's document scanner couldn't start.\n\n"
          'Error: ${error.message}';
    }
    return 'The scanner failed (${error.code ?? 'error'}).\n\n'
        'Error: ${error.message}';
  }
  return 'The scanner failed.\n\nError: $error';
}

/// Shows a scanner failure in a dialog with selectable text and a Share
/// button, so the exact error can be sent on instead of retyped.
Future<void> showScannerError(BuildContext context, Object error) {
  final text = scannerErrorText(error);
  return showDialog<void>(
    context: context,
    builder: (ctx) => AlertDialog(
      icon: const Icon(Icons.error_outline),
      title: const Text('Scanner problem'),
      content: SingleChildScrollView(child: SelectableText(text)),
      actions: [
        TextButton(
          onPressed: () => SharePlus.instance.share(
            ShareParams(text: text, subject: 'Paperless Scanner error'),
          ),
          child: const Text('Share'),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(ctx),
          child: const Text('OK'),
        ),
      ],
    ),
  );
}
