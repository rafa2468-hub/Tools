import 'dart:convert';
import 'dart:io';

/// A CA file the user picked can't be used; [message] is shown as is.
class CaImportException implements Exception {
  const CaImportException(this.message);
  final String message;

  @override
  String toString() => message;
}

final _pemBlock = RegExp(r'-----BEGIN ([A-Z0-9 ]+)-----[\s\S]*?-----END \1-----');

/// Turns the bytes of a CA certificate file (PEM or DER) into PEM text that
/// holds only CERTIFICATE blocks. Refuses files that contain a private key.
String caPemFromBytes(List<int> bytes) {
  if (bytes.isEmpty) {
    throw const CaImportException('The file is empty.');
  }

  String? text;
  try {
    text = utf8.decode(bytes);
  } on FormatException {
    text = null;
  }

  if (text != null && text.contains('-----BEGIN')) {
    final blocks = _pemBlock.allMatches(text).toList();
    if (blocks.any((m) => m.group(1)!.contains('PRIVATE KEY'))) {
      throw const CaImportException(
        'This file holds a private key. Import only the public certificate '
        '(for the home lab: rootCA.pem), never a key.',
      );
    }
    final certs = blocks
        .where((m) => m.group(1) == 'CERTIFICATE')
        .map((m) => m.group(0)!)
        .toList();
    if (certs.isEmpty) {
      throw const CaImportException('No certificate found in this file.');
    }
    final pem = '${certs.join('\n')}\n';
    _checkLoads(pem);
    return pem;
  }

  // Binary (DER) X.509 starts with an ASN.1 SEQUENCE tag.
  if (bytes.first != 0x30) {
    throw const CaImportException(
      'This is not a certificate file (expected PEM or DER).',
    );
  }
  final b64 = base64.encode(bytes);
  final lines = <String>[
    for (var i = 0; i < b64.length; i += 64)
      b64.substring(i, i + 64 > b64.length ? b64.length : i + 64),
  ];
  final pem =
      '-----BEGIN CERTIFICATE-----\n${lines.join('\n')}\n-----END CERTIFICATE-----\n';
  _checkLoads(pem);
  return pem;
}

void _checkLoads(String pem) {
  try {
    SecurityContext(withTrustedRoots: false)
        .setTrustedCertificatesBytes(utf8.encode(pem));
  } on Exception catch (e) {
    throw CaImportException('The certificate could not be read ($e).');
  }
}

/// The TLS context for talking to the server: the platform's trusted roots,
/// plus the imported CA if there is one. Hostname checks stay on.
SecurityContext securityContextFor(String? caPem) {
  final context = SecurityContext(withTrustedRoots: true);
  if (caPem != null && caPem.isNotEmpty) {
    context.setTrustedCertificatesBytes(utf8.encode(caPem));
  }
  return context;
}

/// True when an error (raw or wrapped by package:http) is a failed TLS
/// certificate check.
bool isCertificateError(Object e) {
  if (e is HandshakeException) return true;
  final text = e.toString();
  return text.contains('CERTIFICATE_VERIFY_FAILED') ||
      text.contains('HandshakeException') ||
      text.contains('Hostname mismatch');
}
