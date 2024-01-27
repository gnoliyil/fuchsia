// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "intel-i2c-controller.h"

#include <assert.h>
#include <errno.h>
#include <fidl/fuchsia.hardware.acpi/cpp/wire.h>
#include <fidl/fuchsia.hardware.acpi/cpp/wire_types.h>
#include <fidl/fuchsia.hardware.i2c.businfo/cpp/wire.h>
#include <fidl/fuchsia.hardware.pci/cpp/wire_types.h>
#include <fuchsia/hardware/i2cimpl/c/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/device-protocol/pci.h>
#include <lib/fidl/cpp/wire/object_view.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/listnode.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <memory>
#include <vector>

#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>

#include "binding.h"

namespace intel_i2c {

inline void RmwReg32(MMIO_PTR volatile uint32_t* addr, uint32_t startbit, uint32_t width,
                     uint32_t val) {
  return MmioWrite32((MmioRead32(addr) & ~(((1 << width) - 1) << startbit)) | (val << startbit),
                     addr);
}

constexpr uint32_t kDevidleControl = 0x24c;
constexpr uint32_t kDevidleControlCmdInProgress = 0;
constexpr uint32_t kDevidleControlDevidle = 2;
constexpr uint32_t kDevidleControlRestoreRequired = 3;

// Number of entries at which the FIFO level triggers happen
constexpr uint32_t kDefaultRxFifoTriggerLevel = 8;
constexpr uint32_t kDefaultTxFifoTriggerLevel = 8;

// Signals used on the controller's event_handle
constexpr uint32_t kRxFullSignal = ZX_USER_SIGNAL_0;
constexpr uint32_t kTxEmptySignal = ZX_USER_SIGNAL_1;
constexpr uint32_t kStopDetectedSignal = ZX_USER_SIGNAL_2;
constexpr uint32_t kErrorDetectedSignal = ZX_USER_SIGNAL_3;

// More than enough
constexpr size_t MAX_TRANSFER_SIZE = (UINT16_MAX - 1);

constexpr uint32_t kIntelDesignwareCompType = 0x44570140;

zx_status_t IntelI2cController::Create(void* ctx, zx_device_t* parent) {
  fidl::ClientEnd<fuchsia_hardware_acpi::Device> client;
  auto server_end = fidl::CreateEndpoints(&client);
  if (server_end.is_error()) {
    return server_end.error_value();
  }

  fbl::AllocChecker ac;
  auto dev = std::unique_ptr<IntelI2cController>(new (&ac) IntelI2cController(parent));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  auto status = dev->Init();
  if (status != ZX_OK) {
    return status;
  }

  [[maybe_unused]] auto* dummy = dev.release();
  return ZX_OK;
}

zx_status_t IntelI2cController::Init() {
  mtx_init(&mutex_, mtx_plain);
  mtx_init(&irq_mask_mutex_, mtx_plain);

  auto acpi_result = DdkConnectFragmentFidlProtocol<fuchsia_hardware_acpi::Service::Device>("acpi");
  if (acpi_result.is_error()) {
    return acpi_result.error_value();
  }

  acpi_.Bind(std::move(*acpi_result));

  uint16_t vendor_id;
  uint16_t device_id;
  pci_.ReadConfig16(fuchsia_hardware_pci::Config::kVendorId, &vendor_id);
  pci_.ReadConfig16(fuchsia_hardware_pci::Config::kDeviceId, &device_id);

  zx_status_t status = pci_.MapMmio(0u, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "i2c: failed to map mmio 0: %d", status);
    return status;
  }

  regs_ = reinterpret_cast<MMIO_PTR I2cRegs*>(mmio_->get());

  status = pci_.ConfigureInterruptMode(1, &irq_mode_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "i2c: failed to set irq mode: %d", status);
    return status;
  }

  // get irq handle
  status = pci_.MapInterrupt(0, &irq_handle_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "i2c: failed to get irq handle: %d", status);
    return status;
  }

  status = zx::event::create(0, &event_handle_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "i2c: failed to create event handle: %d", status);
    return status;
  }

  // start irq thread
  int ret = thrd_create_with_name(
      &irq_thread_,
      [](void* arg) -> int { return reinterpret_cast<IntelI2cController*>(arg)->IrqThread(); },
      this, "i2c-irq");
  if (ret != thrd_success) {
    zxlogf(ERROR, "i2c: failed to create irq thread: %d", ret);
    return status;
  }

  // Run the bus at standard speed by default.
  bus_freq_ = kI2cMaxStandardSpeedHz;

  status = DeviceSpecificInit(device_id);
  if (status != ZX_OK) {
    zxlogf(ERROR, "i2c: device specific init failed: %d", status);
    return status;
  }

  status = ComputeBusTiming();
  if (status < 0) {
    zxlogf(ERROR, "i2c: compute bus timing failed: %d", status);
    return status;
  }

  GetAcpiConfiguration("FPCN", &fmp_scl_hcnt_, &fmp_scl_lcnt_, &fmp_sda_hold_);
  GetAcpiConfiguration("FMCN", &fs_scl_hcnt_, &fs_scl_lcnt_, &fs_sda_hold_);
  GetAcpiConfiguration("SSCN", &ss_scl_hcnt_, &ss_scl_lcnt_, &ss_sda_hold_);

  // Configure the I2C controller.
  fbl::AutoLock lock(&mutex_);
  status = Reset();
  if (status < 0) {
    zxlogf(ERROR, "i2c: reset controller failed: %d", status);
    return status;
  }

  // We add one device. This device holds DEVICE_METADATA_I2C_CHANNELS
  // which contains info for each child device.
  // TODO: This should be a composite device that also holds interrupt information.

  char name[ZX_DEVICE_NAME_MAX];
  snprintf(name, sizeof(name), "i2c-bus-%04x", device_id);

  status =
      DdkAdd(ddk::DeviceAddArgs(name).forward_metadata(parent(), DEVICE_METADATA_I2C_CHANNELS));
  if (status < 0) {
    zxlogf(ERROR, "device add failed: %s", zx_status_get_string(status));
    return status;
  }

  zxlogf(INFO,
         "initialized intel serialio i2c driver, "
         "reg=%p regsize=%ld",
         regs_, mmio_->get_size());

  return ZX_OK;
}

void IntelI2cController::GetAcpiConfiguration(const char* name, uint16_t* scl_hcnt,
                                              uint16_t* scl_lcnt, uint16_t* sda_hold) {
  auto result =
      acpi_->EvaluateObject(fidl::StringView::FromExternal(name),
                            fuchsia_hardware_acpi::wire::EvaluateObjectMode::kPlainObject, {});
  if (!result.ok()) {
    zxlogf(WARNING, "FIDL call to EvaluateObject('%s') failed: %s.", name,
           result.FormatDescription().data());
    return;
  }

  if (result.value().is_error()) {
    if (result.value().error_value() != fuchsia_hardware_acpi::wire::Status::kNotFound) {
      zxlogf(WARNING, "EvaluateObject('%s') failed: %d", name,
             static_cast<uint32_t>(result.value().error_value()));
    }
    return;
  }

  fidl::WireOptional<fuchsia_hardware_acpi::wire::EncodedObject>& maybe_encoded =
      result->value()->result;
  if (!maybe_encoded.has_value()) {
    zxlogf(WARNING, "Received an absent |EncodedObject| union.");
    return;
  }

  if (!maybe_encoded->is_object()) {
    zxlogf(WARNING, "Received a present |EncodedObject| union that does not contain an object.");
    return;
  }

  auto& obj = maybe_encoded->object();
  if (obj.is_package_val() && obj.package_val().value.count() == 3) {
    auto& package = obj.package_val().value;
    *scl_hcnt = package[0].integer_val();
    *scl_lcnt = package[1].integer_val();
    *sda_hold = package[2].integer_val();
  }
}

void IntelI2cController::DdkInit(ddk::InitTxn txn) {
  auto status = AddSubordinates();
  if (status != ZX_OK) {
    zxlogf(ERROR, "adding subordinates failed: %s", zx_status_get_string(status));
    txn.Reply(status);
    return;
  }

  txn.Reply(ZX_OK);
}

zx_status_t IntelI2cController::I2cImplTransact(const uint32_t bus_id, const i2c_impl_op_t* op_list,
                                                const size_t op_count) {
  if (op_count == 0) {
    return ZX_OK;
  }

  fbl::AutoLock lock(&mutex_);

  // Every op has the same address/subordinate.
  auto it = subordinates_.find(op_list->address);

  if (it == subordinates_.end()) {
    return ZX_ERR_NOT_FOUND;
  }

  auto& subordinate = it->second;

  IntelI2cSubordinateSegment segs[I2C_IMPL_MAX_RW_OPS];

  if (op_count > I2C_IMPL_MAX_RW_OPS) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  for (size_t i = 0; i < op_count; ++i) {
    segs[i].buf = reinterpret_cast<uint8_t*>(op_list[i].data_buffer);
    segs[i].len = static_cast<int>(op_list[i].data_size);
    if (op_list[i].is_read) {
      segs[i].type = IntelI2cSubordinateSegment::kRead;
    } else {
      segs[i].type = IntelI2cSubordinateSegment::kWrite;
    }
  }

  zx_status_t status = subordinate->Transfer(segs, static_cast<int>(op_count));
  if (status != ZX_OK) {
    zxlogf(ERROR, "intel-i2c-controller: subordinate transfer failed with: %d\n", status);
    Reset();
  }

  return status;
}

uint32_t IntelI2cController::I2cImplGetBusBase() {
  auto result = acpi_->GetBusId();
  if (result.ok() && result->is_ok()) {
    return result->value()->bus_id;
  }
  return UINT32_MAX;
}
uint32_t IntelI2cController::I2cImplGetBusCount() { return 1; }

zx_status_t IntelI2cController::I2cImplGetMaxTransferSize(const uint32_t bus_id, size_t* out_size) {
  *out_size = MAX_TRANSFER_SIZE;
  return ZX_OK;
}

zx_status_t IntelI2cController::I2cImplSetBitrate(const uint32_t bus_id, const uint32_t bitrate) {
  // TODO: implement
  return ZX_ERR_NOT_SUPPORTED;
}

uint8_t IntelI2cController::ExtractTxFifoDepthFromParam(const uint32_t param) {
  return ((param >> 16) & 0xff) + 1;
}

uint8_t IntelI2cController::ExtractRxFifoDepthFromParam(const uint32_t param) {
  return ((param >> 8) & 0xff) + 1;
}

uint32_t IntelI2cController::ChipAddrMask(const int width) { return ((1 << width) - 1); }

zx_status_t IntelI2cController::AddSubordinate(const uint8_t width, const uint16_t address) {
  if ((width != kI2c7BitAddress && width != kI2c10BitAddress) ||
      (address & ~ChipAddrMask(width)) != 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&mutex_);

  // Make sure a subordinate with the given address doesn't already exist.
  auto it = subordinates_.find(address);
  if (it != subordinates_.end()) {
    return ZX_ERR_ALREADY_EXISTS;
  }

  auto subordinate = IntelI2cSubordinate::Create(this, width, address);

  if (subordinate == nullptr) {
    zxlogf(ERROR, "Failed to create subordinate.");
    return ZX_ERR_INVALID_ARGS;
  }

  subordinates_[address] = std::move(subordinate);

  return ZX_OK;
}

uint32_t IntelI2cController::ComputeSclHcnt(const uint32_t controller_freq,
                                            const uint32_t t_high_nanos, const uint32_t t_r_nanos) {
  uint32_t clock_freq_kilohz = controller_freq / 1000;

  // We need high count to satisfy highcount + 3 >= clock * (t_HIGH + t_r_max)
  // Apparently the counter starts as soon as the controller releases SCL, so
  // include t_r to account for potential delay in rising.
  //
  // In terms of units, the division should really be thought of as a
  // (1 s)/(1000000000 ns) factor to get this into the right scale.
  uint32_t high_count = (clock_freq_kilohz * (t_high_nanos + t_r_nanos) + 500000);
  return high_count / 1000000 - 3;
}

uint32_t IntelI2cController::ComputeSclLcnt(const uint32_t controller_freq,
                                            const uint32_t t_low_nanos, const uint32_t t_f_nanos) {
  uint32_t clock_freq_kilohz = controller_freq / 1000;

  // We need low count to satisfy lowcount + 1 >= clock * (t_LOW + t_f_max)
  // Apparently the counter starts as soon as the controller pulls SCL low, so
  // include t_f to account for potential delay in falling.
  //
  // In terms of units, the division should really be thought of as a
  // (1 s)/(1000000000 ns) factor to get this into the right scale.
  uint32_t low_count = (clock_freq_kilohz * (t_low_nanos + t_f_nanos) + 500000);
  return low_count / 1000000 - 1;
}

zx_status_t IntelI2cController::ComputeBusTiming() {
  // These constants are from the i2c timing requirements
  uint32_t fmp_hcnt = ComputeSclHcnt(controller_freq_, 260, 120);
  uint32_t fmp_lcnt = ComputeSclLcnt(controller_freq_, 500, 120);
  uint32_t fs_hcnt = ComputeSclHcnt(controller_freq_, 600, 300);
  uint32_t fs_lcnt = ComputeSclLcnt(controller_freq_, 1300, 300);
  uint32_t ss_hcnt = ComputeSclHcnt(controller_freq_, 4000, 300);
  uint32_t ss_lcnt = ComputeSclLcnt(controller_freq_, 4700, 300);

  // Make sure the counts are within bounds.
  if (fmp_hcnt >= (1 << 16) || fmp_hcnt < 6 || fmp_lcnt >= (1 << 16) || fmp_lcnt < 8) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  if (fs_hcnt >= (1 << 16) || fs_hcnt < 6 || fs_lcnt >= (1 << 16) || fs_lcnt < 8) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  if (ss_hcnt >= (1 << 16) || ss_hcnt < 6 || ss_lcnt >= (1 << 16) || ss_lcnt < 8) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  fmp_scl_hcnt_ = static_cast<uint16_t>(fmp_hcnt);
  fmp_scl_lcnt_ = static_cast<uint16_t>(fmp_lcnt);
  fs_scl_hcnt_ = static_cast<uint16_t>(fs_hcnt);
  fs_scl_lcnt_ = static_cast<uint16_t>(fs_lcnt);
  ss_scl_hcnt_ = static_cast<uint16_t>(ss_hcnt);
  ss_scl_lcnt_ = static_cast<uint16_t>(ss_lcnt);
  ss_sda_hold_ = fs_sda_hold_ = fmp_sda_hold_ = 1;
  return ZX_OK;
}

zx_status_t IntelI2cController::SetBusFrequency(const uint32_t frequency) {
  if (frequency != kI2cMaxFastSpeedHz && frequency != kI2cMaxStandardSpeedHz &&
      frequency != kI2cMaxFastPlusSpeedHz) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&mutex_);
  bus_freq_ = frequency;

  zx_status_t status = Reset();
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

int IntelI2cController::IrqThread() {
  zx_status_t status;
  for (;;) {
    if (irq_mode_ == fuchsia_hardware_pci::InterruptMode::kLegacy) {
      pci_.AckInterrupt();
    }

    status = irq_handle_.wait(nullptr);
    if (status != ZX_OK) {
      zxlogf(ERROR, "i2c: error waiting for interrupt: %d", status);
      break;
    }
    uint32_t intr_stat = MmioRead32(&regs_->intr_stat);
    zxlogf(TRACE, "Received i2c interrupt: %x %x", intr_stat, MmioRead32(&regs_->raw_intr_stat));
    if (intr_stat & (1u << kIntrRxUnder)) {
      // If we hit an underflow, it's a bug.
      event_handle_.signal(0, kErrorDetectedSignal);
      MmioRead32(&regs_->clr_rx_under);
      zxlogf(ERROR, "i2c: rx underflow detected!");
    }
    if (intr_stat & (1u << kIntrRxOver)) {
      // If we hit an overflow, it's a bug.
      event_handle_.signal(0, kErrorDetectedSignal);
      MmioRead32(&regs_->clr_rx_over);
      zxlogf(ERROR, "i2c: rx overflow detected!");
    }
    if (intr_stat & (1u << kIntrRxFull)) {
      fbl::AutoLock lock(&irq_mask_mutex_);
      event_handle_.signal(0, kRxFullSignal);
      RmwReg32(&regs_->intr_mask, kIntrRxFull, 1, 0);
    }
    if (intr_stat & (1u << kIntrTxOver)) {
      // If we hit an overflow, it's a bug.
      event_handle_.signal(0, kErrorDetectedSignal);
      MmioRead32(&regs_->clr_tx_over);
      zxlogf(ERROR, "i2c: tx overflow detected!");
    }
    if (intr_stat & (1u << kIntrTxEmpty)) {
      fbl::AutoLock lock(&irq_mask_mutex_);
      event_handle_.signal(0, kTxEmptySignal);
      RmwReg32(&regs_->intr_mask, kIntrTxEmpty, 1, 0);
    }
    if (intr_stat & (1u << kIntrTxAbort)) {
      zxlogf(ERROR, "i2c: tx abort detected: 0x%08x", MmioRead32(&regs_->tx_abrt_source));
      event_handle_.signal(0, kErrorDetectedSignal);
      MmioRead32(&regs_->clr_tx_abort);
    }
    if (intr_stat & (1u << kIntrActivity)) {
      // Should always be masked...remask it.
      fbl::AutoLock lock(&irq_mask_mutex_);
      RmwReg32(&regs_->intr_mask, kIntrActivity, 1, 0);
      zxlogf(INFO, "i2c: spurious activity irq");
    }
    if (intr_stat & (1u << kIntrStopDetection)) {
      event_handle_.signal(0, kStopDetectedSignal);
      MmioRead32(&regs_->clr_stop_det);
    }
    if (intr_stat & (1u << kIntrStartDetection)) {
      MmioRead32(&regs_->clr_start_det);
    }
    if (intr_stat & (1u << kIntrGeneralCall)) {
      // Should always be masked...remask it.
      fbl::AutoLock lock(&irq_mask_mutex_);
      RmwReg32(&regs_->intr_mask, kIntrGeneralCall, 1, 0);
      zxlogf(INFO, "i2c: spurious general call irq");
    }
  }
  return 0;
}

zx_status_t IntelI2cController::WaitForRxFull(const zx::time deadline) {
  uint32_t observed;
  zx_status_t status =
      event_handle_.wait_one(kRxFullSignal | kErrorDetectedSignal, deadline, &observed);
  if (status != ZX_OK) {
    return status;
  }
  if (observed & kErrorDetectedSignal) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t IntelI2cController::WaitForTxEmpty(const zx::time deadline) {
  uint32_t observed;
  zx_status_t status =
      event_handle_.wait_one(kTxEmptySignal | kErrorDetectedSignal, deadline, &observed);
  if (status != ZX_OK) {
    return status;
  }
  if (observed & kErrorDetectedSignal) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t IntelI2cController::WaitForStopDetect(const zx::time deadline) {
  uint32_t observed;
  zx_status_t status =
      event_handle_.wait_one(kStopDetectedSignal | kErrorDetectedSignal, deadline, &observed);
  if (status != ZX_OK) {
    return status;
  }
  if (observed & kErrorDetectedSignal) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t IntelI2cController::CheckForError() {
  uint32_t observed;
  zx_status_t status = event_handle_.wait_one(kErrorDetectedSignal, zx::time(0), &observed);
  if (status != ZX_OK && status != ZX_ERR_TIMED_OUT) {
    return status;
  }
  if (observed & kErrorDetectedSignal) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t IntelI2cController::ClearStopDetect() {
  return event_handle_.signal(kStopDetectedSignal, 0);
}

// Perform a write to the DATA_CMD register, and clear
// interrupt masks as appropriate
zx_status_t IntelI2cController::IssueRx(const uint32_t data_cmd) {
  MmioWrite32(data_cmd, &regs_->data_cmd);
  return ZX_OK;
}

zx_status_t IntelI2cController::FlushRxFullIrq() {
  fbl::AutoLock lock(&irq_mask_mutex_);
  zx_status_t status = event_handle_.signal(kRxFullSignal, 0);
  RmwReg32(&regs_->intr_mask, kIntrRxFull, 1, 1);
  return status;
}

uint8_t IntelI2cController::ReadRx() { return static_cast<uint8_t>(MmioRead32(&regs_->data_cmd)); }

zx_status_t IntelI2cController::IssueTx(const uint32_t data_cmd) {
  MmioWrite32(data_cmd, &regs_->data_cmd);
  uint32_t tx_tl = GetTxFifoThreshold();
  const uint32_t txflr = MmioRead32(&regs_->txflr) & 0x1ff;
  // If we've raised the TX queue level above the threshold, clear the signal
  // and unmask the interrupt.
  if (txflr > tx_tl) {
    fbl::AutoLock lock(&irq_mask_mutex_);
    zx_status_t status = event_handle_.signal(kTxEmptySignal, 0);
    RmwReg32(&regs_->intr_mask, kIntrTxEmpty, 1, 1);
    return status;
  }
  return ZX_OK;
}

void IntelI2cController::Enable() { RmwReg32(&regs_->i2c_en, kI2cEnEnable, 1, 1); }

uint32_t IntelI2cController::GetRxFifoThreshold() { return (MmioRead32(&regs_->rx_tl) & 0xff) + 1; }

// Get an RX interrupt whenever the RX FIFO size is >= the threshold.
zx_status_t IntelI2cController::SetRxFifoThreshold(const uint32_t threshold) {
  if (threshold - 1 > UINT8_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }

  RmwReg32(&regs_->rx_tl, 0, 8, threshold - 1);
  return ZX_OK;
}

uint32_t IntelI2cController::GetRxFifoLevel() { return MmioRead32(&regs_->rxflr) & 0x1ff; }

bool IntelI2cController::IsRxFifoEmpty() {
  return !(MmioRead32(&regs_->i2c_sta) & (0x1 << kI2cStaRfne));
}

bool IntelI2cController::IsTxFifoFull() {
  return !(MmioRead32(&regs_->i2c_sta) & (0x1 << kI2cStaTfnf));
}

uint32_t IntelI2cController::GetTxFifoThreshold() { return (MmioRead32(&regs_->tx_tl) & 0xff) + 1; }

// Get a TX interrupt whenever the TX FIFO size is <= the threshold.
zx_status_t IntelI2cController::SetTxFifoThreshold(const uint32_t threshold) {
  if (threshold - 1 > UINT8_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }

  RmwReg32(&regs_->tx_tl, 0, 8, threshold - 1);
  return ZX_OK;
}

bool IntelI2cController::IsBusIdle() {
  uint32_t i2c_sta = MmioRead32(&regs_->i2c_sta);
  return !(i2c_sta & (0x1 << kI2cStaCa)) && (i2c_sta & (0x1 << kI2cStaTfce));
}

uint32_t IntelI2cController::StopDetected() {
  return (MmioRead32(&regs_->raw_intr_stat) & (0x1 << kIntrStopDetection));
}

void IntelI2cController::SetAddressingMode(const uint32_t addr_mode_bit) {
  RmwReg32(&regs_->ctl, kCtlAddressingMode, 1, addr_mode_bit);
}

void IntelI2cController::SetTargetAddress(const uint32_t addr_mode_bit, const uint32_t address) {
  MmioWrite32((addr_mode_bit << kTarAddWidth) | (address << kTarAddIcTar), &regs_->tar_add);
}

zx_status_t IntelI2cController::Reset() {
  zx_status_t status = ZX_OK;

  // The register will only return valid values if the ACPI _PS0 has been
  // evaluated.
  if (MmioRead32(reinterpret_cast<MMIO_PTR uint32_t*>(reinterpret_cast<MMIO_PTR char*>(regs_) +
                                                      kDevidleControl)) != 0xffffffff) {
    // Wake up device if it is in DevIdle state
    RmwReg32(reinterpret_cast<MMIO_PTR uint32_t*>(reinterpret_cast<MMIO_PTR char*>(regs_) +
                                                  kDevidleControl),
             kDevidleControlDevidle, 1, 0);

    // Wait for wakeup to finish processing
    int retry = 10;
    while (retry-- && (MmioRead32(reinterpret_cast<MMIO_PTR uint32_t*>(
                           reinterpret_cast<MMIO_PTR char*>(regs_) + kDevidleControl)) &
                       (1 << kDevidleControlCmdInProgress))) {
      usleep(10);
    }
    if (!retry) {
      zxlogf(ERROR, "i2c-controller: timed out waiting for device idle");
      return ZX_ERR_TIMED_OUT;
    }
  }

  // Reset the device.
  RmwReg32(soft_reset_, 0, 2, 0x0);
  RmwReg32(soft_reset_, 0, 2, 0x3);

  // Clear the "Restore Required" flag
  RmwReg32(reinterpret_cast<MMIO_PTR uint32_t*>(reinterpret_cast<MMIO_PTR char*>(regs_) +
                                                kDevidleControl),
           kDevidleControlRestoreRequired, 1, 0);

  // Disable the controller.
  RmwReg32(&regs_->i2c_en, kI2cEnEnable, 1, 0);

  // Reconfigure the bus timing
  if (bus_freq_ == kI2cMaxFastPlusSpeedHz) {
    RmwReg32(&regs_->fs_scl_hcnt, 0, 16, fmp_scl_hcnt_);
    RmwReg32(&regs_->fs_scl_lcnt, 0, 16, fmp_scl_lcnt_);
  } else {
    RmwReg32(&regs_->fs_scl_hcnt, 0, 16, fs_scl_hcnt_);
    RmwReg32(&regs_->fs_scl_lcnt, 0, 16, fs_scl_lcnt_);
  }
  RmwReg32(&regs_->ss_scl_hcnt, 0, 16, ss_scl_hcnt_);
  RmwReg32(&regs_->ss_scl_lcnt, 0, 16, ss_scl_lcnt_);
  switch (bus_freq_) {
    case kI2cMaxFastPlusSpeedHz:
      RmwReg32(&regs_->sda_hold, 0, 16, fmp_sda_hold_);
      break;
    case kI2cMaxFastSpeedHz:
      RmwReg32(&regs_->sda_hold, 0, 16, fs_sda_hold_);
      break;
    case kI2cMaxStandardSpeedHz:
      RmwReg32(&regs_->sda_hold, 0, 16, ss_sda_hold_);
      break;
  }

  uint32_t speed = kCtlSpeedStandard;
  if (bus_freq_ == kI2cMaxFastSpeedHz || bus_freq_ == kI2cMaxFastPlusSpeedHz) {
    speed = kCtlSpeedFast;
  }

  MmioWrite32((0x1 << kCtlSlaveDisable) | (0x1 << kCtlRestartEnable) | (speed << kCtlSpeed) |
                  (kCtlMasterModeEnabled << kCtlMasterMode),
              &regs_->ctl);

  fbl::AutoLock lock(&irq_mask_mutex_);
  // Mask all interrupts
  MmioWrite32(0, &regs_->intr_mask);

  if (MmioRead32(&regs_->comp_type) == kIntelDesignwareCompType) {
    uint32_t param = MmioRead32(&regs_->comp_param1);
    tx_fifo_depth_ = ExtractTxFifoDepthFromParam(param);
    rx_fifo_depth_ = ExtractRxFifoDepthFromParam(param);
  } else {
    tx_fifo_depth_ = 8;
    rx_fifo_depth_ = 8;
  }

  status = SetRxFifoThreshold(kDefaultRxFifoTriggerLevel);
  if (status != ZX_OK) {
    return status;
  }
  status = SetTxFifoThreshold(kDefaultTxFifoTriggerLevel);
  if (status != ZX_OK) {
    return status;
  }

  // Clear the signals
  status = event_handle_.signal(
      kRxFullSignal | kTxEmptySignal | kStopDetectedSignal | kErrorDetectedSignal, 0);
  if (status != ZX_OK) {
    return status;
  }

  // Reading this register clears all interrupts.
  MmioRead32(&regs_->clr_intr);

  // Unmask the interrupts we care about
  MmioWrite32((1u << kIntrStopDetection) | (1u << kIntrTxAbort) | (1u << kIntrTxEmpty) |
                  (1u << kIntrTxOver) | (1u << kIntrRxFull) | (1u << kIntrRxOver) |
                  (1u << kIntrRxUnder),
              &regs_->intr_mask);

  return status;
}

zx_status_t IntelI2cController::DeviceSpecificInit(const uint16_t device_id) {
  static const struct {
    uint16_t device_ids[16];
    // Offset of the soft reset register
    size_t reset_offset;
    // Internal controller frequency, in hertz
    uint32_t controller_clock_frequency;
  } dev_props[] = {
      {
          .device_ids =
              {
                  INTEL_SUNRISE_POINT_SERIALIO_I2C0_DID,
                  INTEL_SUNRISE_POINT_SERIALIO_I2C1_DID,
                  INTEL_SUNRISE_POINT_SERIALIO_I2C2_DID,
                  INTEL_SUNRISE_POINT_SERIALIO_I2C3_DID,
                  INTEL_SUNRISE_POINT_SERIALIO_I2C4_DID,
              },
          .reset_offset = 0x204,
          .controller_clock_frequency = 120 * 1000 * 1000,
      },
      {
          .device_ids =
              {
                  INTEL_WILDCAT_POINT_SERIALIO_I2C0_DID,
                  INTEL_WILDCAT_POINT_SERIALIO_I2C1_DID,
              },
          .reset_offset = 0x804,
          .controller_clock_frequency = 100 * 1000 * 1000,
      },
  };

  for (unsigned int i = 0; i < std::size(dev_props); ++i) {
    const unsigned int num_dev_ids = std::size(dev_props[0].device_ids);
    for (unsigned int dev_idx = 0; dev_idx < num_dev_ids; ++dev_idx) {
      if (!dev_props[i].device_ids[dev_idx]) {
        break;
      }
      if (dev_props[i].device_ids[dev_idx] != device_id) {
        continue;
      }

      controller_freq_ = dev_props[i].controller_clock_frequency;
      soft_reset_ = reinterpret_cast<MMIO_PTR uint32_t*>(reinterpret_cast<MMIO_PTR char*>(regs_) +
                                                         dev_props[i].reset_offset);
      return ZX_OK;
    }
  }

  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t IntelI2cController::AddSubordinates() {
  // Try to fetch our metadata so that we know who is on the bus.
  size_t metadata_size;
  zx_status_t status = DdkGetMetadataSize(DEVICE_METADATA_I2C_CHANNELS, &metadata_size);

  if ((status == ZX_ERR_NOT_FOUND) || ((status == ZX_OK) && !metadata_size)) {
    // No metadata means that there are no devices on this bus.  For now, we do
    // nothing, but it might be a good idea to (someday) put the hardware into a
    // low power state if we can, and perhaps even unload the driver at that
    // point.
    zxlogf(INFO, "i2c: failed to fetch metadata (%s)", zx_status_get_string(status));
    return ZX_OK;
  }

  if (status != ZX_OK) {
    zxlogf(ERROR, "i2c: failed to fetch metadata size (status %d)", status);
    return status;
  }

  auto buffer_deleter = std::make_unique<uint8_t[]>(metadata_size);
  auto buffer = buffer_deleter.get();
  size_t actual;
  status = DdkGetMetadata(DEVICE_METADATA_I2C_CHANNELS, buffer, metadata_size, &actual);
  if (status != ZX_OK || actual != metadata_size) {
    zxlogf(ERROR, "%s: device_get_metadata failed %d", __func__, status);
    return (status == ZX_OK) ? ZX_ERR_INTERNAL : status;
  }

  auto decoded = fidl::InplaceUnpersist<fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata>(
      cpp20::span(buffer, metadata_size));
  if (!decoded.is_ok()) {
    zxlogf(ERROR, "%s: Failed to deserialize metadata.", __func__);
    return decoded.error_value().status();
  }

  fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata& metadata = *decoded.value();
  if (!metadata.has_channels()) {
    // One day we might put the bus in a lower power state.
    zxlogf(INFO, "%s: no channels supplied.", __func__);
    return ZX_OK;
  }

  uint32_t bus_speed = 0;
  for (auto const& child : metadata.channels()) {
    zxlogf(INFO, "i2c: got child bus_controller=%d ten_bit=%d address=0x%x bus_speed=%u, bus_id=%u",
           child.is_bus_controller(), child.is_ten_bit(), child.address(), child.bus_speed(),
           child.bus_id());

    if (bus_speed && bus_speed != child.bus_speed()) {
      zxlogf(ERROR, "i2c: cannot add devices with different bus speeds (%u, %u)", bus_speed,
             child.bus_speed());
    }
    if (!bus_speed) {
      SetBusFrequency(child.bus_speed());
      bus_speed = child.bus_speed();
    }
    AddSubordinate(child.is_ten_bit() ? kI2c10BitAddress : kI2c7BitAddress, child.address());
  }

  return ZX_OK;
}

void IntelI2cController::DdkUnbind(ddk::UnbindTxn txn) {
  zxlogf(INFO, "intel-i2c: unbind irq_handle %d irq_thread %lu", irq_handle_.get(), irq_thread_);

  irq_handle_.destroy();
  thrd_join(irq_thread_, nullptr);

  txn.Reply();
}

void IntelI2cController::DdkRelease() { delete this; }

static zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = IntelI2cController::Create;
  return ops;
}();

}  // namespace intel_i2c

ZIRCON_DRIVER(intel_i2c, intel_i2c::driver_ops, "zircon", "0.1");
