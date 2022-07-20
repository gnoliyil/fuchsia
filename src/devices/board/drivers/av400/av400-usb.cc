// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/usb/modeswitch/cpp/banjo.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <zircon/status.h>

#include <soc/aml-a5/a5-gpio.h>
#include <soc/aml-common/aml-registers.h>

#include "src/devices/board/drivers/av400/av400.h"

namespace av400 {

static const pbus_mmio_t usb_phy_mmios[] = {
    {
        .base = A5_USBCOMB_BASE,
        .length = A5_USBCOMB_LENGTH,
    },
    {
        .base = A5_USBPHY_BASE,
        .length = A5_USBPHY_LENGTH,
    },
    {
        .base = A5_SYS_CTRL_BASE,
        .length = A5_SYS_CTRL_LENGTH,
    },
};

static const pbus_irq_t usb_phy_irqs[] = {
    {
        .irq = A5_USB_IDDIG_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_bti_t usb_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_USB,
    },
};

// Static PLL configuration parameters.
static const uint32_t pll_settings[] = {
    0x09400414, 0x927e0000, 0xac5f49e5, 0xbe18, 0x7, 0x78000, 0xe0004, 0xe000c,
};

static const usb_mode_t dr_mode = USB_MODE_HOST;

static const pbus_metadata_t usb_phy_metadata[] = {
    {
        .type = DEVICE_METADATA_PRIVATE,
        .data_buffer = reinterpret_cast<const uint8_t*>(pll_settings),
        .data_size = sizeof(pll_settings),
    },
    {
        .type = DEVICE_METADATA_USB_MODE,
        .data_buffer = reinterpret_cast<const uint8_t*>(&dr_mode),
        .data_size = sizeof(dr_mode),
    },
};

static const pbus_dev_t usb_phy_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "aml-usb-crg-phy-v2";
  dev.vid = PDEV_VID_AMLOGIC;
  dev.did = PDEV_DID_AML_USB_CRG_PHY_V2;
  dev.mmio_list = usb_phy_mmios;
  dev.mmio_count = std::size(usb_phy_mmios);
  dev.irq_list = usb_phy_irqs;
  dev.irq_count = std::size(usb_phy_irqs);
  dev.bti_list = usb_btis;
  dev.bti_count = std::size(usb_btis);
  dev.metadata_list = usb_phy_metadata;
  dev.metadata_count = std::size(usb_phy_metadata);
  return dev;
}();

static const zx_bind_inst_t reset_register_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_REGISTERS),
    BI_MATCH_IF(EQ, BIND_REGISTER_ID, aml_registers::REGISTER_USB_PHY_V2_RESET),
};
static const device_fragment_part_t reset_register_fragment[] = {
    {std::size(reset_register_match), reset_register_match},
};
static const device_fragment_t usb_phy_fragments[] = {
    {"register-reset", std::size(reset_register_fragment), reset_register_fragment},
};

static const pbus_mmio_t xhci_mmios[] = {
    {
        .base = A5_USB_BASE,
        .length = A5_USB_LENGTH,
    },
};

static const pbus_irq_t xhci_irqs[] = {
    {
        .irq = A5_USB2DRD_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_dev_t xhci_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "xhci";
  dev.vid = PDEV_VID_GENERIC;
  dev.pid = PDEV_PID_GENERIC;
  dev.did = PDEV_DID_USB_XHCI;
  dev.mmio_list = xhci_mmios;
  dev.mmio_count = std::size(xhci_mmios);
  dev.irq_list = xhci_irqs;
  dev.irq_count = std::size(xhci_irqs);
  dev.bti_list = usb_btis;
  dev.bti_count = std::size(usb_btis);
  return dev;
}();

static const zx_bind_inst_t xhci_phy_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB_PHY),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_USB_XHCI_COMPOSITE),
};
static const device_fragment_part_t xhci_phy_fragment[] = {
    {std::size(xhci_phy_match), xhci_phy_match},
};
static const device_fragment_t xhci_fragments[] = {
    {"xhci-phy", std::size(xhci_phy_fragment), xhci_phy_fragment},
};

zx_status_t Av400::UsbInit() {
  // Power on USB.
  gpio_impl_.ConfigOut(A5_GPIOD(10), 1);

  // Create USB Phy Device
  zx_status_t status =
      pbus_.CompositeDeviceAdd(&usb_phy_dev, reinterpret_cast<uint64_t>(usb_phy_fragments),
                               std::size(usb_phy_fragments), nullptr);
  if (status != ZX_OK) {
    zxlogf(ERROR, "DeviceAdd(usb_phy) failed %s", zx_status_get_string(status));
    return status;
  }

  // Create XHCI device.
  status = pbus_.CompositeDeviceAdd(&xhci_dev, reinterpret_cast<uint64_t>(xhci_fragments),
                                    std::size(xhci_fragments), "xhci-phy");
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: CompositeDeviceAdd(xhci) failed %d", __func__, status);
    return status;
  }

  return ZX_OK;
}

}  // namespace av400
