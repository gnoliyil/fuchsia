// Copyright 2017 The Fuchsia Authors. All rights reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

#include "src/connectivity/bluetooth/core/bt-host/transport/acl_data_channel.h"

#include <lib/async/cpp/task.h>

#include <unordered_map>

#include <gmock/gmock.h>

#include "gtest/gtest.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/constants.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/defaults.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/inspect.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/inspect_util.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/mock_controller.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/acl_data_packet.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/data_buffer_info.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/fake_acl_connection.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/link_type.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"

namespace bt::hci {
namespace {

constexpr hci_spec::ConnectionHandle kConnectionHandle0 = 0x0000;
constexpr hci_spec::ConnectionHandle kConnectionHandle1 = 0x0001;
constexpr size_t kMaxMtu = 10;
constexpr size_t kBufferMaxNumPackets = 2;

std::array<std::pair<DataBufferInfo, DataBufferInfo>, 2> bredr_both_buffers = {{
    {DataBufferInfo(kMaxMtu, kBufferMaxNumPackets), DataBufferInfo()},  // OnlyBREDRBufferAvailable
    {DataBufferInfo(kMaxMtu, kBufferMaxNumPackets),
     DataBufferInfo(kMaxMtu, kBufferMaxNumPackets)}  // BothBuffersAvailable
}};

std::array<std::pair<DataBufferInfo, DataBufferInfo>, 3> all_buffer_options = {{
    {DataBufferInfo(kMaxMtu, kBufferMaxNumPackets), DataBufferInfo()},  // OnlyBREDRBufferAvailable
    {DataBufferInfo(), DataBufferInfo(kMaxMtu, kBufferMaxNumPackets)},  // OnlyLEBufferAvailable
    {DataBufferInfo(kMaxMtu, kBufferMaxNumPackets),
     DataBufferInfo(kMaxMtu, kBufferMaxNumPackets)}  // BothBuffersAvailable
}};

using pw::bluetooth::AclPriority;
using TestingBase = bt::testing::ControllerTest<bt::testing::MockController>;

class AclDataChannelTest : public TestingBase {
 public:
  AclDataChannelTest() = default;

 protected:
  // TestBase overrides:
  void SetUp() override { TestingBase::SetUp(); }

  // Fill up controller buffer then queue one additional packet
  void FillControllerBufferThenQueuePacket(FakeAclConnection& connection) {
    for (size_t i = 0; i <= kBufferMaxNumPackets; i++) {
      // Last packet should remain queued
      if (i < kBufferMaxNumPackets) {
        const StaticByteBuffer kPacket(
            // ACL data header (handle: 0, length 1)
            LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
            // payload length
            0x01, 0x00,
            // payload
            static_cast<uint8_t>(i));
        EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
      }
      // Create packet to send
      ACLDataPacketPtr packet = ACLDataPacket::New(
          kConnectionHandle0, hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
          hci_spec::ACLBroadcastFlag::kPointToPoint,
          /*payload_size=*/1);
      packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
      connection.QueuePacket(std::move(packet));
      RunLoopUntilIdle();
    }
  }

 private:
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AclDataChannelTest);
};
class AclDataChannelMtuTest : public AclDataChannelTest {
 public:
  DataBufferInfo BREDRBufferInfo() { return DataBufferInfo(1024, 50); };
  DataBufferInfo LEBufferInfo() { return DataBufferInfo(64, 16); };
};

class AclDataChannelOnlyBREDRBufferAvailable : public AclDataChannelTest {
 public:
  void SetUp() override {
    AclDataChannelTest::SetUp();

    InitializeACLDataChannel(DataBufferInfo(kMaxMtu, kBufferMaxNumPackets), DataBufferInfo());
  }
};

class AclDataChannelOnlyLEBufferAvailable : public AclDataChannelTest {
 public:
  void SetUp() override {
    AclDataChannelTest::SetUp();

    InitializeACLDataChannel(DataBufferInfo(), DataBufferInfo(kMaxMtu, kBufferMaxNumPackets));
  }
};

// Value-parameterized test class for when: 1) only BR/EDR buffer available 2) when both BR/EDR and
// LE buffers available
class AclDataChannelBREDRAndBothBuffers
    : public AclDataChannelTest,
      public ::testing::WithParamInterface<std::pair<DataBufferInfo, DataBufferInfo>> {};

// Value-parameterized test class for when: 1) only BR/EDR buffer available 2) only LE buffer
// available 3) when both BR/EDR and LE buffers available
class AclDataChannelAllBufferCombinations
    : public AclDataChannelTest,
      public ::testing::WithParamInterface<std::pair<DataBufferInfo, DataBufferInfo>> {};

/*
 * Tests Begin
 */
#ifndef NINSPECT
TEST_F(AclDataChannelTest, InspectHierarchyContainsOutboundQueueState) {
  InitializeACLDataChannel(DataBufferInfo(kMaxMtu, kBufferMaxNumPackets),
                           DataBufferInfo(kMaxMtu, kBufferMaxNumPackets));

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  // Fill up both BR/EDR and LE controller buffers then queue one additional packet in the queue of
  // each type
  for (size_t i = 0; i <= kBufferMaxNumPackets; i++) {
    for (FakeAclConnection* connection : {&connection_0, &connection_1}) {
      if (i < kBufferMaxNumPackets) {
        const StaticByteBuffer kPacket(LowerBits(connection->handle()),
                                       UpperBits(connection->handle()),
                                       // payload length
                                       0x01, 0x00,
                                       // payload
                                       static_cast<uint8_t>(i));
        EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
      }

      // Create packet to send
      ACLDataPacketPtr packet = ACLDataPacket::New(
          connection->handle(), hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
          hci_spec::ACLBroadcastFlag::kPointToPoint,
          /*payload_size=*/1);
      packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
      connection->QueuePacket(std::move(packet));
      RunLoopUntilIdle();
    }
  }

  inspect::Inspector inspector;
  const std::string kNodeName = "adc_node_name";
  acl_data_channel()->AttachInspect(inspector.GetRoot(), kNodeName);

  using namespace ::inspect::testing;
  auto bredr_matcher = NodeMatches(
      AllOf(NameMatches("bredr"),
            PropertyList(ElementsAre(UintIs("num_sent_packets", kBufferMaxNumPackets)))));

  auto le_matcher = NodeMatches(
      AllOf(NameMatches("le"),
            PropertyList(UnorderedElementsAre(UintIs("num_sent_packets", kBufferMaxNumPackets),
                                              BoolIs("independent_from_bredr", true)))));
}
#endif  // NINSPECT

class AclPriorityTest : public AclDataChannelTest,
                        public ::testing::WithParamInterface<std::pair<AclPriority, bool>> {};
TEST_P(AclPriorityTest, RequestAclPriority) {
  const auto kPriority = GetParam().first;
  const bool kExpectSuccess = GetParam().second;

  InitializeACLDataChannel(DataBufferInfo(1024, 50), DataBufferInfo());

  // Arbitrary command payload larger than hci_spec::CommandHeader.
  const auto op_code = hci_spec::VendorOpCode(0x01);
  const StaticByteBuffer kEncodedCommand(LowerBits(op_code), UpperBits(op_code),  // op code
                                         0x04,                                    // parameter size
                                         0x00, 0x01, 0x02, 0x03);                 // test parameter

  std::optional<hci_spec::ConnectionHandle> connection;
  std::optional<AclPriority> priority;
  test_device()->set_encode_vendor_command_cb(
      [&](pw::bluetooth::VendorCommandParameters cb_params,
          fit::callback<void(pw::Result<pw::span<const std::byte>>)> cb) {
        ASSERT_TRUE(
            std::holds_alternative<pw::bluetooth::SetAclPriorityCommandParameters>(cb_params));
        pw::bluetooth::SetAclPriorityCommandParameters params =
            std::get<pw::bluetooth::SetAclPriorityCommandParameters>(cb_params);
        connection = params.connection_handle;
        priority = params.priority;
        cb(pw::span(reinterpret_cast<const std::byte*>(kEncodedCommand.data()),
                    kEncodedCommand.size()));
      });

  auto cmd_complete = bt::testing::CommandCompletePacket(
      op_code, kExpectSuccess ? pw::bluetooth::emboss::StatusCode::SUCCESS
                              : pw::bluetooth::emboss::StatusCode::UNKNOWN_COMMAND);
  EXPECT_CMD_PACKET_OUT(test_device(), kEncodedCommand, &cmd_complete);

  size_t request_cb_count = 0;
  acl_data_channel()->RequestAclPriority(kPriority, kConnectionHandle1, [&](auto result) {
    request_cb_count++;
    EXPECT_EQ(kExpectSuccess, result.is_ok());
  });

  RunLoopUntilIdle();
  EXPECT_EQ(request_cb_count, 1u);
  ASSERT_TRUE(connection);
  EXPECT_EQ(connection.value(), kConnectionHandle1);
  ASSERT_TRUE(priority);
  EXPECT_EQ(priority.value(), kPriority);
}
const std::array<std::pair<AclPriority, bool>, 4> kPriorityParams = {
    {{AclPriority::kSource, /*expect_success=*/false},
     {AclPriority::kSource, true},
     {AclPriority::kSink, true},
     {AclPriority::kNormal, true}}};
INSTANTIATE_TEST_SUITE_P(ACLDataChannelTest, AclPriorityTest, ::testing::ValuesIn(kPriorityParams));

TEST_F(AclDataChannelTest, RequestAclPriorityEncodeFails) {
  InitializeACLDataChannel(DataBufferInfo(1024, 50), DataBufferInfo());

  test_device()->set_encode_vendor_command_cb([](auto, auto cb) { cb(pw::Status::Internal()); });

  size_t request_cb_count = 0;
  acl_data_channel()->RequestAclPriority(hci::AclPriority::kSink, kConnectionHandle1,
                                         [&](auto result) {
                                           request_cb_count++;
                                           EXPECT_TRUE(result.is_error());
                                         });

  RunLoopUntilIdle();
  EXPECT_EQ(request_cb_count, 1u);
}

TEST_F(AclDataChannelTest, RequestAclPriorityEncodeReturnsTooSmallBuffer) {
  InitializeACLDataChannel(DataBufferInfo(1024, 50), DataBufferInfo());

  test_device()->set_encode_vendor_command_cb(
      [](auto, fit::callback<void(pw::Result<pw::span<const std::byte>>)> cb) {
        const std::byte buffer[] = {std::byte{0x00}};
        cb(buffer);
      });

  size_t request_cb_count = 0;
  acl_data_channel()->RequestAclPriority(hci::AclPriority::kSink, kConnectionHandle1,
                                         [&](auto result) {
                                           request_cb_count++;
                                           EXPECT_TRUE(result.is_error());
                                         });

  RunLoopUntilIdle();
  EXPECT_EQ(request_cb_count, 1u);
}

TEST_F(AclDataChannelMtuTest, VerifyBREDRBufferMtus) {
  InitializeACLDataChannel(BREDRBufferInfo(), DataBufferInfo());
  EXPECT_EQ(BREDRBufferInfo(), acl_data_channel()->GetBufferInfo());
  EXPECT_EQ(BREDRBufferInfo(), acl_data_channel()->GetLeBufferInfo());
}

TEST_F(AclDataChannelMtuTest, VerifyLEBufferMtus) {
  InitializeACLDataChannel(DataBufferInfo(), LEBufferInfo());
  EXPECT_EQ(DataBufferInfo(), acl_data_channel()->GetBufferInfo());
  EXPECT_EQ(LEBufferInfo(), acl_data_channel()->GetLeBufferInfo());
}

TEST_F(AclDataChannelMtuTest, VerifyBREDRAndLEBufferMtus) {
  InitializeACLDataChannel(BREDRBufferInfo(), LEBufferInfo());
  EXPECT_EQ(BREDRBufferInfo(), acl_data_channel()->GetBufferInfo());
  EXPECT_EQ(LEBufferInfo(), acl_data_channel()->GetLeBufferInfo());
}

TEST_F(AclDataChannelOnlyBREDRBufferAvailable, NumberOfCompletedPacketsExceedsPendingPackets) {
  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  FillControllerBufferThenQueuePacket(connection_0);
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Send out last packet
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
                            // payload length
                            0x01, 0x00,
                            // payload
                            static_cast<uint8_t>(kBufferMaxNumPackets)));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, kBufferMaxNumPackets + 1));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(AclDataChannelOnlyBREDRBufferAvailable, UnregisterLinkDropsFutureSentPackets) {
  constexpr size_t kMaxNumPackets = 1;

  InitializeACLDataChannel(DataBufferInfo(kMaxMtu, kMaxNumPackets), DataBufferInfo());

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  const StaticByteBuffer kPacket(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      static_cast<uint8_t>(1));
  EXPECT_ACL_PACKET_OUT(test_device(), kPacket);

  // Create packet to send
  ACLDataPacketPtr packet =
      ACLDataPacket::New(kConnectionHandle0, hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  connection_0.QueuePacket(std::move(packet));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kConnectionHandle0);

  // Attempt to send packet on an unregistered link
  connection_0.QueuePacket(std::move(packet));
  RunLoopUntilIdle();

  // Second packet should not have been sent
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(AclDataChannelOnlyLEBufferAvailable, UnregisterLinkDropsFutureSentPackets) {
  constexpr size_t kMaxNumPackets = 1;

  InitializeACLDataChannel(DataBufferInfo(kMaxMtu, kMaxNumPackets), DataBufferInfo());

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  const StaticByteBuffer kPacket(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      static_cast<uint8_t>(1));
  EXPECT_ACL_PACKET_OUT(test_device(), kPacket);

  // Create packet to send
  ACLDataPacketPtr packet =
      ACLDataPacket::New(kConnectionHandle0, hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  connection_0.QueuePacket(std::move(packet));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kConnectionHandle0);

  // Attempt to send packet on an unregistered link
  connection_0.QueuePacket(std::move(packet));
  RunLoopUntilIdle();

  // Second packet should not have been sent
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(AclDataChannelOnlyBREDRBufferAvailable,
       IgnoreNumberOfCompletedPacketsEventForUnknownConnectionHandle) {
  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  FillControllerBufferThenQueuePacket(connection_0);
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // |kConnectionHandle1| is not registered so this event is ignored (no packets should be sent)
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 1));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
}

TEST_F(AclDataChannelOnlyLEBufferAvailable,
       IgnoreNumberOfCompletedPacketsEventForUnknownConnectionHandle) {
  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  FillControllerBufferThenQueuePacket(connection_0);
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // |kConnectionHandle1| is not registered so this event is ignored (no packets should be sent)
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 1));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
}

TEST_P(AclDataChannelBREDRAndBothBuffers, SendMoreBREDRPacketsThanMaximumBufferSpace) {
  InitializeACLDataChannel(GetParam().first, GetParam().second);

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  FillControllerBufferThenQueuePacket(connection_0);
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Send out last packet
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
                            // payload length
                            0x01, 0x00,
                            // payload
                            static_cast<uint8_t>(kBufferMaxNumPackets)));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 1));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(AclDataChannelOnlyLEBufferAvailable, SendMoreBREDRPacketsThanMaximumBufferSpace) {
  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  // Create packet to send
  ACLDataPacketPtr packet =
      ACLDataPacket::New(kConnectionHandle0, hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  connection_0.QueuePacket(std::move(packet));
  RunLoopUntilIdle();

  // No packet should be sent since the controller's BR/EDR buffer has no availabity
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_P(AclDataChannelAllBufferCombinations, SendMoreLEPacketsThanMaximumBufferSpace) {
  InitializeACLDataChannel(GetParam().first, GetParam().second);

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());

  FillControllerBufferThenQueuePacket(connection_0);
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Send out last packet
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
                            // payload length
                            0x01, 0x00,
                            // payload
                            static_cast<uint8_t>(kBufferMaxNumPackets)));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 1));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

/*
 * Multiple Connections
 */
TEST_P(AclDataChannelBREDRAndBothBuffers, RegisterTwoBREDRConnectionsAndUnregisterFirstConnection) {
  InitializeACLDataChannel(GetParam().first, GetParam().second);

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  StaticByteBuffer kPacket0(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      0x00);

  StaticByteBuffer kPacket1(
      // ACL data header (handle: 1, length 1)
      LowerBits(kConnectionHandle1), UpperBits(kConnectionHandle1),
      // payload length
      0x01, 0x00,
      // payload
      0x01);

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);
  // Create packet to send
  ACLDataPacketPtr out_packet_0 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_0->mutable_view()->mutable_data().Write(kPacket0);
  out_packet_0->InitializeFromBuffer();
  connection_0.QueuePacket(std::move(out_packet_0));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  // Sending a NumberOfCompletedPackets event is necessary because since |kBufferMaxNumPackets| is
  // 2, the controller buffer is full and we won't be able to send any more packets until at least
  // 1 is ACKed by the controller to free up the buffer space
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 1));
  RunLoopUntilIdle();

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);
  // Create packet to send
  ACLDataPacketPtr out_packet_1 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_1->mutable_view()->mutable_data().Write(kPacket1);
  out_packet_1->InitializeFromBuffer();
  connection_1.QueuePacket(std::move(out_packet_1));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 1));
  RunLoopUntilIdle();

  acl_data_channel()->UnregisterConnection(kConnectionHandle0);
  RunLoopUntilIdle();

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);
  // Create packet to send
  out_packet_1 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_1->mutable_view()->mutable_data().Write(kPacket1);
  out_packet_1->InitializeFromBuffer();
  connection_1.QueuePacket(std::move(out_packet_1));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kConnectionHandle1);
}

TEST_P(AclDataChannelBREDRAndBothBuffers,
       RegisterTwoBREDRConnectionsAndClearControllerPacketCountOfFirstConnection) {
  InitializeACLDataChannel(GetParam().first, GetParam().second);

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kACL);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  StaticByteBuffer kPacket0(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      0x00);

  StaticByteBuffer kPacket1(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      0x01);

  StaticByteBuffer kPacket2(
      // ACL data header (handle: 1, length 1)
      LowerBits(kConnectionHandle1), UpperBits(kConnectionHandle1),
      // payload length
      0x01, 0x00,
      // payload
      0x02);

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);
  ACLDataPacketPtr out_packet_0 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_0->mutable_view()->mutable_data().Write(kPacket0);
  out_packet_0->InitializeFromBuffer();
  connection_0.QueuePacket(std::move(out_packet_0));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // The second packet should fill up the controller buffer (kBufferMaxNumPackets)
  ASSERT_EQ(kBufferMaxNumPackets, 2u);
  EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);
  ACLDataPacketPtr out_packet_1 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_1->mutable_view()->mutable_data().Write(kPacket1);
  out_packet_1->InitializeFromBuffer();
  connection_0.QueuePacket(std::move(out_packet_1));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  ACLDataPacketPtr out_packet_2 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_2->mutable_view()->mutable_data().Write(kPacket2);
  out_packet_2->InitializeFromBuffer();
  connection_1.QueuePacket(std::move(out_packet_2));
  RunLoopUntilIdle();

  // |out_packet_2| should not be sent because the controller buffer is full
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);

  // |UnregisterConnection| should not free up any buffer space, so next packet should not be sent
  acl_data_channel()->UnregisterConnection(kConnectionHandle0);
  RunLoopUntilIdle();

  // Clearing the pending packet count for |connection_0| should result in |out_packet_2| being
  // sent
  EXPECT_ACL_PACKET_OUT(test_device(), kPacket2);
  acl_data_channel()->ClearControllerPacketCount(kConnectionHandle0);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // There are no active connections now
  acl_data_channel()->UnregisterConnection(kConnectionHandle1);
  acl_data_channel()->ClearControllerPacketCount(kConnectionHandle1);
  RunLoopUntilIdle();
}

TEST_P(AclDataChannelAllBufferCombinations, RegisterTwoLEConnectionsAndUnregisterFirstConnection) {
  InitializeACLDataChannel(GetParam().first, GetParam().second);

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kLE);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  StaticByteBuffer kPacket0(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      0x00);

  StaticByteBuffer kPacket1(
      // ACL data header (handle: 1, length 1)
      LowerBits(kConnectionHandle1), UpperBits(kConnectionHandle1),
      // payload length
      0x01, 0x00,
      // payload
      0x01);

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);
  // Create packet to send
  ACLDataPacketPtr out_packet_0 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_0->mutable_view()->mutable_data().Write(kPacket0);
  out_packet_0->InitializeFromBuffer();
  connection_0.QueuePacket(std::move(out_packet_0));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  // Sending a NumberOfCompletedPackets event is necessary because since |kBufferMaxNumPackets| is
  // 2, the controller buffer is full and we won't be able to send any more packets until at least
  // 1 is ACKed by the controller to free up the buffer space
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 1));
  RunLoopUntilIdle();

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);
  // Create packet to send
  ACLDataPacketPtr out_packet_1 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_1->mutable_view()->mutable_data().Write(kPacket1);
  out_packet_1->InitializeFromBuffer();
  connection_1.QueuePacket(std::move(out_packet_1));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 1));
  RunLoopUntilIdle();

  acl_data_channel()->UnregisterConnection(kConnectionHandle0);
  RunLoopUntilIdle();

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);
  // Create packet to send
  out_packet_1 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_1->mutable_view()->mutable_data().Write(kPacket1);
  out_packet_1->InitializeFromBuffer();
  connection_1.QueuePacket(std::move(out_packet_1));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kConnectionHandle1);
}

TEST_P(AclDataChannelAllBufferCombinations,
       RegisterTwoLEConnectionsAndClearControllerPacketCountOfFirstConnection) {
  InitializeACLDataChannel(GetParam().first, GetParam().second);

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kLE);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  StaticByteBuffer kPacket0(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      0x00);

  StaticByteBuffer kPacket1(
      // ACL data header (handle: 0, length 1)
      LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
      // payload length
      0x01, 0x00,
      // payload
      0x01);

  StaticByteBuffer kPacket2(
      // ACL data header (handle: 1, length 1)
      LowerBits(kConnectionHandle1), UpperBits(kConnectionHandle1),
      // payload length
      0x01, 0x00,
      // payload
      0x02);

  EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);
  ACLDataPacketPtr out_packet_0 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_0->mutable_view()->mutable_data().Write(kPacket0);
  out_packet_0->InitializeFromBuffer();
  connection_0.QueuePacket(std::move(out_packet_0));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // The second packet should fill up the controller buffer (kBufferMaxNumPackets)
  ASSERT_EQ(kBufferMaxNumPackets, 2u);
  EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);
  ACLDataPacketPtr out_packet_1 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_1->mutable_view()->mutable_data().Write(kPacket1);
  out_packet_1->InitializeFromBuffer();
  connection_0.QueuePacket(std::move(out_packet_1));
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  ACLDataPacketPtr out_packet_2 = ACLDataPacket::New(/*payload_size=*/1);
  out_packet_2->mutable_view()->mutable_data().Write(kPacket2);
  out_packet_2->InitializeFromBuffer();
  connection_1.QueuePacket(std::move(out_packet_2));
  RunLoopUntilIdle();

  // |out_packet_2| should not be sent because the controller buffer is full
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);

  // |UnregisterConnection| should not free up any buffer space, so next packet should not be sent
  acl_data_channel()->UnregisterConnection(kConnectionHandle0);
  RunLoopUntilIdle();

  // Clearing the pending packet count for |connection_0| should result in |out_packet_2| being
  // sent
  EXPECT_ACL_PACKET_OUT(test_device(), kPacket2);
  acl_data_channel()->ClearControllerPacketCount(kConnectionHandle0);
  RunLoopUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // There are no active connections now
  acl_data_channel()->UnregisterConnection(kConnectionHandle1);
  acl_data_channel()->ClearControllerPacketCount(kConnectionHandle1);
  RunLoopUntilIdle();
}

TEST_F(AclDataChannelTest, SendMoreBREDRAndLEPacketsThanMaximumBREDRBufferSpace) {
  constexpr size_t kBufferMaxNumPackets = 5;

  // Only BR/EDR buffer available
  InitializeACLDataChannel(DataBufferInfo(kMaxMtu, kBufferMaxNumPackets), DataBufferInfo());

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  // Queue 12 packets in total, distributed between the two connections
  // Although the LE MTU is zero, we still expect all packets to be sent using the BR/EDR buffer
  // First 5 packets should be sent immediately, and the next 6 should be queued
  for (size_t i = 0; i < 12; ++i) {
    FakeAclConnection* connection = (i % 2) ? &connection_1 : &connection_0;

    const StaticByteBuffer kPacket(
        // ACL data header (handle: 0, length 1)
        LowerBits(connection->handle()), UpperBits(connection->handle()),
        // payload length
        0x01, 0x00,
        // payload
        static_cast<uint8_t>(i));
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket);

    // Create packet to send
    ACLDataPacketPtr packet = ACLDataPacket::New(
        connection->handle(), hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
        hci_spec::ACLBroadcastFlag::kPointToPoint,
        /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    connection->QueuePacket(std::move(packet));
    RunLoopUntilIdle();
  }

  // Since |kBufferMaxNumPackets| is 5, the controller should have received 3 packets on
  // |connection_0| and 2 on |connection_1|
  EXPECT_EQ(connection_0.queued_packets().size(), 3u);
  EXPECT_EQ(connection_1.queued_packets().size(), 4u);
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  // Notify the processed packets with a Number Of Completed Packet HCI event
  // This should cause 5 more packets to be sent
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 3));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 2));
  RunLoopUntilIdle();

  // Since we're alternating between |connection_0| and |connection_1|, the controller should have
  // received 2 more packets on |connection_0| and 3 more packets on |connection_1|
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  // Notify the processed packets with a Number Of Completed Packet HCI event
  // This should cause the remaining 2 packets to be sent
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 2));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 3));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(AclDataChannelTest, SendMoreBREDRAndLEPacketsThanMaximumLEBufferSpace) {
  constexpr size_t kBufferMaxNumPackets = 3;

  // Only LE buffer available
  InitializeACLDataChannel(DataBufferInfo(), DataBufferInfo(kMaxMtu, kBufferMaxNumPackets));

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  // Queue 12 packets in total, distributed between the two connections
  // Since the BR/EDR MTU is zero, we expect to only see LE packets transmitted
  for (size_t i = 0; i < 12; ++i) {
    FakeAclConnection* connection = (i % 2) ? &connection_0 : &connection_1;

    if (i % 2) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
          // payload length
          0x01, 0x00,
          // payload
          static_cast<uint8_t>(i));
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }
    // Create packet to send
    ACLDataPacketPtr packet = ACLDataPacket::New(
        connection->handle(), hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
        hci_spec::ACLBroadcastFlag::kPointToPoint,
        /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    connection->QueuePacket(std::move(packet));
    RunLoopUntilIdle();
  }

  // Since |kBufferMaxNumPackets| is 3 and no BR/EDR packets should have been sent, the controller
  // should have received 3 packets on |connection_0| and none on |connection_1|
  EXPECT_EQ(connection_0.queued_packets().size(), 3u);
  EXPECT_EQ(connection_1.queued_packets().size(), 6u);
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 3));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 6u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(AclDataChannelTest, SendMoreBREDRAndLEPacketsThanMaximumSharedBufferSpace) {
  InitializeACLDataChannel(DataBufferInfo(kMaxMtu, kBufferMaxNumPackets),
                           DataBufferInfo(kMaxMtu, kBufferMaxNumPackets));

  FakeAclConnection connection_0(acl_data_channel(), kConnectionHandle0, bt::LinkType::kLE);
  FakeAclConnection connection_1(acl_data_channel(), kConnectionHandle1, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  // Fill up BR/EDR controller buffer then queue one additional packet
  for (size_t i = 0; i <= kBufferMaxNumPackets; ++i) {
    // Last packet should remain queued
    if (i < kBufferMaxNumPackets) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
          // payload length
          0x01, 0x00,
          // payload
          static_cast<uint8_t>(i));
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }
    // Create packet to send
    ACLDataPacketPtr packet =
        ACLDataPacket::New(kConnectionHandle0, hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    connection_0.QueuePacket(std::move(packet));
    RunLoopUntilIdle();
  }
  EXPECT_EQ(connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Fill up LE controller buffer then queue one additional packet
  for (size_t i = 0; i <= kBufferMaxNumPackets; ++i) {
    // Last packet should remain queued
    if (i < kBufferMaxNumPackets) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
          // payload length
          0x01, 0x00,
          // payload
          static_cast<uint8_t>(i));
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }
    // Create packet to send
    ACLDataPacketPtr packet =
        ACLDataPacket::New(kConnectionHandle0, hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    connection_1.QueuePacket(std::move(packet));
    RunLoopUntilIdle();
  }
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Send out last packet on BR/EDR link
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
                            // payload length
                            0x01, 0x00,
                            // payload
                            static_cast<uint8_t>(kBufferMaxNumPackets)));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle0, 1));
  RunLoopUntilIdle();

  // Send out last packet on LE link
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle0), UpperBits(kConnectionHandle0),
                            // payload length
                            0x01, 0x00,
                            // payload
                            static_cast<uint8_t>(kBufferMaxNumPackets)));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kConnectionHandle1, 1));
  RunLoopUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

INSTANTIATE_TEST_SUITE_P(AclDataChannelTest, AclDataChannelBREDRAndBothBuffers,
                         ::testing::ValuesIn(bredr_both_buffers));

INSTANTIATE_TEST_SUITE_P(AclDataChannelTest, AclDataChannelAllBufferCombinations,
                         ::testing::ValuesIn(all_buffer_options));

}  // namespace
}  // namespace bt::hci
