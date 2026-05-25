import 'dart:io';
import 'package:http/http.dart' as http;
import 'storage_service.dart';

enum UploadStatus { success, unauthorized, serverError, networkError }

class UploadResult {
  final UploadStatus status;
  final String? message;

  const UploadResult({required this.status, this.message});

  bool get isSuccess => status == UploadStatus.success;
}

class PaperlessService {
  final _storage = StorageService();

  /// Upload a PDF file to Paperless-ngx via its REST API.
  Future<UploadResult> uploadDocument(File pdfFile, {String? title}) async {
    final serverUrl = await _storage.getServerUrl();
    final token = await _storage.getToken();

    if (serverUrl == null || token == null) {
      return const UploadResult(
        status: UploadStatus.serverError,
        message: 'Server not configured. Open Settings and save your URL and token.',
      );
    }

    try {
      final uri = Uri.parse('$serverUrl/api/documents/post_document/');
      final request = http.MultipartRequest('POST', uri)
        ..headers['Authorization'] = 'Token $token'
        ..files.add(await http.MultipartFile.fromPath(
          'document',
          pdfFile.path,
          filename: '${(title ?? 'scan').replaceAll(RegExp(r'[^\w\s-]'), '')}.pdf',
        ));

      if (title != null && title.isNotEmpty) {
        request.fields['title'] = title;
      }

      final streamed = await request.send().timeout(const Duration(seconds: 90));

      switch (streamed.statusCode) {
        case 200:
        case 201:
        case 202: // Paperless returns 202 Accepted for async processing
          return const UploadResult(status: UploadStatus.success);
        case 401:
        case 403:
          return const UploadResult(
            status: UploadStatus.unauthorized,
            message: 'Invalid API token. Check Settings.',
          );
        default:
          return UploadResult(
            status: UploadStatus.serverError,
            message: 'Server returned HTTP ${streamed.statusCode}.',
          );
      }
    } on SocketException catch (e) {
      return UploadResult(
        status: UploadStatus.networkError,
        message: 'Cannot reach server: ${e.message}',
      );
    } on HttpException catch (e) {
      return UploadResult(
        status: UploadStatus.networkError,
        message: e.message,
      );
    } on Exception catch (e) {
      return UploadResult(
        status: UploadStatus.networkError,
        message: e.toString(),
      );
    }
  }

  /// Quick connectivity + auth check.
  Future<({bool ok, String? error})> testConnection() async {
    final serverUrl = await _storage.getServerUrl();
    final token = await _storage.getToken();

    if (serverUrl == null || token == null) {
      return (ok: false, error: 'Not configured');
    }

    try {
      final uri = Uri.parse('$serverUrl/api/');
      final response = await http
          .get(uri, headers: {'Authorization': 'Token $token'})
          .timeout(const Duration(seconds: 10));

      if (response.statusCode == 200) return (ok: true, error: null);
      if (response.statusCode == 401 || response.statusCode == 403) {
        return (ok: false, error: 'Invalid token (HTTP ${response.statusCode})');
      }
      return (ok: false, error: 'HTTP ${response.statusCode}');
    } on SocketException {
      return (ok: false, error: 'Cannot reach host. Check URL and network.');
    } on Exception catch (e) {
      return (ok: false, error: e.toString());
    }
  }
}
