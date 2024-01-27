// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <stdlib.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/macros.h"
#include "src/performance/trace_manager/app.h"

namespace {

constexpr char kDefaultConfigFile[] = "/pkg/data/tracing.config";

}  // namespace

int main(int argc, char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line)) {
    exit(EXIT_FAILURE);
  }

  auto config_file = command_line.GetOptionValueWithDefault("config", kDefaultConfigFile);

  tracing::Config config;
  if (!config.ReadFrom(config_file)) {
    FX_LOGS(ERROR) << "Failed to read configuration from " << config_file;
    exit(EXIT_FAILURE);
  }

  FX_LOGS(INFO) << "Trace Manager starting with config: " << config_file;

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  tracing::TraceManagerApp trace_manager_app{
      sys::ComponentContext::CreateAndServeOutgoingDirectory(), std::move(config),
      loop.dispatcher()};
  loop.Run();
  return EXIT_SUCCESS;
}
