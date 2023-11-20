// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDIO_CONTROLLER_DEVICE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDIO_CONTROLLER_DEVICE_H_

#include <fidl/fuchsia.hardware.sdmmc/cpp/wire.h>
#include <fuchsia/hardware/sdio/c/banjo.h>
#include <lib/driver/component/cpp/driver_base.h>
#include <lib/inspect/component/cpp/component.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/result.h>

#include <array>
#include <atomic>
#include <memory>

#include <fbl/algorithm.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>

#include "sdio-function-device.h"
#include "sdmmc-device.h"

namespace sdmmc {

class SdmmcRootDevice;

class SdioControllerDevice : public ddk::InBandInterruptProtocol<SdioControllerDevice> {
 public:
  static constexpr char kDeviceName[] = "sdmmc-sdio";

  SdioControllerDevice(SdmmcRootDevice* parent, std::unique_ptr<SdmmcDevice> sdmmc)
      : parent_(parent), sdmmc_(std::move(sdmmc)) {
    for (size_t i = 0; i < funcs_.size(); i++) {
      funcs_[i] = {};
    }
  }
  ~SdioControllerDevice() { StopSdioIrqThread(); }

  static zx_status_t Create(SdmmcRootDevice* parent, std::unique_ptr<SdmmcDevice> sdmmc,
                            bool use_fidl, std::unique_ptr<SdioControllerDevice>* out_dev);
  // Returns the SdmmcDevice. Used if this SdioControllerDevice fails to probe (i.e., no eligible
  // device present).
  std::unique_ptr<SdmmcDevice> TakeSdmmcDevice() { return std::move(sdmmc_); }

  zx_status_t Probe(const fuchsia_hardware_sdmmc::wire::SdmmcMetadata& metadata);
  zx_status_t AddDevice();

  zx_status_t SdioGetDevHwInfo(uint8_t fn_idx, sdio_hw_info_t* out_hw_info) TA_EXCL(lock_);
  zx_status_t SdioEnableFn(uint8_t fn_idx) TA_EXCL(lock_);
  zx_status_t SdioDisableFn(uint8_t fn_idx) TA_EXCL(lock_);
  zx_status_t SdioEnableFnIntr(uint8_t fn_idx) TA_EXCL(lock_);
  zx_status_t SdioDisableFnIntr(uint8_t fn_idx) TA_EXCL(lock_);
  zx_status_t SdioUpdateBlockSize(uint8_t fn_idx, uint16_t blk_sz, bool deflt) TA_EXCL(lock_);
  zx_status_t SdioGetBlockSize(uint8_t fn_idx, uint16_t* out_cur_blk_size);
  zx_status_t SdioDoRwByte(bool write, uint8_t fn_idx, uint32_t addr, uint8_t write_byte,
                           uint8_t* out_read_byte) TA_EXCL(lock_);
  zx_status_t SdioGetInBandIntr(uint8_t fn_idx, zx::interrupt* out_irq);
  void SdioAckInBandIntr(uint8_t fn_idx);
  zx_status_t SdioIoAbort(uint8_t fn_idx) TA_EXCL(lock_);
  zx_status_t SdioIntrPending(uint8_t fn_idx, bool* out_pending);
  zx_status_t SdioDoVendorControlRwByte(bool write, uint8_t addr, uint8_t write_byte,
                                        uint8_t* out_read_byte);
  zx_status_t SdioRegisterVmo(uint8_t fn_idx, uint32_t vmo_id, zx::vmo vmo, uint64_t offset,
                              uint64_t size, uint32_t vmo_rights);
  zx_status_t SdioUnregisterVmo(uint8_t fn_idx, uint32_t vmo_id, zx::vmo* out_vmo);
  zx_status_t SdioDoRwTxn(uint8_t fn_idx, const sdio_rw_txn_t* txn);
  zx_status_t SdioRequestCardReset() TA_EXCL(lock_);
  zx_status_t SdioPerformTuning();

  void InBandInterruptCallback();

  // Called by children of this device.
  fidl::WireSyncClient<fuchsia_driver_framework::Node>& sdio_controller_node() {
    return sdio_controller_node_;
  }
  SdmmcRootDevice* parent() { return parent_; }

  // Visible for testing.
  zx_status_t Init(bool use_fidl = false) TA_EXCL(lock_) {
    fbl::AutoLock _(&lock_);
    return sdmmc_->Init(use_fidl);
  }

  zx_status_t StartSdioIrqThreadIfNeeded() TA_EXCL(irq_thread_lock_);
  void StopSdioIrqThread() TA_EXCL(irq_thread_lock_);

 private:
  struct SdioFuncTuple {
    uint8_t tuple_code;
    uint8_t tuple_body_size;
    uint8_t tuple_body[UINT8_MAX];
  };

  // SDIO cards support one common function and up to seven I/O functions. This struct is used to
  // keep track of each function's state as they can be configured independently.
  struct SdioFunction {
    sdio_func_hw_info_t hw_info;
    uint16_t cur_blk_size;
    bool enabled;
    bool intr_enabled;
  };

  zx_status_t ProbeLocked() TA_REQ(lock_);

  zx_status_t SdioReset() TA_REQ(lock_);
  // Reads the card common control registers (CCCR) to enumerate the card's capabilities.
  zx_status_t ProcessCccr() TA_REQ(lock_);
  // Reads the card information structure (CIS) for the given function to get the manufacturer
  // identification and function extensions tuples.
  zx_status_t ProcessCis(uint8_t fn_idx) TA_REQ(lock_);
  // Parses a tuple read from the CIS.
  zx_status_t ParseFnTuple(uint8_t fn_idx, const SdioFuncTuple& tup) TA_REQ(lock_);
  // Parses the manufacturer ID tuple and saves it in the given function's struct.
  zx_status_t ParseMfidTuple(uint8_t fn_idx, const SdioFuncTuple& tup) TA_REQ(lock_);
  // Parses the function extensions tuple and saves it in the given function's struct.
  zx_status_t ParseFuncExtTuple(uint8_t fn_idx, const SdioFuncTuple& tup) TA_REQ(lock_);
  // Reads the I/O function code and saves it in the given function's struct.
  zx_status_t ProcessFbr(uint8_t fn_idx) TA_REQ(lock_);
  // Popluates the given function's struct by calling the methods above. Also enables the
  // function and sets its default block size.
  zx_status_t InitFunc(uint8_t fn_idx) TA_REQ(lock_);

  zx_status_t SwitchFreq(uint32_t new_freq) TA_REQ(lock_);
  zx_status_t TrySwitchHs() TA_REQ(lock_);
  zx_status_t TrySwitchUhs() TA_REQ(lock_);
  zx_status_t Enable4BitBus() TA_REQ(lock_);
  zx_status_t SwitchBusWidth(uint32_t bw) TA_REQ(lock_);

  zx_status_t ReadData16(uint8_t fn_idx, uint32_t addr, uint16_t* word) TA_REQ(lock_);
  zx_status_t WriteData16(uint8_t fn_idx, uint32_t addr, uint16_t word) TA_REQ(lock_);

  zx_status_t SdioEnableFnLocked(uint8_t fn_idx) TA_REQ(lock_);
  zx_status_t SdioUpdateBlockSizeLocked(uint8_t fn_idx, uint16_t blk_sz, bool deflt) TA_REQ(lock_);
  zx_status_t SdioDoRwByteLocked(bool write, uint8_t fn_idx, uint32_t addr, uint8_t write_byte,
                                 uint8_t* out_read_byte) TA_REQ(lock_);

  zx::result<uint8_t> ReadCccrByte(uint32_t addr) TA_REQ(lock_);

  struct SdioTxnPosition {
    cpp20::span<const sdmmc_buffer_region_t> buffers;  // The buffers remaining to be processed.
    uint64_t first_buffer_offset;                      // The offset into the first buffer.
    uint32_t address;  // The current SDIO address, fixed if txn.incr is false.
  };

  // Returns an SdioTxnPosition representing the new position in the buffers list.
  zx::result<SdioTxnPosition> DoOneRwTxnRequest(uint8_t fn_idx, const sdio_rw_txn_t& txn,
                                                SdioTxnPosition current_position) TA_REQ(lock_);

  int SdioIrqThread();
  uint8_t interrupt_enabled_mask_ TA_GUARDED(lock_) = UINT8_MAX;

  fbl::Mutex irq_thread_lock_;  // Used to make thread start and stop atomic.
  thrd_t irq_thread_ TA_GUARDED(irq_thread_lock_) = 0;
  sync_completion_t irq_signal_;

  fbl::Mutex lock_;
  SdmmcRootDevice* const parent_;
  std::unique_ptr<SdmmcDevice> sdmmc_;
  std::atomic<bool> dead_ = false;
  std::array<zx::interrupt, SDIO_MAX_FUNCS> sdio_irqs_;
  std::array<SdioFunction, SDIO_MAX_FUNCS> funcs_ TA_GUARDED(lock_);
  sdio_device_hw_info_t hw_info_ TA_GUARDED(lock_);
  bool tuned_ = false;
  std::atomic<bool> tuning_in_progress_ = false;
  inspect::Inspector inspector_;
  inspect::Node root_;
  inspect::UintProperty tx_errors_;
  inspect::UintProperty rx_errors_;

  async_dispatcher_t* dispatcher_ = nullptr;

  std::optional<inspect::ComponentInspector> exposed_inspector_;

  fidl::WireSyncClient<fuchsia_driver_framework::Node> sdio_controller_node_;
  fidl::WireSyncClient<fuchsia_driver_framework::NodeController> controller_;

  std::array<std::unique_ptr<SdioFunctionDevice>, SDIO_MAX_FUNCS> child_sdio_function_devices_ = {};
};

}  // namespace sdmmc

#endif  // SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDIO_CONTROLLER_DEVICE_H_
