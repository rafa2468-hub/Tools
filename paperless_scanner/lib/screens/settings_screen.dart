import 'package:flutter/material.dart';
import '../services/paperless_service.dart';
import '../services/storage_service.dart';

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
  final _paperless = PaperlessService();

  bool _tokenVisible = false;
  bool _isTesting = false;
  bool _isSaving = false;
  bool _allowInsecure = false;
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
    final url = await _storage.getServerUrl();
    final token = await _storage.getToken();
    final allowInsecure = await _storage.getAllowInsecure();
    if (mounted) {
      _urlController.text = url ?? '';
      _tokenController.text = token ?? '';
      setState(() => _allowInsecure = allowInsecure);
    }
  }

  Future<void> _testConnection() async {
    if (!_formKey.currentState!.validate()) return;

    // Save temporarily so the service can read the values
    await _storage.saveSettings(
      serverUrl: _urlController.text,
      token: _tokenController.text,
      allowInsecure: _allowInsecure,
    );

    setState(() {
      _isTesting = true;
      _testResult = null;
    });

    final result = await _paperless.testConnection();

    if (!mounted) return;
    setState(() {
      _isTesting = false;
      _testPassed = result.ok;
      _testResult = result.ok ? 'Connected successfully!' : result.error;
    });
  }

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;

    setState(() => _isSaving = true);
    await _storage.saveSettings(
      serverUrl: _urlController.text,
      token: _tokenController.text,
      allowInsecure: _allowInsecure,
    );
    if (!mounted) return;
    setState(() => _isSaving = false);

    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Settings saved')),
    );
    Navigator.pop(context);
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;

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
                style: Theme.of(context).textTheme.titleMedium?.copyWith(
                      color: cs.primary,
                      fontWeight: FontWeight.bold,
                    ),
              ),
              const SizedBox(height: 12),
              TextFormField(
                controller: _urlController,
                decoration: const InputDecoration(
                  labelText: 'Server URL',
                  hintText: 'https://paperless.example.com',
                  prefixIcon: Icon(Icons.dns_outlined),
                  border: OutlineInputBorder(),
                  helperText: 'No trailing slash needed',
                ),
                keyboardType: TextInputType.url,
                autocorrect: false,
                validator: (v) {
                  if (v == null || v.trim().isEmpty) return 'Required';
                  final uri = Uri.tryParse(v.trim());
                  if (uri == null || !uri.hasScheme) {
                    return 'Enter a valid URL (include https://)';
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
                  helperText: 'Found in Paperless → Settings → API Token',
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
                validator: (v) =>
                    (v == null || v.trim().isEmpty) ? 'Required' : null,
              ),
              const SizedBox(height: 8),

              // ── Self-signed cert toggle ──────────────────────────────
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                value: _allowInsecure,
                onChanged: (v) => setState(() => _allowInsecure = v),
                title: const Text('Allow self-signed certificates'),
                subtitle: Text(
                  'Enable for home labs with private CAs or self-signed TLS certs.',
                  style: TextStyle(color: cs.onSurface.withOpacity(0.6)),
                ),
                secondary: Icon(
                  _allowInsecure ? Icons.lock_open_outlined : Icons.lock_outline,
                  color: _allowInsecure ? cs.tertiary : cs.primary,
                ),
              ),
              const SizedBox(height: 16),

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
                            style: Theme.of(context)
                                .textTheme
                                .titleSmall
                                ?.copyWith(color: cs.primary),
                          ),
                        ],
                      ),
                      const SizedBox(height: 8),
                      const Text(
                        '1. Open Paperless-ngx in a browser\n'
                        '2. Go to Settings → Users → your user\n'
                        '3. Copy the token shown under "API Token"',
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
        color: color.withOpacity(0.15),
        border: Border.all(color: color.withOpacity(0.4)),
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
