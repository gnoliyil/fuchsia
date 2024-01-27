// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/media/playback/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/time.h>

#include <array>
#include <memory>
#include <optional>
#include <queue>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "lib/async-loop/cpp/loop.h"
#include "lib/media/cpp/timeline_function.h"
#include "lib/media/cpp/timeline_rate.h"
#include "lib/media/cpp/type_converters.h"
#include "lib/sync/condition.h"
#include "lib/sys/component/cpp/testing/realm_builder_types.h"
#include "lib/ui/scenic/cpp/view_token_pair.h"
#include "lib/zx/time.h"
#include "src/lib/fsl/io/fd.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/media/playback/mediaplayer/audio_consumer_impl.h"
#include "src/media/playback/mediaplayer/test/fakes/fake_audio.h"
#include "src/media/playback/mediaplayer/test/sink_feeder.h"
#include "zircon/status.h"

namespace media_player {
namespace test {

static constexpr uint16_t kSamplesPerFrame = 2;      // Stereo
static constexpr uint32_t kFramesPerSecond = 48000;  // 48kHz
static constexpr size_t kVmoSize = 1024;
static constexpr uint32_t kNumVmos = 4;

// Base class for audio consumer tests.
class AudioConsumerTests : public gtest::RealLoopFixture {
 protected:
  void SetUp() override {
    using namespace component_testing;
    fuchsia_logging::SetTags({"mediaplayer"});
    auto realm_builder = RealmBuilder::Create();
    // Configure the realm.
    // ...
    realm_builder.AddChild("mediaplayer", "#meta/mediaplayer.cm");
    realm_builder.AddLocalChild("audiocore",
                                [this]() { return std::move(fake_audio_core_owned_); });
    realm_builder.AddLocalChild("audio", [this]() { return std::move(fake_audio_owned_); });
    realm_builder.AddChild("scenic",
                           "fuchsia-pkg://fuchsia.com/test-ui-stack#meta/test-ui-stack.cm");
    // My additions of scenic and audio
    // Route fuchsia.media.AudioCore from audiocore child to mediaplayer child
    realm_builder.AddRoute(Route{.capabilities = {Protocol{fuchsia::media::AudioCore::Name_}},
                                 .source = ChildRef{"audiocore"},
                                 .targets = {ChildRef{"mediaplayer"}}});
    // Route fuchsia.media.Audio from audio_main child to mediaplayer child
    realm_builder.AddRoute(Route{.capabilities = {Protocol{fuchsia::media::Audio::Name_}},
                                 .source = ChildRef{"audio"},
                                 .targets = {ChildRef{"mediaplayer"}}});
    // Route fuchsia.media.SessionAudioConsumerFactory up to this test component
    realm_builder.AddRoute(
        Route{.capabilities = {Protocol{fuchsia::media::SessionAudioConsumerFactory::Name_}},
              .source = ChildRef{"mediaplayer"},
              .targets = {ParentRef{}}});
    // Route protocols to scenic
    realm_builder.AddRoute(Route{.capabilities =
                                     {
                                         Protocol{"fuchsia.logger.LogSink"},
                                         Protocol{"fuchsia.scheduler.ProfileProvider"},
                                         Protocol{"fuchsia.sysmem.Allocator"},
                                         Protocol{"fuchsia.tracing.provider.Registry"},
                                         Protocol{"fuchsia.vulkan.loader.Loader"},
                                     },
                                 .source = ParentRef{},
                                 .targets = {ChildRef{"scenic"}}});
    // Route scenic from test_ui_stack to mediaplayer child
    realm_builder.AddRoute(Route{.capabilities = {Protocol{"fuchsia.ui.scenic.Scenic"}},
                                 .source = ChildRef{"scenic"},
                                 .targets = {ChildRef{"mediaplayer"}}});

    // Route services fuchsia.logger.LogSink, fuchsia.tracing.provider.Registry,
    // fuchsia.scheduler.ProfileProvider from parent to children mediaplayer, audiocore
    realm_builder.AddRoute(Route{.capabilities =
                                     {
                                         Protocol{"fuchsia.logger.LogSink"},
                                         Protocol{"fuchsia.tracing.provider.Registry"},
                                         Protocol{"fuchsia.sysmem.Allocator"},
                                         Protocol{"fuchsia.scheduler.ProfileProvider"},
                                         Protocol{"fuchsia.mediacodec.CodecFactory"},
                                     },
                                 .source = ParentRef(),
                                 .targets = {ChildRef{"mediaplayer"}, ChildRef{"audiocore"}}});
    // services->AllowParentService("fuchsia.logger.LogSink");
    realm_ = realm_builder.Build(dispatcher());
    auto session_audio_consumer_factory =
        realm_->component().ConnectSync<fuchsia::media::SessionAudioConsumerFactory>();
    ASSERT_EQ(ZX_OK, session_audio_consumer_factory->CreateAudioConsumer(
                         0, audio_consumer_.NewRequest(dispatcher())));
    EXPECT_EQ(true, audio_consumer_.is_bound());
    audio_consumer_.set_error_handler([this](zx_status_t status) {
      FX_LOGS(ERROR) << "Audio consumer connection closed, status " << zx_status_get_string(status)
                     << ".";
      audio_consumer_connection_closed_ = true;
      QuitLoop();
    });
  }

  void TearDown() override {
    EXPECT_FALSE(audio_consumer_connection_closed_);
    // Wait for realm to teardown before fakes, so we don't end up with crashes that lead to flakes.
    bool done = false;
    realm_->Teardown([&](fit::result<fuchsia::component::Error> err) { done = true; });
    RunLoopUntil([&]() { return done; });
  }

  void StartWatcher() {
    audio_consumer_->WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
      got_status_ = true;
      last_status_ = std::move(status);
      StartWatcher();
    });
  }

  fuchsia::media::AudioConsumerPtr audio_consumer_;
  bool audio_consumer_connection_closed_ = false;
  bool got_status_ = false;
  fuchsia::media::AudioConsumerStatus last_status_;
  std::unique_ptr<FakeAudioCore> fake_audio_core_owned_{
      std::make_unique<FakeAudioCore>(dispatcher())};
  FakeAudioCore* fake_audio_core_{fake_audio_core_owned_.get()};
  std::unique_ptr<FakeAudio> fake_audio_owned_{std::make_unique<FakeAudio>(dispatcher())};
  std::optional<component_testing::RealmRoot> realm_;
};

// Test that factory channel is closed and we still have a connection to the created AudioConsumer
TEST_F(AudioConsumerTests, FactoryClosed) {
  got_status_ = false;
  audio_consumer_.events().WatchStatus(
      [this](fuchsia::media::AudioConsumerStatus status) { got_status_ = true; });
  RunLoopUntil([this]() { return got_status_; });
  // FX_LOGS(ERROR) << "-------RunLoopUntil function has been ran-------";
  EXPECT_FALSE(audio_consumer_connection_closed_);
}

TEST_F(AudioConsumerTests, ConsumerClosed) {
  bool factory_closed = false;
  fuchsia::media::AudioConsumerPtr audio_consumer2;
  // Instantiate the audio consumer under test.
  auto session_audio_consumer_factory =
      realm_->component().Connect<fuchsia::media::SessionAudioConsumerFactory>();
  session_audio_consumer_factory.set_error_handler(
      [&factory_closed](zx_status_t status) { factory_closed = true; });
  {
    fuchsia::media::AudioStreamType stream_type;
    stream_type.frames_per_second = kFramesPerSecond;
    stream_type.channels = kSamplesPerFrame;
    stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
    fuchsia::media::StreamSinkPtr sink;
    fuchsia::media::AudioConsumerPtr audio_consumer;
    bool sink_connection_closed = false;
    session_audio_consumer_factory->CreateAudioConsumer(0, audio_consumer.NewRequest(dispatcher()));
    audio_consumer.set_error_handler(
        [this](zx_status_t status) { audio_consumer_connection_closed_ = true; });
    auto compression = std::make_unique<fuchsia::media::Compression>();
    compression->type = fuchsia::media::AUDIO_ENCODING_AACLATM;
    std::vector<zx::vmo> vmos(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
      EXPECT_EQ(status, ZX_OK);
    }
    audio_consumer.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
      EXPECT_FALSE(status.has_presentation_timeline());
      got_status_ = true;
    });
    got_status_ = false;
    RunLoopUntil([this]() { return got_status_; });
    audio_consumer->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                     sink.NewRequest(dispatcher()));
    sink.set_error_handler(
        [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
    audio_consumer->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                          fuchsia::media::NO_TIMESTAMP);
    audio_consumer->Stop();
    audio_consumer.events().WatchStatus(
        [this](fuchsia::media::AudioConsumerStatus status) { got_status_ = true; });
    got_status_ = false;
    RunLoopUntil([this]() { return got_status_; });
    session_audio_consumer_factory->CreateAudioConsumer(0,
                                                        audio_consumer2.NewRequest(dispatcher()));
  }
  audio_consumer2.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
    EXPECT_FALSE(status.has_presentation_timeline());
    got_status_ = true;
  });
  got_status_ = false;
  RunLoopUntil([this]() { return got_status_; });
  EXPECT_FALSE(factory_closed);
}

// // Test packet flow of AudioConsumer interface by using a synthetic environment
// // to push a packet through and checking that it is processed.
TEST_F(AudioConsumerTests, CreateStreamSink) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  got_status_ = false;
  auto compression = std::make_unique<fuchsia::media::Compression>();
  compression->type = fuchsia::media::AUDIO_ENCODING_AACLATM;
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }
  audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
    EXPECT_FALSE(status.has_presentation_timeline());
    got_status_ = true;
  });
  RunLoopUntil([this]() { return got_status_; });
  got_status_ = false;
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                    sink.NewRequest(dispatcher()));
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);
  audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
    EXPECT_TRUE(status.has_presentation_timeline());
    // test things are progressing
    EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
    got_status_ = true;
  });
  RunLoopUntil([this]() { return got_status_; });
  auto packet = std::make_unique<fuchsia::media::StreamPacket>();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = fuchsia::media::NO_TIMESTAMP;
  bool sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
  RunLoopUntil([&sent_packet]() { return sent_packet; });
  EXPECT_TRUE(sent_packet);
  EXPECT_FALSE(sink_connection_closed);
}

TEST_F(AudioConsumerTests, SetRate) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  got_status_ = false;
  auto compression = std::make_unique<fuchsia::media::Compression>();
  compression->type = fuchsia::media::AUDIO_ENCODING_AACLATM;
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }
  StartWatcher();
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                    sink.NewRequest(dispatcher()));
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  // clear initial
  RunLoopUntil([this]() { return got_status_; });
  got_status_ = false;
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN,
                         /* reference_time= */ 0,
                         /* media_time= */ 0);
  RunLoopUntil([this]() { return got_status_ && last_status_.has_presentation_timeline(); });
  // default rate should be 1
  EXPECT_EQ(last_status_.presentation_timeline().subject_delta, 1u);
  got_status_ = false;
  // Send a packet through to test subject time updating
  auto packet = fuchsia::media::StreamPacket::New();
  packet->payload_buffer_id = 0;
  packet->payload_size = 1;
  packet->payload_offset = 0;
  packet->pts = 0;
  bool sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
  RunLoopUntil([&sent_packet]() { return sent_packet; });
  EXPECT_TRUE(sent_packet);
  audio_consumer_->SetRate(0.0f);
  RunLoopUntil([this]() { return got_status_ && last_status_.has_presentation_timeline(); });
  EXPECT_EQ(last_status_.presentation_timeline().subject_delta, 0u);
  // Time should be moved forward from initial
  EXPECT_GT(last_status_.presentation_timeline().subject_time, 0);
  EXPECT_GT(last_status_.presentation_timeline().reference_time, 0);
  got_status_ = false;
  audio_consumer_->SetRate(1.0f);
  RunLoopUntil([this]() { return got_status_ && last_status_.has_presentation_timeline(); });
  EXPECT_EQ(last_status_.presentation_timeline().subject_delta, 1u);
  got_status_ = false;
  EXPECT_FALSE(sink_connection_closed);
}

// // Test that error is generated when unsupported codec is specified
TEST_F(AudioConsumerTests, UnsupportedCodec) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  got_status_ = false;
  auto compression = std::make_unique<fuchsia::media::Compression>();
  compression->type = "not a real compression type";
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                    sink.NewRequest(dispatcher()));
  sink.set_error_handler([&sink_connection_closed](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
    sink_connection_closed = true;
  });
  RunLoopUntil([&sink_connection_closed]() { return sink_connection_closed; });
}

// // Test expected behavior of AudioConsumer interface when no compression type is
// // set when creating a StreamSink
TEST_F(AudioConsumerTests, NoCompression) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  got_status_ = false;
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr,
                                    sink.NewRequest(dispatcher()));
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);
  audio_consumer_.events().WatchStatus(
      [this](fuchsia::media::AudioConsumerStatus status) { got_status_ = true; });
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  RunLoopUntil([this]() { return got_status_; });
  EXPECT_TRUE(got_status_);
  EXPECT_FALSE(sink_connection_closed);
}

// Test that creating multiple StreamSink's back to back results in both
// returned sinks functioning correctly
TEST_F(AudioConsumerTests, MultipleSinks) {
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  {
    got_status_ = false;
    fuchsia::media::StreamSinkPtr sink;
    std::vector<zx::vmo> vmos(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
      EXPECT_EQ(status, ZX_OK);
    }
    auto compression = std::make_unique<fuchsia::media::Compression>();
    compression->type = fuchsia::media::AUDIO_ENCODING_LPCM;
    audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                      sink.NewRequest());
    audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                           fuchsia::media::NO_TIMESTAMP);
    fuchsia::media::AudioConsumerStatus received_status;
    audio_consumer_.events().WatchStatus(
        [this, &received_status](fuchsia::media::AudioConsumerStatus status) {
          received_status = std::move(status);
          // test things are progressing
          got_status_ = true;
        });
    RunLoopUntil([this, &received_status]() {
      return got_status_ && received_status.has_presentation_timeline();
    });
    // test things are progressing
    EXPECT_EQ(received_status.presentation_timeline().subject_delta, 1u);
    got_status_ = false;
  }
  audio_consumer_->Stop();
  {
    fuchsia::media::StreamSinkPtr sink;
    std::vector<zx::vmo> vmos(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
      EXPECT_EQ(status, ZX_OK);
    }
    auto compression = std::make_unique<fuchsia::media::Compression>();
    compression->type = fuchsia::media::AUDIO_ENCODING_LPCM;
    audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                      sink.NewRequest());
    audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                           fuchsia::media::NO_TIMESTAMP);
    fuchsia::media::AudioConsumerStatus received_status;
    audio_consumer_.events().WatchStatus(
        [this, &received_status](fuchsia::media::AudioConsumerStatus status) {
          received_status = std::move(status);
          // test things are progressing
          got_status_ = true;
        });
    RunLoopUntil([this, &received_status]() {
      return got_status_ && received_status.has_presentation_timeline();
    });
    // test things are progressing
    EXPECT_EQ(received_status.presentation_timeline().subject_delta, 1u);
    EXPECT_TRUE(got_status_);
  }
}

// Test that multiple stream sinks can be created at the same time, but packets
// can only be sent on the most recently active one. Also test that packets can
// be queued on the 'pending' sink.
TEST_F(AudioConsumerTests, OverlappingStreamSink) {
  fuchsia::media::StreamSinkPtr sink2;
  bool sink2_packet = false;
  got_status_ = false;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  auto packet = std::make_unique<fuchsia::media::StreamPacket>();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = fuchsia::media::NO_TIMESTAMP;
  {
    fuchsia::media::StreamSinkPtr sink1;
    auto compression1 = std::make_unique<fuchsia::media::Compression>();
    compression1->type = fuchsia::media::AUDIO_ENCODING_LPCM;
    auto compression2 = std::make_unique<fuchsia::media::Compression>();
    compression2->type = fuchsia::media::AUDIO_ENCODING_LPCM;
    std::vector<zx::vmo> vmos1(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos1[i]);
      EXPECT_EQ(status, ZX_OK);
    }
    std::vector<zx::vmo> vmos2(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos2[i]);
      EXPECT_EQ(status, ZX_OK);
    }
    audio_consumer_->CreateStreamSink(std::move(vmos1), stream_type, std::move(compression1),
                                      sink1.NewRequest());
    audio_consumer_->CreateStreamSink(std::move(vmos2), stream_type, std::move(compression2),
                                      sink2.NewRequest());
    audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                           fuchsia::media::NO_TIMESTAMP);
    audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
      EXPECT_TRUE(status.has_presentation_timeline());
      // test things are progressing
      EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
      got_status_ = true;
    });
    bool sink1_packet = false;
    sink1->SendPacket(*packet, [&sink1_packet]() { sink1_packet = true; });
    RunLoopUntil([&sink1_packet]() { return sink1_packet; });
    EXPECT_TRUE(sink1_packet);
    EXPECT_FALSE(sink2_packet);
  }
  // sink 1 dropped, now should be getting packets flowing from sink2
  sink2->SendPacket(*packet, [&sink2_packet]() { sink2_packet = true; });
  RunLoopUntil([&sink2_packet]() { return sink2_packet; });
  EXPECT_TRUE(sink2_packet);
}

// Test that packet timestamps are properly transformed from input rate of
// nanoseconds to the renderer rate of frames
TEST_F(AudioConsumerTests, CheckPtsRate) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  fake_audio_core_->renderer().ExpectPackets({{kFramesPerSecond, kVmoSize, 0x0000000000000000}});
  got_status_ = false;
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  RunLoopUntilIdle();
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);
  audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
    EXPECT_TRUE(status.has_presentation_timeline());
    // test things are progressing
    EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
    got_status_ = true;
  });
  RunLoopUntil([this]() { return got_status_; });
  auto packet = std::make_unique<fuchsia::media::StreamPacket>();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = ZX_SEC(1);
  bool sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
  RunLoopUntil([&sent_packet]() { return sent_packet; });
  RunLoopUntil([this]() { return fake_audio_core_->renderer().expected(); });
  EXPECT_FALSE(sink_connection_closed);
}

// Test that packet buffers are consumed in the order they were supplied
TEST_F(AudioConsumerTests, BufferOrdering) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  fake_audio_core_->renderer().ExpectPackets(
      {{0, kVmoSize, 0x0000000000000000}, {kFramesPerSecond / 1000, kVmoSize, 0xa844a65edadbefbf}});
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    std::array<char, 1> test_data = {static_cast<char>(i)};
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
    vmos[i].write(test_data.data(), 0, test_data.size());
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);
  auto packet = std::make_unique<fuchsia::media::StreamPacket>();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = 0;
  bool sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
  RunLoopUntil([&sent_packet]() { return sent_packet; });
  packet = std::make_unique<fuchsia::media::StreamPacket>();
  packet->payload_buffer_id = 1;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = ZX_MSEC(1);
  sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
  RunLoopUntil([&sent_packet]() { return sent_packet; });
  RunLoopUntil([this]() { return fake_audio_core_->renderer().expected(); });
  EXPECT_FALSE(sink_connection_closed);
}

// Test that status reports flow correctly when client always requeues watch requests
TEST_F(AudioConsumerTests, StatusLoop) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  StartWatcher();
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    std::array<char, 1> test_data = {static_cast<char>(i)};
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
    vmos[i].write(test_data.data(), 0, test_data.size());
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  RunLoopUntil([this]() { return got_status_; });
  got_status_ = false;
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);
  RunLoopUntil([this]() { return got_status_ && last_status_.has_presentation_timeline(); });
  // test things are progressing
  EXPECT_EQ(last_status_.presentation_timeline().subject_delta, 1u);
  EXPECT_FALSE(sink_connection_closed);
}

// Test that packet discard returns packets to client
TEST_F(AudioConsumerTests, DiscardAllPackets) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    std::array<char, 1> test_data = {static_cast<char>(i)};
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
    vmos[i].write(test_data.data(), 0, test_data.size());
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  auto packet = std::make_unique<fuchsia::media::StreamPacket>();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = 0;
  bool sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
  // no start
  RunLoopUntilIdle();
  EXPECT_FALSE(sent_packet);
  sink->DiscardAllPacketsNoReply();
  RunLoopUntil([&sent_packet]() { return sent_packet; });
  EXPECT_FALSE(sink_connection_closed);
}

// Test that packets are rendered even if timeline is started before Start is called. Use media time
// offset from zero to expose issues with an initial internal media time of 0.
TEST_F(AudioConsumerTests, SetRateBeforeStart) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  constexpr uint32_t kBytesPerFrame = kSamplesPerFrame * sizeof(int16_t);
  bool sink_connection_closed = false;
  fake_audio_core_->renderer().ExpectPackets({{255, kVmoSize, 0x0000000000000000},
                                              {511, kVmoSize, 0x0000000000000000},
                                              {768, kVmoSize, 0x0000000000000000},
                                              {1023, kVmoSize, 0x0000000000000000},
                                              {1279, kVmoSize, 0x0000000000000000},
                                              {1536, kVmoSize, 0x0000000000000000},
                                              {1791, kVmoSize, 0x0000000000000000},
                                              {2047, kVmoSize, 0x0000000000000000},
                                              {2304, kVmoSize, 0x0000000000000000},
                                              {2559, kVmoSize, 0x0000000000000000}});
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    std::array<char, 1> test_data = {static_cast<char>(i)};
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
    vmos[i].write(test_data.data(), 0, test_data.size());
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());
  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });
  audio_consumer_->SetRate(1.0f);
  auto pts_rate = media::TimelineRate(ZX_SEC(1), kFramesPerSecond);
  for (int32_t i = 0; i < 10; i++) {
    auto packet = fuchsia::media::StreamPacket::New();
    packet->payload_buffer_id = 0;
    packet->payload_size = kVmoSize;
    packet->payload_offset = 0;
    packet->pts = (i + 1) * kVmoSize / kBytesPerFrame * pts_rate;
    bool sent_packet = false;
    sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });
    RunLoopUntil([&sent_packet]() { return sent_packet; });
  }
  auto initial_pts = kVmoSize / kBytesPerFrame * pts_rate;
  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0, initial_pts);
  RunLoopUntil([this]() { return fake_audio_core_->renderer().expected(); });
  EXPECT_FALSE(sink_connection_closed);
}

TEST_F(AudioConsumerTests, VolumeControl) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::audio::VolumeControlPtr volume_control;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    std::array<char, 1> test_data = {static_cast<char>(i)};
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
    vmos[i].write(test_data.data(), 0, test_data.size());
  }
  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());
  audio_consumer_->BindVolumeControl(volume_control.NewRequest());
  volume_control->SetVolume(0.5f);
  volume_control->SetMute(true);
  RunLoopUntil([this]() {
    return fake_audio_core_->renderer().gain() == -20.0f && fake_audio_core_->renderer().mute();
  });
  EXPECT_EQ(fake_audio_core_->renderer().gain(), -20.0f);
  EXPECT_EQ(fake_audio_core_->renderer().mute(), true);
}

}  // namespace test
}  // namespace media_player
