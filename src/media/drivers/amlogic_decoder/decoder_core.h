// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DECODER_CORE_H_
#define SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DECODER_CORE_H_

#include <lib/ddk/io-buffer.h>
#include <lib/zx/handle.h>
#include <zircon/status.h>

#include <mutex>

#include "macros.h"
#include "registers.h"
#include "src/lib/memory_barriers/memory_barriers.h"
#include "src/media/lib/internal_buffer/internal_buffer.h"

namespace amlogic_decoder {

struct MmioRegisters {
  DosRegisterIo* dosbus;
  AoRegisterIo* aobus;
  DmcRegisterIo* dmc;
  HiuRegisterIo* hiubus;
  ResetRegisterIo* reset;
  ParserRegisterIo* parser;
  DemuxRegisterIo* demux;
};

struct InputContext {
  ~InputContext() {
    BarrierBeforeRelease();
    // ~buffer
  }

  std::optional<InternalBuffer> buffer;

  uint32_t processed_video = 0;
};

enum class DeviceType;

enum class ClockType {
  kGclkVdec,
  kClkDos,
  kMax,
};

class DecoderCore {
 public:
  class Owner {
   public:
    [[nodiscard]] virtual zx::unowned_bti bti() = 0;

    [[nodiscard]] virtual MmioRegisters* mmio() = 0;

    // Increment the powered refcount by one, and if was 0, ungate clocks.
    virtual zx_status_t UngateClocks() = 0;

    // Decrement the powered refcount by one, and if now 0, gate clocks.
    virtual zx_status_t GateClocks() = 0;

    // Set a clock enabled or disabled.  In contrast to UngateClocks() / GateClocks(), this has no
    // refcount, so should only be used (via this interface) for clocks that are unique to only one
    // DecoderCore and which do not impact another DecoderCore.  An implementation of
    // DecoderCore::Owner is also free to use the implementation of this method as part of the
    // implementation of UngageClocks() / GateClocks(), but that detail is internal to those
    // implementations, not the concern of this interface (and the refcount on those calls still
    // applies).
    virtual zx_status_t ToggleClock(ClockType type, bool enable) = 0;

    [[nodiscard]] virtual DeviceType device_type() = 0;

    [[nodiscard]] virtual fuchsia::sysmem::AllocatorSyncPtr& SysmemAllocatorSyncPtr() = 0;
  };

  virtual ~DecoderCore() {}

  [[nodiscard]] virtual std::optional<InternalBuffer> LoadFirmwareToBuffer(const uint8_t* data,
                                                                           uint32_t len) = 0;
  [[nodiscard]] virtual zx_status_t LoadFirmware(const uint8_t* data, uint32_t len) = 0;
  [[nodiscard]] virtual zx_status_t LoadFirmware(InternalBuffer& buffer) = 0;
  virtual void StartDecoding() = 0;
  virtual void StopDecoding() = 0;
  virtual void WaitForIdle() = 0;
  virtual void InitializeStreamInput(bool use_parser, uint32_t buffer_address,
                                     uint32_t buffer_size) = 0;
  virtual void InitializeParserInput() = 0;
  virtual void InitializeDirectInput() = 0;
  // The write offset points to just after the last thing that was written into
  // the stream buffer.
  //
  // write_offset - offset into the stream buffer just after the last byte
  //     written.
  virtual void UpdateWriteOffset(uint32_t write_offset) = 0;
  // The write pointer points to just after the last thing that was written into
  // the stream buffer.
  //
  // write_pointer - physical pointer that must lie within the stream_buffer
  //     just after the last byte written into the stream buffer.
  virtual void UpdateWritePointer(uint32_t write_pointer) = 0;
  // This is the offset between the start of the stream buffer and the write
  // pointer.
  [[nodiscard]] virtual uint32_t GetStreamInputOffset() = 0;
  [[nodiscard]] virtual uint32_t GetReadOffset() = 0;

  [[nodiscard]] virtual zx_status_t InitializeInputContext(InputContext* context, bool is_secure) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  [[nodiscard]] virtual zx_status_t SaveInputContext(InputContext* context) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  [[nodiscard]] virtual zx_status_t RestoreInputContext(InputContext* context) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t IncrementPowerRef() {
    std::lock_guard<std::mutex> lock(power_ref_lock_);
    if (power_ref_count_++ == 0) {
      zx_status_t status = PowerOn();
      if (status != ZX_OK) {
        DECODE_ERROR("Failed to power on: %s", zx_status_get_string(status));
        return status;
      }
    }
    return ZX_OK;
  }

  zx_status_t DecrementPowerRef() {
    std::lock_guard<std::mutex> lock(power_ref_lock_);
    if (--power_ref_count_ == 0) {
      zx_status_t status = PowerOff();
      if (status != ZX_OK) {
        DECODE_ERROR("Failed to power off: %s", zx_status_get_string(status));
        return status;
      }
    }
    return ZX_OK;
  }

 protected:
  virtual zx_status_t PowerOn() __TA_REQUIRES(power_ref_lock_) = 0;
  virtual zx_status_t PowerOff() __TA_REQUIRES(power_ref_lock_) = 0;

 private:
  std::mutex power_ref_lock_;
  // In practice power_ref_count_ will only be accesses under the video decoder lock, but adding its
  // own lock makes locking easier to enforce.
  __TA_GUARDED(power_ref_lock_) uint64_t power_ref_count_ = 0;
};

// This is an RAII struct used to ensure the core is powered up as long as a client is using it.
class PowerReference {
 public:
  explicit PowerReference(DecoderCore* core) : core_(core) { core_->IncrementPowerRef(); }
  PowerReference(const PowerReference& ref) = delete;

  ~PowerReference() { core_->DecrementPowerRef(); }

 private:
  DecoderCore* core_;
};

}  // namespace amlogic_decoder

#endif  // SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DECODER_CORE_H_
