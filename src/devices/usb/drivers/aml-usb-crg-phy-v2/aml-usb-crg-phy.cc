// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/usb/drivers/aml-usb-crg-phy-v2/aml-usb-crg-phy.h"

#include <assert.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/hw/reg.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fit/defer.h>
#include <lib/zx/time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/errors.h>

#include <cstdio>
#include <sstream>
#include <string>

#include <ddktl/fidl.h>
#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>

#include "src/devices/usb/drivers/aml-usb-crg-phy-v2/aml_usb_crg_phy_bind.h"
#include "src/devices/usb/drivers/aml-usb-crg-phy-v2/usb-phy-regs.h"

namespace aml_usb_crg_phy {

void AmlUsbCrgPhy::CaliTrim(fdf::MmioBuffer* mmio) {
  uint32_t value = 0;
  uint32_t cali, i;
  uint8_t cali_en;

  cali = sysctrl_mmio_->Read32(0x330);
  cali_en = (cali >> 12) & 0x1;
  cali = cali >> 8;

  if (cali_en) {
    cali = cali & 0xf;
    if (cali > 12) {
      cali = 12;
    }
  } else {
    cali = pll_settings_[4];
  }

  value = mmio->Read32(0x10);
  value &= (~0xfff);
  for (i = 0; i < cali; i++) {
    value |= (1 << i);
  }

  mmio->Write32(value, 0x10);
}

// Based on set_usb_pll() in phy-aml-crg-drd-usb2.c
void AmlUsbCrgPhy::InitPll(fdf::MmioBuffer* mmio) {
  PLL_REGISTER_40::Get()
      .FromValue(0)
      .set_value(pll_settings_[0])
      .set_enable(1)
      .set_reset(1)
      .WriteTo(mmio);

  PLL_REGISTER::Get(0x44).FromValue(pll_settings_[1]).WriteTo(mmio);

  PLL_REGISTER::Get(0x48).FromValue(pll_settings_[2]).WriteTo(mmio);

  zx::nanosleep(zx::deadline_after(zx::usec(100)));

  PLL_REGISTER_40::Get()
      .FromValue(0)
      .set_value(pll_settings_[0])
      .set_enable(1)
      .set_reset(0)
      .WriteTo(mmio);

  PLL_REGISTER::Get(0xc).FromValue(0x3c).WriteTo(mmio);
  // PLL

  zx::nanosleep(zx::deadline_after(zx::usec(100)));

  PLL_REGISTER::Get(0x50).FromValue(pll_settings_[3]).WriteTo(mmio);

  PLL_REGISTER::Get(0x54).FromValue(0x2a).WriteTo(mmio);

  PLL_REGISTER_38::Get().ReadFrom(mmio).set_threshold(0x2).WriteTo(mmio);

  PLL_REGISTER::Get(0x34).FromValue(0x78000).WriteTo(mmio);
}

zx_status_t AmlUsbCrgPhy::InitPhy() {
  auto* usbctrl_mmio = &*usbctrl_mmio_;

  // phy0-reset-level-bit = <8>
  auto level_result =
      reset_register_->WriteRegister32(RESET0_LEVEL_OFFSET, aml_registers::A5_USB_RESET0_LEVEL_MASK,
                                       aml_registers::A5_USB_RESET0_LEVEL_MASK);
  if ((level_result.status() != ZX_OK) || level_result->is_error()) {
    zxlogf(ERROR, "%s: Reset Level Write failed\n", __func__);
    return ZX_ERR_INTERNAL;
  }

  // usb-reset-bit = <4>
  auto register_result1 = reset_register_->WriteRegister32(
      RESET0_REGISTER_OFFSET, aml_registers::A5_USB_RESET0_MASK, aml_registers::A5_USB_RESET0_MASK);
  if ((register_result1.status() != ZX_OK) || register_result1->is_error()) {
    zxlogf(ERROR, "%s: Reset Register Write on 1 << 2 failed\n", __func__);
    return ZX_ERR_INTERNAL;
  }

  // only one port in the phy
  U2P_R0_V2::Get(0)
      .ReadFrom(usbctrl_mmio)
      .set_por(1)
      .set_idpullup0(1)
      .set_drvvbus0(1)
      .set_host_device((dr_mode_ == USB_MODE_PERIPHERAL) ? 0 : 1)
      .WriteTo(usbctrl_mmio);

  zx::nanosleep(zx::deadline_after(zx::usec(10)));

  // amlogic_crg_drd_usbphy_reset_phycfg()
  auto register_result2 =
      reset_register_->WriteRegister32(RESET0_LEVEL_OFFSET, aml_registers::A5_USB_RESET0_LEVEL_MASK,
                                       aml_registers::A5_USB_RESET0_LEVEL_MASK);
  if ((register_result2.status() != ZX_OK) || register_result2->is_error()) {
    zxlogf(ERROR, "%s: Reset Register Write on 1 << 16 failed\n", __func__);
    return ZX_ERR_INTERNAL;
  }

  zx::nanosleep(zx::deadline_after(zx::usec(50)));

  auto u2p_r1 = U2P_R1_V2::Get(0);

  auto* usbphy_mmio = &*usbphy_mmio_;
  CaliTrim(usbphy_mmio);

  int count = 0;
  while (!u2p_r1.ReadFrom(usbctrl_mmio).phy_rdy()) {
    // wait phy ready max 5ms, common is 100us
    if (count > 1000) {
      zxlogf(WARNING, "AmlUsbCrgPhy::InitPhy U2P_R1_PHY_RDY wait failed");
      break;
    }

    count++;
    zx::nanosleep(zx::deadline_after(zx::usec(5)));
  }

  return ZX_OK;
}

zx_status_t AmlUsbCrgPhy::InitOtg() {
  auto* mmio = &*usbctrl_mmio_;

  USB_R1_V2::Get().ReadFrom(mmio).set_u3h_fladj_30mhz_reg(0x20).WriteTo(mmio);

  USB_R5_V2::Get().ReadFrom(mmio).set_iddig_en0(1).set_iddig_en1(1).set_iddig_th(255).WriteTo(mmio);

  return ZX_OK;
}

void AmlUsbCrgPhy::SetMode(UsbMode mode, SetModeCompletion completion) {
  ZX_DEBUG_ASSERT(mode == UsbMode::HOST || mode == UsbMode::PERIPHERAL);
  // Only the irq thread calls |SetMode|, and it should have waited for the
  // previous call to |SetMode| to complete.
  ZX_DEBUG_ASSERT(!set_mode_completion_);
  auto cleanup = fit::defer([&]() {
    if (completion)
      completion();
    // If the DdkInit thread is waiting, unblock it.
    init_cond_.Signal();
  });

  if (mode == phy_mode_)
    return;

  auto* usbctrl_mmio = &*usbctrl_mmio_;

  auto r0 = USB_R0_V2::Get().ReadFrom(usbctrl_mmio);
  if (mode == UsbMode::HOST) {
    r0.set_u2d_act(0);
  } else {
    r0.set_u2d_act(1);
    r0.set_u2d_ss_scaledown_mode(0);
  }
  r0.WriteTo(usbctrl_mmio);

  USB_R4_V2::Get()
      .ReadFrom(usbctrl_mmio)
      .set_p21_sleepm0(mode == UsbMode::PERIPHERAL)
      .WriteTo(usbctrl_mmio);

  U2P_R0_V2::Get(0)
      .ReadFrom(usbctrl_mmio)
      .set_host_device(mode == UsbMode::HOST)
      .set_por(0)
      .WriteTo(usbctrl_mmio);

  zx::nanosleep(zx::deadline_after(zx::usec(500)));

  auto old_mode = phy_mode_;
  phy_mode_ = mode;

  if (old_mode == UsbMode::UNKNOWN) {
    // One time PLL initialization
    InitPll(&*usbphy_mmio_);
  } else {
    auto* phy_mmio = &*usbphy_mmio_;

    PLL_REGISTER::Get(0x38)
        .FromValue(mode == UsbMode::HOST ? pll_settings_[6] : 0)
        .WriteTo(phy_mmio);
    PLL_REGISTER::Get(0x34).FromValue(pll_settings_[5]).WriteTo(phy_mmio);
  }

  if (mode == UsbMode::HOST) {
    AddXhciDevice();
    RemoveUdcDevice(std::move(completion));
  } else {
    AddUdcDevice();
    RemoveXhciDevice(std::move(completion));
  }
}

int AmlUsbCrgPhy::IrqThread() {
  auto* mmio = &*usbctrl_mmio_;

  // Wait for PHY to stabilize before reading initial mode.
  zx::nanosleep(zx::deadline_after(zx::sec(1)));

  lock_.Acquire();

  while (true) {
    auto r5 = USB_R5_V2::Get().ReadFrom(mmio);

    // Since |SetMode| is asynchronous, we need to block until it completes.
    sync_completion_t set_mode_sync;
    auto completion = [&]() { sync_completion_signal(&set_mode_sync); };
    // Read current host/device role.
    if (r5.iddig_curr() == 0) {
      zxlogf(INFO, "Entering USB Host Mode");
      SetMode(UsbMode::HOST, std::move(completion));
    } else {
      zxlogf(INFO, "Entering USB Peripheral Mode");
      SetMode(UsbMode::PERIPHERAL, std::move(completion));
    }

    lock_.Release();
    sync_completion_wait(&set_mode_sync, ZX_TIME_INFINITE);
    auto status = irq_.wait(nullptr);
    if (status == ZX_ERR_CANCELED) {
      return 0;
    }
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: irq_.wait failed: %s", __func__, zx_status_get_string(status));
      return -1;
    }
    lock_.Acquire();

    // Acknowledge interrupt
    r5.ReadFrom(mmio).set_usb_iddig_irq(0).WriteTo(mmio);
  }

  lock_.Release();

  return 0;
}

zx_status_t AmlUsbCrgPhy::Create(void* ctx, zx_device_t* parent) {
  auto dev = std::make_unique<AmlUsbCrgPhy>(parent);

  auto status = dev->Init();
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* _ = dev.release();
  return ZX_OK;
}

zx_status_t AmlUsbCrgPhy::AddXhciDevice() {
  if (xhci_device_) {
    return ZX_ERR_BAD_STATE;
  }

  xhci_device_ = new XhciDevice(zxdev());

  zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_GENERIC},
      {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GENERIC},
      {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_USB_XHCI_COMPOSITE},
  };

  return xhci_device_->DdkAdd(
      ddk::DeviceAddArgs("xhci").set_props(props).set_proto_id(ZX_PROTOCOL_USB_PHY));
}

void AmlUsbCrgPhy::RemoveXhciDevice(SetModeCompletion completion) {
  auto cleanup = fit::defer([&]() {
    if (completion)
      completion();
  });
  if (xhci_device_) {
    // The callback will be run by the ChildPreRelease hook once the xhci device has been removed.
    set_mode_completion_ = std::move(completion);
    xhci_device_->DdkAsyncRemove();
  }
}

zx_status_t AmlUsbCrgPhy::AddUdcDevice() {
  if (udc_device_) {
    return ZX_ERR_BAD_STATE;
  }

  udc_device_ = new UdcDevice(zxdev());

  zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_AMLOGIC},
      {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GENERIC},
      {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_USB_CRG_UDC},
  };

  return udc_device_->DdkAdd(
      ddk::DeviceAddArgs("udc").set_props(props).set_proto_id(ZX_PROTOCOL_USB_PHY));
}

void AmlUsbCrgPhy::RemoveUdcDevice(SetModeCompletion completion) {
  auto cleanup = fit::defer([&]() {
    if (completion)
      completion();
  });
  if (udc_device_) {
    // The callback will be run by the ChildPreRelease hook once the udc device has been removed.
    set_mode_completion_ = std::move(completion);
    udc_device_->DdkAsyncRemove();
  }
}

zx_status_t AmlUsbCrgPhy::Init() {
  zx_status_t status = ZX_OK;
  pdev_ = ddk::PDev::FromFragment(parent());
  if (!pdev_.is_valid()) {
    zxlogf(ERROR, "AmlUsbCrgPhy::Init: could not get platform device protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }

  ddk::RegistersProtocolClient reset_register(parent(), "register-reset");
  if (!reset_register.is_valid()) {
    zxlogf(ERROR, "%s: could not get reset_register fragment", __func__);
    return ZX_ERR_NO_RESOURCES;
  }
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_registers::Device>();
  if (endpoints.is_error()) {
    zxlogf(ERROR, "%s: could not create channel %s\n", __func__, endpoints.status_string());
    return status;
  }
  auto& [register_client_end, register_server_end] = endpoints.value();
  reset_register.Connect(register_server_end.TakeChannel());
  reset_register_.Bind(std::move(register_client_end));

  size_t actual;
  status = DdkGetMetadata(DEVICE_METADATA_PRIVATE, pll_settings_, sizeof(pll_settings_), &actual);
  if (status != ZX_OK || actual != sizeof(pll_settings_)) {
    zxlogf(ERROR, "AmlUsbCrgPhy::Init could not get metadata for PLL settings");
    return ZX_ERR_INTERNAL;
  }
  status = DdkGetMetadata(DEVICE_METADATA_USB_MODE, &dr_mode_, sizeof(dr_mode_), &actual);
  if (status == ZX_OK && actual != sizeof(dr_mode_)) {
    zxlogf(ERROR, "AmlUsbCrgPhy::Init could not get metadata for USB Mode");
    return ZX_ERR_INTERNAL;
  }
  if (status != ZX_OK) {
    dr_mode_ = USB_MODE_OTG;
  }

  status = pdev_.MapMmio(0, &usbctrl_mmio_);
  if (status != ZX_OK) {
    return status;
  }
  status = pdev_.MapMmio(1, &usbphy_mmio_);
  if (status != ZX_OK) {
    return status;
  }

  status = pdev_.MapMmio(2, &sysctrl_mmio_);
  if (status != ZX_OK) {
    return status;
  }

  status = pdev_.GetInterrupt(0, &irq_);
  if (status != ZX_OK) {
    return status;
  }

  status = InitPhy();
  if (status != ZX_OK) {
    return status;
  }
  status = InitOtg();
  if (status != ZX_OK) {
    return status;
  }

  return DdkAdd("aml-usb-crg-phy-v2", DEVICE_ADD_NON_BINDABLE);
}

void AmlUsbCrgPhy::DdkInit(ddk::InitTxn txn) {
  if (dr_mode_ != USB_MODE_OTG) {
    sync_completion_t set_mode_sync;
    auto completion = [&]() { sync_completion_signal(&set_mode_sync); };
    fbl::AutoLock lock(&lock_);
    if (dr_mode_ == USB_MODE_PERIPHERAL) {
      zxlogf(INFO, "Entering USB Peripheral Mode");
      SetMode(UsbMode::PERIPHERAL, std::move(completion));
    } else {
      zxlogf(INFO, "Entering USB Host Mode");
      SetMode(UsbMode::HOST, std::move(completion));
    }
    sync_completion_wait(&set_mode_sync, ZX_TIME_INFINITE);

    return txn.Reply(ZX_OK);
  }

  irq_thread_created_ = true;
  {
    fbl::AutoLock lock(&lock_);
    int rc = thrd_create_with_name(
        &irq_thread_,
        [](void* arg) -> int { return reinterpret_cast<AmlUsbCrgPhy*>(arg)->IrqThread(); },
        reinterpret_cast<void*>(this), "amlogic-usb-thread");
    if (rc != thrd_success) {
      irq_thread_created_ = false;
      lock.release();
      return txn.Reply(ZX_ERR_INTERNAL);  // This will schedule the device to be unbound.
    }
    // Wait until the IrqThread sets the given mode.
    init_cond_.Wait(&lock_);
  }
  return txn.Reply(ZX_OK);
}

// PHY tuning based on connection state
void AmlUsbCrgPhy::UsbPhyConnectStatusChanged(bool connected) {
  fbl::AutoLock lock(&lock_);

  if (udc_connected_ == connected)
    return;

  udc_connected_ = connected;
}

void AmlUsbCrgPhy::DdkUnbind(ddk::UnbindTxn txn) {
  irq_.destroy();
  if (irq_thread_created_) {
    thrd_join(irq_thread_, nullptr);
  }
  txn.Reply();
}

void AmlUsbCrgPhy::DdkChildPreRelease(void* child_ctx) {
  fbl::AutoLock lock(&lock_);
  // devmgr will own the device until it is destroyed.
  if (xhci_device_ && (child_ctx == xhci_device_)) {
    xhci_device_ = nullptr;
  } else if (udc_device_ && (child_ctx == udc_device_)) {
    udc_device_ = nullptr;
  } else {
    zxlogf(ERROR, "AmlUsbCrgPhy::DdkChildPreRelease unexpected child ctx %p", child_ctx);
  }
  if (set_mode_completion_) {
    // If the mode is currently being set, the irq thread will be blocked
    // until we call this completion.
    set_mode_completion_();
  }
}

void AmlUsbCrgPhy::DdkRelease() { delete this; }

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AmlUsbCrgPhy::Create;
  return ops;
}();

}  // namespace aml_usb_crg_phy

ZIRCON_DRIVER(aml_usb_crg_phy, aml_usb_crg_phy::driver_ops, "zircon", "0.1");
