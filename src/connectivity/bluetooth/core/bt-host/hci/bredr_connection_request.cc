// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bredr_connection_request.h"

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"

namespace bt::hci {

EmbossCommandPacket CreateConnectionPacket(
    DeviceAddress address,
    std::optional<hci_spec::PageScanRepetitionMode> page_scan_repetition_mode,
    std::optional<uint16_t> clock_offset) {
  auto request = EmbossCommandPacket::New<hci_spec::CreateConnectionCommandWriter>(
      hci_spec::kCreateConnection);
  auto params = request.view_t();
  params.bd_addr().CopyFrom(address.value().view());
  params.packet_type().BackingStorage().WriteUInt(kEnableAllPacketTypes);

  // The Page Scan Repetition Mode of the remote device as retrieved by Inquiry.
  // If we do not have one for the device, opt for R2 so we will send for at
  // least 2.56s
  if (page_scan_repetition_mode) {
    params.page_scan_repetition_mode().Write(*page_scan_repetition_mode);
  } else {
    params.page_scan_repetition_mode().Write(hci_spec::PageScanRepetitionMode::R2_);
  }

  params.reserved().Write(0);  // Reserved, must be set to 0.

  // Clock Offset.  The lower 15 bits are set to the clock offset as retrieved
  // by an Inquiry. The highest bit is set to 1 if the rest of this parameter
  // is valid. If we don't have one, use the default.
  if (clock_offset) {
    params.clock_offset().valid().Write(true);
    params.clock_offset().clock_offset().Write(*clock_offset);
  } else {
    params.clock_offset().valid().Write(false);
  }

  params.allow_role_switch().Write(hci_spec::GenericEnableParam::DISABLE);

  return request;
}

void BrEdrConnectionRequest::CreateConnection(
    CommandChannel* command_channel, async_dispatcher_t* dispatcher,
    std::optional<uint16_t> clock_offset,
    std::optional<hci_spec::PageScanRepetitionMode> page_scan_repetition_mode, zx::duration timeout,
    OnCompleteDelegate on_command_fail) {
  BT_DEBUG_ASSERT(timeout > zx::msec(0));

  // HCI Command Status Event will be sent as our completion callback.
  auto self = weak_self_.GetWeakPtr();
  auto complete_cb = [self, timeout, peer_id = peer_id_,
                      on_command_fail = std::move(on_command_fail)](auto,
                                                                    const EventPacket& event) {
    BT_DEBUG_ASSERT(event.event_code() == hci_spec::kCommandStatusEventCode);

    if (!self.is_alive())
      return;

    Result<> status = event.ToResult();
    if (status.is_error()) {
      on_command_fail(status, peer_id);
    } else {
      // Both CommandChannel and the controller perform some scheduling, so log when the controller
      // finally acknowledges Create Connection to observe outgoing connection sequencing.
      // TODO(fxbug.dev/92299): Added to investigate timing and can be removed if it adds no value
      bt_log(INFO, "hci-bredr", "Create Connection for peer %s successfully dispatched",
             bt_str(peer_id));

      // The request was started but has not completed; initiate the command
      // timeout period. NOTE: The request will complete when the controller
      // asynchronously notifies us of with a BrEdr Connection Complete event.
      self->timeout_task_.PostDelayed(async_get_default_dispatcher(), timeout);
    }
  };

  auto packet = CreateConnectionPacket(peer_address_, page_scan_repetition_mode, clock_offset);

  bt_log(INFO, "hci-bredr", "initiating connection request (peer: %s)", bt_str(peer_id_));
  command_channel->SendCommand(std::move(packet), std::move(complete_cb),
                               hci_spec::kCommandStatusEventCode);
}

// Status is either a Success or an Error value
Result<> BrEdrConnectionRequest::CompleteRequest(Result<> status) {
  bt_log(INFO, "hci-bredr", "connection complete (status: %s, peer: %s)", bt_str(status),
         bt_str(peer_id_));
  timeout_task_.Cancel();

  if (status.is_error()) {
    if (state_ == RequestState::kTimedOut) {
      return ToResult(HostError::kTimedOut);
    }
    if (status == ToResult(hci_spec::StatusCode::UNKNOWN_CONNECTION_ID)) {
      // The "Unknown Connection Identifier" error code is returned if this
      // event was sent due to a successful cancellation via the
      // HCI_Create_Connection_Cancel command
      // See Core Spec v5.0 Vol 2, Part E, Section 7.1.7
      state_ = RequestState::kCanceled;
      return ToResult(HostError::kCanceled);
    }
  }
  state_ = RequestState::kSuccess;
  return status;
}

void BrEdrConnectionRequest::Timeout() {
  // If the request was cancelled, this handler will have been removed
  BT_ASSERT(state_ == RequestState::kPending);
  bt_log(INFO, "hci-bredr", "create connection timed out: canceling request (peer: %s)",
         bt_str(peer_id_));
  state_ = RequestState::kTimedOut;
  timeout_task_.Cancel();
}

bool BrEdrConnectionRequest::Cancel() {
  if (state_ == RequestState::kSuccess) {
    bt_log(DEBUG, "hci-bredr", "connection has already succeeded (peer: %s)", bt_str(peer_id_));
    return false;
  }
  if (state_ != RequestState::kPending) {
    bt_log(WARN, "hci-bredr", "connection attempt already canceled! (peer: %s)", bt_str(peer_id_));
    return false;
  }
  // TODO(fxbug.dev/65157) - We should correctly handle cancels due to a disconnect call during a
  // pending connection creation attempt
  bt_log(INFO, "hci-bredr", "canceling connection request (peer: %s)", bt_str(peer_id_));
  state_ = RequestState::kCanceled;
  timeout_task_.Cancel();
  return true;
}

BrEdrConnectionRequest::~BrEdrConnectionRequest() { Cancel(); }

}  // namespace bt::hci
