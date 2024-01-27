// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_MISC_DRIVERS_COMPAT_DEVICE_H_
#define SRC_DEVICES_MISC_DRIVERS_COMPAT_DEVICE_H_

#include <fidl/fuchsia.device.manager/cpp/wire.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.driver.compat/cpp/wire.h>
#include <fidl/fuchsia.driver.framework.deprecated/cpp/wire.h>
#include <fidl/fuchsia.driver.framework/cpp/wire.h>
#include <lib/async/cpp/executor.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/driver/compat/cpp/compat.h>
#include <lib/driver/compat/cpp/symbols.h>
#include <lib/driver/devfs/cpp/connector.h>
#include <lib/driver/logging/cpp/logger.h>
#include <lib/fdf/cpp/channel.h>
#include <lib/fpromise/bridge.h>
#include <lib/fpromise/scope.h>
#include <lib/inspect/component/cpp/component.h>
#include <lib/sync/cpp/completion.h>

#include <list>
#include <memory>
#include <unordered_map>

#include <fbl/intrusive_double_list.h>

#include "src/devices/lib/fidl/device_server.h"
#include "src/lib/storage/vfs/cpp/vmo_file.h"

namespace compat {

constexpr char kDfv2Variable[] = "IS_DFV2";

// The DFv1 ops: zx_protocol_device_t.
constexpr char kOps[] = "compat-ops";

class Driver;

// Device is an implementation of a DFv1 device.
class Device : public std::enable_shared_from_this<Device>,
               public devfs_fidl::DeviceInterface,
               public fidl::WireServer<fuchsia_driver_framework_deprecated::RuntimeConnector> {
 public:
  Device(device_t device, const zx_protocol_device_t* ops, Driver* driver,
         std::optional<Device*> parent, fdf::Logger* logger, async_dispatcher_t* dispatcher);

  ~Device() override;

  zx_device_t* ZxDevice();

  // Binds a device to a DFv2 node.
  void Bind(fidl::WireSharedClient<fuchsia_driver_framework::Node> node);

  // Unbinds a device from a DFv2 node.
  void Unbind();

  // Call Unbind or Suspend based on the power state.
  fpromise::promise<void> HandleStopSignal(fuchsia_device_manager::wire::SystemPowerState state);

  // Call the Unbind op for the device.
  fpromise::promise<void> UnbindOp();

  // Call the Suspend op for the device.
  fpromise::promise<void> SuspendOp(fuchsia_device_manager::wire::SystemPowerState state);

  // Removes all of the child devices.
  fpromise::promise<void> RemoveChildren();

  // Suspends all of the child devices.
  fpromise::promise<void> SuspendChildren(fuchsia_device_manager::wire::SystemPowerState state);

  const char* Name() const;
  bool HasChildren() const;

  // Functions to implement the DFv1 device API.
  zx_status_t Add(device_add_args_t* zx_args, zx_device_t** out);

  // Remove this device. This call will make sure that DFv1 unbind and release
  // are called in the correct order. This promise will finish once the device
  // has been completely removed.
  fpromise::promise<void> Remove();
  zx_status_t GetProtocol(uint32_t proto_id, void* out) const;
  zx_status_t AddMetadata(uint32_t type, const void* data, size_t size);
  zx_status_t GetMetadata(uint32_t type, void* buf, size_t buflen, size_t* actual);
  zx_status_t GetMetadataSize(uint32_t type, size_t* out_size);
  zx::result<uint32_t> SetPerformanceStateOp(uint32_t state);

  void InitReply(zx_status_t status);
  zx_status_t ConnectFragmentFidl(const char* fragment_name, const char* service_name,
                                  const char* protocol_name, zx::channel request);
  zx_status_t AddComposite(const char* name, const composite_device_desc_t* composite);
  zx_status_t AddCompositeNodeSpec(const char* name, const composite_node_spec_t* spec);
  // Connects to the runtime service using the v2 protocol discovery with tokens.
  zx_status_t ConnectRuntime(const char* service_name, const char* protocol_name,
                             fdf::Channel request);

  fpromise::promise<void, zx_status_t> WaitForInitToComplete();

  zx_status_t CreateNode();

  void CompleteUnbind();
  void CompleteSuspend();

  // Serves the |fuchsia_driver_framework_deprecated::RuntimeConnector| protocol,
  // used for supporting v1 of driver runtime protocol discovery.
  zx::result<fidl::ClientEnd<fuchsia_io::Directory>> ServeRuntimeConnectorProtocol();

  // Serves the |inspect_vmo| from the driver's diagnostics directory.
  zx_status_t ServeInspectVmo(zx::vmo inspect_vmo);

  // |fuchsia_driver_framework_deprecated::RuntimeConnector| implementation.
  void ListProtocols(ListProtocolsRequestView request,
                     ListProtocolsCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }
  void Connect(ConnectRequestView request, ConnectCompleter::Sync& completer) override;

  std::string_view topological_path() const { return topological_path_; }
  void set_topological_path(std::string path) { topological_path_ = std::move(path); }
  void set_fragments(std::vector<std::string> names) { fragments_ = std::move(names); }
  Driver* driver() { return driver_; }

  async_dispatcher_t* dispatcher() { return dispatcher_; }
  fpromise::scope& scope() { return scope_; }
  fdf::Logger& logger() { return *logger_; }
  async::Executor& executor() { return executor_; }
  DeviceServer& device_server() { return device_server_; }
  devfs_fidl::DeviceServer& devfs_server() { return devfs_server_; }

  void set_logger(fdf::Logger* logger) { logger_ = logger; }

  const std::vector<std::string>& fragments() { return fragments_; }

 private:
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  // This function will export everything about the device. Meaning it will:
  // - Add the device's capabilities into the outgoing directory.
  // - Export the device to devfs.
  // - Create the device's Node.
  // This must only be called after the device's init has finished.
  zx_status_t ExportAfterInit();

  // device_fidl::DeviceInterface APIs.
  void LogError(const char* error) override;
  bool IsUnbound() override;
  bool MessageOp(fidl::IncomingHeaderAndMessage msg, device_fidl_txn_t txn) override;
  void ConnectToDeviceFidl(ConnectToDeviceFidlRequestView request,
                           ConnectToDeviceFidlCompleter::Sync& completer) override;
  void Bind(BindRequestView request, BindCompleter::Sync& completer) override;
  void Rebind(RebindRequestView request, RebindCompleter::Sync& completer) override;
  void UnbindChildren(UnbindChildrenCompleter::Sync& completer) override;
  void ScheduleUnbind(ScheduleUnbindCompleter::Sync& completer) override;
  void GetTopologicalPath(GetTopologicalPathCompleter::Sync& completer) override;
  void GetMinDriverLogSeverity(GetMinDriverLogSeverityCompleter::Sync& completer) override;
  void GetCurrentPerformanceState(GetCurrentPerformanceStateCompleter::Sync& completer) override;
  void SetMinDriverLogSeverity(SetMinDriverLogSeverityRequestView request,
                               SetMinDriverLogSeverityCompleter::Sync& completer) override;
  void SetPerformanceState(SetPerformanceStateRequestView request,
                           SetPerformanceStateCompleter::Sync& completer) override;

  // This calls Unbind on the device and then frees it.
  void UnbindAndRelease();

  void InsertOrUpdateProperty(fuchsia_driver_framework::wire::NodePropertyKey key,
                              fuchsia_driver_framework::wire::NodePropertyValue value);

  std::string OutgoingName();

  bool ShouldCallRelease() {
    // We only shut down the devices that have a parent, since that means that *this* compat driver
    // owns the device. If the device does not have a parent, then ops_ belongs to another driver,
    // and it's that driver's responsibility to be shut down. We purposefully leak in
    // shutdown/reboot flows to emulate DFv1 shutdown. The fdf::Node client should have been torn
    // down by the driver runtime canceling all outstanding waits by the time stop has been called,
    // allowing shutdown to proceed.
    return parent_ && system_power_state_ == fuchsia_device_manager::SystemPowerState::kFullyOn;
  }

  // This arena backs `properties_`.
  // This should be declared before any objects it backs so it is destructed last.
  fidl::Arena<512> arena_;
  std::vector<fuchsia_driver_framework::wire::NodeProperty> properties_;

  std::optional<driver_devfs::Connector<fuchsia_device::Controller>> devfs_connector_;

  devfs_fidl::DeviceServer devfs_server_;

  DeviceServer device_server_;

  // This is the device's topological path without the leading '/dev/'.
  // TODO(fxbug.dev/117180): Simplify this and the GetTopologicalPath API.
  std::string topological_path_;
  const std::string name_;
  // A unique id for the device.
  uint32_t device_id_ = 0;

  fdf::Logger* logger_;
  fidl::ServerBindingGroup<fuchsia_driver_framework_deprecated::RuntimeConnector> bindings_;
  async_dispatcher_t* const dispatcher_;
  uint32_t device_flags_ = 0;
  std::vector<std::string> fragments_;

  // This device's driver. The driver owns all of its Device objects, so it
  // is garaunteed to outlive the Device.
  Driver* driver_ = nullptr;

  std::mutex init_lock_;
  bool init_is_finished_ __TA_GUARDED(init_lock_) = false;
  zx_status_t init_status_ __TA_GUARDED(init_lock_) = ZX_OK;
  std::vector<fpromise::completer<void, zx_status_t>> init_waiters_ __TA_GUARDED(init_lock_);

  bool pending_removal_ = false;

  // Completed when unbind is replied to.
  fpromise::completer<void> unbind_completer_;

  // Completed when suspend is replied to.
  fpromise::completer<void> suspend_completer_;
  fuchsia_device_manager::SystemPowerState system_power_state_ =
      fuchsia_device_manager::SystemPowerState::kFullyOn;

  // The default protocol of the device.
  device_t compat_symbol_;
  const zx_protocol_device_t* ops_;

  // A list of completers for promises that are waiting for this device to be
  // removed.
  std::vector<fpromise::completer<void>> remove_completers_;

  std::optional<fpromise::promise<>> controller_teardown_finished_;

  // The device's parent. If this field is set then the Device ptr is guaranteed
  // to be non-null. The parent is also guaranteed to outlive its child.
  //
  // This is used by a Device to free itself, by calling parent_.RemoveChild(this).
  //
  // parent_ will be std::nullopt when the Device is the fake device created
  // by the Driver class in the DFv1 shim. When parent_ is std::nullopt, the
  // Device will be freed when the Driver is freed.

  fidl::WireSharedClient<fuchsia_driver_framework::Node> node_;
  fidl::WireSharedClient<fuchsia_driver_framework::NodeController> controller_;

  const std::optional<Device*> parent_;

  // The Device's children. The Device has full ownership of the children,
  // but these are shared pointers so that the NodeController can get a weak
  // pointer to the child in order to erase them.
  std::list<std::shared_ptr<Device>> children_;

  async::Executor executor_;

  // File representing the device's inspect vmo, if any.
  std::optional<fbl::RefPtr<fs::VmoFile>> inspect_vmo_file_;

  // NOTE: Must be the last member.
  fpromise::scope scope_;
};

std::vector<fuchsia_driver_framework::wire::NodeProperty> CreateProperties(
    fidl::AnyArena& arena, fdf::Logger& logger, device_add_args_t* zx_args);

}  // namespace compat

struct zx_device : public compat::Device {
  // NOTE: Intentionally empty, do not add to this.
};

#endif  // SRC_DEVICES_MISC_DRIVERS_COMPAT_DEVICE_H_
