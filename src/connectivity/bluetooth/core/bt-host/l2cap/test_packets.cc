// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test_packets.h"

#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fcs.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/frame_headers.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_helpers.h"

namespace bt::l2cap::testing {

DynamicByteBuffer AclExtFeaturesInfoReq(l2cap::CommandId id, hci_spec::ConnectionHandle handle) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle, length: 10)
      LowerBits(handle), UpperBits(handle), 0x0a, 0x00,

      // L2CAP B-frame header (length: 6, chanel-id: 0x0001 (ACL sig))
      0x06, 0x00, 0x01, 0x00,

      // Extended Features Information Request
      // (ID, length: 2, type)
      0x0a, id, 0x02, 0x00,
      LowerBits(static_cast<uint16_t>(InformationType::kExtendedFeaturesSupported)),
      UpperBits(static_cast<uint16_t>(InformationType::kExtendedFeaturesSupported))));
}

DynamicByteBuffer AclCommandRejectNotUnderstoodRsp(l2cap::CommandId id,
                                                   hci_spec::ConnectionHandle handle,
                                                   ChannelId chan_id) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link_handle|, length: 10 bytes)
      LowerBits(handle), UpperBits(handle), 0x0a, 0x00,
      // L2CAP B-frame header (length: 6 bytes, channel-id: 0x0001 (ACL sig))
      0x06, 0x00, LowerBits(chan_id), UpperBits(chan_id),
      // Information Response (type, ID, length: 2)
      l2cap::kCommandRejectCode, id, 0x02, 0x00,
      // Reason = Not Understood
      LowerBits(static_cast<uint16_t>(RejectReason::kNotUnderstood)),
      UpperBits(static_cast<uint16_t>(RejectReason::kNotUnderstood))));
}

DynamicByteBuffer AclExtFeaturesInfoRsp(l2cap::CommandId id, hci_spec::ConnectionHandle handle,
                                        l2cap::ExtendedFeatures features) {
  const auto features_bytes = ToBytes(features);
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link_handle|, length: 16 bytes)
      LowerBits(handle), UpperBits(handle), 0x10, 0x00,
      // L2CAP B-frame header (length: 12 bytes, channel-id: 0x0001 (ACL sig))
      0x0c, 0x00, 0x01, 0x00,
      // Information Response (type, ID, length: 8)
      l2cap::kInformationResponse, id, 0x08, 0x00,
      // Type = Features Mask
      0x02, 0x00,
      // Result = Success
      0x00, 0x00,
      // Data (Mask)
      features_bytes[0], features_bytes[1], features_bytes[2], features_bytes[3]));
}

DynamicByteBuffer AclFixedChannelsSupportedInfoReq(l2cap::CommandId id,
                                                   hci_spec::ConnectionHandle handle) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle, length: 10)
      LowerBits(handle), UpperBits(handle), 0x0a, 0x00,

      // L2CAP B-frame header (length: 6, chanel-id: 0x0001 (ACL sig))
      0x06, 0x00, 0x01, 0x00,

      // Fixed Channels Supported Information Request
      // (ID, length: 2, info type)
      l2cap::kInformationRequest, id, 0x02, 0x00,
      LowerBits(static_cast<uint16_t>(InformationType::kFixedChannelsSupported)),
      UpperBits(static_cast<uint16_t>(InformationType::kFixedChannelsSupported))));
}

DynamicByteBuffer AclFixedChannelsSupportedInfoRsp(l2cap::CommandId id,
                                                   hci_spec::ConnectionHandle handle,
                                                   l2cap::FixedChannelsSupported chan_mask) {
  const auto chan_bytes = ToBytes(chan_mask);
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link_handle|, length: 20 bytes)
      LowerBits(handle), UpperBits(handle), 0x14, 0x00,
      // L2CAP B-frame header (length: 16 bytes, channel-id: 0x0001 (ACL sig))
      0x10, 0x00, 0x01, 0x00,
      // Information Response (type, ID, length: 12)
      l2cap::kInformationResponse, id, 0x0c, 0x00,
      // Type = Fixed Channels Supported
      0x03, 0x00,
      // Result = Success
      0x00, 0x00,
      // Data (Mask)
      chan_bytes[0], chan_bytes[1], chan_bytes[2], chan_bytes[3], chan_bytes[4], chan_bytes[5],
      chan_bytes[6], chan_bytes[7]));
}

DynamicByteBuffer AclNotSupportedInformationResponse(l2cap::CommandId id,
                                                     hci_spec::ConnectionHandle handle) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link_handle|, length: 12 bytes)
      LowerBits(handle), UpperBits(handle), 0x0c, 0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,
      // Information Response (type, ID, length: 4)
      l2cap::kInformationResponse, id, 0x04, 0x00,
      // Type = invalid type
      0xFF, 0xFF,
      // Result
      LowerBits(static_cast<uint16_t>(l2cap::InformationResult::kNotSupported)),
      UpperBits(static_cast<uint16_t>(l2cap::InformationResult::kNotSupported))));
}

DynamicByteBuffer AclConfigReq(l2cap::CommandId id, hci_spec::ConnectionHandle handle,
                               l2cap::ChannelId dst_id, l2cap::ChannelParameters params) {
  const auto mode = params.mode.value_or(l2cap::ChannelMode::kBasic);
  const auto mtu = params.max_rx_sdu_size.value_or(l2cap::kMaxMTU);

  switch (mode) {
    case l2cap::ChannelMode::kBasic:
      return DynamicByteBuffer(StaticByteBuffer(
          // ACL data header (handle, length: 16 bytes)
          LowerBits(handle), UpperBits(handle), 0x10, 0x00,
          // L2CAP B-frame header (length: 12, channel-id: 0x0001 (ACL sig))
          0x0c, 0x00, 0x01, 0x00,
          // Configuration Request (ID, length: 8, |dst_id|, flags: 0,
          0x04, id, 0x08, 0x00, LowerBits(dst_id), UpperBits(dst_id), 0x00, 0x00,
          // MTU option: (ID: 1, length: 2, mtu)
          0x01, 0x02, LowerBits(mtu), UpperBits(mtu)));
    case l2cap::ChannelMode::kEnhancedRetransmission:
      return DynamicByteBuffer(StaticByteBuffer(
          // ACL data header (handle, length: 27 bytes)
          LowerBits(handle), UpperBits(handle), 0x1b, 0x00,
          // L2CAP B-frame header (length: 23, channel-id: 0x0001 (ACL sig))
          0x17, 0x00, 0x01, 0x00,
          // Configuration Request (ID, length: 19, |dst_id|, flags: 0,
          0x04, id, 0x13, 0x00, LowerBits(dst_id), UpperBits(dst_id), 0x00, 0x00,
          // MTU option: (ID: 1, length: 2, mtu)
          0x01, 0x02, LowerBits(mtu), UpperBits(mtu),
          // Retransmission & Flow Control option (Type, Length = 9, mode, fields)
          0x04, 0x09, static_cast<uint8_t>(mode), l2cap::kErtmMaxUnackedInboundFrames,
          l2cap::kErtmMaxInboundRetransmissions, 0x00, 0x00, 0x00, 0x00,
          LowerBits(l2cap::kMaxInboundPduPayloadSize),
          UpperBits(l2cap::kMaxInboundPduPayloadSize)));
    default:
      BT_ASSERT_MSG(false, "unsupported mode");
  }
}

DynamicByteBuffer AclConfigRsp(l2cap::CommandId id, hci_spec::ConnectionHandle link_handle,
                               l2cap::ChannelId src_id, l2cap::ChannelParameters params) {
  const auto mode = params.mode.value_or(l2cap::ChannelMode::kBasic);
  const auto mtu = params.max_rx_sdu_size.value_or(l2cap::kMaxMTU);

  switch (mode) {
    case l2cap::ChannelMode::kBasic:
      return DynamicByteBuffer(StaticByteBuffer(
          // ACL data header (handle: |link_handle|, length: 18 bytes)
          LowerBits(link_handle), UpperBits(link_handle), 0x12, 0x00,
          // L2CAP B-frame header (length: 14 bytes, channel-id: 0x0001 (ACL sig))
          0x0e, 0x00, 0x01, 0x00,
          // Configuration Response (ID, length: 10, src cid, flags: 0,
          // result: success)
          0x05, id, 0x0a, 0x00, LowerBits(src_id), UpperBits(src_id), 0x00, 0x00, 0x00, 0x00,
          // MTU option: (ID: 1, length: 2, mtu)
          0x01, 0x02, LowerBits(mtu), UpperBits(mtu)));
    case l2cap::ChannelMode::kEnhancedRetransmission: {
      const auto rtx_timeout = kErtmReceiverReadyPollTimerDuration.to_msecs();
      const auto monitor_timeout = kErtmMonitorTimerDuration.to_msecs();
      return DynamicByteBuffer(StaticByteBuffer(
          // ACL data header (handle: |link_handle|, length: 29 bytes)
          LowerBits(link_handle), UpperBits(link_handle), 0x1d, 0x00,
          // L2CAP B-frame header (length: 25 bytes, channel-id: 0x0001 (ACL sig))
          0x19, 0x00, 0x01, 0x00,
          // Configuration Response (ID, length: 21, src cid, flags: 0,
          // result: success)
          0x05, id, 0x15, 0x00, LowerBits(src_id), UpperBits(src_id), 0x00, 0x00, 0x00, 0x00,
          // MTU option: (ID: 1, length: 2, mtu)
          0x01, 0x02, LowerBits(mtu), UpperBits(mtu),
          // Retransmission & Flow Control option (Type, Length = 9, mode, fields)
          0x04, 0x09, static_cast<uint8_t>(mode), l2cap::kErtmMaxUnackedInboundFrames,
          l2cap::kErtmMaxInboundRetransmissions, LowerBits(rtx_timeout), UpperBits(rtx_timeout),
          LowerBits(monitor_timeout), UpperBits(monitor_timeout),
          LowerBits(l2cap::kMaxInboundPduPayloadSize),
          UpperBits(l2cap::kMaxInboundPduPayloadSize)));
    }
    default:
      BT_ASSERT_MSG(false, "unsupported mode");
  }
}

DynamicByteBuffer AclConnectionReq(l2cap::CommandId id, hci_spec::ConnectionHandle link_handle,
                                   l2cap::ChannelId src_id, l2cap::PSM psm) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link_handle|, length: 12 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x0c, 0x00,

      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,

      // Connection Request (ID, length: 4, |psm|, |src_id|)
      0x02, id, 0x04, 0x00, LowerBits(psm), UpperBits(psm), LowerBits(src_id), UpperBits(src_id)));
}

DynamicByteBuffer AclConnectionRsp(l2cap::CommandId id, hci_spec::ConnectionHandle link_handle,
                                   l2cap::ChannelId src_id, l2cap::ChannelId dst_id,
                                   l2cap::ConnectionResult result) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link handle|, length: 16 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x10, 0x00,
      // L2CAP B-frame header: length 12, channel-id 1 (signaling)
      0x0c, 0x00, 0x01, 0x00,
      // Connection Response (0x03), id, length 8
      l2cap::kConnectionResponse, id, 0x08, 0x00,
      // destination cid
      LowerBits(dst_id), UpperBits(dst_id),
      // source cid
      LowerBits(src_id), UpperBits(src_id),
      // Result
      LowerBits(static_cast<uint16_t>(result)), UpperBits(static_cast<uint16_t>(result)),
      // Status (no further information available)
      0x00, 0x00));
}

DynamicByteBuffer AclDisconnectionReq(l2cap::CommandId id, hci_spec::ConnectionHandle link_handle,
                                      l2cap::ChannelId src_id, l2cap::ChannelId dst_id) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link handle|, length: 12 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x0c, 0x00,
      // L2CAP B-frame header: length 8, channel-id 1 (signaling)
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Request, id, length 4
      l2cap::kDisconnectionRequest, id, 0x04, 0x00,
      // Destination CID
      LowerBits(dst_id), UpperBits(dst_id),
      // Source CID
      LowerBits(src_id), UpperBits(src_id)));
}

DynamicByteBuffer AclDisconnectionRsp(l2cap::CommandId id, hci_spec::ConnectionHandle link_handle,
                                      l2cap::ChannelId src_id, l2cap::ChannelId dst_id) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link handle|, length: 12 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x0c, 0x00,
      // L2CAP B-frame header: length 8, channel-id 1 (signaling)
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Response, id, length 4
      l2cap::kDisconnectionResponse, id, 0x04, 0x00,
      // Destination CID
      LowerBits(dst_id), UpperBits(dst_id),
      // Source CID
      LowerBits(src_id), UpperBits(src_id)));
}

DynamicByteBuffer AclConnectionParameterUpdateReq(l2cap::CommandId id,
                                                  hci_spec::ConnectionHandle link_handle,
                                                  uint16_t interval_min, uint16_t interval_max,
                                                  uint16_t peripheral_latency,
                                                  uint16_t timeout_multiplier) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link handle|, length: 16 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x10, 0x00,
      // L2CAP B-frame header: length 12, channel-id 5 (LE signaling)
      0x0c, 0x00, 0x05, 0x00,
      // Connection Parameter Update Request (0x12), id, length 8
      l2cap::kConnectionParameterUpdateRequest, id, 0x08, 0x00,
      // interval min
      LowerBits(interval_min), UpperBits(interval_min),
      // interval max
      LowerBits(interval_max), UpperBits(interval_max),
      // peripheral latency
      LowerBits(peripheral_latency), UpperBits(peripheral_latency),
      // timeout multiplier
      LowerBits(timeout_multiplier), UpperBits(timeout_multiplier)));
}

DynamicByteBuffer AclConnectionParameterUpdateRsp(l2cap::CommandId id,
                                                  hci_spec::ConnectionHandle link_handle,
                                                  ConnectionParameterUpdateResult result) {
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: |link handle|, length: 10 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x0a, 0x00,
      // L2CAP B-frame header: length 6, channel-id 5 (LE signaling)
      0x06, 0x00, 0x05, 0x00,
      // Connection Parameter Update Response (0x13), id, length 2
      l2cap::kConnectionParameterUpdateResponse, id, 0x02, 0x00,
      // Result
      LowerBits(static_cast<uint16_t>(result)), UpperBits(static_cast<uint16_t>(result))));
}

DynamicByteBuffer AclSFrame(hci_spec::ConnectionHandle link_handle, l2cap::ChannelId channel_id,
                            l2cap::internal::SupervisoryFunction function, uint8_t receive_seq_num,
                            bool is_poll_request, bool is_poll_response) {
  StaticByteBuffer acl_packet{
      // ACL data header (handle: |link handle|, length: 8 bytes)
      LowerBits(link_handle), UpperBits(link_handle), 0x08, 0x00,

      // L2CAP B-frame header: length 4, channel-id
      0x04, 0x00, LowerBits(channel_id), UpperBits(channel_id),

      // Enhanced Control Field: F is_poll_response, P is_poll_request, Supervisory function,
      // Type S-Frame, ReqSeq receive_seq_num
      (is_poll_response ? 0b1000'0000 : 0) | (is_poll_request ? 0b1'0000 : 0) |
          (static_cast<uint8_t>(function) << 2) | 0b1,
      receive_seq_num & 0b11'1111,

      // Frame Check Sequence
      0x00, 0x00};
  const FrameCheckSequence fcs = ComputeFcs(acl_packet.view(
      sizeof(hci_spec::ACLDataHeader),
      acl_packet.size() - sizeof(hci_spec::ACLDataHeader) - sizeof(FrameCheckSequence)));
  acl_packet[acl_packet.size() - 2] = LowerBits(fcs.fcs);
  acl_packet[acl_packet.size() - 1] = UpperBits(fcs.fcs);
  return DynamicByteBuffer(acl_packet);
}

DynamicByteBuffer AclIFrame(hci_spec::ConnectionHandle link_handle, l2cap::ChannelId channel_id,
                            uint8_t receive_seq_num, uint8_t tx_seq, bool is_poll_response,
                            const ByteBuffer& payload) {
  const uint16_t l2cap_size = static_cast<uint16_t>(sizeof(internal::SimpleInformationFrameHeader) +
                                                    payload.size() + sizeof(FrameCheckSequence));
  const uint16_t acl_size = l2cap_size + sizeof(BasicHeader);
  StaticByteBuffer headers(
      // ACL data header (handle: |link handle|, length)
      LowerBits(link_handle), UpperBits(link_handle), LowerBits(acl_size), UpperBits(acl_size),

      // L2CAP B-frame header: length, channel-id
      LowerBits(l2cap_size), UpperBits(l2cap_size), LowerBits(channel_id), UpperBits(channel_id),

      // Enhanced Control Field: F is_poll_response, TxSeq tx_seq, Type I-Frame,
      // ReqSeq receive_seq_num
      (is_poll_response ? 0b1000'0000 : 0) | ((tx_seq << 1) & 0b111'1110),
      receive_seq_num & 0b11'1111);

  FrameCheckSequence fcs = ComputeFcs(headers.view(sizeof(hci_spec::ACLDataHeader), acl_size));
  fcs = ComputeFcs(payload.view(), fcs);

  DynamicByteBuffer acl_packet(headers.size() + payload.size() + sizeof(fcs));
  headers.Copy(&acl_packet);
  auto payload_destination = acl_packet.mutable_view(headers.size());
  payload.Copy(&payload_destination);
  acl_packet[acl_packet.size() - 2] = LowerBits(fcs.fcs);
  acl_packet[acl_packet.size() - 1] = UpperBits(fcs.fcs);
  return acl_packet;
}

}  // namespace bt::l2cap::testing
