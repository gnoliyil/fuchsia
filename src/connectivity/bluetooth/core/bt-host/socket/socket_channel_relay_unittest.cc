// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "socket_channel_relay.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_helpers.h"

namespace bt::socket {
namespace {

// We'll test the template just for L2CAP channels.
using RelayT = SocketChannelRelay<l2cap::Channel>;
constexpr size_t kDefaultSocketWriteQueueLimitFrames = 2;

class SocketChannelRelayTest : public ::testing::Test {
 public:
  SocketChannelRelayTest() : loop_(&kAsyncLoopConfigAttachToCurrentThread) {
    EXPECT_EQ(ASYNC_LOOP_RUNNABLE, loop_.GetState());

    constexpr l2cap::ChannelId kDynamicChannelIdMin = 0x0040;
    constexpr l2cap::ChannelId kRemoteChannelId = 0x0050;
    constexpr hci_spec::ConnectionHandle kDefaultConnectionHandle = 0x0001;
    channel_ = std::make_unique<l2cap::testing::FakeChannel>(
        kDynamicChannelIdMin, kRemoteChannelId, kDefaultConnectionHandle, bt::LinkType::kACL);

    const auto socket_status =
        zx::socket::create(ZX_SOCKET_DATAGRAM, &local_socket_, &remote_socket_);
    local_socket_unowned_ = zx::unowned_socket(local_socket_);
    EXPECT_EQ(ZX_OK, socket_status);
  }

  // Writes data on |local_socket| until the socket is full, or an error occurs.
  // Returns the number of bytes written if the socket fills, and zero
  // otherwise.
  [[nodiscard]] size_t StuffSocket() {
    size_t n_total_bytes_written = 0;
    zx_status_t write_res;
    // Fill the socket buffer completely, while minimzing the number of
    // syscalls required.
    for (const auto spam_size_bytes :
         {65536, 32768, 16384, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1}) {
      DynamicByteBuffer spam_data(spam_size_bytes);
      spam_data.Fill(kSpamChar);
      do {
        size_t n_iter_bytes_written = 0;
        write_res = local_socket_unowned_->write(0, spam_data.data(), spam_data.size(),
                                                 &n_iter_bytes_written);
        if (write_res != ZX_OK && write_res != ZX_ERR_SHOULD_WAIT) {
          bt_log(ERROR, "l2cap", "Failure in zx_socket_write(): %s",
                 zx_status_get_string(write_res));
          return 0;
        }
        n_total_bytes_written += n_iter_bytes_written;
      } while (write_res == ZX_OK);
    }
    return n_total_bytes_written;
  }

  // Reads and discards |n_bytes| on |remote_socket|. Returns true if at-least
  // |n_bytes| were successfully discarded. (The actual number of discarded
  // bytes is not known, as a pending datagram may be larger than our read
  // buffer.)
  [[nodiscard]] bool DiscardFromSocket(size_t n_bytes_requested) {
    DynamicByteBuffer received_data(n_bytes_requested);
    zx_status_t read_res;
    size_t n_total_bytes_read = 0;
    while (n_total_bytes_read < n_bytes_requested) {
      size_t n_iter_bytes_read = 0;
      read_res = remote_socket_.read(0, received_data.mutable_data(), received_data.size(),
                                     &n_iter_bytes_read);
      if (read_res != ZX_OK && read_res != ZX_ERR_SHOULD_WAIT) {
        bt_log(ERROR, "l2cap", "Failure in zx_socket_read(): %s", zx_status_get_string(read_res));
        return false;
      }
      if (read_res == ZX_ERR_SHOULD_WAIT) {
        EXPECT_EQ(n_bytes_requested, n_total_bytes_read);
        return false;
      } else {
        n_total_bytes_read += n_iter_bytes_read;
      }
    }
    return true;
  }

 protected:
  static constexpr auto kGoodChar = 'a';
  static constexpr auto kSpamChar = 'b';
  l2cap::testing::FakeChannel* channel() { return channel_.get(); }
  async_dispatcher_t* dispatcher() { return loop_.dispatcher(); }
  zx::socket* local_socket() { return &local_socket_; }
  zx::unowned_socket local_socket_unowned() { return zx::unowned_socket(local_socket_unowned_); }
  zx::socket* remote_socket() { return &remote_socket_; }
  zx::socket ConsumeLocalSocket() { return std::move(local_socket_); }
  void CloseRemoteSocket() { remote_socket_.reset(); }
  // Note: A call to RunLoopOnce() may cause multiple timer-based tasks
  // to be dispatched. (When the timer expires, async_loop_run_once() dispatches
  // all expired timer-based tasks.)
  void RunLoopOnce() { loop_.Run(zx::time::infinite(), /*once=*/true); }
  void RunLoopUntilIdle() { loop_.RunUntilIdle(); }
  void ShutdownLoop() { loop_.Shutdown(); }

 private:
  std::unique_ptr<l2cap::testing::FakeChannel> channel_;
  zx::socket local_socket_;
  zx::socket remote_socket_;
  zx::unowned_socket local_socket_unowned_;
  // TODO(fxbug.dev/716): Move to FakeChannelTest, which incorporates
  // async::TestLoop.
  async::Loop loop_;
};

class SocketChannelRelayLifetimeTest : public SocketChannelRelayTest {
 public:
  SocketChannelRelayLifetimeTest()
      : was_deactivation_callback_invoked_(false),
        relay_(std::make_unique<RelayT>(ConsumeLocalSocket(), channel()->GetWeakPtr(),
                                        [this]() { was_deactivation_callback_invoked_ = true; })) {}

 protected:
  bool was_deactivation_callback_invoked() { return was_deactivation_callback_invoked_; }
  RelayT* relay() {
    BT_DEBUG_ASSERT(relay_);
    return relay_.get();
  }
  void DestroyRelay() { relay_ = nullptr; }

 private:
  bool was_deactivation_callback_invoked_;
  std::unique_ptr<RelayT> relay_;
};

TEST_F(SocketChannelRelayLifetimeTest, ActivateFailsIfGivenStoppedDispatcher) {
  ShutdownLoop();
  EXPECT_FALSE(relay()->Activate());
}

TEST_F(SocketChannelRelayLifetimeTest, ActivateDoesNotInvokeDeactivationCallbackOnSuccess) {
  ASSERT_TRUE(relay()->Activate());
  EXPECT_FALSE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, ActivateDoesNotInvokeDeactivationCallbackOnFailure) {
  ShutdownLoop();
  ASSERT_FALSE(relay()->Activate());
  EXPECT_FALSE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, SocketIsClosedWhenRelayIsDestroyed) {
  const char data = kGoodChar;
  ASSERT_EQ(ZX_OK, remote_socket()->write(0, &data, sizeof(data), nullptr));
  DestroyRelay();
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, remote_socket()->write(0, &data, sizeof(data), nullptr));
}

TEST_F(SocketChannelRelayLifetimeTest, RelayIsDeactivatedWhenDispatcherIsShutDown) {
  ASSERT_TRUE(relay()->Activate());

  ShutdownLoop();
  EXPECT_TRUE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, RelayActivationFailsIfChannelActivationFails) {
  channel()->set_activate_fails(true);
  EXPECT_FALSE(relay()->Activate());
}

TEST_F(SocketChannelRelayLifetimeTest, DestructionWithPendingSdusFromChannelDoesNotCrash) {
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(StaticByteBuffer('h', 'e', 'l', 'l', 'o'));
  DestroyRelay();
  RunLoopUntilIdle();
}

TEST_F(SocketChannelRelayLifetimeTest, RelayIsDeactivatedWhenChannelIsClosed) {
  ASSERT_TRUE(relay()->Activate());

  channel()->Close();
  EXPECT_TRUE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, RelayIsDeactivatedWhenRemoteSocketIsClosed) {
  ASSERT_TRUE(relay()->Activate());

  CloseRemoteSocket();
  RunLoopUntilIdle();
  EXPECT_TRUE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest,
       RelayIsDeactivatedWhenRemoteSocketIsClosedEvenWithPendingSocketData) {
  ASSERT_TRUE(relay()->Activate());
  ASSERT_TRUE(StuffSocket());

  channel()->Receive(StaticByteBuffer('h', 'e', 'l', 'l', 'o'));
  RunLoopUntilIdle();
  ASSERT_FALSE(was_deactivation_callback_invoked());

  CloseRemoteSocket();
  RunLoopUntilIdle();
  EXPECT_TRUE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, OversizedDatagramDeactivatesRelay) {
  const size_t kMessageBufSize = channel()->max_tx_sdu_size() * 5;
  DynamicByteBuffer large_message(kMessageBufSize);
  large_message.Fill('a');
  ASSERT_TRUE(relay()->Activate());

  size_t n_bytes_written_to_socket = 0;
  const auto write_res = remote_socket()->write(0, large_message.data(), large_message.size(),
                                                &n_bytes_written_to_socket);
  ASSERT_EQ(ZX_OK, write_res);
  ASSERT_EQ(large_message.size(), n_bytes_written_to_socket);
  RunLoopUntilIdle();

  EXPECT_TRUE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, SocketClosureAfterChannelClosureDoesNotHangOrCrash) {
  ASSERT_TRUE(relay()->Activate());
  channel()->Close();
  ASSERT_TRUE(was_deactivation_callback_invoked());

  CloseRemoteSocket();
  RunLoopUntilIdle();
}

TEST_F(SocketChannelRelayLifetimeTest, ChannelClosureAfterSocketClosureDoesNotHangOrCrash) {
  ASSERT_TRUE(relay()->Activate());
  CloseRemoteSocket();
  RunLoopUntilIdle();

  channel()->Close();
  ASSERT_TRUE(was_deactivation_callback_invoked());
}

TEST_F(SocketChannelRelayLifetimeTest, DeactivationClosesSocket) {
  ASSERT_TRUE(relay()->Activate());
  channel()->Close();  // Triggers relay deactivation.

  const char data = kGoodChar;
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, remote_socket()->write(0, &data, sizeof(data), nullptr));
}

class SocketChannelRelayDataPathTest : public SocketChannelRelayTest {
 public:
  SocketChannelRelayDataPathTest()
      : relay_(ConsumeLocalSocket(), channel()->GetWeakPtr(), /*deactivation_cb=*/nullptr,
               kDefaultSocketWriteQueueLimitFrames) {
    channel()->SetSendCallback([&](auto data) { sent_to_channel_.push_back(std::move(data)); },
                               dispatcher());
  }

 protected:
  RelayT* relay() { return &relay_; }
  auto& sent_to_channel() { return sent_to_channel_; }

 private:
  RelayT relay_;
  std::vector<ByteBufferPtr> sent_to_channel_;
};

// Fixture for tests which exercise the datapath from the controller.
class SocketChannelRelayRxTest : public SocketChannelRelayDataPathTest {
 protected:
  DynamicByteBuffer ReadDatagramFromSocket(const size_t dgram_len) {
    DynamicByteBuffer socket_read_buffer(dgram_len + 1);  // +1 to detect trailing garbage.
    size_t n_bytes_read = 0;
    const auto read_res = remote_socket()->read(0, socket_read_buffer.mutable_data(),
                                                socket_read_buffer.size(), &n_bytes_read);
    if (read_res != ZX_OK) {
      bt_log(ERROR, "l2cap", "Failure in zx_socket_read(): %s", zx_status_get_string(read_res));
      return {};
    }
    return DynamicByteBuffer(BufferView(socket_read_buffer, n_bytes_read));
  }
};

TEST_F(SocketChannelRelayRxTest, MessageFromChannelIsCopiedToSocketSynchronously) {
  const StaticByteBuffer kExpectedMessage('h', 'e', 'l', 'l', 'o');
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(kExpectedMessage);
  // The data should be copied synchronously, so the async loop should not be run here.
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, ReadDatagramFromSocket(kExpectedMessage.size())));
}

TEST_F(SocketChannelRelayRxTest, MultipleSdusFromChannelAreCopiedToSocketPreservingSduBoundaries) {
  const StaticByteBuffer kExpectedMessage1('h', 'e', 'l', 'l', 'o');
  const StaticByteBuffer kExpectedMessage2('g', 'o', 'o', 'd', 'b', 'y', 'e');
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(kExpectedMessage1);
  channel()->Receive(kExpectedMessage2);
  RunLoopUntilIdle();

  EXPECT_TRUE(ContainersEqual(kExpectedMessage1, ReadDatagramFromSocket(kExpectedMessage1.size())));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage2, ReadDatagramFromSocket(kExpectedMessage2.size())));
}

TEST_F(SocketChannelRelayRxTest, SduFromChannelIsCopiedToSocketWhenSocketUnblocks) {
  size_t n_junk_bytes = StuffSocket();
  ASSERT_TRUE(n_junk_bytes);

  const StaticByteBuffer kExpectedMessage('h', 'e', 'l', 'l', 'o');
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(kExpectedMessage);
  RunLoopUntilIdle();

  ASSERT_TRUE(DiscardFromSocket(n_junk_bytes));
  RunLoopUntilIdle();
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, ReadDatagramFromSocket(kExpectedMessage.size())));
}

TEST_F(SocketChannelRelayRxTest, CanQueueAndWriteMultipleSDUs) {
  size_t n_junk_bytes = StuffSocket();
  ASSERT_TRUE(n_junk_bytes);

  const StaticByteBuffer kExpectedMessage1('h', 'e', 'l', 'l', 'o');
  const StaticByteBuffer kExpectedMessage2('g', 'o', 'o', 'd', 'b', 'y', 'e');
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(kExpectedMessage1);
  channel()->Receive(kExpectedMessage2);
  RunLoopUntilIdle();

  ASSERT_TRUE(DiscardFromSocket(n_junk_bytes));
  // Run only one task. This verifies that the relay writes both pending SDUs in
  // one shot, rather than re-arming the async::Wait for each SDU.
  RunLoopOnce();

  EXPECT_TRUE(ContainersEqual(kExpectedMessage1, ReadDatagramFromSocket(kExpectedMessage1.size())));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage2, ReadDatagramFromSocket(kExpectedMessage2.size())));
}

TEST_F(SocketChannelRelayRxTest, CanQueueAndIncrementallyWriteMultipleSDUs) {
  // Find the socket buffer size.
  const size_t socket_buffer_size = StuffSocket();
  ASSERT_TRUE(DiscardFromSocket(socket_buffer_size));

  // Stuff the socket manually, rather than using StuffSocket(), so that we know
  // exactly how much buffer space we free, as we read datagrams out of
  // |remote_socket()|.
  constexpr size_t kLargeSduSize = 1023;
  zx_status_t write_res = ZX_ERR_INTERNAL;
  DynamicByteBuffer spam_sdu(kLargeSduSize);
  size_t n_junk_bytes = 0;
  size_t n_junk_datagrams = 0;
  spam_sdu.Fill('s');
  do {
    size_t n_iter_bytes_written = 0;
    write_res =
        local_socket_unowned()->write(0, spam_sdu.data(), spam_sdu.size(), &n_iter_bytes_written);
    ASSERT_TRUE(write_res == ZX_OK || write_res == ZX_ERR_SHOULD_WAIT)
        << "Failure in zx_socket_write: " << zx_status_get_string(write_res);
    if (write_res == ZX_OK) {
      ASSERT_EQ(spam_sdu.size(), n_iter_bytes_written);
      n_junk_bytes += spam_sdu.size();
      n_junk_datagrams += 1;
    }
  } while (write_res == ZX_OK);
  ASSERT_NE(socket_buffer_size, n_junk_bytes) << "Need non-zero free space in socket buffer.";

  DynamicByteBuffer hello_sdu(kLargeSduSize);
  DynamicByteBuffer goodbye_sdu(kLargeSduSize);
  hello_sdu.Fill('h');
  goodbye_sdu.Fill('g');
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(hello_sdu);
  channel()->Receive(goodbye_sdu);
  RunLoopUntilIdle();

  // Free up space for just the first SDU.
  ASSERT_TRUE(ContainersEqual(spam_sdu, ReadDatagramFromSocket(spam_sdu.size())));
  n_junk_datagrams -= 1;
  RunLoopUntilIdle();

  // Free up space for just the second SDU.
  ASSERT_TRUE(ContainersEqual(spam_sdu, ReadDatagramFromSocket(spam_sdu.size())));
  n_junk_datagrams -= 1;
  RunLoopUntilIdle();

  // Discard spam.
  while (n_junk_datagrams) {
    ASSERT_TRUE(ContainersEqual(spam_sdu, ReadDatagramFromSocket(spam_sdu.size())));
    n_junk_datagrams -= 1;
  }

  // Read out our expected datagrams, verifying that boundaries are preserved.
  EXPECT_TRUE(ContainersEqual(hello_sdu, ReadDatagramFromSocket(hello_sdu.size())));
  EXPECT_TRUE(ContainersEqual(goodbye_sdu, ReadDatagramFromSocket(goodbye_sdu.size())));
  EXPECT_EQ(0u, ReadDatagramFromSocket(1u).size()) << "Found unexpected datagram";
}

TEST_F(SocketChannelRelayRxTest, ZeroByteSDUsDropped) {
  const StaticByteBuffer kMessage1('h', 'e', 'l', 'l', 'o');
  DynamicByteBuffer kMessageZero(0);
  const StaticByteBuffer kMessage3('f', 'u', 'c', 'h', 's', 'i', 'a');

  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(kMessageZero);
  channel()->Receive(kMessage1);
  channel()->Receive(kMessageZero);
  channel()->Receive(kMessage3);
  channel()->Receive(kMessageZero);
  RunLoopUntilIdle();

  ASSERT_TRUE(ContainersEqual(kMessage1, ReadDatagramFromSocket(kMessage1.size())));
  ASSERT_TRUE(ContainersEqual(kMessage3, ReadDatagramFromSocket(kMessage3.size())));

  EXPECT_EQ(0u, ReadDatagramFromSocket(1u).size()) << "Found unexpected datagram";
}

TEST_F(SocketChannelRelayRxTest, OldestSDUIsDroppedOnOverflow) {
  size_t n_junk_bytes = StuffSocket();
  ASSERT_TRUE(n_junk_bytes);

  const StaticByteBuffer kSentMessage1(1);
  const StaticByteBuffer kSentMessage2(2);
  const StaticByteBuffer kSentMessage3(3);
  ASSERT_TRUE(relay()->Activate());
  channel()->Receive(kSentMessage1);
  channel()->Receive(kSentMessage2);
  channel()->Receive(kSentMessage3);
  RunLoopUntilIdle();

  ASSERT_TRUE(DiscardFromSocket(n_junk_bytes));
  RunLoopUntilIdle();

  EXPECT_TRUE(ContainersEqual(kSentMessage2, ReadDatagramFromSocket(kSentMessage2.size())));
  EXPECT_TRUE(ContainersEqual(kSentMessage3, ReadDatagramFromSocket(kSentMessage3.size())));
}

TEST_F(SocketChannelRelayRxTest, SdusReceivedBeforeChannelActivationAreCopiedToSocket) {
  const StaticByteBuffer kExpectedMessage1('h', 'e', 'l', 'l', 'o');
  const StaticByteBuffer kExpectedMessage2('g', 'o', 'o', 'd', 'b', 'y', 'e');
  channel()->Receive(kExpectedMessage1);
  channel()->Receive(kExpectedMessage2);
  ASSERT_TRUE(relay()->Activate());
  // Note: we omit RunLoopOnce()/RunLoopUntilIdle(), as Channel activation
  // delivers the messages synchronously.

  EXPECT_TRUE(ContainersEqual(kExpectedMessage1, ReadDatagramFromSocket(kExpectedMessage1.size())));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage2, ReadDatagramFromSocket(kExpectedMessage2.size())));
}

TEST_F(SocketChannelRelayRxTest, SdusPendingAtChannelClosureAreCopiedToSocket) {
  ASSERT_TRUE(StuffSocket());
  ASSERT_TRUE(relay()->Activate());

  const StaticByteBuffer kExpectedMessage1('h');
  const StaticByteBuffer kExpectedMessage2('i');
  channel()->Receive(kExpectedMessage1);
  channel()->Receive(kExpectedMessage2);
  RunLoopUntilIdle();

  // Discard two datagrams from socket, to make room for our SDUs to be copied
  // over.
  ASSERT_NE(0u, ReadDatagramFromSocket(1u).size());
  ASSERT_NE(0u, ReadDatagramFromSocket(1u).size());
  channel()->Close();

  // Read past all of the spam from StuffSocket().
  DynamicByteBuffer dgram;
  do {
    dgram = ReadDatagramFromSocket(1u);
  } while (dgram.size() && dgram[0] == kSpamChar);

  // First non-spam message should be kExpectedMessage1, and second should be
  // kExpectedMessage2.
  EXPECT_TRUE(ContainersEqual(kExpectedMessage1, dgram));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage2, ReadDatagramFromSocket(1u)));
}

TEST_F(SocketChannelRelayRxTest,
       ReceivingFromChannelBetweenSocketCloseAndCloseWaitTriggerDoesNotCrash) {
  // Note: we call Channel::Receive() first, to force FakeChannel to deliver the
  // SDU synchronously to the SocketChannelRelay. Asynchronous delivery could
  // compromise the test's validity, since that would allow OnSocketClosed() to
  // be invoked before OnChannelDataReceived().
  channel()->Receive(StaticByteBuffer(kGoodChar));
  CloseRemoteSocket();
  ASSERT_TRUE(relay()->Activate());
}

TEST_F(SocketChannelRelayRxTest,
       SocketCloseBetweenReceivingFromChannelAndSocketWritabilityDoesNotCrashOrHang) {
  ASSERT_TRUE(relay()->Activate());

  size_t n_junk_bytes = StuffSocket();
  ASSERT_TRUE(n_junk_bytes);
  channel()->Receive(StaticByteBuffer(kGoodChar));
  RunLoopUntilIdle();

  ASSERT_TRUE(DiscardFromSocket(n_junk_bytes));
  CloseRemoteSocket();
  RunLoopUntilIdle();
}

TEST_F(SocketChannelRelayRxTest, NoDataFromChannelIsWrittenToSocketAfterDeactivation) {
  ASSERT_TRUE(relay()->Activate());

  size_t n_junk_bytes = StuffSocket();
  ASSERT_TRUE(n_junk_bytes);

  channel()->Receive(StaticByteBuffer('h', 'e', 'l', 'l', 'o'));
  RunLoopUntilIdle();

  channel()->Close();  // Triggers relay deactivation.
  ASSERT_TRUE(DiscardFromSocket(n_junk_bytes));
  RunLoopUntilIdle();

  zx_info_socket_t info = {};
  info.rx_buf_available = std::numeric_limits<size_t>::max();
  const auto status = remote_socket()->get_info(ZX_INFO_SOCKET, &info, sizeof(info),
                                                /*actual_count=*/nullptr, /*avail_count=*/nullptr);
  EXPECT_EQ(ZX_OK, status);
  EXPECT_EQ(0u, info.rx_buf_available);
}

// Alias for the fixture for tests which exercise the datapath to the
// controller.
using SocketChannelRelayTxTest = SocketChannelRelayDataPathTest;

TEST_F(SocketChannelRelayTxTest, SduFromSocketIsCopiedToChannel) {
  const StaticByteBuffer kExpectedMessage('h', 'e', 'l', 'l', 'o');
  ASSERT_TRUE(relay()->Activate());

  size_t n_bytes_written = 0;
  const auto write_res =
      remote_socket()->write(0, kExpectedMessage.data(), kExpectedMessage.size(), &n_bytes_written);
  ASSERT_EQ(ZX_OK, write_res);
  ASSERT_EQ(kExpectedMessage.size(), n_bytes_written);
  RunLoopUntilIdle();

  const auto& sdus = sent_to_channel();
  ASSERT_FALSE(sdus.empty());
  EXPECT_EQ(1u, sdus.size());
  ASSERT_TRUE(sdus[0]);
  EXPECT_EQ(kExpectedMessage.size(), sdus[0]->size());
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[0]));
}

TEST_F(SocketChannelRelayTxTest, MultipleSdusFromSocketAreCopiedToChannel) {
  const StaticByteBuffer kExpectedMessage('h', 'e', 'l', 'l', 'o');
  const size_t kNumMessages = 3;
  ASSERT_TRUE(relay()->Activate());

  for (size_t i = 0; i < kNumMessages; ++i) {
    size_t n_bytes_written = 0;
    const auto write_res = remote_socket()->write(0, kExpectedMessage.data(),
                                                  kExpectedMessage.size(), &n_bytes_written);
    ASSERT_EQ(ZX_OK, write_res);
    ASSERT_EQ(kExpectedMessage.size(), n_bytes_written);
    RunLoopUntilIdle();
  }

  const auto& sdus = sent_to_channel();
  ASSERT_FALSE(sdus.empty());
  ASSERT_EQ(3U, sdus.size());
  ASSERT_TRUE(sdus[0]);
  ASSERT_TRUE(sdus[1]);
  ASSERT_TRUE(sdus[2]);
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[0]));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[1]));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[2]));
}

TEST_F(SocketChannelRelayTxTest, MultipleSdusAreCopiedToChannelInOneRelayTask) {
  const StaticByteBuffer kExpectedMessage('h', 'e', 'l', 'l', 'o');
  const size_t kNumMessages = 3;
  ASSERT_TRUE(relay()->Activate());

  for (size_t i = 0; i < kNumMessages; ++i) {
    size_t n_bytes_written = 0;
    const auto write_res = remote_socket()->write(0, kExpectedMessage.data(),
                                                  kExpectedMessage.size(), &n_bytes_written);
    ASSERT_EQ(ZX_OK, write_res);
    ASSERT_EQ(kExpectedMessage.size(), n_bytes_written);
  }

  RunLoopOnce();  // Runs SocketChannelRelay::OnSocketReadable().
  RunLoopOnce();  // Runs all tasks queued by FakeChannel::Send().

  const auto& sdus = sent_to_channel();
  ASSERT_FALSE(sdus.empty());
  ASSERT_EQ(3U, sdus.size());
  ASSERT_TRUE(sdus[0]);
  ASSERT_TRUE(sdus[1]);
  ASSERT_TRUE(sdus[2]);
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[0]));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[1]));
  EXPECT_TRUE(ContainersEqual(kExpectedMessage, *sdus[2]));
}

TEST_F(SocketChannelRelayTxTest, OversizedSduIsDropped) {
  const size_t kMessageBufSize = channel()->max_tx_sdu_size() * 5;
  DynamicByteBuffer large_message(kMessageBufSize);
  large_message.Fill(kGoodChar);
  ASSERT_TRUE(relay()->Activate());

  size_t n_bytes_written_to_socket = 0;
  const auto write_res = remote_socket()->write(0, large_message.data(), large_message.size(),
                                                &n_bytes_written_to_socket);
  ASSERT_EQ(ZX_OK, write_res);
  ASSERT_EQ(large_message.size(), n_bytes_written_to_socket);
  RunLoopUntilIdle();

  ASSERT_TRUE(sent_to_channel().empty());
}

TEST_F(SocketChannelRelayTxTest, ValidSduAfterOversizedSduIsIgnored) {
  const StaticByteBuffer kSentMsg('h', 'e', 'l', 'l', 'o');
  ASSERT_TRUE(relay()->Activate());

  {
    DynamicByteBuffer dropped_msg(channel()->max_tx_sdu_size() + 1);
    size_t n_bytes_written = 0;
    zx_status_t write_res = ZX_ERR_INTERNAL;
    dropped_msg.Fill(kGoodChar);
    write_res = remote_socket()->write(0, dropped_msg.data(), dropped_msg.size(), &n_bytes_written);
    ASSERT_EQ(ZX_OK, write_res);
    ASSERT_EQ(dropped_msg.size(), n_bytes_written);
  }

  {
    size_t n_bytes_written = 0;
    zx_status_t write_res = ZX_ERR_INTERNAL;
    write_res = remote_socket()->write(0, kSentMsg.data(), kSentMsg.size(), &n_bytes_written);
    ASSERT_EQ(ZX_OK, write_res);
    ASSERT_EQ(kSentMsg.size(), n_bytes_written);
  }

  RunLoopUntilIdle();
  EXPECT_TRUE(sent_to_channel().empty());
}

TEST_F(SocketChannelRelayTxTest, NewSocketDataAfterChannelClosureIsNotSentToChannel) {
  ASSERT_TRUE(relay()->Activate());
  channel()->Close();

  const char data = kGoodChar;
  const auto write_res = remote_socket()->write(0, &data, sizeof(data), /*actual=*/nullptr);
  ASSERT_TRUE(write_res == ZX_OK || write_res == ZX_ERR_PEER_CLOSED)
      << ": " << zx_status_get_string(write_res);
  RunLoopUntilIdle();
  EXPECT_TRUE(sent_to_channel().empty());
}

TEST_F(SocketChannelRelayRxTest, DrainWriteQueueWhileSocketWritableWaitIsPending) {
  ASSERT_EQ(kDefaultSocketWriteQueueLimitFrames, 2u);
  const StaticByteBuffer kSentMessage0(0);
  const StaticByteBuffer kSentMessage1(1, 1, 1);
  const StaticByteBuffer kSentMessage2(2);
  const StaticByteBuffer kSentMessage3(3);

  // Make the first 2 datagrams 1 byte each so that we can discard exactly 2 bytes after stuffing
  // the socket.
  size_t n_bytes_written = 0;
  zx_status_t write_res = local_socket_unowned()->write(0, kSentMessage0.data(),
                                                        kSentMessage0.size(), &n_bytes_written);
  ASSERT_EQ(write_res, ZX_OK);
  ASSERT_EQ(n_bytes_written, 1u);
  write_res = local_socket_unowned()->write(0, kSentMessage0.data(), kSentMessage0.size(),
                                            &n_bytes_written);
  ASSERT_EQ(write_res, ZX_OK);
  ASSERT_EQ(n_bytes_written, 1u);

  size_t n_junk_bytes = StuffSocket();
  ASSERT_TRUE(n_junk_bytes);

  ASSERT_TRUE(relay()->Activate());

  // Drain enough space for kSentMessage2 and kSentMessage3, but not kSentMessage1!
  ASSERT_TRUE(DiscardFromSocket(2 * kSentMessage0.size()));

  // The kSentMessage1 is too big to write to the almost-stuffed socket. The packet will be queued,
  // and the "socket writable" signal wait will start.
  channel()->Receive(kSentMessage1);
  // The second 1-byte packet that will fit into the socket is also queued.
  channel()->Receive(kSentMessage2);
  // When the third 1-byte packet is queued, kSentMessage1 will be dropped from the queue, allowing
  // kSentMessage2 and kSendMessage3 to be written to the socket. The queue will be emptied.
  channel()->Receive(kSentMessage3);

  // Allow any signals to be processed.
  RunLoopUntilIdle();

  // Verify that kSentMessage2 and kSentMessage3 were actually written to the socket.
  ASSERT_TRUE(DiscardFromSocket(n_junk_bytes));
  RunLoopUntilIdle();
  EXPECT_TRUE(ContainersEqual(kSentMessage2, ReadDatagramFromSocket(kSentMessage2.size())));
  EXPECT_TRUE(ContainersEqual(kSentMessage3, ReadDatagramFromSocket(kSentMessage3.size())));
}

}  // namespace
}  // namespace bt::socket
