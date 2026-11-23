import 'package:material_ui/material_ui.dart';
import 'screens/home_screen.dart';
import 'services/background.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await initBackground();
  runApp(const PaperlessScannerApp());
}

class PaperlessScannerApp extends StatelessWidget {
  const PaperlessScannerApp({super.key});

  @override
  Widget build(BuildContext context) {
    ThemeData theme(Brightness b) => ThemeData(
          colorScheme: ColorScheme.fromSeed(
            seedColor: const Color(0xFF1565C0),
            brightness: b,
          ),
          useMaterial3: true,
          appBarTheme: const AppBarTheme(centerTitle: true, elevation: 0),
        );

    return MaterialApp(
      title: 'Paperless Scanner',
      theme: theme(Brightness.light),
      darkTheme: theme(Brightness.dark),
      themeMode: ThemeMode.system,
      home: const HomeScreen(),
      debugShowCheckedModeBanner: false,
    );
  }
}
