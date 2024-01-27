// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/cpp/task.h>

#include "pw_bluetooth/vendor.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/test_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/types.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/error.h"
#include "src/connectivity/bluetooth/core/bt-host/socket/socket_factory.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/inspect.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/mock_controller.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_packets.h"

namespace bt::l2cap {
namespace {

using namespace inspect::testing;
using namespace bt::testing;

using bt::testing::MockController;
using TestingBase = bt::testing::ControllerTest<MockController>;

using AclPriority = pw::bluetooth::AclPriority;

// Sized intentionally to cause fragmentation for outbound dynamic channel data but not others. May
// need adjustment if Signaling Channel Configuration {Request,Response} default transactions get
// much bigger.
constexpr size_t kMaxDataPacketLength = 64;
constexpr size_t kMaxPacketCount = 10;

// 2x Information Requests: Extended Features, Fixed Channels Supported
constexpr size_t kConnectionCreationPacketCount = 2;

constexpr l2cap::ChannelParameters kChannelParameters{l2cap::ChannelMode::kBasic, l2cap::kMaxMTU,
                                                      std::nullopt};
constexpr l2cap::ExtendedFeatures kExtendedFeatures =
    l2cap::kExtendedFeaturesBitEnhancedRetransmission;

// This test harness provides test cases for interations between the L2CAP layer, SocketFactory, and
// the transport layer.
class L2capIntegrationTest : public TestingBase {
 public:
  L2capIntegrationTest() = default;
  ~L2capIntegrationTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();
    const auto bredr_buffer_info = hci::DataBufferInfo(kMaxDataPacketLength, kMaxPacketCount);
    InitializeACLDataChannel(bredr_buffer_info);

    // TODO(63074): Remove assumptions about channel ordering so we can turn random ids on.
    l2cap_ = ChannelManager::Create(transport()->acl_data_channel(), transport()->command_channel(),
                                    /*random_channel_ids=*/false);

    test_device()->set_data_expectations_enabled(true);

    next_command_id_ = 1;

    socket_factory_ = std::make_unique<socket::SocketFactory<l2cap::Channel>>();
  }

  void TearDown() override {
    socket_factory_.reset();
    l2cap_ = nullptr;
    TestingBase::TearDown();
  }

  l2cap::CommandId NextCommandId() { return next_command_id_++; }

  zx::socket MakeSocketForChannel(fxl::WeakPtr<l2cap::Channel> channel) {
    return socket_factory_->MakeSocketForChannel(std::move(channel));
  }

  void QueueConfigNegotiation(hci_spec::ConnectionHandle handle,
                              l2cap::ChannelParameters local_params,
                              l2cap::ChannelParameters peer_params, l2cap::ChannelId local_cid,
                              l2cap::ChannelId remote_cid, l2cap::CommandId local_config_req_id,
                              l2cap::CommandId peer_config_req_id) {
    const auto kPeerConfigRsp =
        l2cap::testing::AclConfigRsp(local_config_req_id, handle, local_cid, local_params);
    const auto kPeerConfigReq =
        l2cap::testing::AclConfigReq(peer_config_req_id, handle, local_cid, peer_params);
    EXPECT_ACL_PACKET_OUT(
        test_device(),
        l2cap::testing::AclConfigReq(local_config_req_id, handle, remote_cid, local_params),
        &kPeerConfigRsp, &kPeerConfigReq);
    EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclConfigRsp(peer_config_req_id, handle,
                                                                      remote_cid, peer_params));
  }

  void QueueInboundL2capConnection(hci_spec::ConnectionHandle handle, l2cap::PSM psm,
                                   l2cap::ChannelId local_cid, l2cap::ChannelId remote_cid,
                                   l2cap::ChannelParameters local_params = kChannelParameters,
                                   l2cap::ChannelParameters peer_params = kChannelParameters) {
    const l2cap::CommandId kPeerConnReqId = 1;
    const l2cap::CommandId kPeerConfigReqId = kPeerConnReqId + 1;
    const auto kConfigReqId = NextCommandId();
    EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclConnectionRsp(kPeerConnReqId, handle,
                                                                          remote_cid, local_cid));
    QueueConfigNegotiation(handle, local_params, peer_params, local_cid, remote_cid, kConfigReqId,
                           kPeerConfigReqId);

    test_device()->SendACLDataChannelPacket(
        l2cap::testing::AclConnectionReq(kPeerConnReqId, handle, remote_cid, psm));
  }

  void QueueOutboundL2capConnection(hci_spec::ConnectionHandle handle, l2cap::PSM psm,
                                    l2cap::ChannelId local_cid, l2cap::ChannelId remote_cid,
                                    ChannelCallback open_cb,
                                    l2cap::ChannelParameters local_params = kChannelParameters,
                                    l2cap::ChannelParameters peer_params = kChannelParameters) {
    const l2cap::CommandId kPeerConfigReqId = 1;
    const auto kConnReqId = NextCommandId();
    const auto kConfigReqId = NextCommandId();
    const auto kConnRsp =
        l2cap::testing::AclConnectionRsp(kConnReqId, handle, local_cid, remote_cid);
    EXPECT_ACL_PACKET_OUT(test_device(),
                          l2cap::testing::AclConnectionReq(kConnReqId, handle, local_cid, psm),
                          &kConnRsp);
    QueueConfigNegotiation(handle, local_params, peer_params, local_cid, remote_cid, kConfigReqId,
                           kPeerConfigReqId);

    l2cap()->OpenL2capChannel(handle, psm, local_params, std::move(open_cb));
  }

  struct QueueAclConnectionRetVal {
    l2cap::CommandId extended_features_id;
    l2cap::CommandId fixed_channels_supported_id;
  };

  QueueAclConnectionRetVal QueueAclConnection(
      hci_spec::ConnectionHandle handle,
      hci_spec::ConnectionRole role = hci_spec::ConnectionRole::CENTRAL) {
    QueueAclConnectionRetVal cmd_ids;
    cmd_ids.extended_features_id = NextCommandId();
    cmd_ids.fixed_channels_supported_id = NextCommandId();

    const auto kExtFeaturesRsp = l2cap::testing::AclExtFeaturesInfoRsp(cmd_ids.extended_features_id,
                                                                       handle, kExtendedFeatures);
    EXPECT_ACL_PACKET_OUT(
        test_device(), l2cap::testing::AclExtFeaturesInfoReq(cmd_ids.extended_features_id, handle),
        &kExtFeaturesRsp);
    EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclFixedChannelsSupportedInfoReq(
                                             cmd_ids.fixed_channels_supported_id, handle));

    acl_data_channel()->RegisterLink(handle, bt::LinkType::kACL);
    l2cap()->AddACLConnection(
        handle, role, /*link_error_callback=*/[]() {},
        /*security_callback=*/[](auto, auto, auto) {});
    return cmd_ids;
  }

  ChannelManager::LEFixedChannels QueueLEConnection(hci_spec::ConnectionHandle handle,
                                                    hci_spec::ConnectionRole role) {
    acl_data_channel()->RegisterLink(handle, bt::LinkType::kLE);
    return l2cap()->AddLEConnection(
        handle, role, /*link_error_callback=*/[] {}, /*conn_param_callback=*/[](auto&) {},
        /*security_callback=*/[](auto, auto, auto) {});
  }

  ChannelManager* l2cap() const { return l2cap_.get(); }

 private:
  std::unique_ptr<ChannelManager> l2cap_;
  l2cap::CommandId next_command_id_;
  std::unique_ptr<socket::SocketFactory<l2cap::Channel>> socket_factory_;

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(L2capIntegrationTest);
};

TEST_F(L2capIntegrationTest, InboundL2capSocket) {
  constexpr l2cap::PSM kPSM = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId = 0x0040;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  QueueAclConnection(kLinkHandle);

  fxl::WeakPtr<l2cap::Channel> chan;
  auto chan_cb = [&](auto cb_chan) {
    EXPECT_EQ(kLinkHandle, cb_chan->link_handle());
    chan = std::move(cb_chan);
  };

  l2cap()->RegisterService(kPSM, kChannelParameters, std::move(chan_cb));
  RunLoopUntilIdle();

  QueueInboundL2capConnection(kLinkHandle, kPSM, kLocalId, kRemoteId);

  RunLoopUntilIdle();
  ASSERT_TRUE(chan);
  zx::socket sock = MakeSocketForChannel(chan);

  // Test basic channel<->socket interaction by verifying that an ACL packet
  // gets routed to the socket.
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01, 0x00, 0x08, 0x00,

      // L2CAP B-frame: (length: 4, channel-id: 0x0040 (kLocalId))
      0x04, 0x00, 0x40, 0x00, 't', 'e', 's', 't'));

  // Run until the packet is written to the socket buffer.
  RunLoopUntilIdle();

  // Allocate a larger buffer than the number of SDU bytes we expect (which is
  // 4).
  StaticByteBuffer<10> socket_bytes;
  size_t bytes_read;
  zx_status_t status = sock.read(0, socket_bytes.mutable_data(), socket_bytes.size(), &bytes_read);
  EXPECT_EQ(ZX_OK, status);
  ASSERT_EQ(4u, bytes_read);
  EXPECT_EQ("test", socket_bytes.view(0, bytes_read).AsString());

  const char write_data[81] = "🚂🚃🚄🚅🚆🚈🚇🚈🚉🚊🚋🚌🚎🚝🚞🚟🚠🚡🛤🛲";

  // Test outbound data fragments using |kMaxDataPacketLength|.
  constexpr size_t kFirstFragmentPayloadSize = kMaxDataPacketLength - sizeof(l2cap::BasicHeader);
  const StaticByteBuffer<sizeof(hci_spec::ACLDataHeader) + sizeof(l2cap::BasicHeader) +
                         kFirstFragmentPayloadSize>
      kFirstFragment(
          // ACL data header (handle: 1, length 64)
          0x01, 0x00, 0x40, 0x00,

          // L2CAP B-frame: (length: 80, channel-id: 0x9042 (kRemoteId))
          0x50, 0x00, 0x42, 0x90,

          // L2CAP payload (fragmented)
          0xf0, 0x9f, 0x9a, 0x82, 0xf0, 0x9f, 0x9a, 0x83, 0xf0, 0x9f, 0x9a, 0x84, 0xf0, 0x9f, 0x9a,
          0x85, 0xf0, 0x9f, 0x9a, 0x86, 0xf0, 0x9f, 0x9a, 0x88, 0xf0, 0x9f, 0x9a, 0x87, 0xf0, 0x9f,
          0x9a, 0x88, 0xf0, 0x9f, 0x9a, 0x89, 0xf0, 0x9f, 0x9a, 0x8a, 0xf0, 0x9f, 0x9a, 0x8b, 0xf0,
          0x9f, 0x9a, 0x8c, 0xf0, 0x9f, 0x9a, 0x8e, 0xf0, 0x9f, 0x9a, 0x9d, 0xf0, 0x9f, 0x9a, 0x9e);

  constexpr size_t kSecondFragmentPayloadSize = sizeof(write_data) - 1 - kFirstFragmentPayloadSize;
  const StaticByteBuffer<sizeof(hci_spec::ACLDataHeader) + kSecondFragmentPayloadSize>
      kSecondFragment(
          // ACL data header (handle: 1, pbf: continuing fr., length: 20)
          0x01, 0x10, 0x14, 0x00,

          // L2CAP payload (final fragment)
          0xf0, 0x9f, 0x9a, 0x9f, 0xf0, 0x9f, 0x9a, 0xa0, 0xf0, 0x9f, 0x9a, 0xa1, 0xf0, 0x9f, 0x9b,
          0xa4, 0xf0, 0x9f, 0x9b, 0xb2);

  // The 80-byte write should be fragmented over 64- and 20-byte HCI payloads in order to send it
  // to the controller.
  EXPECT_ACL_PACKET_OUT(test_device(), kFirstFragment);
  EXPECT_ACL_PACKET_OUT(test_device(), kSecondFragment);

  size_t bytes_written = 0;
  // Write 80 outbound bytes to the socket buffer.
  status = sock.write(0, write_data, sizeof(write_data) - 1, &bytes_written);
  EXPECT_EQ(ZX_OK, status);
  EXPECT_EQ(80u, bytes_written);

  // Run until the data is flushed out to the MockController.
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Synchronously closes channels & sockets.
  l2cap()->RemoveConnection(kLinkHandle);
  acl_data_channel()->UnregisterLink(kLinkHandle);
  acl_data_channel()->ClearControllerPacketCount(kLinkHandle);

  // try resending data now that connection is closed
  bytes_written = 0;
  status = sock.write(0, write_data, sizeof(write_data) - 1, &bytes_written);

  EXPECT_EQ(ZX_ERR_PEER_CLOSED, status);
  EXPECT_EQ(0u, bytes_written);

  // no packets should be sent
  RunLoopUntilIdle();
}

TEST_F(L2capIntegrationTest, InboundRfcommSocketFails) {
  constexpr l2cap::PSM kPSM = l2cap::kRFCOMM;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  QueueAclConnection(kLinkHandle);

  RunLoopUntilIdle();

  const l2cap::CommandId kPeerConnReqId = 1;

  // Incoming connection refused, RFCOMM is not routed.
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclConnectionRsp(kPeerConnReqId, kLinkHandle, kRemoteId, 0x0000 /*dest id*/,
                                       l2cap::ConnectionResult::kPSMNotSupported));

  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclConnectionReq(kPeerConnReqId, kLinkHandle, kRemoteId, kPSM));

  RunLoopUntilIdle();
}

TEST_F(L2capIntegrationTest, InboundPacketQueuedAfterChannelOpenIsNotDropped) {
  constexpr l2cap::PSM kPSM = l2cap::kSDP;
  constexpr l2cap::ChannelId kLocalId = 0x0040;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  QueueAclConnection(kLinkHandle);

  fxl::WeakPtr<l2cap::Channel> chan;
  auto chan_cb = [&](auto cb_chan) {
    EXPECT_EQ(kLinkHandle, cb_chan->link_handle());
    chan = std::move(cb_chan);
  };

  l2cap()->RegisterService(kPSM, kChannelParameters, std::move(chan_cb));
  RunLoopUntilIdle();

  constexpr l2cap::CommandId kConnectionReqId = 1;
  constexpr l2cap::CommandId kPeerConfigReqId = 6;
  const l2cap::CommandId kConfigReqId = NextCommandId();
  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclConnectionRsp(
                                           kConnectionReqId, kLinkHandle, kRemoteId, kLocalId));
  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclConfigReq(kConfigReqId, kLinkHandle,
                                                                    kRemoteId, kChannelParameters));
  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclConnectionReq(kConnectionReqId, kLinkHandle, kRemoteId, kPSM));

  // Config negotiation will not complete yet.
  RunLoopUntilIdle();

  // Remaining config negotiation will be added to dispatch loop.
  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclConfigRsp(kPeerConfigReqId, kLinkHandle,
                                                                    kRemoteId, kChannelParameters));
  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclConfigReq(kPeerConfigReqId, kLinkHandle, kLocalId, kChannelParameters));
  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclConfigRsp(kConfigReqId, kLinkHandle, kLocalId, kChannelParameters));

  // Queue up a data packet for the new channel before the channel configuration has been
  // processed.
  ASSERT_FALSE(chan);
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01, 0x00, 0x08, 0x00,

      // L2CAP B-frame: (length: 4, channel-id: 0x0040 (kLocalId))
      0x04, 0x00, 0x40, 0x00, 0xf0, 0x9f, 0x94, 0xb0));

  // Run until the channel opens and the packet is written to the socket buffer.
  RunLoopUntilIdle();
  ASSERT_TRUE(chan);
  zx::socket sock = MakeSocketForChannel(chan);

  // Allocate a larger buffer than the number of SDU bytes we expect (which is 4).
  StaticByteBuffer<10> socket_bytes;
  size_t bytes_read;
  zx_status_t status = sock.read(0, socket_bytes.mutable_data(), socket_bytes.size(), &bytes_read);
  EXPECT_EQ(ZX_OK, status);
  ASSERT_EQ(4u, bytes_read);
  EXPECT_EQ("🔰", socket_bytes.view(0, bytes_read).AsString());

  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclDisconnectionReq(
                                           NextCommandId(), kLinkHandle, kLocalId, kRemoteId));
}

TEST_F(L2capIntegrationTest, OutboundL2capSocket) {
  constexpr l2cap::PSM kPSM = l2cap::kAVCTP;
  constexpr l2cap::ChannelId kLocalId = 0x0040;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  QueueAclConnection(kLinkHandle);
  RunLoopUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  fxl::WeakPtr<l2cap::Channel> chan;
  auto chan_cb = [&](auto cb_chan) {
    EXPECT_EQ(kLinkHandle, cb_chan->link_handle());
    chan = std::move(cb_chan);
  };

  QueueOutboundL2capConnection(kLinkHandle, kPSM, kLocalId, kRemoteId, std::move(chan_cb));

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  // We should have opened a channel successfully.
  ASSERT_TRUE(chan);
  zx::socket sock = MakeSocketForChannel(chan);

  // Test basic channel<->socket interaction by verifying that an ACL packet
  // gets routed to the socket.
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01, 0x00, 0x08, 0x00,

      // L2CAP B-frame: (length: 4, channel-id: 0x0040 (kLocalId))
      0x04, 0x00, 0x40, 0x00, 't', 'e', 's', 't'));

  // Run until the packet is written to the socket buffer.
  RunLoopUntilIdle();

  // Allocate a larger buffer than the number of SDU bytes we expect (which is
  // 4).
  StaticByteBuffer<10> socket_bytes;
  size_t bytes_read;
  zx_status_t status = sock.read(0, socket_bytes.mutable_data(), socket_bytes.size(), &bytes_read);
  EXPECT_EQ(ZX_OK, status);
  ASSERT_EQ(4u, bytes_read);
  EXPECT_EQ("test", socket_bytes.view(0, bytes_read).AsString());

  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclDisconnectionReq(
                                           NextCommandId(), kLinkHandle, kLocalId, kRemoteId));
}

TEST_F(L2capIntegrationTest, OutboundChannelIsInvalidWhenL2capFailsToOpenChannel) {
  constexpr l2cap::PSM kPSM = l2cap::kAVCTP;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  // Don't register any links. This should cause outbound channels to fail.
  bool chan_cb_called = false;
  auto chan_cb = [&chan_cb_called](auto chan) {
    chan_cb_called = true;
    EXPECT_FALSE(chan);
  };

  l2cap()->OpenL2capChannel(kLinkHandle, kPSM, kChannelParameters, std::move(chan_cb));

  RunLoopUntilIdle();

  EXPECT_TRUE(chan_cb_called);
}

// Queue dynamic channel packets, then open a new dynamic channel.
// The signaling channel packets should be sent before the queued dynamic channel packets.
TEST_F(L2capIntegrationTest, ChannelCreationPrioritizedOverDynamicChannelData) {
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  constexpr l2cap::PSM kPSM0 = l2cap::kAVCTP;
  constexpr l2cap::ChannelId kLocalId0 = 0x0040;
  constexpr l2cap::ChannelId kRemoteId0 = 0x9042;

  constexpr l2cap::PSM kPSM1 = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId1 = 0x0041;
  constexpr l2cap::ChannelId kRemoteId1 = 0x9043;

  // l2cap connection request (or response), config request, config response
  constexpr size_t kChannelCreationPacketCount = 3;

  QueueAclConnection(kLinkHandle);

  fxl::WeakPtr<l2cap::Channel> chan0;
  auto chan_cb0 = [&](auto cb_chan) {
    EXPECT_EQ(kLinkHandle, cb_chan->link_handle());
    chan0 = std::move(cb_chan);
  };
  l2cap()->RegisterService(kPSM0, kChannelParameters, chan_cb0);

  QueueInboundL2capConnection(kLinkHandle, kPSM0, kLocalId0, kRemoteId0);

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(chan0);
  zx::socket sock0 = MakeSocketForChannel(chan0);

  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kLinkHandle, kConnectionCreationPacketCount + kChannelCreationPacketCount));

  // Dummy dynamic channel packet
  const StaticByteBuffer kPacket0(
      // ACL data header (handle: 1, length 5)
      0x01, 0x00, 0x05, 0x00,

      // L2CAP B-frame: (length: 1, channel-id: 0x9042 (kRemoteId))
      0x01, 0x00, 0x42, 0x90,

      // L2CAP payload
      0x01);

  // |kMaxPacketCount| packets should be sent to the controller,
  // and 1 packet should be left in the queue
  const char write_data[] = {0x01};
  for (size_t i = 0; i < kMaxPacketCount + 1; i++) {
    if (i != kMaxPacketCount) {
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);
    }
    size_t bytes_written = 0;
    zx_status_t status = sock0.write(0, write_data, sizeof(write_data), &bytes_written);
    EXPECT_EQ(ZX_OK, status);
    EXPECT_EQ(sizeof(write_data), bytes_written);
  }

  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());
  // Run until the data is flushed out to the MockController.
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  fxl::WeakPtr<l2cap::Channel> chan1;
  auto chan_cb1 = [&](auto cb_chan) {
    EXPECT_EQ(kLinkHandle, cb_chan->link_handle());
    chan1 = std::move(cb_chan);
  };

  QueueOutboundL2capConnection(kLinkHandle, kPSM1, kLocalId1, kRemoteId1, std::move(chan_cb1));

  for (size_t i = 0; i < kChannelCreationPacketCount; i++) {
    test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(kLinkHandle, 1));
    // Wait for next connection creation packet to be queued (eg. configuration request/response).
    RunLoopUntilIdle();
  }

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(chan1);

  // Make room in buffer for queued dynamic channel packet.
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(kLinkHandle, 1));

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);
  RunLoopUntilIdle();
  // 1 Queued dynamic channel data packet should have been sent.
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(L2capIntegrationTest, NegotiateChannelParametersOnOutboundL2capSocket) {
  constexpr l2cap::PSM kPSM = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId = 0x0040;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;
  constexpr uint16_t kMtu = l2cap::kMinACLMTU;

  l2cap::ChannelParameters chan_params;
  chan_params.mode = l2cap::ChannelMode::kEnhancedRetransmission;
  chan_params.max_rx_sdu_size = kMtu;

  QueueAclConnection(kLinkHandle);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  fxl::WeakPtr<l2cap::Channel> chan;
  auto chan_cb = [&](fxl::WeakPtr<l2cap::Channel> cb_chan) { chan = std::move(cb_chan); };

  QueueOutboundL2capConnection(kLinkHandle, kPSM, kLocalId, kRemoteId, chan_cb, chan_params,
                               chan_params);

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(chan);
  EXPECT_EQ(kLinkHandle, chan->link_handle());
  EXPECT_EQ(*chan_params.max_rx_sdu_size, chan->max_rx_sdu_size());
  EXPECT_EQ(*chan_params.mode, chan->mode());
}

TEST_F(L2capIntegrationTest, NegotiateChannelParametersOnInboundL2capSocket) {
  constexpr l2cap::PSM kPSM = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId = 0x0040;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  l2cap::ChannelParameters chan_params;
  chan_params.mode = l2cap::ChannelMode::kEnhancedRetransmission;
  chan_params.max_rx_sdu_size = l2cap::kMinACLMTU;

  QueueAclConnection(kLinkHandle);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  fxl::WeakPtr<l2cap::Channel> chan;
  auto chan_cb = [&](auto cb_chan) { chan = std::move(cb_chan); };
  l2cap()->RegisterService(kPSM, chan_params, chan_cb);

  QueueInboundL2capConnection(kLinkHandle, kPSM, kLocalId, kRemoteId, chan_params, chan_params);

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(chan);
  EXPECT_EQ(*chan_params.max_rx_sdu_size, chan->max_rx_sdu_size());
  EXPECT_EQ(*chan_params.mode, chan->mode());

  zx::socket sock = MakeSocketForChannel(chan);
  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclDisconnectionReq(
                                           NextCommandId(), kLinkHandle, kLocalId, kRemoteId));
}

TEST_F(L2capIntegrationTest, RequestConnectionParameterUpdateAndReceiveResponse) {
  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;
  const hci_spec::LEPreferredConnectionParameters kParams(kIntervalMin, kIntervalMax,
                                                          kPeripheralLatency, kTimeoutMult);

  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;
  QueueLEConnection(kLinkHandle, hci_spec::ConnectionRole::PERIPHERAL);

  std::optional<bool> accepted;
  auto request_cb = [&accepted](bool cb_accepted) { accepted = cb_accepted; };

  // Receive "Accepted" Response:

  l2cap::CommandId param_update_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclConnectionParameterUpdateReq(
                                           param_update_req_id, kLinkHandle, kIntervalMin,
                                           kIntervalMax, kPeripheralLatency, kTimeoutMult));
  l2cap()->RequestConnectionParameterUpdate(kLinkHandle, kParams, request_cb);
  RunLoopUntilIdle();
  EXPECT_FALSE(accepted.has_value());

  test_device()->SendACLDataChannelPacket(l2cap::testing::AclConnectionParameterUpdateRsp(
      param_update_req_id, kLinkHandle, l2cap::ConnectionParameterUpdateResult::kAccepted));
  RunLoopUntilIdle();
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(accepted.value());
  accepted.reset();
}

#ifndef NINSPECT
TEST_F(L2capIntegrationTest, InspectHierarchy) {
  inspect::Inspector inspector;
  l2cap()->AttachInspect(inspector.GetRoot(), ChannelManager::kInspectNodeName);
  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy);
  auto l2cap_matcher =
      AllOf(NodeMatches(PropertyList(::testing::IsEmpty())),
            ChildrenMatch(UnorderedElementsAre(NodeMatches(NameMatches("logical_links")),
                                               NodeMatches(NameMatches("services")))));
  EXPECT_THAT(hierarchy.value(), AllOf(ChildrenMatch(UnorderedElementsAre(l2cap_matcher))));
}
#endif  // NINSPECT

TEST_F(L2capIntegrationTest, AddLEConnectionReturnsFixedChannels) {
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;
  auto channels = QueueLEConnection(kLinkHandle, hci_spec::ConnectionRole::PERIPHERAL);
  ASSERT_TRUE(channels.att);
  EXPECT_EQ(l2cap::kATTChannelId, channels.att->id());
  ASSERT_TRUE(channels.smp);
  EXPECT_EQ(l2cap::kLESMPChannelId, channels.smp->id());
}

class AclPriorityTest : public L2capIntegrationTest,
                        public ::testing::WithParamInterface<std::pair<AclPriority, bool>> {};

TEST_P(AclPriorityTest, OutboundConnectAndSetPriority) {
  const auto kPriority = GetParam().first;
  const bool kExpectSuccess = GetParam().second;

  // Arbitrary command payload larger than CommandHeader.
  const auto op_code = hci_spec::VendorOpCode(0x01);
  const StaticByteBuffer kEncodedCommand(LowerBits(op_code), UpperBits(op_code),  // op code
                                         0x04,                                    // parameter size
                                         0x00, 0x01, 0x02, 0x03);                 // test parameter

  constexpr l2cap::PSM kPSM = l2cap::kAVCTP;
  constexpr l2cap::ChannelId kLocalId = 0x0040;
  constexpr l2cap::ChannelId kRemoteId = 0x9042;
  constexpr hci_spec::ConnectionHandle kLinkHandle = 0x0001;

  std::optional<hci_spec::ConnectionHandle> connection_handle_from_encode_cb;
  std::optional<AclPriority> priority_from_encode_cb;
  test_device()->set_encode_vendor_command_cb(
      [&](pw::bluetooth::VendorCommandParameters vendor_params,
          fit::callback<void(pw::Result<pw::span<const std::byte>>)> callback) {
        ASSERT_TRUE(
            std::holds_alternative<pw::bluetooth::SetAclPriorityCommandParameters>(vendor_params));
        auto& params = std::get<pw::bluetooth::SetAclPriorityCommandParameters>(vendor_params);
        connection_handle_from_encode_cb = params.connection_handle;
        priority_from_encode_cb = params.priority;
        callback(kEncodedCommand.subspan());
      });

  QueueAclConnection(kLinkHandle);
  RunLoopUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  fxl::WeakPtr<l2cap::Channel> channel = nullptr;
  auto chan_cb = [&](auto chan) { channel = std::move(chan); };

  QueueOutboundL2capConnection(kLinkHandle, kPSM, kLocalId, kRemoteId, std::move(chan_cb));

  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  // We should have opened a channel successfully.
  ASSERT_TRUE(channel);
  channel->Activate([](auto) {}, []() {});

  if (kPriority != AclPriority::kNormal) {
    auto cmd_complete =
        CommandCompletePacket(op_code, kExpectSuccess ? hci_spec::StatusCode::SUCCESS
                                                      : hci_spec::StatusCode::UNKNOWN_COMMAND);
    EXPECT_CMD_PACKET_OUT(test_device(), kEncodedCommand, &cmd_complete);
  }

  size_t request_cb_count = 0;
  channel->RequestAclPriority(kPriority, [&](auto result) {
    request_cb_count++;
    EXPECT_EQ(kExpectSuccess, result.is_ok());
  });

  RunLoopUntilIdle();
  EXPECT_EQ(request_cb_count, 1u);
  if (kPriority == AclPriority::kNormal) {
    EXPECT_FALSE(connection_handle_from_encode_cb);
  } else {
    ASSERT_TRUE(connection_handle_from_encode_cb);
    EXPECT_EQ(connection_handle_from_encode_cb.value(), kLinkHandle);
    ASSERT_TRUE(priority_from_encode_cb);
    EXPECT_EQ(priority_from_encode_cb.value(), kPriority);
  }
  connection_handle_from_encode_cb.reset();
  priority_from_encode_cb.reset();

  if (kPriority != AclPriority::kNormal && kExpectSuccess) {
    auto cmd_complete = CommandCompletePacket(op_code, hci_spec::StatusCode::SUCCESS);
    EXPECT_CMD_PACKET_OUT(test_device(), kEncodedCommand, &cmd_complete);
  }

  EXPECT_ACL_PACKET_OUT(test_device(), l2cap::testing::AclDisconnectionReq(
                                           NextCommandId(), kLinkHandle, kLocalId, kRemoteId));

  // Deactivating channel should send priority command to revert priority back to normal if it was
  // changed.
  channel->Deactivate();
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  if (kPriority != AclPriority::kNormal && kExpectSuccess) {
    ASSERT_TRUE(connection_handle_from_encode_cb);
    EXPECT_EQ(connection_handle_from_encode_cb.value(), kLinkHandle);
    ASSERT_TRUE(priority_from_encode_cb);
    EXPECT_EQ(priority_from_encode_cb.value(), AclPriority::kNormal);
  } else {
    EXPECT_FALSE(connection_handle_from_encode_cb);
  }
}

const std::array<std::pair<AclPriority, bool>, 4> kPriorityParams = {
    {{AclPriority::kSource, false},
     {AclPriority::kSource, true},
     {AclPriority::kSink, true},
     {AclPriority::kNormal, true}}};
INSTANTIATE_TEST_SUITE_P(L2capTest, AclPriorityTest, ::testing::ValuesIn(kPriorityParams));

}  // namespace
}  // namespace bt::l2cap
