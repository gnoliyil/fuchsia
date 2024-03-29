// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/coordinator/display-info.h"

#include <fuchsia/hardware/display/controller/c/banjo.h>
#include <fuchsia/hardware/i2cimpl/cpp/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/result.h>
#include <zircon/assert.h>
#include <zircon/device/audio.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/time.h>

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>

#include <audio-proto-utils/format-utils.h>
#include <fbl/alloc_checker.h>
#include <fbl/ref_ptr.h>
#include <fbl/string_printf.h>
#include <pretty/hexdump.h>

#include "src/devices/lib/audio/audio.h"
#include "src/graphics/display/drivers/coordinator/migration-util.h"
#include "src/graphics/display/lib/api-types-cpp/display-id.h"
#include "src/graphics/display/lib/api-types-cpp/display-timing.h"
#include "src/graphics/display/lib/edid/edid.h"

namespace display {

namespace {

edid::ddc_i2c_transact ddc_tx = [](void* ctx, edid::ddc_i2c_msg_t* msgs, uint32_t count) -> bool {
  auto i2c = static_cast<ddk::I2cImplProtocolClient*>(ctx);
  i2c_impl_op_t ops[count];
  for (unsigned i = 0; i < count; i++) {
    ops[i].address = msgs[i].addr;
    ops[i].data_buffer = msgs[i].buf;
    ops[i].data_size = msgs[i].length;
    ops[i].is_read = msgs[i].is_read;
    ops[i].stop = i == (count - 1);
  }
  return i2c->Transact(ops, count) == ZX_OK;
};

}  // namespace

void DisplayInfo::InitializeInspect(inspect::Node* parent_node) {
  ZX_DEBUG_ASSERT(init_done);
  node = parent_node->CreateChild(fbl::StringPrintf("display-%" PRIu64, id.value()).c_str());

  if (mode.has_value()) {
    node.CreateUint("width", mode->h_addressable, &properties);
    node.CreateUint("height", mode->v_addressable, &properties);
    return;
  }

  ZX_DEBUG_ASSERT(edid.has_value());
  size_t i = 0;
  for (const display::DisplayTiming& t : edid->timings) {
    auto child = node.CreateChild(fbl::StringPrintf("timing-parameters-%lu", ++i).c_str());
    child.CreateDouble("vsync-hz",
                       static_cast<double>(t.vertical_field_refresh_rate_millihertz()) / 1000.0,
                       &properties);
    child.CreateInt("pixel-clock-khz", t.pixel_clock_frequency_khz, &properties);
    child.CreateInt("horizontal-pixels", t.horizontal_active_px, &properties);
    child.CreateInt("horizontal-blanking", t.horizontal_blank_px(), &properties);
    child.CreateInt("horizontal-sync-offset", t.horizontal_front_porch_px, &properties);
    child.CreateInt("horizontal-sync-pulse", t.horizontal_sync_width_px, &properties);
    child.CreateInt("vertical-pixels", t.vertical_active_lines, &properties);
    child.CreateInt("vertical-blanking", t.vertical_blank_lines(), &properties);
    child.CreateInt("vertical-sync-offset", t.vertical_front_porch_lines, &properties);
    child.CreateInt("vertical-sync-pulse", t.vertical_sync_width_lines, &properties);
    properties.emplace(std::move(child));
  }
}

// static
zx::result<fbl::RefPtr<DisplayInfo>> DisplayInfo::Create(const added_display_args_t& info) {
  fbl::AllocChecker ac;
  fbl::RefPtr<DisplayInfo> out = fbl::AdoptRef(new (&ac) DisplayInfo);
  if (!ac.check()) {
    return zx::error(ZX_ERR_NO_MEMORY);
  }

  out->pending_layer_change = false;
  out->layer_count = 0;
  out->id = ToDisplayId(info.display_id);

  zx::result get_display_info_pixel_formats_result =
      CoordinatorPixelFormat::CreateFblVectorFromBanjoVector(
          cpp20::span(info.pixel_format_list, info.pixel_format_count));
  if (get_display_info_pixel_formats_result.is_error()) {
    zxlogf(ERROR, "Cannot convert pixel formats to FIDL pixel format value: %s",
           get_display_info_pixel_formats_result.status_string());
    return get_display_info_pixel_formats_result.take_error();
  }
  out->pixel_formats = std::move(get_display_info_pixel_formats_result.value());

  zx::result get_display_info_cursor_infos_result =
      CoordinatorCursorInfo::CreateFblVectorFromBanjoVector(
          cpp20::span(info.cursor_info_list, info.cursor_info_count));
  if (get_display_info_cursor_infos_result.is_error()) {
    zxlogf(ERROR, "Cannot convert cursor info to FIDL cursor info value: %s",
           get_display_info_cursor_infos_result.status_string());
    return get_display_info_cursor_infos_result.take_error();
  }
  out->cursor_infos = std::move(get_display_info_cursor_infos_result.value());

  if (info.panel_capabilities_source == PANEL_CAPABILITIES_SOURCE_DISPLAY_MODE) {
    out->mode = info.panel.mode;
    return zx::ok(std::move(out));
  }

  ZX_DEBUG_ASSERT(info.panel_capabilities_source == PANEL_CAPABILITIES_SOURCE_EDID);
  out->edid = DisplayInfo::Edid{};
  ddk::I2cImplProtocolClient i2c(&info.panel.i2c);
  if (!i2c.is_valid()) {
    zxlogf(ERROR, "Presented edid display with no i2c bus");
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  bool success = false;
  const char* edid_err = "unknown error";

  uint32_t edid_attempt = 0;
  static constexpr uint32_t kEdidRetries = 3;
  do {
    if (edid_attempt != 0) {
      zxlogf(DEBUG, "Error %d/%d initializing edid: \"%s\"", edid_attempt, kEdidRetries, edid_err);
      zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));
    }
    edid_attempt++;

    success = out->edid->base.Init(&i2c, ddc_tx, &edid_err);
  } while (!success && edid_attempt < kEdidRetries);

  if (!success) {
    zxlogf(INFO, "Failed to parse edid (%d bytes) \"%s\"", out->edid->base.edid_length(), edid_err);
    if (zxlog_level_enabled(INFO) && out->edid->base.edid_bytes()) {
      hexdump(out->edid->base.edid_bytes(), out->edid->base.edid_length());
    }
    return zx::error(ZX_ERR_INTERNAL);
  }

  out->PopulateDisplayAudio();

  if (zxlog_level_enabled(DEBUG) && out->edid->audio.size()) {
    zxlogf(DEBUG, "Supported audio formats:");
    for (auto range : out->edid->audio) {
      audio_stream_format_range temp_range;
      audio::audio_stream_format_fidl_from_banjo(range, &temp_range);
      for (auto rate : audio::utils::FrameRateEnumerator(temp_range)) {
        zxlogf(DEBUG, "  rate=%d, channels=[%d, %d], sample=%x", rate, range.min_channels,
               range.max_channels, range.sample_formats);
      }
    }
  }

  if (zxlog_level_enabled(DEBUG)) {
    const auto& edid = out->edid->base;
    const char* manufacturer =
        strlen(edid.manufacturer_name()) ? edid.manufacturer_name() : edid.manufacturer_id();
    zxlogf(DEBUG, "Manufacturer \"%s\", product %d, name \"%s\", serial \"%s\"", manufacturer,
           edid.product_code(), edid.monitor_name(), edid.monitor_serial());
    edid.Print([](const char* str) { zxlogf(DEBUG, "%s", str); });
  }
  return zx::ok(std::move(out));
}

void DisplayInfo::PopulateDisplayAudio() {
  fbl::AllocChecker ac;

  // Displays which support any audio are required to support basic
  // audio, so just bail if that bit isn't set.
  if (!edid->base.supports_basic_audio()) {
    return;
  }

  // TODO(https://fxbug.dev/42107544): Revisit dedupe/merge logic once the audio API takes a stance.
  // First, this code always adds the basic audio formats before processing the SADs, which is
  // likely redundant on some hardware (the spec isn't clear about whether or not the basic audio
  // formats should also be included in the SADs). Second, this code assumes that the SADs are
  // compact and not redundant, which is not guaranteed.

  // Add the range for basic audio support.
  audio_types_audio_stream_format_range_t range;
  range.min_channels = 2;
  range.max_channels = 2;
  range.sample_formats = AUDIO_SAMPLE_FORMAT_16BIT;
  range.min_frames_per_second = 32000;
  range.max_frames_per_second = 48000;
  range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY | ASF_RANGE_FLAG_FPS_44100_FAMILY;

  edid->audio.push_back(range, &ac);
  if (!ac.check()) {
    zxlogf(ERROR, "Out of memory attempting to construct supported format list.");
    return;
  }

  for (auto it = edid::audio_data_block_iterator(&edid->base); it.is_valid(); ++it) {
    if (it->format() != edid::ShortAudioDescriptor::kLPcm) {
      // TODO(stevensd): Add compressed formats when audio format supports it
      continue;
    }
    audio_types_audio_stream_format_range_t range;

    constexpr audio_sample_format_t zero_format = static_cast<audio_sample_format_t>(0);
    range.sample_formats = static_cast<audio_sample_format_t>(
        (it->lpcm_24() ? AUDIO_SAMPLE_FORMAT_24BIT_PACKED | AUDIO_SAMPLE_FORMAT_24BIT_IN32
                       : zero_format) |
        (it->lpcm_20() ? AUDIO_SAMPLE_FORMAT_20BIT_PACKED | AUDIO_SAMPLE_FORMAT_20BIT_IN32
                       : zero_format) |
        (it->lpcm_16() ? AUDIO_SAMPLE_FORMAT_16BIT : zero_format));

    range.min_channels = 1;
    range.max_channels = static_cast<uint8_t>(it->num_channels_minus_1() + 1);

    // Now build continuous ranges of sample rates in the each family
    static constexpr struct {
      const uint32_t flag, val;
    } kRateLut[7] = {
        {edid::ShortAudioDescriptor::kHz32, 32000},   {edid::ShortAudioDescriptor::kHz44, 44100},
        {edid::ShortAudioDescriptor::kHz48, 48000},   {edid::ShortAudioDescriptor::kHz88, 88200},
        {edid::ShortAudioDescriptor::kHz96, 96000},   {edid::ShortAudioDescriptor::kHz176, 176400},
        {edid::ShortAudioDescriptor::kHz192, 192000},
    };

    for (uint32_t i = 0; i < std::size(kRateLut); ++i) {
      if (!(it->sampling_frequencies & kRateLut[i].flag)) {
        continue;
      }
      range.min_frames_per_second = kRateLut[i].val;

      if (audio::utils::FrameRateIn48kFamily(kRateLut[i].val)) {
        range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY;
      } else {
        range.flags = ASF_RANGE_FLAG_FPS_44100_FAMILY;
      }

      // We found the start of a range.  At this point, we are guaranteed
      // to add at least one new entry into the set of format ranges.
      // Find the end of this range.
      uint32_t j;
      for (j = i + 1; j < std::size(kRateLut); ++j) {
        if (!(it->bitrate & kRateLut[j].flag)) {
          break;
        }

        if (audio::utils::FrameRateIn48kFamily(kRateLut[j].val)) {
          range.flags |= ASF_RANGE_FLAG_FPS_48000_FAMILY;
        } else {
          range.flags |= ASF_RANGE_FLAG_FPS_44100_FAMILY;
        }
      }

      i = j - 1;
      range.max_frames_per_second = kRateLut[i].val;

      edid->audio.push_back(range, &ac);
      if (!ac.check()) {
        zxlogf(ERROR, "Out of memory attempting to construct supported format list.");
        return;
      }
    }
  }
}

}  // namespace display
