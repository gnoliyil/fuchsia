// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/mixer/gain.h"

#include <lib/trace/event.h>

namespace media::audio {

void Gain::Control::SetGainWithRamp(float target_gain_db, zx::duration duration,
                                    fuchsia::media::audio::RampType ramp_type) {
  TRACE_DURATION("audio", "Gain::Control::SetGainWithRamp");
  FX_DCHECK(target_gain_db <= kMaxGainDb)
      << "Gain::Control(" << this << "): " << name_ << ".SetGainWithRamp: target gain ("
      << target_gain_db << " db) cannot exceed maximum (" << kMaxGainDb << " db)";

  if (duration <= zx::nsec(0)) {
    FX_LOGS(WARNING) << "Gain::Control(" << this << "): " << name_
                     << ".SetGainWithRamp non-positive duration (" << duration.to_usecs()
                     << " usec); calling SetGain(" << target_gain_db << " dB)";
    SetGain(target_gain_db);
    return;
  }

  if (target_gain_db == gain_db_) {
    if constexpr (kLogSetRamp) {
      FX_LOGS(WARNING) << "Gain::Control(" << this << "): " << name_
                       << ".SetSourceGainWithRamp is no-change (already " << target_gain_db
                       << " dB); " << duration.to_usecs() << "-usec ramp is ignored";
    }
    ramp_duration_ = zx::nsec(0);
    return;
  }

  if (target_gain_db <= kMinGainDb && gain_db_ <= kMinGainDb) {
    if constexpr (kLogSetRamp) {
      FX_LOGS(WARNING) << "Gain::Control(" << this << "): " << name_
                       << ".SetSourceGainWithRamp starts at (" << gain_db_ << " dB) and ends at ("
                       << target_gain_db << " dB), below min gain (" << kMinGainDb << " dB); "
                       << duration.to_usecs() << "-usec ramp is ignored";
    }
    SetGain(target_gain_db);
    return;
  }

  if constexpr (kLogSetRamp) {
    FX_LOGS(WARNING) << "Gain::Control(" << this << "): " << name_ << ".SetSourceGainWithRamp("
                     << target_gain_db << " dB, " << duration.to_usecs() << " usec)";
  }

  // Start ramping.
  ramp_duration_ = duration;
  frames_ramped_so_far_ = 0;

  ramp_start_gain_db_ = gain_db_;
  ramp_start_scale_ = DbToScale(gain_db_);

  ramp_end_gain_db_ = target_gain_db;
  ramp_end_scale_ = DbToScale(target_gain_db);
}

void Gain::Control::Advance(int64_t num_frames,
                            const TimelineRate& destination_frames_per_reference_tick) {
  TRACE_DURATION("audio", "Gain::Control::Advance");
  if (!IsRamping() || num_frames == 0) {
    return;
  }

  // If the output device's clock is not running, then it isn't possible to
  // convert from output frames to wallclock (local) time.
  FX_CHECK(destination_frames_per_reference_tick.invertible())
      << "Output clock must be running! Numerator of frames/ref_tick is zero";

  frames_ramped_so_far_ += num_frames;

  zx::duration duration_ramped_so_far =
      zx::nsec(destination_frames_per_reference_tick.Inverse().Scale(frames_ramped_so_far_));

  if constexpr (kLogRampAdvance) {
    FX_LOGS(WARNING) << "Gain::Control(" << this << "): " << name_ << ".Advance for ramp ["
                     << ramp_start_gain_db_ << "dB -> " << ramp_end_gain_db_ << "dB for "
                     << ramp_duration_.to_usecs() << " usec];"
                     << " advancing " << num_frames << " frames to "
                     << duration_ramped_so_far.to_usecs() << " usec; total frames ramped is "
                     << frames_ramped_so_far_;
  }

  if (ramp_duration_ > duration_ramped_so_far) {
    // Even after this advance, some duration of ramp remains.
    auto scale_offset = static_cast<double>(duration_ramped_so_far.to_nsecs()) /
                        static_cast<double>(ramp_duration_.to_nsecs()) *
                        (ramp_end_scale_ - ramp_start_scale_);
    AScale scale = static_cast<AScale>(scale_offset + ramp_start_scale_);
    gain_db_ = ScaleToDb(scale);
  } else {
    // This advance takes us beyond the end of ramp.
    ramp_duration_ = zx::nsec(0);
    frames_ramped_so_far_ = 0;
    gain_db_ = ramp_end_gain_db_;
  }

  if constexpr (kLogRampAdvance) {
    FX_LOGS(WARNING) << "Gain::Control(" << this << "): " << name_ << ".Advance gain is now "
                     << gain_db_ << "dB";
  }
}

void Gain::Control::AccumulateScaleArrayForRamp(
    AScale* scale_arr, int64_t num_frames,
    const TimelineRate& destination_frames_per_reference_tick) const {
  FX_CHECK(IsRamping());

  TimelineRate output_to_local = destination_frames_per_reference_tick.Inverse();

  const auto start_scale = ramp_start_scale_;
  const auto end_scale = (ramp_end_scale_ <= kMinScale) ? kMuteScale : ramp_end_scale_;
  const float inverse_source_ramp_duration = 1.0f / static_cast<float>(ramp_duration_.to_nsecs());

  for (int64_t idx = 0; idx < num_frames; ++idx) {
    zx::duration frame_time = zx::nsec(output_to_local.Scale(frames_ramped_so_far_ + idx));
    if (frame_time >= ramp_duration_) {
      scale_arr[idx] *= end_scale;
    } else {
      auto ramp_fraction = static_cast<float>(frame_time.to_nsecs()) * inverse_source_ramp_duration;
      auto scale_factor = start_scale + (end_scale - start_scale) * ramp_fraction;
      scale_arr[idx] *= (scale_factor <= kMinScale) ? kMuteScale : scale_factor;
    }
  }
}

Gain::AScale Gain::CalculateScaleArray(AScale* scale_arr, int64_t num_frames,
                                       const TimelineRate& destination_frames_per_reference_tick) {
  TRACE_DURATION("audio", "Gain::CalculateScaleArray");
  if (num_frames == 0) {
    return GetGainScale();
  }

  FX_CHECK(scale_arr);

  if (!IsRamping()) {
    // Gain is flat for this mix job; retrieve gainscale once and set them all.
    AScale scale = GetGainScale();
    for (int64_t idx = 0; idx < num_frames; ++idx) {
      scale_arr[idx] = scale;
    }
    return scale;
  }

  // Accumulate from Source.
  if (source_.IsRamping()) {
    // Since there is no prior gain control, start with 1.0.
    for (int64_t idx = 0; idx < num_frames; ++idx) {
      scale_arr[idx] = 1.0f;
    }
    source_.AccumulateScaleArrayForRamp(scale_arr, num_frames,
                                        destination_frames_per_reference_tick);
  } else {
    auto db = source_.GainDb();
    auto scale = (db <= kMinGainDb) ? kMuteScale : DbToScale(db);
    for (int64_t idx = 0; idx < num_frames; ++idx) {
      scale_arr[idx] = scale;
    }
  }

  // Accumulate from Dest.
  if (dest_.IsRamping()) {
    dest_.AccumulateScaleArrayForRamp(scale_arr, num_frames, destination_frames_per_reference_tick);
  } else {
    auto db = dest_.GainDb();
    auto scale = (db <= kMinGainDb) ? kMuteScale : DbToScale(db);
    if (scale != 1.0f) {
      for (int64_t idx = 0; idx < num_frames; ++idx) {
        scale_arr[idx] *= scale;
      }
    }
  }

  AScale max_scale = kMuteScale;
  // Apply gain limits; normalize sub-kMinScale values to kMuteScale; return the max scale value.
  for (int64_t idx = 0; idx < num_frames; ++idx) {
    if (scale_arr[idx] <= kMinScale) {
      scale_arr[idx] = kMuteScale;
    } else {
      scale_arr[idx] = std::clamp(scale_arr[idx], min_gain_scale_, max_gain_scale_);
    }
    max_scale = std::max(max_scale, scale_arr[idx]);
  }

  return max_scale;
}

// Calculate a stream's gain-scale multiplier from source and dest gains in
// dB. Optimize to avoid doing the full calculation unless we must.
Gain::AScale Gain::GetGainScale() {
  TRACE_DURATION("audio", "Gain::GetGainScale");

  // Note: mute toggles are not exposed for Dest.
  if (source_.IsMuted()) {
    return kMuteScale;
  }

  auto source_gain_db = source_.GainDb();
  auto dest_gain_db = dest_.GainDb();

  // Use the cached value if nothing changed.
  if (cached_source_gain_db_ == source_gain_db && cached_dest_gain_db_ == dest_gain_db) {
    if constexpr (kLogGainScaleCalculation) {
      FX_LOGS(INFO) << "Gain(" << this
                    << ") reused existing combined gain scale: " << cached_combined_gain_scale_;
    }
    return cached_combined_gain_scale_;
  }

  if (dest_gain_db + source_gain_db == kUnityGainDb) {
    // If sum of the source and dest cancel each other, the combined is kUnityScale.
    cached_combined_gain_scale_ = kUnityScale;
  } else if (source_gain_db <= kMinGainDb || dest_gain_db <= kMinGainDb) {
    // If source or dest are at the mute point, then silence the stream.
    cached_combined_gain_scale_ = kMuteScale;
  } else {
    float effective_gain_db = source_gain_db + dest_gain_db;
    // Likewise, silence the stream if the combined gain is at the mute point.
    if (effective_gain_db <= kMinGainDb) {
      cached_combined_gain_scale_ = kMuteScale;
    } else if (effective_gain_db >= kMaxGainDb) {
      cached_combined_gain_scale_ = kMaxScale;
    } else {
      // Else, we really do need to compute the combined gain-scale.
      cached_combined_gain_scale_ = DbToScale(effective_gain_db);
    }
  }

  // Apply gain limits.
  if (cached_combined_gain_scale_ > kMuteScale) {
    cached_combined_gain_scale_ =
        std::clamp(cached_combined_gain_scale_, min_gain_scale_, max_gain_scale_);
  }

  if constexpr (kLogGainScaleCalculation) {
    FX_LOGS(INFO) << "Gain(" << this << ") new gain_scale: " << cached_combined_gain_scale_;
  }

  // Cache inputs for the next call.
  cached_source_gain_db_ = source_gain_db;
  cached_dest_gain_db_ = dest_gain_db;

  return cached_combined_gain_scale_;
}

}  // namespace media::audio
