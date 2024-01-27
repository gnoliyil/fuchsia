// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/power/drivers/fusb302/fusb302-protocol.h"

#include <lib/ddk/debug.h>
#include <lib/zx/result.h>
#include <lib/zx/time.h>
#include <zircon/assert.h>
#include <zircon/status.h>

#include <cstdint>
#include <utility>

#include "src/devices/power/drivers/fusb302/fusb302-fifos.h"
#include "src/devices/power/drivers/fusb302/usb-pd-defs.h"
#include "src/devices/power/drivers/fusb302/usb-pd-message-type.h"
#include "src/devices/power/drivers/fusb302/usb-pd-message.h"

namespace fusb302 {

Fusb302Protocol::Fusb302Protocol(Fusb302Fifos& fifos)
    : fifos_(fifos),
      good_crc_template_(usb_pd::MessageType::kGoodCrc, /*data_object_count=*/0,
                         usb_pd::MessageId(0), usb_pd::PowerRole::kSink,
                         usb_pd::SpecRevision::kRev2, usb_pd::DataRole::kUpstreamFacingPort),
      next_transmitted_message_id_(0),
      next_expected_message_id_(0) {}

zx::result<> Fusb302Protocol::MarkMessageAsRead() {
  ZX_DEBUG_ASSERT(HasUnreadMessage());

  const usb_pd::MessageId read_message_id = received_message_queue_.front().header().message_id();
  received_message_queue_.pop();

  if (!good_crc_transmission_pending_) {
    // Hardware replied with GoodCRC.
    return zx::ok();
  }
  if (read_message_id != next_expected_message_id_) {
    // There is an unacknowledged message, but it's not this one.
    return zx::ok();
  }

  StampGoodCrcTemplate();
  usb_pd::Message good_crc(good_crc_template_, {});
  return fifos_.TransmitMessage(good_crc);
}

zx::result<> Fusb302Protocol::DrainReceiveFifo() {
  while (true) {
    zx::result<std::optional<usb_pd::Message>> result = fifos_.ReadReceivedMessage();
    if (result.is_error()) {
      return result.take_error();
    }

    if (!result.value().has_value()) {
      return zx::ok();
    }

    ProcessReceivedMessage(result.value().value());
  }
}

void Fusb302Protocol::ProcessReceivedMessage(const usb_pd::Message& message) {
  const usb_pd::Header& header = message.header();
  if (header.message_type() == usb_pd::MessageType::kGoodCrc) {
    // Discard repeated GoodCRCs.
    if (transmission_state_ != TransmissionState::kPending) {
      zxlogf(WARNING,
             "PD protocol de-synchronization: discarded GoodCRC with MessageID %" PRIu8
             ". No unacknowledged message.",
             static_cast<uint8_t>(header.message_id()));
      return;
    }

    if (header.message_id() != next_transmitted_message_id_) {
      zxlogf(WARNING,
             "PD protocol de-synchronization: discarded GoodCRC with MessageID %" PRIu8
             "; while waiting for a GoodCRC for MessageID is %" PRIu8,
             static_cast<uint8_t>(header.message_id()),
             static_cast<uint8_t>(next_transmitted_message_id_));
      return;
    }

    next_transmitted_message_id_ = next_transmitted_message_id_.Next();
    transmission_state_ = TransmissionState::kSuccess;
    return;
  }

  if (header.message_type() == usb_pd::MessageType::kSoftReset) {
    zxlogf(WARNING, "PD protocol de-synchronization: received Soft Reset with MessageID %" PRIu8,
           static_cast<uint8_t>(header.message_id()));

    // usbpd3.1 6.8.1 "Soft Reset and Protocol error" states that the MessageID
    // counter must be reset before sending the Soft Reset / Accept messages in
    // the soft reset sequence. This implies that Soft Reset messages must
    // always have a Message ID of zero.
    if (header.message_id() != usb_pd::MessageId(0)) {
      zxlogf(WARNING, "Received Soft Reset with non-zero Message ID %" PRIu8,
             static_cast<uint8_t>(header.message_id()));
    }

    // Both the Source and Sink sub-sections in usbpd3.1 8.3.3.4 "SOP Soft Reset
    // and Protocol Error State Diagrams" mandate that the sender of a Soft
    // Reset waits for an Accept before sending any other message.
    //
    // That being said, resetting PD protocol state here let us recognize the
    // MessageIDs of any messages coming our way from a non-compliant Port
    // partner.
    DidReceiveSoftReset();

    // Drop all messages received before the Soft Reset. It's too late to act on
    // them now, and we have to produce an Accept reply in 15ms / 30ms
    // (tSenderResponse / tReceiverResponse in usbpd3.1 6.6.2 "Sender Response
    // Timer").
    received_message_queue_.clear();

    received_message_queue_.push(message);
    return;
  }

  // Discard repeated messages.
  if (good_crc_transmission_pending_) {
    if (header.message_id() == next_expected_message_id_.Next()) {
      zxlogf(WARNING,
             "Received message with MessageID %" PRIu8
             " while expecting to have to send GoodCRC for Message ID %" PRIu8
             ". Fixing state, assuming GoodCRC was auto-generated.",
             static_cast<uint8_t>(header.message_id()),
             static_cast<uint8_t>(next_expected_message_id_));
      next_expected_message_id_ = header.message_id();
    } else {
      zxlogf(WARNING,
             "PD protocol de-synchronization: discarded message with MessageID %" PRIu8
             " because we still need to send GoodCRC for MessageID %" PRIu8,
             static_cast<uint8_t>(header.message_id()),
             static_cast<uint8_t>(next_expected_message_id_));
      return;
    }
  } else {
    if (header.message_id() != next_expected_message_id_) {
      zxlogf(WARNING,
             "PD re-transmission: discarded message with MessageID %" PRIu8
             " because next expected MessageID is %" PRIu8,
             static_cast<uint8_t>(header.message_id()),
             static_cast<uint8_t>(next_expected_message_id_));
      return;
    }
  }

  good_crc_transmission_pending_ = true;

  if (received_message_queue_.full()) {
    zxlogf(WARNING, "PD received message queue (size %" PRIu32 ") full! Dropping oldest message.",
           received_message_queue_.size());
    received_message_queue_.pop();
  }
  received_message_queue_.push(message);
}

zx::result<> Fusb302Protocol::Transmit(const usb_pd::Message& message) {
  ZX_DEBUG_ASSERT(message.header().message_type() != usb_pd::MessageType::kGoodCrc);
  ZX_DEBUG_ASSERT(transmission_state_ != TransmissionState::kPending);
  ZX_DEBUG_ASSERT(message.header().message_id() == next_transmitted_message_id_);

  zx::result<> result = fifos_.TransmitMessage(message);
  if (!result.is_ok()) {
    return result.take_error();
  }
  transmission_state_ = TransmissionState::kPending;
  return zx::ok();
}

void Fusb302Protocol::FullReset() {
  next_expected_message_id_.Reset();
  next_transmitted_message_id_.Reset();
  transmission_state_ = TransmissionState::kSuccess;
  good_crc_transmission_pending_ = false;
}

void Fusb302Protocol::DidReceiveSoftReset() {
  // usbpd3.1 6.8.1 "Soft Reset and Protocol error" states that the MessageID
  // counter must be reset before sending the Soft Reset / Accept messages in
  // the soft reset sequence. This implies that Soft Reset message we received
  // must have had a Message ID of zero.
  next_expected_message_id_ = usb_pd::MessageId(0);

  // Table 8-28 "Steps for a Soft Reset" in the USB PD spec states that the Soft
  // Reset message must be acknowledged via GoodCRC, just like any other
  // message. Table 8-28 is usbpd3.1 8.3.2.5 "Soft Reset" under usbpd3.1 8.3.2
  // "Atomic Message diagrams".
  //
  // We discard any previously pending GoodCRC when we receive a Soft Reset.
  // GoodCRC messages do flow control, and we're about to reset the entire
  // message flow.
  good_crc_transmission_pending_ = true;

  next_transmitted_message_id_.Reset();
  transmission_state_ = TransmissionState::kSuccess;
}

void Fusb302Protocol::DidTimeoutWaitingForGoodCrc() {
  if (transmission_state_ != TransmissionState::kPending) {
    zxlogf(WARNING,
           "Hardware PD layer reported GoodCRC timeout, but we weren't expecting any GoodCRC.");
    return;
  }
  transmission_state_ = TransmissionState::kTimedOut;
}

void Fusb302Protocol::DidTransmitGoodCrc() {
  if (good_crc_transmission_pending_) {
    // We will not be using the GoodCRC template, but stamping also performs all
    // GoodCRC-related state updates.
    StampGoodCrcTemplate();
  } else {
    zxlogf(WARNING,
           "Hardware PD layer reported transmitting a GoodCRC, but we didn't need to send one");
  }
}

void Fusb302Protocol::StampGoodCrcTemplate() {
  ZX_DEBUG_ASSERT(good_crc_transmission_pending_);

  good_crc_template_.set_message_id(next_expected_message_id_);
  next_expected_message_id_ = next_expected_message_id_.Next();
  good_crc_transmission_pending_ = false;
}

}  // namespace fusb302
