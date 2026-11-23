import 'package:flutter_test/flutter_test.dart';
import 'package:paperless_scanner/models/server_config.dart';
import 'package:paperless_scanner/services/filename.dart';
import 'package:paperless_scanner/services/paperless_service.dart';
import 'package:paperless_scanner/services/task_result.dart';

void main() {
  final when = DateTime(2026, 11, 22, 18, 30);

  group('pdfFilename', () {
    test('keeps Polish letters', () {
      expect(pdfFilename('Rachunek Łódź źdźbło', when), 'Rachunek Łódź źdźbło.pdf');
    });
    test('drops slashes and punctuation', () {
      expect(pdfFilename('Tax 2025/26: T4 (final)!', when), 'Tax 202526 T4 final.pdf');
    });
    test('falls back to a dated name', () {
      expect(pdfFilename(null, when), 'scan-2026-11-22-1830.pdf');
      expect(pdfFilename('  ', when), 'scan-2026-11-22-1830.pdf');
      expect(pdfFilename('///', when), 'scan-2026-11-22-1830.pdf');
    });
    test('caps the length', () {
      expect(pdfFilename('a' * 200, when).length, 84);
    });
  });

  test('normaliseUrl strips spaces and trailing slashes', () {
    expect(ServerConfig.normaliseUrl(' https://p.home.lab/// '), 'https://p.home.lab');
  });

  group('taskIdFromBody', () {
    test('JSON string (post_document)', () {
      expect(taskIdFromBody('"0b2c6e5a-1f2e-4c1d-9d1e-1234567890ab"'),
          '0b2c6e5a-1f2e-4c1d-9d1e-1234567890ab');
    });
    test('bare UUID', () {
      expect(taskIdFromBody('0b2c6e5a-1f2e-4c1d-9d1e-1234567890ab\n'),
          '0b2c6e5a-1f2e-4c1d-9d1e-1234567890ab');
    });
    test('garbage', () => expect(taskIdFromBody('<html>OK</html>'), isNull));
  });

  group('taskFromResponse', () {
    test('paginated v10', () {
      expect(taskFromResponse({'count': 1, 'results': [{'status': 'success'}]}),
          {'status': 'success'});
    });
    test('bare list (older API)', () {
      expect(taskFromResponse([{'status': 'SUCCESS'}]), {'status': 'SUCCESS'});
    });
    test('empty', () {
      expect(taskFromResponse({'count': 0, 'results': []}), isNull);
    });
  });

  group('interpretTask (API v10)', () {
    test('pending and started keep polling', () {
      expect(interpretTask({'status': 'pending'}).state, TaskState.processing);
      expect(interpretTask({'status': 'started'}).state, TaskState.processing);
    });
    test('success with document_id', () {
      final o = interpretTask({
        'status': 'success',
        'result_data': {'document_id': 42},
        'related_document_ids': [42],
      });
      expect(o.state, TaskState.success);
      expect(o.documentId, 42);
    });
    test('success with only related_document_ids', () {
      final o = interpretTask({'status': 'success', 'related_document_ids': [7]});
      expect(o.documentId, 7);
    });
    test('duplicate is a failure with duplicate_of', () {
      final o = interpretTask({
        'status': 'failure',
        'result_data': {'duplicate_of': 12, 'duplicate_in_trash': true},
      });
      expect(o.state, TaskState.duplicate);
      expect(o.documentId, 12);
      expect(o.inTrash, isTrue);
    });
    test('failure carries error_message', () {
      final o = interpretTask({
        'status': 'failure',
        'result_data': {'error_type': 'ConsumerError', 'error_message': 'Unsupported mime type'},
      });
      expect(o.state, TaskState.failed);
      expect(o.message, 'Unsupported mime type');
    });
    test('revoked carries reason', () {
      final o = interpretTask({
        'status': 'revoked',
        'result_data': {'reason': 'Worker lost. Check worker logs for details.'},
      });
      expect(o.state, TaskState.failed);
      expect(o.message, contains('Worker lost'));
    });
  });

  test('interpretTask reads the older v9 shape', () {
    final ok = interpretTask({'status': 'SUCCESS', 'related_document': '9'});
    expect(ok.state, TaskState.success);
    expect(ok.documentId, 9);
    final bad = interpretTask({'status': 'FAILURE', 'result': 'It is a duplicate of x.'});
    expect(bad.state, TaskState.failed);
    expect(bad.message, 'It is a duplicate of x.');
  });
}
