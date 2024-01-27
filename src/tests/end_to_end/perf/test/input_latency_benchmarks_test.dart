// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:math';

import 'package:sl4f/sl4f.dart';
import 'package:test/test.dart';

import 'helpers.dart';

const String _catapultConverterPath = 'runtime_deps/catapult_converter';
const String _trace2jsonPath = 'runtime_deps/trace2json';

Future<void> _killProcesses(PerfTestHelper helper) async {
  print('Killing processes for input_latency_benchmarks_test');
  await helper.sl4fDriver.ssh.run('killall "scene_manager*"');
  await helper.sl4fDriver.ssh.run('killall "scenic*"');
  await helper.sl4fDriver.ssh.run('killall "basemgr*"');
  await helper.sl4fDriver.ssh.run('killall "*flutter*"');
  await helper.sl4fDriver.ssh.run('killall "tiles*"');
  await helper.sl4fDriver.ssh.run('killall "flatland-view-provider*"');
  await helper.sl4fDriver.ssh.run('component destroy "simplest-app-flatland"');
  print('Finished killing processes for input_latency_benchmarks_test');
}

void main() {
  enableLoggingOutput();

  const testName = 'fuchsia.input_latency.simplest_app';
  test(testName, () async {
    final helper = await PerfTestHelper.make();

    await _killProcesses(helper);

    await helper.component.launchAndDetach(
        'fuchsia-pkg://fuchsia.com/flatland-examples#meta/simplest-app-flatland.cm');

    // Wait for the application to start.
    await Future.delayed(Duration(seconds: 3));

    // Start tracing.
    final traceSession = await helper.performance.initializeTracing(
        categories: ['input', 'gfx', 'magma'], bufferSize: 36);

    await traceSession.start();

    // Each tap will be 33.5ms apart, drifting 0.166ms against regular 60 fps
    // vsync interval. 100 taps span the entire vsync interval 1 time at 100
    // equidistant points.
    final input = Input(helper.sl4fDriver);
    await input.tap(Point<int>(500, 500), tapEventCount: 100, duration: 3350);

    await traceSession.stop();

    final fxtTraceFile = await traceSession.terminateAndDownload(testName);
    final jsonTraceFile = await helper.performance
        .convertTraceFileToJson(_trace2jsonPath, fxtTraceFile);

    final metricsSpecSet = MetricsSpecSet(
      testName: testName,
      metricsSpecs: [
        MetricsSpec(name: 'input_latency'),
      ],
    );

    expect(
        await helper.performance.processTrace(metricsSpecSet, jsonTraceFile,
            converterPath: _catapultConverterPath,
            expectedMetricNamesFile: 'fuchsia.input_latency.simplest_app.txt'),
        isNotNull);

    // Clean up by killing the processes.  The reason for this is that we want
    // to prevent these processes from interfering with later performance
    // tests.
    await _killProcesses(helper);
  });
}
