// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_CLOCK_UTILS_H_
#define SRC_MEDIA_AUDIO_LIB_CLOCK_UTILS_H_

#include <lib/affine/transform.h>
#include <lib/fpromise/result.h>
#include <lib/zx/clock.h>
#include <zircon/rights.h>
#include <zircon/types.h>

#include <string>

#include "src/media/audio/lib/timeline/timeline_function.h"

namespace media::audio::clock {

zx_status_t GetAndDisplayClockDetails(const zx::clock& ref_clock);
fpromise::result<zx_clock_details_v1_t, zx_status_t> GetClockDetails(const zx::clock& ref_clock);
void DisplayClockDetails(const zx_clock_details_v1_t& clock_details);

constexpr uint32_t kInvalidClockGeneration = 0xFFFFFFFF;
struct ClockSnapshot {
  TimelineFunction reference_to_monotonic;
  uint32_t generation = kInvalidClockGeneration;
};

zx_koid_t GetKoid(const zx::clock& clock);

zx::clock DuplicateClock(const zx::clock& clock, zx_rights_t rights = ZX_RIGHT_SAME_RIGHTS);

fpromise::result<ClockSnapshot, zx_status_t> SnapshotClock(const zx::clock& ref_clock);

fpromise::result<zx::time, zx_status_t> ReferenceTimeFromMonotonicTime(const zx::clock& ref_clock,
                                                                       zx::time mono_time);
fpromise::result<zx::time, zx_status_t> MonotonicTimeFromReferenceTime(const zx::clock& ref_clock,
                                                                       zx::time ref_time);
fpromise::result<zx::time, zx_status_t> ReferenceTimeFromReferenceTime(
    const zx::clock& ref_clock_a, zx::time ref_time_a, const zx::clock& ref_clock_b);

affine::Transform ToAffineTransform(TimelineFunction& tl_function);
TimelineFunction ToTimelineFunction(affine::Transform affine_trans);

}  // namespace media::audio::clock

#endif  // SRC_MEDIA_AUDIO_LIB_CLOCK_UTILS_H_
