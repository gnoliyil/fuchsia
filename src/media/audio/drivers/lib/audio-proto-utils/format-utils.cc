// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/stdcompat/bit.h>
#include <string.h>

#include <algorithm>
#include <iterator>
#include <span>

#include <audio-proto-utils/format-utils.h>

namespace {
namespace audio_fidl = fuchsia_hardware_audio;
}  // namespace
namespace audio {
namespace utils {

// Note: these sets must be kept in monotonically increasing order.
static const uint32_t RATES_48000_FAMILY[] = {8000,  16000,  32000,  48000,
                                              96000, 192000, 384000, 768000};
static const uint32_t RATES_44100_FAMILY[] = {11025, 22050, 44100, 88200, 176400};
static const uint32_t* RATES_48000_FAMILY_LAST = RATES_48000_FAMILY + std::size(RATES_48000_FAMILY);
static const uint32_t* RATES_44100_FAMILY_LAST = RATES_44100_FAMILY + std::size(RATES_44100_FAMILY);
static constexpr auto DISCRETE_FLAGS =
    ASF_RANGE_FLAG_FPS_48000_FAMILY | ASF_RANGE_FLAG_FPS_44100_FAMILY;

bool FrameRateIn48kFamily(uint32_t rate) {
  const uint32_t* found = std::lower_bound(RATES_48000_FAMILY, RATES_48000_FAMILY_LAST, rate);
  return ((found < RATES_48000_FAMILY_LAST) && (*found == rate));
}

bool FrameRateIn441kFamily(uint32_t rate) {
  const uint32_t* found = std::lower_bound(RATES_44100_FAMILY, RATES_44100_FAMILY_LAST, rate);
  return ((found < RATES_44100_FAMILY_LAST) && (*found == rate));
}

size_t MaxFrameRatesIn48kFamily() { return std::size(RATES_48000_FAMILY); }

size_t MaxFrameRatesIn441kFamily() { return std::size(RATES_44100_FAMILY); }

// Figure out the size of an audio frame based on the sample format.  Returns 0
// in the case of an error (bad channel count, bad sample format)
uint32_t ComputeFrameSize(uint16_t channels, audio_sample_format_t sample_format) {
  uint32_t fmt_noflags = sample_format & ~AUDIO_SAMPLE_FORMAT_FLAG_MASK;

  switch (fmt_noflags) {
    case AUDIO_SAMPLE_FORMAT_8BIT:
      return 1u * channels;
    case AUDIO_SAMPLE_FORMAT_16BIT:
      return 2u * channels;
    case AUDIO_SAMPLE_FORMAT_24BIT_PACKED:
      return 3u * channels;
    case AUDIO_SAMPLE_FORMAT_20BIT_IN32:
    case AUDIO_SAMPLE_FORMAT_24BIT_IN32:
    case AUDIO_SAMPLE_FORMAT_32BIT:
    case AUDIO_SAMPLE_FORMAT_32BIT_FLOAT:
      return 4u * channels;

    // See fxbug.dev/30949
    // We currently don't really know how 20 bit audio should be packed.  For
    // now, treat it as an error.
    case AUDIO_SAMPLE_FORMAT_20BIT_PACKED:
    default:
      return 0;
  }
}

std::vector<Format> GetAllFormats(audio_sample_format_t sample_format) {
  ZX_DEBUG_ASSERT(!(sample_format & AUDIO_SAMPLE_FORMAT_BITSTREAM));
  ZX_DEBUG_ASSERT(!(sample_format & AUDIO_SAMPLE_FORMAT_FLAG_INVERT_ENDIAN));
  std::vector<Format> all;
  uint32_t fmt_noflags = sample_format & ~AUDIO_SAMPLE_FORMAT_FLAG_MASK;
  while (fmt_noflags) {
    audio_fidl::wire::SampleFormat out_format = audio_fidl::wire::SampleFormat::kPcmSigned;
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_FLAG_UNSIGNED) {
      out_format = audio_fidl::wire::SampleFormat::kPcmUnsigned;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_8BIT) {
      all.push_back({out_format, 8, 1});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_8BIT;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_16BIT) {
      all.push_back({out_format, 16, 2});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_16BIT;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_24BIT_PACKED) {
      all.push_back({out_format, 24, 3});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_24BIT_PACKED;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_20BIT_IN32) {
      all.push_back({out_format, 20, 4});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_20BIT_IN32;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_24BIT_IN32) {
      all.push_back({out_format, 24, 4});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_24BIT_IN32;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_32BIT) {
      all.push_back({out_format, 32, 4});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_32BIT;
    }
    if (fmt_noflags & AUDIO_SAMPLE_FORMAT_32BIT_FLOAT) {
      all.push_back({audio_fidl::wire::SampleFormat::kPcmFloat, 32, 4});
      fmt_noflags &= ~AUDIO_SAMPLE_FORMAT_32BIT_FLOAT;
    }
    // We expect all bits to have been processed.
    if (fmt_noflags != 0) {
      ZX_PANIC("Invalid audio_out_format_t: %X", sample_format);
    }
  }
  return all;
}

audio_sample_format_t GetSampleFormat(uint8_t bits_per_sample, uint8_t bits_per_channel) {
  audio_sample_format_t sample_format = 0;
  if (bits_per_sample == bits_per_channel) {
    switch (bits_per_sample) {
        // clang-format off
      case 8:  sample_format = AUDIO_SAMPLE_FORMAT_8BIT;         break;
      case 16: sample_format = AUDIO_SAMPLE_FORMAT_16BIT;        break;
      case 24: sample_format = AUDIO_SAMPLE_FORMAT_24BIT_PACKED; break;
      case 32: sample_format = AUDIO_SAMPLE_FORMAT_32BIT;        break;
      // clang-format on
      default:
        return 0;
    }
  }
  if (bits_per_sample == 20 && bits_per_channel == 32) {
    sample_format = AUDIO_SAMPLE_FORMAT_20BIT_IN32;
  } else if (bits_per_sample == 24 && bits_per_channel == 32) {
    sample_format = AUDIO_SAMPLE_FORMAT_24BIT_IN32;
  }
  return sample_format;
}

bool FormatIsCompatible(uint32_t frame_rate, uint16_t channels, audio_sample_format_t sample_format,
                        const audio_stream_format_range_t& format_range) {
  // Are the requested number of channels in range?
  if ((channels < format_range.min_channels) || (channels > format_range.max_channels))
    return false;

  // Is the requested sample format compatible with the range's supported
  // formats?  If so...
  //
  // 1) The flags for each (requested and supported) must match exactly.
  // 2) The requested format must be unique, and a PCM format (we don't know
  //    how to test compatibility for compressed bitstream formats right now)
  // 3) The requested format must intersect the set of supported formats.
  //
  // Start by testing requirement #1.
  uint32_t requested_flags = sample_format & AUDIO_SAMPLE_FORMAT_FLAG_MASK;
  uint32_t supported_flags = format_range.sample_formats & AUDIO_SAMPLE_FORMAT_FLAG_MASK;
  if (requested_flags != supported_flags)
    return false;

  // Requirement #2.  If this format is unique and PCM, then there is exactly
  // 1 bit set in it and that bit is not AUDIO_SAMPLE_FORMAT_BITSTREAM.  We
  // can use cpp20::has_single_bit to check if there is exactly 1 bit set.  (note,
  // cpp20::has_single_bit does not consider 0 to be a power of 2, so it's perfect for
  // this)
  uint32_t requested_noflags = sample_format & ~AUDIO_SAMPLE_FORMAT_FLAG_MASK;
  if ((requested_noflags == AUDIO_SAMPLE_FORMAT_BITSTREAM) ||
      (!cpp20::has_single_bit(requested_noflags)))
    return false;

  // Requirement #3.  Testing intersection is easy, just and the two.  No need
  // to strip the flags from the supported format bitmask, we have already
  // stripped them from the request when checking requirement #2.
  if (!(format_range.sample_formats & requested_noflags))
    return false;

  // Check the requested frame rate.  If it is not in the range expressed by
  // the format_range, then we know this is not a match.
  if ((frame_rate < format_range.min_frames_per_second) ||
      (frame_rate > format_range.max_frames_per_second))
    return false;

  // The frame rate is in range, if this format_range supports continuous
  // frame rates, then this is a match.
  if (format_range.flags & ASF_RANGE_FLAG_FPS_CONTINUOUS)
    return true;

  // Check the 48k family.
  if ((format_range.flags & ASF_RANGE_FLAG_FPS_48000_FAMILY) && FrameRateIn48kFamily(frame_rate))
    return true;

  // Check the 44.1k family.
  if ((format_range.flags & ASF_RANGE_FLAG_FPS_44100_FAMILY) && FrameRateIn441kFamily(frame_rate))
    return true;

  // No supported frame rates found.  Declare no-match.
  return false;
}

void FrameRateEnumerator::InsertRates(const audio_stream_format_range_t& range,
                                      cpp20::span<const uint32_t> rates) {
  for (uint32_t rate : rates) {
    // If the rate in the table is less than the minimum
    // frames_per_second, keep advancing the index.
    if (rate < range.min_frames_per_second)
      continue;

    // If the rate in the table is greater than the maximum
    // frames_per_second, then we are done with this table.  There are
    // no more matches to be found in it.
    if (rate > range.max_frames_per_second)
      break;

    // The rate in this table is between the min and the max rates
    // supported by this range, so we record it.
    rates_.insert(rate);
  }
}

FrameRateEnumerator::FrameRateEnumerator(const audio_stream_format_range_t& range) {
  // If the range is just one frames per second value, use it and return.
  if (range.min_frames_per_second == range.max_frames_per_second) {
    rates_.insert(range.min_frames_per_second);
    return;
  }

  // Sanity check our range first.  If it is continuous, or invalid in any
  // way, then we are not going to enumerate any valid frame rates, just return.
  if ((range.flags & ASF_RANGE_FLAG_FPS_CONTINUOUS) || !(range.flags & DISCRETE_FLAGS) ||
      (range.min_frames_per_second > range.max_frames_per_second)) {
    return;
  }

  if (range.flags & ASF_RANGE_FLAG_FPS_48000_FAMILY) {
    InsertRates(range, {RATES_48000_FAMILY, std::size(RATES_48000_FAMILY)});
  }
  if (range.flags & ASF_RANGE_FLAG_FPS_44100_FAMILY) {
    InsertRates(range, {RATES_44100_FAMILY, std::size(RATES_44100_FAMILY)});
  }
}

}  // namespace utils
}  // namespace audio
