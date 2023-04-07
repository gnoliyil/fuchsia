// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acl_connection.h"

#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"

namespace bt::hci {

namespace {

template <
    CommandChannel::EventCallbackResult (AclConnection::*EventHandlerMethod)(const EventPacket&)>
CommandChannel::EventCallback BindEventHandler(const WeakSelf<AclConnection>::WeakPtr& conn) {
  return [conn](const EventPacket& event) {
    if (conn.is_alive()) {
      return (conn.get().*EventHandlerMethod)(event);
    }
    return CommandChannel::EventCallbackResult::kRemove;
  };
}

template <CommandChannel::EventCallbackResult (AclConnection::*EventHandlerMethod)(
    const EmbossEventPacket&)>
CommandChannel::EmbossEventCallback BindEventHandler(const WeakPtr<AclConnection>& conn) {
  return [conn](const EmbossEventPacket& event) {
    if (conn.is_alive()) {
      return (conn.get().*EventHandlerMethod)(event);
    }
    return CommandChannel::EventCallbackResult::kRemove;
  };
}

}  // namespace

AclConnection::AclConnection(hci_spec::ConnectionHandle handle, const DeviceAddress& local_address,
                             const DeviceAddress& peer_address,
                             pw::bluetooth::emboss::ConnectionRole role,
                             const Transport::WeakPtr& hci)
    : Connection(handle, local_address, peer_address, hci,
                 [handle, hci] { AclConnection::OnDisconnectionComplete(handle, hci); }),
      role_(role),
      weak_self_(this) {
  auto self = weak_self_.GetWeakPtr();
  enc_change_id_ = hci->command_channel()->AddEventHandler(
      hci_spec::kEncryptionChangeEventCode,
      BindEventHandler<&AclConnection::OnEncryptionChangeEvent>(self));
  enc_key_refresh_cmpl_id_ = hci->command_channel()->AddEventHandler(
      hci_spec::kEncryptionKeyRefreshCompleteEventCode,
      BindEventHandler<&AclConnection::OnEncryptionKeyRefreshCompleteEvent>(self));
}

AclConnection::~AclConnection() {
  // Unregister HCI event handlers.
  hci()->command_channel()->RemoveEventHandler(enc_change_id_);
  hci()->command_channel()->RemoveEventHandler(enc_key_refresh_cmpl_id_);
}

void AclConnection::OnDisconnectionComplete(hci_spec::ConnectionHandle handle,
                                            const Transport::WeakPtr& hci) {
  if (!hci.is_alive()) {
    return;
  }
  // Notify ACL data channel that packets have been flushed from controller buffer.
  hci->acl_data_channel()->ClearControllerPacketCount(handle);
}

CommandChannel::EventCallbackResult AclConnection::OnEncryptionChangeEvent(
    const EmbossEventPacket& event) {
  BT_ASSERT(event.event_code() == hci_spec::kEncryptionChangeEventCode);

  auto params = event.unchecked_view<pw::bluetooth::emboss::EncryptionChangeEventV1View>();
  if (!params.Ok()) {
    bt_log(WARN, "hci", "malformed encryption change event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  hci_spec::ConnectionHandle handle = params.connection_handle().Read();

  // Silently ignore the event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  if (state() != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "encryption change ignored: connection closed");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  Result<> result = event.ToResult();
  bool enabled = params.encryption_enabled().Read() != pw::bluetooth::emboss::EncryptionStatus::OFF;

  bt_log(DEBUG, "hci", "encryption change (%s) %s", enabled ? "enabled" : "disabled",
         bt_str(result));

  HandleEncryptionStatus(result.is_ok() ? Result<bool>(fit::ok(enabled)) : result.take_error(),
                         /*key_refreshed=*/false);
  return CommandChannel::EventCallbackResult::kContinue;
}

CommandChannel::EventCallbackResult AclConnection::OnEncryptionKeyRefreshCompleteEvent(
    const EventPacket& event) {
  BT_ASSERT(event.event_code() == hci_spec::kEncryptionKeyRefreshCompleteEventCode);

  if (event.view().payload_size() != sizeof(hci_spec::EncryptionKeyRefreshCompleteEventParams)) {
    bt_log(WARN, "hci", "malformed encryption key refresh complete event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  const auto& params = event.params<hci_spec::EncryptionKeyRefreshCompleteEventParams>();
  hci_spec::ConnectionHandle handle = le16toh(params.connection_handle);

  // Silently ignore this event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  if (state() != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "encryption key refresh ignored: connection closed");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  Result<> status = event.ToResult();
  bt_log(DEBUG, "hci", "encryption key refresh %s", bt_str(status));

  // Report that encryption got disabled on failure status. The accuracy of this
  // isn't that important since the link will be disconnected.
  HandleEncryptionStatus(
      status.is_ok() ? Result<bool>(fit::ok(/*enabled=*/true)) : status.take_error(),
      /*key_refreshed=*/true);

  return CommandChannel::EventCallbackResult::kContinue;
}

}  // namespace bt::hci
