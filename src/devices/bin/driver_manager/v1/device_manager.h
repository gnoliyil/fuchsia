// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_V1_DEVICE_MANAGER_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_V1_DEVICE_MANAGER_H_

#include <fidl/fuchsia.device.manager/cpp/wire.h>

#include "src/devices/bin/driver_manager/composite_device.h"
#include "src/devices/bin/driver_manager/device.h"
#include "src/devices/bin/driver_manager/driver_host.h"

enum class DriverHostCrashPolicy;
class Coordinator;

// In charge of creating, adding, and removing devices. Doesn't include the root device.
class DeviceManager {
 public:
  using CompositeNodeSpecMap = std::unordered_map<std::string, std::unique_ptr<CompositeDevice>>;

  DeviceManager(Coordinator* coordinator, DriverHostCrashPolicy crash_policy);

  // Add a new device to a parent device (same driver_host)
  // New device is published in devfs.
  // Caller closes handles on error, so we don't have to.
  // TODO(fxbug.dev/43370): remove |always_init| once init tasks can be enabled for all devices.
  zx_status_t AddDevice(const fbl::RefPtr<Device>& parent,
                        fidl::ClientEnd<fuchsia_device_manager::DeviceController> device_controller,
                        fidl::ServerEnd<fuchsia_device_manager::Coordinator> coordinator,
                        const fuchsia_device_manager::wire::DeviceProperty* props_data,
                        size_t props_count,
                        const fuchsia_device_manager::wire::DeviceStrProperty* str_props_data,
                        size_t str_props_count, std::string_view name, uint32_t protocol_id,
                        std::string_view driver_path,
                        fuchsia_device_manager::wire::AddDeviceConfig add_device_config,
                        bool has_init, bool always_init, zx::vmo inspect,
                        fidl::ClientEnd<fio::Directory> outgoing_dir,
                        fbl::RefPtr<Device>* new_device);

  zx_status_t AddCompositeDevice(const fbl::RefPtr<Device>& dev, std::string_view name,
                                 fuchsia_device_manager::wire::CompositeDeviceDescriptor comp_desc);

  void AddCompositeDeviceFromSpec(CompositeNodeSpecInfo info,
                                  fbl::Array<std::unique_ptr<Metadata>> metadata);
  zx::result<> BindFragmentForSpec(const fbl::RefPtr<Device>& dev, const std::string& spec,
                                   size_t fragment_idx);

  // Begin scheduling for removal of the device and unbinding of its children.
  void ScheduleRemove(const fbl::RefPtr<Device>& dev);

  // Schedules the initial unbind task as a result of a driver_host's |ScheduleRemove|
  // request. If |do_unbind| is true, unbinding is also requested for |dev|.
  void ScheduleDriverHostRequestedRemove(const fbl::RefPtr<Device>& dev, bool do_unbind = false);

  void ScheduleDriverHostRequestedUnbindChildren(const fbl::RefPtr<Device>& parent);

  // Schedule unbind and remove tasks for all devices in |driver_host|.
  // Used as part of RestartDriverHosts().
  void ScheduleUnbindRemoveAllDevices(fbl::RefPtr<DriverHost> driver_host);

  // Removes the device from the parent. |forced| indicates this is
  // removal due to a channel close or process exit, which means we should
  // remove all other devices that share the driver_host at the same time.
  zx_status_t RemoveDevice(const fbl::RefPtr<Device>& dev, bool forced);

  // Pushes |new_device| to |devices_|.
  void AddToDevices(fbl::RefPtr<Device> new_device);

  fbl::TaggedDoublyLinkedList<fbl::RefPtr<Device>, Device::AllDevicesListTag>& devices() {
    return devices_;
  }

  const fbl::TaggedDoublyLinkedList<fbl::RefPtr<Device>, Device::AllDevicesListTag>& devices()
      const {
    return devices_;
  }

  // Returns all composite devices that are not added through composite node specs.
  // TODO(fxb/115878): DeviceManager should also return composites in |composites_from_specs_|.
  fbl::DoublyLinkedList<std::unique_ptr<CompositeDevice>>& composite_devices() {
    return composite_devices_;
  }

 private:
  // Owner. Must outlive DeviceManager.
  Coordinator* coordinator_;

  // All Devices (excluding static immortal devices)
  fbl::TaggedDoublyLinkedList<fbl::RefPtr<Device>, Device::AllDevicesListTag> devices_;

  // All composite devices that are added from the legacy composite system.
  fbl::DoublyLinkedList<std::unique_ptr<CompositeDevice>> composite_devices_;

  // All composite devices that are added from composite node specs.
  CompositeNodeSpecMap composites_from_specs_;

  DriverHostCrashPolicy crash_policy_;
};

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_V1_DEVICE_MANAGER_H_
