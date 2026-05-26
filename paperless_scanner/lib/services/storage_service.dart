import 'package:flutter_secure_storage/flutter_secure_storage.dart';

class StorageService {
  static const _storage = FlutterSecureStorage(
    aOptions: AndroidOptions(encryptedSharedPreferences: true),
    iOptions: IOSOptions(accessibility: KeychainAccessibility.first_unlock),
  );

  static const _keyServerUrl = 'paperless_server_url';
  static const _keyToken = 'paperless_token';

  Future<String?> getServerUrl() => _storage.read(key: _keyServerUrl);
  Future<String?> getToken() => _storage.read(key: _keyToken);

  Future<void> saveSettings({
    required String serverUrl,
    required String token,
  }) async {
    // Normalise: strip trailing slash
    final normalised = serverUrl.trim().replaceAll(RegExp(r'/+$'), '');
    await Future.wait([
      _storage.write(key: _keyServerUrl, value: normalised),
      _storage.write(key: _keyToken, value: token.trim()),
    ]);
  }

  Future<bool> isConfigured() async {
    final results = await Future.wait([getServerUrl(), getToken()]);
    return results.every((v) => v != null && v.isNotEmpty);
  }
}
