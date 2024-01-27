// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/services/device_registry/control_creator_server.h"

#include <fidl/fuchsia.audio.device/cpp/fidl.h>
#include <lib/fit/internal/result.h>
#include <lib/syslog/cpp/macros.h>

#include <optional>

#include "src/media/audio/services/device_registry/audio_device_registry.h"
#include "src/media/audio/services/device_registry/device.h"
#include "src/media/audio/services/device_registry/logging.h"

namespace media_audio {

// static
uint64_t ControlCreatorServer::count_ = 0;

std::shared_ptr<ControlCreatorServer> ControlCreatorServer::Create(
    std::shared_ptr<const FidlThread> thread,
    fidl::ServerEnd<fuchsia_audio_device::ControlCreator> server_end,
    std::shared_ptr<AudioDeviceRegistry> parent) {
  ADR_LOG_CLASS(kLogControlCreatorServerMethods) << " parent " << parent;

  return BaseFidlServer::Create(std::move(thread), std::move(server_end), parent);
}

ControlCreatorServer::ControlCreatorServer(std::shared_ptr<AudioDeviceRegistry> parent)
    : parent_(parent) {
  ADR_LOG_OBJECT(kLogObjectLifetimes);
  ++ControlCreatorServer::count_;
  LogObjectCounts();
}

ControlCreatorServer::~ControlCreatorServer() {
  ADR_LOG_OBJECT(kLogObjectLifetimes);
  --ControlCreatorServer::count_;
  LogObjectCounts();
}

void ControlCreatorServer::Create(CreateRequest& request, CreateCompleter::Sync& completer) {
  ADR_LOG_OBJECT(kLogControlCreatorServerMethods);

  if (!request.token_id()) {
    ADR_WARN_OBJECT() << "required 'token_id' is absent";
    completer.Reply(fit::error(fuchsia_audio_device::ControlCreatorError::kInvalidTokenId));
    return;
  }

  if (!request.control_server()) {
    ADR_WARN_OBJECT() << "required 'control_server' is absent";
    completer.Reply(fit::error(fuchsia_audio_device::ControlCreatorError::kInvalidControl));
    return;
  }

  auto [status, device] = parent_->FindDeviceByTokenId(*request.token_id());
  // We could break these out into separate error codes if needed.
  if (status == AudioDeviceRegistry::DevicePresence::Unknown) {
    completer.Reply(fit::error(fuchsia_audio_device::ControlCreatorError::kDeviceNotFound));
    return;
  }
  if (status == AudioDeviceRegistry::DevicePresence::Error) {
    completer.Reply(fit::error(fuchsia_audio_device::ControlCreatorError::kDeviceError));
    return;
  }

  FX_CHECK(device);
  // TODO(fxbug.dev/117199): Decide when we proactively call GetHealthState, if at all.

  // For now, just mark the Device as controlled.
  if (!device->SetControl()) {
    completer.Reply(fit::error(fuchsia_audio_device::ControlCreatorError::kDeviceAlreadyAllocated));
    return;
  }

  completer.Reply(fit::success(fuchsia_audio_device::ControlCreatorCreateResponse{}));
}

}  // namespace media_audio
