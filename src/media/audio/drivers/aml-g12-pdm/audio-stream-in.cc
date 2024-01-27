// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "audio-stream-in.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/zx/clock.h>
#include <math.h>
#include <threads.h>
#include <unistd.h>

#include <numeric>
#include <optional>
#include <utility>

#include "src/media/audio/drivers/aml-g12-pdm/aml_g12_pdm_bind.h"
#include "src/media/lib/memory_barriers/memory_barriers.h"

namespace audio::aml_g12 {

constexpr size_t kMinSampleRate = 48000;
constexpr size_t kMaxSampleRate = 96000;

AudioStreamIn::AudioStreamIn(zx_device_t* parent) : SimpleAudioStream(parent, true /* is input */) {
  frames_per_second_ = kMinSampleRate;
  status_time_ = inspect().GetRoot().CreateInt("status_time", 0);
  dma_status_ = inspect().GetRoot().CreateUint("dma_status", 0);
  pdm_status_ = inspect().GetRoot().CreateUint("pdm_status", 0);
  ring_buffer_physical_address_ = inspect().GetRoot().CreateUint("ring_buffer_physical_address", 0);
}

int AudioStreamIn::Thread() {
  while (1) {
    zx::time timestamp;
    irq_.wait(&timestamp);
    if (!running_.load()) {
      break;
    }
    zxlogf(ERROR, "DMA status: 0x%08X  PDM status: 0x%08X", lib_->GetDmaStatus(),
           lib_->GetPdmStatus());
    status_time_.Set(timestamp.get());
    dma_status_.Set(lib_->GetDmaStatus());
    pdm_status_.Set(lib_->GetPdmStatus());
  }
  zxlogf(INFO, "Exiting interrupt thread");
  return 0;
}

zx_status_t AudioStreamIn::Create(void* ctx, zx_device_t* parent) {
  auto stream = audio::SimpleAudioStream::Create<AudioStreamIn>(parent);
  if (stream == nullptr) {
    zxlogf(ERROR, "Could not create aml-g12-pdm driver");
    return ZX_ERR_NO_MEMORY;
  }
  [[maybe_unused]] auto unused = fbl::ExportToRawPtr(&stream);
  return ZX_OK;
}

zx_status_t AudioStreamIn::SetGain(const audio_proto::SetGainReq& req) {
  return req.gain == 0.f ? ZX_OK : ZX_ERR_INVALID_ARGS;
}

zx_status_t AudioStreamIn::Init() {
  auto status = InitPDev();
  if (status != ZX_OK) {
    return status;
  }

  status = AddFormats();
  if (status != ZX_OK) {
    return status;
  }
  // Set our gain capabilities.
  cur_gain_state_.cur_gain = 0;
  cur_gain_state_.cur_mute = false;
  cur_gain_state_.cur_agc = false;
  cur_gain_state_.min_gain = 0;
  cur_gain_state_.max_gain = 0;
  cur_gain_state_.gain_step = 0;
  cur_gain_state_.can_mute = false;
  cur_gain_state_.can_agc = false;

  strncpy(mfr_name_, metadata_.manufacturer, sizeof(mfr_name_));
  strncpy(prod_name_, metadata_.product_name, sizeof(prod_name_));
  unique_id_ = AUDIO_STREAM_UNIQUE_ID_BUILTIN_MICROPHONE;
  snprintf(device_name_, sizeof(device_name_), "%s-audio-pdm-in", prod_name_);

  // TODO(mpuryear): change this to the domain of the clock received from the board driver
  clock_domain_ = 0;

  return ZX_OK;
}

zx_status_t AudioStreamIn::InitPDev() {
  size_t actual = 0;
  auto status = device_get_metadata(parent(), DEVICE_METADATA_PRIVATE, &metadata_,
                                    sizeof(metadata::AmlPdmConfig), &actual);
  if (status != ZX_OK || sizeof(metadata::AmlPdmConfig) != actual) {
    zxlogf(ERROR, "device_get_metadata failed %d", status);
    return status;
  }

  zx::result pdev_result = ddk::PDevFidl::Create(parent());
  if (pdev_result.is_error()) {
    pdev_result = ddk::PDevFidl::Create(parent(), ddk::PDevFidl::kFragmentName);
    if (pdev_result.is_error()) {
      zxlogf(ERROR, "get pdev protocol failed %s", pdev_result.status_string());
      return pdev_result.error_value();
    }
  }
  ddk::PDevFidl pdev = std::move(pdev_result.value());
  status = pdev.GetBti(0, &bti_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "could not obtain bti %d", status);
    return status;
  }

  std::optional<fdf::MmioBuffer> mmio0, mmio1, mmio2;
  status = pdev.MapMmio(0, &mmio0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "could not map mmio0 %d", status);
    return status;
  }
  status = pdev.MapMmio(1, &mmio1);
  if (status != ZX_OK) {
    zxlogf(ERROR, "could not map mmio1 %d", status);
    return status;
  }
  if (metadata_.version == metadata::AmlVersion::kA5) {
    status = pdev.MapMmio(2, &mmio2);
    if (status != ZX_OK) {
      zxlogf(ERROR, "could not map mmio2 %d", status);
      return status;
    }
  }
  status = pdev.GetInterrupt(0, 0, &irq_);
  if (status != ZX_ERR_OUT_OF_RANGE) {  // Not specified in the board file.
    if (status != ZX_OK) {
      zxlogf(ERROR, "could not get IRQ %d", status);
      return status;
    }

    auto irq_thread = [](void* arg) -> int {
      return reinterpret_cast<AudioStreamIn*>(arg)->Thread();
    };
    running_.store(true);
    int rc = thrd_create_with_name(&thread_, irq_thread, reinterpret_cast<void*>(this),
                                   "aml_pdm_irq_thread");
    if (rc != thrd_success) {
      zxlogf(ERROR, "could not create thread %d", rc);
      return status;
    }
  }

  lib_ = AmlPdmDevice::Create(*std::move(mmio0), *std::move(mmio1), *std::move(mmio2), HIFI_PLL,
                              metadata_.sysClockDivFactor - 1, metadata_.dClockDivFactor - 1,
                              TODDR_B, metadata_.version);
  if (lib_ == nullptr) {
    zxlogf(ERROR, "failed to create audio device");
    return ZX_ERR_NO_MEMORY;
  }

  // Initial setup of one page of buffer, just to be safe.
  status = InitBuffer(zx_system_get_page_size());
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to init buffer %d", status);
    return status;
  }
  status =
      lib_->SetBuffer(pinned_ring_buffer_.region(0).phys_addr, pinned_ring_buffer_.region(0).size);
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to set buffer %d", status);
    return status;
  }
  ring_buffer_physical_address_.Set(pinned_ring_buffer_.region(0).phys_addr);

  InitHw();

  return ZX_OK;
}

void AudioStreamIn::InitHw() {
  // Enable first metadata_.number_of_channels channels.
  lib_->ConfigPdmIn(static_cast<uint8_t>((1 << metadata_.number_of_channels) - 1));
  lib_->SetRate(frames_per_second_);
  lib_->Sync();
}

zx_status_t AudioStreamIn::ChangeFormat(const audio_proto::StreamSetFmtReq& req) {
  fifo_depth_ = lib_->fifo_depth();
  external_delay_nsec_ = 0;

  if (req.channels != metadata_.number_of_channels) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (req.frames_per_second != 48'000 && req.frames_per_second != 96'000) {
    return ZX_ERR_INVALID_ARGS;
  }
  frames_per_second_ = req.frames_per_second;

  InitHw();

  return ZX_OK;
}

zx_status_t AudioStreamIn::GetBuffer(const audio_proto::RingBufGetBufferReq& req,
                                     uint32_t* out_num_rb_frames, zx::vmo* out_buffer) {
  size_t ring_buffer_size = fbl::round_up<size_t, size_t>(
      req.min_ring_buffer_frames * frame_size_, std::lcm(frame_size_, lib_->GetBufferAlignment()));
  size_t out_frames = ring_buffer_size / frame_size_;
  if (out_frames > std::numeric_limits<uint32_t>::max()) {
    return ZX_ERR_INVALID_ARGS;
  }

  size_t vmo_size = fbl::round_up<size_t, size_t>(ring_buffer_size, zx_system_get_page_size());
  auto status = InitBuffer(vmo_size);
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to init buffer %d", status);
    return status;
  }

  constexpr uint32_t rights = ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER;
  status = ring_buffer_vmo_.duplicate(rights, out_buffer);
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to duplicate vmo");
    return status;
  }
  status = lib_->SetBuffer(pinned_ring_buffer_.region(0).phys_addr, ring_buffer_size);
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to set buffer %d", status);
    return status;
  }
  // This is safe because of the overflow check we made above.
  *out_num_rb_frames = static_cast<uint32_t>(out_frames);
  return status;
}

void AudioStreamIn::RingBufferShutdown() { lib_->Shutdown(); }

zx_status_t AudioStreamIn::Start(uint64_t* out_start_time) {
  *out_start_time = lib_->Start();

  uint32_t notifs = LoadNotificationsPerRing();
  if (notifs) {
    size_t size = 0;
    ring_buffer_vmo_.get_size(&size);
    notification_rate_ =
        zx::duration(zx_duration_from_usec(1'000 * pinned_ring_buffer_.region(0).size /
                                           (frame_size_ * frames_per_second_ / 1'000 * notifs)));
    notify_timer_.PostDelayed(dispatcher(), notification_rate_);
  } else {
    notification_rate_ = {};
  }
  return ZX_OK;
}

// Timer handler for sending out position notifications.
void AudioStreamIn::ProcessRingNotification() {
  ScopedToken t(domain_token());
  ZX_ASSERT(notification_rate_ != zx::duration());

  notify_timer_.PostDelayed(dispatcher(), notification_rate_);

  audio_proto::RingBufPositionNotify resp = {};
  resp.hdr.cmd = AUDIO_RB_POSITION_NOTIFY;

  resp.monotonic_time = zx::clock::get_monotonic().get();
  resp.ring_buffer_pos = lib_->GetRingPosition();
  NotifyPosition(resp);
}

void AudioStreamIn::ShutdownHook() {
  if (running_.load()) {
    running_.store(false);
    irq_.destroy();
    thrd_join(thread_, NULL);
  }
  lib_->Shutdown();
}

zx_status_t AudioStreamIn::Stop() {
  notify_timer_.Cancel();
  notification_rate_ = {};
  lib_->Stop();
  return ZX_OK;
}

zx_status_t AudioStreamIn::AddFormats() {
  fbl::AllocChecker ac;
  supported_formats_.reserve(1, &ac);
  if (!ac.check()) {
    zxlogf(ERROR, "Out of memory, can not create supported formats list");
    return ZX_ERR_NO_MEMORY;
  }

  SimpleAudioStream::SupportedFormat format = {};
  format.range.min_channels = metadata_.number_of_channels;
  format.range.max_channels = metadata_.number_of_channels;
  format.range.sample_formats = AUDIO_SAMPLE_FORMAT_16BIT;
  format.range.min_frames_per_second = kMinSampleRate;
  format.range.max_frames_per_second = kMaxSampleRate;
  format.range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY;

  supported_formats_.push_back(std::move(format));

  return ZX_OK;
}

zx_status_t AudioStreamIn::InitBuffer(size_t size) {
  lib_->Stop();
  // Make sure that all reads/writes have gone through.
  BarrierBeforeRelease();
  zx_status_t status = bti_.release_quarantine();
  if (status != ZX_OK) {
    zxlogf(ERROR, "could not release quarantine bti - %d", status);
    return status;
  }
  pinned_ring_buffer_.Unpin();
  status = zx_vmo_create_contiguous(bti_.get(), size, 0, ring_buffer_vmo_.reset_and_get_address());
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to allocate ring buffer vmo - %d", status);
    return status;
  }
  status = pinned_ring_buffer_.Pin(ring_buffer_vmo_, bti_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to pin ring buffer vmo - %d", status);
    return status;
  }
  if (pinned_ring_buffer_.region_count() != 1) {
    if (!AllowNonContiguousRingBuffer()) {
      zxlogf(ERROR, "buffer is not contiguous");
      return ZX_ERR_NO_MEMORY;
    }
  }

  return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AudioStreamIn::Create;
  return ops;
}();

}  // namespace audio::aml_g12

ZIRCON_DRIVER(aml_g12_pdm, audio::aml_g12::driver_ops, "zircon", "0.1");
