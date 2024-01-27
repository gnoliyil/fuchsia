// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/testing/integration/hermetic_fidelity_test.h"

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/thermal/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>
#include <zircon/types.h>

#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include <test/thermal/cpp/fidl.h>

#include "src/lib/fxl/strings/concatenate.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/media/audio/audio_core/shared/device_id.h"
#include "src/media/audio/audio_core/testing/integration/renderer_shim.h"
#include "src/media/audio/lib/analysis/analysis.h"
#include "src/media/audio/lib/analysis/generators.h"
#include "src/media/audio/lib/clock/clone_mono.h"
#include "src/media/audio/lib/format/audio_buffer.h"

using ASF = fuchsia::media::AudioSampleFormat;

namespace media::audio::test {

// Value related to cmdline flags
//
// If --save-input-and-output is specified, saving input|output files for every test frequency
// consumes too much on-device storage. Just save the files for this specified frequency.
constexpr int32_t kFrequencyForSavedWavFiles = 1000;

//
// Custom build-time flags
//
// For normal CQ operation, the below should be FALSE.
//
// Debug positioning and values of the renderer's input buffer, by showing certain sections.
constexpr bool kDisplayInputBuffer = false;
// Debug positioning and values of the output ring buffer snapshot, by showing certain sections.
constexpr bool kDisplayOutputBuffer = false;
// If debugging input/output ring buffer contents, show sections for ALL frequencies.
constexpr bool kDisplayBuffersAtAllFrequencies = false;
// Retain/display worst-case single-test-case results in a looped run. Used to update limits.
constexpr bool kRetainWorstCaseResults = false;
// Show results at test-end in tabular form, for copy/compare to results vectors.
constexpr bool kDisplaySummaryResults = false;
//
// For normal CQ operation, the below should be TRUE.  (They aid in debugging sporadic CQ issues.)
//
// Displaying results on-the-fly helps correlate an UNDERFLOW with the affected frequency.
constexpr bool kDisplayInProgressResults = true;
// On significant FR/SiNAD failure, display relevant output buffer sections or analysis metadata.
constexpr bool kDisplayOutputBufferOnFailure = true;
constexpr bool kDisplayAnalysisDataOnFailure = true;

// Related configuration
//
// How many input frames on either side of "positions of interest" to display
constexpr int64_t kInputDisplayWindow = 16;
// How many output frames on either side of "positions of interest" to display
constexpr int64_t kOutputDisplayWindow = 48;
// Displaying a larger set of "beginning of signal" and "end of signal" frames helps us diagnose
// output delays and incorrect pipeline widths.
constexpr int64_t kOutputAdditionalSignalStartDisplayWindow = 128;
constexpr int64_t kOutputAdditionalSignalEndDisplayWindow = 80;
// If not displaying buffers at all frequencies, only show this one (applies to input and output).
// 1 kHz is a reasonable mid-range input for saved files, debugging, and single-frequency tests.
constexpr int32_t kFrequencyForBufferDebugging = 1000;
// Dumping buffers for every failure may be too verbose. Only dump ones where FR fails by 20 dB...
constexpr double kDisplayOutputBufferOnFailureFreqRespDbTolerance = 20.0;
// ... or SiNAD fails by 20 dB.
constexpr double kDisplayOutputBufferOnFailureSinadDbTolerance = 20.0;
// Only display analysis data (significant frequency bins) if FR or SiNAD fail by 20 dB.
constexpr double kDisplayAnalysisDataOnFailureDbTolerance = 20.0;

//
// Consts related to fidelity testing thresholds
//
// The power-of-two size of our spectrum analysis buffer, and our frequency spectrum set.
constexpr int64_t kFreqTestBufSize = 65536;
// When testing fidelity, we compare actual measured dB to expected dB. These tests are designed
// to pass if 'actual >= expected', OR less but within the following tolerance. This tolerance
// also sets the digits of precision for 'expected' values, when stored or displayed.
// TODO(fxbug.dev/116506): Revisit our "measurement vs. enforcement" stance; revert this to 0.001.
constexpr double kFidelityDbTolerance = 1.0;
// If kDisplayAnalysisDataOnFailureDbTolerance, display freq bins with magnitude >= this val.
constexpr double kMinAnalysisMagnitudeToDisplay = 1e-4;

// For each test_name|channel, we maintain two results arrays: Frequency Response and
// Signal-to-Noise-and-Distortion (SiNAD). A map of array results is saved as a function-local
// static variable. If kRetainWorstCaseResults is set, we persist results across repeated test runs.
//
// Note: two test cases must not collide on the same test_name/channel. Thus, test cases must take
// care not to reuse test_name upon copy-and-paste.
struct ResultsIndex {
  std::string test_name;
  int32_t channel;

  bool operator<(const ResultsIndex& rhs) const {
    return std::tie(test_name, channel) < std::tie(rhs.test_name, rhs.channel);
  }
};

// When displaying in-progress results, storing worst-case results and comparing results to required
// minimum thresholds, we use the actual level_db and sinad_db values as calculated.
// Purely when displaying summary results, we cap level_db and sinad_db to 0 and 160 respectively.
//
// Why? Summary results are usually shown for copying to results vectors (e.g. fidelity_results.cc)
// to be used for future pass/fail comparisons. When making these assessments, frequency response >0
// dB is not preferable to 0 dB. Most all analog hardware cannot realize SiNAD >160 dB, and within
// our current implementation a value >160 dB is clearly a "harmonic spike" since it would require
// >26.5 bits of precision (exceeding the precision of a normalized-float32 pipeline like ours).
// Also, clamping our required SiNAD to 160 dB conceptually pairs with FIDL constant MUTED_GAIN_DB.
constexpr double kMaxFrequencyResponse = 0.0;
constexpr double kMaxSignalToNoiseAndDistortion = 160.0;

// static
std::array<double, HermeticFidelityTest::kNumReferenceFreqs> HermeticFidelityTest::FillArray(
    double val) {
  std::array<double, HermeticFidelityTest::kNumReferenceFreqs> arr;
  arr.fill(val);
  return arr;
}

// static
// Retrieve (initially allocating, if necessary) the array of level results for this path|channel.
std::array<double, HermeticFidelityTest::kNumReferenceFreqs>& HermeticFidelityTest::level_results(
    std::string test_name, int32_t channel) {
  // Allocated only when first needed, and automatically cleaned up when process exits.
  static auto results_level_db =
      new std::map<ResultsIndex, std::array<double, HermeticFidelityTest::kNumReferenceFreqs>>();

  ResultsIndex index{
      .test_name = std::move(test_name),
      .channel = channel,
  };
  if (results_level_db->find(index) == results_level_db->end()) {
    auto& results = (*results_level_db)[index];
    std::fill(results.begin(), results.end(), std::numeric_limits<double>::infinity());
  }

  return results_level_db->find(index)->second;
}

// static
// Retrieve (initially allocating, if necessary) the array of SiNAD results for this path|channel.
// A map of these array results is saved as a function-local static variable.
std::array<double, HermeticFidelityTest::kNumReferenceFreqs>& HermeticFidelityTest::sinad_results(
    std::string test_name, int32_t channel) {
  // Allocated only when first needed, and automatically cleaned up when process exits.
  static auto results_sinad_db =
      new std::map<ResultsIndex, std::array<double, HermeticFidelityTest::kNumReferenceFreqs>>();

  ResultsIndex index{
      .test_name = std::move(test_name),
      .channel = channel,
  };
  if (results_sinad_db->find(index) == results_sinad_db->end()) {
    auto& results = (*results_sinad_db)[index];
    std::fill(results.begin(), results.end(), std::numeric_limits<double>::infinity());
  }

  return results_sinad_db->find(index)->second;
}

void HermeticFidelityTest::SetUp() {
  HermeticPipelineTest::SetUp();

  // We save input|output files if requested. Ensure the requested frequency is one we measure.
  save_fidelity_wav_files_ = HermeticPipelineTest::save_input_and_output_files_;
  if (save_fidelity_wav_files_) {
    bool requested_frequency_found = false;
    for (auto freq : kReferenceFrequencies) {
      if (freq == kFrequencyForSavedWavFiles) {
        requested_frequency_found = true;
        break;
      }
    }

    if (!requested_frequency_found) {
      FX_LOGS(WARNING) << kFrequencyForSavedWavFiles
                       << " is not in the frequency list, a WAV file cannot be saved";
      save_fidelity_wav_files_ = false;
    }
  }
}

// Translate real-world frequencies into 'internal_periods', the number of complete wavelengths that
// fit perfectly into our signal buffer. If this is an integer, we won't need to Window the output
// before frequency analysis. Example: when measuring real-world frequency 2000 Hz at frame rate 96
// kHz, for buffer size 65536 this translates into 1365.333... periods, but we use the integer 1365.
// This translates back to a real-world frequency of 1999.5 Hz, which is not a problem.
//
// We also want internal_periods to have fewer common factors with our buffer size and frame rates,
// as this can mask problems where previous buffer sections are erroneously repeated. So if it is
// not integral, we return the odd neighbor rather than round.
int32_t HermeticFidelityTest::FrequencyToPeriods(int32_t device_frame_rate, int32_t frequency) {
  double internal_periods = static_cast<double>(frequency * kFreqTestBufSize) / device_frame_rate;
  auto floor_periods = static_cast<int32_t>(std::floor(internal_periods));
  auto ceil_periods = static_cast<int32_t>(std::ceil(internal_periods));
  return (floor_periods % 2) ? floor_periods : ceil_periods;
}

template <fuchsia::media::AudioSampleFormat InputFormat,
          fuchsia::media::AudioSampleFormat OutputFormat>
std::vector<HermeticFidelityTest::Frequency> HermeticFidelityTest::GetTestFrequencies(
    const HermeticFidelityTest::TestCase<InputFormat, OutputFormat>& tc) {
  if (tc.single_frequency_to_test.has_value()) {
    auto freq_display_val = tc.single_frequency_to_test.value();
    // If `single_frequency_to_test` is specified, it should be in kReferenceFrequencies.
    // If not, round down to a lower frequency that is, and use those results.
    size_t freq_idx;
    for (freq_idx = 0u; freq_idx < kNumReferenceFreqs - 1; ++freq_idx) {
      if (freq_display_val < kReferenceFrequencies[freq_idx + 1]) {
        break;
      }
    }
    if (freq_display_val != kReferenceFrequencies[freq_idx]) {
      FX_LOGS(WARNING) << "Frequency " << freq_display_val
                       << " not found in kReferenceFrequencies. Using "
                       << kReferenceFrequencies[freq_idx] << " Hz instead";
      freq_display_val = kReferenceFrequencies[freq_idx];
    }
    return {{
        .display_val = freq_display_val,
        .periods = FrequencyToPeriods(tc.output_format.frames_per_second(), freq_display_val),
        .idx = freq_idx,
    }};
  }

  std::vector<HermeticFidelityTest::Frequency> frequencies;
  for (auto freq_idx = 0u; freq_idx < kNumReferenceFreqs; ++freq_idx) {
    frequencies.push_back({
        .display_val = kReferenceFrequencies[freq_idx],
        .periods = FrequencyToPeriods(tc.output_format.frames_per_second(),
                                      kReferenceFrequencies[freq_idx]),
        .idx = freq_idx,
    });
  }
  return frequencies;
}

// Render source such that first input frame will be rendered into first ring buffer frame.
// Create a renderer, submit packets, play, wait for them to be rendered, shut down the renderer,
// and extract the output from the VAD ring buffer.
template <ASF InputFormat, ASF OutputFormat>
AudioBuffer<OutputFormat> HermeticFidelityTest::GetRendererOutput(
    TypedFormat<InputFormat> input_format, int64_t input_buffer_frames, RenderPath path,
    AudioBuffer<InputFormat> input, VirtualOutput<OutputFormat>* device, ClockMode clock_mode,
    std::optional<float> gain_db) {
  RendererShimImpl* renderer;

  if (path != RenderPath::Ultrasound) {
    std::optional<zx::clock> clock;
    zx::clock::update_args args;
    zx::clock offset_clock;
    zx::time now;
    switch (clock_mode) {
      case ClockMode::Default:
        break;
      case ClockMode::Flexible:
        clock = zx::clock(ZX_HANDLE_INVALID);
        break;
      case ClockMode::Monotonic:
        clock = audio::clock::CloneOfMonotonic();
        break;
      case ClockMode::Offset:
        // Set a reference clock with an offset of +20usec.
        EXPECT_EQ(zx::clock::create(ZX_CLOCK_OPT_MONOTONIC | ZX_CLOCK_OPT_CONTINUOUS, nullptr,
                                    &offset_clock),
                  ZX_OK)
            << "Offset clock could not be created";
        now = zx::clock::get_monotonic();
        args.reset().set_both_values(now, now + zx::usec(20));
        EXPECT_EQ(offset_clock.update(args), ZX_OK) << "clock.update with set_both_values failed";
        clock = std::move(offset_clock);
        break;
      case ClockMode::RateAdjusted:
        clock = audio::clock::AdjustableCloneOfMonotonic();
        args.reset().set_rate_adjust(100);
        EXPECT_EQ(clock->update(args), ZX_OK) << "Could not rate-adjust a custom clock";
        break;
    }

    fuchsia::media::AudioRenderUsage usage = fuchsia::media::AudioRenderUsage::MEDIA;
    if (path == RenderPath::Communications) {
      usage = fuchsia::media::AudioRenderUsage::COMMUNICATION;
    }
    auto audio_renderer =
        CreateAudioRenderer(input_format, input_buffer_frames, usage, std::move(clock));
    if (gain_db.has_value()) {
      audio_renderer->SetGain(*gain_db);
    }
    renderer = audio_renderer;
  } else {
    renderer = CreateUltrasoundRenderer(input_format, input_buffer_frames, true);
  }
  auto packets = renderer->AppendPackets<InputFormat>({&input});

  renderer->PlaySynchronized(this, device, 0);
  renderer->WaitForPackets(this, packets);

  auto buffer = device->SnapshotRingBuffer();

  // Free the renderer now, instead of accumulating one for every test frequency.
  if (path == RenderPath::Ultrasound) {
    Unbind(static_cast<UltrasoundRendererShim<InputFormat>*>(renderer));
  } else {
    Unbind(static_cast<AudioRendererShim<InputFormat>*>(renderer));
  }
  return buffer;
}

// Measuring system response requires providing enough input for a full output response.
//
// Our input buffer contains initial silence, (more-than-enough) signal, then final silence.
// The [silence+signal+silence] must include adequate length for OUTPUT ramp-up and stabilization,
// a sufficient section of fully stabilized signal for analysis, and ultimately ramp-down/ring-out.
//
// Output ramping may occur before AND after input transitions, so we refer to 5 output sections:
//   ramp-in,         initial stabilization, analysis section, final stabilization, ramp-out.
// The input signal contains these directly-corresponding sections:
//   initial silence, initial stabilization, analysis section, final stabilization, final silence.
//
// For this source                              ___________________________                       .
// input signal, with             _____________|                           |____________________  .
// Initial and Final                                                                              .
// frames I and F:                             I                          F                       .
//                                                                                                .
// A system may produce                        /\_^=~_~_--------------^-_~/\_                     .
// this output signal:            -------_~w^_/                              \/~_=w~-/\~v-_-----  .
//                                                      ^            ^                            .
// "Ramp-in" (pre I):                    RRRRRR         .            .                            .
// "initial Stabilization" (at/post I):        SSSSSSSSS.            .                            .
// "final (De)stabilization" (pre/at F):                .            .DDDDD                       .
// "ramp-Out"/"ring-Out" (post F):                      .            .     OOOOOOOOOOOOOOOO       .
// stable "Analysis section":                           AAAAAAAAAAAAAA                            .
//                                                                                                .
// Thus, our source signals                     ___________________________                       .
// conceptually include the       _____________|                           |____________________  .
// corresponding sections:                                                                        .
// 1: initial silence             1111111111111                                                   .
// 2: initial stabilization                    222222222                                          .
// 3: analysis section                                  33333333333333                            .
// 4: final stabilization                                             44444                       .
// 5: final silence                                                        555555555555555555555  .
//
// Test writers use HermeticPipelineTest::PipelineConstants to convey these transition widths.

// static
// Input buffer should contain exact silence for first/last sections and immediate continuous signal
// across the three middle sections, with a full-scale value at start of analysis section (this
// becomes the OUTPUT analysis section's first frame). Depending on input signal frequency, there
// will be an identical full-scale value at either start of final stabilization (periods-per-buffer
// is integral), or earlier by less than a frame (if non-integral). Conceptually, these values must
// be identical so that the resulting (guaranteed-integral) output analysis section can be perfectly
// "infinitely looped" (which is how spectral-analysis FFT essentially treats it).
template <ASF InputFormat>
void HermeticFidelityTest::DisplayInputBufferSections(const AudioBuffer<InputFormat>& buffer,
                                                      const std::string& initial_tag,
                                                      const SignalSectionIndices& input_indices) {
  printf("\n");
  buffer.Display(0, kInputDisplayWindow, initial_tag);
  buffer.Display(input_indices.stabilization_start - kInputDisplayWindow,
                 input_indices.stabilization_start,
                 "End of initial silence (should be entirely silent)");

  buffer.Display(input_indices.stabilization_start,
                 input_indices.stabilization_start + kInputDisplayWindow,
                 "Start of initial stabilization (should start immediately)");
  buffer.Display(input_indices.analysis_start - kInputDisplayWindow, input_indices.analysis_start,
                 "End of initial stabilization (should lead toward a full-scale value)");

  buffer.Display(input_indices.analysis_start, input_indices.analysis_start + kInputDisplayWindow,
                 "Start of signal-to-be-analyzed (should start at a full-scale value)");
  buffer.Display(input_indices.analysis_end - kInputDisplayWindow, input_indices.analysis_end,
                 "End of signal-to-be-analyzed (should lead toward a full-scale value)");

  buffer.Display(input_indices.analysis_end, input_indices.analysis_end + kInputDisplayWindow,
                 "Start of final stabilization (should start at/after a full-scale value)");
  buffer.Display(input_indices.stabilization_end - kInputDisplayWindow,
                 input_indices.stabilization_end,
                 "End of final stabilization (should continue without attenuation)");

  buffer.Display(input_indices.stabilization_end,
                 input_indices.stabilization_end + kInputDisplayWindow,
                 "Start of final_silence (should be immediately silent)");
  buffer.Display(buffer.NumFrames() - kInputDisplayWindow, buffer.NumFrames(),
                 "End of final silence (and end of input buffer)");
}

// static
// If output pipeline has no phase shift, then we expect full-scale values in both first frame of
// analysis section, and first frame after analysis section. If pipeline has phase shift, they
// should still be identical but may not be full-scale (analysis section should still be loopable).
template <ASF OutputFormat>
void HermeticFidelityTest::DisplayOutputBufferSections(const AudioBuffer<OutputFormat>& buffer,
                                                       const std::string& initial_tag,
                                                       const SignalSectionIndices& output_indices) {
  printf("\n");
  buffer.Display(0, kOutputDisplayWindow, initial_tag);
  buffer.Display(output_indices.stabilization_start - kOutputDisplayWindow,
                 output_indices.stabilization_start,
                 "End of ramp-in (may end in destabilization, then sudden rise)");

  buffer.Display(output_indices.stabilization_start,
                 output_indices.stabilization_start + kOutputDisplayWindow,
                 "Start of initial stabilization (may start with overshoot; should stabilize)");
  buffer.Display(output_indices.analysis_start - kOutputDisplayWindow -
                     kOutputAdditionalSignalStartDisplayWindow,
                 output_indices.analysis_start,
                 "End of initial stabilization (should fully stabilize by end of section)");

  buffer.Display(output_indices.analysis_start,
                 output_indices.analysis_start + kOutputDisplayWindow +
                     kOutputAdditionalSignalStartDisplayWindow,
                 "Start of analysis section (should start with max value for this channel)");
  buffer.Display(
      output_indices.analysis_end - kOutputDisplayWindow - kOutputAdditionalSignalEndDisplayWindow,
      output_indices.analysis_end,
      "End of analysis section (should resemble end of initial stabilization)");

  buffer.Display(
      output_indices.analysis_end,
      output_indices.analysis_end + kOutputDisplayWindow + kOutputAdditionalSignalEndDisplayWindow,
      "Start of final stabilization (should resemble start of analysis section)");
  buffer.Display(output_indices.stabilization_end - kOutputDisplayWindow,
                 output_indices.stabilization_end,
                 "End of final stabilization (may destabilize, then suddenly drop)");

  buffer.Display(output_indices.stabilization_end,
                 output_indices.stabilization_end + kOutputDisplayWindow,
                 "Start of final ramp-out (may be unstable)");
  buffer.Display(buffer.NumFrames() - kOutputDisplayWindow, buffer.NumFrames(),
                 "End of output buffer (should be silent)");
}

template <ASF InputFormat, ASF OutputFormat>
void HermeticFidelityTest::DisplaySummaryResults(
    const TestCase<InputFormat, OutputFormat>& test_case,
    const std::vector<HermeticFidelityTest::Frequency>& frequencies_to_display) {
  // Loop by channel, displaying summary results, in a separate loop from checking each result.
  std::string single_freq_info;
  if (frequencies_to_display.size() == 1) {
    single_freq_info =
        fxl::Concatenate({" source ", std::to_string(frequencies_to_display[0].display_val),
                          " Hz [", std::to_string(frequencies_to_display[0].idx), "] -"});
  }

  for (const auto& channel_spec : test_case.channels_to_measure) {
    // Show results in tabular forms, for easy copy into hermetic_fidelity_results.cc.
    // We don't enforce greater-than-unity response if it occurs, so clamp these to a max of 0.0.
    const auto& chan_level_results_db = level_results(test_case.test_name, channel_spec.channel);
    printf("\n\tFull-spectrum Frequency Response - %s -%s output channel %d",
           test_case.test_name.c_str(), single_freq_info.c_str(), channel_spec.channel);
    for (const auto& freq : frequencies_to_display) {
      printf("%s %8.3f,", (freq.idx % 10 == 0 ? "\n" : ""),
             std::min(floor(chan_level_results_db[freq.idx] / kFidelityDbTolerance) *
                          kFidelityDbTolerance,
                      kMaxFrequencyResponse));
    }
    printf("\n");

    const auto& chan_sinad_results_db = sinad_results(test_case.test_name, channel_spec.channel);
    printf("\n\tSignal-to-Noise and Distortion -   %s -%s output channel %d",
           test_case.test_name.c_str(), single_freq_info.c_str(), channel_spec.channel);
    for (const auto& freq : frequencies_to_display) {
      printf("%s %8.3f,", (freq.idx % 10 == 0 ? "\n" : ""),
             std::min(floor(chan_sinad_results_db[freq.idx] / kFidelityDbTolerance) *
                          kFidelityDbTolerance,
                      kMaxSignalToNoiseAndDistortion));
    }
    printf("\n\n");
  }
}

template <ASF InputFormat, ASF OutputFormat>
void HermeticFidelityTest::VerifyResults(
    const TestCase<InputFormat, OutputFormat>& test_case,
    const std::vector<HermeticFidelityTest::Frequency>& frequencies_to_verify) {
  // Loop by channel_to_measure
  for (const auto& channel_spec : test_case.channels_to_measure) {
    const auto& chan_level_results_db = level_results(test_case.test_name, channel_spec.channel);
    for (const auto& freq : frequencies_to_verify) {
      EXPECT_GE(chan_level_results_db[freq.idx],
                channel_spec.freq_resp_lower_limits_db[freq.idx] - kFidelityDbTolerance)
          << "  Channel " << channel_spec.channel << ", FreqResp [" << std::setw(2) << freq.idx
          << "]  (" << std::setw(5) << freq.display_val << " Hz):  " << std::setprecision(7)
          << floor(chan_level_results_db[freq.idx] / kFidelityDbTolerance) * kFidelityDbTolerance;
    }

    const auto& chan_sinad_results_db = sinad_results(test_case.test_name, channel_spec.channel);
    for (const auto& freq : frequencies_to_verify) {
      EXPECT_GE(chan_sinad_results_db[freq.idx],
                channel_spec.sinad_lower_limits_db[freq.idx] - kFidelityDbTolerance)
          << "  Channel " << channel_spec.channel << ", SINAD    [" << std::setw(2) << freq.idx
          << "]  (" << std::setw(5) << freq.display_val << " Hz):  " << std::setprecision(7)
          << floor(chan_sinad_results_db[freq.idx] / kFidelityDbTolerance) * kFidelityDbTolerance;
    }
  }
}

// Additional fidelity assessments, potentially added in the future:
// (1) Dynamic range (1kHz input at -30/60/90 db: measure level, SiNAD. Overall gain sensitivity)
//     This should clearly show the impact of dynamic compression in the effects chain.
// (2) Assess the e2e input data path (from device to capturer)
//     Included for completeness: we apply no capture effects; should equal audio_fidelity_tests.
template <ASF InputFormat, ASF OutputFormat>
void HermeticFidelityTest::Run(
    const HermeticFidelityTest::TestCase<InputFormat, OutputFormat>& tc) {
  // Translate from input frame number to output frame number.
  // Return a double-precision float; let the caller decide whether/how to reduce it to int.
  auto input_frame_to_output_frame = [tc](int64_t input_frame) {
    return static_cast<double>(input_frame * tc.output_format.frames_per_second()) /
           static_cast<double>(tc.input_format.frames_per_second());
  };
  // Translate from output frame number to input frame number.
  auto output_frame_to_input_frame = [tc](int64_t output_frame) {
    return static_cast<double>(output_frame * tc.input_format.frames_per_second()) /
           static_cast<double>(tc.output_format.frames_per_second());
  };

  if (tc.path == RenderPath::Ultrasound) {
    ASSERT_EQ(tc.renderer_clock_mode, ClockMode::Default)
        << "Ultrasound path cannot be tested with a non-default clock";
  }

  // We will analyze a specific number of output frames (our 'analysis section'). Depending on
  // rate-conversion, this translates to a different number of input signal frames.
  //
  // We'll need this potentially-fractional input-signal-length value later.
  auto input_signal_frames_to_measure_double = output_frame_to_input_frame(kFreqTestBufSize);

  // Our frequency analysis does not window the output it receives, which means we want a specific
  // number of (integral) signal wavelengths to fit within the OUTPUT buffer analysis section.
  // We want the SAME number of wavelengths in our INPUT signal (regardless of rate-conversion
  // ratio), but the LENGTH of that input signal is scaled by rate-conversion ratio and becomes
  // input_signal_frames_to_measure.
  //
  // However, certain rate-conversion ratios WOULD lead to non-integral input buffer lengths!
  // Buffer lengths of course must be integral, but frequencies need not be.
  // If our ideal input length WOULD be fractional, we (1) "ceiling" the input buffer length to be
  // integral, then compensate later by (2) adjusting input frequency correspondingly.
  // We insert a slightly-larger number of signal wavelengths in our slightly-larger (integral)
  // input buffer, which is equivalent to inserting the intended (integral) number of signal
  // wavelengths in the FRACTIONAL input length that (via rate-conversion) will translate perfectly
  // to the integral frequency, within an output buffer of the required integral length.
  //
  // Here's the actual (integral) signal length corresponding to the output section we analyze.
  // We use input_signal_frames_to_measure_double later, if we must adjust the source frequency.
  auto input_signal_frames_to_measure =
      static_cast<int64_t>(std::ceil(input_signal_frames_to_measure_double));

  // Compute lengths of the other portions of our full input signal, so that we generate an output
  // signal with a fully-stabilized steady-state analysis section.
  auto init_silence_len =
      static_cast<int64_t>(std::max(tc.pipeline.ramp_in_width, tc.pipeline.pos_filter_width));
  auto init_stabilization_len =
      static_cast<int64_t>(std::max(tc.pipeline.stabilization_width, tc.pipeline.neg_filter_width));
  auto final_stabilization_len = static_cast<int64_t>(
      std::max(tc.pipeline.destabilization_width, tc.pipeline.pos_filter_width));
  auto final_silence_len =
      static_cast<int64_t>(std::max(tc.pipeline.decay_width, tc.pipeline.neg_filter_width));

  auto input_type_mono =
      Format::Create<InputFormat>(1, tc.input_format.frames_per_second()).take_value();
  auto init_silence = GenerateSilentAudio(input_type_mono, init_silence_len);
  auto final_silence = GenerateSilentAudio(input_type_mono, final_silence_len);

  auto input_stabilization_start = init_silence_len;
  auto input_analysis_start = input_stabilization_start + init_stabilization_len;
  auto input_analysis_end = input_analysis_start + input_signal_frames_to_measure;
  auto input_stabilization_end = input_analysis_end + final_stabilization_len;

  auto input_signal_len =
      init_stabilization_len + input_signal_frames_to_measure + final_stabilization_len;
  auto total_input_buffer_len = init_silence_len + input_signal_len + final_silence_len;
  if constexpr (kDisplayInputBuffer) {
    FX_LOGS(INFO) << "init_silence_len " << init_silence_len << " + pre-stabilization "
                  << init_stabilization_len << " + frames_to_measure "
                  << input_signal_frames_to_measure << " + post-stabilization "
                  << final_stabilization_len << " + final_silence_len " << final_silence_len
                  << " = total buffer " << total_input_buffer_len;
  }

  // We create the AudioBuffer later. Ensure no out-of-range channels are requested to play.
  for (const auto& channel : tc.channels_to_play) {
    ASSERT_LT(static_cast<int32_t>(channel), tc.input_format.channels())
        << "Cannot play out-of-range input channel";
  }

  // Calculate the output buffer length needed for our total input signal (initial silence, full
  // ramp-in, the signal to be analyzed, and full ramp-out). Set up a virtual audio device with
  // a ring-buffer large enough to receive that output length. Round up any partial frames, to
  // guarantee we have adequate output space for the full input signal.
  auto output_buffer_frames_needed =
      static_cast<int64_t>(std::ceil(input_frame_to_output_frame(total_input_buffer_len)));

  audio_stream_unique_id_t device_id = AUDIO_STREAM_UNIQUE_ID_BUILTIN_SPEAKERS;
  if (tc.device_id.has_value()) {
    device_id = tc.device_id.value();
  }
  auto device = CreateOutput(device_id, tc.output_format, output_buffer_frames_needed, std::nullopt,
                             tc.pipeline.output_device_gain_db);

  if (tc.thermal_state.has_value()) {
    if (ConfigurePipelineForThermal(tc.thermal_state.value()) != ZX_OK) {
      return;
    }
  }

  for (const auto& effect_config : tc.effect_configs) {
    fuchsia::media::audio::EffectsController_UpdateEffect_Result result;
    auto status =
        effects_controller()->UpdateEffect(effect_config.name, effect_config.config, &result);
    ASSERT_EQ(status, ZX_OK);
  }

  int32_t nyquist_limit, low_pass_frequency;
  nyquist_limit =
      std::min(tc.input_format.frames_per_second(), tc.output_format.frames_per_second()) / 2;

  low_pass_frequency = tc.low_pass_frequency.value_or(nyquist_limit);
  if (tc.low_pass_frequency.has_value() && tc.low_pass_frequency.value() > nyquist_limit) {
    FX_LOGS(WARNING) << "low_pass_frequency (" << tc.low_pass_frequency.value()
                     << ") should not exceed the Nyquist limits for this input/output pair ("
                     << tc.input_format.frames_per_second() << ", "
                     << tc.output_format.frames_per_second() << "): reducing low_pass_frequency to "
                     << nyquist_limit;
    low_pass_frequency = nyquist_limit;
  }

  ASSERT_GE(tc.low_cut_frequency, 0)
      << "low_cut_frequency (" << tc.low_cut_frequency << ") cannot be negative";
  ASSERT_LE(tc.low_cut_frequency, low_pass_frequency)
      << "low_cut_frequency (" << tc.low_cut_frequency << ") cannot exceed low_pass_frequency ("
      << low_pass_frequency << ")";

  if (tc.single_frequency_to_test.has_value()) {
    ASSERT_LE(tc.single_frequency_to_test.value(), nyquist_limit)
        << "Specified frequency (" << tc.single_frequency_to_test.value() << ") exceeds "
        << nyquist_limit << ", the Nyquist limit for this input/output pair ("
        << tc.input_format.frames_per_second() << ", " << tc.output_format.frames_per_second()
        << ")";
    if (tc.low_pass_frequency.has_value()) {
      ASSERT_LE(tc.single_frequency_to_test.value(), tc.low_pass_frequency.value())
          << "Specified frequency (" << tc.single_frequency_to_test.value() << ") exceeds "
          << tc.low_pass_frequency.value() << ", the specified low-pass limit";
    }
    ASSERT_GE(tc.single_frequency_to_test.value(), tc.low_cut_frequency)
        << "Specified frequency (" << tc.single_frequency_to_test.value() << ") is less than "
        << tc.low_cut_frequency << ", the specified low-cut limit";
  }

  // This is the factor mentioned earlier (where we set input_signal_frames_to_measure_double). We
  // apply this adjustment to freq, to perfectly fit an integral number of wavelengths into the
  // intended FRACTIONAL Input buffer length. (This fractional input length is translated via
  // rate-conversion into the exact integral Output buffer length used in our analysis.)
  auto source_rate_adjustment_factor =
      static_cast<double>(input_signal_len) / input_signal_frames_to_measure_double;

  // Generate rate-specific internal frequency values for our power-of-two-sized analysis buffer.
  auto frequencies_to_test = GetTestFrequencies(tc);

  // Process each frequency completely, one at a time
  for (Frequency freq : frequencies_to_test) {
    auto adjusted_periods = source_rate_adjustment_factor * static_cast<double>(freq.periods);

    if (freq.display_val * 2 > tc.input_format.frames_per_second() ||
        adjusted_periods * 2.0 > static_cast<double>(input_signal_len)) {
      continue;
    }

    // To make it easier to debug the generation of the input signal, include a phase offset so that
    // the beginning of the signal section is aligned with the exact beginning of the cosine signal.
    // But don't apply any phase offset if the frequency is zero.
    auto phase = freq.periods ? (-2.0 * M_PI * static_cast<double>(init_stabilization_len) *
                                 adjusted_periods / static_cast<double>(input_signal_len))
                              : 0.0;
    auto amplitude = SampleFormatTraits<InputFormat>::kUnityValue -
                     SampleFormatTraits<InputFormat>::kSilentValue;
    auto signal_section =
        GenerateCosineAudio(input_type_mono, input_signal_len, adjusted_periods, amplitude, phase);

    // Write input signal to input buffer. This starts with silence for pre-ramp-in (which aligns
    // input and output WAV files, if enabled). Before/after signal_section, we include additional
    // signal to account for the stabilization periods corresponding to input signal start and end.
    auto input_mono = init_silence;
    input_mono.Append(AudioBufferSlice(&signal_section));
    input_mono.Append(AudioBufferSlice(&final_silence));
    FX_CHECK(input_mono.NumFrames() == static_cast<int64_t>(total_input_buffer_len))
        << "Incorrect input_mono length: testcode logic error";

    auto silence_mono = GenerateSilentAudio(input_type_mono, total_input_buffer_len);

    std::vector<AudioBufferSlice<InputFormat>> channels;
    for (auto play_channel = 0; play_channel < tc.input_format.channels(); ++play_channel) {
      if (tc.channels_to_play.find(play_channel) != tc.channels_to_play.end()) {
        channels.push_back(AudioBufferSlice(&input_mono));
      } else {
        channels.push_back(AudioBufferSlice(&silence_mono));
      }
    }
    auto input = AudioBuffer<InputFormat>::Interleave(channels);
    FX_CHECK(input.NumFrames() == static_cast<int64_t>(total_input_buffer_len))
        << "Incorrect input length: testcode logic error";

    if constexpr (kDisplayInputBuffer) {
      if (kDisplayBuffersAtAllFrequencies || freq.display_val == kFrequencyForBufferDebugging) {
        // We construct the input buffer in pieces. If signals don't align at these seams, it causes
        // distortion. For debugging, show these "seam" locations in the input buffer we created.
        DisplayInputBufferSections(
            input,
            fxl::Concatenate({"Input buffer for ", std::to_string(freq.display_val), " Hz [",
                              std::to_string(freq.idx), "]"}),
            {.stabilization_start = input_stabilization_start,
             .analysis_start = input_analysis_start,
             .analysis_end = input_analysis_end,
             .stabilization_end = input_stabilization_end});
      }
    }

    // Save off the input file, if requested.
    if (save_fidelity_wav_files_) {
      // We shouldn't save files for ALL frequencies -- just save the files for this frequency.
      if (freq.display_val == kFrequencyForSavedWavFiles) {
        HermeticPipelineTest::WriteWavFile<InputFormat>(
            fxl::Concatenate({tc.test_name, "_", std::to_string(freq.display_val)}), "input",
            AudioBufferSlice(&input));
      }
    }

    // Set up the renderer, run it and retrieve the output.
    auto ring_buffer = GetRendererOutput(tc.input_format, total_input_buffer_len, tc.path, input,
                                         device, tc.renderer_clock_mode, tc.gain_db);

    // In case of underflows, exit NOW (don't assess this buffer or run other frequencies).
    // TODO(fxbug.dev/80003): Remove workarounds when underflow conditions are fixed.
    if (DeviceHasUnderflows(DeviceUniqueIdToString(device_id))) {
      FX_LOGS(INFO) << "Test case will exit early: underflows were detected";
      break;
    }

    // For each channel: 1. analyze output, 2) display in-progress results if configured, 3) display
    // output buffer sections if applicable, 4) exit if underflows, 5) save results for later.
    for (const auto& channel_spec : tc.channels_to_measure) {
      auto ring_buffer_chan = AudioBufferSlice(&ring_buffer).GetChannel(channel_spec.channel);

      // Analyze the results. Round our output position, so we start as close as possible to the
      // input signal start. That said, being off by one in either direction is still OK since the
      // analysis section is bookended by full ramps in/out on either side, containing identical
      // data (i.e. the analysis section's first value is repeated immediately after the section
      // ends; conversely its final value is "pre-repeated" immediately prior to section start).
      auto output_stabilization_start =
          static_cast<int64_t>(std::round(input_frame_to_output_frame(input_stabilization_start)));
      auto output_analysis_start =
          static_cast<int64_t>(std::round(input_frame_to_output_frame(input_analysis_start)));
      auto output_analysis_end = output_analysis_start + kFreqTestBufSize;
      auto output_stabilization_end =
          output_analysis_end +
          static_cast<int64_t>(std::round(input_frame_to_output_frame(final_stabilization_len)));
      auto output = AudioBufferSlice(&ring_buffer_chan, output_analysis_start, output_analysis_end);

      auto channel_is_out_of_band = (channel_spec.freq_resp_lower_limits_db[0] == -INFINITY);
      auto out_of_band = (freq.display_val < tc.low_cut_frequency ||
                          freq.display_val > low_pass_frequency || channel_is_out_of_band);

      double sinad_db, level_db = 0.0;
      AudioFreqResult freq_result;
      if (out_of_band) {
        // For out-of-band frequencies, we use the sinad array to store Out-of-Band Rejection,
        // which is measured as the sinad(all frequencies), assuming a full-scale input.
        freq_result = MeasureAudioFreqs(output, {});
        sinad_db = DoubleToDb(1.0 / freq_result.total_magn_other);

        if constexpr (kDisplayInProgressResults) {
          FX_LOGS(INFO) << "Channel " << channel_spec.channel << ": " << std::setw(5)
                        << freq.display_val << " Hz [" << std::setw(2) << freq.idx
                        << "] --       out-of-band rejection " << std::fixed << std::setprecision(4)
                        << std::setw(8) << sinad_db << " db";
        }
      } else {
        freq_result = MeasureAudioFreqs(output, {static_cast<int32_t>(freq.periods)});
        level_db = DoubleToDb(freq_result.magnitudes[freq.periods]);
        if (isinf(level_db) && level_db < 0) {
          // If an expected signal was truly absent (silence), we probably underflowed. This
          // [level_db, sinad_db] pair is meaningless, so set sinad_db to -INFINITY as well.
          sinad_db = -INFINITY;
        } else {
          sinad_db =
              DoubleToDb(freq_result.magnitudes[freq.periods] / freq_result.total_magn_other);
        }

        if constexpr (kDisplayInProgressResults) {
          FX_LOGS(INFO) << "Channel " << channel_spec.channel << ": " << std::setw(5)
                        << freq.display_val << " Hz [" << std::setw(2) << freq.idx << "] --  level "
                        << std::fixed << std::setprecision(4) << std::setw(9) << level_db
                        << " db,  SiNAD " << std::setw(8) << sinad_db << " db";
        }
      }

      if (save_fidelity_wav_files_) {
        // We shouldn't save files for the full frequency set -- just save files for this frequency.
        if (freq.display_val == kFrequencyForSavedWavFiles) {
          HermeticPipelineTest::WriteWavFile<OutputFormat>(
              fxl::Concatenate({tc.test_name, "_chan", std::to_string(channel_spec.channel), "_",
                                std::to_string(freq.display_val)}),
              "output", output);
        }
      }

      if constexpr (kDisplayOutputBufferOnFailure || kDisplayAnalysisDataOnFailure ||
                    kDisplayOutputBuffer) {
        double required_level_db =
            channel_spec.freq_resp_lower_limits_db[freq.idx] - kFidelityDbTolerance;
        double required_sinad_db =
            channel_spec.sinad_lower_limits_db[freq.idx] - kFidelityDbTolerance;

        // Display output buffer on failure, if all of 1) 'display buffer on failure' flag is set,
        // 2) frequency is not out-of-band, 3) buffer is NOT entirely silent (SiNAD > -infinity),
        // and 4) failure (Frequency Response or SiNAD) exceeds tolerance.
        bool display_output_buffer_for_failure =
            kDisplayOutputBufferOnFailure && !out_of_band && !(isinf(sinad_db) && sinad_db < 0) &&
            (level_db + kDisplayOutputBufferOnFailureFreqRespDbTolerance < required_level_db ||
             sinad_db + kDisplayOutputBufferOnFailureSinadDbTolerance < required_sinad_db);

        // Display FFT metadata on failure, if all of 1) 'display metadata on failure' flag is set,
        // 2) buffer is NOT entirely silent (SiNAD == -infinity), and 3) failure exceeds tolerance.
        bool display_analysis_data_for_failure =
            kDisplayAnalysisDataOnFailure && !isinf(sinad_db) &&
            (level_db + kDisplayAnalysisDataOnFailureDbTolerance < required_level_db ||
             sinad_db + kDisplayAnalysisDataOnFailureDbTolerance < required_sinad_db);

        // Display output buffer anyway, if 1) 'display output buffer' config flag is set, and 2) we
        // are configured to display either all frequencies or this specific frequency.
        bool display_output_buffer_for_success =
            kDisplayOutputBuffer &&
            (kDisplayBuffersAtAllFrequencies || freq.display_val == kFrequencyForBufferDebugging);

        if (display_output_buffer_for_failure || display_analysis_data_for_failure) {
          printf(
              "\nFAILURE (freq resp %f dB, should have been %f dB; sinad %f dB, should have been "
              "%f dB)",
              level_db, required_level_db, sinad_db, required_sinad_db);
        }
        if (display_analysis_data_for_failure) {
          freq_result.Display(fxl::Concatenate({"Frequency analysis results for ",
                                                std::to_string(freq.display_val), " Hz:"}),
                              kMinAnalysisMagnitudeToDisplay);
        }
        if (display_output_buffer_for_failure || display_output_buffer_for_success) {
          std::string tag =
              fxl::Concatenate({"Output buffer for ", std::to_string(freq.display_val), " Hz [",
                                std::to_string(freq.idx), "] (", std::to_string(freq.periods),
                                "-periods-in-", std::to_string(kFreqTestBufSize),
                                ", adjusted-freq ", std::to_string(adjusted_periods), "; channel ",
                                std::to_string(channel_spec.channel)});
          DisplayOutputBufferSections(ring_buffer_chan, tag,
                                      {.stabilization_start = output_stabilization_start,
                                       .analysis_start = output_analysis_start,
                                       .analysis_end = output_analysis_end,
                                       .stabilization_end = output_stabilization_end});
        }
      }

      // Retrieve the arrays of measurements for this path and channel
      auto& curr_level_db = level_results(tc.test_name, channel_spec.channel);
      auto& curr_sinad_db = sinad_results(tc.test_name, channel_spec.channel);
      if constexpr (kRetainWorstCaseResults) {
        curr_level_db[freq.idx] = std::min(curr_level_db[freq.idx], level_db);
        curr_sinad_db[freq.idx] = std::min(curr_sinad_db[freq.idx], sinad_db);
      } else {
        curr_sinad_db[freq.idx] = sinad_db;
        curr_level_db[freq.idx] = level_db;
      }
    }
  }

  if constexpr (kDisplaySummaryResults) {
    DisplaySummaryResults(tc, frequencies_to_test);
  }

  // Only check results and pass/fail if we made a complete set of measurements without underflows.
  // If there were underflows, SKIP (don't fail) so it won't look like a fidelity regression.
  // TODO(fxbug.dev/80003): Remove workarounds when underflow conditions are fixed.
  if (DeviceHasUnderflows(DeviceUniqueIdToString(device_id))) {
    GTEST_SKIP() << "Skipping threshold checks due to underflows";
    __builtin_unreachable();
  }

  VerifyResults(tc, frequencies_to_test);
}

template void HermeticFidelityTest::Run<ASF::UNSIGNED_8, ASF::FLOAT>(
    const TestCase<ASF::UNSIGNED_8, ASF::FLOAT>& tc);
template void HermeticFidelityTest::Run<ASF::SIGNED_16, ASF::FLOAT>(
    const TestCase<ASF::SIGNED_16, ASF::FLOAT>& tc);
template void HermeticFidelityTest::Run<ASF::SIGNED_24_IN_32, ASF::FLOAT>(
    const TestCase<ASF::SIGNED_24_IN_32, ASF::FLOAT>& tc);
template void HermeticFidelityTest::Run<ASF::FLOAT, ASF::FLOAT>(
    const TestCase<ASF::FLOAT, ASF::FLOAT>& tc);

}  // namespace media::audio::test
