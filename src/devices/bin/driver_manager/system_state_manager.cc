// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "system_state_manager.h"

#include <zircon/status.h>

#include "coordinator.h"
#include "src/devices/lib/log/log.h"

zx_status_t SystemStateManager::BindPowerManagerInstance(
    async_dispatcher_t* dispatcher,
    fidl::ServerEnd<fuchsia_device_manager::SystemStateTransition> server) {
  // Invoked when the channel is closed or on any binding-related error.
  // When power manager exists, but closes this channel, it means power manager
  // existed but crashed, and we will not have a way to reboot the system.
  // We would need to reboot in that case.
  fidl::OnUnboundFn<SystemStateManager> unbound_fn(
      [](SystemStateManager* sys_state_manager, fidl::UnbindInfo info,
         fidl::ServerEnd<fuchsia_device_manager::SystemStateTransition>) {
        LOGF(ERROR, "system state transition channel with power manager got unbound: %s",
             info.FormatDescription().c_str());
        Coordinator* dev_coord = sys_state_manager->dev_coord_;
        if (!dev_coord->power_manager_registered()) {
          return;
        }
        dev_coord->set_power_manager_registered(false);
      });
  fidl::BindServer(dispatcher, std::move(server), this, std::move(unbound_fn));
  return ZX_OK;
}

void SystemStateManager::SetTerminationSystemState(
    SetTerminationSystemStateRequestView request,
    SetTerminationSystemStateCompleter::Sync& completer) {
  if (request->state == fuchsia_device_manager::wire::SystemPowerState::kFullyOn) {
    LOGF(INFO, "Invalid termination state");
    completer.ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }
  LOGF(INFO, "Setting shutdown system state to %hhu", request->state);
  dev_coord_->set_shutdown_system_state(request->state);
  completer.ReplySuccess();
}

void SystemStateManager::SetMexecZbis(SetMexecZbisRequestView request,
                                      SetMexecZbisCompleter::Sync& completer) {
  zx_status_t status =
      dev_coord_->SetMexecZbis(std::move(request->kernel_zbi), std::move(request->data_zbi));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to prepare to mexec on shutdown: %s", zx_status_get_string(status));
    completer.ReplyError(status);
    return;
  }
  LOGF(INFO, "Prepared to mexec on shutdown");
  completer.ReplySuccess();
}
