
// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  // NOTE(rudominer) With the following initialization of logging it is
  // possible to see the output of the FX_VLOGS(6) statements in
  // the code under test by doing the following:
  // (1) In the fuchsia terminal in QEMU:
  //     run-test-suite cobalt_utils_unittests --verbose=6
  // (2) In a bash tab on your host workstation:
  //     fx syslog --tag test --verbosity 6
  const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fxl::SetLogSettingsFromCommandLine(command_line, {"cobalt", "test"});
  return RUN_ALL_TESTS();
}
