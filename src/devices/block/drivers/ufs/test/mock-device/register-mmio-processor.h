// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_UFS_TEST_MOCK_DEVICE_REGISTER_MMIO_PROCESSOR_H_
#define SRC_DEVICES_BLOCK_DRIVERS_UFS_TEST_MOCK_DEVICE_REGISTER_MMIO_PROCESSOR_H_

#include <lib/mmio-ptr/fake.h>
#include <lib/mmio/mmio-buffer.h>

#include <functional>

#include "handler.h"
#include "src/devices/block/drivers/ufs/ufs.h"

namespace ufs {
namespace ufs_mock_device {

class UfsMockDevice;

class RegisterMmioProcessor {
 public:
  using MmioWriteHandler = std::function<void(UfsMockDevice&, uint32_t)>;

  RegisterMmioProcessor(const RegisterMmioProcessor&) = delete;
  RegisterMmioProcessor& operator=(const RegisterMmioProcessor&) = delete;
  RegisterMmioProcessor(const RegisterMmioProcessor&&) = delete;
  RegisterMmioProcessor& operator=(const RegisterMmioProcessor&&) = delete;
  explicit RegisterMmioProcessor(UfsMockDevice& mock_device) : mock_device_(mock_device) {}

  static const fdf::internal::MmioBufferOps& GetMmioOps() { return kMmioOps; }

  static void DefaultISHandler(UfsMockDevice& mock_device, uint32_t value);
  static void DefaultHCEHandler(UfsMockDevice& mock_device, uint32_t value);
  static void DefaultUTRLDBRHandler(UfsMockDevice& mock_device, uint32_t value);
  static void DefaultUICCMDHandler(UfsMockDevice& mock_device, uint32_t value);

  DEF_DEFAULT_HANDLER_BEGIN(RegisterMap, MmioWriteHandler)
  DEF_DEFAULT_HANDLER(RegisterMap::kHCE, DefaultHCEHandler)
  DEF_DEFAULT_HANDLER(RegisterMap::kIS, DefaultISHandler)
  DEF_DEFAULT_HANDLER(RegisterMap::kUTRLDBR, DefaultUTRLDBRHandler)
  DEF_DEFAULT_HANDLER(RegisterMap::kUICCMD, DefaultUICCMDHandler)
  DEF_DEFAULT_HANDLER_END()

 private:
  void MmioWrite(uint32_t value, zx_off_t offset);
  uint32_t MmioRead(zx_off_t offset);

#define STUB_IO_OP(bits)                                                                        \
  static void Write##bits(const void* ctx, const mmio_buffer_t& mmio, uint##bits##_t value,     \
                          zx_off_t offs) {                                                      \
    ZX_ASSERT(false);                                                                           \
  }                                                                                             \
  static uint##bits##_t Read##bits(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs) { \
    ZX_ASSERT(false);                                                                           \
  }

  STUB_IO_OP(64)
  STUB_IO_OP(16)
  STUB_IO_OP(8)
#undef STUB_IO_OP

  static void Write32(const void* ctx, const mmio_buffer_t& mmio, uint32_t value, zx_off_t offset);
  static uint32_t Read32(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs);

  static constexpr fdf::internal::MmioBufferOps kMmioOps = {
      .Read8 = Read8,
      .Read16 = Read16,
      .Read32 = Read32,
      .Read64 = Read64,
      .Write8 = Write8,
      .Write16 = Write16,
      .Write32 = Write32,
      .Write64 = Write64,
  };

  UfsMockDevice& mock_device_;
};

}  // namespace ufs_mock_device
}  // namespace ufs

#endif  // SRC_DEVICES_BLOCK_DRIVERS_UFS_TEST_MOCK_DEVICE_REGISTER_MMIO_PROCESSOR_H_
