// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/media/cpp/fidl.h>
#include <lib/zx/clock.h>
#include <zircon/device/audio.h>

#include "lib/media/audio/cpp/types.h"
#include "src/media/audio/lib/clock/clone_mono.h"
#include "src/media/audio/lib/clock/testing/clock_test.h"
#include "src/media/audio/lib/test/hermetic_audio_test.h"

namespace media::audio::test {

//
// AudioCapturerTest
//
// This set of tests verifies asynchronous usage of AudioCapturer.
class AudioCapturerTest : public HermeticAudioTest {
 protected:
  void SetUp() override {
    HermeticAudioTest::SetUp();

    audio_core_->CreateAudioCapturer(false, audio_capturer_.NewRequest());
    AddErrorHandler(audio_capturer_, "AudioCapturer");
  }

  void TearDown() override {
    gain_control_.Unbind();
    audio_capturer_.Unbind();

    HermeticAudioTest::TearDown();
  }

  void SetFormat(size_t frames_per_second = 16000) {
    auto t = media::CreateAudioStreamType(fuchsia::media::AudioSampleFormat::SIGNED_16, 1,
                                          frames_per_second);
    format_ = Format::Create(t).take_value();
    audio_capturer_->SetPcmStreamType(t);
  }

  void SetUpPayloadBuffer(size_t num_frames = 16000) {
    zx::vmo audio_capturer_vmo;

    auto status = zx::vmo::create(num_frames * sizeof(int16_t), 0, &audio_capturer_vmo);
    ASSERT_EQ(status, ZX_OK) << "Failed to create payload buffer";

    audio_capturer_->AddPayloadBuffer(0, std::move(audio_capturer_vmo));
  }

  std::optional<Format> format_;
  fuchsia::media::AudioCapturerPtr audio_capturer_;
  fuchsia::media::audio::GainControlPtr gain_control_;
};

class AudioCapturerClockTest : public AudioCapturerTest {
 protected:
  // The clock received from GetRefClock is read-only, but the original can still be adjusted.
  static constexpr auto kClockRights = ZX_RIGHT_DUPLICATE | ZX_RIGHT_TRANSFER | ZX_RIGHT_READ;

  zx::clock GetAndValidateReferenceClock() {
    zx::clock clock;

    audio_capturer_->GetReferenceClock(
        AddCallback("GetReferenceClock",
                    [&clock](zx::clock received_clock) { clock = std::move(received_clock); }));

    ExpectCallback();
    EXPECT_TRUE(clock.is_valid());

    return clock;
  }
};

//
// Test cases
//
// AudioCapturer implements the base classes StreamBufferSet and StreamSource.

// StreamBufferSet methods
//

// TODO(mpuryear): test AddPayloadBuffer(uint32 id, handle<vmo> payload_buffer);
// Also negative testing: bad id, null or bad handle

// TODO(mpuryear): test RemovePayloadBuffer(uint32 id);
// Also negative testing: unknown or already-removed id

// TODO(mpuryear): apply same tests to AudioRenderer and AudioCapturer
// (although their implementations within AudioCore differ somewhat).

// StreamSource methods
//

// TODO(mpuryear): test -> OnPacketProduced(StreamPacket packet);
// Always received for every packet - even malformed ones?

// TODO(mpuryear): test -> OnEndOfStream();
// Also proper sequence vis-a-vis other completion and disconnect callbacks
// Also negative testing: malformed or non-submitted packet, before started
//
// Also capture StreamPacket flags

// DiscardAllPackets waits to deliver its completion callback until all packets have returned.
TEST_F(AudioCapturerTest, DiscardAll_ReturnsAfterAllPackets) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_->CaptureAt(0, 0, 4000, AddCallback("CaptureAt 0"));
  audio_capturer_->CaptureAt(0, 4000, 4000, AddCallback("CaptureAt 4000"));
  audio_capturer_->CaptureAt(0, 8000, 4000, AddCallback("CaptureAt 8000"));
  audio_capturer_->CaptureAt(0, 12000, 4000, AddCallback("CaptureAt 12000"));

  // Packets should complete in strict order, with DiscardAllPackets' completion afterward.
  audio_capturer_->DiscardAllPackets(AddCallback("DiscardAllPackets"));
  ExpectCallback();
}

TEST_F(AudioCapturerTest, DiscardAll_WithNoVmoShouldDisconnect) {
  SetFormat();

  audio_capturer_->DiscardAllPackets(AddUnexpectedCallback("DiscardAllPackets"));
  ExpectDisconnect(audio_capturer_);
}

// DiscardAllPackets should fail, if async capture is active
TEST_F(AudioCapturerTest, DiscardAll_DuringAsyncCaptureShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->DiscardAllPackets(AddUnexpectedCallback("DiscardAllPackets"));
  ExpectDisconnect(audio_capturer_);
}

// DiscardAllPackets should fail, if async capture is in the process of stopping
TEST_F(AudioCapturerTest, DISABLED_DiscardAll_AsyncCaptureStoppingShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->StopAsyncCaptureNoReply();
  audio_capturer_->DiscardAllPackets(AddUnexpectedCallback("DiscardAllPackets"));
  ExpectDisconnect(audio_capturer_);
}

// DiscardAllPackets should succeed, if async capture is completely stopped
TEST_F(AudioCapturerTest, DiscardAll_AfterAsyncCapture) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->StopAsyncCapture(AddCallback("StopAsyncCapture"));
  ExpectCallback();

  audio_capturer_->DiscardAllPackets(AddCallback("DiscardAllPackets"));
  ExpectCallback();
}

// TODO(mpuryear): DiscardAllPacketsNoReply() post-stop
TEST_F(AudioCapturerTest, DiscardAllNoReply_WithNoVmoShouldDisconnect) {
  SetFormat();

  audio_capturer_->DiscardAllPacketsNoReply();
  ExpectDisconnect(audio_capturer_);
}

// DiscardAllPacketsNoReply should fail, if async capture is active
TEST_F(AudioCapturerTest, DiscardAllNoReply_DuringAsyncCaptureShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->DiscardAllPacketsNoReply();
  ExpectDisconnect(audio_capturer_);
}

// DiscardAllPacketsNoReply should fail, if async capture is in the process of stopping
TEST_F(AudioCapturerTest, DISABLED_DiscardAllNoReply_AsyncCaptureStoppingShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->StopAsyncCaptureNoReply();
  audio_capturer_->DiscardAllPacketsNoReply();
  ExpectDisconnect(audio_capturer_);
}

// DiscardAllPacketsNoReply should succeed, if async capture is completely stopped
TEST_F(AudioCapturerTest, DiscardAllNoReply_AfterAsyncCapture) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->StopAsyncCapture(AddCallback("StopAsyncCapture"));
  ExpectCallback();

  audio_capturer_->DiscardAllPacketsNoReply();
  RunLoopUntilIdle();
}

// AudioCapturer methods
//

// TODO(mpuryear): test SetPcmStreamType(AudioStreamType stream_type);
// Also when already set, when packets submitted, when started
// Also negative testing: malformed type

// TODO(mpuryear): test CaptureAt(uint32 id, uint32 offset, uint32 frames)
//                        -> (StreamPacket captured_packet);
// Also when in async capture, before format set, before packets submitted
// Also negative testing: bad id, bad offset, 0/tiny/huge num frames

// TODO(mpuryear): test StartAsyncCapture(uint32 frames_per_packet);
// Also when already started, before format set, before packets submitted
// Also negative testing: 0/tiny/huge num frames (bigger than packet)

TEST_F(AudioCapturerTest, Stop_WhenStoppedShouldDisconnect) {
  audio_capturer_->StopAsyncCapture(AddUnexpectedCallback("StopAsyncCapture"));
  ExpectDisconnect(audio_capturer_);
}
// Also test before format set, before packets submitted

TEST_F(AudioCapturerTest, StopNoReply_WhenStoppedShouldDisconnect) {
  audio_capturer_->StopAsyncCaptureNoReply();
  ExpectDisconnect(audio_capturer_);
}
// Also before format set, before packets submitted

// Test creation and interface independence of GainControl.
// In a number of tests below, we run the message loop to give the AudioCapturer
// or GainControl binding a chance to disconnect, if an error occurred.
TEST_F(AudioCapturerTest, BindGainControl) {
  // Validate AudioCapturers can create GainControl interfaces.
  audio_capturer_->BindGainControl(gain_control_.NewRequest());
  AddErrorHandler(gain_control_, "AudioCapturer::GainControl");

  fuchsia::media::AudioCapturerPtr audio_capturer_2;
  audio_core_->CreateAudioCapturer(true, audio_capturer_2.NewRequest());
  AddErrorHandler(audio_capturer_2, "AudioCapturer2");

  fuchsia::media::audio::GainControlPtr gain_control_2;
  audio_capturer_2->BindGainControl(gain_control_2.NewRequest());
  AddErrorHandler(gain_control_2, "AudioCapturer::GainControl2");

  // What happens to a child gain_control, when a capturer is unbound?
  audio_capturer_.Unbind();

  // What happens to a parent capturer, when a gain_control is unbound?
  gain_control_2.Unbind();

  // Give audio_capturer_ a chance to disconnect gain_control_
  ExpectDisconnect(gain_control_);

  // Give time for other Disconnects to occur, if they must.
  audio_capturer_2->GetStreamType(AddCallback("GetStreamType"));
  ExpectCallback();
}

// Null requests to BindGainControl should have no effect.
TEST_F(AudioCapturerTest, BindGainControlNull) {
  audio_capturer_->BindGainControl(nullptr);

  // Give time for Disconnect to occur, if it must.
  audio_capturer_->GetStreamType(AddCallback("GetStreamType"));
  ExpectCallback();
}

// TODO(mpuryear): test GetStreamType() -> (StreamType stream_type);
// Also negative testing: before format set

//
// Validation of AudioCapturer reference clock methods

// Accept the default clock that is returned if we set no clock
TEST_F(AudioCapturerClockTest, SetRefClock_Default) {
  zx::clock ref_clock = GetAndValidateReferenceClock();

  clock::testing::VerifyReadOnlyRights(ref_clock);
  clock::testing::VerifyIsSystemMonotonic(ref_clock);

  clock::testing::VerifyAdvances(ref_clock);
  clock::testing::VerifyCannotBeRateAdjusted(ref_clock);
}

// Set a null clock; representing selecting the AudioCore-generated optimal clock.
TEST_F(AudioCapturerClockTest, SetRefClock_Optimal) {
  audio_capturer_->SetReferenceClock(zx::clock(ZX_HANDLE_INVALID));
  zx::clock optimal_clock = GetAndValidateReferenceClock();

  clock::testing::VerifyReadOnlyRights(optimal_clock);
  clock::testing::VerifyIsSystemMonotonic(optimal_clock);

  clock::testing::VerifyAdvances(optimal_clock);
  clock::testing::VerifyCannotBeRateAdjusted(optimal_clock);
}

TEST_F(AudioCapturerClockTest, SetRefClock_Custom) {
  // Set a recognizable custom reference clock -- should be what we receive from GetReferenceClock.
  zx::clock dupe_clock, retained_clock, orig_clock = clock::AdjustableCloneOfMonotonic();
  zx::clock::update_args args;
  args.reset().set_rate_adjust(-100);
  ASSERT_EQ(orig_clock.update(args), ZX_OK) << "clock.update with rate_adjust failed";

  ASSERT_EQ(orig_clock.duplicate(kClockRights, &dupe_clock), ZX_OK);
  ASSERT_EQ(orig_clock.duplicate(kClockRights, &retained_clock), ZX_OK);

  audio_capturer_->SetReferenceClock(std::move(dupe_clock));
  zx::clock received_clock = GetAndValidateReferenceClock();

  clock::testing::VerifyReadOnlyRights(received_clock);
  clock::testing::VerifyIsNotSystemMonotonic(received_clock);

  clock::testing::VerifyAdvances(received_clock);
  clock::testing::VerifyCannotBeRateAdjusted(received_clock);

  // We can still rate-adjust our custom clock.
  clock::testing::VerifyCanBeRateAdjusted(orig_clock);
  clock::testing::VerifyAdvances(orig_clock);
}

// inadequate ZX_RIGHTS -- if no TRANSFER, the SetReferenceClock silently does nothing.
// The reference clock should remain the unique recognizable reference clock from before the call.
TEST_F(AudioCapturerClockTest, SetRefClock_NoTransferNoChange) {
  // First create a unique custom clock that we will recognize...
  zx::clock dupe_clock, retained_clock, orig_clock = clock::AdjustableCloneOfMonotonic();
  ASSERT_EQ(orig_clock.duplicate(kClockRights, &dupe_clock), ZX_OK);
  ASSERT_EQ(orig_clock.duplicate(kClockRights, &retained_clock), ZX_OK);

  zx::clock::update_args args;
  args.reset().set_rate_adjust(-100);
  ASSERT_EQ(orig_clock.update(args), ZX_OK) << "clock.update with rate_adjust failed";

  // ... and set it on this capturer.
  audio_capturer_->SetReferenceClock(std::move(dupe_clock));
  zx::clock received_clock = GetAndValidateReferenceClock();
  clock::testing::VerifyIsNotSystemMonotonic(received_clock);

  //
  // Now create another clock without transfer rights...
  zx::clock no_transfer_clock = clock::CloneOfMonotonic();
  ASSERT_TRUE(no_transfer_clock.is_valid());
  ASSERT_EQ(no_transfer_clock.replace(kClockRights & ~ZX_RIGHT_TRANSFER, &no_transfer_clock),
            ZX_OK);
  clock::testing::VerifyIsSystemMonotonic(no_transfer_clock);

  // ... and try to set it as our reference clock...
  audio_capturer_->SetReferenceClock(std::move(no_transfer_clock));
  zx::clock received_clock2 = GetAndValidateReferenceClock();

  // ... but this should not result in any change.
  clock::testing::VerifyReadOnlyRights(received_clock2);
  clock::testing::VerifyIsNotSystemMonotonic(received_clock2);
}

// inadequate ZX_RIGHTS -- no DUPLICATE should cause GetReferenceClock to fail.
TEST_F(AudioCapturerClockTest, SetRefClock_NoDuplicateShouldDisconnect) {
  zx::clock dupe_clock, orig_clock = clock::CloneOfMonotonic();
  ASSERT_EQ(orig_clock.duplicate(kClockRights & ~ZX_RIGHT_DUPLICATE, &dupe_clock), ZX_OK);

  audio_capturer_->SetReferenceClock(std::move(dupe_clock));
  ExpectDisconnect(audio_capturer_);
}

// inadequate ZX_RIGHTS -- no READ should cause GetReferenceClock to fail.
TEST_F(AudioCapturerClockTest, SetRefClock_NoReadShouldDisconnect) {
  zx::clock dupe_clock, orig_clock = clock::CloneOfMonotonic();
  ASSERT_EQ(orig_clock.duplicate(kClockRights & ~ZX_RIGHT_READ, &dupe_clock), ZX_OK);

  audio_capturer_->SetReferenceClock(std::move(dupe_clock));
  ExpectDisconnect(audio_capturer_);
}

// Regardless of the type of clock, calling SetReferenceClock a second time should fail.
TEST_F(AudioCapturerClockTest, SetRefClock_CustomThenOptimalShouldDisconnect) {
  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());

  audio_capturer_->SetReferenceClock(zx::clock(ZX_HANDLE_INVALID));
  ExpectDisconnect(audio_capturer_);
}

// Regardless of the type of clock, calling SetReferenceClock a second time should fail.
TEST_F(AudioCapturerClockTest, SetRefClock_SecondCustomShouldDisconnect) {
  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());

  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// Regardless of the type of clock, calling SetReferenceClock a second time should fail.
TEST_F(AudioCapturerClockTest, SetRefClock_SecondOptimalShouldDisconnect) {
  audio_capturer_->SetReferenceClock(zx::clock(ZX_HANDLE_INVALID));

  audio_capturer_->SetReferenceClock(zx::clock(ZX_HANDLE_INVALID));
  ExpectDisconnect(audio_capturer_);
}

// Regardless of the type of clock, calling SetReferenceClock a second time should fail.
TEST_F(AudioCapturerClockTest, SetRefClock_OptimalThenCustomShouldDisconnect) {
  audio_capturer_->SetReferenceClock(zx::clock(ZX_HANDLE_INVALID));

  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// If client-submitted clock has ZX_RIGHT_WRITE, this should be removed upon GetReferenceClock
TEST_F(AudioCapturerClockTest, GetRefClock_RemovesWriteRight) {
  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());

  zx::clock received_clock = GetAndValidateReferenceClock();
  clock::testing::VerifyReadOnlyRights(received_clock);
}

// You can set the reference clock at any time before the payload buffer is added
TEST_F(AudioCapturerClockTest, SetRefClock_BeforeBuffer) {
  SetFormat();

  audio_capturer_->SetReferenceClock(zx::clock(ZX_HANDLE_INVALID));
  GetAndValidateReferenceClock();
}

// Setting the reference clock should fail, once payload buffer has been added
TEST_F(AudioCapturerClockTest, SetRefClock_AfterBufferShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// Setting the reference clock should fail, once payload buffer has been added
TEST_F(AudioCapturerClockTest, SetRefClock_DuringCaptureShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_->CaptureAt(0, 0, 8000, [](fuchsia::media::StreamPacket) { FAIL(); });

  audio_capturer_->SetReferenceClock(clock::CloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// Setting the reference clock should fail, even after all active capture packets have returned
TEST_F(AudioCapturerClockTest, SetRefClock_AfterCaptureShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_->CaptureAt(0, 0, 8000, AddCallback("CaptureAt"));
  ExpectCallback();

  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// Setting the reference clock should fail, any time after capture has started (even if cancelled)
TEST_F(AudioCapturerClockTest, SetRefClock_CaptureCancelledShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_->CaptureAt(0, 0, 8000, [](fuchsia::media::StreamPacket) {});
  audio_capturer_->DiscardAllPackets(AddCallback("DiscardAllPackets"));
  ExpectCallback();

  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// Setting the reference clock should fail, if at least one capture packet is active
TEST_F(AudioCapturerClockTest, SetRefClock_DuringAsyncCaptureShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->SetReferenceClock(clock::CloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

// Setting the reference clock should fail, any time after capture has started (even if stopped)
TEST_F(AudioCapturerClockTest, SetRefClock_AfterAsyncCaptureShouldDisconnect) {
  SetFormat();
  SetUpPayloadBuffer();

  audio_capturer_.events().OnPacketProduced = AddCallback("OnPacketProduced");
  audio_capturer_->StartAsyncCapture(1600);
  ExpectCallback();

  audio_capturer_->StopAsyncCapture(AddCallback("StopAsyncCapture"));
  ExpectCallback();

  audio_capturer_->SetReferenceClock(clock::AdjustableCloneOfMonotonic());
  ExpectDisconnect(audio_capturer_);
}

}  // namespace media::audio::test
