// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:test/test.dart';

import 'helpers.dart';

void main() {
  enableLoggingOutput();

  test('fuchsia.audio.mixer', () async {
    await runTestComponent(
        packageName: 'audio_mixer_profiler',
        componentName: 'audio_mixer_profiler.cm',
        commandArgs: '--perftest-json=${PerfTestHelper.componentOutputPath}',
        expectedMetricNamesFile: 'fuchsia.audio.txt');
  }, timeout: Timeout.none);
}
