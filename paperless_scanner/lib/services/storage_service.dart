import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import '../models/server_config.dart';

class StorageService {
  static const _storage = FlutterSecureStorage(
    aOptions: AndroidOptions(encryptedSharedPreferences: true),
    iOptions: IOSOptions(accessibility: KeychainAccessibility.first_unlock),
  );

  static const _keyServerUrl = 'paperless_server_url';
  static const _keyToken = 'paperless_token';
  static const _keyCaPem = 'paperless_ca_pem';
  static const _keyCaLabel = 'paperless_ca_label';

  // Up to 1.0.0 the app had an "accept any certificate" switch. It is gone,
  // replaced by importing the server's CA; drop the old flag on first load.
  static const _legacyAllowInsecure = 'paperless_allow_insecure';

  Future<ServerConfig> load() async {
    final values = await Future.wait([
      _storage.read(key: _keyServerUrl),
      _storage.read(key: _keyToken),
      _storage.read(key: _keyCaPem),
      _storage.read(key: _keyCaLabel),
    ]);
    await _storage.delete(key: _legacyAllowInsecure);
    final caPem = values[2];
    return ServerConfig(
      url: values[0] ?? '',
      token: values[1] ?? '',
      caPem: (caPem == null || caPem.isEmpty) ? null : caPem,
      caLabel: values[3],
    );
  }

  Future<void> save(ServerConfig config) async {
    final caPem = config.caPem;
    await Future.wait([
      _storage.write(
          key: _keyServerUrl, value: ServerConfig.normaliseUrl(config.url)),
      _storage.write(key: _keyToken, value: config.token.trim()),
      if (caPem == null) ...[
        _storage.delete(key: _keyCaPem),
        _storage.delete(key: _keyCaLabel),
      ] else ...[
        _storage.write(key: _keyCaPem, value: caPem),
        _storage.write(key: _keyCaLabel, value: config.caLabel ?? ''),
      ],
    ]);
  }

  Future<bool> isConfigured() async => (await load()).isComplete;
}
