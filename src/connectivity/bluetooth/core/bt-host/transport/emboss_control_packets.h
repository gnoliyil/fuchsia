// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_EMBOSS_CONTROL_PACKETS_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_EMBOSS_CONTROL_PACKETS_H_

#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/emboss_packet.h"

#include <src/connectivity/bluetooth/core/bt-host/hci-spec/hci-protocol.emb.h>

namespace bt::hci {

template <class ViewT>
class EmbossCommandPacketT;

// EmbossCommandPacket is the HCI Command packet specialization of DynamicPacket.
class EmbossCommandPacket : public DynamicPacket {
 public:
  // Construct an HCI Command packet from an Emboss view T and initialize its header with the
  // |opcode| and size.
  template <typename T>
  static EmbossCommandPacketT<T> New(hci_spec::OpCode opcode) {
    return New<T>(opcode, T::IntrinsicSizeInBytes().Read());
  }

  // Construct an HCI Command packet from an Emboss view T of |packet_size| total bytes (header +
  // payload) and initialize its header with the |opcode| and size. This constructor is meant for
  // variable size packets, for which clients must calculate packet size manually.
  template <typename T>
  static EmbossCommandPacketT<T> New(hci_spec::OpCode opcode, size_t packet_size) {
    EmbossCommandPacketT<T> packet(opcode, packet_size);
    return packet;
  }

  hci_spec::OpCode opcode() const;
  // Returns the OGF (OpCode Group Field) which occupies the upper 6-bits of the opcode.
  uint8_t ogf() const;
  // Returns the OCF (OpCode Command Field) which occupies the lower 10-bits of the opcode.
  uint16_t ocf() const;

 protected:
  explicit EmbossCommandPacket(hci_spec::OpCode opcode, size_t packet_size);

 private:
  hci_spec::EmbossCommandHeaderView header_view() const;
};

// Helper subclass that remembers the view type it was constructed with. It is safe to slice
// an EmbossCommandPacketT into an EmbossCommandPacket.
template <class ViewT>
class EmbossCommandPacketT : public EmbossCommandPacket {
 public:
  ViewT view_t() { return view<ViewT>(); }

 private:
  friend class EmbossCommandPacket;

  EmbossCommandPacketT(hci_spec::OpCode opcode, size_t packet_size)
      : EmbossCommandPacket(opcode, packet_size) {}
};

template <class ViewT>
class EmbossEventPacketT;

// EmbossEventPacket is the HCI Event packet specialization of DynamicPacket.
class EmbossEventPacket : public DynamicPacket {
 public:
  // Construct an HCI Event packet from an Emboss view T of |packet_size| total bytes (header +
  // payload).
  template <typename T>
  static EmbossEventPacketT<T> New(size_t packet_size) {
    EmbossEventPacketT<T> packet(packet_size);
    return packet;
  }

  // Construct an HCI Event packet from an Emboss view T and initialize its header with the
  // |event_code| and size.
  template <typename T>
  static EmbossEventPacketT<T> New(hci_spec::EventCode event_code) {
    EmbossEventPacketT<T> packet(T::IntrinsicSizeInBytes().Read());
    auto header = packet.template view<hci_spec::EmbossEventHeaderWriter>();
    header.event_code().Write(event_code);
    header.parameter_total_size().Write(T::IntrinsicSizeInBytes().Read() -
                                        hci_spec::EmbossEventHeader::IntrinsicSizeInBytes());
    return packet;
  }

 protected:
  explicit EmbossEventPacket(size_t packet_size);
};

// Helper subclass that remembers the view type it was constructed with. It is safe to slice
// an EmbossEventPacketT into an EmbossEventPacket.
template <class ViewT>
class EmbossEventPacketT : public EmbossEventPacket {
 public:
  ViewT view_t() { return view<ViewT>(); }

 private:
  friend class EmbossEventPacket;

  explicit EmbossEventPacketT(size_t packet_size) : EmbossEventPacket(packet_size) {}
};

}  // namespace bt::hci

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_EMBOSS_CONTROL_PACKETS_H_
