// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "main.h"

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <fidl/fuchsia.driver.index/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fidl/fuchsia.kernel/cpp/wire.h>
#include <getopt.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/io.h>
#include <lib/zx/event.h>
#include <lib/zx/port.h>
#include <lib/zx/resource.h>
#include <lib/zx/vmo.h>
#include <threads.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#include <zircon/syscalls/policy.h>
#include <zircon/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

#include <fbl/string_printf.h>

#include "component_lifecycle.h"
#include "coordinator.h"
#include "driver_host_loader_service.h"
#include "fdio.h"
#include "fidl/fuchsia.process.lifecycle/cpp/markers.h"
#include "lib/async/cpp/task.h"
#include "src/devices/bin/driver_manager/devfs/devfs.h"
#include "src/devices/bin/driver_manager/devfs/devfs_exporter.h"
#include "src/devices/bin/driver_manager/device_watcher.h"
#include "src/devices/bin/driver_manager/v2/driver_development_service.h"
#include "src/devices/lib/log/log.h"
#include "src/lib/storage/vfs/cpp/synchronous_vfs.h"
#include "src/sys/lib/stdout-to-debuglog/cpp/stdout-to-debuglog.h"
#include "system_instance.h"
#include "v2/driver_runner.h"

DriverHostCrashPolicy CrashPolicyFromString(const std::string& crash_policy) {
  if (crash_policy == "reboot-system") {
    return DriverHostCrashPolicy::kRebootSystem;
  } else if (crash_policy == "restart-driver-host") {
    return DriverHostCrashPolicy::kRestartDriverHost;
  } else if (crash_policy == "do-nothing") {
    return DriverHostCrashPolicy::kDoNothing;
  } else {
    LOGF(ERROR, "Unexpected option for driver-manager.driver-host-crash-policy: %s",
         crash_policy.c_str());
    return DriverHostCrashPolicy::kRestartDriverHost;
  }
}

// Get the root job from the root job service.
zx::result<zx::job> get_root_job() {
  zx::result client_end = component::Connect<fuchsia_kernel::RootJob>();
  if (client_end.is_error()) {
    return client_end.take_error();
  }
  fidl::WireResult result = fidl::WireCall(client_end.value())->Get();
  if (!result.ok()) {
    return zx::error(result.status());
  }
  return zx::ok(std::move(result.value().job));
}

// Get the root resource from the root resource service. Not receiving the
// startup handle is logged, but not fatal.  In test environments, it would not
// be present.
zx::result<zx::resource> get_root_resource() {
  zx::result client_end = component::Connect<fuchsia_boot::RootResource>();
  if (client_end.is_error()) {
    return client_end.take_error();
  }
  fidl::WireResult result = fidl::WireCall(client_end.value())->Get();
  if (!result.ok()) {
    return zx::error(result.status());
  }
  return zx::ok(std::move(result.value().resource));
}

// Get the mexec resource from the mexec resource service. Not receiving the
// startup handle is logged, but not fatal.  In test environments, it would not
// be present.
zx::result<zx::resource> get_mexec_resource() {
  zx::result client_end = component::Connect<fuchsia_kernel::MexecResource>();
  if (client_end.is_error()) {
    return client_end.take_error();
  }
  fidl::WireResult result = fidl::WireCall(client_end.value())->Get();
  if (!result.ok()) {
    return zx::error(result.status());
  }
  return zx::ok(std::move(result.value().resource));
}

int main(int argc, char** argv) {
  zx_status_t status = StdoutToDebuglog::Init();
  if (status != ZX_OK) {
    LOGF(INFO, "Failed to redirect stdout to debuglog, assuming test environment and continuing");
  }

  auto args_result = component::Connect<fuchsia_boot::Arguments>();
  if (args_result.is_error()) {
    LOGF(ERROR, "Failed to get boot arguments service handle: %s", args_result.status_string());
    return args_result.error_value();
  }

  auto config = driver_manager_config::Config::TakeFromStartupHandle();

  if (config.verbose()) {
    fx_logger_t* logger = fx_log_get_logger();
    if (logger) {
      fx_logger_set_min_severity(logger, std::numeric_limits<fx_log_severity_t>::min());
    }
  }

  auto boot_args = fidl::WireSyncClient<fuchsia_boot::Arguments>{std::move(*args_result)};
  if (config.use_driver_framework_v2()) {
    return RunDfv2(std::move(config), std::move(boot_args));
  }
  return RunDfv1(std::move(config), std::move(boot_args));
}

int RunDfv1(driver_manager_config::Config dm_config,
            fidl::WireSyncClient<fuchsia_boot::Arguments> boot_args) {
  SuspendCallback suspend_callback = [](zx_status_t status) {
    if (status != ZX_OK) {
      // TODO(https://fxbug.dev/56208): Change this log back to error once isolated devmgr is fixed.
      LOGF(WARNING, "Error suspending devices while stopping the component:%s",
           zx_status_get_string(status));
    }
    LOGF(INFO, "Exiting driver manager gracefully");
    // TODO(fxb:52627) This event handler should teardown devices and driver hosts
    // properly for system state transitions where driver manager needs to go down.
    // Exiting like so, will not run all the destructors and clean things up properly.
    // Instead the main devcoordinator loop should be quit.
    exit(0);
  };

  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  auto outgoing = component::OutgoingDirectory(loop.dispatcher());
  InspectManager inspect_manager(loop.dispatcher());

  inspect::Node config_node = inspect_manager.root_node().CreateChild("config");
  dm_config.RecordInspect(&config_node);

  CoordinatorConfig config;
  SystemInstance system_instance;
  config.boot_args = &boot_args;
  config.delay_fallback_until_base_drivers_indexed =
      dm_config.delay_fallback_until_base_drivers_indexed();
  config.verbose = dm_config.verbose();
  config.fs_provider = &system_instance;
  config.path_prefix = "/boot/";
  config.crash_policy = CrashPolicyFromString(dm_config.driver_host_crash_policy());

  // Waiting an infinite amount of time before falling back is effectively not
  // falling back at all.
  if (!dm_config.suspend_timeout_fallback()) {
    config.suspend_timeout = zx::duration::infinite();
  }

  auto driver_index_client = component::Connect<fuchsia_driver_index::DriverIndex>();
  if (driver_index_client.is_error()) {
    LOGF(ERROR, "Failed to connect to driver_index: %d", driver_index_client.error_value());
    return driver_index_client.error_value();
  }
  config.driver_index = fidl::WireSharedClient<fuchsia_driver_index::DriverIndex>(
      std::move(driver_index_client.value()), loop.dispatcher());

  // TODO(fxbug.dev/33958): Remove all uses of the root resource.
  if (zx::result root_resource = get_root_resource(); root_resource.is_error()) {
    LOGF(INFO, "Failed to get root resource, assuming test environment and continuing (%s)",
         root_resource.status_string());
  } else {
    config.root_resource = std::move(root_resource.value());
  }
  // TODO(fxbug.dev/33957): Remove all uses of the root job.
  zx::result root_job = get_root_job();
  if (root_job.is_error()) {
    LOGF(ERROR, "Failed to get root job: %s", root_job.status_string());
    return root_job.status_value();
  }
  if (zx::result mexec_resource = get_mexec_resource(); mexec_resource.is_error()) {
    LOGF(INFO, "Failed to get mexec resource, assuming test environment and continuing (%s)",
         mexec_resource.status_string());
  } else {
    config.mexec_resource = std::move(mexec_resource.value());
  }

  async::Loop firmware_loop(&kAsyncLoopConfigNeverAttachToThread);
  firmware_loop.StartThread("firmware-loop");

  Coordinator coordinator(std::move(config), &inspect_manager, loop.dispatcher(),
                          firmware_loop.dispatcher());

  // Services offered to the rest of the system.
  coordinator.InitOutgoingServices(outgoing);

  std::optional<driver_manager::DriverDevelopmentService> driver_development_service;

  // Launch devfs_exporter.
  std::optional<Devnode>& root_node = coordinator.root_devnode();
  ZX_ASSERT(root_node.has_value());
  driver_manager::DevfsExporter devfs_exporter(coordinator.devfs(), &root_node.value(),
                                               loop.dispatcher());
  devfs_exporter.PublishExporter(outgoing);

  // Launch DriverRunner for DFv2 drivers.
  auto realm_result = component::Connect<fuchsia_component::Realm>();
  if (realm_result.is_error()) {
    return realm_result.error_value();
  }
  auto driver_index_result = component::Connect<fuchsia_driver_index::DriverIndex>();
  if (driver_index_result.is_error()) {
    LOGF(ERROR, "Failed to connect to driver_index: %d", driver_index_result.error_value());
    return driver_index_result.error_value();
  }

  fbl::unique_fd lib_fd;
  {
    zx_status_t status = fdio_open_fd("/boot/lib/",
                                      static_cast<uint32_t>(fio::wire::OpenFlags::kDirectory |
                                                            fio::wire::OpenFlags::kRightReadable |
                                                            fio::wire::OpenFlags::kRightExecutable),
                                      lib_fd.reset_and_get_address());
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to open /boot/lib/ : %s", zx_status_get_string(status));
      return status;
    }
  }
  // The loader needs its own thread because DriverManager makes synchronous calls to the
  // DriverHosts, which make synchronous calls to load their shared libraries.
  async::Loop loader_loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  loader_loop.StartThread("loader-loop");

  auto loader_service =
      DriverHostLoaderService::Create(loader_loop.dispatcher(), std::move(lib_fd));
  auto driver_runner = dfv2::DriverRunner(
      std::move(realm_result.value()), std::move(driver_index_result.value()),
      inspect_manager.inspector(), [loader_service]() { return loader_service->Connect(); },
      loop.dispatcher());
  driver_runner.PublishComponentRunner(outgoing);

  // Find and load v1 Drivers.
  coordinator.set_driver_runner(&driver_runner);
  coordinator.PublishDriverDevelopmentService(outgoing);

  // V1 Drivers.
  zx_status_t status =
      system_instance.CreateDriverHostJob(root_job.value(), &config.driver_host_job);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to create driver_host job: %s", zx_status_get_string(status));
    return status;
  }

  coordinator.LoadV1Drivers(dm_config.root_driver());

  if (dm_config.set_root_driver_host_critical()) {
    // Set root driver host as critical so the system reboots if it crashes. It houses an escrow for
    // BTI handles and if we lose that we must reboot.
    status = root_job->set_critical(0, *coordinator.root_device()->proxy()->host()->proc());
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to set root driver host as critical: %s", zx_status_get_string(status));
      return status;
    }
  }

  // TODO(https://fxbug.dev/99076) Remove this when this issue is fixed.
  LOGF(INFO, "Drivers loaded and published");

  // Check if whatever launched devmgr gave a channel for component lifecycle events
  fidl::ServerEnd<fuchsia_process_lifecycle::Lifecycle> component_lifecycle_request(
      zx::channel(zx_take_startup_handle(PA_LIFECYCLE)));
  if (component_lifecycle_request.is_valid()) {
    status = devmgr::ComponentLifecycleServer::Create(loop.dispatcher(), &coordinator,
                                                      std::move(component_lifecycle_request),
                                                      std::move(suspend_callback));
    if (status != ZX_OK) {
      LOGF(ERROR, "driver_manager: Cannot create componentlifecycleserver: %s",
           zx_status_get_string(status));
      return status;
    }
  } else {
    LOGF(INFO,
         "No valid handle found for lifecycle events, assuming test environment and "
         "continuing");
  }

  // TODO(https://fxbug.dev/116638): Wire this up to do something useful.
  class DevfsLifecycle : public fidl::WireServer<fuchsia_process_lifecycle::Lifecycle> {
   public:
    explicit DevfsLifecycle() = default;

    void Stop(StopCompleter::Sync& completer) override { completer.Close(ZX_OK); }
  };

  {
    zx::result result = outgoing.AddProtocol<fuchsia_process_lifecycle::Lifecycle>(
        std::make_unique<DevfsLifecycle>(), "fuchsia.device.fs.lifecycle.Lifecycle");
    ZX_ASSERT_MSG(result.is_ok(), "%s", result.status_string());
  }

  coordinator.set_loader_service_connector(
      [loader_service = std::move(loader_service)](zx::channel* c) {
        auto conn = loader_service->Connect();
        if (conn.is_error()) {
          LOGF(ERROR, "Failed to add driver_host loader connection: %s", conn.status_string());
        } else {
          *c = conn->TakeChannel();
        }
        return conn.status_value();
      });

  // TODO(https://fxbug.dev/99076) Remove this when this issue is fixed.
  LOGF(INFO, "driver_manager loader loop started");

  fs::SynchronousVfs vfs(loop.dispatcher());

  // Serve the USB device watcher protocol.
  {
    zx::result devfs_client = coordinator.devfs().Connect(vfs);
    ZX_ASSERT_MSG(devfs_client.is_ok(), "%s", devfs_client.status_string());

    const zx::result result = outgoing.AddUnmanagedProtocol<fuchsia_device_manager::DeviceWatcher>(
        [devfs_client = std::move(devfs_client.value()), dispatcher = loader_loop.dispatcher()](
            fidl::ServerEnd<fuchsia_device_manager::DeviceWatcher> request) {
          // Move off the main loop, which is also serving devfs.
          async::PostTask(
              dispatcher, [&devfs_client, dispatcher, request = std::move(request)]() mutable {
                zx::result dir =
                    [&devfs_client]() -> zx::result<fidl::ClientEnd<fuchsia_io::Directory>> {
                  zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
                  if (endpoints.is_error()) {
                    return endpoints.take_error();
                  }
                  auto& [client, server] = endpoints.value();

                  if (const zx_status_t status =
                          fdio_service_connect_at(devfs_client.channel().get(), "class/usb-device",
                                                  server.TakeChannel().release());
                      status != ZX_OK) {
                    return zx::error(status);
                  }

                  return zx::ok(std::move(client));
                }();
                if (dir.is_error()) {
                  request.Close(dir.status_value());
                }
                std::unique_ptr watcher =
                    std::make_unique<DeviceWatcher>(dispatcher, std::move(dir.value()));
                fidl::BindServer(dispatcher, std::move(request), std::move(watcher));
              });
        },
        "fuchsia.hardware.usb.DeviceWatcher");
    ZX_ASSERT_MSG(result.is_ok(), "%s", result.status_string());
  }

  zx::result diagnostics_client = coordinator.inspect_manager().Connect();
  ZX_ASSERT_MSG(diagnostics_client.is_ok(), "%s", diagnostics_client.status_string());

  zx::result devfs_client = coordinator.devfs().Connect(vfs);
  ZX_ASSERT_MSG(devfs_client.is_ok(), "%s", devfs_client.status_string());

  {
    const zx::result result = outgoing.AddDirectory(std::move(devfs_client.value()), "dev");
    ZX_ASSERT_MSG(result.is_ok(), "%s", result.status_string());
  }
  {
    const zx::result result =
        outgoing.AddDirectory(std::move(diagnostics_client.value()), "diagnostics");
    ZX_ASSERT_MSG(result.is_ok(), "%s", result.status_string());
  }

  {
    const zx::result result = outgoing.ServeFromStartupInfo();
    ZX_ASSERT_MSG(result.is_ok(), "%s", result.status_string());
  }

  async::PostTask(loop.dispatcher(), [] { LOGF(INFO, "driver_manager main loop is running"); });

  coordinator.set_running(true);
  status = loop.Run();
  LOGF(ERROR, "Coordinator exited unexpectedly: %s", zx_status_get_string(status));
  return status;
}
