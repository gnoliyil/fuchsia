// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:test/test.dart';
import 'helpers.dart';

// This runs prebuilt gVisor benchmarks on Starnix.
// The benchmarks come from here:
// https://github.com/google/gvisor/tree/master/test/perf/linux

void main() {
  const starnix_gvisor_benchmarks = 'starnix_gvisor_benchmarks';

  enableLoggingOutput();

  const benchmarks = {
    'clock_getres_benchmark.cm':
        'fuchsia.starnix.gvisor_benchmarks.clock_getres.txt',
    'clock_gettime_benchmark.cm':
        'fuchsia.starnix.gvisor_benchmarks.clock_gettime.txt',
    // 'death_benchmark' - not passing
    'dup_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.dup.txt',
    'epoll_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.epoll.txt',
    'fork_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.fork.txt',
    // 'futex_benchmark' - not passing
    'getdents_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.getdents.txt',
    'getpid_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.getpid.txt',
    // 'gettid_benchmark' - long running
    'mapping_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.mapping.txt',
    'open_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.open.txt',
    'open_read_close_benchmark.cm':
        'fuchsia.starnix.gvisor_benchmarks.open_read_close.txt',
    'pipe_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.pipe.txt',
    'randread_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.randread.txt',
    'read_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.read.txt',
    // 'sched_yield_benchmark.cm' - long running
    // 'send_recv_benchmar'k - not passing
    'seqwrite_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.seqwrite.txt',
    'signal_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.signal.txt',
    'sleep_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.sleep.txt',
    'stat_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.stat.txt',
    'unlink_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.unlink.txt',
    'write_benchmark.cm': 'fuchsia.starnix.gvisor_benchmarks.write.txt',
  };

  const filters = {
    // TODO(b/275745984): Remove when BM_ThreadSwitch passes.
    'fork_benchmark.cm': '--benchmark_filter_internal="'
        'BM_CPUBoundSymmetric'
        '|BM_CPUBoundUniprocess'
        '|BM_CPUBoundAsymmetric'
        '|BM_ProcessSwitch'
        '|BM_ThreadStart'
        '|BM_ProcessLifecycle"',
  };

  benchmarks.forEach((String componentName, String expectedMetricNamesFile) {
    var commandArgs = '';
    if (filters.containsKey(componentName)) {
      commandArgs = filters[componentName];
    }

    test(starnix_gvisor_benchmarks, () async {
      await runTestComponent(
          packageName: starnix_gvisor_benchmarks,
          componentName: componentName,
          commandArgs: commandArgs,
          expectedMetricNamesFile: expectedMetricNamesFile);
    }, timeout: Timeout.none);
  });
}
