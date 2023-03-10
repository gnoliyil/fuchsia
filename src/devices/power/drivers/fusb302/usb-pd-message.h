// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_POWER_DRIVERS_FUSB302_USB_PD_MESSAGE_H_
#define SRC_DEVICES_POWER_DRIVERS_FUSB302_USB_PD_MESSAGE_H_

// The comments in this file reference the USB Power Delivery Specification,
// downloadable at https://usb.org/document-library/usb-power-delivery
//
// usbpd3.1 is Revision 3.1, Version 1.7, published January 2023.

#include <lib/stdcompat/span.h>
#include <zircon/assert.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include <hwreg/bitfields.h>

#include "src/devices/power/drivers/fusb302/usb-pd-defs.h"
#include "src/devices/power/drivers/fusb302/usb-pd-message-type.h"

namespace usb_pd {

// Type-safe wrapper for USB message ID manipulation.
//
// For best readability, use the fully-qualified name `usb_pd::MessageId`.
//
// usbpd3.1 6.2.1.1.3 "Message ID"
class MessageId {
 public:
  // Default-initialized Message IDs start at zero.
  MessageId() = default;

  // Support explicit casting from uint8_t.
  explicit MessageId(uint8_t bits) : bits_(bits) { ZX_DEBUG_ASSERT(bits < 8); }

  // Value type, copying allowed.
  MessageId(const MessageId&) = default;
  MessageId& operator=(const MessageId&) = default;

  // Trivially destructible.
  ~MessageId() = default;

  // Support explicit casting to uint8_t.
  explicit operator uint8_t() const { return bits_; }

  // In C++20, equality comparison can be defaulted.
  bool operator==(const MessageId& other) const { return bits_ == other.bits_; }
  bool operator!=(const MessageId& other) const { return bits_ != other.bits_; }

  MessageId Next() const {
    // Rolling 3-bit counter.
    return MessageId((bits_ + 1) % 8);
  }

  void Reset() { bits_ = 0; }

 private:
  uint8_t bits_ = 0;
};

// Type-safe serialization and de-serialization of USB PD message headers.
//
// For best readability, use the fully-qualified name `usb_pd::Header`.
//
// usbpd3.1 6.2.1.1 "Message Header"
class Header {
 private:
  // Needed for the following to compile.
  uint16_t bits_;

  // Needed here for message_id() and set_message_id() to compile.
  DEF_SUBFIELD(bits_, 11, 9, message_id_bits);

  // Needed here for message_type() and set_message_type() to compile.
  DEF_SUBFIELD(bits_, 4, 0, message_type_bits);

 public:
  // True for extended PD messages. Not handled by this driver.
  //
  // usbpd3.1 6.2.1.1.1 "Extended"
  DEF_SUBBIT(bits_, 15, is_extended);

  // Message payload size, in 32-bit objects. Zero (0) for Control messages.
  //
  // usbpd3.1 6.2.1.1.3 "Message ID"
  DEF_SUBFIELD(bits_, 14, 12, data_object_count);

  // Message nonce generated by a 3-bit rolling counter.
  //
  // The nonces are unique per-Port. In other words, each side needs to track
  // separate rolling counters for transmitted and received messages.
  //
  // usbpd3.1 6.2.1.1.3 "Message ID"
  MessageId message_id() const { return static_cast<MessageId>(message_id_bits()); }
  Header& set_message_id(MessageId message_id) {
    return set_message_id_bits(static_cast<uint8_t>(message_id));
  }

  // The message sender's power role. Only valid for SOP messages.
  //
  // The USB PD spec description of this header field prohibits checking the
  // field in received messages. In particular, incorrect values don't lead to
  // Soft Reset or Hard Reset.
  //
  // usbpd3.1 6.2.1.1.4 "Port Power Role"
  DEF_ENUM_SUBFIELD(bits_, PowerRole, 8, 8, power_role);

  // The PD specification revision used by the message sender.
  //
  // Source_Capabilities messages have the maximum PD spec revision supported by
  // the Source. Request messages have the maximum PD spec revision supported by
  // both the Source and Sink.
  //
  // The USB PD spec mandates ignoring the revision field in GoodCRC messages.
  //
  // usbpd3.1 6.2.1.1.5 "Specification Revision"
  DEF_ENUM_SUBFIELD(bits_, SpecRevision, 7, 6, spec_revision);

  // The message sender's data role. Only valid for SOP messages.
  //
  // SOP' and SOP" messages use the underlying bit for the Cable Plug field
  // instead. (usbpd3.1 6.2.1.1.7)
  //
  // usbpd3.1 6.2.1.1.6 "Port Data Role"
  DEF_ENUM_SUBFIELD(bits_, DataRole, 5, 5, data_role);

  // Determines the message's type, together with `data_object_count`.
  //
  // usbpd3.1 6.2.1.1.8 "Message Type"
  MessageType message_type() const {
    if (data_object_count() != 0) {
      const DataMessageType type = static_cast<DataMessageType>(message_type_bits());
      return MessageTypeFromDataMessageType(type);
    }
    const ControlMessageType type = static_cast<ControlMessageType>(message_type_bits());
    return MessageTypeFromControlMessageType(type);
  }

  // `data_object_count()` must already be set to a valid value.
  Header& set_message_type(MessageType message_type) {
    ZX_DEBUG_ASSERT((data_object_count() != 0) == IsDataMessageType(message_type));

    // The IsDataMessageType() check and branches will be optimized away.
    return set_message_type_bits(
        IsDataMessageType(message_type)
            ? static_cast<uint8_t>(DataMessageTypeFromMessageType(message_type))
            : static_cast<uint8_t>(ControlMessageTypeFromMessageType(message_type)));
  }

  // The number of data objects is transmitted in a 3-bit field.
  static constexpr int kMaxDataObjectCount = 7;

  static Header CreateFromBytes(uint8_t byte1, uint8_t byte2) {
    uint16_t bits = static_cast<uint16_t>(byte1 | (byte2 << 8));
    return Header(bits);
  }

  Header(MessageType message_type, uint8_t data_object_count, MessageId message_id,
         PowerRole power_role, SpecRevision spec_revision, DataRole data_role) {
    set_data_object_count(data_object_count);

    // Must be called after `set_data_object_count()`.
    set_message_type(message_type);

    set_message_id(message_id);
    set_power_role(power_role);
    set_spec_revision(spec_revision);
    set_data_role(data_role);
    set_is_extended(false);
  }

  // Value type, copying is allowed.
  Header(const Header&) = default;
  Header& operator=(const Header&) = default;

  // Trivially destructible.
  ~Header() = default;

  // Expected usage: std::tie(byte1, byte2) = header.bytes();
  std::pair<uint8_t, uint8_t> bytes() const {
    return {static_cast<uint8_t>(bits_), static_cast<uint8_t>(bits_ >> 8)};
  }

  // The size of the message's payload, in bytes.
  int8_t payload_bytes() const {
    // The casts and multiplication will not overflow (causing UB) because
    // `data_object_count()` is a 3-bit field.
    return static_cast<int8_t>(static_cast<int8_t>(data_object_count()) * 4);
  }

  // The size of the message, including the header.
  int8_t message_bytes() const { return static_cast<int8_t>(2 + payload_bytes()); }

 private:
  explicit Header(uint16_t bits) : bits_(bits) {}
};

// Container for a USB PD message.
//
// For best readability, use the fully-qualified name `usb_pd::Message`.
class Message {
 public:
  // Message payloads consist of 32-bit data objects.
  static constexpr int kMaxPayloadBytes = Header::kMaxDataObjectCount * sizeof(uint32_t);

  // Messages have a 2-byte header and the payload.
  static constexpr int kMaxMessageBytes = kMaxPayloadBytes + 2;

  // Assembles a message from its serialized parts.
  Message(Header header, cpp20::span<const uint32_t> data_objects) : header_(header) {
    ZX_DEBUG_ASSERT(header.data_object_count() <= Header::kMaxDataObjectCount);
    ZX_DEBUG_ASSERT(header.data_object_count() == data_objects.size());

    std::copy(data_objects.begin(), data_objects.end(), data_objects_.begin());
  }

  Message(MessageType message_type, MessageId message_id, PowerRole power_role,
          SpecRevision spec_revision, DataRole data_role, cpp20::span<const uint32_t> data_objects)
      : header_(message_type, data_objects.size(), message_id, power_role, spec_revision,
                data_role) {
    std::copy(data_objects.begin(), data_objects.end(), data_objects_.begin());
  }

  // Value type, copying is allowed.
  Message(const Message& message) = default;
  Message& operator=(const Message& message) = default;

  // Trivially destructible.
  ~Message() = default;

  Header header() const { return header_; }

  Message& set_message_id(MessageId new_message_id) {
    header_.set_message_id(new_message_id);
    return *this;
  }

  // The returned span will be empty for control messages.
  cpp20::span<const uint32_t> data_objects() const {
    return cpp20::span(data_objects_.data(), header_.data_object_count());
  }

 private:
  Header header_;
  std::array<uint32_t, Header::kMaxDataObjectCount> data_objects_ = {};
};

}  // namespace usb_pd

#endif  // SRC_DEVICES_POWER_DRIVERS_FUSB302_USB_PD_MESSAGE_H_
