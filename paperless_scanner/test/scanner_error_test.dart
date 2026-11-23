import 'package:cunning_document_scanner/cunning_document_scanner.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';
import 'package:paperless_scanner/screens/scanner_error.dart';

void main() {
  const unavailable = CunningDocumentScannerException(
    'com.google.mlkit.common.MlKitException (ML Kit code 15): Failed',
    code: scannerUnavailableCode,
  );

  test('a scanner that will not start is named, with the exact error', () {
    final text = scannerErrorText(unavailable);
    expect(text, startsWith("Google's document scanner couldn't start."));
    expect(text, contains('MlKitException (ML Kit code 15): Failed'));
  });

  test('other scanner errors keep their code and message', () {
    final text = scannerErrorText(
      const CunningDocumentScannerException('boom', code: 'ERROR'),
    );
    expect(text, contains('(ERROR)'));
    expect(text, contains('Error: boom'));
    expect(scannerErrorText(StateError('x')), contains('Bad state: x'));
  });

  testWidgets('the dialog shows the error and closes on OK', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () => showScannerError(context, unavailable),
            child: const Text('scan'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('scan'));
    await tester.pumpAndSettle();
    expect(find.text('Scanner problem'), findsOneWidget);
    expect(find.textContaining('ML Kit code 15'), findsOneWidget);
    expect(find.text('Share'), findsOneWidget);
    await tester.tap(find.text('OK'));
    await tester.pumpAndSettle();
    expect(find.text('Scanner problem'), findsNothing);
  });
}
