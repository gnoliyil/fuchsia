// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_AS370_INCLUDE_SOC_AS370_SYN_AUDIO_OUT_H_
#define SRC_DEVICES_LIB_AS370_INCLUDE_SOC_AS370_SYN_AUDIO_OUT_H_

#include <assert.h>
#include <fuchsia/hardware/shareddma/cpp/banjo.h>
#include <lib/mmio/mmio.h>
#include <threads.h>
#include <zircon/syscalls/port.h>

#include <memory>
#include <utility>

// Output device set fixed at:
// Input: 2 x 32 bits samples from DMA.
// Output: I2S 48kHz, 32 bits slots/samples.
class SynAudioOutDevice {
 public:
  // Move operators are implicitly disabled.
  SynAudioOutDevice(const SynAudioOutDevice&) = delete;
  SynAudioOutDevice& operator=(const SynAudioOutDevice&) = delete;

  static std::unique_ptr<SynAudioOutDevice> Create(ddk::MmioBuffer mmio_avio_global,
                                                   ddk::MmioBuffer mmio_i2s,
                                                   ddk::SharedDmaProtocolClient dma);

  // Returns offset of dma pointer in the ring buffer.
  uint32_t GetRingPosition();

  // Starts clocking data with data fetched from the beginning of the buffer.
  // Returns its best estimation of the actual time the buffer pointer started moving.
  uint64_t Start();

  // Stops clocking data out (physical bus signals remain active).
  // Returns its best estimation of the actual time the buffer pointer stopped moving.
  uint64_t Stop();

  // Stops clocking data and quiets output signals.
  void Shutdown();

  uint32_t fifo_depth() const;
  zx_status_t GetBuffer(size_t size, zx::vmo* buffer);

 private:
  SynAudioOutDevice(ddk::MmioBuffer mmio_avio_global, ddk::MmioBuffer mmio_i2s,
                    ddk::SharedDmaProtocolClient dma);
  zx_status_t Init();

  ddk::MmioBuffer avio_global_;
  ddk::MmioBuffer i2s_;
  bool enabled_ = false;
  zx::vmo ring_buffer_;
  ddk::SharedDmaProtocolClient dma_;
  zx::vmo dma_buffer_;
};

#endif  // SRC_DEVICES_LIB_AS370_INCLUDE_SOC_AS370_SYN_AUDIO_OUT_H_
