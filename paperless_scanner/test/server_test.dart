// End-to-end tests against a fake Paperless server on localhost, served
// with a certificate from a throwaway CA made by openssl for this run.
// Needs the `openssl` command (present on the GitHub ubuntu runners).
import 'dart:convert';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:paperless_scanner/models/server_config.dart';
import 'package:paperless_scanner/models/upload_meta.dart';
import 'package:paperless_scanner/services/paperless_service.dart';
import 'package:paperless_scanner/services/queue_runner.dart';
import 'package:paperless_scanner/services/task_result.dart';
import 'package:paperless_scanner/services/tls.dart';
import 'package:paperless_scanner/services/upload_queue.dart';

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
String? _lastUploadType;
int _uploads = 0;
// HTTP status for the next uploads (200 = accept).
int _uploadStatus = 200;

// 7 tags, served 3 per page whatever page_size asks, to test paging.
final _tags = [for (var i = 1; i <= 7; i++) {'id': i, 'name': 'Tag $i'}];
const _tagsPerPage = 3;

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
      _uploads++;
      _lastUploadType = req.headers.value('content-type');
      _lastUploadBody = utf8.decode(
        await req.fold<List<int>>(<int>[], (a, b) => a..addAll(b)),
        allowMalformed: true,
      );
      if (_uploadStatus != 200) {
        res.statusCode = _uploadStatus;
        res.write('{"tags":["Invalid pk \\"99\\" - object does not exist."]}');
      } else {
        res.write(jsonEncode(_taskId));
      }
    case '/api/tags/':
      final page = int.parse(req.uri.queryParameters['page'] ?? '1');
      final start = (page - 1) * _tagsPerPage;
      final slice = _tags.skip(start).take(_tagsPerPage).toList();
      final more = start + _tagsPerPage < _tags.length;
      res.write(jsonEncode({
        'count': _tags.length,
        // Like a proxy that forgets the scheme: the app must not follow it.
        'next': more ? 'http://wrong.example/api/tags/?page=${page + 1}' : null,
        'previous': null,
        'results': slice,
      }));
    case '/api/correspondents/':
      res.write(jsonEncode({
        'count': 1,
        'next': null,
        'results': [
          {'id': 3, 'name': 'Canada Revenue Agency', 'document_count': 12},
        ],
      }));
    case '/api/document_types/':
      res.write(jsonEncode({'count': 0, 'next': null, 'results': <Object>[]}));
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
    _uploads = 0;
    _uploadStatus = 200;
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
      final up = await s.upload(pdf,
          meta: const UploadMeta(title: 'Rachunek Łódź'), filename: 'Rachunek Łódź.pdf');
      expect(up.isQueued, isTrue, reason: up.message);
      expect(up.taskId, _taskId);
      expect(_lastUploadBody, contains('name="title"\r\n\r\nRachunek Łódź\r\n'));
      expect(_lastUploadBody, contains('filename="Rachunek Łódź.pdf"'));
      expect(_lastUploadBody, contains('%PDF-1.4 test'));
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

    test('details go as form fields, one per tag', () async {
      final up = await _service(_config()).upload(
        pdf,
        meta: UploadMeta(
          tagIds: const [2, 5],
          correspondentId: 3,
          documentTypeId: 4,
          created: DateTime(2026, 2, 28),
        ),
        filename: 'x.pdf',
      );
      expect(up.isQueued, isTrue, reason: up.message);
      expect(_lastUploadType, startsWith('multipart/form-data; boundary='));
      final boundary = _lastUploadType!.split('boundary=').last;
      final parts = _lastUploadBody.split('--$boundary');
      // preamble, 5 fields, the file, then the closing "--"
      expect(parts, hasLength(8));
      String field(String name) => parts
          .firstWhere((p) => p.contains('name="$name"\r\n'))
          .split('\r\n\r\n')
          .last
          .trim();
      expect(
        parts.where((p) => p.contains('name="tags"')).map((p) => p.split('\r\n\r\n').last.trim()),
        ['2', '5'],
      );
      expect(field('correspondent'), '3');
      expect(field('document_type'), '4');
      expect(field('created'), '2026-02-28T12:00:00');
      expect(parts.last.trim(), '--');
    });

    test('unreachable server is a network error', () async {
      final closed = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      final port = closed.port;
      await closed.close();
      final up = await _service(ServerConfig(url: 'https://localhost:$port', token: _token, caPem: _caPem))
          .upload(pdf, filename: 'x.pdf');
      expect(up.status, UploadStatus.networkError);
      expect(up.isUnreachable, isTrue);
    });
  });

  test('fetchChoices reads every page by number, not the next link', () async {
    final c = await _service(_config()).fetchChoices();
    expect(c.tags.map((t) => t.id), [1, 2, 3, 4, 5, 6, 7]);
    expect(c.correspondents.single.name, 'Canada Revenue Agency');
    expect(c.documentTypes, isEmpty);
    expect(c.fetchedAt, isNotNull);
  });

  test('fetchChoices fails loudly with a bad token', () async {
    await expectLater(
        _service(_config(token: 'bad')).fetchChoices(), throwsA(isA<HttpException>()));
  });

  group('drainQueue', () {
    late UploadQueue q;
    late File pdf;
    setUp(() {
      q = UploadQueue(Directory('${_tmp.path}/q_${DateTime.now().microsecondsSinceEpoch}'));
      pdf = File(_path('queued.pdf'))..writeAsStringSync('%PDF-1.4 queued');
    });

    test('uploads everything waiting and reports it', () async {
      await q.add(pdf, const UploadMeta(title: 'One'), 'One.pdf');
      await q.add(pdf, const UploadMeta(title: 'Two'), 'Two.pdf');
      final s = await drainQueue(q, _service(_config()));
      expect(s!.imported, 2);
      expect(s.stoppedBecause, isNull);
      expect(await q.items(), isEmpty);
      final r = await q.reports();
      expect(r.map((e) => e.summary), everyElement(contains('imported as document #42')));
    });

    test('stops at an unreachable server and keeps the scans', () async {
      await q.add(pdf, const UploadMeta(title: 'One'), 'One.pdf');
      await q.add(pdf, const UploadMeta(title: 'Two'), 'Two.pdf');
      final closed = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      final port = closed.port;
      await closed.close();
      final away = ServerConfig(url: 'https://localhost:$port', token: _token, caPem: _caPem);
      final s = await drainQueue(q, _service(away));
      expect(s!.imported, 0);
      expect(s.stillWaiting, 2);
      expect(s.stoppedBecause, contains("Can't reach the server"));
      final items = await q.items();
      expect(items, hasLength(2));
      expect(items.first.attempts, 1);
      expect(items.last.attempts, 0, reason: 'one quick try, then stop');
      expect(items.every((i) => !i.failed), isTrue);
    });

    test('a refused upload is marked failed and the rest still go', () async {
      await q.add(pdf, const UploadMeta(tagIds: [99]), 'bad.pdf');
      final good = await q.add(pdf, const UploadMeta(title: 'Good'), 'good.pdf');
      // The first upload is refused, the second accepted.
      _uploadStatus = 400;
      final s = await drainQueue(
          q, _QueueTestService(_config(), onUpload: () => _uploadStatus = 200));
      expect(s!.failed, 1);
      expect(s.imported, 1);
      final left = await q.items();
      expect(left.single.failed, isTrue);
      expect(left.single.lastError, contains('HTTP 400'));
      expect(left.any((i) => i.id == good.id), isFalse);
    });

    test('a duplicate is removed and reported as already there', () async {
      _finalStatus = 'failure';
      _resultData = {'duplicate_of': 7, 'duplicate_in_trash': false};
      await q.add(pdf, const UploadMeta(title: 'Again'), 'Again.pdf');
      final s = await drainQueue(q, _service(_config()));
      expect(s!.duplicates, 1);
      expect(await q.items(), isEmpty);
      expect((await q.reports()).single.summary, contains('already in Paperless (#7)'));
    });

    test('a failed import stays, marked failed, with the reason', () async {
      _finalStatus = 'failure';
      _resultData = {'error_message': 'Broken PDF'};
      await q.add(pdf, const UploadMeta(title: 'Broken'), 'Broken.pdf');
      final s = await drainQueue(q, _service(_config()));
      expect(s!.failed, 1);
      final item = (await q.items()).single;
      expect(item.failed, isTrue);
      expect(item.lastError, 'Broken PDF');
      expect(await q.waiting(), isEmpty);
    });

    test('a second run while one is going does nothing', () async {
      await q.add(pdf, const UploadMeta(title: 'One'), 'One.pdf');
      final results = await Future.wait([
        drainQueue(q, _service(_config())),
        drainQueue(q, _service(_config())),
      ]);
      expect(results.whereType<DrainSummary>(), hasLength(1));
      expect(_uploads, 1);
    });
  });
}

/// Calls [onUpload] after each upload, to change the fake server between
/// two uploads of one drain.
class _QueueTestService extends PaperlessService {
  _QueueTestService(super.config, {required this.onUpload})
      : super(pollInterval: const Duration(milliseconds: 10), pollTimeout: const Duration(seconds: 5));

  final void Function() onUpload;

  @override
  Future<UploadResult> upload(File pdf, {UploadMeta meta = const UploadMeta(), required String filename}) async {
    final r = await super.upload(pdf, meta: meta, filename: filename);
    onUpload();
    return r;
  }
}
