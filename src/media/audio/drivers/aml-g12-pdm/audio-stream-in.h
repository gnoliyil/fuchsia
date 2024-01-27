// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_AML_G12_PDM_AUDIO_STREAM_IN_H_
#define SRC_MEDIA_AUDIO_DRIVERS_AML_G12_PDM_AUDIO_STREAM_IN_H_

#include <fuchsia/hardware/platform/device/cpp/banjo.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/fzl/pinned-vmo.h>
#include <lib/simple-audio-stream/simple-audio-stream.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/bti.h>
#include <lib/zx/vmo.h>

#include <memory>
#include <optional>

#include <audio-proto/audio-proto.h>
#include <ddktl/device.h>
#include <soc/aml-common/aml-pdm-audio.h>

namespace audio::aml_g12 {

class AudioStreamIn : public SimpleAudioStream {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

 protected:
  zx_status_t Init() __TA_REQUIRES(domain_token()) override;
  zx_status_t ChangeFormat(const audio_proto::StreamSetFmtReq& req)
      __TA_REQUIRES(domain_token()) override;
  zx_status_t GetBuffer(const audio_proto::RingBufGetBufferReq& req, uint32_t* out_num_rb_frames,
                        zx::vmo* out_buffer) __TA_REQUIRES(domain_token()) override;
  zx_status_t Start(uint64_t* out_start_time) __TA_REQUIRES(domain_token()) override;
  zx_status_t Stop() __TA_REQUIRES(domain_token()) override;
  zx_status_t SetGain(const audio_proto::SetGainReq& req) override;
  zx_status_t ChangeActiveChannels(uint64_t mask) __TA_REQUIRES(domain_token()) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
  void RingBufferShutdown() TA_REQ(domain_token()) override;
  void ShutdownHook() __TA_REQUIRES(domain_token()) override;
  explicit AudioStreamIn(zx_device_t* parent);

 private:
  friend class fbl::RefPtr<AudioStreamIn>;
  friend class SimpleAudioStream;

  zx_status_t AddFormats() TA_REQ(domain_token());
  zx_status_t InitBuffer(size_t size) TA_REQ(domain_token());
  zx_status_t InitPDev() TA_REQ(domain_token());
  void InitHw();
  void ProcessRingNotification();
  virtual bool AllowNonContiguousRingBuffer() { return false; }
  int Thread();

  zx::duration notification_rate_ = {};
  uint32_t frames_per_second_ = 0;
  async::TaskClosureMethod<AudioStreamIn, &AudioStreamIn::ProcessRingNotification> notify_timer_
      __TA_GUARDED(domain_token()){this};

  zx::vmo ring_buffer_vmo_;
  fzl::PinnedVmo pinned_ring_buffer_;
  std::unique_ptr<AmlPdmDevice> lib_;
  zx::bti bti_;
  metadata::AmlPdmConfig metadata_ = {};

  zx::interrupt irq_;
  std::atomic<bool> running_ = false;
  thrd_t thread_ = {};
  inspect::IntProperty status_time_;
  inspect::UintProperty dma_status_;
  inspect::UintProperty pdm_status_;
  inspect::UintProperty ring_buffer_physical_address_;
};
}  // namespace audio::aml_g12

#endif  // SRC_MEDIA_AUDIO_DRIVERS_AML_G12_PDM_AUDIO_STREAM_IN_H_
