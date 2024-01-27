// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <fuchsia/ultrasound/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <zircon/device/audio.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <memory>
#include <vector>

#include "src/media/audio/audio_core/testing/integration/hermetic_audio_test.h"
#include "src/media/audio/audio_core/testing/integration/renderer_shim.h"
#include "src/media/audio/lib/clock/testing/clock_test.h"
#include "src/media/audio/lib/clock/utils.h"

namespace media::audio::test {

constexpr uint32_t kUltrasoundSampleRate = 96000;
constexpr uint32_t kUltrasoundChannels = 2;
constexpr uint32_t kBufferSize = kUltrasoundSampleRate;  // 1s buffers

constexpr fuchsia::media::AudioSampleFormat kSampleFormat =
    fuchsia::media::AudioSampleFormat::FLOAT;

static const auto kUltrasoundFormat =
    Format::Create<kSampleFormat>(kUltrasoundChannels, kUltrasoundSampleRate).value();

// This matches the configuration in ultrasound_audio_core_config.json
static const audio_stream_unique_id_t kUltrasoundOutputDeviceId = {{
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
}};
static const audio_stream_unique_id_t kUltrasoundInputDeviceId = {{
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
    0xee,
}};

class UltrasoundTest : public HermeticAudioTest {
 protected:
  static void SetUpTestSuite() {
    HermeticAudioTest::SetTestSuiteRealmOptions([] {
      return HermeticAudioRealm::Options{
          .audio_core_config_data = MakeAudioCoreConfig({
              .output_device_config = R"x(
                "device_id": "*",
                "supported_stream_types": [
                  "render:media",
                  "render:interruption",
                  "render:background",
                  "render:communications",
                  "render:system_agent",
                  "render:ultrasound",
                  "capture:loopback"
                ],
                "pipeline":  {
                  "name": "linearize",
                  "streams": [
                    "render:ultrasound"
                  ],
                  "output_rate": 96000,
                  "output_channels": 2,
                  "inputs": [
                    {
                      "name": "mix",
                      "streams": [
                        "render:media",
                        "render:interruption",
                        "render:background",
                        "render:communications",
                        "render:system_agent"
                      ],
                      "output_rate": 96000,
                      "loopback": true
                    }
                  ]
                }
              )x",
              .input_device_config = R"x(
                "device_id": "*",
                "supported_stream_types": [
                  "capture:background",
                  "capture:foreground",
                  "capture:system_agent",
                  "capture:communications",
                  "capture:ultrasound"
                ],
                "rate": 96000
              )x",
          }),
      };
    });
  }

  VirtualOutput<kSampleFormat>* CreateOutput() {
    return HermeticAudioTest::CreateOutput(kUltrasoundOutputDeviceId, kUltrasoundFormat,
                                           kBufferSize);
  }

  VirtualInput<kSampleFormat>* CreateInput() {
    return HermeticAudioTest::CreateInput(kUltrasoundInputDeviceId, kUltrasoundFormat, kBufferSize);
  }

  UltrasoundRendererShim<kSampleFormat>* CreateRenderer() {
    return HermeticAudioTest::CreateUltrasoundRenderer(kUltrasoundFormat, kBufferSize);
  }

  UltrasoundCapturerShim<kSampleFormat>* CreateCapturer() {
    return HermeticAudioTest::CreateUltrasoundCapturer(kUltrasoundFormat, kBufferSize);
  }
};

TEST_F(UltrasoundTest, CreateRenderer) {
  CreateOutput();
  auto renderer = CreateRenderer();

  clock::testing::VerifyReadOnlyRights(renderer->reference_clock());
  clock::testing::VerifyAdvances(renderer->reference_clock());
  clock::testing::VerifyCannotBeRateAdjusted(renderer->reference_clock());
  clock::testing::VerifyIsSystemMonotonic(renderer->reference_clock());
}

TEST_F(UltrasoundTest, CreateRendererWithoutOutputDevice) {
  // Create a renderer but do not wait for it to fully initialize because there is no device for it
  // to link to yet.
  auto renderer = CreateUltrasoundRenderer(kUltrasoundFormat, kBufferSize,
                                           /* wait_for_creation */ false);

  // Now create an input and capturer. This is just to synchronize with audio_core to verify that
  // the above |CreateRenderer| has been processed. We're relying here on the fact that audio_core
  // will form links synchronously on the FIDL thread as part of the CreateRenderer operation, so
  // if we've linked our Capturer then we know we have not linked our renderer.
  CreateInput();
  CreateCapturer();
  EXPECT_FALSE(renderer->created());

  // Now add the output, which will allow the renderer to be linked.
  CreateOutput();
  renderer->WaitForDevice();
  EXPECT_TRUE(renderer->created());
}

TEST_F(UltrasoundTest, RendererDoesNotSupportSetPcmStreamType) {
  CreateOutput();
  auto renderer = CreateRenderer();

  std::optional<zx_status_t> renderer_error;
  renderer->fidl().set_error_handler([&renderer_error](auto status) { renderer_error = {status}; });

  // Call SetPcmStreamType. We use the current stream type here to ensure we're definitely
  // requesting a supported stream type.
  renderer->fidl()->SetPcmStreamType(kUltrasoundFormat.stream_type());

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&renderer_error] { return renderer_error.has_value(); });
  ASSERT_TRUE(renderer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *renderer_error);
}

TEST_F(UltrasoundTest, RendererDoesNotSupportSetUsage) {
  CreateOutput();
  auto renderer = CreateRenderer();

  std::optional<zx_status_t> renderer_error;
  renderer->fidl().set_error_handler([&renderer_error](auto status) { renderer_error = {status}; });

  renderer->fidl()->SetUsage(fuchsia::media::AudioRenderUsage::MEDIA);

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&renderer_error] { return renderer_error.has_value(); });
  ASSERT_TRUE(renderer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *renderer_error);
}

TEST_F(UltrasoundTest, RendererDoesNotSupportBindGainControl) {
  CreateOutput();
  auto renderer = CreateRenderer();

  std::optional<zx_status_t> renderer_error;
  renderer->fidl().set_error_handler([&renderer_error](auto status) { renderer_error = {status}; });

  fuchsia::media::audio::GainControlPtr gain_control;
  renderer->fidl()->BindGainControl(gain_control.NewRequest());

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&renderer_error] { return renderer_error.has_value(); });
  ASSERT_TRUE(renderer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *renderer_error);
}

TEST_F(UltrasoundTest, RendererDoesNotSupportSetReferenceClock) {
  CreateOutput();
  auto renderer = CreateRenderer();

  std::optional<zx_status_t> renderer_error;
  renderer->fidl().set_error_handler([&renderer_error](auto status) { renderer_error = {status}; });
  renderer->fidl()->SetReferenceClock(clock::DuplicateClock(renderer->reference_clock()));

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&renderer_error] { return renderer_error.has_value(); });
  ASSERT_TRUE(renderer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *renderer_error);
}

TEST_F(UltrasoundTest, CreateCapturer) {
  CreateInput();
  auto capturer = CreateCapturer();

  clock::testing::VerifyReadOnlyRights(capturer->reference_clock());
  clock::testing::VerifyAdvances(capturer->reference_clock());
  clock::testing::VerifyCannotBeRateAdjusted(capturer->reference_clock());
  clock::testing::VerifyIsSystemMonotonic(capturer->reference_clock());
}

TEST_F(UltrasoundTest, CreateCapturerWithoutInputDevice) {
  // Create a capturer but do not wait for it to fully initialize because there is no device for it
  // to link to yet.
  auto capturer = CreateUltrasoundCapturer(kUltrasoundFormat, kBufferSize,
                                           /* wait_for_creation */ false);

  // Now create an output and renderer. This is just to synchronize with audio_core to verify that
  // the above |CreateCapturer| has been processed. We're relying here on the fact that audio_core
  // will form links synchronously on the FIDL thread as part of the CreateCapturer operation, so
  // if we've linked our renderer then we know we have not linked our capturer.
  CreateOutput();
  CreateRenderer();
  EXPECT_FALSE(capturer->created());

  // Now add the input, which will allow the capturer to be linked.
  CreateInput();
  capturer->WaitForDevice();
  EXPECT_TRUE(capturer->created());
}

TEST_F(UltrasoundTest, CapturerDoesNotSupportSetPcmStreamType) {
  CreateInput();
  auto capturer = CreateCapturer();

  std::optional<zx_status_t> capturer_error;
  capturer->fidl().set_error_handler([&capturer_error](auto status) { capturer_error = {status}; });

  // Call SetPcmStreamType. We use the current stream type here to ensure we're definitely
  // requesting a supported stream type.
  capturer->fidl()->SetPcmStreamType(kUltrasoundFormat.stream_type());

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&capturer_error] { return capturer_error.has_value(); });
  ASSERT_TRUE(capturer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *capturer_error);
}

TEST_F(UltrasoundTest, CapturerDoesNotSupportSetUsage) {
  CreateInput();
  auto capturer = CreateCapturer();

  std::optional<zx_status_t> capturer_error;
  capturer->fidl().set_error_handler([&capturer_error](auto status) { capturer_error = {status}; });

  capturer->fidl()->SetUsage(fuchsia::media::AudioCaptureUsage::SYSTEM_AGENT);

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&capturer_error] { return capturer_error.has_value(); });
  ASSERT_TRUE(capturer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *capturer_error);
}

TEST_F(UltrasoundTest, CapturerDoesNotSupportBindGainControl) {
  CreateInput();
  auto capturer = CreateCapturer();

  std::optional<zx_status_t> capturer_error;
  capturer->fidl().set_error_handler([&capturer_error](auto status) { capturer_error = {status}; });

  fuchsia::media::audio::GainControlPtr gain_control;
  capturer->fidl()->BindGainControl(gain_control.NewRequest());

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&capturer_error] { return capturer_error.has_value(); });
  ASSERT_TRUE(capturer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *capturer_error);
}

TEST_F(UltrasoundTest, CapturerDoesNotSupportSetReferenceClock) {
  CreateInput();
  auto capturer = CreateCapturer();

  std::optional<zx_status_t> capturer_error;
  capturer->fidl().set_error_handler([&capturer_error](auto status) { capturer_error = {status}; });
  capturer->fidl()->SetReferenceClock(clock::DuplicateClock(capturer->reference_clock()));

  // Now expect we get disconnected with ZX_ERR_NOT_SUPPORTED.
  RunLoopUntil([&capturer_error] { return capturer_error.has_value(); });
  ASSERT_TRUE(capturer_error);
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *capturer_error);
}

}  // namespace media::audio::test
