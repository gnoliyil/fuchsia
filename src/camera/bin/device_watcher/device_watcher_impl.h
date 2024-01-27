// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_BIN_DEVICE_WATCHER_DEVICE_WATCHER_IMPL_H_
#define SRC_CAMERA_BIN_DEVICE_WATCHER_DEVICE_WATCHER_IMPL_H_

#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <fuchsia/camera3/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>
#include <lib/fpromise/result.h>
#include <lib/sys/cpp/component_context.h>
#include <zircon/status.h>

#include <memory>
#include <queue>
#include <set>
#include <unordered_map>

#include "lib/async/dispatcher.h"
#include "src/camera/bin/device_watcher/device_instance.h"

namespace camera {

constexpr auto kCameraPath = "/dev/class/camera";

constexpr std::string_view kMipiCsiDeviceInstanceCollectionName{"csi_camera_devices"};
constexpr std::string_view kMipiCsiDeviceInstanceNamePrefix{"csi_camera_device_"};
constexpr std::string_view kMipiCsiDeviceInstanceUrl{
    "fuchsia-pkg://fuchsia.com/camera_device#meta/camera_device.cm"};

constexpr std::string_view kUvcDeviceInstanceCollectionName{"usb_camera_devices"};
constexpr std::string_view kUvcDeviceInstanceNamePrefix{"usb_camera_device_"};
constexpr std::string_view kUvcDeviceInstanceUrl{
    "fuchsia-pkg://fuchsia.com/usb_camera_device#meta/usb_camera_device.cm"};

using ClientId = uint64_t;
using TransientDeviceId = uint64_t;
using PersistentDeviceId = uint64_t;

enum CameraType {
  kCameraTypeMipiCsi = 1,
  kCameraTypeUvc = 2,
};

struct UniqueDevice {
  TransientDeviceId id;
  std::unique_ptr<DeviceInstance> instance;
};

using DevicesMap = std::unordered_map<PersistentDeviceId, UniqueDevice>;

class DeviceWatcherImpl {
 public:
  static fpromise::result<std::unique_ptr<DeviceWatcherImpl>, zx_status_t> Create(
      std::unique_ptr<sys::ComponentContext> context, fuchsia::component::RealmHandle realm,
      async_dispatcher_t* dispatcher);
  void AddDeviceByPath(const std::string& path);
  void UpdateClients();
  fidl::InterfaceRequestHandler<fuchsia::camera3::DeviceWatcher> GetHandler();

  sys::ComponentContext* context() { return context_.get(); }

 private:
  void OnNewRequest(fidl::InterfaceRequest<fuchsia::camera3::DeviceWatcher> request);

  void ConnectDynamicChild(fidl::InterfaceRequest<fuchsia::camera3::Device> request,
                           const UniqueDevice& unique_device);

  fpromise::result<PersistentDeviceId, zx_status_t> AddMipiCsiDevice(
      fuchsia::hardware::camera::DeviceHandle camera, const std::string& path);

  fpromise::result<PersistentDeviceId, zx_status_t> AddUvcDevice(
      fuchsia::hardware::camera::DeviceHandle camera, const std::string& path);

  // Implements the server endpoint for a single client, and maintains per-client state.
  class Client : public fuchsia::camera3::DeviceWatcher {
   public:
    explicit Client(DeviceWatcherImpl& watcher);
    static fpromise::result<std::unique_ptr<Client>, zx_status_t> Create(
        DeviceWatcherImpl& watcher, ClientId id,
        fidl::InterfaceRequest<fuchsia::camera3::DeviceWatcher> request,
        async_dispatcher_t* dispatcher);
    void UpdateDevices(const DevicesMap& devices);
    explicit operator bool();

   private:
    void CheckDevicesChanged();
    // |fuchsia::camera3::DeviceWatcher|
    void WatchDevices(WatchDevicesCallback callback) override;
    void ConnectToDevice(TransientDeviceId id,
                         fidl::InterfaceRequest<fuchsia::camera3::Device> request) override;

    DeviceWatcherImpl& watcher_;
    ClientId id_;
    fidl::Binding<fuchsia::camera3::DeviceWatcher> binding_;
    WatchDevicesCallback callback_;
    std::set<TransientDeviceId> last_known_ids_;
    std::optional<std::set<TransientDeviceId>> last_sent_ids_;
  };

  async_dispatcher_t* dispatcher_;
  std::unique_ptr<sys::ComponentContext> context_;
  fuchsia::component::RealmPtr realm_;
  TransientDeviceId device_id_next_ = 1;
  DevicesMap devices_;
  ClientId client_id_next_ = 1;
  std::unordered_map<ClientId, std::unique_ptr<Client>> clients_;
  bool initial_update_received_ = false;
  std::queue<fidl::InterfaceRequest<fuchsia::camera3::DeviceWatcher>> requests_;
};

}  // namespace camera

#endif  // SRC_CAMERA_BIN_DEVICE_WATCHER_DEVICE_WATCHER_IMPL_H_
