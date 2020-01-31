// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_ENHANCED_RETRANSMISSION_MODE_RX_ENGINE_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_ENHANCED_RETRANSMISSION_MODE_RX_ENGINE_H_

#include <variant>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/frame_headers.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/rx_engine.h"

namespace bt {
namespace l2cap {
namespace internal {

// Implements the receiver state and logic for an L2CAP channel operating in
// Enhanced Retransmission Mode.
//
// THREAD-SAFETY: This class is not thread-safe.
class EnhancedRetransmissionModeRxEngine final : public RxEngine {
 public:
  using SendFrameCallback = fit::function<void(ByteBufferPtr pdu)>;

  EnhancedRetransmissionModeRxEngine(SendFrameCallback send_frame_callback);
  virtual ~EnhancedRetransmissionModeRxEngine() = default;

  ByteBufferPtr ProcessPdu(PDU) override;

  // Set a callback to be invoked when any frame is received that indicates the peer's
  // acknowledgment for the sequence of packets that it received from the local host. The values are
  // not checked against the local sender's TxWindow. |is_poll_response| reflects the 'F' bit in the
  // header of the received frame.
  using ReceiveSeqNumCallback = fit::function<void(uint8_t receive_seq_num, bool is_poll_response)>;
  void set_receive_seq_num_callback(ReceiveSeqNumCallback receive_seq_num_callback) {
    receive_seq_num_callback_ = std::move(receive_seq_num_callback);
  }

  // Set a callback to be invoked that reports our acknowledgment of inbound frames from the peer.
  // |ack_seq_num| is the TxSeq of the next I-frame we expect from the peer.
  using AckSeqNumCallback = fit::function<void(uint8_t ack_seq_num)>;
  void set_ack_seq_num_callback(AckSeqNumCallback ack_seq_num_callback) {
    ack_seq_num_callback_ = std::move(ack_seq_num_callback);
  }

 private:
  ByteBufferPtr ProcessFrame(const SimpleInformationFrameHeader, PDU);
  ByteBufferPtr ProcessFrame(const SimpleStartOfSduFrameHeader, PDU);
  ByteBufferPtr ProcessFrame(const SimpleSupervisoryFrame, PDU);
  ByteBufferPtr ProcessFrame(std::monostate, PDU);
  void AdvanceSeqNum();

  // We assume that the Extended Window Size option is _not_ enabled. In such
  // cases, the sequence number is a 6-bit counter that wraps on overflow. See
  // Core Spec Ver 5, Vol 3, Part A, Secs 5.7 and 8.3.
  uint8_t next_seqnum_;  // (AKA Expected-TxSeq)

  SendFrameCallback send_frame_callback_;

  ReceiveSeqNumCallback receive_seq_num_callback_;
  AckSeqNumCallback ack_seq_num_callback_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(EnhancedRetransmissionModeRxEngine);
};

}  // namespace internal
}  // namespace l2cap
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_ENHANCED_RETRANSMISSION_MODE_RX_ENGINE_H_
