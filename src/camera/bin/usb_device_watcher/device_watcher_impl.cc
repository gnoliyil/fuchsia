// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/usb_device_watcher/device_watcher_impl.h"

#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <fuchsia/camera3/cpp/fidl.h>
#include <fuchsia/component/cpp/fidl.h>
#include <fuchsia/component/decl/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <algorithm>
#include <iterator>
#include <limits>

#include "src/camera/bin/usb_device_watcher/device_instance.h"

namespace camera {

fpromise::result<std::unique_ptr<DeviceWatcherImpl>, zx_status_t> DeviceWatcherImpl::Create(
    std::unique_ptr<sys::ComponentContext> context, fuchsia::component::RealmHandle realm,
    async_dispatcher_t* dispatcher) {
  auto server = std::make_unique<DeviceWatcherImpl>();

  server->context_ = std::move(context);
  server->dispatcher_ = dispatcher;

  ZX_ASSERT(server->realm_.Bind(std::move(realm), server->dispatcher_) == ZX_OK);
  return fpromise::ok(std::move(server));
}

fpromise::result<PersistentDeviceId, zx_status_t> DeviceWatcherImpl::AddDevice(
    fuchsia::hardware::camera::DeviceHandle camera, const std::string& path) {
  FX_LOGS(INFO) << "AddDevice: " << path;
  fuchsia::hardware::camera::DeviceSyncPtr dev;
  dev.Bind(std::move(camera));

  fuchsia::camera2::DeviceInfo info_return;  // FAKE
  info_return.set_vendor_id(0x1212);
  info_return.set_product_id(0x9898);

  // TODO(fxbug.dev/43565): This generates the same ID for multiple instances of the same device. It
  // should be made unique by incorporating a truly unique value such as the bus ID.
  constexpr uint32_t kVendorShift = 16;
  PersistentDeviceId persistent_id =
      (static_cast<uint64_t>(info_return.vendor_id()) << kVendorShift) | info_return.product_id();

  // Launch the camera device instance.
  std::string instance_name = std::string(kDeviceInstanceNamePrefix) + path;
  auto result = DeviceInstance::Create(dev.Unbind(), realm_, dispatcher_, instance_name);
  if (result.is_error()) {
    FX_PLOGS(ERROR, result.error()) << "Failed to launch device instance.";
    return fpromise::error(result.error());
  }
  auto instance = result.take_value();

  devices_[persistent_id] = {.id = device_id_next_, .instance = std::move(instance)};
  FX_LOGS(DEBUG) << "Added device " << persistent_id << " as device ID " << device_id_next_;
  ++device_id_next_;

  return fpromise::ok(persistent_id);
}

void DeviceWatcherImpl::UpdateClients() {
  if (!initial_update_received_) {
    initial_update_received_ = true;
    while (!requests_.empty()) {
      OnNewRequest(std::move(requests_.front()));
      requests_.pop();
    }
  }
  for (auto& client : clients_) {
    client.second->UpdateDevices(devices_);
  }
}

fidl::InterfaceRequestHandler<fuchsia::camera3::DeviceWatcher> DeviceWatcherImpl::GetHandler() {
  return fit::bind_member(this, &DeviceWatcherImpl::OnNewRequest);
}

void DeviceWatcherImpl::OnNewRequest(
    fidl::InterfaceRequest<fuchsia::camera3::DeviceWatcher> request) {
  if (!initial_update_received_) {
    requests_.push(std::move(request));
    return;
  }
  auto result = Client::Create(*this, client_id_next_, std::move(request), dispatcher_);
  if (result.is_error()) {
    FX_PLOGS(ERROR, result.error());
    return;
  }
  auto client = result.take_value();
  clients_[client_id_next_] = std::move(client);
  FX_LOGS(DEBUG) << "DeviceWatcher client " << client_id_next_ << " connected.";
  ++client_id_next_;
}

void DeviceWatcherImpl::ConnectDynamicChild(
    fidl::InterfaceRequest<fuchsia::camera3::Device> request, const UniqueDevice& unique_device) {
  fuchsia::component::decl::ChildRef child;
  child.name = unique_device.instance->name();
  child.collection = kDeviceInstanceCollectionName;

  fidl::InterfaceHandle<fuchsia::io::Directory> exposed_dir;
  realm_->OpenExposedDir(
      child, exposed_dir.NewRequest(),
      [exposed_dir = std::move(exposed_dir), request = std::move(request)](
          fuchsia::component::Realm_OpenExposedDir_Result result) mutable {
        if (result.is_err()) {
          FX_LOGS(ERROR) << "Failed to connect to exposed directory. Result: "
                         << static_cast<long>(result.err());

          // TODO(b/241604541) - Need a more graceful recovery here.
          ZX_ASSERT(false);
        }
        std::shared_ptr<sys::ServiceDirectory> svc_dir =
            std::make_shared<sys::ServiceDirectory>(sys::ServiceDirectory(std::move(exposed_dir)));
        // Connect to the service on behalf of client.
        svc_dir->Connect(std::move(request), "fuchsia.camera3.Device");
      });
}

DeviceWatcherImpl::Client::Client(DeviceWatcherImpl& watcher) : watcher_(watcher), binding_(this) {}

fpromise::result<std::unique_ptr<DeviceWatcherImpl::Client>, zx_status_t>
DeviceWatcherImpl::Client::Create(DeviceWatcherImpl& watcher, ClientId id,
                                  fidl::InterfaceRequest<fuchsia::camera3::DeviceWatcher> request,
                                  async_dispatcher_t* dispatcher) {
  auto client = std::make_unique<DeviceWatcherImpl::Client>(watcher);

  client->id_ = id;
  client->UpdateDevices(watcher.devices_);

  zx_status_t status = client->binding_.Bind(request.TakeChannel(), dispatcher);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }

  client->binding_.set_error_handler([client = client.get()](zx_status_t status) {
    FX_PLOGS(DEBUG, status) << "DeviceWatcher client " << client->id_ << " disconnected.";
    client->watcher_.clients_.erase(client->id_);
  });

  return fpromise::ok(std::move(client));
}

void DeviceWatcherImpl::Client::UpdateDevices(const DevicesMap& devices) {
  last_known_ids_.clear();
  for (const auto& device : devices) {
    last_known_ids_.insert(device.second.id);
  }
  CheckDevicesChanged();
}

DeviceWatcherImpl::Client::operator bool() { return binding_.is_bound(); }

// Constructs the set of events that should be returned to the client, or null if the response
// should not be sent.
static std::optional<std::vector<fuchsia::camera3::WatchDevicesEvent>> BuildEvents(
    const std::set<TransientDeviceId>& last_known,
    const std::optional<std::set<TransientDeviceId>>& last_sent) {
  std::vector<fuchsia::camera3::WatchDevicesEvent> events;

  // If never sent, just populate with Added events for the known IDs.
  if (!last_sent.has_value()) {
    for (auto id : last_known) {
      events.push_back(fuchsia::camera3::WatchDevicesEvent::WithAdded(std::move(id)));
    }
    return events;
  }

  // Otherwise, build a full event list.
  std::set<TransientDeviceId> existing;
  std::set<TransientDeviceId> added;
  std::set<TransientDeviceId> removed;

  // Existing = Known && Sent
  std::set_intersection(last_known.begin(), last_known.end(), last_sent.value().begin(),
                        last_sent.value().end(), std::inserter(existing, existing.begin()));

  // Added = Known - Sent
  std::set_difference(last_known.begin(), last_known.end(), last_sent.value().begin(),
                      last_sent.value().end(), std::inserter(added, added.begin()));

  // Removed = Sent - Known
  std::set_difference(last_sent.value().begin(), last_sent.value().end(), last_known.begin(),
                      last_known.end(), std::inserter(removed, removed.begin()));

  if (added.empty() && removed.empty()) {
    return std::nullopt;
  }

  for (auto id : existing) {
    events.push_back(fuchsia::camera3::WatchDevicesEvent::WithExisting(std::move(id)));
  }
  for (auto id : added) {
    events.push_back(fuchsia::camera3::WatchDevicesEvent::WithAdded(std::move(id)));
  }
  for (auto id : removed) {
    events.push_back(fuchsia::camera3::WatchDevicesEvent::WithRemoved(std::move(id)));
  }

  return events;
}

void DeviceWatcherImpl::Client::CheckDevicesChanged() {
  if (!callback_) {
    return;
  }

  auto events = BuildEvents(last_known_ids_, last_sent_ids_);
  if (!events.has_value()) {
    return;
  }

  callback_(std::move(events.value()));

  callback_ = nullptr;

  last_sent_ids_ = last_known_ids_;
}

void DeviceWatcherImpl::Client::WatchDevices(WatchDevicesCallback callback) {
  if (callback_) {
    FX_LOGS(INFO) << "Client called WatchDevices while a previous call was still pending.";
    binding_.Close(ZX_ERR_BAD_STATE);
    return;
  }

  callback_ = std::move(callback);

  CheckDevicesChanged();
}

void DeviceWatcherImpl::Client::ConnectToDevice(
    TransientDeviceId id, fidl::InterfaceRequest<fuchsia::camera3::Device> request) {
  if (!last_sent_ids_.has_value()) {
    FX_LOGS(INFO) << "Clients must watch for devices prior to attempting a connection.";
    request.Close(ZX_ERR_BAD_STATE);
    return;
  }

  // Find the UniqueDevice to connect to.
  const auto& devices = watcher_.devices_;
  auto unique_device = std::find_if(devices.begin(), devices.end(),
                                    [=](const auto& it) { return it.second.id == id; });
  if (unique_device == devices.end()) {
    request.Close(ZX_ERR_NOT_FOUND);
    return;
  }

  // Create and connect to dynamic child instance.
  watcher_.ConnectDynamicChild(std::move(request), unique_device->second);
}

}  // namespace camera
