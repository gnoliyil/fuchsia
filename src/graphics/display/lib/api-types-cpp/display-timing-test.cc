// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/lib/api-types-cpp/display-timing.h"

#include <fidl/fuchsia.hardware.display/cpp/wire.h>
#include <fuchsia/hardware/display/controller/c/banjo.h>

#include <cstdint>

#include <gtest/gtest.h>

namespace display {

namespace {

TEST(DisplayTiming, EqualityReflective) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  EXPECT_EQ(kParam, kParam);
}

TEST(DisplayTiming, EqualitySymmetric) {
  constexpr DisplayTiming kParam1 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  EXPECT_EQ(kParam1, kParam2);
  EXPECT_EQ(kParam2, kParam1);
}

TEST(DisplayTiming, EqualityTransitive) {
  constexpr DisplayTiming kParam1 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  constexpr DisplayTiming kParam3 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  EXPECT_EQ(kParam1, kParam2);
  EXPECT_EQ(kParam2, kParam3);
  EXPECT_EQ(kParam1, kParam3);
}

TEST(DisplayTiming, NonEquality) {
  constexpr DisplayTiming kParam1 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  {
    DisplayTiming kParam = kParam1;
    kParam.horizontal_active_px = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.horizontal_front_porch_px = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.horizontal_sync_width_px = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.horizontal_back_porch_px = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.vertical_active_lines = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.vertical_front_porch_lines = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.vertical_sync_width_lines = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.vertical_back_porch_lines = 0xff'ff;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.pixel_clock_frequency_khz = 0x7f'7f'7f'7f;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.fields_per_frame = FieldsPerFrame::kProgressive;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.hsync_polarity = SyncPolarity::kNegative;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.vsync_polarity = SyncPolarity::kNegative;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.vblank_alternates = false;
    EXPECT_NE(kParam, kParam1);
  }
  {
    DisplayTiming kParam = kParam1;
    kParam.pixel_repetition = 1;
    EXPECT_NE(kParam, kParam1);
  }
}

TEST(DisplayTiming, FromBanjo) {
  {
    constexpr DisplayTiming kExpected = {
        .horizontal_active_px = 0x0f'0f,
        .horizontal_front_porch_px = 0x0a'0a,
        .horizontal_sync_width_px = 0x01'01,
        .horizontal_back_porch_px = 0x02'02,
        .vertical_active_lines = 0x0b'0b,
        .vertical_front_porch_lines = 0x03'03,
        .vertical_sync_width_lines = 0x04'04,
        .vertical_back_porch_lines = 0x05'05,
        .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
        .fields_per_frame = FieldsPerFrame::kInterlaced,
        .hsync_polarity = SyncPolarity::kPositive,
        .vsync_polarity = SyncPolarity::kPositive,
        .vblank_alternates = true,
        .pixel_repetition = 0,
    };
    constexpr display_mode_t kBanjoDisplayMode = {
        .pixel_clock_khz = 0x1f'1f'1f'1f,
        .h_addressable = 0x0f'0f,
        .h_front_porch = 0x0a'0a,
        .h_sync_pulse = 0x01'01,
        .h_blanking = 0x0d'0d,
        .v_addressable = 0x0b'0b,
        .v_front_porch = 0x03'03,
        .v_sync_pulse = 0x04'04,
        .v_blanking = 0x0c'0c,
        .flags = MODE_FLAG_INTERLACED | MODE_FLAG_HSYNC_POSITIVE | MODE_FLAG_VSYNC_POSITIVE |
                 MODE_FLAG_ALTERNATING_VBLANK,
    };
    EXPECT_EQ(kExpected, ToDisplayTiming(kBanjoDisplayMode));
  }

  // Verify Hsync / Vsync polarities are correctly mapped.
  {
    constexpr DisplayTiming kExpected = {
        .horizontal_active_px = 0x0f'0f,
        .horizontal_front_porch_px = 0x0a'0a,
        .horizontal_sync_width_px = 0x01'01,
        .horizontal_back_porch_px = 0x02'02,
        .vertical_active_lines = 0x0b'0b,
        .vertical_front_porch_lines = 0x03'03,
        .vertical_sync_width_lines = 0x04'04,
        .vertical_back_porch_lines = 0x05'05,
        .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
        .fields_per_frame = FieldsPerFrame::kInterlaced,
        .hsync_polarity = SyncPolarity::kPositive,
        .vsync_polarity = SyncPolarity::kNegative,
        .vblank_alternates = true,
        .pixel_repetition = 0,
    };
    constexpr display_mode_t kBanjoDisplayMode = {
        .pixel_clock_khz = 0x1f'1f'1f'1f,
        .h_addressable = 0x0f'0f,
        .h_front_porch = 0x0a'0a,
        .h_sync_pulse = 0x01'01,
        .h_blanking = 0x0d'0d,
        .v_addressable = 0x0b'0b,
        .v_front_porch = 0x03'03,
        .v_sync_pulse = 0x04'04,
        .v_blanking = 0x0c'0c,
        .flags = MODE_FLAG_INTERLACED | MODE_FLAG_HSYNC_POSITIVE | MODE_FLAG_ALTERNATING_VBLANK,
    };
    EXPECT_EQ(kExpected, ToDisplayTiming(kBanjoDisplayMode));
  }
  {
    constexpr DisplayTiming kExpected = {
        .horizontal_active_px = 0x0f'0f,
        .horizontal_front_porch_px = 0x0a'0a,
        .horizontal_sync_width_px = 0x01'01,
        .horizontal_back_porch_px = 0x02'02,
        .vertical_active_lines = 0x0b'0b,
        .vertical_front_porch_lines = 0x03'03,
        .vertical_sync_width_lines = 0x04'04,
        .vertical_back_porch_lines = 0x05'05,
        .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
        .fields_per_frame = FieldsPerFrame::kInterlaced,
        .hsync_polarity = SyncPolarity::kNegative,
        .vsync_polarity = SyncPolarity::kPositive,
        .vblank_alternates = true,
        .pixel_repetition = 0,
    };
    constexpr display_mode_t kBanjoDisplayMode = {
        .pixel_clock_khz = 0x1f'1f'1f'1f,
        .h_addressable = 0x0f'0f,
        .h_front_porch = 0x0a'0a,
        .h_sync_pulse = 0x01'01,
        .h_blanking = 0x0d'0d,
        .v_addressable = 0x0b'0b,
        .v_front_porch = 0x03'03,
        .v_sync_pulse = 0x04'04,
        .v_blanking = 0x0c'0c,
        .flags = MODE_FLAG_INTERLACED | MODE_FLAG_VSYNC_POSITIVE | MODE_FLAG_ALTERNATING_VBLANK,
    };
    EXPECT_EQ(kExpected, ToDisplayTiming(kBanjoDisplayMode));
  }
}

TEST(DisplayTiming, ToBanjo) {
  {
    constexpr DisplayTiming kDisplayTiming = {
        .horizontal_active_px = 0x0f'0f,
        .horizontal_front_porch_px = 0x0a'0a,
        .horizontal_sync_width_px = 0x01'01,
        .horizontal_back_porch_px = 0x02'02,
        .vertical_active_lines = 0x0b'0b,
        .vertical_front_porch_lines = 0x03'03,
        .vertical_sync_width_lines = 0x04'04,
        .vertical_back_porch_lines = 0x05'05,
        .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
        .fields_per_frame = FieldsPerFrame::kInterlaced,
        .hsync_polarity = SyncPolarity::kPositive,
        .vsync_polarity = SyncPolarity::kPositive,
        .vblank_alternates = true,
        .pixel_repetition = 0,
    };
    const display_mode_t kBanjoDisplayMode = ToBanjoDisplayMode(kDisplayTiming);
    EXPECT_EQ(kBanjoDisplayMode.pixel_clock_khz, 0x1f'1f'1f'1fu);
    EXPECT_EQ(kBanjoDisplayMode.h_addressable, 0x0f'0fu);
    EXPECT_EQ(kBanjoDisplayMode.h_front_porch, 0x0a'0au);
    EXPECT_EQ(kBanjoDisplayMode.h_sync_pulse, 0x01'01u);
    EXPECT_EQ(kBanjoDisplayMode.h_blanking, 0x0d'0du);
    EXPECT_EQ(kBanjoDisplayMode.v_addressable, 0x0b'0bu);
    EXPECT_EQ(kBanjoDisplayMode.v_front_porch, 0x03'03u);
    EXPECT_EQ(kBanjoDisplayMode.v_sync_pulse, 0x04'04u);
    EXPECT_EQ(kBanjoDisplayMode.v_blanking, 0x0c'0cu);
    EXPECT_EQ(kBanjoDisplayMode.flags, MODE_FLAG_INTERLACED | MODE_FLAG_HSYNC_POSITIVE |
                                           MODE_FLAG_VSYNC_POSITIVE | MODE_FLAG_ALTERNATING_VBLANK);
  }

  // Verify Hsync / Vsync polarities are correctly mapped.
  {
    constexpr DisplayTiming kDisplayTiming = {
        .horizontal_active_px = 0x0f'0f,
        .horizontal_front_porch_px = 0x0a'0a,
        .horizontal_sync_width_px = 0x01'01,
        .horizontal_back_porch_px = 0x02'02,
        .vertical_active_lines = 0x0b'0b,
        .vertical_front_porch_lines = 0x03'03,
        .vertical_sync_width_lines = 0x04'04,
        .vertical_back_porch_lines = 0x05'05,
        .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
        .fields_per_frame = FieldsPerFrame::kInterlaced,
        .hsync_polarity = SyncPolarity::kPositive,
        .vsync_polarity = SyncPolarity::kNegative,
        .vblank_alternates = true,
        .pixel_repetition = 0,
    };
    const display_mode_t kBanjoDisplayMode = ToBanjoDisplayMode(kDisplayTiming);
    EXPECT_EQ(kBanjoDisplayMode.flags,
              MODE_FLAG_INTERLACED | MODE_FLAG_HSYNC_POSITIVE | MODE_FLAG_ALTERNATING_VBLANK);
  }
  {
    constexpr DisplayTiming kDisplayTiming = {
        .horizontal_active_px = 0x0f'0f,
        .horizontal_front_porch_px = 0x0a'0a,
        .horizontal_sync_width_px = 0x01'01,
        .horizontal_back_porch_px = 0x02'02,
        .vertical_active_lines = 0x0b'0b,
        .vertical_front_porch_lines = 0x03'03,
        .vertical_sync_width_lines = 0x04'04,
        .vertical_back_porch_lines = 0x05'05,
        .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
        .fields_per_frame = FieldsPerFrame::kInterlaced,
        .hsync_polarity = SyncPolarity::kNegative,
        .vsync_polarity = SyncPolarity::kPositive,
        .vblank_alternates = true,
        .pixel_repetition = 0,
    };
    const display_mode_t kBanjoDisplayMode = ToBanjoDisplayMode(kDisplayTiming);
    EXPECT_EQ(kBanjoDisplayMode.flags,
              MODE_FLAG_INTERLACED | MODE_FLAG_VSYNC_POSITIVE | MODE_FLAG_ALTERNATING_VBLANK);
  }
}

TEST(DisplayTiming, BanjoRoundTrip) {
  constexpr DisplayTiming kDisplayTiming = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  EXPECT_EQ(kDisplayTiming, ToDisplayTiming(ToBanjoDisplayMode(kDisplayTiming)));
}

TEST(DisplayTiming, AggregateHelpers) {
  constexpr DisplayTiming kDisplayTiming = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kNegative,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };

  // 0x0a'0a + 0x01'01 + 0x02'02 = 0x0d'0d
  EXPECT_EQ(kDisplayTiming.horizontal_blank_px(), 0x0d'0d);
  // 0x0f'0f + 0x0d'0d = 0x1c'1c
  EXPECT_EQ(kDisplayTiming.horizontal_total_px(), 0x1c'1c);
  // 0x03'03 + 0x04'04 + 0x05'05 = 0x0c'0c
  EXPECT_EQ(kDisplayTiming.vertical_blank_lines(), 0x0c'0c);
  // 0x0b'0b + 0x0c'0c = 0x17'17
  EXPECT_EQ(kDisplayTiming.vertical_total_lines(), 0x17'17);
}

TEST(DisplayTiming, AggregateHelpersInterlaced) {
  constexpr DisplayTiming kDisplayTimingWithoutAlternatingVblank = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kNegative,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };

  // 0x0a'0a + 0x01'01 + 0x02'02 = 0x0d'0d
  EXPECT_EQ(kDisplayTimingWithoutAlternatingVblank.horizontal_blank_px(), 0x0d'0d);
  // 0x0f'0f + 0x0d'0d = 0x1c'1c
  EXPECT_EQ(kDisplayTimingWithoutAlternatingVblank.horizontal_total_px(), 0x1c'1c);
  // 0x03'03 + 0x04'04 + 0x05'05 = 0x0c'0c
  EXPECT_EQ(kDisplayTimingWithoutAlternatingVblank.vertical_blank_lines(), 0x0c'0c);
  // 2 * 0x0c'0c + 0x0b'0b = 0x23'23
  EXPECT_EQ(kDisplayTimingWithoutAlternatingVblank.vertical_total_lines(), 0x23'23);

  constexpr DisplayTiming kDisplayTimingWithAlternatingVblank = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kInterlaced,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kNegative,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };

  // 0x0a'0a + 0x01'01 + 0x02'02 = 0x0d'0d
  EXPECT_EQ(kDisplayTimingWithAlternatingVblank.horizontal_blank_px(), 0x0d'0d);
  // 0x0f'0f + 0x0d'0d = 0x1c'1c
  EXPECT_EQ(kDisplayTimingWithAlternatingVblank.horizontal_total_px(), 0x1c'1c);
  // 0x03'03 + 0x04'04 + 0x05'05 = 0x0c'0c
  EXPECT_EQ(kDisplayTimingWithAlternatingVblank.vertical_blank_lines(), 0x0c'0c);
  // 0x0c'0c + (0x0c'0c + 1) + 0x0b'0b = 0x23'24
  EXPECT_EQ(kDisplayTimingWithAlternatingVblank.vertical_total_lines(), 0x23'24);
}

TEST(DisplayTimingIsValid, Valid) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_TRUE(kParam.IsValid());
}

TEST(DisplayTimingIsValid, InvalidHorizontalActive) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0xff'ff'ff,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = -1,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidHorizontalFrontPorch) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0xff'ff'ff,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = -1,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidHorizontalSyncWidth) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0xff'ff'ff,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = -1,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidHorizontalBackPorch) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0xff'ff'ff,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = -1,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidVerticalActive) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0xff'ff'ff,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = -1,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidVerticalFrontPorch) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0xff'ff'ff,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = -1,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidVerticalSyncWidth) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0xff'ff'ff,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = -1,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidVerticalBackPorch) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0xff'ff'ff,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = -1,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidPixelClockFrequencyKhz) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = -1,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());
}

TEST(DisplayTimingIsValid, InvalidPixelRepetition) {
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 10,
  };
  EXPECT_FALSE(kParam.IsValid());

  constexpr DisplayTiming kParam2 = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = -1,
  };
  EXPECT_FALSE(kParam2.IsValid());
}

TEST(DisplayTimingIsValid, InvalidHorizontalTotal) {
  // All the atomic fields (horizontal_active_px, horizontal_front_porch_px,
  // horizontal_sync_width_px and horizontal_back_porch_px) don't exceed their
  // maximum allowed limits, but the horizontal total pixels (0x3bbb8) exceeds
  // the maximum limit.
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0xee'ee,
      .horizontal_front_porch_px = 0xee'ee,
      .horizontal_sync_width_px = 0xee'ee,
      .horizontal_back_porch_px = 0xee'ee,
      .vertical_active_lines = 0x0b'0b,
      .vertical_front_porch_lines = 0x03'03,
      .vertical_sync_width_lines = 0x04'04,
      .vertical_back_porch_lines = 0x05'05,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = true,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());
}

TEST(DisplayTimingIsValid, InvalidVerticalTotal) {
  // All the atomic fields (vertical_active_lines, vertical_front_porch_lines,
  // vertical_sync_width_lines and vertical_back_porch_lines) don't exceed their
  // maximum allowed limits, but the vertical total lines (0x3bbb8) exceeds
  // the maximum limit.
  constexpr DisplayTiming kParam = {
      .horizontal_active_px = 0x0f'0f,
      .horizontal_front_porch_px = 0x0a'0a,
      .horizontal_sync_width_px = 0x01'01,
      .horizontal_back_porch_px = 0x02'02,
      .vertical_active_lines = 0xee'ee,
      .vertical_front_porch_lines = 0xee'ee,
      .vertical_sync_width_lines = 0xee'ee,
      .vertical_back_porch_lines = 0xee'ee,
      .pixel_clock_frequency_khz = 0x1f'1f'1f'1f,
      .fields_per_frame = FieldsPerFrame::kProgressive,
      .hsync_polarity = SyncPolarity::kPositive,
      .vsync_polarity = SyncPolarity::kPositive,
      .vblank_alternates = false,
      .pixel_repetition = 0,
  };
  EXPECT_FALSE(kParam.IsValid());
}

}  // namespace

}  // namespace display
