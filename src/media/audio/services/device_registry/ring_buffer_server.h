// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_SERVICES_DEVICE_REGISTRY_RING_BUFFER_SERVER_H_
#define SRC_MEDIA_AUDIO_SERVICES_DEVICE_REGISTRY_RING_BUFFER_SERVER_H_

#include <fidl/fuchsia.audio.device/cpp/fidl.h>

#include <memory>

#include "src/media/audio/services/common/base_fidl_server.h"
#include "src/media/audio/services/device_registry/control_server.h"
#include "src/media/audio/services/device_registry/device.h"

namespace media_audio {

class RingBufferServer
    : public std::enable_shared_from_this<RingBufferServer>,
      public BaseFidlServer<RingBufferServer, fidl::Server, fuchsia_audio_device::RingBuffer> {
 public:
  static std::shared_ptr<RingBufferServer> Create(
      std::shared_ptr<const FidlThread> thread,
      fidl::ServerEnd<fuchsia_audio_device::RingBuffer> server_end,
      std::shared_ptr<ControlServer> parent, std::shared_ptr<Device> device);

  void OnShutdown(fidl::UnbindInfo info) override;
  ~RingBufferServer() override;
  void DeviceDroppedRingBuffer();
  void ClientDroppedControl();

  // fuchsia.audio.device.RingBuffer implementation
  void SetActiveChannels(SetActiveChannelsRequest& request,
                         SetActiveChannelsCompleter::Sync& completer) override;

  void Start(StartRequest& request, StartCompleter::Sync& completer) override;
  void Stop(StopRequest& request, StopCompleter::Sync& completer) override;
  void WatchDelayInfo(WatchDelayInfoCompleter::Sync& completer) override;

  void DelayInfoChanged(const fuchsia_audio_device::DelayInfo& delay_info);

  // Static object count, for debugging purposes.
  static inline uint64_t count() { return count_; }

 private:
  template <typename ServerT, template <typename T> typename FidlServerT, typename ProtocolT>
  friend class BaseFidlServer;

  static inline const std::string_view kClassName = "RingBufferServer";
  static inline uint64_t count_ = 0;

  RingBufferServer(std::shared_ptr<ControlServer> parent, std::shared_ptr<Device> device);

  std::shared_ptr<ControlServer> parent_;
  std::shared_ptr<Device> device_;

  std::optional<SetActiveChannelsCompleter::Async> active_channels_completer_;

  std::optional<StartCompleter::Async> start_completer_;
  std::optional<StopCompleter::Async> stop_completer_;
  bool started_ = false;

  std::optional<WatchDelayInfoCompleter::Async> delay_info_completer_;
  std::optional<fuchsia_audio_device::DelayInfo> delay_info_update_;

  bool device_dropped_ring_buffer_ = false;
};

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_SERVICES_DEVICE_REGISTRY_RING_BUFFER_SERVER_H_
