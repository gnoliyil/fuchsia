// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <lib/zircon-internal/align.h>
#include <zircon/status.h>

#include <ddk/usb-peripheral-config.h>
#include <soc/aml-a5/a5-gpio.h>
#include <soc/aml-common/aml-registers.h>
#include <usb/cdc.h>
#include <usb/dwc2/metadata.h>
#include <usb/peripheral-config.h>
#include <usb/peripheral.h>
#include <usb/usb.h>

#include "src/devices/board/drivers/av400/av400.h"
#include "src/devices/board/drivers/av400/udc-phy-bind.h"
#include "src/devices/board/drivers/av400/usb-phy-bind.h"
#include "src/devices/board/drivers/av400/xhci-bind.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace av400 {
namespace fpbus = fuchsia_hardware_platform_bus;

static const std::vector<fpbus::Mmio> usb_phy_mmios{
    {{
        .base = A5_USBCOMB_BASE,
        .length = A5_USBCOMB_LENGTH,
    }},
    {{
        .base = A5_USBPHY_BASE,
        .length = A5_USBPHY_LENGTH,
    }},
    {{
        .base = A5_SYS_CTRL_BASE,
        .length = A5_SYS_CTRL_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> usb_phy_irqs{
    {{
        .irq = A5_USB_IDDIG_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
};

static const std::vector<fpbus::Bti> usb_btis{
    {{
        .iommu_index = 0,
        .bti_id = BTI_USB,
    }},
};

// Static PLL configuration parameters.
static const uint32_t pll_settings[] = {
    0x09400414, 0x927e0000, 0xac5f49e5, 0xbe18, 0x7, 0x78000, 0xe0004, 0xe000c,
};

static const usb_mode_t dr_mode = USB_MODE_PERIPHERAL;

static const std::vector<fpbus::Metadata> usb_phy_metadata{
    {{
        .type = DEVICE_METADATA_PRIVATE,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&pll_settings),
            reinterpret_cast<const uint8_t*>(&pll_settings) + sizeof(pll_settings)),
    }},
    {{
        .type = DEVICE_METADATA_USB_MODE,
        .data = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&dr_mode),
                                     reinterpret_cast<const uint8_t*>(&dr_mode) + sizeof(dr_mode)),
    }},
};

static const fpbus::Node usb_phy_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "aml-usb-crg-phy-v2";
  dev.vid() = PDEV_VID_AMLOGIC;
  dev.did() = PDEV_DID_AML_USB_CRG_PHY_V2;
  dev.mmio() = usb_phy_mmios;
  dev.irq() = usb_phy_irqs;
  dev.bti() = usb_btis;
  dev.metadata() = usb_phy_metadata;
  return dev;
}();

static const std::vector<fpbus::Mmio> xhci_mmios{
    {{
        .base = A5_USB_BASE,
        .length = A5_USB_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> xhci_irqs{
    {{
        .irq = A5_USB2DRD_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
};

static const fpbus::Node xhci_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "xhci";
  dev.vid() = PDEV_VID_GENERIC;
  dev.pid() = PDEV_PID_GENERIC;
  dev.did() = PDEV_DID_USB_XHCI_COMPOSITE;
  dev.mmio() = xhci_mmios;
  dev.irq() = xhci_irqs;
  dev.bti() = usb_btis;
  return dev;
}();

static const std::vector<fpbus::Mmio> udc_mmios{
    {{
        .base = A5_USB_BASE,
        .length = A5_USB_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> udc_irqs{
    {{
        .irq = A5_USB2DRD_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
};

static const std::vector<fpbus::Bti> udc_btis{
    {{
        .iommu_index = 0,
        .bti_id = BTI_USB,
    }},
};

// Metadata for UDC driver.
static const dwc2_metadata_t udc_metadata = {
    .dma_burst_len = DWC2_DMA_BURST_INCR8,
    .usb_turnaround_time = 9,
    .rx_fifo_size = 256,   // for all OUT endpoints.
    .nptx_fifo_size = 32,  // for endpoint zero IN direction.
    .tx_fifo_sizes =
        {
            128,  // for CDC ethernet bulk IN.
            4,    // for CDC ethernet interrupt IN.
            128,  // for test function bulk IN.
            16,   // for test function interrupt IN.
        },
};

using FunctionDescriptor = fuchsia_hardware_usb_peripheral::wire::FunctionDescriptor;

static std::vector<fpbus::Metadata> usb_metadata{
    {{
        .type = DEVICE_METADATA_USB_CONFIG,
        // No metadata for this item.
    }},
    {{
        .type = DEVICE_METADATA_PRIVATE,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&udc_metadata),
            reinterpret_cast<const uint8_t*>(&udc_metadata) + sizeof(udc_metadata)),
    }},
};

static const std::vector<fpbus::BootMetadata> usb_boot_metadata{
    {{
        .zbi_type = DEVICE_METADATA_MAC_ADDRESS,
        .zbi_extra = MACADDR_WIFI,
    }},
};

zx_status_t Av400::UsbInit() {
  // Power on USB.
  // Force to device mode, use external power
  gpio_init_steps_.push_back(
      {A5_GPIOD(10), fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(gpio_init_arena_)
                         .output_value(0)
                         .Build()});
  // Create USB Phy Device
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('USB_');
  auto result = pbus_.buffer(arena)->AddComposite(
      fidl::ToWire(fidl_arena, usb_phy_dev),
      platform_bus_composite::MakeFidlFragment(fidl_arena, usb_phy_fragments,
                                               std::size(usb_phy_fragments)),
      "pdev");
  if (!result.ok()) {
    zxlogf(ERROR, "AddComposite Usb(usb_phy_dev) request failed: %s",
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "AddComposite Usb(usb_phy_dev) failed: %s",
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  // Create XHCI device.
  result =
      pbus_.buffer(arena)->AddComposite(fidl::ToWire(fidl_arena, xhci_dev),
                                        platform_bus_composite::MakeFidlFragment(
                                            fidl_arena, xhci_fragments, std::size(xhci_fragments)),
                                        "xhci-phy");
  if (!result.ok()) {
    zxlogf(ERROR, "AddComposite Usb(xhci_dev) request failed: %s",
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "AddComposite Usb(xhci_dev) failed: %s",
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  // Create UDC Device
  std::unique_ptr<usb::UsbPeripheralConfig> peripheral_config;
  auto status = usb::UsbPeripheralConfig::CreateFromBootArgs(parent_, &peripheral_config);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to get usb config from boot args - %d", status);
    return status;
  }
  usb_metadata[0].data() = peripheral_config->config_data();

  const fpbus::Node udc_dev = []() {
    fpbus::Node dev = {};
    dev.name() = "udc";
    dev.vid() = PDEV_VID_AMLOGIC;
    dev.pid() = PDEV_PID_GENERIC;
    dev.did() = PDEV_DID_USB_CRG_UDC;
    dev.mmio() = udc_mmios;
    dev.irq() = udc_irqs;
    dev.bti() = udc_btis;
    dev.metadata() = usb_metadata;
    dev.boot_metadata() = usb_boot_metadata;
    return dev;
  }();

  result = pbus_.buffer(arena)->AddComposite(
      fidl::ToWire(fidl_arena, udc_dev),
      platform_bus_composite::MakeFidlFragment(fidl_arena, udc_fragments, std::size(udc_fragments)),
      "udc-phy");
  if (!result.ok()) {
    zxlogf(ERROR, "AddComposite Usb(udc_dev) request failed: %s",
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "AddComposite Usb(udc_dev) failed: %s",
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace av400
