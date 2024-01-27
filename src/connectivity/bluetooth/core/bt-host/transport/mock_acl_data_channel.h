// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_MOCK_ACL_DATA_CHANNEL_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_MOCK_ACL_DATA_CHANNEL_H_

#include "src/connectivity/bluetooth/core/bt-host/common/inspect.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/acl_data_channel.h"

namespace bt::hci::testing {

class MockAclDataChannel final : public AclDataChannel {
 public:
  MockAclDataChannel() = default;
  ~MockAclDataChannel() override = default;

  void set_bredr_buffer_info(DataBufferInfo info) { bredr_buffer_info_ = info; }
  void set_le_buffer_info(DataBufferInfo info) { le_buffer_info_ = info; }

  using SendPacketsCallback = fit::function<bool(
      std::list<ACLDataPacketPtr> packets, UniqueChannelId channel_id, PacketPriority priority)>;
  void set_send_packets_cb(SendPacketsCallback cb) { send_packets_cb_ = std::move(cb); }

  using DropQueuedPacketsCallback = fit::function<void(AclPacketPredicate predicate)>;
  void set_drop_queued_packets_cb(DropQueuedPacketsCallback cb) {
    drop_queued_packets_cb_ = std::move(cb);
  }

  using RequestAclPriorityCallback =
      fit::function<void(pw::bluetooth::AclPriority priority, hci_spec::ConnectionHandle handle,
                         fit::callback<void(fit::result<fit::failed>)> callback)>;
  void set_request_acl_priority_cb(RequestAclPriorityCallback cb) {
    request_acl_priority_cb_ = std::move(cb);
  }

  void ReceivePacket(std::unique_ptr<ACLDataPacket> packet) {
    BT_ASSERT(data_rx_handler_);
    data_rx_handler_(std::move(packet));
  }

  // AclDataChannel overrides:
  void AttachInspect(inspect::Node& /*unused*/, const std::string& /*unused*/) override {}
  void SetDataRxHandler(ACLPacketHandler rx_callback) override {
    BT_ASSERT(rx_callback);
    data_rx_handler_ = std::move(rx_callback);
  }
  bool SendPacket(ACLDataPacketPtr data_packet, UniqueChannelId channel_id,
                  PacketPriority priority) override {
    return false;
  }
  bool SendPackets(std::list<ACLDataPacketPtr> packets, UniqueChannelId channel_id,
                   PacketPriority priority) override {
    if (send_packets_cb_) {
      return send_packets_cb_(std::move(packets), channel_id, priority);
    }
    return true;
  }
  void RegisterLink(hci_spec::ConnectionHandle handle, bt::LinkType ll_type) override {}
  void UnregisterLink(hci_spec::ConnectionHandle handle) override {}
  void DropQueuedPackets(AclPacketPredicate predicate) override {
    if (drop_queued_packets_cb_) {
      drop_queued_packets_cb_(std::move(predicate));
    }
  }
  void ClearControllerPacketCount(hci_spec::ConnectionHandle handle) override {}
  const DataBufferInfo& GetBufferInfo() const override { return bredr_buffer_info_; }
  const DataBufferInfo& GetLeBufferInfo() const override { return le_buffer_info_; }
  void RequestAclPriority(pw::bluetooth::AclPriority priority, hci_spec::ConnectionHandle handle,
                          fit::callback<void(fit::result<fit::failed>)> callback) override {
    if (request_acl_priority_cb_) {
      request_acl_priority_cb_(priority, handle, std::move(callback));
    }
  }

 private:
  DataBufferInfo bredr_buffer_info_;
  DataBufferInfo le_buffer_info_;

  ACLPacketHandler data_rx_handler_;
  SendPacketsCallback send_packets_cb_;
  DropQueuedPacketsCallback drop_queued_packets_cb_;
  RequestAclPriorityCallback request_acl_priority_cb_;
};

}  // namespace bt::hci::testing

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TRANSPORT_MOCK_ACL_DATA_CHANNEL_H_
