// Copyright 2023 The Fuchsia Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// @dart=2.12

import 'package:test/test.dart';

import 'helpers.dart';

void main() {
  enableLoggingOutput();

  test('extended_pstate', () async {
    await runTestComponent(
        packageName: 'extended_pstate',
        componentName: 'extended_pstate_bench.cm',
        commandArgs: PerfTestHelper.componentOutputPath,
        expectedMetricNamesFile: 'fuchsia.extended_pstate.txt');
  }, timeout: Timeout.none);
}
