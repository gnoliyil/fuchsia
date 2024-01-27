// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This manual test is a basic integration test of the codec_factory +
// amlogic_video_decoder driver.
//
// If this test breaks and it's not immediately obvoius why, please feel free to
// involve dustingreen@ (me) in figuring it out.

#include "use_video_decoder_test.h"

#include <inttypes.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fit/defer.h>
#include <lib/media/codec_impl/fourcc.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <map>
#include <set>

#include <fbl/string_printf.h>

#include "../in_stream_buffer.h"
#include "../in_stream_file.h"
#include "../in_stream_peeker.h"
#include "../input_copier.h"
#include "../use_video_decoder.h"
#include "../util.h"
#include "openssl/base.h"

namespace {

// 8MiB max peek is essentially for h264 streams.  VP9 streams don't need to
// scan for start codes so won't peek anywhere near this much.
constexpr uint32_t kMaxPeekBytes = 8 * 1024 * 1024;
constexpr uint64_t kMaxBufferBytes = 8 * 1024 * 1024;

std::mutex tags_lock;

std::string GetSha256SoFar(const SHA256_CTX* sha256_ctx) {
  uint8_t md[SHA256_DIGEST_LENGTH] = {};
  // struct copy so caller can keep hashing more data into sha256_ctx
  SHA256_CTX sha256_ctx_copy = *sha256_ctx;
  ZX_ASSERT(SHA256_Final(md, &sha256_ctx_copy));
  char actual_sha256[SHA256_DIGEST_LENGTH * 2 + 1];
  char* actual_sha256_ptr = actual_sha256;
  for (uint8_t byte : md) {
    // Writes the terminating 0 each time, returns 2 each time.
    actual_sha256_ptr += snprintf(actual_sha256_ptr, 3, "%02x", byte);
  }
  FX_CHECK(actual_sha256_ptr == actual_sha256 + SHA256_DIGEST_LENGTH * 2);
  return std::string(actual_sha256, SHA256_DIGEST_LENGTH * 2);
}

}  // namespace

int use_video_decoder_test(std::string input_file_path, int expected_frame_count,
                           UseVideoDecoderFunction use_video_decoder, bool is_secure_output,
                           bool is_secure_input, uint32_t min_output_buffer_count,
                           const UseVideoDecoderTestParams* test_params) {
  const UseVideoDecoderTestParams default_test_params;
  if (!test_params) {
    test_params = &default_test_params;
  }
  test_params->Validate();

  {
    std::lock_guard<std::mutex> lock(tags_lock);
    fuchsia_logging::SetTags({"use_video_decoder_test"});
  }

  async::Loop fidl_loop(&kAsyncLoopConfigAttachToCurrentThread);
  thrd_t fidl_thread;
  ZX_ASSERT(ZX_OK == fidl_loop.StartThread("FIDL_thread", &fidl_thread));
  std::unique_ptr<sys::ComponentContext> component_context =
      sys::ComponentContext::CreateAndServeOutgoingDirectory();

  printf("Decoding test file %s\n", input_file_path.c_str());

  std::unique_ptr<InStream> in_stream_so_far = std::make_unique<InStreamFile>(
      &fidl_loop, fidl_thread, component_context.get(), input_file_path);
  // default 1
  const int64_t loop_stream_count = test_params->loop_stream_count;
  if (loop_stream_count >= 2) {
    std::unique_ptr<InStream> next_in_stream =
        std::make_unique<InStreamBuffer>(&fidl_loop, fidl_thread, component_context.get(),
                                         std::move(in_stream_so_far), kMaxBufferBytes);
    in_stream_so_far = std::move(next_in_stream);
  }
  auto in_stream_peeker = std::make_unique<InStreamPeeker>(
      &fidl_loop, fidl_thread, component_context.get(), std::move(in_stream_so_far), kMaxPeekBytes);

  std::vector<std::pair<bool, uint64_t>> timestamps;
  SHA256_CTX sha256_ctx;
  SHA256_Init(&sha256_ctx);

  uint32_t frame_index = 0;
  bool got_output_data = false;
  bool emit_frame_failure_seen = false;
  uint32_t seen_stream_lifetime_ordinal = 0;
  uint32_t hash_success_count = 0;
  uint32_t hash_failure_count = 0;
  // default 2
  const uint64_t keep_stream_modulo = test_params->keep_stream_modulo;
  auto on_reset_hash = [test_params, &sha256_ctx, &hash_failure_count, &hash_success_count,
                        &emit_frame_failure_seen, &frame_index] {
    std::string actual_sha256 = GetSha256SoFar(&sha256_ctx);
    printf("Previous iteration's computed sha256 is: %s\n", actual_sha256.c_str());
    if (test_params->golden_sha256 != UseVideoDecoderTestParams::kDefaultGoldenSha256) {
      if (strcmp(actual_sha256.c_str(), test_params->golden_sha256)) {
        printf("The sha256 doesn't match this iteration - expected: %s actual: %s\n",
               test_params->golden_sha256, actual_sha256.c_str());
        ++hash_failure_count;
      } else {
        printf("The computed sha256 matches golden sha256 for previous iteration.\n");
        ++hash_success_count;
      }
    }
    printf("hash_success_count: %u hash_failure_count: %u\n", hash_success_count,
           hash_failure_count);
    SHA256_Init(&sha256_ctx);
    emit_frame_failure_seen = false;
    frame_index = 0;
  };
  EmitFrame emit_frame = [&sha256_ctx, &timestamps, &frame_index, &got_output_data,
                          keep_stream_modulo, test_params, input_file_path, expected_frame_count,
                          use_video_decoder, is_secure_output, is_secure_input,
                          min_output_buffer_count, &emit_frame_failure_seen,
                          &seen_stream_lifetime_ordinal,
                          &on_reset_hash](uint64_t stream_lifetime_ordinal, uint8_t* i420_data,
                                          uint32_t width, uint32_t height, uint32_t stride,
                                          bool has_timestamp_ish, uint64_t timestamp_ish) {
    VLOGF("emit_frame stream_lifetime_ordinal: %" PRIu64
          " frame_index: %u has_timestamp_ish: %d timestamp_ish: %" PRId64,
          stream_lifetime_ordinal, frame_index, has_timestamp_ish, timestamp_ish);
    // For debugging a flake:
    if (test_params->loop_stream_count > 1) {
      LOGF("emit_frame stream_lifetime_ordinal: %" PRIu64
           " frame_index: %u has_timestamp_ish: %d timestamp_ish: %" PRId64,
           stream_lifetime_ordinal, frame_index, has_timestamp_ish, timestamp_ish);
    }
    if (stream_lifetime_ordinal > seen_stream_lifetime_ordinal) {
      seen_stream_lifetime_ordinal = stream_lifetime_ordinal;
      if (test_params->reset_hash_each_iteration) {
        if (stream_lifetime_ordinal != 1) {
          on_reset_hash();
        }
      }
    }
    ZX_DEBUG_ASSERT(stream_lifetime_ordinal % 2 == 1);
    ZX_ASSERT_MSG(width % 2 == 0, "odd width not yet handled");
    ZX_ASSERT_MSG(width == stride, "stride != width not yet handled");
    auto increment_frame_index = fit::defer([&frame_index] { frame_index++; });
    // For streams where this isn't true, we don't flush the input EOS, so there's no guarantee
    // how many output frames we'll get.
    if (stream_lifetime_ordinal % keep_stream_modulo != 1) {
      // ~increment_frame_index
      return;
    }
    if (emit_frame_failure_seen) {
      // We only want to see LOGF()s about the first frame mismatch problem; subsequent frames
      // will be computing a completely different cumulative hash.
      return;
    }
    timestamps.push_back({has_timestamp_ish, timestamp_ish});
    if (i420_data) {
      got_output_data = true;
      if (test_params->golden_sha256 || test_params->per_frame_golden_sha256) {
        SHA256_Update(&sha256_ctx, i420_data, width * height * 3 / 2);
        std::string sha256_so_far = GetSha256SoFar(&sha256_ctx);
        LOGF("%s frame_index: %u SHA256 so far: %s",
             test_params->mime_type ? test_params->mime_type->c_str() : "<no mime type>",
             frame_index, sha256_so_far.c_str());
        if (test_params->per_frame_golden_sha256) {
          ZX_ASSERT(test_params->per_frame_golden_sha256[frame_index]);
          std::string expected_sha256(test_params->per_frame_golden_sha256[frame_index]);
          if (sha256_so_far != expected_sha256) {
            LOGF("does not match per_frame_golden_sha256 - actual: %s expected %s",
                 sha256_so_far.c_str(), expected_sha256.c_str());
            if (test_params->compare_to_sw_decode && !test_params->require_sw) {
              // Grade sherlock video decoder's paper.

              UseVideoDecoderTestParams sw_decode_test_params = test_params->Clone();

              sw_decode_test_params.compare_to_sw_decode = false;
              sw_decode_test_params.require_sw = true;
              sw_decode_test_params.loop_stream_count =
                  UseVideoDecoderTestParams::kDefaultLoopStreamCount;

              FrameToCompare frame_to_compare{
                  .data = i420_data,
                  .ordinal = frame_index,
                  .width = width,
                  .height = height,
              };
              sw_decode_test_params.frame_to_compare = &frame_to_compare;

              LOGF(
                  "\n\n\n######## RECURSIVELY CALLING use_video_decoder_test TO GET EXPECTED "
                  "FRAME; "
                  "TEST WILL FAIL AFTER COMPARING FRAMES AND FINISHING UP ########\n\n\n");
              if (0 != use_video_decoder_test(input_file_path, expected_frame_count,
                                              use_video_decoder, is_secure_output, is_secure_input,
                                              min_output_buffer_count, &sw_decode_test_params)) {
                emit_frame_failure_seen = true;
              }
            }
            emit_frame_failure_seen = true;
            LOGF("finished handling sha256 mis-match (%s)",
                 test_params->frame_to_compare ? "inner" : "outer");
          }
        }
      } else {
        LOGF("frame_index: %u", frame_index);
      }
      if (test_params->frame_to_compare &&
          (frame_index == test_params->frame_to_compare->ordinal)) {
        auto& frame_to_compare = *test_params->frame_to_compare;
        ZX_ASSERT(frame_index == frame_to_compare.ordinal);
        ZX_ASSERT(width = frame_to_compare.width);
        ZX_ASSERT(height = frame_to_compare.height);
        // If memcmp() returned 0, it'd mean the per-frame hash would have matched, which it
        // didn't.
        ZX_ASSERT(0 != memcmp(i420_data, frame_to_compare.data, width * height * 3 / 2));
        int32_t luma_min_diff_x = static_cast<int32_t>(width);
        int32_t luma_min_diff_y = static_cast<int32_t>(height);
        int32_t luma_max_diff_x = -1;
        int32_t luma_max_diff_y = -1;
        int32_t u_min_diff_x = static_cast<int32_t>(width);
        int32_t u_min_diff_y = static_cast<int32_t>(height);
        int32_t u_max_diff_x = -1;
        int32_t u_max_diff_y = -1;
        int32_t v_min_diff_x = static_cast<int32_t>(width);
        int32_t v_min_diff_y = static_cast<int32_t>(height);
        int32_t v_max_diff_x = -1;
        int32_t v_max_diff_y = -1;
        for (int32_t y = 0; y < static_cast<int32_t>(height); ++y) {
          for (int32_t x = 0; x < static_cast<int32_t>(width); ++x) {
            uint8_t luma_sample_expected = i420_data[y * width + x];
            uint8_t luma_sample_actual = frame_to_compare.data[y * width + x];
            if (luma_sample_actual != luma_sample_expected) {
              luma_min_diff_x = std::min(luma_min_diff_x, x);
              luma_min_diff_y = std::min(luma_min_diff_y, y);
              luma_max_diff_x = std::max(luma_max_diff_x, x);
              luma_max_diff_y = std::max(luma_max_diff_y, y);
            }
            if ((y % 2 == 0) && (x % 2 == 0)) {
              uint8_t u_sample_expected =
                  i420_data[width * height + (y / 2) * (width / 2) + (x / 2)];
              uint8_t u_sample_actual =
                  frame_to_compare.data[width * height + (y / 2) * (width / 2) + (x / 2)];
              if (u_sample_actual != u_sample_expected) {
                u_min_diff_x = std::min(u_min_diff_x, x);
                u_min_diff_y = std::min(u_min_diff_y, y);
                u_max_diff_x = std::max(u_max_diff_x, x);
                u_max_diff_y = std::max(u_max_diff_y, y);
              }
              uint8_t v_sample_expected = i420_data[width * height + (width / 2) * (height / 2) +
                                                    (y / 2) * (width / 2) + (x / 2)];
              uint8_t v_sample_actual =
                  frame_to_compare.data[width * height + (width / 2) * (height / 2) +
                                        (y / 2) * (width / 2) + (x / 2)];
              if (v_sample_actual != v_sample_expected) {
                v_min_diff_x = std::min(v_min_diff_x, x);
                v_min_diff_y = std::min(v_min_diff_y, y);
                v_max_diff_x = std::max(v_max_diff_x, x);
                v_max_diff_y = std::max(v_max_diff_y, y);
              }
            }
          }
        }
        LOGF("luma_min_diff_x: %d luma_max_diff_x: %d luma_min_diff_y: %d luma_max_diff_y: %d",
             luma_min_diff_x, luma_max_diff_x, luma_min_diff_y, luma_max_diff_y);
        LOGF("u_min_diff_x: %d u_max_diff_x: %d u_min_diff_y: %d u_max_diff_y: %d", u_min_diff_x,
             u_max_diff_x, u_min_diff_y, u_max_diff_y);
        LOGF("v_min_diff_x: %d v_max_diff_x: %d v_min_diff_y: %d v_max_diff_y: %d", v_min_diff_x,
             v_max_diff_x, v_min_diff_y, v_max_diff_y);
        for (int32_t y = luma_min_diff_y; y <= luma_max_diff_y; ++y) {
          for (int32_t x = luma_min_diff_x; x <= luma_max_diff_x; ++x) {
            int32_t expected = i420_data[y * width + x];
            int32_t actual = frame_to_compare.data[y * width + x];
            LOGF(
                "luma diff - y: %d x: %d expected: 0x%02x actual: 0x%02x expected minus actual: %d",
                y, x, expected, actual, expected - actual);
          }
        }
        for (int32_t y = u_min_diff_y; y <= u_max_diff_y; y += 2) {
          for (int32_t x = u_min_diff_x; x <= u_max_diff_x; x += 2) {
            uint8_t expected = i420_data[width * height + (y / 2) * (width / 2) + (x / 2)];
            uint8_t actual =
                frame_to_compare.data[width * height + (y / 2) * (width / 2) + (x / 2)];
            LOGF("u diff - y: %d x: %d expected: 0x%02x actual: 0x%02x expected minus actual: %d",
                 y, x, expected, actual, expected - actual);
          }
        }
        for (int32_t y = v_min_diff_y; y <= v_max_diff_y; y += 2) {
          for (int32_t x = v_min_diff_x; x <= v_max_diff_x; x += 2) {
            uint8_t expected = i420_data[width * height + (width / 2) * (height / 2) +
                                         (y / 2) * (width / 2) + (x / 2)];
            uint8_t actual = frame_to_compare.data[width * height + (width / 2) * (height / 2) +
                                                   (y / 2) * (width / 2) + (x / 2)];
            LOGF("v diff - y: %d x: %d expected: 0x%02x actual: 0x%02x expected minus actual: %d",
                 y, x, expected, actual, expected - actual);
          }
        }
        for (int32_t y = luma_min_diff_y; y <= luma_max_diff_y; ++y) {
          std::string line_string;
          for (int32_t x = luma_min_diff_x; x <= luma_max_diff_x; ++x) {
            int32_t expected = i420_data[y * width + x];
            int32_t actual = frame_to_compare.data[y * width + x];
            line_string += fbl::StringPrintf(" %4d", expected - actual);
          }
          LOGF("%s", line_string.c_str());
        }
        LOGF(
            "\n\n\nDECODED FRAME DIFFERS FROM EXPECTED - SEE ABOVE DIFFS - TEST WILL FAIL AFTER "
            "FINISHING UP\n\n\n");
        emit_frame_failure_seen = true;
      }
    } else {
      LOGF("%s frame_index: %u",
           test_params->mime_type ? test_params->mime_type->c_str() : "<no mime type>",
           frame_index);
    }
    // ~increment_frame_index
  };

  if (!decode_video_stream_test(&fidl_loop, fidl_thread, component_context.get(),
                                in_stream_peeker.get(), use_video_decoder, 0,
                                min_output_buffer_count, is_secure_output, is_secure_input,
                                std::move(emit_frame), test_params)) {
    printf("decode_video_stream_test() failed. (%s)\n",
           test_params->frame_to_compare ? "inner" : "outer");
    return -1;
  }

  if (test_params->reset_hash_each_iteration) {
    on_reset_hash();
    if (hash_failure_count != 0) {
      printf("hash_failure_count != 0 -- failing\n");
      return -1;
    }
  }

  if (emit_frame_failure_seen) {
    printf("emit_frame_failure_seen (%s)", test_params->frame_to_compare ? "inner" : "outer");
    return -1;
  }

  if (test_params->min_expected_output_frame_count !=
      UseVideoDecoderTestParams::kDefaultMinExpectedOutputFrameCount) {
    ZX_ASSERT(test_params->min_expected_output_frame_count >= 0);
    if (frame_index < static_cast<uint32_t>(test_params->min_expected_output_frame_count)) {
      printf(
          "frame_index < test_params->min_expected_output_frame_count -- frame_index: %d "
          "min_expected_output_frame_count: %d",
          frame_index, test_params->min_expected_output_frame_count);
      return -1;
    }
  }

  const int internal_expected_frame_count =
      expected_frame_count != -1 ? expected_frame_count : frame_index;
  std::set<uint64_t> expected_timestamps;

  // default 0
  const int64_t first_expected_output_frame_ordinal =
      test_params->first_expected_output_frame_ordinal;

  for (int i = first_expected_output_frame_ordinal; i < internal_expected_frame_count; i++) {
    expected_timestamps.insert(i);
  }
  for (size_t i = 0; i < timestamps.size(); i++) {
    if (!timestamps[i].first) {
      printf("A frame had !has_timstamp_ish - frame_index: %lu\n", i);
      return -1;
    }
    int64_t output_frame_index = i;
    int64_t timestamp_ish = timestamps[i].second;
    if (timestamp_ish < output_frame_index - 1 && timestamp_ish > output_frame_index + 1) {
      printf(
          "A frame had output timestamp_ish out of order beyond expected "
          "degree of re-ordering - frame_index: %lu timestamp_ish: "
          "%lu\n",
          i, timestamps[i].second);
      return -1;
    }
    if (expected_timestamps.find(timestamps[i].second) == expected_timestamps.end()) {
      printf(
          "A frame had timestamp_ish not in the expected set (or duplicated) - "
          "frame_index: %lu timestamp_ish: %lu\n",
          i, timestamps[i].second);
      return -1;
    }
    expected_timestamps.erase(timestamps[i].second);
  }
  if (!expected_timestamps.empty()) {
    printf("not all expected_timestamps seen\n");
    for (uint64_t timestamp : expected_timestamps) {
      printf("missing timestamp: %lu\n", timestamp);
    }
    return -1;
  }

  if (got_output_data) {
    if (test_params->golden_sha256) {
      std::string actual_sha256 = GetSha256SoFar(&sha256_ctx);
      printf("Done decoding - computed sha256 is: %s\n", actual_sha256.c_str());
      if (test_params->golden_sha256 != UseVideoDecoderTestParams::kDefaultGoldenSha256) {
        if (strcmp(actual_sha256.c_str(), test_params->golden_sha256)) {
          printf("The sha256 doesn't match - expected: %s actual: %s\n", test_params->golden_sha256,
                 actual_sha256.c_str());
          return -1;
        }
        printf("The computed sha256 matches golden sha256.  Yay!\n");
      }
    }
  } else if (is_secure_output) {
    printf("Can't check output data sha256 because output is secure.\n");
  } else if (test_params->skip_formatting_output_pixels) {
    printf("Can't check output data sha256 because skip_formatting_output_pixels\n");
  } else {
    printf("No output data received");
    return -1;
  }

  fidl_loop.Quit();
  fidl_loop.JoinThreads();
  component_context = nullptr;
  fidl_loop.Shutdown();

  printf("PASS\n");
  return 0;
}

bool decode_video_stream_test(async::Loop* fidl_loop, thrd_t fidl_thread,
                              sys::ComponentContext* component_context,
                              InStreamPeeker* in_stream_peeker,
                              UseVideoDecoderFunction use_video_decoder,
                              uint64_t min_output_buffer_size, uint32_t min_output_buffer_count,
                              bool is_secure_output, bool is_secure_input, EmitFrame emit_frame,
                              const UseVideoDecoderTestParams* test_params) {
  fuchsia::mediacodec::CodecFactoryHandle codec_factory;

  component_context->svc()->Connect<fuchsia::mediacodec::CodecFactory>(codec_factory.NewRequest());
  fuchsia::sysmem::AllocatorHandle sysmem;

  component_context->svc()->Connect<fuchsia::sysmem::Allocator>(sysmem.NewRequest());

  std::unique_ptr<InputCopier> input_copier;
  if (is_secure_input)
    input_copier = InputCopier::Create();

  UseVideoDecoderParams params{.fidl_loop = fidl_loop,
                               .fidl_thread = fidl_thread,
                               .codec_factory = std::move(codec_factory),
                               .sysmem = std::move(sysmem),
                               .in_stream = in_stream_peeker,
                               .input_copier = input_copier.get(),
                               .min_output_buffer_size = min_output_buffer_size,
                               .min_output_buffer_count = min_output_buffer_count,
                               .is_secure_output = is_secure_output,
                               .is_secure_input = is_secure_input,
                               .lax_mode = false,
                               .emit_frame = std::move(emit_frame),
                               .test_params = test_params};
  use_video_decoder(std::move(params));

  return true;
}
