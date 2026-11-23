import 'package:file_selector/file_selector.dart';
import 'package:material_ui/material_ui.dart';
import '../models/server_config.dart';
import '../services/paperless_service.dart';
import '../services/storage_service.dart';
import '../services/tls.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  final _formKey = GlobalKey<FormState>();
  final _urlController = TextEditingController();
  final _tokenController = TextEditingController();
  final _storage = StorageService();

  bool _tokenVisible = false;
  bool _isTesting = false;
  bool _isSaving = false;
  String? _caPem;
  String? _caLabel;
  String? _testResult;
  bool _testPassed = false;

  @override
  void initState() {
    super.initState();
    _loadSaved();
  }

  @override
  void dispose() {
    _urlController.dispose();
    _tokenController.dispose();
    super.dispose();
  }

  Future<void> _loadSaved() async {
    final config = await _storage.load();
    if (!mounted) return;
    _urlController.text = config.url;
    _tokenController.text = config.token;
    setState(() {
      _caPem = config.caPem;
      _caLabel = config.caLabel;
    });
  }

  /// What's on screen right now, saved or not.
  ServerConfig get _formConfig => ServerConfig(
        url: ServerConfig.normaliseUrl(_urlController.text),
        token: _tokenController.text.trim(),
        caPem: _caPem,
        caLabel: _caLabel,
      );

  void _clearTestResult() {
    if (_testResult != null) setState(() => _testResult = null);
  }

  Future<void> _importCa() async {
    // No type filter: .pem has no standard MIME type on Android, so a
    // filter would hide the file.
    final file = await openFile();
    if (file == null) return;
    try {
      final pem = caPemFromBytes(await file.readAsBytes());
      setState(() {
        _caPem = pem;
        _caLabel = file.name;
        _testResult = null;
      });
    } on CaImportException catch (e) {
      _snack(e.message, error: true);
    } on Exception catch (e) {
      _snack('Could not read the file: $e', error: true);
    }
  }

  void _removeCa() => setState(() {
        _caPem = null;
        _caLabel = null;
        _testResult = null;
      });

  /// Tests what's on screen without saving it, so a failed test never
  /// replaces settings that work.
  Future<void> _testConnection() async {
    if (!_formKey.currentState!.validate()) return;
    setState(() {
      _isTesting = true;
      _testResult = null;
    });

    final result = await PaperlessService(_formConfig).testConnection();

    if (!mounted) return;
    setState(() {
      _isTesting = false;
      _testPassed = result.ok;
      _testResult = result.message;
    });
  }

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;

    setState(() => _isSaving = true);
    await _storage.save(_formConfig);
    if (!mounted) return;
    setState(() => _isSaving = false);

    _snack('Settings saved');
    Navigator.pop(context);
  }

  void _snack(String message, {bool error = false}) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(message),
        backgroundColor: error ? Theme.of(context).colorScheme.error : null,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final isPlainHttp =
        _urlController.text.trim().toLowerCase().startsWith('http://');

    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(20),
        child: Form(
          key: _formKey,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              // ── Server section ──────────────────────────────────────
              Text(
                'Paperless-ngx Server',
                style: tt.titleMedium?.copyWith(
                  color: cs.primary,
                  fontWeight: FontWeight.bold,
                ),
              ),
              const SizedBox(height: 12),
              TextFormField(
                controller: _urlController,
                decoration: InputDecoration(
                  labelText: 'Server URL',
                  hintText: 'https://paperless.example.com',
                  prefixIcon: const Icon(Icons.dns_outlined),
                  border: const OutlineInputBorder(),
                  helperText: isPlainHttp
                      ? 'Plain http sends your token unencrypted.'
                      : 'No trailing slash needed',
                  helperStyle: isPlainHttp ? TextStyle(color: cs.error) : null,
                ),
                keyboardType: TextInputType.url,
                autocorrect: false,
                onChanged: (_) => setState(() => _testResult = null),
                validator: (v) {
                  if (v == null || v.trim().isEmpty) return 'Required';
                  final uri = Uri.tryParse(v.trim());
                  if (uri == null ||
                      !(uri.scheme == 'https' || uri.scheme == 'http') ||
                      uri.host.isEmpty) {
                    return 'Enter a full URL, starting with https://';
                  }
                  return null;
                },
              ),
              const SizedBox(height: 16),

              // ── Token section ────────────────────────────────────────
              TextFormField(
                controller: _tokenController,
                decoration: InputDecoration(
                  labelText: 'API Token',
                  hintText: 'Paste your Paperless token here',
                  prefixIcon: const Icon(Icons.key_outlined),
                  border: const OutlineInputBorder(),
                  helperText: 'Found in Paperless → My Profile → API Auth Token',
                  suffixIcon: IconButton(
                    icon: Icon(
                      _tokenVisible
                          ? Icons.visibility_off_outlined
                          : Icons.visibility_outlined,
                    ),
                    onPressed: () =>
                        setState(() => _tokenVisible = !_tokenVisible),
                  ),
                ),
                obscureText: !_tokenVisible,
                autocorrect: false,
                enableSuggestions: false,
                onChanged: (_) => _clearTestResult(),
                validator: (v) =>
                    (v == null || v.trim().isEmpty) ? 'Required' : null,
              ),
              const SizedBox(height: 24),

              // ── Home certificate authority ───────────────────────────
              Text(
                'Home certificate authority',
                style: tt.titleSmall?.copyWith(color: cs.primary),
              ),
              const SizedBox(height: 4),
              Row(
                children: [
                  Icon(
                    _caPem == null ? Icons.public : Icons.verified_user_outlined,
                    size: 20,
                    color: cs.primary,
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      _caPem == null
                          ? 'Not set: only publicly trusted certificates are accepted.'
                          : 'Also trusting: ${(_caLabel == null || _caLabel!.isEmpty) ? 'imported certificate' : _caLabel}',
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                children: [
                  OutlinedButton.icon(
                    onPressed: _importCa,
                    icon: const Icon(Icons.file_open_outlined),
                    label: Text(_caPem == null ? 'Import CA file' : 'Replace'),
                  ),
                  if (_caPem != null)
                    TextButton(onPressed: _removeCa, child: const Text('Remove')),
                ],
              ),
              Text(
                'For a home-lab server (e.g. https://paperless.home.lab), import '
                'the lab\'s public root certificate, rootCA.pem. Never import a key.',
                style: tt.bodySmall?.copyWith(color: cs.onSurfaceVariant),
              ),
              const SizedBox(height: 24),

              // ── Test connection ──────────────────────────────────────
              OutlinedButton.icon(
                onPressed: _isTesting ? null : _testConnection,
                icon: _isTesting
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.wifi_tethering),
                label: Text(_isTesting ? 'Testing…' : 'Test Connection'),
              ),

              if (_testResult != null) ...[
                const SizedBox(height: 12),
                _StatusBanner(message: _testResult!, success: _testPassed),
              ],

              const SizedBox(height: 24),
              const Divider(),
              const SizedBox(height: 16),

              // ── Save ─────────────────────────────────────────────────
              FilledButton.icon(
                onPressed: _isSaving ? null : _save,
                icon: _isSaving
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(
                          strokeWidth: 2,
                          color: Colors.white,
                        ),
                      )
                    : const Icon(Icons.save_outlined),
                label: Text(_isSaving ? 'Saving…' : 'Save Settings'),
                style: FilledButton.styleFrom(
                  padding: const EdgeInsets.symmetric(vertical: 14),
                ),
              ),

              const SizedBox(height: 32),

              // ── How to get token ─────────────────────────────────────
              Card(
                color: cs.surfaceContainerLow,
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Icon(Icons.help_outline, size: 18, color: cs.primary),
                          const SizedBox(width: 8),
                          Text(
                            'How to get your API token',
                            style: tt.titleSmall?.copyWith(color: cs.primary),
                          ),
                        ],
                      ),
                      const SizedBox(height: 8),
                      const Text(
                        '1. Open Paperless-ngx in a browser\n'
                        '2. Click your name (top right) → My Profile\n'
                        '3. Under "API Auth Token", create or copy the token',
                        style: TextStyle(height: 1.6),
                      ),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _StatusBanner extends StatelessWidget {
  const _StatusBanner({required this.message, required this.success});

  final String message;
  final bool success;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final color = success ? Colors.green : cs.errorContainer;
    final textColor = success ? Colors.green.shade800 : cs.onErrorContainer;
    final icon = success ? Icons.check_circle_outline : Icons.error_outline;

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.15),
        border: Border.all(color: color.withValues(alpha: 0.4)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          Icon(icon, size: 18, color: textColor),
          const SizedBox(width: 8),
          Expanded(child: Text(message, style: TextStyle(color: textColor))),
        ],
      ),
    );
  }
}
