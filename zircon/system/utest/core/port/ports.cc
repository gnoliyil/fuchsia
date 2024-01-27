// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/clock.h>
#include <lib/zx/event.h>
#include <lib/zx/job.h>
#include <lib/zx/port.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmar.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>
#include <zircon/types.h>

#include <atomic>
#include <cstdio>
#include <iterator>
#include <string>
#include <thread>

#include <fbl/algorithm.h>
#include <mini-process/mini-process.h>
#include <zxtest/zxtest.h>

namespace {

TEST(PortTest, QueueNullPtrReturnsInvalidArgs) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  EXPECT_EQ(port.queue(nullptr), ZX_ERR_INVALID_ARGS);
}

TEST(PortTest, QueueWaitVerifyUserPacket) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  constexpr zx_port_packet_t kPortUserPacket = {
      12ull,
      ZX_PKT_TYPE_USER + 5u,  // kernel overrides the |type|.
      -3,
      {{}}};

  zx_port_packet_t out = {};

  ASSERT_OK(port.queue(&kPortUserPacket));

  ASSERT_OK(port.wait(zx::time::infinite(), &out));

  EXPECT_EQ(out.key, 12u);
  EXPECT_EQ(out.type, ZX_PKT_TYPE_USER);
  EXPECT_EQ(out.status, -3);

  EXPECT_EQ(memcmp(&kPortUserPacket.user, &out.user, sizeof(zx_port_packet_t::user)), 0);
}

TEST(PortTest, PortTimeout) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  zx_port_packet_t packet = {};

  EXPECT_EQ(port.wait(zx::deadline_after(zx::nsec(1)), &packet), ZX_ERR_TIMED_OUT);
}

TEST(PortTest, QueueAndClose) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  constexpr zx_port_packet_t kPortUserPacket = {1ull, ZX_PKT_TYPE_USER, 0, {{}}};

  EXPECT_OK(port.queue(&kPortUserPacket));
}

TEST(PortTest, AsyncWaitChannelTimedOut) {
  constexpr uint64_t kEventKey = 6567;

  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  zx::channel ch[2];
  ASSERT_OK(zx::channel::create(0u, &ch[0], &ch[1]));

  zx_port_packet_t out = {};
  ASSERT_OK(ch[1].wait_async(port, kEventKey, ZX_CHANNEL_READABLE, 0));

  EXPECT_EQ(port.wait(zx::deadline_after(zx::usec(200)), &out), ZX_ERR_TIMED_OUT);
}

TEST(PortTest, AsyncWaitChannel) {
  constexpr uint64_t kEventKey = 6567;

  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  zx::channel ch[2];
  ASSERT_OK(zx::channel::create(0u, &ch[0], &ch[1]));

  zx_port_packet_t out = {};
  ASSERT_OK(ch[1].wait_async(port, kEventKey, ZX_CHANNEL_READABLE, 0));

  EXPECT_EQ(port.wait(zx::deadline_after(zx::usec(200)), &out), ZX_ERR_TIMED_OUT);

  EXPECT_OK(ch[0].write(0u, "here", 4, nullptr, 0u));

  EXPECT_OK(port.wait(zx::time::infinite(), &out));

  EXPECT_EQ(out.key, kEventKey);
  EXPECT_EQ(out.type, ZX_PKT_TYPE_SIGNAL_ONE);
  EXPECT_EQ(out.signal.observed, ZX_CHANNEL_WRITABLE | ZX_CHANNEL_READABLE);
  EXPECT_EQ(out.signal.trigger, ZX_CHANNEL_READABLE);
  EXPECT_EQ(out.signal.count, 1u);

  EXPECT_EQ(ch[1].read(ZX_CHANNEL_READ_MAY_DISCARD, nullptr, nullptr, 0u, 0, nullptr, nullptr),
            ZX_ERR_BUFFER_TOO_SMALL);

  zx_port_packet_t out1 = {};
  EXPECT_EQ(port.wait(zx::deadline_after(zx::usec(200)), &out1), ZX_ERR_TIMED_OUT);

  EXPECT_OK(ch[1].wait_async(port, kEventKey, ZX_CHANNEL_READABLE, 0));
}

// What matters here is not so much the return values, but that the system doesn't
// crash as a result of the order. Refer to the diagram at the top of port_dispatcher.h.
TEST(PortTest, AsyncWaitCloseOrder) {
  constexpr uint64_t kEventKey = 1122;

  enum Handle { ChannelB, ChannelA, Port };
  struct CloseOrder {
    Handle first;
    Handle second;
    Handle third;
    const std::string close_list;
  };

  CloseOrder close_order_list[] = {{ChannelB, ChannelA, Port, "ChannelB, ChannelA, Port"},
                                   {ChannelB, Port, ChannelA, "ChannelB, Port, ChannelA"},
                                   {ChannelA, Port, ChannelB, "ChannelA, Port, ChannelB"},
                                   {ChannelA, ChannelB, Port, "ChannelA, ChannelB, Port"},
                                   {Port, ChannelA, ChannelB, "Port, ChannelA, ChannelB"},
                                   {Port, ChannelB, ChannelA, "Port, ChannelB, ChannelA"}};

  for (auto& a : close_order_list) {
    zx_handle_t handle[3];
    EXPECT_OK(zx_port_create(0, &handle[Port]), "%s", a.close_list.c_str());

    EXPECT_OK(zx_channel_create(0u, &handle[ChannelA], &handle[ChannelB]), "%s",
              a.close_list.c_str());

    EXPECT_OK(zx_object_wait_async(handle[ChannelB], handle[Port], kEventKey,
                                   ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, 0),
              "%s", a.close_list.c_str());

    EXPECT_OK(zx_handle_close(handle[a.first]), "%s", a.close_list.c_str());

    EXPECT_OK(zx_handle_close(handle[a.second]), "%s", a.close_list.c_str());

    EXPECT_OK(zx_handle_close(handle[a.third]), "%s", a.close_list.c_str());
  }
}

TEST(PortTest, EventAsyncSignalWaitSingle) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  zx::event event;
  ASSERT_OK(zx::event::create(0u, &event));

  constexpr uint32_t kNumAwaits = 7;

  for (uint32_t ix = 0; ix != kNumAwaits; ++ix) {
    ASSERT_OK(event.wait_async(port, ix, ZX_EVENT_SIGNALED, 0));
  }

  EXPECT_OK(event.signal(0u, ZX_EVENT_SIGNALED));

  zx_port_packet_t out = {};
  uint64_t key_sum = 0;

  for (uint32_t ix = 0; ix != (kNumAwaits - 2); ++ix) {
    EXPECT_OK(port.wait(zx::time::infinite(), &out));
    key_sum += out.key;
    EXPECT_EQ(out.type, ZX_PKT_TYPE_SIGNAL_ONE);
    EXPECT_EQ(out.signal.count, 1u);
  }

  EXPECT_EQ(key_sum, 20u);
}

TEST(PortTest, AsyncWaitEventRepeat) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  zx::event event;
  ASSERT_OK(zx::event::create(0u, &event));

  constexpr uint64_t kEventKey = 1122;

  zx_port_packet_t packet = {};
  uint64_t count[3] = {};

  constexpr uint64_t kWaitAsyncRepeats = 24;

  for (int ix = 0; ix != kWaitAsyncRepeats; ++ix) {
    ASSERT_OK(event.wait_async(port, kEventKey, ZX_EVENT_SIGNALED | ZX_USER_SIGNAL_2, 0));

    uint32_t ub = (ix % 2) ? 0u : ZX_USER_SIGNAL_2;
    // Set, then clear the signal.
    EXPECT_OK(event.signal(0u, ZX_EVENT_SIGNALED | ub));
    EXPECT_OK(event.signal(ZX_EVENT_SIGNALED | ub, 0u));

    ASSERT_OK(port.wait(zx::time::infinite_past(), &packet));
    ASSERT_EQ(packet.type, ZX_PKT_TYPE_SIGNAL_ONE);
    ASSERT_EQ(packet.signal.count, 1u);
    count[0] += (packet.signal.observed & ZX_EVENT_SIGNALED) ? 1 : 0;
    count[1] += (packet.signal.observed & ZX_USER_SIGNAL_2) ? 1 : 0;
    count[2] += (packet.signal.observed & ~(ZX_EVENT_SIGNALED | ZX_USER_SIGNAL_2)) ? 1 : 0;
  }

  EXPECT_EQ(count[0], kWaitAsyncRepeats);
  EXPECT_EQ(count[1], kWaitAsyncRepeats / 2);
  EXPECT_EQ(count[2], 0u);
}

TEST(PortTest, AsyncWaitEventManyAllProcessed) {
  constexpr uint64_t key = 6567;
  // One more than the size of the packet arena.
  constexpr size_t kEventCount = 16 * 1024 + 1;

  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  zx::event event[kEventCount];
  for (size_t i = 0; i < kEventCount; i++) {
    EXPECT_OK(zx::event::create(0u, &event[i]));

    EXPECT_OK(event[i].wait_async(port, key, ZX_EVENT_SIGNALED, 0));

    EXPECT_OK(event[i].signal(0u, ZX_EVENT_SIGNALED));
  }

  size_t count = 0;
  zx_port_packet_t packet = {};
  zx_status_t status;
  while (ZX_OK == (status = port.wait(zx::time::infinite_past(), &packet))) {
    EXPECT_EQ(packet.key, key);
    EXPECT_EQ(packet.type, ZX_PKT_TYPE_SIGNAL_ONE);
    EXPECT_EQ(packet.signal.observed, ZX_EVENT_SIGNALED);
    EXPECT_EQ(packet.signal.trigger, ZX_EVENT_SIGNALED);
    EXPECT_EQ(packet.signal.count, 1u);

    ++count;
  }
  EXPECT_EQ(status, ZX_ERR_TIMED_OUT);
  EXPECT_EQ(count, kEventCount);
}

// Check that zx_object_wait_async() returns an error if it is passed an
// invalid option.
TEST(PortTest, AsyncWaitInvalidOption) {
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));
  zx::event event;
  ASSERT_OK(zx::event::create(0u, &event));

  constexpr uint64_t kKey = 0;
  constexpr uint32_t kInvalidOption = 20;
  EXPECT_EQ(event.wait_async(port, kKey, ZX_EVENT_SIGNALED, kInvalidOption), ZX_ERR_INVALID_ARGS);
}

TEST(PortTest, ChannelAsyncWaitOnExistingStateIsNotified) {
  constexpr uint64_t kEventKey = 65667;

  // Create a channel pair, and write 5 messages into it.
  zx::channel ch[2];
  ASSERT_OK(zx::channel::create(0u, &ch[0], &ch[1]));
  for (int ix = 0; ix != 5; ++ix) {
    ASSERT_OK(ch[0].write(0u, "123456", 6, nullptr, 0u));
  }
  ch[0].reset();

  // Create a port, and set it up to be notified when the channel is
  // readable or closed.
  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(ch[1].wait_async(port, kEventKey, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, 0));

  // Wait for a packet to be received on the port, with both the
  // READABLE and PEER_CLOSED signals asserted.
  zx_port_packet_t packet = {};
  zx_status_t status = port.wait(zx::time::infinite_past(), &packet);
  EXPECT_EQ(status, ZX_OK);
  EXPECT_EQ(packet.signal.count, 1u);  // count is always 1.
  EXPECT_EQ(packet.signal.trigger, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED);
  EXPECT_EQ(packet.signal.observed, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED);

  // We don't expect any other events on the port.
  EXPECT_EQ(port.wait(zx::time::infinite_past(), &packet), ZX_ERR_TIMED_OUT);
}

TEST(PortTest, CancelEventKey) {
  zx::port port;
  zx::event event;

  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event));

  // Notice repeated key below.
  const uint64_t keys[] = {128u, 13u, 7u, 13u};

  for (uint32_t ix = 0; ix != std::size(keys); ++ix) {
    ASSERT_OK(event.wait_async(port, keys[ix], ZX_EVENT_SIGNALED, 0));
  }

  // We cancel before it is signaled so no packets from |13| are seen.
  EXPECT_OK(port.cancel(event, 13u));

  for (int ix = 0; ix != 2; ++ix) {
    EXPECT_OK(event.signal(0u, ZX_EVENT_SIGNALED));
    EXPECT_OK(event.signal(ZX_EVENT_SIGNALED, 0u));
  }

  zx_port_packet_t packet = {};
  int wait_count = 0;
  uint64_t key_sum = 0;

  zx_status_t status;
  while (true) {
    status = port.wait(zx::time::infinite_past(), &packet);
    if (status != ZX_OK) {
      break;
    }
    wait_count++;
    key_sum += packet.key;
    EXPECT_EQ(packet.signal.trigger, ZX_EVENT_SIGNALED);
    EXPECT_EQ(packet.signal.observed, ZX_EVENT_SIGNALED);
  }

  // We cancel after the packet has been delivered.
  EXPECT_EQ(port.cancel(event, 128u), ZX_ERR_NOT_FOUND);

  EXPECT_EQ(wait_count, 2);
  EXPECT_EQ(key_sum, keys[0] + keys[2]);
}

TEST(PortTest, CancelEventKeyAfter) {
  zx::port port;

  ASSERT_OK(zx::port::create(0u, &port));

  const uint64_t keys[] = {128u, 3u, 3u};
  zx::event ev[std::size(keys)];
  for (uint32_t ix = 0; ix != std::size(keys); ++ix) {
    ASSERT_OK(zx::event::create(0u, &ev[ix]));
    ASSERT_OK(ev[ix].wait_async(port, keys[ix], ZX_EVENT_SIGNALED, 0));
  }

  EXPECT_OK(ev[0].signal(0u, ZX_EVENT_SIGNALED));
  EXPECT_OK(ev[1].signal(0u, ZX_EVENT_SIGNALED));

  // We cancel after the first two signals and before the third. So it should
  // test both cases with queued packets and no-yet-fired packets.
  EXPECT_OK(port.cancel(ev[1], 3u));
  EXPECT_OK(port.cancel(ev[2], 3u));

  EXPECT_OK(ev[2].signal(0u, ZX_EVENT_SIGNALED));

  zx_port_packet_t packet = {};
  int wait_count = 0;
  uint64_t key_sum = 0;

  zx_status_t status;
  while (true) {
    status = port.wait(zx::time::infinite_past(), &packet);
    if (status != ZX_OK) {
      break;
    }
    wait_count++;
    key_sum += packet.key;
    EXPECT_EQ(packet.signal.trigger, ZX_EVENT_SIGNALED);
    EXPECT_EQ(packet.signal.observed, ZX_EVENT_SIGNALED);
  }

  EXPECT_EQ(wait_count, 1);
  EXPECT_EQ(key_sum, keys[0]);
}

TEST(PortTest, ThreadEvents) {
  zx::port port;
  zx::event event;
  constexpr size_t kNumPortWaiterThreads = 3;

  auto PortWaiter = [](zx::port* port, uint32_t count, zx_status_t* return_status) {
    zx_port_packet_t packet = {};
    do {
      *return_status = port->wait(zx::time::infinite(), &packet);
      if (*return_status < 0) {
        return;
      }
    } while (--count);
  };

  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event));

  std::thread port_waiters[kNumPortWaiterThreads];
  zx_status_t return_status[kNumPortWaiterThreads];
  std::fill_n(return_status, kNumPortWaiterThreads, ZX_ERR_INTERNAL);

  for (size_t ix = 0; ix != kNumPortWaiterThreads; ++ix) {
    // |count| is one so each thread is going to pick one packet each
    // and exit. See bug fxbug.dev/30605 for the case this is testing.
    EXPECT_OK(event.wait_async(port, (500u + ix), ZX_EVENT_SIGNALED, 0));

    port_waiters[ix] = std::thread(PortWaiter, &port, 1, &return_status[ix]);
  }

  EXPECT_OK(event.signal(0u, ZX_EVENT_SIGNALED));

  for (size_t ix = 0; ix != kNumPortWaiterThreads; ++ix) {
    port_waiters[ix].join();
    EXPECT_OK(return_status[ix]);
  }
}

TEST(PortTest, Timestamp) {
  // Test that the timestamp feature returns reasonable numbers. We use
  // a single thread so the numbers should be nanosecond-grade reliable.
  zx::port port;
  zx::event event[2];
  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event[0]));
  ASSERT_OK(zx::event::create(0u, &event[1]));

  ASSERT_OK(event[0].wait_async(port, 1, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_TIMESTAMP));
  ASSERT_OK(event[1].wait_async(port, 2, ZX_EVENT_SIGNALED, 0u));

  auto before = zx::clock::get_monotonic();
  ASSERT_OK(event[0].signal(0u, ZX_EVENT_SIGNALED));
  auto after = zx::clock::get_monotonic();
  ASSERT_OK(event[1].signal(0u, ZX_EVENT_SIGNALED));

  zx_port_packet_t packet[2];
  ASSERT_OK(port.wait(zx::time::infinite(), &packet[0]));
  ASSERT_OK(port.wait(zx::time::infinite(), &packet[1]));

  ASSERT_EQ(packet[0].signal.trigger, ZX_EVENT_SIGNALED);
  ASSERT_EQ(packet[1].signal.trigger, ZX_EVENT_SIGNALED);

  EXPECT_LE(before, zx::time(packet[0].signal.timestamp));
  EXPECT_GE(after, zx::time(packet[0].signal.timestamp));

  EXPECT_EQ(0u, packet[1].signal.timestamp);

  // Now test the same using event[0] in place of event[1].
  ASSERT_OK(event[0].signal(ZX_EVENT_SIGNALED, 0u));
  ASSERT_OK(event[1].signal(ZX_EVENT_SIGNALED, 0u));

  ASSERT_OK(event[1].wait_async(port, 1, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_TIMESTAMP));
  ASSERT_OK(event[0].wait_async(port, 2, ZX_EVENT_SIGNALED, 0u));

  ASSERT_OK(event[0].signal(0u, ZX_EVENT_SIGNALED));
  before = zx::clock::get_monotonic();
  ASSERT_OK(event[1].signal(0u, ZX_EVENT_SIGNALED));
  after = zx::clock::get_monotonic();

  ASSERT_OK(port.wait(zx::time::infinite(), &packet[0]));
  ASSERT_OK(port.wait(zx::time::infinite(), &packet[1]));

  EXPECT_LE(before, zx::time(packet[1].signal.timestamp));
  EXPECT_GE(after, zx::time(packet[1].signal.timestamp));

  EXPECT_EQ(0u, packet[0].signal.timestamp);
}

TEST(PortTest, EdgeTriggerDetected) {
  // Test ZX_WAIT_ASYNC_EDGE only generates a packet when a signal
  // becomes active after the wait_async.
  zx::port port;
  zx::event event;
  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event));

  uint64_t key = 2u;
  ASSERT_OK(event.wait_async(port, key, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_EDGE));

  zx_handle_t main_thread = zx_thread_self();

  auto loop_signal = [&event, main_thread]() {
    zx_info_thread thd_info;
    do {
      // This sleep is to prevent repeated calls to zx_object_get_info.
      // The test is deterministic.
      zx_nanosleep(zx_deadline_after(ZX_USEC(600)));
      EXPECT_OK(zx_object_get_info(main_thread, ZX_INFO_THREAD, &thd_info, sizeof(thd_info),
                                   nullptr, nullptr));
    } while (thd_info.state != ZX_THREAD_STATE_BLOCKED_PORT);
    EXPECT_OK(event.signal(0u, ZX_EVENT_SIGNALED));
  };
  std::thread signal_thread(loop_signal);
  zx_port_packet_t packet;
  ASSERT_OK(port.wait(zx::time::infinite(), &packet));
  ASSERT_OK(packet.status);
  ASSERT_EQ(ZX_PKT_TYPE_SIGNAL_ONE, packet.type);
  ASSERT_EQ(ZX_EVENT_SIGNALED, packet.signal.observed);
  ASSERT_EQ(key, packet.key);

  signal_thread.join();
}

TEST(PortTest, EdgeTriggerCancel) {
  // This is the same as EdgeTriggerDetected, but the wait
  // is cancelled before the signal is set.
  zx::port port;
  zx::event event;
  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event));

  uint64_t key1 = 42u;
  uint64_t key2 = 999u;
  uint64_t key3 = 2020u;

  ASSERT_OK(event.wait_async(port, key1, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_EDGE));
  ASSERT_OK(event.wait_async(port, key2, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_EDGE));
  ASSERT_OK(event.wait_async(port, key3, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_EDGE));

  EXPECT_OK(port.cancel(event, key2));
  ASSERT_OK(event.signal(0u, ZX_EVENT_SIGNALED));
  EXPECT_OK(port.cancel(event, key3));

  zx_port_packet_t packet;
  ASSERT_OK(port.wait(zx::time::infinite(), &packet));
  ASSERT_OK(packet.status);
  ASSERT_EQ(ZX_PKT_TYPE_SIGNAL_ONE, packet.type);
  ASSERT_EQ(ZX_EVENT_SIGNALED, packet.signal.observed);
  ASSERT_EQ(key1, packet.key);

  // We cancelled key2 and key3, so they should not be queued.
  ASSERT_EQ(ZX_ERR_TIMED_OUT, port.wait(zx::time(zx_deadline_after(ZX_USEC(600))), &packet));
}

TEST(PortTest, EdgeTriggerTimeout) {
  // Test ZX_WAIT_ASYNC_EDGE only generates a packet when a signal
  // becomes active after the wait_async.
  zx::port port;
  zx::event event;
  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event));

  // Call signal before wait, so no packet will be queued.
  ASSERT_OK(event.signal(0u, ZX_EVENT_SIGNALED));
  ASSERT_OK(event.wait_async(port, 1, ZX_EVENT_SIGNALED, ZX_WAIT_ASYNC_EDGE));

  zx_port_packet_t packet;
  ASSERT_EQ(ZX_ERR_TIMED_OUT, port.wait(zx::time(zx_deadline_after(ZX_USEC(600))), &packet));
}

// Queue a packet while another thread is closing the port.  See that queuing thread observes
// ZX_ERR_BAD_HANDLE. This test is inherently racy in that we can not always get both the
// closing thread and the queuing thread to enter the code under test at the same time.
TEST(PortTest, CloseQueueRace) {
  zx::port port;
  std::atomic<uint64_t> count = 0;
  ASSERT_OK(zx::port::create(0, &port));

  auto queue_loop = [port = port.borrow(), &count]() {
    constexpr zx_port_packet_t kPacket = {1ull, ZX_PKT_TYPE_USER, 0, {{}}};
    constexpr uint64_t kMaxMessages = 1024;
    uint64_t enqueued_messages = 0;

    while (true) {
      // Enqueue a message.
      {
        zx_status_t status = port->queue(&kPacket);
        if (status != ZX_OK) {
          ZX_ASSERT_MSG(status == ZX_ERR_BAD_HANDLE, "Unexpected status: %d", status);
          break;
        }
        enqueued_messages++;
      }

      // If too many messages are enqueued, dequeue one to avoid hitting the
      // port depth limit.
      while (enqueued_messages >= kMaxMessages) {
        zx_port_packet read_packet;
        zx_status_t status = port->wait(zx::time::infinite_past(), &read_packet);
        if (status == ZX_ERR_TIMED_OUT) {
          // We may race with the port being closed in the following manner:
          //
          //  1. This thread enters the kernel, converts its handle into
          //     a reference to the port object, but doesn't yet read
          //     the messages on the queue.
          //
          //  2. The other thread deletes the handle to the port,
          //     deleting all pending messages in the process.
          //
          //  3. Because no message is waiting, the port operation times
          //     out with ZX_ERR_TIMED_OUT.
          //
          // In this case, we just try again.
          continue;
        } else if (status == ZX_ERR_BAD_HANDLE) {
          // Port was closed.
          break;
        }
        ZX_ASSERT_MSG(status == ZX_OK, "Unexpected status: %d", status);
        enqueued_messages--;
      }

      count.fetch_add(1);
    }
  };

  std::thread queue_thread(queue_loop);

  // Spin until |queue_thread| completes at least one loop iteration.
  while (count.load() == 0) {
  }

  // Close the port out from under it.
  port.reset();

  // See that it gets ZX_ERR_BAD_HANDLE.
  queue_thread.join();
}

// See that creating too many object observers raises an exception.
TEST(PortTest, TooManyObservers) {
  if (getenv("NO_NEW_PROCESS")) {
    ZXTEST_SKIP("Running without the ZX_POL_NEW_PROCESS policy, skipping test case.");
  }

  constexpr char kName[] = "too-many-observers-test";
  zx::process proc;
  zx::thread thread;
  zx::vmar vmar;
  ASSERT_OK(
      zx::process::create(*zx::job::default_job(), kName, sizeof(kName) - 1, 0, &proc, &vmar));
  ASSERT_OK(zx::thread::create(proc, kName, sizeof(kName) - 1, 0u, &thread));

  zx::channel local;
  zx::channel remote;
  ASSERT_OK(zx::channel::create(0, &local, &remote));

  zx::channel cmd_channel;
  ASSERT_OK(start_mini_process_etc(proc.get(), thread.get(), vmar.get(), local.release(), true,
                                   cmd_channel.reset_and_get_address()));
  // If the process is terminated as expected, we should peer closed.
  ASSERT_EQ(ZX_ERR_PEER_CLOSED, mini_process_cmd(cmd_channel.get(), MINIP_CMD_WAIT_ASYNC, nullptr));
  ASSERT_OK(proc.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr));

  zx_info_process_t proc_info;
  ASSERT_OK(proc.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr));
  ASSERT_EQ(proc_info.return_code, ZX_TASK_RETCODE_EXCEPTION_KILL);
}

TEST(PortStressTest, WaitSignalCancel) {
  // This tests a race that existed between the port observer
  // removing itself from the event and the cancellation logic which is
  // also working with the same internal object. The net effect of the
  // bug is that port_cancel() would fail with ZX_ERR_NOT_FOUND.
  //
  // When running on real hardware or KVM-accelerated emulation
  // a good number to set for kStressCount is 50000000.
  constexpr uint32_t kStressCount = 20000;

  auto CancelStressWaiter = [](zx::port* port, zx::event* event, zx_status_t* return_status) {
    const auto key = 919u;
    auto count = kStressCount;

    while (--count) {
      *return_status = event->wait_async(*port, key, ZX_EVENT_SIGNALED, 0);
      if (*return_status != ZX_OK) {
        break;
      }

      zx_signals_t observed;
      *return_status = event->wait_one(ZX_EVENT_SIGNALED, zx::time::infinite(), &observed);
      if (*return_status != ZX_OK) {
        break;
      }

      *return_status = port->cancel(*event, key);
      if (*return_status != ZX_OK) {
        break;
      }
    }
  };

  auto CancelStressSignaler = [](zx::port* port, zx::event* event,
                                 std::atomic<bool>* keep_running) {
    uint64_t count = 0;
    zx_status_t status;

    while (keep_running->load()) {
      status = event->signal(0u, ZX_EVENT_SIGNALED);
      if (status != ZX_OK) {
        return;
      }

      constexpr uint32_t kSleeps[] = {0, 10, 2, 0, 15, 0};
      auto duration = kSleeps[count++ % std::size(kSleeps)];
      if (duration > 0) {
        zx::nanosleep(zx::deadline_after(zx::nsec(duration)));
      }

      status = event->signal(ZX_EVENT_SIGNALED, 0u);
      if (status != ZX_OK) {
        return;
      }
    }
  };

  zx::port port;
  zx::event event;
  ASSERT_OK(zx::port::create(0u, &port));
  ASSERT_OK(zx::event::create(0u, &event));
  zx_status_t waiter_status = ZX_ERR_INTERNAL;
  std::atomic<bool> keep_running(true);

  std::thread waiter(CancelStressWaiter, &port, &event, &waiter_status);
  std::thread signaler(CancelStressSignaler, &port, &event, &keep_running);

  waiter.join();
  keep_running.store(false);
  signaler.join();

  EXPECT_OK(waiter_status);
}

// A stress test that repeatedly signals and closes events registered with a port.
TEST(PortStressTest, SignalCloseWait) {
  constexpr zx::duration kTestDuration = zx::msec(100);
  srand(4);

  // Continually reads packets from a port until it gets a ZX_PKT_TYPE_USER.
  auto PortWaitDrainer = [](zx::port* port, zx_status_t* return_status) {
    while (true) {
      zx_port_packet_t packet{};
      zx_status_t status = port->wait(zx::time::infinite(), &packet);
      if (status != ZX_OK) {
        *return_status = status;
        return;
      }
      if (packet.type == ZX_PKT_TYPE_USER) {
        *return_status = ZX_OK;
        break;
      }
    }
  };

  // Creates an event registered with the port then performs the following actions randomly:
  //   a. sleep
  //   b. signal the event
  //   c. signal the event, then close it
  auto WaitEventSignalClose = [](zx::port* port, std::atomic<bool>* keep_running,
                                 zx_status_t* return_status) {
    zx_status_t status;
    zx::event event;

    while (keep_running->load()) {
      if (!event.is_valid()) {
        status = zx::event::create(0u, &event);
        if (status != ZX_OK) {
          *return_status = status;
          return;
        }
        status = event.wait_async(*port, 0, ZX_EVENT_SIGNALED, 0);
        if (status != ZX_OK) {
          *return_status = status;
          return;
        }
      }

      unsigned action = rand() % 3;
      switch (action) {
        case 0:  // sleep
          zx::nanosleep(zx::deadline_after(zx::msec(1)));
          break;
        case 1:  // signal
          status = event.signal(0u, ZX_EVENT_SIGNALED);
          if (status != ZX_OK) {
            *return_status = status;
            return;
          }
          break;
        default:  // signal and close
          status = event.signal(0u, ZX_EVENT_SIGNALED);
          if (status != ZX_OK) {
            *return_status = status;
            return;
          }
          event.reset();
      }
    }
    *return_status = ZX_OK;
  };

  zx::port port;
  ASSERT_OK(zx::port::create(0u, &port));

  constexpr unsigned kNumSignalers = 4;
  std::thread signalers[kNumSignalers];
  zx_status_t signaler_return_status[kNumSignalers];
  std::fill_n(signaler_return_status, kNumSignalers, ZX_ERR_INTERNAL);
  std::atomic<bool> keep_running(true);

  for (size_t ix = 0; ix != kNumSignalers; ++ix) {
    signalers[ix] =
        std::thread(WaitEventSignalClose, &port, &keep_running, &signaler_return_status[ix]);
  }

  constexpr unsigned kNumDrainers = 4;
  std::thread drainers[kNumDrainers];
  zx_status_t drainer_return_status[kNumDrainers];
  std::fill_n(drainer_return_status, kNumSignalers, ZX_ERR_INTERNAL);
  for (size_t ix = 0; ix != kNumDrainers; ++ix) {
    drainers[ix] = std::thread(PortWaitDrainer, &port, &drainer_return_status[ix]);
  }

  zx::nanosleep(zx::deadline_after(kTestDuration));
  keep_running.store(false);

  for (size_t ix = 0; ix < kNumDrainers; ++ix) {
    zx_port_packet_t pkt{};
    pkt.type = ZX_PKT_TYPE_USER;
    zx_status_t status;
    do {
      status = port.queue(&pkt);
    } while (status == ZX_ERR_SHOULD_WAIT);
    EXPECT_OK(status);
  }

  for (size_t ix = 0; ix < kNumDrainers; ++ix) {
    drainers[ix].join();
    EXPECT_OK(drainer_return_status[ix]);
  }

  for (size_t ix = 0; ix < kNumSignalers; ++ix) {
    signalers[ix].join();
    EXPECT_OK(signaler_return_status[ix]);
  }
}

// A stress test designed to create a race where one thread is closing the port as another thread is
// performing an object_wait_async using the same port handle.
TEST(PortStressTest, CloseWaitRace) {
  constexpr zx::duration kTestDuration = zx::msec(100);
  srand(4);

  constexpr uint64_t kNumWaiters = 4;
  constexpr uint64_t kTotalThreads = kNumWaiters + 1;
  struct Args {
    std::atomic<uint64_t>* ready_count{};
    std::atomic<bool>* keep_running{};
    std::atomic<zx_handle_t>* port{};
    zx_status_t status{ZX_ERR_INTERNAL};
  };

  // Repeatedly asynchronously wait on an event.
  auto WaitAsyncLoop = [](Args* args) {
    // Keep track of how many async waits we perform so we can self-limit and stay below any kernel
    // imposed maximum.
    constexpr uint64_t kMaxWaitsPerObject = 100;
    uint64_t num_waits = 0;

    // Create an event that we'll async_wait on.
    zx::event event;
    args->status = zx::event::create(0, &event);
    if (args->status != ZX_OK) {
      return;
    }

    // Signal that we're ready.
    args->ready_count->fetch_add(1);
    // Wait for all the other threads to become ready.
    while (args->ready_count->load() < kTotalThreads) {
    }
    while (args->keep_running->load()) {
      zx_status_t status =
          zx_object_wait_async(event.get(), args->port->load(), 0, ZX_EVENT_SIGNALED, 0);
      if (status != ZX_OK && status != ZX_ERR_BAD_HANDLE) {
        args->status = status;
        return;
      }
      if (++num_waits >= kMaxWaitsPerObject) {
        args->status = zx::event::create(0, &event);
        if (args->status != ZX_OK) {
          return;
        }

        num_waits = 0;
      }
    }
    args->status = ZX_OK;
  };

  // Repeatedly create and close a port.
  auto CreatePortLoop = [](Args* args) {
    // Signal that we're ready.
    args->ready_count->fetch_add(1);
    // Wait for all the other threads to become ready.
    while (args->ready_count->load() < kTotalThreads) {
    }
    while (args->keep_running->load()) {
      zx_handle_t temp_port;
      zx_status_t status = zx_port_create(0, &temp_port);
      if (status != ZX_OK) {
        args->status = status;
        return;
      }
      args->port->store(temp_port);

      // Give the waiter threads an opportunity to get the handle and wait_async on it.
      zx::nanosleep(zx::deadline_after(zx::msec(1)));

      // Then close it out from under them.
      status = zx_handle_close(temp_port);
      args->port->store(ZX_HANDLE_INVALID);
      if (status != ZX_OK) {
        args->status = status;
        return;
      }
    }
    args->status = ZX_OK;
  };

  std::atomic<uint64_t> ready_count(0);
  std::atomic<bool> keep_running(true);
  std::atomic<zx_handle_t> port(ZX_HANDLE_INVALID);

  Args waiter_args[kNumWaiters];
  std::thread wait_async_thread[kNumWaiters];
  for (size_t ix = 0; ix != kNumWaiters; ++ix) {
    Args* args = &waiter_args[ix];
    args->ready_count = &ready_count;
    args->keep_running = &keep_running;
    args->port = &port;
    wait_async_thread[ix] = std::thread(WaitAsyncLoop, args);
  }

  Args creator_args{&ready_count, &keep_running, &port, ZX_ERR_INTERNAL};
  std::thread create_port_thread(CreatePortLoop, &creator_args);

  zx::nanosleep(zx::deadline_after(kTestDuration));
  keep_running.store(false);

  for (size_t ix = 0; ix != kNumWaiters; ++ix) {
    wait_async_thread[ix].join();
    ASSERT_OK(waiter_args[ix].status);
  }

  create_port_thread.join();
  EXPECT_OK(creator_args.status);

  zx_handle_close(port.load());
}

}  // namespace
