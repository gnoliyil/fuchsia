// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dlfcn.h>

#include <cmath>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "examples/media/audio/effects/delay_effect.h"
#include "examples/media/audio/effects/effect_base.h"
#include "examples/media/audio/effects/rechannel_effect.h"
#include "examples/media/audio/effects/swap_effect.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/media/audio/lib/effects_loader/effects_loader.h"
#include "src/media/audio/lib/effects_loader/effects_processor.h"

namespace media::audio_effects_example {

static constexpr const char* kDelayEffectConfig = "{\"delay_frames\": 0}";

//
// Tests EffectsLoader, which directly calls the shared library interface.
//
class EffectsLoaderTest : public testing::Test {
 protected:
  audio::EffectsLoader effects_loader_{"audio_effects_example.so"};

  void SetUp() override { ASSERT_EQ(effects_loader_.LoadLibrary(), ZX_OK); }
};

//
// These child classes may not differentiate, but we use different classes for
// Delay/Rechannel/Swap in order to group related test results accordingly.
//
class DelayEffectTest : public EffectsLoaderTest {
 protected:
  void TestDelayBounds(uint32_t frame_rate, uint32_t channels, uint32_t delay_frames);
};
class RechannelEffectTest : public EffectsLoaderTest {};
class SwapEffectTest : public EffectsLoaderTest {};

// We test the delay effect with certain configuration values, making assumptions
// about how those values relate to the allowed range for this effect.
constexpr uint32_t kTestDelay1 = 1u;
constexpr uint32_t kTestDelay2 = 2u;
static_assert(DelayEffect::kMaxDelayFrames >= kTestDelay2, "Test value too high");
static_assert(DelayEffect::kMinDelayFrames <= kTestDelay1, "Test value too low");

// For the most part, the below tests use a specific channel_count.
constexpr uint16_t kTestChans = 2;

// When testing or using the delay effect, we make certain channel assumptions.
static_assert(DelayEffect::kNumChannelsIn == kTestChans ||
                  DelayEffect::kNumChannelsIn == FUCHSIA_AUDIO_EFFECTS_CHANNELS_ANY,
              "DelayEffect::kNumChannelsIn must match kTestChans");
static_assert(DelayEffect::kNumChannelsOut == kTestChans ||
                  DelayEffect::kNumChannelsOut == FUCHSIA_AUDIO_EFFECTS_CHANNELS_ANY ||
                  DelayEffect::kNumChannelsOut == FUCHSIA_AUDIO_EFFECTS_CHANNELS_SAME_AS_IN,
              "DelayEffect::kNumChannelsOut must match kTestChans");

// When testing or using rechannel effect, we make certain channel assumptions.
static_assert(RechannelEffect::kNumChannelsIn != 2 || RechannelEffect::kNumChannelsOut != 2,
              "RechannelEffect must not be stereo-in/-out");
static_assert(RechannelEffect::kNumChannelsIn != RechannelEffect::kNumChannelsOut &&
                  RechannelEffect::kNumChannelsOut != FUCHSIA_AUDIO_EFFECTS_CHANNELS_ANY &&
                  RechannelEffect::kNumChannelsOut != FUCHSIA_AUDIO_EFFECTS_CHANNELS_SAME_AS_IN,
              "RechannelEffect must not be in-place");

// When testing or using the swap effect, we make certain channel assumptions.
static_assert(SwapEffect::kNumChannelsIn == kTestChans ||
                  SwapEffect::kNumChannelsIn == FUCHSIA_AUDIO_EFFECTS_CHANNELS_ANY,
              "SwapEffect::kNumChannelsIn must match kTestChans");
static_assert(SwapEffect::kNumChannelsOut == kTestChans ||
                  SwapEffect::kNumChannelsOut == FUCHSIA_AUDIO_EFFECTS_CHANNELS_ANY ||
                  SwapEffect::kNumChannelsOut == FUCHSIA_AUDIO_EFFECTS_CHANNELS_SAME_AS_IN,
              "SwapEffect::kNumChannelsOut must match kTestChans");

// Tests the get_parameters ABI, and that the effect behaves as expected.
TEST_F(DelayEffectTest, GetParameters) {
  fuchsia_audio_effects_parameters effect_params;

  uint32_t frame_rate = 48000;
  fuchsia_audio_effects_handle_t effect_handle = effects_loader_.CreateFx(
      Effect::Delay, frame_rate, kTestChans, kTestChans, kDelayEffectConfig);
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  EXPECT_EQ(effects_loader_.FxGetParameters(effect_handle, &effect_params), ZX_OK);
  EXPECT_EQ(effect_params.frame_rate, frame_rate);
  EXPECT_EQ(effect_params.channels_in, kTestChans);
  EXPECT_EQ(effect_params.channels_out, kTestChans);
  EXPECT_TRUE(effect_params.signal_latency_frames == DelayEffect::kLatencyFrames);
  EXPECT_TRUE(effect_params.suggested_frames_per_buffer == DelayEffect::kLatencyFrames);

  // Verify invalid handle
  EXPECT_NE(effects_loader_.FxGetParameters(FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE, &effect_params),
            ZX_OK);

  // Verify null struct*
  EXPECT_NE(effects_loader_.FxGetParameters(effect_handle, nullptr), ZX_OK);

  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests the get_parameters ABI, and that the effect behaves as expected.
TEST_F(RechannelEffectTest, GetParameters) {
  fuchsia_audio_effects_parameters effect_params;

  uint32_t frame_rate = 48000;
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Rechannel, frame_rate, RechannelEffect::kNumChannelsIn,
                               RechannelEffect::kNumChannelsOut, {});
  effect_params.frame_rate = 44100;  // should be overwritten

  EXPECT_EQ(effects_loader_.FxGetParameters(effect_handle, &effect_params), ZX_OK);
  EXPECT_EQ(effect_params.frame_rate, frame_rate);
  EXPECT_TRUE(effect_params.channels_in == RechannelEffect::kNumChannelsIn);
  EXPECT_TRUE(effect_params.channels_out == RechannelEffect::kNumChannelsOut);
  EXPECT_TRUE(effect_params.signal_latency_frames == RechannelEffect::kLatencyFrames);
  EXPECT_TRUE(effect_params.suggested_frames_per_buffer == RechannelEffect::kLatencyFrames);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests the get_parameters ABI, and that the effect behaves as expected.
TEST_F(SwapEffectTest, GetParameters) {
  fuchsia_audio_effects_parameters effect_params;

  uint32_t frame_rate = 44100;
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Swap, frame_rate, kTestChans, kTestChans, {});
  effect_params.frame_rate = 48000;  // should be overwritten

  EXPECT_EQ(effects_loader_.FxGetParameters(effect_handle, &effect_params), ZX_OK);
  EXPECT_EQ(effect_params.frame_rate, frame_rate);
  EXPECT_EQ(effect_params.channels_in, kTestChans);
  EXPECT_EQ(effect_params.channels_out, kTestChans);
  EXPECT_TRUE(effect_params.signal_latency_frames == SwapEffect::kLatencyFrames);
  EXPECT_TRUE(effect_params.suggested_frames_per_buffer == SwapEffect::kLatencyFrames);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

TEST_F(SwapEffectTest, UpdateConfiguration) {
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Swap, 48000, kTestChans, kTestChans, {});
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, {}), ZX_OK);
}

TEST_F(RechannelEffectTest, UpdateConfiguration) {
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Rechannel, 48000, RechannelEffect::kNumChannelsIn,
                               RechannelEffect::kNumChannelsOut, {});
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, {}), ZX_OK);
}

TEST_F(DelayEffectTest, UpdateConfiguration) {
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Delay, 48000, kTestChans, kTestChans, kDelayEffectConfig);
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  // Validate min/max values are accepted.
  EXPECT_EQ(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": 0}"), ZX_OK);
  EXPECT_EQ(
      effects_loader_.FxUpdateConfiguration(
          effect_handle, fxl::StringPrintf("{\"delay_frames\": %u}", DelayEffect::kMaxDelayFrames)),
      ZX_OK);

  // Some invalid configs
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, {}), ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "{}"), ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": -1}"), ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": \"foobar\"}"),
            ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": false}"),
            ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": {}}"), ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": []}"), ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(
                effect_handle,
                fxl::StringPrintf("{\"delay_frames\": %u}", DelayEffect::kMaxDelayFrames + 1)),
            ZX_OK);
  EXPECT_NE(effects_loader_.FxUpdateConfiguration(effect_handle, "[]"), ZX_OK);
}

// Tests the process_inplace ABI, and that the effect behaves as expected.
TEST_F(DelayEffectTest, ProcessInPlace) {
  uint32_t num_samples = 12 * kTestChans;
  uint32_t delay_samples = 6 * kTestChans;
  float delay_buff_in_out[num_samples];
  float expect[num_samples];

  for (uint32_t i = 0; i < delay_samples; ++i) {
    delay_buff_in_out[i] = static_cast<float>(i + 1);
    expect[i] = 0.0f;
  }
  for (uint32_t i = delay_samples; i < num_samples; ++i) {
    delay_buff_in_out[i] = static_cast<float>(i + 1);
    expect[i] = delay_buff_in_out[i - delay_samples];
  }

  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Delay, 48000, kTestChans, kTestChans, kDelayEffectConfig);
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  ASSERT_EQ(effects_loader_.FxUpdateConfiguration(effect_handle, "{\"delay_frames\": 6}"), ZX_OK);
  EXPECT_EQ(effects_loader_.FxProcessInPlace(effect_handle, 4, delay_buff_in_out), ZX_OK);
  EXPECT_EQ(
      effects_loader_.FxProcessInPlace(effect_handle, 4, delay_buff_in_out + (4 * kTestChans)),
      ZX_OK);
  EXPECT_EQ(
      effects_loader_.FxProcessInPlace(effect_handle, 4, delay_buff_in_out + (8 * kTestChans)),
      ZX_OK);

  for (uint32_t sample = 0; sample < num_samples; ++sample) {
    EXPECT_EQ(delay_buff_in_out[sample], expect[sample]) << sample;
  }
  EXPECT_EQ(effects_loader_.FxProcessInPlace(effect_handle, 0, delay_buff_in_out), ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests cases in which we expect process_inplace to fail.
TEST_F(RechannelEffectTest, ProcessInPlace) {
  constexpr uint32_t kNumFrames = 1;
  float buff_in_out[kNumFrames * RechannelEffect::kNumChannelsIn] = {0};

  // Effects that change the channelization should not process in-place.
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Rechannel, 48000, RechannelEffect::kNumChannelsIn,
                               RechannelEffect::kNumChannelsOut, {});
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  EXPECT_NE(effects_loader_.FxProcessInPlace(effect_handle, kNumFrames, buff_in_out), ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests the process_inplace ABI, and that the effect behaves as expected.
TEST_F(SwapEffectTest, ProcessInPlace) {
  constexpr uint32_t kNumFrames = 4;
  float swap_buff_in_out[kNumFrames * kTestChans] = {1.0f, -1.0f, 1.0f, -1.0f,
                                                     1.0f, -1.0f, 1.0f, -1.0f};

  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Swap, 48000, kTestChans, kTestChans, {});
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  EXPECT_EQ(effects_loader_.FxProcessInPlace(effect_handle, kNumFrames, swap_buff_in_out), ZX_OK);
  for (uint32_t sample_num = 0; sample_num < kNumFrames * kTestChans; ++sample_num) {
    EXPECT_EQ(swap_buff_in_out[sample_num], (sample_num % 2 ? 1.0f : -1.0f));
  }

  EXPECT_EQ(effects_loader_.FxProcessInPlace(effect_handle, 0, swap_buff_in_out), ZX_OK);

  // Calls with invalid handle or null buff_ptr should fail.
  EXPECT_NE(effects_loader_.FxProcessInPlace(FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE, kNumFrames,
                                             swap_buff_in_out),
            ZX_OK);
  EXPECT_NE(effects_loader_.FxProcessInPlace(effect_handle, kNumFrames, nullptr), ZX_OK);
  EXPECT_NE(effects_loader_.FxProcessInPlace(effect_handle, 0, nullptr), ZX_OK);

  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests cases in which we expect process to fail.
TEST_F(DelayEffectTest, Process) {
  constexpr uint32_t kNumFrames = 1;
  float audio_buff_in[kNumFrames * kTestChans] = {0.0f};
  float audio_buff_out[kNumFrames * kTestChans] = {0.0f};

  // These stereo-to-stereo effects should ONLY process in-place
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Delay, 48000, kTestChans, kTestChans, kDelayEffectConfig);
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  EXPECT_NE(effects_loader_.FxProcess(effect_handle, kNumFrames, audio_buff_in, audio_buff_out),
            ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests the process ABI, and that the effect behaves as expected.
TEST_F(RechannelEffectTest, Process) {
  constexpr uint32_t kNumFrames = 1;
  float audio_buff_in[kNumFrames * RechannelEffect::kNumChannelsIn] = {
      1.0f, -1.0f, 0.25f, -1.0f, 0.98765432f, -0.09876544f};
  float audio_buff_out[kNumFrames * RechannelEffect::kNumChannelsOut] = {0.0f};
  float expected[kNumFrames * RechannelEffect::kNumChannelsOut] = {0.799536645f, -0.340580851f};

  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Rechannel, 48000, RechannelEffect::kNumChannelsIn,
                               RechannelEffect::kNumChannelsOut, {});
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  EXPECT_EQ(effects_loader_.FxProcess(effect_handle, kNumFrames, audio_buff_in, audio_buff_out),
            ZX_OK);
  EXPECT_EQ(audio_buff_out[0], expected[0]) << std::setprecision(9) << audio_buff_out[0];
  EXPECT_EQ(audio_buff_out[1], expected[1]) << std::setprecision(9) << audio_buff_out[1];

  EXPECT_EQ(effects_loader_.FxProcess(effect_handle, 0, audio_buff_in, audio_buff_out), ZX_OK);

  // Test null effects_handle, buffer_in, buffer_out
  EXPECT_NE(effects_loader_.FxProcess(FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE, kNumFrames,
                                      audio_buff_in, audio_buff_out),
            ZX_OK);
  EXPECT_NE(effects_loader_.FxProcess(effect_handle, kNumFrames, nullptr, audio_buff_out), ZX_OK);
  EXPECT_NE(effects_loader_.FxProcess(effect_handle, kNumFrames, audio_buff_in, nullptr), ZX_OK);
  EXPECT_NE(effects_loader_.FxProcess(effect_handle, 0, nullptr, audio_buff_out), ZX_OK);
  EXPECT_NE(effects_loader_.FxProcess(effect_handle, 0, audio_buff_in, nullptr), ZX_OK);

  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests cases in which we expect process to fail.
TEST_F(SwapEffectTest, Process) {
  constexpr uint32_t kNumFrames = 1;
  float audio_buff_in[kNumFrames * kTestChans] = {0.0f};
  float audio_buff_out[kNumFrames * kTestChans] = {0.0f};

  // These stereo-to-stereo effects should ONLY process in-place
  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Swap, 48000, kTestChans, kTestChans, {});
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  EXPECT_NE(effects_loader_.FxProcess(effect_handle, kNumFrames, audio_buff_in, audio_buff_out),
            ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Tests the process_inplace ABI thru successive in-place calls.
TEST_F(DelayEffectTest, ProcessInPlace_Chain) {
  constexpr uint32_t kNumFrames = 6;

  std::vector<float> buff_in_out = {1.0f,  -0.1f, -0.2f, 2.0f,  0.3f,  -3.0f,
                                    -4.0f, 0.4f,  5.0f,  -0.5f, -0.6f, 6.0f};
  std::vector<float> expected = {0.0f,  0.0f, 0.0f, 0.0f,  0.0f,  0.0f,
                                 -0.1f, 1.0f, 2.0f, -0.2f, -3.0f, 0.3f};

  fuchsia_audio_effects_handle_t delay1_handle, swap_handle, delay2_handle;
  delay1_handle =
      effects_loader_.CreateFx(Effect::Delay, 44100, kTestChans, kTestChans, kDelayEffectConfig);
  swap_handle = effects_loader_.CreateFx(Effect::Swap, 44100, kTestChans, kTestChans, {});
  delay2_handle =
      effects_loader_.CreateFx(Effect::Delay, 44100, kTestChans, kTestChans, kDelayEffectConfig);

  ASSERT_NE(delay1_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  ASSERT_NE(swap_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);
  ASSERT_NE(delay2_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  ASSERT_EQ(effects_loader_.FxUpdateConfiguration(
                delay1_handle, fxl::StringPrintf("{\"delay_frames\": %u}", kTestDelay1)),
            ZX_OK);
  ASSERT_EQ(effects_loader_.FxUpdateConfiguration(
                delay2_handle, fxl::StringPrintf("{\"delay_frames\": %u}", kTestDelay2)),
            ZX_OK);

  EXPECT_EQ(effects_loader_.FxProcessInPlace(delay1_handle, kNumFrames, buff_in_out.data()), ZX_OK);
  EXPECT_EQ(effects_loader_.FxProcessInPlace(swap_handle, kNumFrames, buff_in_out.data()), ZX_OK);
  EXPECT_EQ(effects_loader_.FxProcessInPlace(delay2_handle, kNumFrames, buff_in_out.data()), ZX_OK);

  EXPECT_THAT(buff_in_out, testing::ContainerEq(expected));

  EXPECT_EQ(effects_loader_.FxProcessInPlace(delay2_handle, 0, buff_in_out.data()), ZX_OK);
  EXPECT_EQ(effects_loader_.FxProcessInPlace(swap_handle, 0, buff_in_out.data()), ZX_OK);
  EXPECT_EQ(effects_loader_.FxProcessInPlace(delay1_handle, 0, buff_in_out.data()), ZX_OK);

  EXPECT_EQ(effects_loader_.DeleteFx(delay2_handle), ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(swap_handle), ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(delay1_handle), ZX_OK);
}

// Tests the flush ABI, and effect discards state.
TEST_F(DelayEffectTest, Flush) {
  constexpr uint32_t kNumFrames = 1;
  float buff_in_out[kTestChans] = {1.0f, -1.0f};

  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Delay, 44100, kTestChans, kTestChans,
                               fxl::StringPrintf("{\"delay_frames\": %u}", kTestDelay1));

  ASSERT_EQ(effects_loader_.FxProcessInPlace(effect_handle, kNumFrames, buff_in_out), ZX_OK);
  ASSERT_EQ(buff_in_out[0], 0.0f);

  EXPECT_EQ(effects_loader_.FxFlush(effect_handle), ZX_OK);

  // Validate that cached samples are flushed.
  EXPECT_EQ(effects_loader_.FxProcessInPlace(effect_handle, kNumFrames, buff_in_out), ZX_OK);
  EXPECT_EQ(buff_in_out[0], 0.0f);

  // Verify invalid handle
  EXPECT_NE(effects_loader_.FxFlush(FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE), ZX_OK);
  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

//
// We use this subfunction to test the outer limits allowed by ProcessInPlace.
void DelayEffectTest::TestDelayBounds(uint32_t frame_rate, uint32_t channels,
                                      uint32_t delay_frames) {
  uint32_t delay_samples = delay_frames * channels;
  uint32_t num_frames = frame_rate;
  uint32_t num_samples = num_frames * channels;

  std::vector<float> delay_buff_in_out(num_samples);
  std::vector<float> expect(num_samples);

  fuchsia_audio_effects_handle_t effect_handle =
      effects_loader_.CreateFx(Effect::Delay, frame_rate, channels, channels, kDelayEffectConfig);
  ASSERT_NE(effect_handle, FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE);

  ASSERT_EQ(effects_loader_.FxUpdateConfiguration(
                effect_handle, fxl::StringPrintf("{\"delay_frames\": %u}", delay_frames)),
            ZX_OK);

  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < num_samples; ++i) {
      delay_buff_in_out[i] = static_cast<float>(i + pass * num_samples + 1);
      expect[i] = fmax(delay_buff_in_out[i] - delay_samples, 0.0f);
    }
    ASSERT_EQ(effects_loader_.FxProcessInPlace(effect_handle, num_frames, delay_buff_in_out.data()),
              ZX_OK);

    EXPECT_THAT(delay_buff_in_out, testing::ContainerEq(expect));
  }

  EXPECT_EQ(effects_loader_.DeleteFx(effect_handle), ZX_OK);
}

// Verifies DelayEffect at the outer allowed bounds (largest delays and buffers).
TEST_F(DelayEffectTest, ProcessInPlace_Bounds) {
  TestDelayBounds(192000, 2, DelayEffect::kMaxDelayFrames);
  TestDelayBounds(2000, FUCHSIA_AUDIO_EFFECTS_CHANNELS_MAX, DelayEffect::kMaxDelayFrames);
}

}  // namespace media::audio_effects_example
