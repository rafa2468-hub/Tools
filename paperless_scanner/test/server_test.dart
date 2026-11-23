// End-to-end tests against a fake Paperless server on localhost, served
// with a certificate from a throwaway CA made by openssl for this run.
// Needs the `openssl` command (present on the GitHub ubuntu runners).
import 'dart:convert';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:paperless_scanner/models/server_config.dart';
import 'package:paperless_scanner/services/paperless_service.dart';
import 'package:paperless_scanner/services/task_result.dart';
import 'package:paperless_scanner/services/tls.dart';

const _token = 'good-token';
const _taskId = '0b2c6e5a-1f2e-4c1d-9d1e-1234567890ab';

late Directory _tmp;
late String _caPem;
late HttpServer _server;

// What the fake server's task endpoint answers with.
String _finalStatus = 'success';
Map<String, dynamic> _resultData = {'document_id': 42};
int _polls = 0;
String _lastUploadBody = '';

Future<void> _openssl(List<String> args) async {
  final r = await Process.run('openssl', args, workingDirectory: _tmp.path);
  if (r.exitCode != 0) {
    throw StateError('openssl ${args.first} failed: ${r.stderr}');
  }
}

String _path(String name) => '${_tmp.path}/$name';

Future<void> _handle(HttpRequest req) async {
  final res = req.response;
  if (req.headers.value('authorization') != 'Token $_token') {
    res.statusCode = 401;
    res.headers.set('www-authenticate', 'Token');
    await res.close();
    return;
  }
  res.headers.contentType = ContentType.json;
  switch (req.uri.path) {
    case '/api/ui_settings/':
      res.headers.set('X-Api-Version', '10');
      res.headers.set('X-Version', '9.9.9');
      res.write('{}');
    case '/api/documents/post_document/':
      _lastUploadBody = utf8.decode(
        await req.fold<List<int>>(<int>[], (a, b) => a..addAll(b)),
        allowMalformed: true,
      );
      res.write(jsonEncode(_taskId));
    case '/api/tasks/':
      _polls++;
      final done = _polls >= 2;
      res.write(jsonEncode({
        'count': 1,
        'next': null,
        'previous': null,
        'results': [
          {
            'task_id': req.uri.queryParameters['task_id'],
            'status': done ? _finalStatus : 'started',
            'result_data': done ? _resultData : null,
            'related_document_ids': <int>[],
          }
        ],
      }));
    default:
      res.statusCode = 404;
  }
  await res.close();
}

ServerConfig _config({String host = 'localhost', String token = _token, bool withCa = true}) =>
    ServerConfig(
      url: 'https://$host:${_server.port}/',
      token: token,
      caPem: withCa ? _caPem : null,
    );

PaperlessService _service(ServerConfig c) => PaperlessService(
      c,
      pollInterval: const Duration(milliseconds: 10),
      pollTimeout: const Duration(seconds: 5),
    );

void main() {
  setUpAll(() async {
    HttpOverrides.global = null; // real sockets, not the test binding's mock
    _tmp = await Directory.systemTemp.createTemp('paperless_test');
    await _openssl([
      'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
      '-keyout', 'ca.key', '-out', 'ca.pem', '-subj', '/CN=Test Home CA',
      '-addext', 'basicConstraints=critical,CA:TRUE',
      '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
    ]);
    await _openssl([
      'req', '-newkey', 'rsa:2048', '-nodes',
      '-keyout', 'leaf.key', '-out', 'leaf.csr', '-subj', '/CN=localhost',
    ]);
    File(_path('ext.cnf')).writeAsStringSync(
      'subjectAltName=DNS:localhost\n'
      'basicConstraints=CA:FALSE\n'
      'extendedKeyUsage=serverAuth\n',
    );
    await _openssl([
      'x509', '-req', '-in', 'leaf.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
      '-CAcreateserial', '-out', 'leaf.pem', '-days', '2', '-extfile', 'ext.cnf',
    ]);
    await _openssl(['x509', '-in', 'ca.pem', '-outform', 'DER', '-out', 'ca.der']);
    _caPem = File(_path('ca.pem')).readAsStringSync();

    final serverContext = SecurityContext()
      ..useCertificateChain(_path('leaf.pem'))
      ..usePrivateKey(_path('leaf.key'));
    _server = await HttpServer.bindSecure(
        InternetAddress.loopbackIPv4, 0, serverContext);
    _server.listen(_handle);
  });

  tearDownAll(() async {
    await _server.close(force: true);
    await _tmp.delete(recursive: true);
  });

  setUp(() {
    _polls = 0;
    _finalStatus = 'success';
    _resultData = {'document_id': 42};
  });

  group('caPemFromBytes', () {
    test('accepts the PEM CA', () {
      final pem = caPemFromBytes(File(_path('ca.pem')).readAsBytesSync());
      expect(pem, startsWith('-----BEGIN CERTIFICATE-----'));
    });
    test('converts a DER CA to PEM', () {
      final pem = caPemFromBytes(File(_path('ca.der')).readAsBytesSync());
      expect(pem, startsWith('-----BEGIN CERTIFICATE-----'));
      expect(pem.trim(), _caPem.trim());
    });
    test('refuses a private key', () {
      expect(() => caPemFromBytes(File(_path('ca.key')).readAsBytesSync()),
          throwsA(isA<CaImportException>()));
    });
    test('refuses a CA file with the key appended', () {
      final both = '$_caPem\n${File(_path('ca.key')).readAsStringSync()}';
      expect(() => caPemFromBytes(utf8.encode(both)),
          throwsA(isA<CaImportException>()));
    });
    test('refuses text that is not a certificate', () {
      expect(() => caPemFromBytes(utf8.encode('hello')),
          throwsA(isA<CaImportException>()));
    });
    test('refuses a corrupted certificate block', () {
      const bad = '-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n';
      expect(() => caPemFromBytes(utf8.encode(bad)),
          throwsA(isA<CaImportException>()));
    });
  });

  group('testConnection', () {
    test('without the CA the certificate is not trusted', () async {
      final r = await _service(_config(withCa: false)).testConnection();
      expect(r.ok, isFalse);
      expect(r.message, contains("certificate isn't trusted"));
    });
    test('with the CA it connects and reports the version', () async {
      final r = await _service(_config()).testConnection();
      expect(r.ok, isTrue, reason: r.message);
      expect(r.message, contains('9.9.9'));
      expect(r.message, contains('API 10'));
    });
    test('wrong token is refused', () async {
      final r = await _service(_config(token: 'bad')).testConnection();
      expect(r.ok, isFalse);
      expect(r.message, contains('refused the token'));
    });
    test('hostname is still checked when the CA is trusted', () async {
      // The certificate only names "localhost".
      final r = await _service(_config(host: '127.0.0.1')).testConnection();
      expect(r.ok, isFalse);
      expect(r.message, contains("certificate isn't trusted"));
    });
  });

  group('upload and waitForTask', () {
    late File pdf;
    setUp(() {
      pdf = File(_path('scan.pdf'))..writeAsBytesSync(utf8.encode('%PDF-1.4 test'));
    });

    test('success reports the new document', () async {
      final s = _service(_config());
      final up = await s.upload(pdf, title: 'Rachunek Łódź', filename: 'Rachunek Łódź.pdf');
      expect(up.isQueued, isTrue, reason: up.message);
      expect(up.taskId, _taskId);
      expect(_lastUploadBody, contains('name="title"'));
      expect(_lastUploadBody, contains('Rachunek Łódź'));
      final o = await s.waitForTask(up.taskId!);
      expect(o.state, TaskState.success);
      expect(o.documentId, 42);
      expect(_polls, 2);
    });

    test('duplicate is reported as a duplicate', () async {
      _finalStatus = 'failure';
      _resultData = {'duplicate_of': 7, 'duplicate_in_trash': false};
      final o = await _service(_config()).waitForTask(_taskId);
      expect(o.state, TaskState.duplicate);
      expect(o.documentId, 7);
    });

    test('failure carries the error message', () async {
      _finalStatus = 'failure';
      _resultData = {'error_type': 'ConsumerError', 'error_message': 'Broken PDF'};
      final o = await _service(_config()).waitForTask(_taskId);
      expect(o.state, TaskState.failed);
      expect(o.message, 'Broken PDF');
    });

    test('wrong token on upload is unauthorized', () async {
      final up = await _service(_config(token: 'bad'))
          .upload(pdf, filename: 'x.pdf');
      expect(up.status, UploadStatus.unauthorized);
    });

    test('untrusted certificate on upload is a certificate error', () async {
      final up = await _service(_config(withCa: false))
          .upload(pdf, filename: 'x.pdf');
      expect(up.status, UploadStatus.certificateError);
    });
  });
}
