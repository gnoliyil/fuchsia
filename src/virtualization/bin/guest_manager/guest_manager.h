// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_VIRTUALIZATION_BIN_GUEST_MANAGER_GUEST_MANAGER_H_
#define SRC_VIRTUALIZATION_BIN_GUEST_MANAGER_GUEST_MANAGER_H_

#include <fuchsia/virtualization/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/result.h>
#include <zircon/types.h>

#include "src/virtualization/bin/guest_manager/memory_pressure_handler.h"
#include "src/virtualization/lib/guest_config/guest_config.h"

enum class GuestNetworkState {
  // There are at least enough virtual device interfaces to match the guest configuration, and
  // if there is a bridged configuration, there's at least one bridged interface. This doesn't
  // guarantee working networking, but means that the system state is likely correct.
  OK = 0,

  // This guest wasn't started with a network device, so no networking is expected.
  NO_NETWORK_DEVICE = 1,

  // Failed to query network interfaces. Note that if there's no guest network device, ability to
  // query network interfaces is irrelevant.
  FAILED_TO_QUERY = 2,

  // Host doesn't have a WLAN or ethernet interface, so there's probably no guest networking.
  NO_HOST_NETWORKING = 3,

  // There's at least one missing virtual interface that was expected to be present. Check
  // virtio-net device logs for a failure.
  MISSING_VIRTUAL_INTERFACES = 4,

  // An interface is bridged, there's an ethernet interface to bridge against, but the
  // bridge hasn't been created yet. This might be a transient issue while the bridge is created.
  NO_BRIDGE_CREATED = 5,

  // An interface is bridged, and there's no ethernet to bridge against, but the host is
  // connected to WLAN. This is a common user misconfiguration.
  ATTEMPTED_TO_BRIDGE_WITH_WLAN = 6,
};

class GuestManager : public fuchsia::virtualization::GuestManager {
 public:
  GuestManager(async_dispatcher_t* dispatcher, sys::ComponentContext* context,
               std::string config_pkg_dir_path, std::string config_path);

  GuestManager(async_dispatcher_t* dispatcher, sys::ComponentContext* context)
      : GuestManager(dispatcher, context, "/guest_pkg/", "data/guest.cfg") {}

  // |fuchsia::virtualization::GuestManager|
  void Launch(fuchsia::virtualization::GuestConfig user_config,
              fidl::InterfaceRequest<fuchsia::virtualization::Guest> controller,
              LaunchCallback callback) override;
  void ForceShutdown(ForceShutdownCallback callback) override;
  void Connect(fidl::InterfaceRequest<fuchsia::virtualization::Guest> controller,
               ConnectCallback) override;
  void GetInfo(GetInfoCallback callback) override;

  // Store a subset of the configuration. This can be queried while the guest is running using
  // the GuestManager::GetInfo FIDL message.
  void SnapshotConfig(const fuchsia::virtualization::GuestConfig& config);

  // Attempt to query the guest network state by iterating over the host network interfaces.
  GuestNetworkState QueryGuestNetworkState();
  static std::string GuestNetworkStateToStringExplanation(GuestNetworkState state);

  // Check for suspected problems with a running guest.
  std::vector<std::string> CheckForProblems();

  // Returns true if the guest was started, but hasn't stopped.
  bool is_guest_started() const;

 protected:
  virtual fit::result<::fuchsia::virtualization::GuestManagerError,
                      ::fuchsia::virtualization::GuestConfig>
  GetDefaultGuestConfig();
  virtual void OnGuestLaunched() {}
  virtual void OnGuestStopped() {}

 private:
  void HandleCreateResult(::fuchsia::virtualization::GuestLifecycle_Create_Result result,
                          bool balloon_enabled, LaunchCallback callback);
  void HandleRunResult(::fuchsia::virtualization::GuestLifecycle_Run_Result result);
  void HandleGuestStopped(fit::result<::fuchsia::virtualization::GuestError> err);

  async_dispatcher_t* dispatcher_;
  sys::ComponentContext* context_;
  fidl::BindingSet<fuchsia::virtualization::GuestManager> manager_bindings_;
  std::string config_pkg_dir_path_;
  std::string config_path_;

  // The VMM lifecycle control channel. If closed, this will terminate the VMM component.
  ::fuchsia::virtualization::GuestLifecyclePtr lifecycle_;

  // Cached error reported by the VMM upon stopping if not stopped due to a clean shutdown.
  std::optional<fuchsia::virtualization::GuestError> last_error_;

  // Used to calculate the guest's uptime for guest info reporting.
  zx::time start_time_ = zx::time::infinite_past();
  zx::time stop_time_ = zx::time::infinite_past();

  // Snapshot of some of the configuration settings used to start this guest. This is
  // informational only, and sent in response to a GetInfo call.
  fuchsia::virtualization::GuestDescriptor guest_descriptor_;

  // Current state of the guest.
  fuchsia::virtualization::GuestStatus state_ = fuchsia::virtualization::GuestStatus::NOT_STARTED;
  // Inflates / deflates the guest balloon in response to the host memory pressure events
  std::unique_ptr<MemoryPressureHandler> memory_pressure_handler_;
};

#endif  // SRC_VIRTUALIZATION_BIN_GUEST_MANAGER_GUEST_MANAGER_H_
