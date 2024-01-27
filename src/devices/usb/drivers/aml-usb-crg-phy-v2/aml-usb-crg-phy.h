// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_USB_DRIVERS_AML_USB_CRG_PHY_V2_AML_USB_CRG_PHY_H_
#define SRC_DEVICES_USB_DRIVERS_AML_USB_CRG_PHY_V2_AML_USB_CRG_PHY_H_

#include <fidl/fuchsia.hardware.registers/cpp/wire.h>
#include <fuchsia/hardware/registers/cpp/banjo.h>
#include <fuchsia/hardware/usb/modeswitch/cpp/banjo.h>
#include <fuchsia/hardware/usb/phy/cpp/banjo.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/interrupt.h>
#include <threads.h>

#include <ddktl/device.h>
#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/mutex.h>
#include <soc/aml-common/aml-registers.h>

#include "src/devices/usb/drivers/aml-usb-crg-phy-v2/udc-device.h"
#include "src/devices/usb/drivers/aml-usb-crg-phy-v2/xhci-device.h"

namespace aml_usb_crg_phy {

class AmlUsbCrgPhy;
using AmlUsbCrgPhyType =
    ddk::Device<AmlUsbCrgPhy, ddk::Initializable, ddk::Unbindable, ddk::ChildPreReleaseable>;

// This is the main class for the platform bus driver.
class AmlUsbCrgPhy : public AmlUsbCrgPhyType,
                     public ddk::UsbPhyProtocol<AmlUsbCrgPhy, ddk::base_protocol> {
 public:
  // Public for testing.
  enum class UsbMode {
    UNKNOWN,
    HOST,
    PERIPHERAL,
  };

  explicit AmlUsbCrgPhy(zx_device_t* parent) : AmlUsbCrgPhyType(parent) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // USB PHY protocol implementation.
  void UsbPhyConnectStatusChanged(bool connected);

  // Device protocol implementation.
  void DdkInit(ddk::InitTxn txn);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkChildPreRelease(void* child_ctx);
  void DdkRelease();

  // Public for testing.
  UsbMode mode() {
    fbl::AutoLock lock(&lock_);
    return phy_mode_;
  }

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(AmlUsbCrgPhy);

  void InitPll(fdf::MmioBuffer* mmio);
  void CaliTrim(fdf::MmioBuffer* mmio);
  zx_status_t InitPhy();
  zx_status_t InitOtg();

  // Called when |SetMode| completes.
  using SetModeCompletion = fit::callback<void(void)>;
  void SetMode(UsbMode mode, SetModeCompletion completion) __TA_REQUIRES(lock_);

  zx_status_t AddXhciDevice() __TA_REQUIRES(lock_);
  void RemoveXhciDevice(SetModeCompletion completion) __TA_REQUIRES(lock_);
  zx_status_t AddUdcDevice() __TA_REQUIRES(lock_);
  void RemoveUdcDevice(SetModeCompletion completion) __TA_REQUIRES(lock_);

  zx_status_t Init();
  int IrqThread();

  ddk::PDev pdev_;
  fidl::WireSyncClient<fuchsia_hardware_registers::Device> reset_register_;
  std::optional<fdf::MmioBuffer> usbctrl_mmio_;
  std::optional<fdf::MmioBuffer> usbphy_mmio_;
  std::optional<fdf::MmioBuffer> sysctrl_mmio_;

  zx::interrupt irq_;
  thrd_t irq_thread_;
  std::atomic_bool irq_thread_created_ = false;

  fbl::Mutex lock_;

  // Synchronizes DdkInit with the IrqThread().
  fbl::ConditionVariable init_cond_;

  // Magic numbers for PLL from metadata
  uint32_t pll_settings_[8];

  // Device nodes for child devices. The resources pointed at are managed by the DDK.
  XhciDevice* xhci_device_ __TA_GUARDED(lock_) = nullptr;
  UdcDevice* udc_device_ __TA_GUARDED(lock_) = nullptr;

  UsbMode phy_mode_ __TA_GUARDED(lock_) = UsbMode::UNKNOWN;  // Physical USB mode.
  usb_mode_t dr_mode_ = USB_MODE_OTG;  // USB Controller Mode. Internal to Driver.
  bool udc_connected_ = false;

  // If set, indicates that the device has a pending SetMode which
  // will be completed once |DdkChildPreRelease| is called.
  SetModeCompletion set_mode_completion_ __TA_GUARDED(lock_);
};

}  // namespace aml_usb_crg_phy

#endif  // SRC_DEVICES_USB_DRIVERS_AML_USB_CRG_PHY_V2_AML_USB_CRG_PHY_H_
