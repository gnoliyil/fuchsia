// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:test/test.dart';

import 'helpers.dart';

void main() {
  enableLoggingOutput();

  test('network_device_microbenchmarks', () async {
    await runTestComponent(
        packageName: 'network-device-microbenchmarks',
        componentName: 'network-device-microbenchmarks.cm',
        commandArgs: '-p --quiet --out ${PerfTestHelper.componentOutputPath}',
        expectedMetricNamesFile: 'fuchsia.network.device.txt');
  }, timeout: Timeout.none);
}
