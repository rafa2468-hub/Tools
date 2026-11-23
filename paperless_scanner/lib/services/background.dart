import 'package:flutter/widgets.dart';
import 'package:workmanager/workmanager.dart';
import 'app_files.dart';
import 'paperless_service.dart';
import 'queue_runner.dart';
import 'storage_service.dart';

/// Android job that uploads waiting scans while the app is closed. It runs
/// about every 15 minutes, only on an unmetered network (in practice Wi-Fi),
/// and only while something is waiting: it cancels itself when the queue
/// is empty.
const drainJob = 'paperless-upload-queue';

@pragma('vm:entry-point')
void backgroundDispatcher() {
  Workmanager().executeTask((task, inputData) async {
    try {
      await drainFromStoredSettings();
    } on Object {
      // Nothing useful to report from here; the next run tries again.
    }
    // Always "done": the periodic schedule is the retry.
    return true;
  });
}

/// Uploads what's waiting with the saved settings. Used by the background
/// job and by the app itself (on open, on return, "Upload now").
Future<DrainSummary?> drainFromStoredSettings() async {
  WidgetsFlutterBinding.ensureInitialized();
  final config = await StorageService().load();
  final queue = await AppFiles.queue();
  if (!config.isComplete) return null;
  final summary = await drainQueue(queue, PaperlessService(config));
  if ((await queue.waiting()).isEmpty) await cancelBackgroundUploads();
  return summary;
}

Future<void> initBackground() async {
  try {
    await Workmanager().initialize(backgroundDispatcher);
  } on Object {
    // Background uploads are a convenience; the app still drains on open.
  }
}

Future<void> scheduleBackgroundUploads() async {
  try {
    await Workmanager().registerPeriodicTask(
      drainJob,
      drainJob,
      frequency: const Duration(minutes: 15),
      constraints: Constraints(networkType: NetworkType.unmetered),
      existingWorkPolicy: ExistingPeriodicWorkPolicy.keep,
    );
  } on Object {
    // see initBackground
  }
}

Future<void> cancelBackgroundUploads() async {
  try {
    await Workmanager().cancelByUniqueName(drainJob);
  } on Object {
    // see initBackground
  }
}
