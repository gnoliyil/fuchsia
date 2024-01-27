// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_AS370_TDM_OUTPUT_AUDIO_STREAM_OUT_H_
#define SRC_MEDIA_AUDIO_DRIVERS_AS370_TDM_OUTPUT_AUDIO_STREAM_OUT_H_

#include <fuchsia/hardware/clock/cpp/banjo.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <fuchsia/hardware/platform/device/cpp/banjo.h>
#include <lib/ddk/io-buffer.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/fzl/pinned-vmo.h>
#include <lib/simple-audio-stream/simple-audio-stream.h>
#include <lib/simple-codec/simple-codec-client.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/thread_annotations.h>

#include <memory>
#include <optional>

#include <audio-proto/audio-proto.h>
#include <ddktl/device-internal.h>
#include <ddktl/device.h>
#include <fbl/mutex.h>
#include <soc/as370/as370-audio.h>
#include <soc/as370/syn-audio-out.h>

namespace audio {
namespace as370 {

class As370AudioStreamOut : public SimpleAudioStream {
 protected:
  zx_status_t Init() TA_REQ(domain_token()) override;
  zx_status_t ChangeFormat(const audio_proto::StreamSetFmtReq& req) TA_REQ(domain_token()) override;
  zx_status_t GetBuffer(const audio_proto::RingBufGetBufferReq& req, uint32_t* out_num_rb_frames,
                        zx::vmo* out_buffer) TA_REQ(domain_token()) override;
  zx_status_t Start(uint64_t* out_start_time) TA_REQ(domain_token()) override;
  zx_status_t Stop() TA_REQ(domain_token()) override;
  zx_status_t SetGain(const audio_proto::SetGainReq& req) TA_REQ(domain_token()) override;
  zx_status_t ChangeActiveChannels(uint64_t mask) __TA_REQUIRES(domain_token()) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
  void ShutdownHook() TA_REQ(domain_token()) override;

 private:
  enum {
    kAvpll0Clk,
    kAvpll1Clk,
    kClockCount,
  };

  friend class SimpleAudioStream;
  friend class fbl::RefPtr<As370AudioStreamOut>;

  As370AudioStreamOut(zx_device_t* parent);
  ~As370AudioStreamOut() {}

  zx_status_t AddFormats() TA_REQ(domain_token());
  zx_status_t InitBuffer(size_t size) TA_REQ(domain_token());
  zx_status_t InitPdev() TA_REQ(domain_token());
  void ProcessRingNotification();

  uint32_t us_per_notification_ = 0;
  async::TaskClosureMethod<As370AudioStreamOut, &As370AudioStreamOut::ProcessRingNotification>
      notify_timer_ TA_GUARDED(domain_token()){this};
  ddk::PDevFidl pdev_ TA_GUARDED(domain_token());
  zx::vmo ring_buffer_vmo_ TA_GUARDED(domain_token());
  std::unique_ptr<SynAudioOutDevice> lib_;
  ddk::ClockProtocolClient clks_[kClockCount] TA_GUARDED(domain_token());
  SimpleCodecClient codec_ TA_GUARDED(domain_token());
  metadata::As370Config metadata_ = {};
};

}  // namespace as370
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_AS370_TDM_OUTPUT_AUDIO_STREAM_OUT_H_
