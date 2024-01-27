// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/services/mixer/mix/start_stop_control.h"

#include <lib/zx/time.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/media/audio/lib/clock/synthetic_clock_realm.h"
#include "src/media/audio/lib/format2/fixed.h"
#include "src/media/audio/lib/format2/format.h"
#include "src/media/audio/services/common/logging.h"

namespace media_audio {
namespace {

using StreamTime = StartStopControl::StreamTime;
using RealTime = StartStopControl::RealTime;
using StartError = StartStopControl::StartError;
using StopError = StartStopControl::StopError;
using When = StartStopControl::When;
using WhichClock = StartStopControl::WhichClock;

using ::testing::ElementsAre;

// Our tests will convert between durations and frame counts. We choose a frame rate high enough
// that 1 subframe is < 1 ns, to ensure that conversions are invertible. A conversion is
// "invertible" if we can convert from duration T to from count F (rounding with floor), then
// convert from F back to T (rounding with ceil).
//
// Invertibility makes these tests easier to write because we can define constants for T and F (see
// kRefT0 and kFrameAtRefT0) and be guarateed that a conversion in either direction will produce the
// other value.
//
// For example:
//
// If 1 subframe is 2 ns and T = 21ns, then F = floor((21/2)/8192) = 10/8192. The conversion
// back to T is ceil(10*2) = 20, hence this is not invertible.
//
// If 1 subframe is 0.3 ns and T = 10ns, then F = floor((10/0.3)/8192) = 33/8192. The conversion
// back to T is ceil(33*0.3) = ceil(9.9) = 10, hence this is invertible.
const Format kFormat = Format::CreateOrDie({fuchsia_audio::SampleType::kFloat32, 2, 128000});
const auto kFramesPer10ms = Fixed(1280);

// One tick every 2 nanoseconds.
const auto kMediaTicksPerNs = TimelineRate(1, 2);

zx::time ReferenceTimeFromFrame(const StartStopControl& control, Fixed frame) {
  return zx::time(control.presentation_time_to_frac_frame()->ApplyInverse(frame.raw_value()));
}

Fixed FrameFromReferenceTime(const StartStopControl& control, zx::time ref_time) {
  return Fixed::FromRaw(control.presentation_time_to_frac_frame()->Apply(ref_time.get()));
}

class StartStopControlTest : public ::testing::Test {
 public:
  // These two variables represent the same point in time. The reference clock runs 1000PPM slower
  // than the mono clock, so after 1ms, the ref clock has advanced 1us less.
  static inline constexpr zx::time kMonoT0 = zx::time(0) + zx::msec(1);
  static inline constexpr zx::time kRefT0 = zx::time(0) + zx::msec(1) - zx::usec(1);
  static inline constexpr zx::duration kStreamTimeAtT0 = kRefT0 - zx::time(0);
  static inline constexpr int64_t kMediaTicksAtT0 = kStreamTimeAtT0.to_nsecs() / 2;  // 2ns per tick

  // Assuming the frame position starts at Fixed(0) at reference time 0, this is the frame position
  // that overlaps kRefT0.
  static inline const Fixed kFrameAtRefT0 =
      kFormat.frac_frames_per(kRefT0 - zx::time(0), TimelineRate::RoundingMode::Floor);

  StartStopControlTest() {
    clock_->SetRate(-1000);
    clock_snapshots_.AddClock(clock_);
    clock_snapshots_.Update(clock_realm_->now());
  }

  void ExpectStartApplied(const StartStopControl& control,
                          std::optional<fpromise::result<When, StartError>> start_callback_result,
                          When when) {
    EXPECT_TRUE(control.is_started());
    ASSERT_TRUE(control.presentation_time_to_frac_frame().has_value());

    // Check the TimelineFunction at the epoch.
    EXPECT_EQ(ReferenceTimeFromFrame(control, when.frame), when.reference_time);
    EXPECT_EQ(FrameFromReferenceTime(control, when.reference_time), when.frame);

    // Check the TimelineFunction 10ms in the future to validate the slope.
    EXPECT_EQ(ReferenceTimeFromFrame(control, when.frame + kFramesPer10ms),
              when.reference_time + zx::msec(10));
    EXPECT_EQ(FrameFromReferenceTime(control, when.reference_time + zx::msec(10)),
              when.frame + kFramesPer10ms);

    // The callback should have fired.
    ASSERT_TRUE(start_callback_result.has_value());
    ASSERT_TRUE(start_callback_result->is_ok());
    EXPECT_EQ(start_callback_result->value().mono_time, when.mono_time);
    EXPECT_EQ(start_callback_result->value().reference_time, when.reference_time);
    EXPECT_EQ(start_callback_result->value().stream_time, when.stream_time);
    EXPECT_EQ(start_callback_result->value().packet_timestamp, when.packet_timestamp);
    EXPECT_EQ(start_callback_result->value().frame, when.frame);

    // No more pending commands.
    EXPECT_FALSE(control.PendingCommand(clock_snapshots_).has_value());
  }

  void ExpectStopApplied(const StartStopControl& control,
                         std::optional<fpromise::result<When, StopError>> stop_callback_result,
                         When when) {
    EXPECT_FALSE(control.is_started());
    EXPECT_FALSE(control.presentation_time_to_frac_frame().has_value());

    // The callback should have fired.
    ASSERT_TRUE(stop_callback_result.has_value());
    ASSERT_TRUE(stop_callback_result->is_ok());
    EXPECT_EQ(stop_callback_result->value().mono_time, when.mono_time);
    EXPECT_EQ(stop_callback_result->value().reference_time, when.reference_time);
    EXPECT_EQ(stop_callback_result->value().stream_time, when.stream_time);
    EXPECT_EQ(stop_callback_result->value().packet_timestamp, when.packet_timestamp);
    EXPECT_EQ(stop_callback_result->value().frame, when.frame);

    // No more pending commands.
    EXPECT_FALSE(control.PendingCommand(clock_snapshots_).has_value());
  }

 protected:
  const std::shared_ptr<SyntheticClockRealm> clock_realm_ = SyntheticClockRealm::Create();
  const std::shared_ptr<Clock> clock_ =
      clock_realm_->CreateClock("ref_clock", Clock::kExternalDomain, true);

  ClockSnapshots clock_snapshots_;
};

TEST_F(StartStopControlTest, StoppedAfterCreation) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  EXPECT_FALSE(control.is_started());
  EXPECT_FALSE(control.presentation_time_to_frac_frame().has_value());
}

TEST_F(StartStopControlTest, ScheduleStartImmediately) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  std::optional<fpromise::result<When, StartError>> result;
  control.Start({
      .start_time = std::nullopt,
      .stream_time = Fixed(0),
      .callback = [&result](auto r) { result = r; },
  });

  control.AdvanceTo(clock_snapshots_, zx::time(0));

  ExpectStartApplied(control, result,
                     When{
                         .mono_time = zx::time(0),
                         .reference_time = zx::time(0),
                         .stream_time = zx::nsec(0),
                         .packet_timestamp = 0,
                         .frame = Fixed(0),
                     });
}

TEST_F(StartStopControlTest, ScheduleStartInFutureOrPast) {
  const auto expected_when = When{
      .mono_time = kMonoT0,
      .reference_time = kRefT0,
      .stream_time = zx::msec(1),                      // 128 frames = 1ms
      .packet_timestamp = zx::msec(1).to_nsecs() / 2,  // 2ns per tick
      .frame = Fixed(128),
  };

  struct TestCase {
    std::string name;
    RealTime start_time;
    StreamTime stream_time;
  };
  std::vector<TestCase> test_cases = {
      {
          .name = "schedule with SystemMonotonic time and frame position",
          .start_time = {.clock = WhichClock::kSystemMonotonic, .time = kMonoT0},
          .stream_time = expected_when.frame,
      },
      {
          .name = "schedule with Reference time and frame position",
          .start_time = {.clock = WhichClock::kReference, .time = kRefT0},
          .stream_time = expected_when.frame,
      },
      {
          .name = "schedule with Reference time and stream_time",
          .start_time = {.clock = WhichClock::kReference, .time = kRefT0},
          .stream_time = expected_when.stream_time,
      },
      {
          .name = "schedule with Reference time and packet_timestamp",
          .start_time = {.clock = WhichClock::kReference, .time = kRefT0},
          .stream_time = expected_when.packet_timestamp,
      },
  };

  {
    SCOPED_TRACE("ScheduleInFuture");

    for (auto& tc : test_cases) {
      SCOPED_TRACE(tc.name);

      StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

      std::optional<fpromise::result<When, StartError>> result;
      control.Start({
          .start_time = tc.start_time,
          .stream_time = tc.stream_time,
          .callback = [&result](auto r) { result = r; },
      });

      // No change after advancing to before the scheduled time.
      control.AdvanceTo(clock_snapshots_, kRefT0 - zx::nsec(1));
      EXPECT_FALSE(control.is_started());
      EXPECT_FALSE(result.has_value());

      // Applied when advancing to the scheduled time.
      control.AdvanceTo(clock_snapshots_, kRefT0);
      ExpectStartApplied(control, result, expected_when);
    }
  }

  {
    SCOPED_TRACE("ScheduleInPast");

    for (auto& tc : test_cases) {
      SCOPED_TRACE(tc.name);

      StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

      // No change after advancing because nothing is scheduled.
      control.AdvanceTo(clock_snapshots_, kRefT0 + zx::sec(1));
      EXPECT_FALSE(control.is_started());

      // Now schedule a command in the past.
      std::optional<fpromise::result<When, StartError>> result;
      control.Start({
          .start_time = tc.start_time,
          .stream_time = tc.stream_time,
          .callback = [&result](auto r) { result = r; },
      });

      // Applied immediately when advancing.
      control.AdvanceTo(clock_snapshots_, kRefT0 + zx::sec(1) + zx::nsec(1));
      ExpectStartApplied(control, result, expected_when);
    }
  }
}

TEST_F(StartStopControlTest, ScheduleStopImmediately) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  // Initially started.
  control.Start({
      .start_time = RealTime{.clock = WhichClock::kReference, .time = zx::time(0)},
      .stream_time = Fixed(0),
  });
  control.AdvanceTo(clock_snapshots_, zx::time(0));
  EXPECT_TRUE(control.is_started());

  // Schedule a stop to happen immediately.
  std::optional<fpromise::result<When, StopError>> result;
  control.Stop({
      .when = std::nullopt,
      .callback = [&result](auto r) { result = r; },
  });

  control.AdvanceTo(clock_snapshots_, kRefT0);
  ExpectStopApplied(control, result,
                    When{
                        .mono_time = kMonoT0,
                        .reference_time = kRefT0,
                        .stream_time = kStreamTimeAtT0,
                        .packet_timestamp = kMediaTicksAtT0,
                        .frame = kFrameAtRefT0,
                    });
}

TEST_F(StartStopControlTest, ScheduleStopInFutureOrPast) {
  const auto expected_when = When{
      .mono_time = kMonoT0,
      .reference_time = kRefT0,
      .stream_time = kStreamTimeAtT0,
      .packet_timestamp = kMediaTicksAtT0,
      .frame = kFrameAtRefT0,
  };

  struct TestCase {
    std::string name;
    std::variant<RealTime, StreamTime> when;
  };
  std::vector<TestCase> test_cases = {
      {
          .name = "schedule with kSystemMonotonic time",
          .when = RealTime{.clock = WhichClock::kSystemMonotonic, .time = kMonoT0},
      },
      {
          .name = "schedule with Reference time",
          .when = RealTime{.clock = WhichClock::kReference, .time = kRefT0},
      },
      {
          .name = "schedule with frame position",
          .when = StreamTime(kFrameAtRefT0),
      },
      {
          .name = "schedule with stream_time",
          .when = StreamTime(kStreamTimeAtT0),
      },
      {
          .name = "schedule with packet_timestamp",
          .when = StreamTime(kMediaTicksAtT0),
      },
  };

  {
    SCOPED_TRACE("ScheduleInFuture");

    for (auto& tc : test_cases) {
      SCOPED_TRACE(tc.name);

      StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

      // Initially started.
      control.Start({
          .start_time = RealTime{.clock = WhichClock::kReference, .time = zx::time(0)},
          .stream_time = Fixed(0),
      });
      control.AdvanceTo(clock_snapshots_, zx::time(0));
      EXPECT_TRUE(control.is_started());

      // Schedule a stop.
      std::optional<fpromise::result<When, StopError>> result;
      control.Stop({
          .when = tc.when,
          .callback = [&result](auto r) { result = r; },
      });

      // No change after advancing to one frame before the scheduled time.
      control.AdvanceTo(clock_snapshots_, kRefT0 - kFormat.duration_per(Fixed(1)));
      EXPECT_TRUE(control.is_started());
      EXPECT_FALSE(result.has_value());

      // Applied when advancing to the scheduled time.
      control.AdvanceTo(clock_snapshots_, kRefT0);
      ExpectStopApplied(control, result, expected_when);
    }
  }

  {
    SCOPED_TRACE("ScheduleInPast");

    for (auto& tc : test_cases) {
      SCOPED_TRACE(tc.name);

      StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

      // Initially started.
      control.Start({
          .start_time = RealTime{.clock = WhichClock::kReference, .time = zx::time(0)},
          .stream_time = Fixed(0),
      });
      control.AdvanceTo(clock_snapshots_, zx::time(0));
      EXPECT_TRUE(control.is_started());

      // No change after advancing because nothing is scheduled.
      // Advance to one frame before the stop command will be scheduled.
      control.AdvanceTo(clock_snapshots_, kRefT0 - kFormat.duration_per(Fixed(1)));
      EXPECT_TRUE(control.is_started());

      // Now schedule a command in the past.
      std::optional<fpromise::result<When, StopError>> result;
      control.Stop({
          .when = tc.when,
          .callback = [&result](auto r) { result = r; },
      });

      // Applied immediately when advancing.
      control.AdvanceTo(clock_snapshots_, kRefT0 + zx::sec(1) + zx::nsec(1));
      ExpectStopApplied(control, result, expected_when);
    }
  }
}

TEST_F(StartStopControlTest, NullCallbacksDontCrash) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  control.Start({.start_time = std::nullopt, .stream_time = Fixed(0)});
  control.AdvanceTo(clock_snapshots_, zx::time(0));
  EXPECT_TRUE(control.is_started());

  control.Stop({.when = std::nullopt});
  control.AdvanceTo(clock_snapshots_, zx::time(1));
  EXPECT_FALSE(control.is_started());
}

TEST_F(StartStopControlTest, PendingImmediateCommand) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  control.AdvanceTo(clock_snapshots_, zx::time(0));
  control.Start({
      .start_time = std::nullopt,
      .stream_time = Fixed(1),
  });

  auto pending = control.PendingCommand(clock_snapshots_);
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(pending->first.mono_time, zx::time(0));
  EXPECT_EQ(pending->first.reference_time, zx::time(0));
  EXPECT_EQ(pending->first.frame, Fixed(1));
  EXPECT_EQ(pending->second, StartStopControl::CommandType::kStart);
}

TEST_F(StartStopControlTest, PendingStartCommand) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  control.Start({
      .start_time = RealTime{.clock = WhichClock::kReference, .time = kRefT0},
      .stream_time = Fixed(1),
  });

  control.AdvanceTo(clock_snapshots_, zx::time(0));

  auto pending = control.PendingCommand(clock_snapshots_);
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(pending->first.mono_time, kMonoT0);
  EXPECT_EQ(pending->first.reference_time, kRefT0);
  EXPECT_EQ(pending->first.frame, Fixed(1));
  EXPECT_EQ(pending->second, StartStopControl::CommandType::kStart);
}

TEST_F(StartStopControlTest, PendingStopCommand) {
  StartStopControl control(kFormat, kMediaTicksPerNs, UnreadableClock(clock_));

  // Initially started.
  control.Start({
      .start_time = RealTime{.clock = WhichClock::kReference, .time = zx::time(0)},
      .stream_time = Fixed(0),
  });
  control.AdvanceTo(clock_snapshots_, zx::time(0));
  EXPECT_TRUE(control.is_started());

  // Pending Stop.
  control.Stop({
      .when = RealTime{.clock = WhichClock::kReference, .time = kRefT0},
  });
  control.AdvanceTo(clock_snapshots_, zx::time(1));

  auto pending = control.PendingCommand(clock_snapshots_);
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(pending->first.mono_time, kMonoT0);
  EXPECT_EQ(pending->first.reference_time, kRefT0);
  EXPECT_EQ(pending->first.frame, kFrameAtRefT0);
  EXPECT_EQ(pending->second, StartStopControl::CommandType::kStop);
}

}  // namespace
}  // namespace media_audio
