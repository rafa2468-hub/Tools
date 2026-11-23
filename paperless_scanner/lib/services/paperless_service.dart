import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:http/http.dart' as http;
import 'package:http/io_client.dart';
import '../models/choices.dart';
import '../models/server_config.dart';
import '../models/upload_meta.dart';
import 'multipart.dart';
import 'task_result.dart';
import 'tls.dart';

/// Pinned so the task fields this app reads keep their shape (API v10:
/// lowercase status, `result_data`, `related_document_ids`).
const _apiAccept = 'application/json; version=10';

class ConnectionCheck {
  const ConnectionCheck(this.ok, this.message);
  final bool ok;
  final String message;
}

enum UploadStatus {
  queued,
  unauthorized,
  certificateError,
  serverError,
  networkError,
  notConfigured,
}

class UploadResult {
  const UploadResult(this.status, {this.taskId, this.message});
  final UploadStatus status;

  /// Paperless's consumption task, used to find out what happened.
  final String? taskId;
  final String? message;

  bool get isQueued => status == UploadStatus.queued;

  /// The server couldn't be reached at all: worth keeping the scan and
  /// trying again later (away from home, Wi-Fi off, server down).
  bool get isUnreachable => status == UploadStatus.networkError;
}

class PaperlessService {
  PaperlessService(
    this.config, {
    this.pollInterval = const Duration(seconds: 2),
    this.pollTimeout = const Duration(minutes: 2),
  });

  final ServerConfig config;
  final Duration pollInterval;
  final Duration pollTimeout;

  http.Client _client() {
    final inner = HttpClient(context: securityContextFor(config.caPem))
      ..connectionTimeout = const Duration(seconds: 10);
    return IOClient(inner);
  }

  Uri _uri(String path, [Map<String, String>? query]) {
    final uri = Uri.parse('${ServerConfig.normaliseUrl(config.url)}$path');
    return query == null ? uri : uri.replace(queryParameters: query);
  }

  Map<String, String> get _auth => {'Authorization': 'Token ${config.token.trim()}'};

  Map<String, String> get _apiHeaders => {..._auth, 'Accept': _apiAccept};

  /// Checks URL, certificate and token without saving anything.
  Future<ConnectionCheck> testConnection() async {
    if (!config.isComplete) {
      return const ConnectionCheck(false, 'Enter the server URL and the API token first.');
    }
    final client = _client();
    try {
      final r = await client
          .get(_uri('/api/ui_settings/'), headers: _apiHeaders)
          .timeout(const Duration(seconds: 15));
      switch (r.statusCode) {
        case 200:
          final api = r.headers['x-api-version'];
          final version = r.headers['x-version'];
          if (api == null) {
            return const ConnectionCheck(true, 'Connected (the server did not report its version).');
          }
          return ConnectionCheck(
            true,
            'Connected to Paperless-ngx${version == null ? '' : ' $version'} (API $api).',
          );
        case 401 || 403:
          return ConnectionCheck(false, 'The server refused the token (HTTP ${r.statusCode}).');
        case 406:
          return const ConnectionCheck(
            false,
            'This server no longer offers API version 10. The app needs an update.',
          );
        default:
          return ConnectionCheck(false, 'The server returned HTTP ${r.statusCode}. Check the URL.');
      }
    } catch (e) {
      return ConnectionCheck(false, describeError(e));
    } finally {
      client.close();
    }
  }

  /// Sends the PDF. A queued result only means Paperless accepted the file;
  /// call [waitForTask] with its [UploadResult.taskId] for the real outcome.
  Future<UploadResult> upload(
    File pdf, {
    UploadMeta meta = const UploadMeta(),
    required String filename,
  }) async {
    if (!config.isComplete) {
      return const UploadResult(
        UploadStatus.notConfigured,
        message: 'Server not configured. Open Settings and save your URL and token.',
      );
    }
    final client = _client();
    try {
      final body = MultipartBody(
        fields: meta.formFields,
        fileField: 'document',
        file: pdf,
        filename: filename,
      );
      final request = http.StreamedRequest('POST', _uri('/api/documents/post_document/'))
        ..headers.addAll(_auth)
        ..headers['Content-Type'] = body.contentType
        ..contentLength = await body.length;
      unawaited(request.sink
          .addStream(body.stream())
          .then((_) => request.sink.close(), onError: request.sink.addError));

      final streamed = await client.send(request).timeout(const Duration(seconds: 120));
      final body = await streamed.stream.bytesToString();

      switch (streamed.statusCode) {
        case 200 || 201 || 202:
          return UploadResult(UploadStatus.queued, taskId: taskIdFromBody(body));
        case 401 || 403:
          return const UploadResult(
            UploadStatus.unauthorized,
            message: 'The server refused the token. Check Settings.',
          );
        default:
          final detail = body.length > 300 ? '${body.substring(0, 300)}…' : body;
          return UploadResult(
            UploadStatus.serverError,
            message: 'Server returned HTTP ${streamed.statusCode}.${detail.isEmpty ? '' : '\n$detail'}',
          );
      }
    } catch (e) {
      return UploadResult(
        isCertificateError(e) ? UploadStatus.certificateError : UploadStatus.networkError,
        message: describeError(e),
      );
    } finally {
      client.close();
    }
  }

  /// Polls the consumption task until Paperless finishes or [pollTimeout]
  /// passes (then the state is [TaskState.processing]).
  Future<TaskOutcome> waitForTask(String taskId) async {
    final deadline = DateTime.now().add(pollTimeout);
    final client = _client();
    try {
      while (true) {
        final r = await client
            .get(_uri('/api/tasks/', {'task_id': taskId}), headers: _apiHeaders)
            .timeout(const Duration(seconds: 15));
        if (r.statusCode != 200) {
          return TaskOutcome(
            TaskState.unknown,
            message: 'Uploaded, but the app could not read the result '
                '(HTTP ${r.statusCode}). Check Paperless.',
          );
        }
        final task = taskFromResponse(jsonDecode(utf8.decode(r.bodyBytes)));
        if (task != null) {
          final outcome = interpretTask(task);
          if (outcome.state != TaskState.processing) return outcome;
        }
        if (DateTime.now().isAfter(deadline)) {
          return const TaskOutcome(TaskState.processing);
        }
        await Future<void>.delayed(pollInterval);
      }
    } catch (e) {
      return TaskOutcome(
        TaskState.unknown,
        message: 'Uploaded, but checking the result failed: ${describeError(e)}',
      );
    } finally {
      client.close();
    }
  }
}

extension PaperlessChoices on PaperlessService {
  /// Tags, correspondents and document types, every page of each.
  /// Throws on any failure, so a half-fetched list never replaces a cache.
  Future<Choices> fetchChoices() async {
    final client = _client();
    try {
      Future<List<NamedItem>> all(String path) async {
        final items = <NamedItem>[];
        // Page numbers instead of the `next` link: behind a proxy that link
        // can carry the wrong scheme or host.
        for (var page = 1; page <= 50; page++) {
          final r = await client
              .get(
                _uri(path, {
                  'page': '$page',
                  'page_size': '500',
                  'ordering': 'name',
                }),
                headers: _apiHeaders,
              )
              .timeout(const Duration(seconds: 15));
          if (r.statusCode != 200) {
            throw HttpException('HTTP ${r.statusCode} from $path');
          }
          final body = jsonDecode(utf8.decode(r.bodyBytes));
          if (body is! Map || body['results'] is! List) {
            throw FormatException('Unexpected answer from $path');
          }
          for (final e in body['results'] as List) {
            if (NamedItem.fromJson(e) case final item?) items.add(item);
          }
          final count = body['count'];
          if (body['next'] == null || (count is int && items.length >= count)) {
            break;
          }
        }
        return items;
      }

      return Choices(
        tags: await all('/api/tags/'),
        correspondents: await all('/api/correspondents/'),
        documentTypes: await all('/api/document_types/'),
        fetchedAt: DateTime.now(),
      );
    } finally {
      client.close();
    }
  }
}

/// post_document answers with the task UUID as a JSON string.
String? taskIdFromBody(String body) {
  try {
    final value = jsonDecode(body);
    if (value is String && value.isNotEmpty) return value;
    if (value is Map && value['task_id'] is String) return value['task_id'] as String;
  } on FormatException {
    // fall through to the plain-text check
  }
  final trimmed = body.trim();
  return RegExp(r'^[0-9a-fA-F-]{32,36}$').hasMatch(trimmed) ? trimmed : null;
}

String describeError(Object e) {
  if (isCertificateError(e)) {
    return "The server's certificate isn't trusted. For a home-lab server, "
        'import its CA certificate in Settings (Home certificate authority).\n\n$e';
  }
  if (e is TimeoutException) return 'The server took too long to answer.';
  if (e is SocketException ||
      (e is http.ClientException && e.toString().contains('SocketException'))) {
    return "Can't reach the server. Check the URL and that the phone is on "
        'the home network.\n\n$e';
  }
  if (e is http.ClientException) return 'Connection failed: ${e.message}';
  return e.toString();
}
