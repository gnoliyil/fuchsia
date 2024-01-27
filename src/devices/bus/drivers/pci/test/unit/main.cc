// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>

#include <zxtest/zxtest.h>

#include "src/devices/testing/mock-ddk/mock-device.h"

// This simple main allows for a test to be built that checks for a single '-v'
// argument to turn on a driver's zxlogf levels.
// ie: fx test pci-unit-test -- -v
int main(int argc, char* argv[]) {
  const char* flag = "-v";
  if (!std::memcmp(argv[argc - 1], flag, 2)) {
    mock_ddk::SetMinLogSeverity(FX_LOG_TRACE);
  }
  return RUN_ALL_TESTS(argc, argv);
}
