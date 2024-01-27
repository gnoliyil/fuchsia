// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-ethernet.h"

#include <fuchsia/hardware/ethernet/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/i2c-channel.h>
#include <stdio.h>
#include <string.h>
#include <zircon/compiler.h>

#include <iterator>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <soc/aml-s912/s912-hw.h>

#include "aml-regs.h"
#include "src/connectivity/ethernet/drivers/aml-ethernet/aml_eth-bind.h"

namespace eth {

#define MCU_I2C_REG_BOOT_EN_WOL 0x21
#define MCU_I2C_REG_BOOT_EN_WOL_RESET_ENABLE 0x03

zx_status_t AmlEthernet::EthBoardResetPhy() {
  if (gpios_[PHY_RESET].is_valid()) {
    gpios_[PHY_RESET].Write(0);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(100)));
    gpios_[PHY_RESET].Write(1);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(100)));
  }
  return ZX_OK;
}

zx_status_t AmlEthernet::InitPdev() {
  pdev_ = ddk::PDevFidl::FromFragment(parent());
  if (!pdev_.is_valid()) {
    zxlogf(ERROR, "Could not get PDEV protocol");
    return ZX_ERR_NO_RESOURCES;
  }

  // Not needed on vim3.
  i2c_ = ddk::I2cChannel(parent(), "i2c");

  // Reset is optional.
  gpios_[PHY_RESET] = ddk::GpioProtocolClient(parent(), "gpio-reset");

  gpios_[PHY_INTR] = ddk::GpioProtocolClient(parent(), "gpio-int");
  if (!gpios_[PHY_INTR].is_valid()) {
    zxlogf(ERROR, "Could not get GPIO protocol");
    return ZX_ERR_NO_RESOURCES;
  }

  // Map amlogic peripheral control registers.
  zx_status_t status = pdev_.MapMmio(MMIO_PERIPH, &periph_mmio_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-dwmac: could not map periph mmio: %d", status);
    return status;
  }

  // Map HHI regs (clocks and power domains).
  status = pdev_.MapMmio(MMIO_HHI, &hhi_mmio_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-dwmac: could not map hiu mmio: %d", status);
    return status;
  }

  return status;
}

zx_status_t AmlEthernet::Bind() {
  // Set reset line to output if implemented
  if (gpios_[PHY_RESET].is_valid()) {
    gpios_[PHY_RESET].ConfigOut(0);
  }

  pdev_board_info_t board;
  bool is_vim3 = false;
  zx_status_t status = pdev_.GetBoardInfo(&board);

  if (status != ZX_OK || board.pid != PDEV_PID_AV400) {
    // Initialize AMLogic peripheral registers associated with dwmac.
    // Sorry about the magic...rtfm
    periph_mmio_->Write32(0x1621, PER_ETH_REG0);
    if (status == ZX_OK) {
      is_vim3 = ((board.vid == PDEV_VID_KHADAS) && (board.pid == PDEV_PID_VIM3));
    }

    if (!is_vim3) {
      periph_mmio_->Write32(0x20000, PER_ETH_REG1);
    }

    periph_mmio_->Write32(REG2_ETH_REG2_REVERSED | REG2_INTERNAL_PHY_ID, PER_ETH_REG2);

    periph_mmio_->Write32(REG3_CLK_IN_EN | REG3_ETH_REG3_19_RESVERD | REG3_CFG_PHY_ADDR |
                              REG3_CFG_MODE | REG3_CFG_EN_HIGH | REG3_ETH_REG3_2_RESERVED,
                          PER_ETH_REG3);

    // Enable clocks and power domain for dwmac
    hhi_mmio_->SetBits32(1 << 3, HHI_GCLK_MPEG1);
    hhi_mmio_->ClearBits32((1 << 3) | (1 << 2), HHI_MEM_PD_REG0);
  }

  if (i2c_.is_valid()) {
    // WOL reset enable to MCU
    uint8_t write_buf[2] = {MCU_I2C_REG_BOOT_EN_WOL, MCU_I2C_REG_BOOT_EN_WOL_RESET_ENABLE};
    status = i2c_.WriteSync(write_buf, sizeof(write_buf));
    if (status) {
      zxlogf(ERROR, "aml-ethernet: WOL reset enable to MCU failed: %d", status);
      return status;
    }
  }

  // Populate board specific information
  eth_dev_metadata_t mac_info;
  size_t actual;
  status = device_get_metadata(parent(), DEVICE_METADATA_ETH_MAC_DEVICE, &mac_info,
                               sizeof(eth_dev_metadata_t), &actual);
  if (status != ZX_OK || actual != sizeof(eth_dev_metadata_t)) {
    zxlogf(ERROR, "aml-ethernet: Could not get MAC metadata %d", status);
    return status;
  }

  zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, mac_info.vid},
      {BIND_PLATFORM_DEV_DID, 0, mac_info.did},
  };

  return DdkAdd(ddk::DeviceAddArgs("aml-ethernet").set_props(props));
}

void AmlEthernet::DdkRelease() { delete this; }

zx_status_t AmlEthernet::Create(void* ctx, zx_device_t* parent) {
  zxlogf(INFO, "aml-ethernet: adding driver");
  fbl::AllocChecker ac;
  auto eth_device = fbl::make_unique_checked<AmlEthernet>(&ac, parent);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = eth_device->InitPdev();
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-ethernet: failed to init platform device");
    return status;
  }

  status = eth_device->Bind();
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-ethernet driver failed to get added: %d", status);
    return status;
  } else {
    zxlogf(INFO, "aml-ethernet driver added");
  }

  // eth_device intentionally leaked as it is now held by DevMgr
  [[maybe_unused]] auto ptr = eth_device.release();

  return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AmlEthernet::Create;
  return ops;
}();

}  // namespace eth

// clang-format off
ZIRCON_DRIVER(aml_eth, eth::driver_ops, "aml-ethernet", "0.1");
