// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_POWER_DRIVERS_FUSB302_FUSB302_PROTOCOL_H_
#define SRC_DEVICES_POWER_DRIVERS_FUSB302_FUSB302_PROTOCOL_H_

// The comments in this file reference the USB Power Delivery Specification,
// downloadable at https://usb.org/document-library/usb-power-delivery
//
// usbpd3.1 is Revision 3.1, Version 1.7, published January 2023.

#include <fidl/fuchsia.hardware.i2c/cpp/wire.h>
#include <lib/zx/result.h>
#include <zircon/assert.h>

#include <cstdint>

#include <fbl/ring_buffer.h>

#include "src/devices/power/drivers/fusb302/fusb302-fifos.h"
#include "src/devices/power/drivers/fusb302/usb-pd-message.h"

namespace fusb302 {

// Tracks the acknowledgement of the last transmitted message.
enum class TransmissionState : uint8_t {
  // Waiting for GoodCRC on last transmitted message.
  //
  // The message may be re-transmitted, according to the PD spec. Transmit()
  // must not be called in this state.
  kPending,

  // Timed out waiting for GoodCRC on last transmitted message.
  kTimedOut,

  // Last transmitted message was received successfully.
  kSuccess,
};

// FUSB302-specific implementation of the USB PD Protocol Layer.
//
// The FUSB302 hardware can take on some aspects of the PD Protocol Layer. This
// class is responsible for a complete implementation, while delegating some
// parts to hardware.
class Fusb302Protocol {
 public:
  // `fifos` must remain alive throughout the new instance's lifetime.
  explicit Fusb302Protocol(Fusb302Fifos& fifos);

  Fusb302Protocol(const Fusb302Protocol&) = delete;
  Fusb302Protocol& operator=(const Fusb302Protocol&) = delete;

  // Trivially destructible.
  ~Fusb302Protocol() = default;

  // See `TransmissionState` member comments.
  TransmissionState transmission_state() const { return transmission_state_; }

  // True if the unread queue is not empty.
  //
  // All the messages in the unread queue must be processed and acknowledged via
  // `MarkMessageAsRead()` before `DrainReceiveFifo()` is called.
  bool HasUnreadMessage() const { return !received_message_queue_.empty(); }

  // Returns the first message in the unread messages queue.
  //
  // `HasUnreadMessage()` must be true.
  const usb_pd::Message& FirstUnreadMessage() {
    ZX_DEBUG_ASSERT(HasUnreadMessage());
    return received_message_queue_.front();
  }

  // Removes a message from the unread queue. Transmits a GoodCRC if neceesary.
  //
  // `HasUnreadMessage()` must be true.
  //
  // Returns an error if an I/O error occurred while transmitting a GoodCRC
  // acknowledging the read message.
  zx::result<> MarkMessageAsRead();

  // Not meaningful while `transmission_state()` is `kPending`.
  usb_pd::MessageId next_transmitted_message_id() const {
    ZX_DEBUG_ASSERT(transmission_state_ != TransmissionState::kPending);
    return next_transmitted_message_id_;
  }

  // Reads any PD messages that may be pending in the Rx (receive) FIFO.
  //
  // Returns an error if retrieving the PD message from the PHY layer encounters
  // an I/O error. Otherwise, performs PD Protocol Layer processing (mostly
  // MessageID validation), and updates (TBD: queue name).
  zx::result<> DrainReceiveFifo();

  // Transmits a PD message.
  //
  // `message` must not be a GoodCRC. MarkMessageAsRead() dispatches any
  // necessary GoodCRC message internally.
  //
  // `message`'s MessageID header field must equal
  // `next_transmitted_message_id()`.
  //
  // Must not be called while `transmission_status()` is `kPending`. This is
  // because PD messages (with the excepton of GoodCRC) form a synchronous
  // stream that is blocked on the other side's GoodCRC acknowledgements.
  zx::result<> Transmit(const usb_pd::Message& message);

  // The template will be used as-is (modulo MessageID) for GoodCRC messages.
  void SetGoodCrcTemplate(usb_pd::Header good_crc_template) {
    good_crc_template_ = good_crc_template;
  }

  // PD protocol layer reset.
  //
  // This method can be used when a new Type C connection is established, or
  // right before sending a Soft Reset message.
  //
  // `DidReceiveSoftReset()` must be called instead of this method when a Soft
  // Reset message is received, because that situation requires different
  // initial values for MessageID counters.
  void FullReset();

  // PD protocol layer reset, used after receiving a Soft Reset packet.
  //
  // This must only be used for a soft reset initiated by a port partner
  // message. Incorrect use will result in incorrect initial MessageID values,
  // which will break future communication.
  void DidReceiveSoftReset();

  // Hardware-side PD protocol layer says it gave up waiting for a GoodCRC.
  //
  // This signal comes from the interrupt unit.
  void DidTimeoutWaitingForGoodCrc();

  // Hardware-side PD protocol layer says it replied with a GoodCRC message.
  //
  // This signal comes from the interrupt unit.
  void DidTransmitGoodCrc();

 private:
  // Prepare `good_crc_template_` for transmission.
  //
  // The `good_crc_transmission_pending_` flag will be consumed. (Must be true,
  // will be set to false.)
  void StampGoodCrcTemplate();

  // Reads a PD message out of the Rx (receive) FIFO.
  //
  // Returns an error if retrieving the PD message from the PHY layer encounters
  // an I/O error. Otherwise, performs PD Protocol Layer processing (mostly
  // MessageID validation). The unread message queue will be updated if the
  // message is accepted by the protocol layer.
  void ProcessReceivedMessage(const usb_pd::Message& message);

  // Guaranteed to outlive this instance, because it's owned by this instance's
  // owner (Fusb302).
  Fusb302Fifos& fifos_;

  usb_pd::Header good_crc_template_;

  // Received messages that haven't been processed yet.
  //
  // With hardware-generated GoodCRC replies, it's possible to have at least 4
  // messages queued up: a GoodCRC for a Request, two replies (Accept, PS_RDY),
  // and a follow-up query such as Get_Sink_Capabilities.
  fbl::RingBuffer<usb_pd::Message, 8> received_message_queue_;

  // If `transmission_state` is `kPending`, this MessageID was used, and we're
  // waiting for a GoodCRC. Otherwise, this is MessageID will be used for the
  // next transmitted message.
  usb_pd::MessageId next_transmitted_message_id_;

  // If `good_crc_transmission_pending_` is true, we're waiting to send a GoodCRC.
  usb_pd::MessageId next_expected_message_id_;

  bool good_crc_transmission_pending_ = false;

  TransmissionState transmission_state_ = TransmissionState::kSuccess;
};

}  // namespace fusb302

#endif  // SRC_DEVICES_POWER_DRIVERS_FUSB302_FUSB302_PROTOCOL_H_
