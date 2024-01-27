// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_MOCK_CHANNEL_TEST_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_MOCK_CHANNEL_TEST_H_

#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_channel.h"
#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

namespace bt::l2cap::testing {

// Provides a GTest harness base class for tests that operate over an L2CAP channel. Outgoing packet
// expectations are written using the EXPECT_PACKET_OUT() macro, and may specify 0 or more responses
// that should be sent when the outgoing packet is received by MockChannelTest:
//
// StaticByteBuffer kRequest_0(...);
// StaticByteBuffer kRequest_1(...);
// ...
// EXPECT_PACKET_OUT(kRequest_0);
// EXPECT_PACKET_OUT(kRequest_1, kResponse_0, kResponse_1);
// foo.Start();
// EXPECT_TRUE(AllExpectedPacketsSent());
// fake_chan()->Receive(kRequest_2); // Simulate inbound packet
class MockChannelTest : public ::gtest::TestLoopFixture {
 public:
  struct ExpectationMetadata {
    const char* file;
    int line;
    const char* expectation;
  };

  struct PacketExpectation {
    DynamicByteBuffer data;
    ExpectationMetadata meta;
  };

  class Transaction {
   public:
    // The |expected| buffer and the buffers in |replies| will be copied, so their lifetime does not
    // need to extend past Transaction construction.
    Transaction(const ByteBuffer& expected, const std::vector<const ByteBuffer*>& replies,
                ExpectationMetadata meta);
    virtual ~Transaction() = default;
    Transaction(Transaction&& other) = default;
    Transaction& operator=(Transaction&& other) = default;

    // Returns true if the transaction matches the given packet.
    bool Match(const ByteBuffer& packet);

    const PacketExpectation& expected() { return expected_; }
    void set_expected(const PacketExpectation& expected) {
      expected_ =
          PacketExpectation{.data = DynamicByteBuffer(expected.data), .meta = expected.meta};
    }

    std::queue<DynamicByteBuffer>& replies() { return replies_; }

   private:
    PacketExpectation expected_;
    std::queue<DynamicByteBuffer> replies_;
  };

  struct ChannelOptions {
    explicit ChannelOptions(ChannelId id, uint16_t mtu = kDefaultMTU)
        : ChannelOptions(id, id, mtu) {}
    ChannelOptions(ChannelId id, ChannelId remote_id, uint16_t mtu)
        : id(id), remote_id(remote_id), mtu(mtu) {}

    ChannelId id;
    ChannelId remote_id;
    uint16_t mtu;
    hci_spec::ConnectionHandle conn_handle = 0x0001;
    bt::LinkType link_type = bt::LinkType::kLE;
  };

  MockChannelTest() = default;
  ~MockChannelTest() override = default;

  void TearDown() override;

  // Queues a transaction into the MockChannelTest's expected packet queue. Each packet received
  // through the channel will be verified against the next expected transaction in the queue. A
  // mismatch will cause a fatal assertion. On a match, MockChannelTest will send back the replies
  // provided in the transaction.
  // NOTE: It is recommended to use the EXPECT_PACKET_OUT macro instead of calling this method
  // directly.
  void QueueTransaction(const ByteBuffer& expected, const std::vector<const ByteBuffer*>& replies,
                        ExpectationMetadata meta);

  // Create a FakeChannel owned by this test fixture. Replaces any existing channel.
  FakeChannel::WeakPtr CreateFakeChannel(const ChannelOptions& options);

  using PacketCallback = fit::function<void(const ByteBuffer& packet)>;
  void SetPacketCallback(PacketCallback callback) { packet_callback_ = std::move(callback); }

  bool AllExpectedPacketsSent() const { return transactions_.empty(); }

  FakeChannel::WeakPtr fake_chan() const { return fake_chan_->AsWeakPtr(); }

 private:
  void OnPacketSent(std::unique_ptr<ByteBuffer> packet);

  std::queue<Transaction> transactions_;
  std::unique_ptr<FakeChannel> fake_chan_;
  PacketCallback packet_callback_;
};

// Helper macro for expecting a packet and receiving a variable number of responses.
#define EXPECT_PACKET_OUT(expected, ...) \
  QueueTransaction(                      \
      (expected), {__VA_ARGS__},         \
      bt::l2cap::testing::MockChannelTest::ExpectationMetadata{__FILE__, __LINE__, #expected})

}  // namespace bt::l2cap::testing

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_L2CAP_MOCK_CHANNEL_TEST_H_
