// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/display/display.h"

#include <fidl/fuchsia.images2/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <zircon/syscalls.h>

namespace scenic_impl {
namespace display {

Display::Display(fuchsia::hardware::display::DisplayId id, uint32_t width_in_px,
                 uint32_t height_in_px, uint32_t width_in_mm, uint32_t height_in_mm,
                 std::vector<fuchsia_images2::PixelFormat> pixel_formats)
    : vsync_timing_(std::make_shared<scheduling::VsyncTiming>()),
      display_id_(id),
      width_in_px_(width_in_px),
      height_in_px_(height_in_px),
      width_in_mm_(width_in_mm),
      height_in_mm_(height_in_mm),
      pixel_formats_(std::move(pixel_formats)) {
  zx::event::create(0, &ownership_event_);
  device_pixel_ratio_.store({1.f, 1.f});
}
Display::Display(fuchsia::hardware::display::DisplayId id, uint32_t width_in_px,
                 uint32_t height_in_px)
    : Display(id, width_in_px, height_in_px, 0, 0, {fuchsia_images2::PixelFormat::kBgra32}) {}

void Display::Claim() {
  FX_DCHECK(!claimed_);
  claimed_ = true;
}

void Display::Unclaim() {
  FX_DCHECK(claimed_);
  claimed_ = false;
}

void Display::OnVsync(zx::time timestamp,
                      fuchsia::hardware::display::ConfigStamp applied_config_stamp) {
  zx::duration time_since_last_vsync = timestamp - vsync_timing_->last_vsync_time();

  if (vsync_timing_->last_vsync_time() != zx::time(0)) {
    // Estimate current vsync interval. Need to include a maximum to mitigate any
    // potential issues during long breaks.
    if (time_since_last_vsync >= kMaximumVsyncInterval) {
      FX_LOGS(WARNING) << "More than " << kMaximumVsyncInterval.to_msecs()
                       << "ms observed between vsyncs.";
    }
    vsync_timing_->set_vsync_interval(time_since_last_vsync < kMaximumVsyncInterval
                                          ? time_since_last_vsync
                                          : vsync_timing_->vsync_interval());
  }

  vsync_timing_->set_last_vsync_time(timestamp);

  TRACE_INSTANT("gfx", "Display::OnVsync", TRACE_SCOPE_PROCESS, "Timestamp", timestamp.get(),
                "Vsync interval", vsync_timing_->vsync_interval().get());

  if (vsync_callback_) {
    vsync_callback_(timestamp, applied_config_stamp);
  }
}

}  // namespace display
}  // namespace scenic_impl
