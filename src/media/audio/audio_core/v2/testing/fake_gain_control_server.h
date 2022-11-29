// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_V2_TESTING_FAKE_GAIN_CONTROL_SERVER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_V2_TESTING_FAKE_GAIN_CONTROL_SERVER_H_

#include <fidl/fuchsia.audio/cpp/natural_messaging.h>
#include <fidl/fuchsia.audio/cpp/natural_types.h>

#include <variant>
#include <vector>

#include "src/media/audio/services/common/base_fidl_server.h"

namespace media_audio {

class FakeGainControlServer
    : public BaseFidlServer<FakeGainControlServer, fidl::Server, fuchsia_audio::GainControl> {
 public:
  static std::shared_ptr<FakeGainControlServer> Create(
      std::shared_ptr<const FidlThread> fidl_thread,
      fidl::ServerEnd<fuchsia_audio::GainControl> server_end) {
    return BaseFidlServer::Create(std::move(fidl_thread), std::move(server_end));
  }

  // Log of all calls to this server.
  using CallType = std::variant<SetGainRequest, SetMuteRequest>;
  const std::vector<CallType>& calls() const { return calls_; }

  // Implementation of fidl::Server<fuchsia_audio::GainControl>.
  void SetGain(SetGainRequest& request, SetGainCompleter::Sync& completer) final {
    calls_.push_back(std::move(request));
    completer.Reply(fit::ok(fuchsia_audio::GainControlSetGainResponse()));
  }
  void SetMute(SetMuteRequest& request, SetMuteCompleter::Sync& completer) final {
    calls_.push_back(std::move(request));
    completer.Reply(fit::ok(fuchsia_audio::GainControlSetMuteResponse()));
  }

 private:
  static inline constexpr std::string_view kClassName = "FakeGainControlServer";
  template <typename ServerT, template <typename T> typename FidlServerT, typename ProtocolT>
  friend class BaseFidlServer;

  FakeGainControlServer() = default;

  std::vector<CallType> calls_;
};

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_V2_TESTING_FAKE_GAIN_CONTROL_SERVER_H_
