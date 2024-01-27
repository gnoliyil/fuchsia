// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device.manager/cpp/wire.h>
#include <fidl/fuchsia.hardware.power.statecontrol/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fidl/fuchsia.process.lifecycle/cpp/wire.h>
#include <fidl/fuchsia.sys2/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/fit/function.h>
#include <zircon/process.h>
#include <zircon/status.h>

#include <chrono>
#include <thread>

#include <fbl/string_printf.h>

#include "src/lib/storage/vfs/cpp/managed_vfs.h"
#include "src/lib/storage/vfs/cpp/pseudo_dir.h"
#include "src/lib/storage/vfs/cpp/service.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/sys/lib/stdout-to-debuglog/cpp/stdout-to-debuglog.h"

namespace fio = fuchsia_io;

namespace statecontrol_fidl = fuchsia_hardware_power_statecontrol;
namespace device_manager_fidl = fuchsia_device_manager;
namespace sys2_fidl = fuchsia_sys2;

// The amount of time that the shim will spend trying to connect to
// power_manager before giving up.
// TODO(fxbug.dev/54426): increase this timeout
const zx::duration SERVICE_CONNECTION_TIMEOUT = zx::sec(2);

// The amount of time that the shim will spend waiting for a manually trigger
// system shutdown to finish before forcefully restarting the system.
const std::chrono::duration MANUAL_SYSTEM_SHUTDOWN_TIMEOUT = std::chrono::minutes(60);

class LifecycleServer final : public fidl::WireServer<fuchsia_process_lifecycle::Lifecycle> {
 public:
  LifecycleServer(fidl::WireServer<statecontrol_fidl::Admin>::MexecCompleter::Async mexec_completer)
      : mexec_completer_(std::move(mexec_completer)) {}

  static zx_status_t Create(
      async_dispatcher_t* dispatcher,
      fidl::WireServer<statecontrol_fidl::Admin>::MexecCompleter::Async completer,
      zx::channel chan);

  void Stop(StopCompleter::Sync& completer) override;

 private:
  fidl::WireServer<statecontrol_fidl::Admin>::MexecCompleter::Async mexec_completer_;
};

zx_status_t LifecycleServer::Create(
    async_dispatcher_t* dispatcher,
    fidl::WireServer<statecontrol_fidl::Admin>::MexecCompleter::Async completer, zx::channel chan) {
  zx_status_t status = fidl::BindSingleInFlightOnly(
      dispatcher, std::move(chan), std::make_unique<LifecycleServer>(std::move(completer)));
  if (status != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: failed to bind lifecycle service: %s\n",
            zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

void LifecycleServer::Stop(StopCompleter::Sync& completer) {
  printf(
      "[shutdown-shim]: received shutdown command over lifecycle interface, completing the mexec "
      "call\n");
  mexec_completer_.ReplySuccess();
}

class StateControlAdminServer final : public fidl::WireServer<statecontrol_fidl::Admin> {
 public:
  StateControlAdminServer() : lifecycle_loop_((&kAsyncLoopConfigNoAttachToCurrentThread)) {}

  // Creates a new fs::Service backed by a new StateControlAdminServer, to be
  // inserted into a pseudo fs.
  static fbl::RefPtr<fs::Service> Create(async_dispatcher* dispatcher);

  void PowerFullyOn(PowerFullyOnCompleter::Sync& completer) override;

  void Reboot(RebootRequestView request, RebootCompleter::Sync& completer) override;

  void RebootToBootloader(RebootToBootloaderCompleter::Sync& completer) override;

  void RebootToRecovery(RebootToRecoveryCompleter::Sync& completer) override;

  void Poweroff(PoweroffCompleter::Sync& completer) override;

  void Mexec(MexecRequestView request, MexecCompleter::Sync& completer) override;

  void SuspendToRam(SuspendToRamCompleter::Sync& completer) override;

 private:
  async::Loop lifecycle_loop_;
};

// Asynchronously connects to the given protocol.
zx_status_t connect_to_protocol(const char* name, zx::channel* local) {
  zx::channel remote;
  zx_status_t status = zx::channel::create(0, local, &remote);
  if (status != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: error creating channel: %s\n", zx_status_get_string(status));
    return status;
  }
  auto path = fbl::StringPrintf("/svc/%s", name);
  status = fdio_service_connect(path.data(), remote.release());
  if (status != ZX_OK) {
    printf("[shutdown-shim]: failed to connect to %s: %s\n", name, zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

// Opens a service node, failing if the provider of the service does not respond
// to messages within SERVICE_CONNECTION_TIMEOUT.
//
// This is accomplished by opening the service node, writing an invalid message
// to the channel, and observing PEER_CLOSED within the timeout. This is testing
// that something is responding to open requests for this service, as opposed to
// the intended provider for this service being stuck on component resolution
// indefinitely, which causes connection attempts to the component to never
// succeed nor fail. By observing a PEER_CLOSED, we can ensure that the service
// provider received our message and threw it out (or the provider doesn't
// exist). Upon receiving the PEER_CLOSED, we then open a new connection and
// save it in `local`.
//
// This is protecting against packaged components being stuck in resolution for
// forever, which happens if pkgfs never starts up (this always happens on
// bringup). Once a component is able to be resolved, then all new service
// connections will either succeed or fail rather quickly.
zx_status_t connect_to_protocol_with_timeout(const char* name, zx::channel* local) {
  zx::channel local_2;
  zx_status_t status = connect_to_protocol(name, &local_2);
  if (status != ZX_OK) {
    return status;
  }

  // We want to use the zx_channel_call syscall directly here, because there's
  // no way to set the timeout field on the call using the FIDL bindings.
  char garbage_data[6] = {0, 1, 2, 3, 4, 5};
  zx_channel_call_args_t call = {
      // Bytes to send in the channel call
      .wr_bytes = garbage_data,
      // Handles to send in the channel call
      .wr_handles = nullptr,
      // Buffer to write received bytes into from the channel call
      .rd_bytes = nullptr,
      // Buffer to write received handles into from the channel call
      .rd_handles = nullptr,
      // Number of bytes to send
      .wr_num_bytes = 6,
      // Number of bytes we can receive
      .rd_num_bytes = 0,
      // Number of handles we can receive
      .rd_num_handles = 0,
  };
  uint32_t actual_bytes, actual_handles;
  status = local_2.call(0, zx::deadline_after(SERVICE_CONNECTION_TIMEOUT), &call, &actual_bytes,
                        &actual_handles);
  if (status == ZX_ERR_TIMED_OUT) {
    fprintf(stderr, "[shutdown-shim]: timed out connecting to %s\n", name);
    return status;
  }
  if (status != ZX_ERR_PEER_CLOSED) {
    fprintf(stderr, "[shutdown-shim]: unexpected response from %s: %s\n", name,
            zx_status_get_string(status));
    return status;
  }
  return connect_to_protocol(name, local);
}

// Connect to fuchsia.device.manager.SystemStateTransition and set the
// termination state.
zx_status_t set_system_state_transition_behavior(
    device_manager_fidl::wire::SystemPowerState state) {
  zx::channel local;
  zx_status_t status = connect_to_protocol(
      fidl::DiscoverableProtocolName<device_manager_fidl::SystemStateTransition>, &local);
  if (status != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: error connecting to driver_manager\n");
    return status;
  }
  auto system_state_transition_behavior_client =
      fidl::WireSyncClient<device_manager_fidl::SystemStateTransition>(std::move(local));

  auto resp = system_state_transition_behavior_client->SetTerminationSystemState(state);
  if (resp.status() != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: transport error sending message to driver_manager: %s\n",
            resp.FormatDescription().c_str());
    return resp.status();
  }
  if (resp->is_error()) {
    return resp->error_value();
  }
  return ZX_OK;
}

// Connect to fuchsia.device.manager.SystemStateTransition and prepare driver
// manager to mexec on shutdown.
zx_status_t SetMexecZbis(zx::vmo kernel_zbi, zx::vmo data_zbi) {
  zx::channel local;
  zx_status_t status = connect_to_protocol(
      fidl::DiscoverableProtocolName<device_manager_fidl::SystemStateTransition>, &local);
  if (status != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: error connecting to driver_manager\n");
    return status;
  }
  auto client = fidl::WireSyncClient<device_manager_fidl::SystemStateTransition>(std::move(local));

  auto resp = client->SetMexecZbis(std::move(kernel_zbi), std::move(data_zbi));
  if (resp.status() != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: transport error sending message to driver_manager: %s\n",
            resp.FormatDescription().c_str());
    return resp.status();
  }
  if (resp->is_error()) {
    return resp->error_value();
  }
  return ZX_OK;
}

// Connect to fuchsia.sys2.SystemController and initiate a system shutdown. If
// everything goes well, this function shouldn't return until shutdown is
// complete.
zx_status_t initiate_component_shutdown() {
  zx::channel local;
  zx_status_t status =
      connect_to_protocol(fidl::DiscoverableProtocolName<sys2_fidl::SystemController>, &local);
  if (status != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: error connecting to component_manager\n");
    return status;
  }
  auto system_controller_client =
      fidl::WireSyncClient<sys2_fidl::SystemController>(std::move(local));

  printf("[shutdown-shim]: calling system_controller_client.Shutdown()\n");
  auto resp = system_controller_client->Shutdown();
  printf("[shutdown-shim]: status was returned: %s\n", zx_status_get_string(status));
  return resp.status();
}

// Sleeps for MANUAL_SYSTEM_SHUTDOWN_TIMEOUT, and then exits the process
void shutdown_timer() {
  std::this_thread::sleep_for(MANUAL_SYSTEM_SHUTDOWN_TIMEOUT);

  // We shouldn't still be running at this point

  exit(1);
}

// Manually drive a shutdown by setting state as driver_manager's termination
// behavior and then instructing component_manager to perform an orderly
// shutdown of components. If the orderly shutdown takes too long the shim will
// exit with a non-zero exit code, killing the root job.
void drive_shutdown_manually(device_manager_fidl::wire::SystemPowerState state) {
  printf("[shutdown-shim]: driving shutdown manually\n");

  // Start a new thread that makes us exit uncleanly after a timeout. This will
  // guarantee that shutdown doesn't take longer than
  // MANUAL_SYSTEM_SHUTDOWN_TIMEOUT, because we're marked as critical to the
  // root job and us exiting will bring down userspace and cause a reboot.
  std::thread(shutdown_timer).detach();

  zx_status_t status = set_system_state_transition_behavior(state);
  if (status != ZX_OK) {
    fprintf(stderr,
            "[shutdown-shim]: error setting system state transition behavior in driver_manager, "
            "proceeding with component shutdown anyway: %s\n",
            zx_status_get_string(status));
    // Proceed here, maybe we can at least gracefully reboot still
    // (driver_manager's default behavior)
  }

  status = initiate_component_shutdown();
  if (status != ZX_OK) {
    fprintf(
        stderr,
        "[shutdown-shim]: error initiating component shutdown, system shutdown impossible: %s\n",
        zx_status_get_string(status));
    // Recovery from this state is impossible. Exit with a non-zero exit code,
    // so our critical marking causes the system to forcefully restart.
    exit(1);
  }
  fprintf(stderr, "[shutdown-shim]: manual shutdown successfully initiated\n");
}

zx_status_t send_command(fidl::WireSyncClient<statecontrol_fidl::Admin> statecontrol_client,
                         device_manager_fidl::wire::SystemPowerState fallback_state,
                         const statecontrol_fidl::wire::RebootReason* reboot_reason = nullptr,
                         StateControlAdminServer::MexecRequestView* mexec_request = nullptr) {
  switch (fallback_state) {
    case device_manager_fidl::wire::SystemPowerState::kReboot: {
      if (reboot_reason == nullptr) {
        fprintf(stderr, "[shutdown-shim]: internal error, bad pointer to reason for reboot\n");
        return ZX_ERR_INTERNAL;
      }
      auto resp = statecontrol_client->Reboot(*reboot_reason);
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp->is_error()) {
        return resp->error_value();
      } else {
        return ZX_OK;
      }
    } break;
    case device_manager_fidl::wire::SystemPowerState::kRebootKernelInitiated: {
      auto resp = statecontrol_client->Reboot(*reboot_reason);
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp.value().is_error()) {
        return resp.value().error_value();
      } else {
        return ZX_OK;
      }
    } break;
    case device_manager_fidl::wire::SystemPowerState::kRebootBootloader: {
      auto resp = statecontrol_client->RebootToBootloader();
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp->is_error()) {
        return resp->error_value();
      } else {
        return ZX_OK;
      }
    } break;
    case device_manager_fidl::wire::SystemPowerState::kRebootRecovery: {
      auto resp = statecontrol_client->RebootToRecovery();
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp->is_error()) {
        return resp->error_value();
      } else {
        return ZX_OK;
      }
    } break;
    case device_manager_fidl::wire::SystemPowerState::kPoweroff: {
      auto resp = statecontrol_client->Poweroff();
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp->is_error()) {
        return resp->error_value();
      } else {
        return ZX_OK;
      }
    } break;
    case device_manager_fidl::wire::SystemPowerState::kMexec: {
      if (mexec_request == nullptr) {
        fprintf(stderr, "[shutdown-shim]: internal error, bad pointer to reason for mexec\n");
        return ZX_ERR_INTERNAL;
      }
      auto resp = statecontrol_client->Mexec(std::move((*mexec_request)->kernel_zbi),
                                             std::move((*mexec_request)->data_zbi));
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp->is_error()) {
        return resp->error_value();
      } else {
        return ZX_OK;
      }
    } break;
    case device_manager_fidl::wire::SystemPowerState::kSuspendRam: {
      auto resp = statecontrol_client->SuspendToRam();
      if (resp.status() != ZX_OK) {
        return ZX_ERR_UNAVAILABLE;
      } else if (resp->is_error()) {
        return resp->error_value();
      } else {
        return ZX_OK;
      }
    } break;
    default:
      return ZX_ERR_INTERNAL;
  }
}

// Connects to power_manager and passes a SyncClient to the given function. The
// function is expected to return an error if there was a transport-related
// issue talking to power_manager, in which case this program will talk to
// driver_manager and component_manager to drive shutdown manually.
zx_status_t forward_command(device_manager_fidl::wire::SystemPowerState fallback_state,
                            const statecontrol_fidl::wire::RebootReason* reboot_reason = nullptr) {
  printf("[shutdown-shim]: checking power_manager liveness\n");
  zx::channel local;
  zx_status_t status = connect_to_protocol_with_timeout(
      fidl::DiscoverableProtocolName<statecontrol_fidl::Admin>, &local);
  if (status == ZX_OK) {
    printf("[shutdown-shim]: trying to forward command\n");
    status = send_command(fidl::WireSyncClient<statecontrol_fidl::Admin>(std::move(local)),
                          fallback_state, reboot_reason);
    if (status != ZX_ERR_UNAVAILABLE && status != ZX_ERR_NOT_SUPPORTED) {
      return status;
    }
    // Power manager may decide not to support suspend. We should respect that and not attempt to
    // suspend manually.
    if (fallback_state == device_manager_fidl::wire::SystemPowerState::kSuspendRam) {
      return status;
    }
  }

  printf("[shutdown-shim]: failed to forward command to power_manager: %s\n",
         zx_status_get_string(status));

  drive_shutdown_manually(fallback_state);

  // We should block on fuchsia.sys.SystemController forever on this thread, if
  // it returns something has gone wrong.
  fprintf(stderr, "[shutdown-shim]: we shouldn't still be running, crashing the system\n");
  exit(1);
}

void StateControlAdminServer::PowerFullyOn(PowerFullyOnCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void StateControlAdminServer::Reboot(RebootRequestView request, RebootCompleter::Sync& completer) {
  device_manager_fidl::wire::SystemPowerState target_state =
      device_manager_fidl::wire::SystemPowerState::kReboot;
  if (request->reason == statecontrol_fidl::wire::RebootReason::kOutOfMemory) {
    target_state = device_manager_fidl::wire::SystemPowerState::kRebootKernelInitiated;
  }
  zx_status_t status = forward_command(target_state, &request->reason);
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

void StateControlAdminServer::RebootToBootloader(RebootToBootloaderCompleter::Sync& completer) {
  zx_status_t status =
      forward_command(device_manager_fidl::wire::SystemPowerState::kRebootBootloader);
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

void StateControlAdminServer::RebootToRecovery(RebootToRecoveryCompleter::Sync& completer) {
  zx_status_t status =
      forward_command(device_manager_fidl::wire::SystemPowerState::kRebootRecovery);
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

void StateControlAdminServer::Poweroff(PoweroffCompleter::Sync& completer) {
  zx_status_t status = forward_command(device_manager_fidl::wire::SystemPowerState::kPoweroff);
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

void StateControlAdminServer::Mexec(MexecRequestView request, MexecCompleter::Sync& completer) {
  // Duplicate the VMOs now, as forwarding the mexec request to power-manager
  // will consume them.
  zx::vmo kernel_zbi, data_zbi;
  zx_status_t status = request->kernel_zbi.duplicate(ZX_RIGHT_SAME_RIGHTS, &kernel_zbi);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  status = request->data_zbi.duplicate(ZX_RIGHT_SAME_RIGHTS, &data_zbi);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }

  printf("[shutdown-shim]: checking power_manager liveness\n");
  zx::channel local;
  status = connect_to_protocol_with_timeout(
      fidl::DiscoverableProtocolName<statecontrol_fidl::Admin>, &local);
  if (status == ZX_OK) {
    printf("[shutdown-shim]: trying to forward command\n");
    status = send_command(fidl::WireSyncClient<statecontrol_fidl::Admin>(std::move(local)),
                          device_manager_fidl::wire::SystemPowerState::kMexec, nullptr, &request);
    if (status == ZX_OK) {
      completer.ReplySuccess();
      return;
    }
    if (status != ZX_ERR_UNAVAILABLE && status != ZX_ERR_NOT_SUPPORTED) {
      completer.ReplyError(status);
      return;
    }
    // Else, fallback logic.
  }

  printf("[shutdown-shim]: failed to forward command to power_manager: %s\n",
         zx_status_get_string(status));

  // In this fallback codepath, we first configure driver_manager to perform
  // the actual mexec syscall on shutdown and then begin an orderly shutdown of
  // all components to indirectly trigger that. Since driver_manager is
  // downstream of the shutdown-shim, this component - and other
  // main_process_critical ones - will not actually be shut down before the
  // mexec is performed (unless of course something goes wrong, in which case a
  // full system shutdown is indeed the right outcome).
  //
  // driver_manager's termination state will be updated as kMexec in
  // drive_shutdown_manually() below.
  status = SetMexecZbis(std::move(kernel_zbi), std::move(data_zbi));
  if (status != ZX_OK) {
    fprintf(stderr, "[shutdown-shim]: failed to prepare driver manager to mexec: %s\n",
            zx_status_get_string(status));
    completer.ReplyError(status);
    return;
  }

  drive_shutdown_manually(device_manager_fidl::wire::SystemPowerState::kMexec);

  // We should block on fuchsia.sys.SystemController forever on this thread, if
  // it returns something has gone wrong.
  fprintf(stderr, "[shutdown-shim]: we shouldn't still be running, crashing the system\n");
  exit(1);
}

void StateControlAdminServer::SuspendToRam(SuspendToRamCompleter::Sync& completer) {
  zx_status_t status = forward_command(device_manager_fidl::wire::SystemPowerState::kSuspendRam);
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

fbl::RefPtr<fs::Service> StateControlAdminServer::Create(async_dispatcher* dispatcher) {
  return fbl::MakeRefCounted<fs::Service>([dispatcher](zx::channel chan) mutable {
    zx_status_t status = fidl::BindSingleInFlightOnly(dispatcher, std::move(chan),
                                                      std::make_unique<StateControlAdminServer>());
    if (status != ZX_OK) {
      fprintf(stderr, "[shutdown-shim] failed to bind statecontrol.Admin service: %s\n",
              zx_status_get_string(status));
      return status;
    }
    return ZX_OK;
  });
}

int main() {
  zx_status_t status = StdoutToDebuglog::Init();
  if (status != ZX_OK) {
    return status;
  }
  printf("[shutdown-shim]: started\n");

  async::Loop loop((async::Loop(&kAsyncLoopConfigAttachToCurrentThread)));

  fs::ManagedVfs outgoing_vfs((loop.dispatcher()));
  auto outgoing_dir = fbl::MakeRefCounted<fs::PseudoDir>();
  auto svc_dir = fbl::MakeRefCounted<fs::PseudoDir>();

  svc_dir->AddEntry(fidl::DiscoverableProtocolName<statecontrol_fidl::Admin>,
                    StateControlAdminServer::Create(loop.dispatcher()));
  outgoing_dir->AddEntry("svc", std::move(svc_dir));

  outgoing_vfs.ServeDirectory(outgoing_dir,
                              zx::channel(zx_take_startup_handle(PA_DIRECTORY_REQUEST)));

  loop.Run();

  fprintf(stderr, "[shutdown-shim]: exited unexpectedly\n");
  return 1;
}
