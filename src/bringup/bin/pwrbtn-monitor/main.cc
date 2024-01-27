// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.input/cpp/wire.h>
#include <fidl/fuchsia.hardware.power.statecontrol/cpp/markers.h>
#include <fidl/fuchsia.hardware.power.statecontrol/cpp/wire.h>
#include <fidl/fuchsia.kernel/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/svc/outgoing.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/channel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <thread>
#include <unordered_map>
#include <utility>

#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <hid-parser/parser.h>
#include <hid-parser/usages.h>
#include <src/sys/lib/stdout-to-debuglog/cpp/stdout-to-debuglog.h>

#include "src/bringup/bin/pwrbtn-monitor/monitor.h"
#include "src/bringup/bin/pwrbtn-monitor/oom_watcher.h"

#define INPUT_PATH "/input"

namespace {
namespace statecontrol_fidl = fuchsia_hardware_power_statecontrol;

bool usage_eq(const hid::Usage& u1, const hid::Usage& u2) {
  return u1.page == u2.page && u1.usage == u2.usage;
}

// Search the report descriptor for a System Power Down input field within a
// Generic Desktop:System Control collection.
//
// This method assumes the HID descriptor does not contain more than one such field.
bool FindSystemPowerDown(const hid::DeviceDescriptor* desc, uint8_t* report_id,
                         size_t* bit_offset) {
  const hid::Usage system_control = {
      .page = static_cast<uint16_t>(hid::usage::Page::kGenericDesktop),
      .usage = static_cast<uint32_t>(hid::usage::GenericDesktop::kSystemControl),
  };

  const hid::Usage power_down = {
      .page = static_cast<uint16_t>(hid::usage::Page::kGenericDesktop),
      .usage = static_cast<uint32_t>(hid::usage::GenericDesktop::kSystemPowerDown),
  };

  // Search for the field
  for (size_t rpt_idx = 0; rpt_idx < desc->rep_count; ++rpt_idx) {
    const hid::ReportDescriptor& report = desc->report[rpt_idx];

    for (size_t i = 0; i < report.input_count; ++i) {
      const hid::ReportField& field = report.input_fields[i];

      if (!usage_eq(field.attr.usage, power_down)) {
        continue;
      }

      const hid::Collection* collection = hid::GetAppCollection(&field);
      if (!collection || !usage_eq(collection->usage, system_control)) {
        continue;
      }
      *report_id = field.report_id;
      *bit_offset = field.attr.offset;
      return true;
    }
  }
  return false;
}

struct PowerButtonInfo {
  fidl::WireSyncClient<fuchsia_hardware_input::Device> client;
  uint8_t report_id;
  size_t bit_offset;
  bool has_report_id_byte;
};

zx_status_t InputDeviceAdded(int dirfd, int event, const char* name, void* cookie) {
  if (event != WATCH_EVENT_ADD_FILE) {
    return ZX_OK;
  }
  if (std::string_view{name} == ".") {
    return ZX_OK;
  }

  fdio_cpp::UnownedFdioCaller caller(dirfd);
  zx::result controller =
      component::ConnectAt<fuchsia_hardware_input::Controller>(caller.directory(), name);
  if (controller.is_error()) {
    return controller.error_value();
  }
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_input::Device>();
  if (endpoints.is_error()) {
    return endpoints.error_value();
  }
  auto& [device, server] = endpoints.value();
  const fidl::Status status = fidl::WireCall(controller.value())->OpenSession(std::move(server));
  if (!status.ok()) {
    return status.status();
  }

  fidl::WireSyncClient client(std::move(device));
  // Get the report descriptor.
  auto result = client->GetReportDesc();
  if (result.status() != ZX_OK) {
    return ZX_OK;
  }

  hid::DeviceDescriptor* desc;
  if (hid::ParseReportDescriptor(result->desc.data(), result->desc.count(), &desc) !=
      hid::kParseOk) {
    return ZX_OK;
  }
  auto cleanup_desc = fit::defer([desc]() { hid::FreeDeviceDescriptor(desc); });

  uint8_t report_id;
  size_t bit_offset;
  if (!FindSystemPowerDown(desc, &report_id, &bit_offset)) {
    return ZX_OK;
  }

  auto info = reinterpret_cast<PowerButtonInfo*>(cookie);
  info->client = std::move(client);
  info->report_id = report_id;
  info->bit_offset = bit_offset;
  info->has_report_id_byte = (desc->rep_count > 1 || desc->report[0].report_id != 0);
  return ZX_ERR_STOP;
}

// Open the input directory, wait for the proper input device type to appear,
// and parse out information about the input event itself.
// Args:
//   - event_out: should point to a zx::event that will be populated
//   - info_out: should point to PowerButtonInfo object which will be populated
// Errors:
//   - ZX_ERR_INTERNAL: if the input directory can not be opened
//   - other: errors returned by `fdio_watch_directory`, other than
//            `ZX_ERR_STOP`
zx_status_t GetButtonReportEvent(zx::event* event_out, PowerButtonInfo* info_out) {
  fbl::unique_fd dirfd;
  {
    int fd = open(INPUT_PATH, O_DIRECTORY);
    if (fd < 0) {
      printf("pwrbtn-monitor: Failed to open " INPUT_PATH ": %d\n", errno);
      // TODO(jmatt) is this the right failure code?
      return ZX_ERR_INTERNAL;
    }
    dirfd.reset(fd);
  }

  zx_status_t status =
      fdio_watch_directory(dirfd.get(), InputDeviceAdded, ZX_TIME_INFINITE, info_out);
  if (status != ZX_ERR_STOP) {
    printf("pwrbtn-monitor: Failed to find power button device\n");
    return status;
  }
  dirfd.reset();

  auto& client = info_out->client;

  // Get the report event.
  auto result = client->GetReportsEvent();
  if (result.status() != ZX_OK) {
    printf("pwrbtn-monitor: failed to get report event: %d\n", result.status());
    return result.status();
  }
  if (result->status != ZX_OK) {
    printf("pwrbtn-monitor: failed to get report event: %d\n", result->status);
    return result->status;
  }
  *event_out = std::move(result->event);
  return ZX_OK;
}

// Processes a power button event, dispatches events appropriately to
// listeners, and quits the execution look if reading a report fails
void ProcessPowerEvent(zx::event* report_event, pwrbtn::PowerButtonMonitor* monitor,
                       PowerButtonInfo* info, zx_status_t status, async::Loop* loop) {
  if (status == ZX_ERR_CANCELED) {
    return;
  }
  auto result = info->client->ReadReport();
  if (result.status() != ZX_OK) {
    printf("pwrbtn-monitor: failed to read report: %d\n", result.status());
    loop->Quit();
    return;
  }
  if (result->status != ZX_OK) {
    printf("pwrbtn-monitor: failed to read report: %d\n", result->status);
    loop->Quit();
    return;
  }

  // Ignore reports from different report IDs
  const fidl::VectorView<uint8_t>& report = result->data;
  if (info->has_report_id_byte && report[0] != info->report_id) {
    printf("pwrbtn-monitor: input-watcher: wrong id\n");
    return;
  }

  // Check if the power button is pressed, and request a poweroff if so.
  const size_t byte_index = info->has_report_id_byte + info->bit_offset / 8;
  if (report[byte_index] & (1u << (info->bit_offset % 8))) {
    // Sends a Press event to clients, regardless of the Action set.
    // Also keep going to |DoAction| even if button event sending failed.
    (void)monitor->SendButtonEvent(fuchsia_power_button::wire::PowerButtonEvent::kPress);

    auto status = monitor->DoAction();
    if (status != ZX_OK) {
      printf("pwrbtn-monitor: input-watcher: failed to handle press.\n");
      return;
    }
  }
}

// Get the root job from the root job service.
zx::result<zx::job> GetRootJob() {
  auto connect_result = component::Connect<fuchsia_kernel::RootJob>();

  if (connect_result.is_error()) {
    return zx::error(connect_result.status_value());
  }

  auto response = fidl::WireCall(connect_result.value())->Get();
  if (response.status() != ZX_OK) {
    printf("pwrbtn-monitor: Service didn't give us the root job: %u\n", response.status());
    return zx::error(response.status());
  }

  zx::job root_job = std::move(response.value().job);
  return zx::ok(std::move(root_job));
}

// Gets a handle to the system's OOM event and places it in the `zx::event`
// object pointed to by the `event_handle_out` parameter.
zx_status_t GetOomEvent(zx::job& root_job, zx::event* event_handle_out) {
  return zx_system_get_event(root_job.get(), ZX_SYSTEM_EVENT_OUT_OF_MEMORY,
                             event_handle_out->reset_and_get_address());
}

bool StartOomWatcher(pwrbtn::OomWatcher* watcher, async_dispatcher_t* dispatcher) {
  auto pwr_client_req = component::Connect<statecontrol_fidl::Admin>();
  if (!pwr_client_req.is_ok()) {
    return false;
  }

  zx::event event_handle;
  zx::result get_root_job = GetRootJob();
  if (!get_root_job.is_ok()) {
    printf("pwrbtn-monitor: failed to get root job, OOM events will not be monitored: %s\n",
           get_root_job.status_string());
    return false;
  }
  zx::job& root_job = get_root_job.value();

  zx_status_t status = GetOomEvent(root_job, &event_handle);
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: couldn't get the OOM system event: %u\n", status);
    return false;
  }
  status =
      watcher->WatchForOom(dispatcher, std::move(event_handle), std::move(pwr_client_req.value()));
  if (status != ZX_OK) {
    printf("pwrbtn-monitor: OOM watcher object failed to initialize: %u\n", status);
    return false;
  }
  printf("pwrbtn-monitor: OOM monitoring active\n");
  return true;
}

void RunOomThread() {
  std::unique_ptr<pwrbtn::OomWatcher> oom_watcher = std::make_unique<pwrbtn::OomWatcher>();

  async::Loop oom_loop(&kAsyncLoopConfigAttachToCurrentThread);
  if (StartOomWatcher(oom_watcher.get(), oom_loop.dispatcher())) {
    oom_loop.Run();
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (StdoutToDebuglog::Init() != ZX_OK) {
    return 1;
  }

  // Declare the looper
  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);

  // Declare some structures needed for the duration of the program, some of
  // these are shared between different tasks.
  pwrbtn::PowerButtonMonitor monitor{loop.dispatcher()};
  PowerButtonInfo info;

  // Create a task which watches for the power button device to appear and then
  // starts monitoring it for events.
  zx::event report_event;
  async::Wait pwrbtn_waiter(ZX_HANDLE_INVALID, ZX_USER_SIGNAL_0, 0, nullptr);

  async::TaskClosure button_init([&report_event, &info, &pwrbtn_waiter, &loop, &monitor]() mutable {
    zx_status_t status = GetButtonReportEvent(&report_event, &info);

    if (status != ZX_OK) {
      printf("pwrbtn-monitor: failed to get power button info, events will not be monitored.\n");
      loop.Quit();
      return;
    }

    pwrbtn_waiter.set_object(report_event.get());
    pwrbtn_waiter.set_handler([&pwrbtn_waiter, &loop, &monitor, &info, &report_event](
                                  async_dispatcher_t*, async::Wait*, zx_status_t status,
                                  const zx_packet_signal_t*) mutable {
      ProcessPowerEvent(&report_event, &monitor, &info, status, &loop);
      pwrbtn_waiter.Begin(loop.dispatcher());
    });

    // schedule the watcher task
    pwrbtn_waiter.Begin(loop.dispatcher());
  });

  button_init.Post(loop.dispatcher());

  // Start the thread and looper that will listen for OOM events. We create a
  // second thread because the power button code uses a synchronous directory
  // watcher to watch for new directory entries. We could change pwrbtn-monitor
  // to use inotify interfaces, but this would require substantial additional
  // work.
  std::thread oom_thread(RunOomThread);

  component::OutgoingDirectory outgoing{loop.dispatcher()};
  zx::result result = outgoing.ServeFromStartupInfo();
  if (!result.is_ok()) {
    printf("pwrbtn-monitor: failed to ServeFromStartupInfo: %s\n", result.status_string());
    return 1;
  }

  result = outgoing.AddUnmanagedProtocol<fuchsia_power_button::Monitor>(monitor.Publish());
  if (!result.is_ok()) {
    printf("pwrbtn-monitor: failed to AddEntry: %s\n", result.status_string());
    return 1;
  }

  loop.Run();

  // Wait for the oom thread to exit
  oom_thread.join();
  return 1;
}
