// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_HOST_DRIVER_HOST_CONTEXT_H_
#define SRC_DEVICES_BIN_DRIVER_HOST_DRIVER_HOST_CONTEXT_H_

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fit/function.h>
#include <lib/zx/event.h>
#include <lib/zx/resource.h>
#include <lib/zx/result.h>

#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <fbl/ref_ptr.h>

#include "src/devices/bin/driver_host/driver.h"
#include "src/devices/bin/driver_host/inspect.h"
#include "src/devices/bin/driver_host/lock.h"
#include "src/devices/bin/driver_host/zx_device.h"
#include "src/devices/bin/driver_host/zx_driver.h"
#include "src/devices/bin/driver_host2/driver.h"

class DriverHostContext {
 public:
  using Callback = fit::inline_callback<void(void), 2 * sizeof(void*)>;

  explicit DriverHostContext(const async_loop_config_t* config, zx::resource root_resource = {})
      : loop_(config), root_resource_(std::move(root_resource)) {}

  ~DriverHostContext();

  void SetupDriverHostController(
      fidl::ServerEnd<fuchsia_device_manager::DriverHostController> request);

  void ProxyIosDestroy(const fbl::RefPtr<zx_device_t>& dev);

  // routines driver_host uses to talk to driver_manager
  zx_status_t DriverManagerAdd(const fbl::RefPtr<zx_device_t>& dev,
                               const fbl::RefPtr<zx_device_t>& child, device_add_args_t* add_args,
                               zx::vmo inspect, fidl::ClientEnd<fuchsia_io::Directory> outgoing_dir)
      TA_REQ(api_lock_);
  // Note that DriverManagerRemove() takes a RefPtr rather than a const RefPtr&.
  // It intends to consume a reference.
  zx_status_t DriverManagerRemove(fbl::RefPtr<zx_device_t> dev) TA_REQ(api_lock_);

  zx_status_t DeviceAdd(const fbl::RefPtr<zx_device_t>& dev, const fbl::RefPtr<zx_device_t>& parent,
                        device_add_args_t* add_args, zx::vmo inspect,
                        fidl::ClientEnd<fuchsia_io::Directory> outgoing_dir) TA_REQ(api_lock_);

  zx_status_t DeviceInit(const fbl::RefPtr<zx_device_t>& dev) TA_REQ(api_lock_);
  void DeviceInitReply(const fbl::RefPtr<zx_device_t>& dev, zx_status_t status,
                       const device_init_reply_args_t* args) TA_REQ(api_lock_);
  zx_status_t DeviceRemove(const fbl::RefPtr<zx_device_t>& dev, bool unbind_self = false)
      TA_REQ(api_lock_);
  zx_status_t DeviceCompleteRemoval(const fbl::RefPtr<zx_device_t>& dev) TA_REQ(api_lock_);
  zx_status_t DeviceUnbind(const fbl::RefPtr<zx_device_t>& dev) TA_REQ(api_lock_);
  void DeviceUnbindReply(const fbl::RefPtr<zx_device_t>& dev) TA_REQ(api_lock_);
  zx_status_t DeviceBind(const fbl::RefPtr<zx_device_t>& dev, const char* drv_libname)
      TA_REQ(api_lock_);
  void DeviceSuspendReply(const fbl::RefPtr<zx_device_t>& dev, zx_status_t status,
                          uint8_t out_state) TA_REQ(api_lock_);
  void DeviceResumeReply(const fbl::RefPtr<zx_device_t>& dev, zx_status_t status,
                         uint8_t out_power_state, uint32_t out_perf_state) TA_REQ(api_lock_);
  zx_status_t DeviceRunCompatibilityTests(const fbl::RefPtr<zx_device_t>& dev,
                                          int64_t hook_wait_time,
                                          fit::callback<void(zx_status_t)> cb) TA_REQ(api_lock_);
  zx_status_t DeviceCreate(fbl::RefPtr<Driver> drv, const char* name, void* ctx,
                           const zx_protocol_device_t* ops, fbl::RefPtr<zx_device_t>* out)
      TA_REQ(api_lock_);
  void DeviceSystemSuspend(const fbl::RefPtr<zx_device_t>& dev, uint32_t flags) TA_REQ(api_lock_);
  zx_status_t DeviceSetPerformanceState(const fbl::RefPtr<zx_device_t>& dev,
                                        uint32_t requested_state, uint32_t* out_state);
  void DeviceSystemResume(const fbl::RefPtr<zx_device_t>& dev, uint32_t target_system_state)
      TA_REQ(api_lock_);
  void DeviceDestroy(zx_device_t* dev) TA_REQ(api_lock_);

  // routines driver_host uses to talk to dev coordinator
  zx_status_t ScheduleRemove(const fbl::RefPtr<zx_device_t>& dev, bool unbind_self)
      TA_REQ(api_lock_);

  zx::result<bool> ScheduleUnbindChildren(const fbl::RefPtr<zx_device_t>& dev) TA_REQ(api_lock_);

  zx_status_t LoadFirmware(const zx_driver_t* drv, const fbl::RefPtr<zx_device_t>& dev,
                           const char* path, zx_handle_t* vmo_handle, size_t* size);
  void LoadFirmwareAsync(const zx_driver_t* drv, const fbl::RefPtr<zx_device_t>& dev,
                         const char* path, load_firmware_callback_t callback, void* context);
  zx_status_t GetTopoPath(const fbl::RefPtr<zx_device_t>& dev, char* path, size_t max,
                          size_t* actual);
  zx_status_t GetMetadata(const fbl::RefPtr<zx_device_t>& dev, uint32_t type, void* buf,
                          size_t buflen, size_t* actual) TA_REQ(api_lock_);

  zx_status_t GetMetadataSize(const fbl::RefPtr<zx_device_t>& dev, uint32_t type, size_t* size)
      TA_REQ(api_lock_);

  zx_status_t AddMetadata(const fbl::RefPtr<zx_device_t>& dev, uint32_t type, const void* data,
                          size_t length) TA_REQ(api_lock_);

  zx_status_t DeviceAddComposite(const fbl::RefPtr<zx_device_t>& dev, const char* name,
                                 const composite_device_desc_t* comp_desc) TA_REQ(api_lock_);

  zx_status_t DeviceAddCompositeNodeSpec(const fbl::RefPtr<zx_device_t>& dev, std::string_view name,
                                         const composite_node_spec_t* spec) TA_REQ(api_lock_);

  zx_status_t FindDriver(std::string_view libname, zx::vmo vmo, fbl::RefPtr<zx_driver_t>* out,
                         fbl::RefPtr<Driver>* out_driver);

  zx_status_t ConnectFidlProtocol(const fbl::RefPtr<zx_device_t>& dev,
                                  std::string_view fragment_name, std::string_view service_name,
                                  std::string_view protocol_name, zx::channel request);

  void AddDriver(fbl::RefPtr<dfv2::Driver> driver);
  void RemoveDriver(dfv2::Driver& driver);

  // Called when a zx_device_t has run out of references and needs its destruction finalized.
  void QueueDeviceForFinalization(zx_device_t* device) TA_REQ(api_lock_);

  async::Loop& loop() { return loop_; }

  const zx::resource& root_resource() { return root_resource_; }

  ApiLock& api_lock() TA_RET_CAP(api_lock_) { return api_lock_; }

  DriverHostInspect& inspect() { return inspect_; }

  const std::string& root_driver_path() const { return root_driver_path_; }
  void set_root_driver_path(std::string_view path) { root_driver_path_ = path; }

 private:
  void FinalizeDyingDevices() TA_REQ(api_lock_);

  // enum_lock_{acquire,release}() are used whenever we're iterating
  // on the device tree.  When "enum locked" it is legal to add a new
  // child to the end of a device's list-of-children, but it is not
  // legal to remove a child.  This avoids badness when we have to
  // drop the DM lock to call into device ops while enumerating.
  void enum_lock_acquire() TA_REQ(api_lock_) { enumerators_++; }

  void enum_lock_release() TA_REQ(api_lock_) {
    if (--enumerators_ == 0) {
      FinalizeDyingDevices();
    }
  }

  zx_status_t DeviceValidate(const fbl::RefPtr<zx_device_t>& dev) TA_REQ(api_lock_);

  async::Loop loop_;

  // Used to serialize API operations
  ApiLock api_lock_;

  fbl::DoublyLinkedList<fbl::RefPtr<zx_driver>> drivers_;
  fbl::DoublyLinkedList<fbl::RefPtr<dfv2::Driver>> dfv2_drivers_;

  fbl::TaggedDoublyLinkedList<zx_device*, zx_device::DeferListTag> defer_device_list_
      TA_GUARDED(api_lock_);
  int enumerators_ TA_GUARDED(api_lock_) = 0;

  zx::resource root_resource_;

  DriverHostInspect inspect_;

  fbl::TaggedDoublyLinkedList<zx_device*, zx_device::ChildrenListTag> dead_devices_;
  unsigned int dead_devices_count_ = 0;
  std::string root_driver_path_ = "/boot/driver/";
};

#endif  // SRC_DEVICES_BIN_DRIVER_HOST_DRIVER_HOST_CONTEXT_H_
