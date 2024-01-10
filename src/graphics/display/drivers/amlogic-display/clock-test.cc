// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/amlogic-display/clock.h"

#include <lib/device-protocol/display-panel.h>

#include <gtest/gtest.h>

#include "src/graphics/display/drivers/amlogic-display/dsi.h"
#include "src/lib/testing/predicates/status.h"

namespace amlogic_display {

namespace {

const display_setting_t kDisplaySettingsWithoutHdmiPllClockRatio = {
    .lane_num = 4,
    .bit_rate_max = 400,
    .clock_factor = 0,  // auto
    .lcd_clock = 49434000,
    .h_active = 600,
    .v_active = 1024,
    .h_period = 770,
    .v_period = 1070,
    .hsync_width = 10,
    .hsync_bp = 80,
    .hsync_pol = 0,
    .vsync_width = 6,
    .vsync_bp = 20,
    .vsync_pol = 0,
};

static display_setting_t display_types[] = {
    kDisplaySettingTV070WSM_FT,
    kDisplaySettingP070ACB_FT,
    kDisplaySettingG101B158_FT,
    kDisplaySettingTV101WXM_FT,
    kDisplaySettingKD070D82_FT,
    kDisplaySettingTV070WSM_ST7703I,
    kDisplaySettingsWithoutHdmiPllClockRatio,
};

// For now, simply test that timing calculations don't segfault.
TEST(AmlogicDisplayClock, PanelTiming) {
  for (const auto t : display_types) {
    Clock::CalculateLcdTiming(t);
  }
}

TEST(AmlogicDisplayClock, PllTiming_ValidMode) {
  for (const auto t : display_types) {
    zx::result<PllConfig> pll_r = Clock::GenerateHPLL(t);
    EXPECT_OK(pll_r.status_value());
  }
}

TEST(AmlogicDisplayClock, PllTimingHdmiPllClockRatioCalculatedCorrectly) {
  // The LCD vendor-provided display settings hardcode the HDMI PLL / DSI
  // clock ratio while the settings below requires the clock ratios to be
  // calculated automatically.
  //
  // This test ensures that the calculated clock ratios match the hardcoded
  // values removed in Ie2c4721b14a92977ef31dd2951dc4cac207cb60e.
  zx::result<PllConfig> pll_tv070wsm_ft = Clock::GenerateHPLL(kDisplaySettingTV070WSM_FT);
  static constexpr int kExpectedHdmiPllClockRatioTv070wsmFt = 8;
  EXPECT_OK(pll_tv070wsm_ft.status_value());
  EXPECT_EQ(kExpectedHdmiPllClockRatioTv070wsmFt, static_cast<int>(pll_tv070wsm_ft->clock_factor));

  zx::result<PllConfig> pll_p070acb_ft = Clock::GenerateHPLL(kDisplaySettingP070ACB_FT);
  static constexpr int kExpectedHdmiPllClockRatioP070acbFt = 8;
  EXPECT_OK(pll_p070acb_ft.status_value());
  EXPECT_EQ(kExpectedHdmiPllClockRatioP070acbFt, static_cast<int>(pll_p070acb_ft->clock_factor));

  zx::result<PllConfig> pll_g101b158_ft = Clock::GenerateHPLL(kDisplaySettingG101B158_FT);
  static constexpr int kExpectedHdmiPllClockRatioG101b158 = 8;
  EXPECT_OK(pll_g101b158_ft.status_value());
  EXPECT_EQ(kExpectedHdmiPllClockRatioG101b158, static_cast<int>(pll_g101b158_ft->clock_factor));

  zx::result<PllConfig> pll_tv101wxm_ft = Clock::GenerateHPLL(kDisplaySettingTV101WXM_FT);
  static constexpr int kExpectedHdmiPllClockRatioTv101wxmFt = 8;
  EXPECT_OK(pll_tv101wxm_ft.status_value());
  EXPECT_EQ(kExpectedHdmiPllClockRatioTv101wxmFt, static_cast<int>(pll_tv101wxm_ft->clock_factor));
}

}  // namespace

}  // namespace amlogic_display
