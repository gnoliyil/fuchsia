// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_AML_DSP_AML_G12_TDM_DSP_AUDIO_STREAM_H_
#define SRC_MEDIA_AUDIO_DRIVERS_AML_DSP_AML_G12_TDM_DSP_AUDIO_STREAM_H_

#include <fidl/fuchsia.hardware.dsp/cpp/wire.h>
#include <fidl/fuchsia.hardware.gpio/cpp/wire.h>
#include <fidl/fuchsia.hardware.mailbox/cpp/wire.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/io-buffer.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/fzl/pinned-vmo.h>
#include <lib/simple-audio-stream/simple-audio-stream.h>
#include <lib/simple-codec/simple-codec-client.h>
#include <lib/zx/bti.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>

#include <memory>

#include <audio-proto/audio-proto.h>
#include <ddktl/device-internal.h>
#include <ddktl/device.h>
#include <ddktl/metadata/audio.h>
#include <fbl/mutex.h>
#include <soc/aml-common/aml-tdm-audio.h>

#include "aml-tdm-config-device.h"
#include "src/media/audio/drivers/lib/aml-dsp/dsp.h"

namespace audio {
namespace aml_g12 {

class AmlG12TdmDspStream : public SimpleAudioStream {
 public:
  AmlG12TdmDspStream(zx_device_t* parent, bool is_input, ddk::PDevFidl pdev,
                     fidl::ClientEnd<fuchsia_hardware_gpio::Gpio> enable_gpio);

 protected:
  zx_status_t Init() __TA_REQUIRES(domain_token()) override;
  zx_status_t ChangeFormat(const audio_proto::StreamSetFmtReq& req)
      __TA_REQUIRES(domain_token()) override;  // virtual for unit testing.
  zx_status_t GetBuffer(const audio_proto::RingBufGetBufferReq& req, uint32_t* out_num_rb_frames,
                        zx::vmo* out_buffer) __TA_REQUIRES(domain_token()) override;
  zx_status_t Start(uint64_t* out_start_time) __TA_REQUIRES(domain_token()) override;
  zx_status_t Stop() __TA_REQUIRES(domain_token()) override;
  zx_status_t SetGain(const audio_proto::SetGainReq& req) __TA_REQUIRES(domain_token()) override;
  zx_status_t ChangeActiveChannels(uint64_t mask, zx_time_t* set_time_out)
      __TA_REQUIRES(domain_token()) override;
  void ShutdownHook() __TA_REQUIRES(domain_token()) override;

  // Protected for unit test.
  zx_status_t InitCodec();
  zx_status_t InitPDev();
  void InitDaiFormats();
  zx_status_t InitCodecsGain() __TA_REQUIRES(domain_token());

  // SimpleCodecClients stored as unique pointers because they are not movable.
  std::vector<std::unique_ptr<SimpleCodecClient>> codecs_;
  std::unique_ptr<AmlTdmConfigDevice> aml_audio_;
  std::unique_ptr<AmlMailboxDevice> audio_mailbox_;
  std::unique_ptr<AmlDspDevice> audio_dsp_;
  metadata::AmlConfig metadata_ = {};

 private:
  friend class fbl::RefPtr<AmlG12TdmDspStream>;

  static constexpr uint8_t kFifoDepth = 0x20;

  zx_status_t AddFormats() __TA_REQUIRES(domain_token());
  zx_status_t InitBuffer(size_t size);
  void ProcessRingNotification();
  void RingNotificationReport();
  void UpdateCodecsGainState(GainState state) __TA_REQUIRES(domain_token());
  void UpdateCodecsGainStateFromCurrent() __TA_REQUIRES(domain_token());
  zx_status_t StopAllCodecs();
  zx_status_t StartAllEnabledCodecs();
  zx_status_t UpdateHardwareSettings();
  virtual bool AllowNonContiguousRingBuffer() { return false; }
  zx_status_t StartCodecIfEnabled(size_t index);
  int Thread();
  int MailboxBind();
  int DspBind();

  uint32_t us_per_notification_ = 0;
  DaiFormat dai_formats_[metadata::kMaxNumberOfCodecs] = {};
  uint32_t frame_rate_ = 0;
  int64_t codecs_turn_on_delay_nsec_ = 0;
  int64_t codecs_turn_off_delay_nsec_ = 0;
  bool hardware_configured_ = false;

  async::TaskClosureMethod<AmlG12TdmDspStream,
                           &AmlG12TdmDspStream::ProcessRingNotification> notify_timer_
      __TA_GUARDED(domain_token()){this};
  // Inform DSP FW of ring buffer location information regularly
  async::TaskClosureMethod<AmlG12TdmDspStream,
                           &AmlG12TdmDspStream::RingNotificationReport> position_timer_
      __TA_GUARDED(domain_token()){this};

  ddk::PDevFidl pdev_;

  zx::bti bti_;
  fidl::WireSyncClient<fuchsia_hardware_gpio::Gpio> enable_gpio_;
  uint64_t active_channels_bitmask_max_ = std::numeric_limits<uint64_t>::max();
  uint64_t active_channels_ = std::numeric_limits<uint64_t>::max();  // Enable all.
  zx::time active_channels_set_time_;

  bool override_mute_ = true;
  zx::interrupt irq_;
  std::atomic<bool> running_ = false;
  thrd_t thread_ = {};
  inspect::IntProperty status_time_;
  inspect::UintProperty dma_status_;
  inspect::UintProperty tdm_status_;
  inspect::UintProperty ring_buffer_physical_address_;
  size_t ring_buffer_size_ = 0;
  std::optional<fdf::MmioBuffer> dsp_mmio_;
};

}  // namespace aml_g12
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_AML_DSP_AML_G12_TDM_DSP_AUDIO_STREAM_H_
