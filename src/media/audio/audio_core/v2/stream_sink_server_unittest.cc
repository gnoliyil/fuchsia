// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/v2/stream_sink_server.h"

#include <lib/async-testing/test_loop.h>
#include <lib/sync/cpp/completion.h>
#include <lib/syslog/cpp/macros.h>

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/media/audio/services/common/logging.h"
#include "src/media/audio/services/common/testing/test_fence.h"

namespace media_audio {
namespace {

using ::fuchsia_audio::wire::Timestamp;
using ::testing::ElementsAre;

const auto kFormat = Format::CreateOrDie({fuchsia_audio::SampleType::kInt32, 2, 1000});
constexpr uint64_t kBufferSize = 4096;

class StreamSinkServerTest : public testing::Test {
 public:
  StreamSinkServerTest() {
    thread_ = FidlThread::CreateFromCurrentThread("TestThread", loop_.dispatcher());
    payload_buffer_ = MemoryMappedBuffer::CreateOrDie(kBufferSize, true);

    auto endpoints = fidl::CreateEndpoints<fuchsia_audio::StreamSink>();
    if (!endpoints.is_ok()) {
      FX_PLOGS(FATAL, endpoints.status_value()) << "fidl::CreateEndpoints failed";
    }
    client_ = fidl::WireClient(std::move(endpoints->client), loop_.dispatcher());
    server_ = StreamSinkServer::Create(thread_, std::move(endpoints->server),
                                       StreamSinkServer::Args{
                                           .format = kFormat,
                                           .payload_buffer = payload_buffer_,
                                       });

    // Fill the payload buffer with an integer sequence.
    const int64_t num_frames =
        static_cast<int64_t>(payload_buffer_->size()) / kFormat.bytes_per_frame();
    int32_t* frames = static_cast<int32_t*>(payload_buffer_->start());
    for (int32_t k = 0; k < num_frames; k++) {
      frames[k] = k;
    }
  }

  ~StreamSinkServerTest() {
    client_ = fidl::WireClient<fuchsia_audio::StreamSink>();
    // RunUntilIdle should run all on_unbound callbacks, so the server should now be shut down.
    loop_.RunUntilIdle();
    EXPECT_TRUE(server_->WaitForShutdown(zx::nsec(0)));
  }

  fidl::Arena<>& arena() { return arena_; }
  async::TestLoop& loop() { return loop_; }
  MemoryMappedBuffer& payload_buffer() { return *payload_buffer_; }
  StreamSinkServer& server() { return *server_; }

  // Calls `StreamSink->PutPacket`.
  // Should be called with ASSERT_NO_FATAL_FAILURE(..).
  void PutPacket(fuchsia_media2::wire::PayloadRange payload, Timestamp timestamp,
                 zx::eventpair fence) {
    auto result =
        client_->PutPacket(fuchsia_audio::wire::StreamSinkPutPacketRequest::Builder(arena_)
                               .packet(fuchsia_audio::wire::Packet::Builder(arena_)
                                           .payload(payload)
                                           .timestamp(timestamp)
                                           .Build())
                               .release_fence(std::move(fence))
                               .Build());
    ASSERT_TRUE(result.ok()) << result;
  }

  struct CapturedPacket {
    std::vector<int32_t> data;
    std::optional<zx::time> timestamp;
    std::optional<int64_t> bytes_captured;
    std::optional<zx::duration> overflow;
  };

  // Calls `server().CapturePacket`.
  std::shared_ptr<CapturedPacket> CapturePacket(int64_t frames_per_capture) {
    auto c = std::make_shared<CapturedPacket>();
    c->data.resize(frames_per_capture * kFormat.channels());
    server().CapturePacket(c->data.data(), sizeof(int32_t) * c->data.size(),
                           [c](auto timestamp, auto bytes_captured, auto overflow) {
                             c->timestamp = zx::time(timestamp);
                             c->bytes_captured = bytes_captured;
                             c->overflow = overflow;
                           });
    return c;
  }

 private:
  fidl::Arena<> arena_;

  async::TestLoop loop_;
  std::shared_ptr<FidlThread> thread_;
  std::shared_ptr<MemoryMappedBuffer> payload_buffer_;
  std::shared_ptr<StreamSinkServer> server_;
  fidl::WireClient<fuchsia_audio::StreamSink> client_;
};

TEST_F(StreamSinkServerTest, CapturesSmallerThanPackets) {
  const int64_t frames_per_capture = 5;
  const int64_t bytes_per_capture = frames_per_capture * kFormat.bytes_per_frame();
  auto capture0 = CapturePacket(frames_per_capture);

  // Nothing to capture from yet.
  loop().RunUntilIdle();
  EXPECT_FALSE(capture0->timestamp);

  const int64_t frames_per_packet = 10;
  const int64_t bytes_per_packet = frames_per_packet * kFormat.bytes_per_frame();

  const auto packet0_ts = zx::time(0) + zx::msec(100);
  TestFence packet0_fence;
  TestFence packet1_fence;

  {
    SCOPED_TRACE("send packet 0 (explicit timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = 0,
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithSpecified(arena(), packet0_ts.get()), packet0_fence.Take()));
  }

  {
    SCOPED_TRACE("send packet 1 (continuous timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = static_cast<uint64_t>(bytes_per_packet),
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithUnspecifiedContinuous({}), packet1_fence.Take()));
  }

  // Now that two packets have been pushed, capture0 should be complete.
  loop().RunUntilIdle();
  EXPECT_EQ(capture0->timestamp, packet0_ts);
  EXPECT_EQ(capture0->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture0->overflow, zx::msec(0));
  EXPECT_FALSE(packet0_fence.Done());
  EXPECT_FALSE(packet1_fence.Done());

  // Next three captures should complete immediately.
  auto capture1 = CapturePacket(frames_per_capture);
  loop().RunUntilIdle();
  EXPECT_EQ(capture1->timestamp, packet0_ts + zx::msec(5));
  EXPECT_EQ(capture1->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture1->overflow, zx::msec(0));
  EXPECT_TRUE(packet0_fence.Done());
  EXPECT_FALSE(packet1_fence.Done());

  auto capture2 = CapturePacket(frames_per_capture);
  loop().RunUntilIdle();
  EXPECT_EQ(capture2->timestamp, packet0_ts + zx::msec(10));
  EXPECT_EQ(capture2->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture2->overflow, zx::msec(0));
  EXPECT_TRUE(packet0_fence.Done());
  EXPECT_FALSE(packet1_fence.Done());

  auto capture3 = CapturePacket(frames_per_capture);
  loop().RunUntilIdle();
  EXPECT_EQ(capture3->timestamp, packet0_ts + zx::msec(15));
  EXPECT_EQ(capture3->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture3->overflow, zx::msec(0));
  EXPECT_TRUE(packet0_fence.Done());
  EXPECT_TRUE(packet1_fence.Done());

  // Should have captured the first 20 frames (40 samples) from the original payload buffer.
  EXPECT_THAT(capture0->data, ElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8, 9));
  EXPECT_THAT(capture1->data, ElementsAre(10, 11, 12, 13, 14, 15, 16, 17, 18, 19));
  EXPECT_THAT(capture2->data, ElementsAre(20, 21, 22, 23, 24, 25, 26, 27, 28, 29));
  EXPECT_THAT(capture3->data, ElementsAre(30, 31, 32, 33, 34, 35, 36, 37, 38, 39));
}

TEST_F(StreamSinkServerTest, CapturesLargerThanPackets) {
  const int64_t frames_per_capture = 10;
  const int64_t bytes_per_capture = frames_per_capture * kFormat.bytes_per_frame();
  auto capture0 = CapturePacket(frames_per_capture);

  // Nothing to capture from yet.
  loop().RunUntilIdle();
  EXPECT_FALSE(capture0->timestamp);

  const int64_t frames_per_packet = 5;
  const int64_t bytes_per_packet = frames_per_packet * kFormat.bytes_per_frame();

  const auto packet0_ts = zx::time(0) + zx::msec(100);
  TestFence packet0_fence;
  TestFence packet1_fence;
  TestFence packet2_fence;
  TestFence packet3_fence;

  {
    SCOPED_TRACE("send packet 0 (explicit timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = 0,
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithSpecified(arena(), packet0_ts.get()), packet0_fence.Take()));
  }

  // This packet is immediately consumed, but capture0 is not full yet.
  loop().RunUntilIdle();
  EXPECT_FALSE(capture0->timestamp);
  EXPECT_TRUE(packet0_fence.Done());

  {
    SCOPED_TRACE("send packet 1 (continuous timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = static_cast<uint64_t>(bytes_per_packet),
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithUnspecifiedContinuous({}), packet1_fence.Take()));
  }

  // Now that two packets have been pushed, capture0 should be complete.
  loop().RunUntilIdle();
  EXPECT_EQ(capture0->timestamp, packet0_ts);
  EXPECT_EQ(capture0->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture0->overflow, zx::msec(0));
  EXPECT_TRUE(packet1_fence.Done());

  {
    SCOPED_TRACE("send packet 2 (continuous timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = static_cast<uint64_t>(2 * bytes_per_packet),
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithUnspecifiedContinuous({}), packet2_fence.Take()));
  }

  {
    SCOPED_TRACE("send packet 3 (continuous timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = static_cast<uint64_t>(3 * bytes_per_packet),
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithUnspecifiedContinuous({}), packet3_fence.Take()));
  }

  // With two more packets already pushed, the next capture should complete immediately.
  auto capture1 = CapturePacket(frames_per_capture);
  loop().RunUntilIdle();
  EXPECT_EQ(capture1->timestamp, packet0_ts + zx::msec(10));
  EXPECT_EQ(capture1->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture1->overflow, zx::msec(0));
  EXPECT_TRUE(packet2_fence.Done());
  EXPECT_TRUE(packet3_fence.Done());

  // Should have captured the first 20 frames (40 samples) from the original payload buffer.
  EXPECT_THAT(capture0->data, ElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8, 9,  //
                                          10, 11, 12, 13, 14, 15, 16, 17, 18, 19));
  EXPECT_THAT(capture1->data, ElementsAre(20, 21, 22, 23, 24, 25, 26, 27, 28, 29,  //
                                          30, 31, 32, 33, 34, 35, 36, 37, 38, 39));
}

TEST_F(StreamSinkServerTest, Underflow) {
  const int64_t frames_per_capture = 5;
  const int64_t frames_per_packet = 5;
  const int64_t bytes_per_capture = frames_per_capture * kFormat.bytes_per_frame();
  const int64_t bytes_per_packet = frames_per_packet * kFormat.bytes_per_frame();

  const auto packet0_ts = zx::time(0) + zx::msec(100);
  const auto packet1_ts = zx::time(0) + zx::msec(200);
  TestFence packet0_fence;
  TestFence packet1_fence;

  {
    SCOPED_TRACE("send packet 0 (explicit timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = 0,
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithSpecified(arena(), packet0_ts.get()), packet0_fence.Take()));
  }

  {
    SCOPED_TRACE("send packet 1 (explicit timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = static_cast<uint64_t>(bytes_per_packet),
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithSpecified(arena(), packet1_ts.get()), packet1_fence.Take()));
  }

  // First capture is immediately ready.
  auto capture0 = CapturePacket(frames_per_capture);
  loop().RunUntilIdle();
  EXPECT_EQ(capture0->timestamp, packet0_ts);
  EXPECT_EQ(capture0->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture0->overflow, zx::msec(0));
  EXPECT_TRUE(packet0_fence.Done());
  EXPECT_FALSE(packet1_fence.Done());

  // Next capture is immediately, but underflowed
  auto capture1 = CapturePacket(frames_per_capture);
  loop().RunUntilIdle();
  EXPECT_EQ(capture1->timestamp, packet1_ts);
  EXPECT_EQ(capture1->bytes_captured, bytes_per_capture);
  EXPECT_EQ(capture1->overflow, packet1_ts - packet0_ts - zx::msec(5));
  EXPECT_TRUE(packet1_fence.Done());
}

TEST_F(StreamSinkServerTest, DiscardPackets) {
  const int64_t frames_per_packet = 5;
  const int64_t bytes_per_packet = frames_per_packet * kFormat.bytes_per_frame();

  // Send two packets, then immediately discard.
  const auto packet0_ts = zx::time(0) + zx::msec(100);
  TestFence packet0_fence;
  TestFence packet1_fence;

  {
    SCOPED_TRACE("send packet 0 (explicit timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = 0,
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithSpecified(arena(), packet0_ts.get()), packet0_fence.Take()));
  }

  {
    SCOPED_TRACE("send packet 1 (continuous timestamp)");
    ASSERT_NO_FATAL_FAILURE(PutPacket(
        {
            .buffer_id = 0,
            .offset = static_cast<uint64_t>(bytes_per_packet),
            .size = static_cast<uint64_t>(bytes_per_packet),
        },
        Timestamp::WithUnspecifiedContinuous({}), packet1_fence.Take()));
  }

  loop().RunUntilIdle();
  server().DiscardPackets();
  EXPECT_TRUE(packet0_fence.Done());
  EXPECT_TRUE(packet1_fence.Done());

  // This capture should not complete because the above packets have been discarded.
  auto capture = CapturePacket(frames_per_packet);
  loop().RunUntilIdle();
  EXPECT_FALSE(capture->timestamp);

  // Discarding again should complete the capture with zero bytes.
  server().DiscardPackets();
  EXPECT_TRUE(capture->timestamp);
  EXPECT_EQ(capture->bytes_captured, 0);
}

}  // namespace
}  // namespace media_audio
