// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_SERVICES_DEVICE_REGISTRY_REGISTRY_SERVER_H_
#define SRC_MEDIA_AUDIO_SERVICES_DEVICE_REGISTRY_REGISTRY_SERVER_H_

#include <fidl/fuchsia.audio.device/cpp/fidl.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <vector>

#include "src/media/audio/services/common/base_fidl_server.h"
#include "src/media/audio/services/device_registry/logging.h"

namespace media_audio {

class AudioDeviceRegistry;
class Device;

class RegistryServer
    : public std::enable_shared_from_this<RegistryServer>,
      public BaseFidlServer<RegistryServer, fidl::Server, fuchsia_audio_device::Registry> {
 public:
  static std::shared_ptr<RegistryServer> Create(
      std::shared_ptr<const FidlThread> thread,
      fidl::ServerEnd<fuchsia_audio_device::Registry> server_end,
      std::shared_ptr<AudioDeviceRegistry> parent);
  ~RegistryServer() override;

  // fuchsia.audio.device.Registry implementation
  void WatchDevicesAdded(WatchDevicesAddedCompleter::Sync& completer) final;
  void WatchDeviceRemoved(WatchDeviceRemovedCompleter::Sync& completer) final;
  void CreateObserver(CreateObserverRequest& request,
                      CreateObserverCompleter::Sync& completer) final;

  void DeviceWasAdded(std::shared_ptr<Device> new_device);
  void DeviceWasRemoved(uint64_t token_id);

  // Static object count, for debugging purposes.
  static uint64_t count() { return count_; }

 private:
  template <typename ServerT, template <typename T> typename FidlServerT, typename ProtocolT>
  friend class BaseFidlServer;

  static inline const std::string_view kClassName = "RegistryServer";
  static uint64_t count_;

  explicit RegistryServer(std::shared_ptr<AudioDeviceRegistry> parent);
  void ReplyWithAddedDevices();
  void ReplyWithNextRemovedDevice();

  std::shared_ptr<AudioDeviceRegistry> parent_;

  std::vector<fuchsia_audio_device::Info> devices_added_since_notify_;
  std::optional<WatchDevicesAddedCompleter::Async> watch_devices_added_completer_;

  std::queue<uint64_t> devices_removed_since_notify_;
  std::optional<WatchDeviceRemovedCompleter::Async> watch_device_removed_completer_;
};

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_SERVICES_DEVICE_REGISTRY_REGISTRY_SERVER_H_
