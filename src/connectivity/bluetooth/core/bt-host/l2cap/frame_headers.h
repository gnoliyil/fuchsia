// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_FRAME_HEADERS_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_FRAME_HEADERS_H_

#include <endian.h>

#include <cstdint>
#include <type_traits>

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"

namespace bt::l2cap::internal {

// The definitions within this namespace don't directly map to full frame
// formats. Rather, they provide access to mode-specific headers beyond the
// L2CAP basic frame header.
//
// The structs can be used in two ways: to parse inbound data, or to format
// outbound data. When used to parse inbound data, the structs due not provide
// any validation. When used to format outbound data, the structs initialize
// bits to 0, or (if appropriate) a value appropriate for that struct.

// For Retransmission and Flow Control Modes. (Vol 3, Part A, Sec 3.3.2)
using StandardControlField = uint16_t;

// See Vol 3, Part A, Sec 3.3.2, Table 3.4.
enum class SegmentationStatus {
  Unsegmented = 0b00,
  FirstSegment = 0b01,  // AKA "Start of L2CAP SDU"
  LastSegment = 0b10,   // AKA "End of L2CAP SDU"
  MiddleSegment = 0b11  // AKA "Continuation of L2CAP SDU"
};

// For Enhanced Retransmission and Streaming Modes _without_ Extended Window
// Size. (Vol 3, Part A, Sec 3.3.2)
struct EnhancedControlField {
  // See Core Spec v5, Vol 3, Part A, Sec 8.3.
  static constexpr auto kMaxSeqNum{63};

  EnhancedControlField() : raw_value(0) {}

  bool designates_information_frame() const { return !(le16toh(raw_value) & 0b1); }
  bool designates_supervisory_frame() const { return le16toh(raw_value) & 0x1; }
  bool designates_start_of_segmented_sdu() const {
    return designates_information_frame() && ((le16toh(raw_value) & (0b11 << 14)) == (0b01 << 14));
  }
  // Returns true for all segmented frames, including the start-of-segment frame
  // (even though the start-of-segment frame has a different header format).
  bool designates_part_of_segmented_sdu() const {
    return designates_information_frame() && (le16toh(raw_value) & (0b11 << 14));
  }

  void set_supervisory_frame() {
    // See Vol 3, Part A, Section 3.3.2, Table 3.2.
    raw_value = htole16(le16toh(raw_value) | 0x1);
  }

  uint8_t receive_seq_num() const {
    // "Receive Sequence Number - ReqSeq" Vol 3, Part A, Section 3.3.2, Table 3.2.
    return (le16toh(raw_value) >> 8) & 0b11'1111;
  }

  bool is_poll_response() const {
    // See Vol 3, Part A, Sec 3.3.2, Table 3.2. The spec calls this the 'final' bit. But poll
    // response seems more intuitive.
    return le16toh(raw_value) & 0b1000'0000;
  }

  void set_receive_seq_num(uint8_t seq_num) {
    BT_DEBUG_ASSERT(seq_num <= kMaxSeqNum);
    // "Receive Sequence Number - ReqSeq" Vol 3, Part A, Section 3.3.2, Table 3.2.
    raw_value = htole16(le16toh(raw_value) | (seq_num << 8));
  }

  void set_is_poll_response() {
    // See Vol 3, Part A, Sec 3.3.2, Table 3.2. The spec calls this the 'final' bit. But poll
    // response seems more intuitive.
    raw_value = htole16(le16toh(raw_value) | 0b1000'0000);
  }

  void set_segmentation_status(SegmentationStatus status) {
    // "Segmentation and Reassembly - SAR" Vol 3, Part A, Section 3.3.2, Table 3.4.
    raw_value = htole16((le16toh(raw_value) & 0b0011'1111'1111'1111) |
                        (static_cast<uint8_t>(status) << 14));
  }

 protected:
  uint16_t raw_value;  // In protocol byte-order (little-endian).
} __attribute__((packed));

// For Enhanced Retransmission and Streaming Modes _with_ Extended Window
// Size. (Vol 3, Part A, Secs 3.3.2 and 5.7. Feature 2/39.)
using ExtendedControlField = uint32_t;

// Represents an I-frame header for:
// * a channel operating in Enhanced Retransmission or
//   Streaming Mode, where
// * the Extended Window Size and Frame Checksum options are
//   disabled, and
// * the frame is _not_ a "Start of L2CAP SDU" frame.
// Omits the Basic L2CAP header. See Vol 3, Part A, Sec 3.3.
struct SimpleInformationFrameHeader : public EnhancedControlField {
  SimpleInformationFrameHeader() = default;

  explicit SimpleInformationFrameHeader(uint8_t tx_seq) {
    BT_DEBUG_ASSERT(tx_seq <= kMaxSeqNum);
    raw_value = htole16(le16toh(raw_value) | (tx_seq << 1));
  }

  uint8_t tx_seq() const {
    BT_DEBUG_ASSERT(!designates_supervisory_frame());
    return (le16toh(raw_value) & (0b0111'1110)) >> 1;
  }
} __attribute__((packed));

// Represents an I-frame header for:
// * a channel operating in Enhanced Retransmission or
//   Streaming Mode, where
// * the Extended Window Size and Frame Checksum options are
//   disabled, and
// * the frame _is_ a "Start of L2CAP SDU" frame.
// Omits the Basic L2CAP header. See Vol 3, Part A, Sec 3.3.
struct SimpleStartOfSduFrameHeader : public SimpleInformationFrameHeader {
  SimpleStartOfSduFrameHeader() = default;

  explicit SimpleStartOfSduFrameHeader(uint8_t tx_seq)
      : SimpleInformationFrameHeader(tx_seq), sdu_len(0) {
    set_segmentation_status(SegmentationStatus::FirstSegment);
  }
  uint16_t sdu_len;
} __attribute__((packed));

// See Vol 3, Part A, Sec 3.3.2, Table 3.5.
enum class SupervisoryFunction {
  ReceiverReady = 0,
  Reject = 1,
  ReceiverNotReady = 2,
  SelectiveReject = 3
};

// Represents an S-frame for:
// * a channel operating in Enhanced Retransmission or
//   Streaming Mode, where
// * the Extended Window Size and Frame Checksum options are
//   disabled
// Omits the Basic L2CAP header. See Vol 3, Part A, Sec 3.3.
struct SimpleSupervisoryFrame : public EnhancedControlField {
  SimpleSupervisoryFrame() = default;

  explicit SimpleSupervisoryFrame(SupervisoryFunction sfunc) {
    BT_DEBUG_ASSERT(sfunc <= SupervisoryFunction::SelectiveReject);
    set_supervisory_frame();
    // See Vol 3, Part A, Sec 3.3.2, Table 3.2.
    raw_value = htole16(le16toh(raw_value) | (static_cast<uint8_t>(sfunc) << 2));
  }

  bool is_poll_request() const {
    return le16toh(raw_value) & 0b1'0000;  // See Vol 3, Part A, Sec 3.3.2, Table 3.2.
  }

  SupervisoryFunction function() const {
    // See Vol 3, Part A, Sec 3.3.2, Table 3.2.
    return static_cast<SupervisoryFunction>((le16toh(raw_value) >> 2) & 0b11);
  }

  void set_is_poll_request() {
    // See Vol 3, Part A, Sec 3.3.2, Table 3.2.
    raw_value = htole16(le16toh(raw_value) | 0b1'0000);
  }
} __attribute__((packed));

struct SimpleReceiverReadyFrame : public SimpleSupervisoryFrame {
  SimpleReceiverReadyFrame() : SimpleSupervisoryFrame(SupervisoryFunction::ReceiverReady) {}
} __attribute__((packed));

}  // namespace bt::l2cap::internal

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_FRAME_HEADERS_H_
