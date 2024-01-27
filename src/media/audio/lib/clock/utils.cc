// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/lib/clock/utils.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/syscalls.h>

#include <cmath>

namespace media::audio::clock {

zx_status_t GetAndDisplayClockDetails(const zx::clock& ref_clock) {
  auto result = GetClockDetails(ref_clock);
  if (result.is_error()) {
    return result.error();
  }

  DisplayClockDetails(result.take_value());
  return ZX_OK;
}

fpromise::result<zx_clock_details_v1_t, zx_status_t> GetClockDetails(const zx::clock& ref_clock) {
  if (!ref_clock.is_valid()) {
    FX_LOGS(INFO) << "Clock is invalid";
    return fpromise::error(ZX_ERR_INVALID_ARGS);
  }

  zx_clock_details_v1_t clock_details;
  zx_status_t status = ref_clock.get_details(&clock_details);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Error calling zx::clock::get_details";
    return fpromise::error(status);
  }

  return fpromise::ok(clock_details);
}

// Only called by custom code when debugging, so can remain at INFO severity.
void DisplayClockDetails(const zx_clock_details_v1_t& clock_details) {
  FX_LOGS(INFO) << "******************************************";
  FX_LOGS(INFO) << "Clock details -";
  FX_LOGS(INFO) << "  options:\t\t\t\t0x" << std::hex << clock_details.options;
  FX_LOGS(INFO) << "  backstop_time:\t\t\t" << clock_details.backstop_time;

  FX_LOGS(INFO) << "  query_ticks:\t\t\t" << clock_details.query_ticks;
  FX_LOGS(INFO) << "  last_value_update_ticks:\t\t" << clock_details.last_value_update_ticks;
  FX_LOGS(INFO) << "  last_rate_adjust_update_ticks:\t"
                << clock_details.last_rate_adjust_update_ticks;

  FX_LOGS(INFO) << "  generation_counter:\t\t" << clock_details.generation_counter;

  FX_LOGS(INFO) << "  mono_to_synthetic -";
  FX_LOGS(INFO) << "    reference_offset:\t\t" << clock_details.mono_to_synthetic.reference_offset;
  FX_LOGS(INFO) << "    synthetic_offset:\t\t" << clock_details.mono_to_synthetic.synthetic_offset;
  FX_LOGS(INFO) << "    rate -";
  FX_LOGS(INFO) << "      synthetic_ticks:\t\t"
                << clock_details.mono_to_synthetic.rate.synthetic_ticks;
  FX_LOGS(INFO) << "      reference_ticks:\t\t"
                << clock_details.mono_to_synthetic.rate.reference_ticks;
  FX_LOGS(INFO) << "******************************************";
}

zx_koid_t GetKoid(const zx::clock& clock) {
  zx_info_handle_basic_t basic_info;
  // size_t actual, avail;
  auto status =
      clock.get_info(ZX_INFO_HANDLE_BASIC, &basic_info, sizeof(basic_info), nullptr, nullptr);
  if (status != ZX_OK) {
    return ZX_HANDLE_INVALID;
  }

  return basic_info.koid;
}

zx::clock DuplicateClock(const zx::clock& clock, zx_rights_t rights) {
  zx::clock duplicate_clock;
  FX_CHECK(clock.duplicate(rights, &duplicate_clock) == ZX_OK);
  return duplicate_clock;
}

fpromise::result<ClockSnapshot, zx_status_t> SnapshotClock(const zx::clock& ref_clock) {
  ClockSnapshot snapshot;
  zx_clock_details_v1_t clock_details;
  zx_status_t status = ref_clock.get_details(&clock_details);

  if (status != ZX_OK) {
    return fpromise::error(status);
  }

  // The inverse of the clock_details.mono_to_synthetic affine transform.
  snapshot.reference_to_monotonic =
      TimelineFunction(clock_details.mono_to_synthetic.reference_offset,
                       clock_details.mono_to_synthetic.synthetic_offset,
                       clock_details.mono_to_synthetic.rate.reference_ticks,
                       clock_details.mono_to_synthetic.rate.synthetic_ticks);
  snapshot.generation = clock_details.generation_counter;

  return fpromise::ok(snapshot);
}

// Naming is confusing here. zx::clock transforms/structs call the underlying baseline clock (ticks
// or monotonic: we use monotonic) their "reference" clock. Unfortunately, in media terminology a
// "reference clock" could be any continuous monotonically increasing clock -- including not only
// the local system monotonic, but also custom clocks maintained outside the kernel (which zx::clock
// calls "synthetic" clocks).
//
// Thus in these util functions that convert between clocks, a conversion that we usually call "from
// monotonic to reference" is (in zx::clock terms) a conversion "from reference to synthetic", where
// the baseline reference here is the monotonic clock.
fpromise::result<zx::time, zx_status_t> ReferenceTimeFromMonotonicTime(const zx::clock& ref_clock,
                                                                       zx::time mono_time) {
  zx_clock_details_v1_t clock_details;
  zx_status_t status = ref_clock.get_details(&clock_details);
  if (status != ZX_OK) {
    return fpromise::error(status);
  }

  return fpromise::ok(zx::time(
      affine::Transform::Apply(clock_details.mono_to_synthetic.reference_offset,
                               clock_details.mono_to_synthetic.synthetic_offset,
                               affine::Ratio(clock_details.mono_to_synthetic.rate.synthetic_ticks,
                                             clock_details.mono_to_synthetic.rate.reference_ticks),
                               mono_time.get())));
}

fpromise::result<zx::time, zx_status_t> MonotonicTimeFromReferenceTime(const zx::clock& ref_clock,
                                                                       zx::time ref_time) {
  zx_clock_details_v1_t clock_details;
  zx_status_t status = ref_clock.get_details(&clock_details);
  if (status != ZX_OK) {
    return fpromise::error(status);
  }

  return fpromise::ok(zx::time(affine::Transform::ApplyInverse(
      clock_details.mono_to_synthetic.reference_offset,
      clock_details.mono_to_synthetic.synthetic_offset,
      affine::Ratio(clock_details.mono_to_synthetic.rate.synthetic_ticks,
                    clock_details.mono_to_synthetic.rate.reference_ticks),
      ref_time.get())));
}

fpromise::result<zx::time, zx_status_t> ReferenceTimeFromReferenceTime(
    const zx::clock& ref_clock_a, zx::time ref_time_a, const zx::clock& ref_clock_b) {
  zx_clock_details_v1_t clock_details_a, clock_details_b;

  auto status = ref_clock_a.get_details(&clock_details_a);
  if (status == ZX_OK) {
    status = ref_clock_b.get_details(&clock_details_b);
  }
  if (status != ZX_OK) {
    return fpromise::error(status);
  }

  auto mono_to_ref_a = clock_details_a.mono_to_synthetic;
  auto mono_to_ref_b = clock_details_b.mono_to_synthetic;

  auto mono_time = affine::Transform::ApplyInverse(
      mono_to_ref_a.reference_offset, mono_to_ref_a.synthetic_offset,
      affine::Ratio(mono_to_ref_a.rate.synthetic_ticks, mono_to_ref_a.rate.reference_ticks),
      ref_time_a.get());

  auto ref_time_b = affine::Transform::Apply(
      mono_to_ref_b.reference_offset, mono_to_ref_b.synthetic_offset,
      affine::Ratio(mono_to_ref_b.rate.synthetic_ticks, mono_to_ref_b.rate.reference_ticks),
      mono_time);

  return fpromise::ok(zx::time(ref_time_b));
}

affine::Transform ToAffineTransform(TimelineFunction& tl_function) {
  return affine::Transform(tl_function.reference_time(), tl_function.subject_time(),
                           affine::Ratio(static_cast<uint32_t>(tl_function.subject_delta()),
                                         static_cast<uint32_t>(tl_function.reference_delta())));
}

TimelineFunction ToTimelineFunction(affine::Transform affine_trans) {
  return TimelineFunction(affine_trans.b_offset(), affine_trans.a_offset(),
                          affine_trans.numerator(), affine_trans.denominator());
}

}  // namespace media::audio::clock
