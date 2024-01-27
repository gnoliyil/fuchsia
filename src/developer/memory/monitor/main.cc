// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/scheduler/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace-provider/provider.h>

#include <filesystem>
#include <system_error>

#include "src/developer/memory/monitor/monitor.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"

namespace {
const char kRamDeviceClassPath[] = "/dev/class/aml-ram";
void SetRamDevice(monitor::Monitor* app) {
  // Look for optional RAM device that provides bandwidth measurement interface.
  fuchsia::hardware::ram::metrics::DevicePtr ram_device;
  std::error_code ec;
  // Use the noexcept version of std::filesystem::exists.
  if (std::filesystem::exists(kRamDeviceClassPath, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(kRamDeviceClassPath)) {
      int fd = open(entry.path().c_str(), O_RDONLY);
      if (fd > -1) {
        zx::channel handle;
        zx_status_t status = fdio_get_service_handle(fd, handle.reset_and_get_address());
        if (status == ZX_OK) {
          ram_device.Bind(std::move(handle));
          app->SetRamDevice(std::move(ram_device));
          FX_LOGS(INFO) << "Will collect memory bandwidth measurements.";
          return;
        }
        break;
      }
    }
  }
  FX_LOGS(INFO) << "CANNOT collect memory bandwidth measurements. error_code: " << ec;
}
const char kNotfiyCrashReporterPath[] = "/config/data/send_critical_pressure_crash_reports";
bool SendCriticalMemoryPressureCrashReports() {
  std::error_code _ignore;
  return std::filesystem::exists(kNotfiyCrashReporterPath, _ignore);
}
}  // namespace

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line, {"memory_monitor"}))
    return 1;

  FX_VLOGS(2) << argv[0] << ": starting";

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher(), monitor::Monitor::kTraceName);
  std::unique_ptr<sys::ComponentContext> startup_context =
      sys::ComponentContext::CreateAndServeOutgoingDirectory();

  // Lower the priority.
  fuchsia::scheduler::ProfileProviderSyncPtr profile_provider;
  startup_context->svc()->Connect<fuchsia::scheduler::ProfileProvider>(
      profile_provider.NewRequest());
  zx_status_t fidl_status;
  zx::profile profile;
  auto status =
      profile_provider->GetProfile(8 /* LOW_PRIORITY */, "memory_monitor", &fidl_status, &profile);
  FX_CHECK(status == ZX_OK) << "status: " << zx_status_get_string(status);
  FX_CHECK(fidl_status == ZX_OK) << "fidl_status: " << zx_status_get_string(fidl_status);
  auto set_status = zx_object_set_profile(zx_thread_self(), profile.get(), 0);
  FX_CHECK(set_status == ZX_OK) << "set_status: " << zx_status_get_string(set_status);

  monitor::Monitor app(std::move(startup_context), command_line, loop.dispatcher(),
                       true /* send_metrics */, true /* watch_memory_pressure */,
                       SendCriticalMemoryPressureCrashReports());
  SetRamDevice(&app);
  loop.Run();

  FX_VLOGS(2) << argv[0] << ": exiting";

  return 0;
}
