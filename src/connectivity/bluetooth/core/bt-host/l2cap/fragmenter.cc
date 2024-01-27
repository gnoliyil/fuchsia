// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fragmenter.h"

#include <endian.h>

#include <limits>
#include <optional>

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fcs.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/acl_data_packet.h"

namespace bt::l2cap {
namespace {

// ByteBuffer::Copy does not allow copying to a smaller destination for safety. This clamps the copy
// size to both the source size and the destination size.
size_t CopyBounded(MutableBufferView destination, const ByteBuffer& source) {
  const size_t size = std::min(destination.size(), source.size());
  source.Copy(&destination, 0, size);
  return size;
}

}  // namespace

OutboundFrame::OutboundFrame(ChannelId channel_id, const ByteBuffer& data,
                             FrameCheckSequenceOption fcs_option)
    : channel_id_(channel_id),
      data_(data.view()),
      fcs_option_(fcs_option),
      fcs_(include_fcs() ? std::optional(MakeFcs()) : std::nullopt) {}

size_t OutboundFrame::size() const {
  return sizeof(BasicHeader) + data_.size() + (include_fcs() ? sizeof(FrameCheckSequence) : 0);
}

void OutboundFrame::WriteToFragment(MutableBufferView fragment_payload, size_t offset) {
  // Build a table of the pages making up the frame's content, in sorted order.
  const StaticByteBuffer header_buffer = MakeBasicHeader();
  const std::optional fcs_buffer = include_fcs() ? std::optional(MakeFcs()) : std::nullopt;
  const BufferView footer_buffer = fcs_buffer ? fcs_buffer->view() : BufferView();
  const std::array pages = {header_buffer.view(), data_.view(), footer_buffer, BufferView()};
  const std::array offsets = {size_t{0}, header_buffer.size(), header_buffer.size() + data_.size(),
                              size()};
  static_assert(pages.size() == offsets.size());

  BT_ASSERT(offset <= size());
  size_t output_offset = 0;

  // Find the last page whose offset is not greater than the current offset.
  const auto page_iter = std::prev(std::upper_bound(offsets.begin(), offsets.end(), offset));
  for (size_t page_index = page_iter - offsets.begin(); page_index < pages.size(); page_index++) {
    if (fragment_payload.size() - output_offset == 0) {
      break;
    }
    const auto& page_buffer = pages[page_index];
    const size_t bytes_copied = CopyBounded(fragment_payload.mutable_view(output_offset),
                                            page_buffer.view(offset - offsets[page_index]));
    offset += bytes_copied;
    output_offset += bytes_copied;
  }
  BT_ASSERT(output_offset <= fragment_payload.size());
}

OutboundFrame::BasicHeaderBuffer OutboundFrame::MakeBasicHeader() const {
  // Length is "the length of the entire L2CAP PDU in octets, excluding the Length and CID field"
  // (v5.0 Vol 3, Part A, Section 3.3.1)
  const size_t pdu_content_length = size() - sizeof(BasicHeader);
  BT_ASSERT_MSG(pdu_content_length <= std::numeric_limits<decltype(BasicHeader::length)>::max(),
                "PDU payload is too large to be encoded");
  BasicHeader header = {};
  header.length = htole16(pdu_content_length);
  header.channel_id = htole16(channel_id_);
  BasicHeaderBuffer buffer;
  buffer.WriteObj(header);
  return buffer;
}

OutboundFrame::FrameCheckSequenceBuffer OutboundFrame::MakeFcs() const {
  BT_ASSERT(include_fcs());
  const BasicHeaderBuffer header = MakeBasicHeader();
  const FrameCheckSequence header_fcs = l2cap::ComputeFcs(header.view());
  const FrameCheckSequence whole_fcs = l2cap::ComputeFcs(data_.view(), header_fcs);
  FrameCheckSequenceBuffer buffer;
  buffer.WriteObj(htole16(whole_fcs.fcs));
  return buffer;
}

Fragmenter::Fragmenter(hci_spec::ConnectionHandle connection_handle, uint16_t max_acl_payload_size)
    : connection_handle_(connection_handle), max_acl_payload_size_(max_acl_payload_size) {
  BT_ASSERT(connection_handle_ <= hci_spec::kConnectionHandleMax);
  BT_ASSERT(max_acl_payload_size_);
  BT_ASSERT(max_acl_payload_size_ >= sizeof(BasicHeader));
}

// NOTE(armansito): The following method copies the contents of |data| into ACL
// data packets. This copying is currently necessary because the complete HCI
// frame (ACL header + payload fragment) we send over the channel to the bt-hci
// driver need to be stored contiguously before the call to zx_channel_write.
// Plus, we perform the HCI flow-control on the host-stack side which requires
// ACL packets to be buffered.
//
// As our future driver architecture will remove the IPC between the HCI driver
// and the host stack, our new interface could support scatter-gather for the
// header and the payload. Then, the bt-hci driver could read the payload
// fragment directly out of |data| and we would only construct the headers,
// removing the extra copy.
//
// * Current theoretical number of data copies:
//     1. service -> L2CAP channel
//     2. channel -> fragmenter ->(move) HCI layer
//     3. HCI layer ->(zx_channel_write)
//     4. (zx_channel_read)-> bt-hci driver
//     5. bt-hci driver -> transport driver
//
// * Potential number of data copies
//     1. service -> L2CAP channel
//     2. channel -> fragmenter ->(move) HCI layer ->(move) bt-hci driver
//     if buffering is needed:
//       3. bt-hci driver -> transport driver
PDU Fragmenter::BuildFrame(ChannelId channel_id, const ByteBuffer& data,
                           FrameCheckSequenceOption fcs_option, bool flushable) const {
  BT_DEBUG_ASSERT(data.size() <= kMaxBasicFramePayloadSize);
  BT_DEBUG_ASSERT(channel_id);

  OutboundFrame frame(channel_id, data, fcs_option);
  const size_t frame_size = frame.size();
  const size_t num_fragments =
      frame_size / max_acl_payload_size_ + (frame_size % max_acl_payload_size_ ? 1 : 0);

  PDU pdu;
  size_t processed = 0;
  for (size_t i = 0; i < num_fragments; i++) {
    BT_DEBUG_ASSERT(frame_size > processed);

    const size_t fragment_size =
        std::min(frame_size - processed, static_cast<size_t>(max_acl_payload_size_));
    auto pbf = (i ? hci_spec::ACLPacketBoundaryFlag::kContinuingFragment
                  : (flushable ? hci_spec::ACLPacketBoundaryFlag::kFirstFlushable
                               : hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable));

    // TODO(armansito): allow passing Active Peripheral Broadcast flag when we support it.
    auto acl_packet = hci::ACLDataPacket::New(
        connection_handle_, pbf, hci_spec::ACLBroadcastFlag::kPointToPoint, fragment_size);
    BT_DEBUG_ASSERT(acl_packet);

    frame.WriteToFragment(acl_packet->mutable_view()->mutable_payload_data(), processed);
    processed += fragment_size;

    pdu.AppendFragment(std::move(acl_packet));
  }

  // The PDU should have been completely processed if we got here.
  BT_DEBUG_ASSERT(processed == frame_size);

  return pdu;
}

}  // namespace bt::l2cap
