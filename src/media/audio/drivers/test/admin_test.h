// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_TEST_ADMIN_TEST_H_
#define SRC_MEDIA_AUDIO_DRIVERS_TEST_ADMIN_TEST_H_

#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/time.h>
#include <zircon/device/audio.h>
#include <zircon/errors.h>

#include <optional>

#include "src/media/audio/drivers/test/test_base.h"

namespace media::audio::drivers::test {

class AdminTest : public TestBase {
 public:
  explicit AdminTest(const DeviceEntry& dev_entry) : TestBase(dev_entry) {}

 protected:
  void TearDown() override;
  void DropRingBuffer();

  void RequestRingBufferChannelWithMinFormat();
  void RequestRingBufferChannelWithMaxFormat();

  void CalculateFrameSize();

  void RequestRingBufferProperties();
  void RequestBuffer(uint32_t min_ring_buffer_frames, uint32_t notifications_per_ring);
  void ActivateChannelsAndExpectSuccess(uint64_t active_channels_bitmask);
  void ActivateChannelsAndExpectFailure(uint64_t active_channels_bitmask);

  void RequestStart();
  void RequestStartAndExpectDisconnect(zx_status_t expected_error);

  void RequestStop();
  void RequestStopAndExpectNoPositionNotifications();
  void RequestStopAndExpectDisconnect(zx_status_t expected_error);

  // Set flag so position notifications (even already-enqueued ones!) cause failures.
  void FailOnPositionNotifications() { fail_on_position_notification_ = true; }
  // Clear flag so position notifications (even already-enqueued ones) do not cause failures.
  void AllowPositionNotifications() { fail_on_position_notification_ = false; }
  void PositionNotificationCallback(fuchsia::hardware::audio::RingBufferPositionInfo position_info);

  void WatchDelayAndExpectUpdate();
  void WatchDelayAndExpectNoUpdate();
  void ValidateInternalDelay();
  void ValidateExternalDelay();
  void ExpectInternalDelayMatchesFifoDepth();
  void ExpectExternalDelayMatchesRingBufferProperties();

  fidl::InterfacePtr<fuchsia::hardware::audio::RingBuffer>& ring_buffer() { return ring_buffer_; }
  uint32_t ring_buffer_frames() const { return ring_buffer_frames_; }
  fuchsia::hardware::audio::PcmFormat pcm_format() const { return pcm_format_; }

  uint32_t notifications_per_ring() const { return notifications_per_ring_; }
  const zx::time& start_time() const { return start_time_; }
  uint16_t frame_size() const { return frame_size_; }

 private:
  void RequestRingBufferChannel();
  void ActivateChannels(uint64_t active_channels_bitmask, bool expect_success);

  fidl::InterfacePtr<fuchsia::hardware::audio::RingBuffer> ring_buffer_;
  std::optional<fuchsia::hardware::audio::RingBufferProperties> ring_buffer_props_;
  std::optional<fuchsia::hardware::audio::DelayInfo> delay_info_;

  uint32_t min_ring_buffer_frames_ = 0;
  uint32_t notifications_per_ring_ = 0;
  uint32_t ring_buffer_frames_ = 0;
  fzl::VmoMapper ring_buffer_mapper_;

  zx::time start_time_;
  fuchsia::hardware::audio::PcmFormat pcm_format_;
  uint16_t frame_size_ = 0;

  // Position notifications are hanging-gets. On receipt, should we register the next one or fail?
  bool fail_on_position_notification_ = false;
};

}  // namespace media::audio::drivers::test

#endif  // SRC_MEDIA_AUDIO_DRIVERS_TEST_ADMIN_TEST_H_
