/// Everything the app needs to talk to one Paperless-ngx server.
class ServerConfig {
  const ServerConfig({
    required this.url,
    required this.token,
    this.caPem,
    this.caLabel,
  });

  /// Base URL, normalised: trimmed, no trailing slash.
  final String url;

  /// Paperless API token.
  final String token;

  /// Extra trusted root certificate(s) in PEM, e.g. a home-lab CA.
  /// Null means only the phone's built-in trusted roots are accepted.
  final String? caPem;

  /// File name the CA was imported from, shown in Settings.
  final String? caLabel;

  bool get isComplete => url.isNotEmpty && token.isNotEmpty;

  static String normaliseUrl(String raw) =>
      raw.trim().replaceAll(RegExp(r'/+$'), '');
}
