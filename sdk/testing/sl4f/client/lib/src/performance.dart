// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// @dart=2.12

import 'dart:convert';
import 'dart:io' show File, Platform, Process, ProcessResult, WebSocket;

import 'package:logging/logging.dart';
import 'package:path/path.dart' as path;

import 'dump.dart';
import 'performance_publish.dart';
import 'sl4f_client.dart';
import 'trace_processing/metrics_spec.dart';
import 'trace_processing/trace_importing.dart';

String _traceExtension({required bool binary, required bool compress}) {
  String extension = 'json';
  if (binary) {
    extension = 'fxt';
  }
  if (compress) {
    extension += '.gz';
  }
  return extension;
}

File _replaceExtension(File file, String newExtension) {
  String basePath = file.path;
  final firstExtension = path.extension(basePath);
  basePath = path.withoutExtension(basePath);
  if (firstExtension == '.gz') {
    basePath = path.withoutExtension(basePath);
  }
  return File('$basePath.$newExtension');
}

// Chromium tracing tools requires only one "Compositor", "CrBrowserMain",
// "CrRendererMain" and "VizCompositorThread" appears in the tracing
// json file, so we need to remove these names in the Fuchsia trace files
// to avoid name collision.
//
// Note that this function mutates the value of argument [rootTraceObject].
Map<String, dynamic> _renameChromiumProcessesInFuchsiaTrace(
    Map<String, dynamic> rootTraceObject) {
  if (!rootTraceObject.containsKey('systemTraceEvents') ||
      !rootTraceObject['systemTraceEvents'].containsKey('events')) {
    return rootTraceObject;
  }
  final List<Map<String, dynamic>> events =
      rootTraceObject['systemTraceEvents']['events'];
  for (final Map<String, dynamic> event in events) {
    if (event['ph'] == 't' && event.containsKey('name')) {
      final String? name = event['name'];
      if (name == 'Compositor' ||
          name == 'CrBrowserMain' ||
          name == 'CrRendererMain' ||
          name == 'VizCompositorThread') {
        event['name'] = '${name}_Fuchsia';
      }
    }
  }
  return rootTraceObject;
}

final _log = Logger('Performance');

/// Manages system-wide performance tracing and processing.
///
/// Use [initializeTracing] to initialize the tracing subsystem on specific
/// categories, along with [TraceSession] to start the tracing on the action you
/// want to measure. Then use the various convert and process methods to further
/// analyze the raw traces.
///
/// See https://fuchsia.dev/fuchsia-src/concepts/tracing for more on tracing
/// concepts.
class Performance {
  final Sl4f _sl4f;
  final Dump _dump;

  /// Constructs a [Performance] object.
  Performance(this._sl4f, [Dump? dump]) : _dump = dump ?? Dump();

  /// Closes the underlying HTTP client.
  ///
  /// This need not be called if the Sl4f client is closed instead.
  void close() {
    _sl4f.close();
  }

  /// Initialize system-wide tracing subsystem and get a [TraceSession] object
  /// to manage the session and get the results.
  ///
  /// There can only be one trace session going at the same time.
  Future<TraceSession> initializeTracing(
      {List<String>? categories, int? bufferSize}) async {
    _log.info('Performance: Initializing trace session');
    final params = {};
    if (categories != null) {
      params['categories'] = categories;
    }
    if (bufferSize != null) {
      params['buffer_size'] = bufferSize;
    }
    await _sl4f.request('tracing_facade.Initialize', params);
    return TraceSession(_sl4f, _dump);
  }

  /// Terminate all existing trace sessions without collecting trace data.
  Future<void> terminateExistingTraceSession() async {
    _log.info('Performance: Terminating any existing trace session');
    await _sl4f
        .request('tracing_facade.Terminate', {'results_destination': 'Ignore'});
  }

  /// Starts a Chrome trace from the given [webSocketUrl] with the default
  /// categories.
  ///
  /// [webSocketUrl] can be obtained from
  /// [Webdriver.webSocketDebuggerUrlsForHost]. Returns a WebSocket object that
  /// is to be passed to [stopChromeTrace] to stop and download the trace data.
  ///
  /// TODO(fxbug.dev/35714): Allow tracing users to specify categories to trace.
  Future<WebSocket> startChromeTrace(String webSocketUrl) async {
    final webSocket = await WebSocket.connect(webSocketUrl);
    _log.info('Starting chrome trace');
    webSocket.add(json.encode({
      'jsonrpc': '2.0',
      'method': 'Tracing.start',
      'params': {},
      'id': 1,
    }));
    return webSocket;
  }

  /// Stops a Chrome trace that was started by [startChromeTrace] and writes it
  /// to a file.
  ///
  /// Returns the file containing the trace data. Calling [stopChromeTrace] on
  /// the same [webSocket] twice will throw an error.
  Future<File?> stopChromeTrace(WebSocket webSocket,
      {required String traceName}) async {
    _log.info('Stopping and saving chrome trace');
    webSocket.add(json.encode({
      'jsonrpc': '2.0',
      'method': 'Tracing.end',
      'params': {},
      'id': 2,
    }));

    final traceEvents = [];

    await for (final content in webSocket) {
      final obj = json.decode(content);
      if (obj['method'] == 'Tracing.tracingComplete') {
        break;
      } else if (obj['method'] == 'Tracing.dataCollected') {
        traceEvents.addAll(obj['params']['value']);
      }
    }
    await webSocket.close();

    _log.info('Writing chrome trace to file');
    return _dump.writeAsBytes('$traceName-chrome-trace', 'json',
        utf8.encode(json.encode(traceEvents)));
  }

  /// Combine [fuchsiaTrace] and [chromeTrace] into a merged JSON-format trace.
  ///
  /// [fuchsiaTrace] must be a trace file in JSON format (not FXT).
  Future<File?> mergeTraces(
      {required File fuchsiaTrace,
      required File chromeTrace,
      required String traceName}) async {
    final fuchsiaTraceData = _renameChromiumProcessesInFuchsiaTrace(
        json.decode(await fuchsiaTrace.readAsString()));
    final chromeTraceData = json.decode(await chromeTrace.readAsString());

    final mergedTraceData = fuchsiaTraceData;
    mergedTraceData['traceEvents'].addAll(chromeTraceData);

    return _dump.writeAsBytes('$traceName-merged-trace', 'json',
        utf8.encode(json.encode(mergedTraceData)));
  }

  /// A helper function that runs a process with the given args.
  ///
  /// Used by the test to capture the parameters passed to [Process.run].
  ///
  /// Returns [true] if the process ran successfully, [false] otherwise.
  Future<bool> runProcess(String executablePath, List<String> args) async {
    _log.info('Performance: Running $executablePath ${args.join(" ")}');
    final ProcessResult results = await Process.run(executablePath, args);
    _log
      ..info(results.stdout)
      ..info(results.stderr);
    return results.exitCode == 0;
  }

  /// Convert the specified [traceFile] from fxt or fxt.gz to json or json.gz.
  ///
  /// In typical uses, [traceFile] should be the return value of a call to
  /// [TraceSession.terminateAndDownload].
  ///
  /// By default, this function guesses whether the input is compressed by
  /// examining [traceFile]'s extension. This can be overridden by passing a
  /// value for [compressedInput]. If [compressedOutput] is set to true, then
  /// this will produce a json.gz file instead of a json file.
  ///
  /// Returns the [File] generated by trace2json.
  Future<File?> convertTraceFileToJson(String trace2jsonPath, File traceFile,
      {bool? compressedInput, bool compressedOutput = false}) async {
    _log.info('Performance: Converting ${traceFile.absolute.path} to json');
    final String outputExtension =
        _traceExtension(binary: false, compress: compressedOutput);
    final File outputFile = _replaceExtension(traceFile, outputExtension);
    final args = [
      '--input-file=${traceFile.path}',
      '--output-file=${outputFile.path}',
    ];
    if (compressedInput ?? path.extension(traceFile.path) == '.gz') {
      args.add('--compressed-input');
    }
    if (compressedOutput) {
      args.add('--compressed-output');
    }
    final trace2json = Platform.script.resolve(trace2jsonPath).toFilePath();
    if (!await runProcess(trace2json, args)) {
      return null;
    }
    return outputFile;
  }

  /// Convert [traceData] from fxt format to json.
  ///
  /// In typical uses, [traceData] will be the return value of a call to
  /// [TraceSession.terminateAndDownloadAsBytes].
  Future<String> convertTraceDataToJson(
      String trace2jsonPath, List<int> traceData) async {
    _log.info('Performance: Converting in-memory trace to json');
    final trace2json = Platform.script.resolve(trace2jsonPath).toFilePath();
    final trace2jsonProcess = await Process.start(trace2json, []);
    final output = trace2jsonProcess.stdout.transform(utf8.decoder).join('');
    trace2jsonProcess.stdin.add(traceData);
    await trace2jsonProcess.stdin.close();
    // Start the stderr consumer to ensure that the trace2json process does not
    // block on writing to stderr.
    final stderr = trace2jsonProcess.stderr.transform(utf8.decoder).join('');
    final exitCode = await trace2jsonProcess.exitCode;
    if (exitCode != 0) {
      throw Exception('trace2json exit code $exitCode stderr: ${await stderr}');
    }
    await stderr;
    return (await output);
  }

  /// Runs the provided [MetricsSpecSet] on the given [trace].
  /// It sets the output file location to be the same as the source.
  /// It will also run the catapult converter if the [converterPath] was provided.
  ///
  /// The [converterPath] must be relative to the script path.
  ///
  /// [registry] defines the set of known metrics processors, which can be
  /// specified to allow processing of custom metrics.
  ///
  /// TODO(fxbug.dev/23077): Avoid explicitly passing the [converterPath].
  ///
  /// Returns the benchmark result [File] generated by the processor.
  Future<File> processTrace(MetricsSpecSet metricsSpecSet, File trace,
      {String? converterPath,
      Map<String, MetricsProcessor> registry = defaultMetricsRegistry,
      String? expectedMetricNamesFile}) async {
    _log.info('Processing trace: ${trace.path}');
    final outputFileName =
        '${trace.parent.absolute.path}/${metricsSpecSet.testName}-benchmark.fuchsiaperf.json';

    final model = await createModelFromFile(trace);
    final List<Map<String, dynamic>> results = [];

    for (final metricsSpec in metricsSpecSet.metricsSpecs) {
      _log.info('Applying metricsSpec ${metricsSpec.name} to ${trace.path}');
      final testCaseResultss =
          processMetrics(model, metricsSpec, registry: registry);
      for (final testCaseResults in testCaseResultss) {
        results
            .add(testCaseResults.toJson(testSuite: metricsSpecSet.testName!));
      }
    }

    File(outputFileName)
      ..createSync()
      ..writeAsStringSync(json.encode(results));

    File processedResultFile = File(outputFileName);
    _log.info('Processing trace completed.');
    if (converterPath != null) {
      await PerformancePublish().convertResults(
          converterPath, processedResultFile, Platform.environment,
          expectedMetricNamesFile: expectedMetricNamesFile);
    }
    return processedResultFile;
  }

  /// Starts logging temperature data into the temperature_logger trace
  /// category.
  ///
  /// The temperature sensors will be polled and a trace event produced after
  /// each [interval] amount of time. If [duration] is specified, then logging
  /// will automatically stop after that amount of time has elapsed. If
  /// [duration] is not specified, then logging will continue until explicitly
  /// stopped.
  ///
  /// This function will fail if logging is already started.
  Future<void> startTemperatureLogging({
    required Duration interval,
    Duration? duration,
  }) async {
    _log.info(
        'Start temperature logging: interval $interval duration $duration');
    if (duration == null) {
      await _sl4f.request('temperature_facade.StartLoggingForever', {
        'interval_ms': interval.inMilliseconds,
      });
    } else {
      await _sl4f.request('temperature_facade.StartLogging', {
        'interval_ms': interval.inMilliseconds,
        'duration_ms': duration.inMilliseconds,
      });
    }
  }

  /// Stops logging temperature data in temperature_logger trace category.
  ///
  /// This function will still succeed if logging is already stopped.
  Future<void> stopTemperatureLogging() async {
    _log.info('Stop temperature logging');
    await _sl4f.request('temperature_facade.StopLogging', {});
  }

  /// Starts logging system metrics data into the system_metrics_logger trace
  /// category.
  ///
  /// The metrics will be polled and a trace event produced after each
  /// [interval] amount of time. If [duration] is specified, then logging will
  /// automatically stop after that amount of time has elapsed. If [duration] is
  /// not specified, then logging will continue until explicitly stopped.
  ///
  /// This function will fail if logging is already started.
  Future<void> startSystemMetricsLogging({
    required Duration interval,
    Duration? duration,
  }) async {
    _log.info(
        'Start system metrics logging: interval $interval duration $duration');
    if (duration == null) {
      await _sl4f.request('system_metrics_facade.StartLoggingForever', {
        'interval_ms': interval.inMilliseconds,
      });
    } else {
      await _sl4f.request('system_metrics_facade.StartLogging', {
        'interval_ms': interval.inMilliseconds,
        'duration_ms': duration.inMilliseconds,
      });
    }
  }

  /// Stops logging system metrics data in system_metrics_logger trace category.
  ///
  /// This function will still succeed if logging is already stopped.
  Future<void> stopSystemMetricsLogging() async {
    _log.info('Stop system metrics logging');
    await _sl4f.request('system_metrics_facade.StopLogging', {});
  }

  /// Starts logging power data into the metrics_logger trace category.
  ///
  /// The power sensors will be polled and a trace event produced after each
  /// `interval` amount of time. If `duration` is specified, then logging will
  /// automatically stop after that amount of time has elapsed. If `duration` is
  /// not specified, then logging will continue until explicitly stopped.
  ///
  /// This function will fail if logging is already started.
  Future<void> startPowerLogging({
    required Duration interval,
    Duration? duration,
  }) async {
    _log.info('Start power logging: interval $interval duration $duration');
    if (duration == null) {
      await _sl4f.request('power_facade.StartLoggingForever', {
        'sampling_interval_ms': interval.inMilliseconds,
      });
    } else {
      await _sl4f.request('power_facade.StartLogging', {
        'sampling_interval_ms': interval.inMilliseconds,
        'duration_ms': duration.inMilliseconds,
      });
    }
  }

  /// Stops logging power data in metrics_logger trace category.
  ///
  /// This function will still succeed if logging is already stopped.
  Future<void> stopPowerLogging() async {
    _log.info('Stop power logging');
    await _sl4f.request('power_facade.StopLogging', {});
  }

  Future<void> convertResults(
      String converterPath, File result, Map<String, String> environment,
      {String? expectedMetricNamesFile}) async {
    return PerformancePublish().convertResults(
        converterPath, result, environment,
        expectedMetricNamesFile: expectedMetricNamesFile);
  }
}

/// Handle a tracing session.
class TraceSession {
  final Sl4f _sl4f;
  final Dump _dump;
  bool _closed;

  TraceSession(this._sl4f, this._dump) : _closed = false;

  bool get closed => _closed;

  /// Start tracing.
  Future<void> start() async {
    if (_closed) {
      throw StateError('Cannot start: Session already terminated');
    }
    _log.info('Tracing: starting trace');
    await _sl4f.request('tracing_facade.Start');
  }

  /// Stop tracing.
  Future<void> stop() async {
    if (_closed) {
      throw StateError('Cannot stop: Session already terminated');
    }
    _log.info('Tracing: stopping trace');
    await _sl4f.request('tracing_facade.Stop');
  }

  /// Terminate the trace session and download the trace data, returning a
  /// [File] object with the Fuchsia trace format data.
  ///
  /// After a call to [terminateAndDownload], further calls on the
  /// [TraceSession] object will throw a [StateError].
  Future<File?> terminateAndDownload(String traceName) async {
    final traceData = await terminateAndDownloadAsBytes();
    return _dump.writeAsBytes('$traceName-trace', 'fxt', traceData);
  }

  /// Terminate the trace session and download the trace data, returning a list
  /// of bytes in Fuchsia trace format.
  ///
  /// After a call to [terminateAndDownloadAsBytes], further calls on the
  /// [TraceSession] object will throw a [StateError].
  Future<List<int>> terminateAndDownloadAsBytes() async {
    if (_closed) {
      throw StateError('Cannot terminate: Session already terminated');
    }
    _log.info('Tracing: terminating trace');
    final response = await _sl4f.request('tracing_facade.Terminate');
    _closed = true;
    final traceData = base64.decode(response['data']);
    return traceData;
  }
}
