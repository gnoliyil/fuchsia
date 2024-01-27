// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/v1/driver_output.h"

#include <lib/async/cpp/time.h>
#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <lib/zx/clock.h>

#include <algorithm>
#include <iomanip>

#include "src/media/audio/audio_core/shared/logging_flags.h"
#include "src/media/audio/audio_core/shared/reporter.h"
#include "src/media/audio/audio_core/v1/audio_driver.h"
#include "src/media/audio/audio_core/v1/idle_policy.h"

constexpr bool VERBOSE_TIMING_DEBUG = false;

namespace media::audio {

namespace {

using ::fuchsia::media::AudioSampleFormat;

// For debugging purposes, dropout checks can be enabled on an OutputProducer. The RMS signal
// strength is checked over a specified window, and if it fails below the specified value, an error
// message is emitted in the system log.
//
// Because these types of checks are intended only for specific content and conditions, they are
// only enabled for renderers that match the specific format, frame rate and channelization.
//
// The parameters for these dropout checkers should be tuned for the test content being used. The
// current values were useful while working with half-amplitude white-noise at unity gain/volume.
//
// By default these checks should be disabled.
constexpr bool kEnableDropoutChecks = false;
// Only enable the dropout checks if the ring buffer format fits these dimensions.
constexpr AudioSampleFormat kPowerCheckerSampleFormat = AudioSampleFormat::SIGNED_16;
constexpr uint32_t kPowerCheckerChannelCount = 4;
constexpr uint32_t kPowerCheckerFrameRate = 96000;
constexpr size_t kRmsWindowFrames = 512;
constexpr double kRmsLevelMin = 0.085;
constexpr int64_t kMaxPermittedSilentFrames = 2;

constexpr AudioSampleFormat kDefaultAudioFmt = AudioSampleFormat::SIGNED_24_IN_32;
constexpr zx::duration kDefaultMaxRetentionNsec = zx::msec(60);
constexpr zx::duration kDefaultRetentionGapNsec = zx::msec(10);
constexpr zx::duration kUnderflowCooldown = zx::msec(1000);

std::atomic<zx_txid_t> TXID_GEN(1);
thread_local zx_txid_t TXID = TXID_GEN.fetch_add(1);
// WAV file location: FilePathName+final_mix_instance_num_+FileExtension
constexpr const char* kDefaultWavFilePathName = "/tmp/final_mix_";
constexpr const char* kWavFileExtension = ".wav";

}  // namespace

// Consts used if kEnableFinalMixWavWriter is set:
//
// This atomic is only used when the final-mix wave-writer is enabled --
// specifically to generate unique ids for each final-mix WAV file.
std::atomic<uint32_t> DriverOutput::final_mix_instance_num_(0u);

DriverOutput::DriverOutput(const std::string& name, const DeviceConfig& config,
                           const MixProfileConfig& mix_profile_config,
                           ThreadingModel* threading_model, DeviceRegistry* registry,
                           fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig> channel,
                           LinkMatrix* link_matrix,
                           std::shared_ptr<AudioCoreClockFactory> clock_factory,
                           EffectsLoaderV2* effects_loader_v2)
    : AudioOutput(name, config, threading_model, registry, link_matrix, clock_factory,
                  effects_loader_v2, std::make_unique<AudioDriver>(this)),
      low_water_duration_(mix_profile_config.period),
      high_water_duration_(low_water_duration_ + mix_profile_config.period),
      initial_stream_channel_(channel.TakeChannel()) {}

DriverOutput::~DriverOutput() { wav_writer_.Close(); }

const PipelineConfig* DriverOutput::pipeline_config() const {
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
  return driver()
             ? &config().output_device_profile(driver()->persistent_unique_id()).pipeline_config()
             : &config().default_output_device_profile().pipeline_config();
}

int64_t DriverOutput::RefTimeToSafeWriteFrame(zx::time ref_time) const {
  auto& time_to_frac_frame = driver_ref_time_to_frac_safe_read_or_write_frame();
  return Fixed::FromRaw(time_to_frac_frame.Apply(ref_time.get())).Floor();
}

zx::time DriverOutput::SafeWriteFrameToRefTime(int64_t frame) const {
  auto& time_to_frac_frame = driver_ref_time_to_frac_safe_read_or_write_frame();
  return zx::time(time_to_frac_frame.ApplyInverse(Fixed(frame).raw_value()));
}

TimelineRate DriverOutput::FramesPerRefTick() const {
  auto frac_frame_per_tick = driver_ref_time_to_frac_safe_read_or_write_frame().rate();
  return TimelineRate::Product(frac_frame_per_tick, TimelineRate(1, Fixed(1).raw_value()));
}

zx_status_t DriverOutput::Init() {
  TRACE_DURATION("audio", "DriverOutput::Init");
  FX_DCHECK(state_ == State::Uninitialized);

  zx_status_t res = AudioOutput::Init();
  if (res != ZX_OK) {
    return res;
  }

  res = driver()->Init(std::move(initial_stream_channel_));
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to initialize driver object";
    return res;
  }

  state_ = State::FormatsUnknown;
  return res;
}

void DriverOutput::OnWakeup() {
  TRACE_DURATION("audio", "DriverOutput::OnWakeup");
  // If we are not in the FormatsUnknown state, then we have already started the
  // state machine.  There is (currently) nothing else to do here.
  FX_DCHECK(state_ != State::Uninitialized);
  if (state_ != State::FormatsUnknown) {
    return;
  }

  // Kick off the process of driver configuration by requesting the basic driver
  // info, which will include the modes which the driver supports.
  driver()->GetDriverInfo();
  state_ = State::FetchingFormats;
}

std::optional<AudioOutput::FrameSpan> DriverOutput::StartMixJob(zx::time ref_time) {
  TRACE_DURATION("audio", "DriverOutput::StartMixJob");
  if (state_ != State::Started) {
    FX_LOGS(ERROR) << "Bad state during StartMixJob " << static_cast<uint32_t>(state_);
    state_ = State::Shutdown;
    ShutdownSelf();
    return std::nullopt;
  }

  // TODO(mpuryear): Depending on policy, use send appropriate commands to the
  // driver to control gain as well.  Some policy settings which might be useful
  // include...
  //
  // ++ Never use HW gain, even if it supports it.
  // ++ Always use HW gain when present, regardless of its limitations.
  // ++ Use HW gain when present, but only if it reaches a minimum bar of
  //    functionality.
  // ++ Implement a hybrid of HW/SW gain.  IOW - Get as close as possible to our
  //    target using HW, and then get the rest of the way there using SW
  //    scaling.  This approach may end up being unreasonably tricky as we may
  //    not be able to synchronize the HW and SW changes in gain well enough to
  //    avoid strange situations where the jumps in one direction (because of
  //    the SW component), and then in the other (as the HW gain command takes
  //    effect).
  //
  bool output_muted = true;
  const auto& settings = device_settings();
  if (settings != nullptr) {
    auto [flags, cur_gain_state] = settings->SnapshotGainState();
    output_muted = cur_gain_state.muted;
  }

  FX_DCHECK(driver_writable_ring_buffer() != nullptr);
  const auto& output_frames_per_reference_tick = FramesPerRefTick();
  const auto& rb = *driver_writable_ring_buffer();
  uint32_t internal_delay_frames = driver()->internal_delay_frames();

  // output_frames_consumed is the number of frames that the audio output
  // device's DMA *may* have read so far.  output_frames_transmitted is the
  // slightly-smaller number of frames that have *must* have been transmitted
  // over the interconnect so far.  Note, this is not technically the number of
  // frames which have made sound so far.  Once a frame has left the
  // interconnect, it still has the device's external_delay before it will
  // finally hit the speaker.
  int64_t output_frames_consumed = RefTimeToSafeWriteFrame(ref_time);
  int64_t output_frames_transmitted = output_frames_consumed - internal_delay_frames;

  auto mono_time = reference_clock()->MonotonicTimeFromReferenceTime(ref_time);

  if (output_frames_consumed >= frames_sent_) {
    if (!underflow_start_time_mono_.get()) {
      // If this was the first time we missed our limit, log a message, mark the start time of the
      // underflow event, and fill our entire ring buffer with silence.
      int64_t output_underflow_frames = output_frames_consumed - frames_sent_;
      int64_t low_water_frames_underflow = output_underflow_frames + low_water_frames_;

      zx::duration output_underflow_duration =
          zx::nsec(output_frames_per_reference_tick.Inverse().Scale(output_underflow_frames));
      FX_DCHECK(output_underflow_duration.get() >= 0);

      zx::duration output_variance_from_expected_wakeup =
          zx::nsec(output_frames_per_reference_tick.Inverse().Scale(low_water_frames_underflow));

      TRACE_INSTANT("audio", "DriverOutput::UNDERFLOW", TRACE_SCOPE_THREAD);
      TRACE_ALERT("audio", "audiounderflow");
      FX_LOGS(ERROR) << "OUTPUT UNDERFLOW: Missed mix target by (worst-case, expected) = ("
                     << std::setprecision(4)
                     << static_cast<double>(output_underflow_duration.to_nsecs()) / ZX_MSEC(1)
                     << ", " << output_variance_from_expected_wakeup.to_msecs()
                     << ") ms. Cooling down for " << kUnderflowCooldown.to_msecs()
                     << " milliseconds.";

      reporter().DeviceUnderflow(mono_time, mono_time + output_underflow_duration);

      underflow_start_time_mono_ = mono_time;
      output_producer_->FillWithSilence(rb.virt(), rb.frames());
      zx_cache_flush(rb.virt(), rb.size(), ZX_CACHE_FLUSH_DATA);

      wav_writer_.Close();
    }

    // Regardless of whether this was the first or a subsequent underflow,
    // update the cooldown deadline (the time at which we will start producing
    // frames again, provided we don't underflow again)
    underflow_cooldown_deadline_mono_ = zx::deadline_after(kUnderflowCooldown);
  }

  // We want to fill up to be HighWaterNsec ahead of the current safe write
  // pointer position.  Add HighWaterNsec to our concept of "now" and run it
  // through our transformation to figure out what frame number this.
  int64_t fill_target = RefTimeToSafeWriteFrame(ref_time + high_water_duration_);

  // Are we in the middle of an underflow cooldown? If so, check whether we have recovered yet.
  if (underflow_start_time_mono_.get()) {
    if (mono_time < underflow_cooldown_deadline_mono_) {
      // Looks like we have not recovered yet.  Pretend to have produced the
      // frames we were going to produce and schedule the next wakeup time.
      frames_sent_ = fill_target;
      ScheduleNextLowWaterWakeup();
      return std::nullopt;
    } else {
      // Looks like we recovered.  Log and go back to mixing.
      FX_LOGS(WARNING) << "OUTPUT UNDERFLOW: Recovered after "
                       << (mono_time - underflow_start_time_mono_).to_msecs() << " ms.";
      underflow_start_time_mono_ = zx::time(0);
      underflow_cooldown_deadline_mono_ = zx::time(0);
    }
  }

  // Compute the number of frames which are currently "in flight".  We define
  // this as the number of frames that we have rendererd into the ring buffer
  // but which have may have not been transmitted over the output's interconnect
  // yet.  The distance between frames_sent_ and output_frames_transmitted
  // should give us this number.
  int64_t frames_in_flight = frames_sent_ - output_frames_transmitted;
  FX_DCHECK((frames_in_flight >= 0) && (frames_in_flight <= rb.frames()));
  FX_DCHECK(frames_sent_ <= fill_target);
  int64_t desired_frames = fill_target - frames_sent_;

  // If we woke up too early to have any work to do, just get out now.
  if (desired_frames == 0) {
    return std::nullopt;
  }

  uint32_t rb_space = rb.frames() - static_cast<uint32_t>(frames_in_flight);
  if (desired_frames > rb.frames()) {
    FX_LOGS(ERROR) << "OUTPUT OVERFLOW: want to produce " << desired_frames
                   << " but the ring buffer is only " << rb.frames() << " frames long.";
    return std::nullopt;
  }

  uint32_t frames_to_mix = static_cast<uint32_t>(std::min<int64_t>(rb_space, desired_frames));

  return AudioOutput::FrameSpan{
      .start = frames_sent_,
      .length = frames_to_mix,
      .is_mute = output_muted,
  };
}

void DriverOutput::WriteMixOutput(int64_t start, int64_t frames_left, const float* buffer) {
  TRACE_DURATION("audio", "DriverOutput::WriteToRing");
  const auto& rb = driver_writable_ring_buffer();
  FX_DCHECK(rb != nullptr);

  int64_t offset = 0;
  while (frames_left > 0) {
    int64_t wr_ptr = (start + offset) % rb->frames();
    int64_t contig_space = rb->frames() - wr_ptr;
    int64_t to_send = frames_left;
    if (to_send > contig_space) {
      to_send = contig_space;
    }
    void* dest_buf = rb->virt() + (rb->format().bytes_per_frame() * wr_ptr);

    if (!buffer) {
      output_producer_->FillWithSilence(dest_buf, to_send);
    } else {
      auto job_buf_offset = offset * output_producer_->channels();
      output_producer_->ProduceOutput(buffer + job_buf_offset, dest_buf, to_send);

      if constexpr (kEnableDropoutChecks) {
        if (power_checker_) {
          power_checker_->Check(buffer + job_buf_offset, frames_sent_ + offset, to_send, true);
        }
        if (silence_checker_) {
          silence_checker_->Check(buffer + job_buf_offset, frames_sent_ + offset, to_send, true);
        }
      }
    }
    size_t dest_buf_len = to_send * output_producer_->bytes_per_frame();
    wav_writer_.Write(dest_buf, dest_buf_len);
    wav_writer_.UpdateHeader();
    zx_cache_flush(dest_buf, dest_buf_len, ZX_CACHE_FLUSH_DATA);

    frames_left -= to_send;
    offset += to_send;
  }
}

void DriverOutput::FinishMixJob(const AudioOutput::FrameSpan& span) {
  TRACE_DURATION("audio", "DriverOutput::FinishMixJob", "start", span.start, "length", span.length,
                 "is_mute", span.is_mute);

  frames_sent_ = span.start + span.length;

  if (VERBOSE_TIMING_DEBUG) {
    auto now = async::Now(mix_domain().dispatcher());
    int64_t output_frames_consumed = RefTimeToSafeWriteFrame(now);
    int64_t playback_lead_end = frames_sent_ - output_frames_consumed;
    int64_t playback_lead_start = playback_lead_end - span.length;

    FX_LOGS(INFO) << "PLead [" << std::setw(4) << playback_lead_start << ", " << std::setw(4)
                  << playback_lead_end << "]";
  }

  ScheduleNextLowWaterWakeup();
}

void DriverOutput::ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info,
                                   fuchsia::media::AudioGainValidFlags set_flags) {
  TRACE_DURATION("audio", "DriverOutput::ApplyGainLimits");
  // See the comment at the start of StartMixJob.  The actual limits we set here
  // are going to eventually depend on what our HW gain control capabilities
  // are, and how we choose to apply them (based on policy)
  FX_DCHECK(in_out_info != nullptr);

  // We do not currently allow more than unity gain for audio outputs.
  if (in_out_info->gain_db > 0.0) {
    in_out_info->gain_db = 0;
  }

  // Audio outputs should never support AGC
  in_out_info->flags &= ~(fuchsia::media::AudioGainInfoFlags::AGC_ENABLED);
}

void DriverOutput::ScheduleNextLowWaterWakeup() {
  TRACE_DURATION("audio", "DriverOutput::ScheduleNextLowWaterWakeup");

  // After filling up, we are "high water frames" ahead of the safe write
  // pointer. Compute when this will have been reduced to low_water_frames_.
  // This is when we want to wake up and repeat the mixing cycle.
  //
  // frames_sent_ is the total number of frames we have ever synthesized since
  // starting.  Subtracting low_water_frames_ from this will give us the
  // absolute frame number at which we are only low_water_frames_ ahead of the
  // safe write pointer.  Running this backwards through the safe write
  // pointer's reference clock <-> frame number function will tell us when it
  // will be time to wake up.
  int64_t low_water_frame_number = frames_sent_ - low_water_frames_;
  auto low_water_ref_time = SafeWriteFrameToRefTime(low_water_frame_number);
  auto low_water_mono_time = reference_clock()->MonotonicTimeFromReferenceTime(low_water_ref_time);

  SetNextSchedTimeMono(low_water_mono_time);
}

void DriverOutput::OnDriverInfoFetched() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverInfoFetched");
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS {
    state_ = State::Shutdown;
    ShutdownSelf();
  });

  if (state_ != State::FetchingFormats) {
    FX_LOGS(ERROR) << "Unexpected GetFormatsComplete while in state "
                   << static_cast<uint32_t>(state_);
    return;
  }

  zx_status_t res;

  DeviceConfig::OutputDeviceProfile profile =
      config().output_device_profile(driver()->persistent_unique_id());

  if (profile.driver_gain_db()) {
    float driver_gain_db = *profile.driver_gain_db();
    AudioDeviceSettings::GainState gain_state = {.gain_db = driver_gain_db, .muted = false};
    if constexpr (kLogSetDeviceGainMuteActions) {
      FX_LOGS(INFO) << "Calling driver->SetGain(" << driver_gain_db << ", unmuted)";
    }
    driver()->SetGain(gain_state, AUDIO_SGF_GAIN_VALID | AUDIO_SGF_MUTE_VALID);
  }

  PipelineConfig pipeline_config = profile.pipeline_config();
  const Format pipeline_format = pipeline_config.OutputFormat(effects_loader_v2());

  uint32_t pref_fps = static_cast<uint32_t>(pipeline_format.frames_per_second());
  uint32_t pref_chan = static_cast<uint32_t>(pipeline_format.channels());
  AudioSampleFormat pref_fmt = kDefaultAudioFmt;
  zx::duration min_rb_duration =
      high_water_duration_ + kDefaultMaxRetentionNsec + kDefaultRetentionGapNsec;

  res = driver()->SelectBestFormat(&pref_fps, &pref_chan, &pref_fmt);

  if (res != ZX_OK) {
    FX_LOGS(ERROR) << "Output: cannot match a driver format to this request: " << pref_fps
                   << " Hz, " << pref_chan << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt);
    return;
  }

  auto format_result = Format::Create(fuchsia::media::AudioStreamType{
      .sample_format = pref_fmt,
      .channels = static_cast<uint32_t>(pref_chan),
      .frames_per_second = pref_fps,
  });
  if (format_result.is_error()) {
    FX_LOGS(ERROR) << "Driver format is invalid";
    return;
  }
  auto& format = format_result.value();
  int32_t frame_rate = static_cast<int32_t>(pref_fps);
  int16_t num_chans = static_cast<int16_t>(pref_chan);

  // Update our pipeline to produce audio in the compatible format.
  if (pipeline_format.frames_per_second() != frame_rate) {
    FX_LOGS(WARNING) << "Hardware does not support the requested rate of "
                     << pipeline_format.frames_per_second() << " fps; hardware will run at "
                     << frame_rate << " fps";
    pipeline_config.mutable_root().output_rate = frame_rate;
  }
  if (pipeline_format.channels() != num_chans) {
    FX_LOGS(WARNING) << "Hardware does not support the requested channelization of "
                     << pipeline_format.channels() << " channels; hardware will run at "
                     << num_chans << " channels";
    pipeline_config.mutable_root().output_channels = num_chans;
    // Some effects may perform rechannelization. If the hardware does not support the
    // channelization with rechannelization effects we clear all effects on the final stage. This
    // is a compromise in being robust and gracefully handling misconfiguration.
    for (const auto& effect : pipeline_config.root().effects_v1) {
      if (effect.output_channels && effect.output_channels != num_chans) {
        FX_LOGS(ERROR) << "Removing effects on the root stage due to unsupported channelization";
        pipeline_config.mutable_root().effects_v1.clear();
        break;
      }
    }
    pipeline_config.mutable_root().effects_v2 = std::nullopt;
  }

  // Sanity check: ensure the new format matches.
  if (pipeline_format.frames_per_second() != frame_rate ||
      pipeline_format.channels() != num_chans) {
    const Format new_pipeline_format = pipeline_config.OutputFormat(effects_loader_v2());
    FX_CHECK(new_pipeline_format.frames_per_second() == frame_rate);
    FX_CHECK(new_pipeline_format.channels() == num_chans);
  }

  // Update the AudioDevice |config_| with the updated |pipeline_config|.
  // Only |frames_per_second| and |channels| were potentially updated in |pipeline_config|, so it is
  // not necessary to UpdateDeviceProfile() to consequently reconstruct the OutputPipeline.
  auto updated_profile = DeviceConfig::OutputDeviceProfile(
      profile.eligible_for_loopback(), profile.supported_usages(), profile.volume_curve(),
      profile.independent_volume_control(), pipeline_config, profile.driver_gain_db(),
      profile.software_gain_db());
  DeviceConfig updated_config = config();
  updated_config.SetOutputDeviceProfile(driver()->persistent_unique_id(), updated_profile);
  set_config(updated_config);

  // Select our output producer
  output_producer_ = OutputProducer::Select(format.stream_type());
  if (!output_producer_) {
    FX_LOGS(ERROR) << "Output: OutputProducer cannot support this request: " << frame_rate
                   << " Hz, " << num_chans << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt);
    return;
  }

  // Start the process of configuring our driver
  res = driver()->Configure(format, min_rb_duration);
  if (res != ZX_OK) {
    FX_LOGS(ERROR) << "Output: failed to configure driver for: " << frame_rate << " Hz, "
                   << num_chans << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt) << " (res " << std::dec << res << ")";
    return;
  }

  if constexpr (kEnableFinalMixWavWriter) {
    std::string file_name_ = kDefaultWavFilePathName;
    uint32_t instance_count = final_mix_instance_num_.fetch_add(1);
    file_name_ += (std::to_string(instance_count) + kWavFileExtension);
    wav_writer_.Initialize(file_name_.c_str(), pref_fmt, num_chans, frame_rate,
                           format.bytes_per_frame() * 8 / num_chans);
  }

  if constexpr (kEnableDropoutChecks) {
    if (pref_fmt == kPowerCheckerSampleFormat && frame_rate == kPowerCheckerFrameRate &&
        num_chans == kPowerCheckerChannelCount) {
      std::ostringstream dropout_log_tag;
      dropout_log_tag << "DriverOutput(" << this << ")";
      power_checker_ = std::make_unique<PowerChecker>(kRmsWindowFrames, num_chans, kRmsLevelMin,
                                                      dropout_log_tag.str());
      silence_checker_ = std::make_unique<SilenceChecker>(kMaxPermittedSilentFrames, num_chans,
                                                          dropout_log_tag.str());
    }
  }

  // Success; now wait until configuration completes.
  state_ = State::Configuring;
  cleanup.cancel();
}

void DriverOutput::OnDriverConfigComplete() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverConfigComplete");
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS {
    state_ = State::Shutdown;
    ShutdownSelf();
  });

  if (state_ != State::Configuring) {
    FX_LOGS(ERROR) << "Unexpected ConfigComplete while in state " << static_cast<uint32_t>(state_);
    return;
  }

  // Driver is configured, we have all the needed info to compute the presentation
  // delay for this output.
  SetPresentationDelay(driver()->external_delay() + driver()->internal_delay() +
                       high_water_duration_);

  // Fill our brand new ring buffer with silence
  FX_DCHECK(driver_writable_ring_buffer() != nullptr);
  const auto& rb = *driver_writable_ring_buffer();
  FX_DCHECK(output_producer_ != nullptr);
  FX_DCHECK(rb.virt() != nullptr);
  output_producer_->FillWithSilence(rb.virt(), rb.frames());

  // Start the ring buffer running
  //
  // TODO(fxbug.dev/13292) : Don't actually start things up here. We should start only when we have
  // clients with work to do, and we should stop when we have no work to do.
  zx_status_t res = driver()->Start();
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to start ring buffer";
    return;
  }

  // Start monitoring plug state.
  res = driver()->SetPlugDetectEnabled(true);
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to enable plug detection";
    return;
  }

  // Now that we are configured, retrieve the driver's channel-to-frequency-range mapping, to
  // determine whether to participate in idle-power-down of the audible/ultrasonic ranges.
  // These members must be set before ActivateSelf() is called.
  channel_config_ = driver()->channel_config();
  current_active_channel_mask_ = (1 << channel_config_.size()) - 1;
  supports_audible_ = ChannelAttributes::IncludesAudible(channel_config_);
  supports_ultrasonic_ = ChannelAttributes::IncludesUltrasonic(channel_config_);
  audible_enabled_ = supports_audible_;
  ultrasonic_enabled_ = supports_ultrasonic_;

  // Success
  state_ = State::Starting;
  cleanup.cancel();
}

void DriverOutput::OnDriverStartComplete() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverStartComplete");
  if (state_ != State::Starting) {
    FX_LOGS(ERROR) << "Unexpected StartComplete while in state " << static_cast<uint32_t>(state_);
    return;
  }

  reporter().SetDriverInfo(driver()->info_for_reporter());

  // Set up the mix task in the AudioOutput.
  //
  // TODO(fxbug.dev/39886): an intermediate buffer probably need not be as large as the entire ring
  // buffer. Consider limiting this to be only slightly larger than a nominal mix job.
  auto format = driver()->GetFormat();
  FX_DCHECK(format);
  SetupMixTask(config().output_device_profile(driver()->persistent_unique_id()),
               driver_writable_ring_buffer()->frames(),
               driver_ref_time_to_frac_presentation_frame());

  // Tell AudioDeviceManager we are ready to be an active audio device.
  ActivateSelf();

  // Compute low_water_frames_.  low_water_frames_ is minimum the number of
  // frames ahead of the safe write position we ever want to be.  When we hit
  // the point where we are only this number of frames ahead of the safe write
  // position, we need to wake up and fill up to our high water mark.
  const TimelineRate rate = FramesPerRefTick();
  low_water_frames_ = rate.Scale(low_water_duration_.get());

  // We started with a buffer full of silence.  Set up our bookkeeping so we
  // consider ourselves to have generated and sent up to our low-water mark's
  // worth of silence already beyond the where the current safe write frame
  // in the ring buffer is.
  //
  // We read from async::Now and convert to reference time to simplify unit
  // tests that mock out time using async::Now.
  //
  // TODO(57377): Keep reference clocks in sync with mono time under test.
  auto mono_time = async::Now(mix_domain().dispatcher());
  auto ref_time = reference_clock()->ReferenceTimeFromMonotonicTime(mono_time);
  int64_t output_frames_consumed = RefTimeToSafeWriteFrame(ref_time);
  frames_sent_ = output_frames_consumed + low_water_frames_;

  if (VERBOSE_TIMING_DEBUG) {
    auto fd_frames = driver()->internal_delay_frames();
    FX_LOGS(INFO) << "Audio output: FIFO depth (" << fd_frames << " frames " << std::fixed
                  << std::setprecision(3) << rate.Inverse().Scale(fd_frames) / 1000000.0
                  << " mSec) Low Water (" << frames_sent_ << " frames " << std::fixed
                  << std::setprecision(3) << rate.Inverse().Scale(frames_sent_) / 1000000.0
                  << " mSec)";
  }

  reporter().StartSession(zx::clock::get_monotonic());
  state_ = State::Started;

  // Once we are Started, begin the device-startup idle countdown
  if (IdlePolicy::startup_idle_countdown_duration().has_value() &&
      !IdlePolicy::kDisableIdlePolicy) {
    StartCountdownToDisableAudible(*IdlePolicy::startup_idle_countdown_duration());
    StartCountdownToDisableUltrasonic(*IdlePolicy::startup_idle_countdown_duration());
  } else {
    if constexpr (kLogIdleTimers) {
      FX_LOGS(INFO) << __FUNCTION__ << " not starting idle countdowns (idle policy disabled)";
    }
  }

  Process();
}

// Enable/disable device channels that are in the audible range. Used for power conservation.
// Called from FIDL thread, but must post to device Mix thread to cancel and UpdateActiveChannels
zx_status_t DriverOutput::EnableAudible() {
  zx_status_t status = ZX_OK;
  if (!supports_audible_) {
    status = ZX_ERR_INTERNAL;
  } else {
    mix_domain().PostTask([this]() {
      OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());

      CancelCountdownAudible();
      audible_enabled_ = supports_audible_;
      UpdateActiveChannels();

      if constexpr (kLogIdleTimers) {
        FX_LOGS(INFO) << "mix_domain task from DriverOutput::EnableAudible completed";
      }
    });
  }

  if constexpr (kLogSetActiveChannelsSupport) {
    if (status == ZX_OK) {
      FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __func__ << " returned " << status;
    } else {
      FX_LOGS(WARNING) << "DriverOutput(" << this << ")::" << __func__ << " returned " << status;
    }
  }
  return status;
}

// Enable/disable device channels that are in the ultrasonic range. Used for power conservation.
// Called from FIDL thread, but must post to device Mix thread to cancel and UpdateActiveChannels
zx_status_t DriverOutput::EnableUltrasonic() {
  zx_status_t status = ZX_OK;
  if (!supports_ultrasonic_) {
    status = ZX_ERR_INTERNAL;
  } else {
    mix_domain().PostTask([this]() {
      OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());

      CancelCountdownUltrasonic();
      ultrasonic_enabled_ = supports_ultrasonic_;
      UpdateActiveChannels();

      if constexpr (kLogIdleTimers) {
        FX_LOGS(INFO) << "mix_domain task from DriverOutput::EnableUltrasonic completed";
      }
    });
  }

  if constexpr (kLogSetActiveChannelsSupport) {
    if (status == ZX_OK) {
      FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __func__ << " returned " << status;
    } else {
      FX_LOGS(WARNING) << "DriverOutput(" << this << ")::" << __func__ << " returned " << status;
    }
  }
  return status;
}

// Called from the FIDL thread, but posts the countdown task to the device's mix thread
zx_status_t DriverOutput::StartCountdownToDisableAudible(zx::duration countdown) {
  zx_status_t status = ZX_OK;
  if (countdown < zx::sec(0)) {
    status = ZX_ERR_INVALID_ARGS;
  } else if (!supports_audible_) {
    status = ZX_ERR_INTERNAL;
  } else {
    // |ZX_OK| (successfully inserted, 0); |ZX_ERR_BAD_STATE| (dispatcher shutting down, -20)
    // |ZX_ERR_NOT_SUPPORTED| (by dispatcher, -2); |ZX_ERR_ALREADY_EXISTS| (already pending, -25)
    status = audible_countdown_.PostDelayed(mix_domain().dispatcher(), countdown);
  }

  if constexpr (kLogIdleTimers) {
    if (status == ZX_OK) {
      FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __func__ << "("
                    << countdown.get() / ZX_MSEC(1) << " ms)";
    } else {
      FX_PLOGS(WARNING, status) << "DriverOutput(" << this << ")::" << __func__ << "("
                                << countdown.get() / ZX_MSEC(1) << " ms)";
    }
  }
  return status;
}

// Called from the FIDL thread, but posts the countdown task to the device's mix thread
zx_status_t DriverOutput::StartCountdownToDisableUltrasonic(zx::duration countdown) {
  zx_status_t status = ZX_OK;
  if (!supports_ultrasonic_) {
    status = ZX_ERR_INTERNAL;
  } else if (countdown < zx::sec(0)) {
    status = ZX_ERR_INVALID_ARGS;
  } else {
    status = ultrasonic_countdown_.PostDelayed(mix_domain().dispatcher(), countdown);
  }

  if constexpr (kLogIdleTimers) {
    if (status == ZX_OK) {
      FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __func__ << "("
                    << countdown.get() / ZX_MSEC(1) << " ms)";
    } else {
      FX_PLOGS(WARNING, status) << "DriverOutput(" << this << ")::" << __func__ << "("
                                << countdown.get() / ZX_MSEC(1) << " ms)";
    }
  }
  return status;
}

// Must be called from the same thread that posts the task (device's Mix thread)
zx_status_t DriverOutput::CancelCountdownAudible() {
  if constexpr (kLogIdleTimers) {
    FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __FUNCTION__;
  }

  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());

  if (!supports_audible_) {
    return ZX_ERR_INTERNAL;
  }

  // Returns ZX_OK(canceled) ZX_ERR_NOT_FOUND(not pending) or ZX_ERR_NOT_SUPPORTED(bad dispatcher)
  // but for all of these, return ZX_OK since the caller shouldn't do anything to fix.
  audible_countdown_.Cancel();

  return ZX_OK;
}

// Must be called from the same thread that posts the task (device's Mix thread)
zx_status_t DriverOutput::CancelCountdownUltrasonic() {
  if constexpr (kLogIdleTimers) {
    FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __FUNCTION__;
  }

  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());

  if (!supports_ultrasonic_) {
    return ZX_ERR_INTERNAL;
  }

  // Returns ZX_OK(canceled) ZX_ERR_NOT_FOUND(not pending) or ZX_ERR_NOT_SUPPORTED(bad dispatcher)
  // but for all of these, return ZX_OK since the caller shouldn't do anything to fix.
  ultrasonic_countdown_.Cancel();

  return ZX_OK;
}

// Must be called from the same thread that posts the task (device's Mix thread)
void DriverOutput::CountdownExpiredAudible() {
  if constexpr (kLogIdleTimers) {
    FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __FUNCTION__;
  }

  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());

  audible_enabled_ = false;
  UpdateActiveChannels();
}

// Called from this device's Mix thread
void DriverOutput::CountdownExpiredUltrasonic() {
  if constexpr (kLogIdleTimers) {
    FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __FUNCTION__;
  }

  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());

  ultrasonic_enabled_ = false;
  UpdateActiveChannels();
}

// Called from this device's Mix thread
// Returns the bitmask of channels that we set, purely for debug logging reasons
uint64_t DriverOutput::UpdateActiveChannels() {
  if (!supports_set_active_channels_) {
    return current_active_channel_mask_;
  }

  uint64_t active_channel_mask = 0ull;
  bool ultrasonic_needed = ultrasonic_enabled_;
  for (size_t channel_index = 0; channel_index < channel_config_.size(); ++channel_index) {
    if ((audible_enabled_ && channel_config_[channel_index].IncludesAudible()) ||
        (ultrasonic_needed && channel_config_[channel_index].IncludesUltrasonic())) {
      active_channel_mask |= 1 << channel_index;

      if (!IdlePolicy::use_all_ultrasonic_channels()) {
        // Configure only the first channel that satisfies ultrasonic; clear the others.
        if (channel_config_[channel_index].IncludesUltrasonic()) {
          ultrasonic_needed = false;
        }
      }
    }
  }

  if (current_active_channel_mask_ != active_channel_mask) {
    if constexpr (kLogSetActiveChannelsSupport) {
      FX_LOGS(INFO) << "DriverOutput(" << this << ")::" << __func__
                    << " calling driver->SetActiveChannels(0x" << std::hex << active_channel_mask
                    << ")";
    }

    zx_status_t status = driver()->SetActiveChannels(active_channel_mask);

    if (status != ZX_OK) {
      FX_PLOGS(WARNING, status) << this << ": driver->SetActiveChannels(0x" << std::hex
                                << active_channel_mask << ")";
      // We only lightly complain on error (like ZX_ERR_NOT_SUPPORTED), but we won't call again
      supports_set_active_channels_ = false;
    } else if constexpr (kLogSetActiveChannelsSupport) {
      FX_LOGS(INFO) << this << ": driver->SetActiveChannels(0x" << std::hex << active_channel_mask
                    << ")";
    }

    current_active_channel_mask_ = active_channel_mask;
  }

  return active_channel_mask;
}

}  // namespace media::audio
