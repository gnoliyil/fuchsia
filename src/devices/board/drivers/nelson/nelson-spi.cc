// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/time.h>

#include <fbl/algorithm.h>
#include <soc/aml-common/aml-registers.h>
#include <soc/aml-common/aml-spi.h>
#include <soc/aml-s905d2/s905d2-gpio.h>
#include <soc/aml-s905d3/s905d3-hw.h>

#include "nelson-gpios.h"
#include "nelson.h"
#include "src/devices/board/drivers/nelson/spi_0_bind.h"
#include "src/devices/board/drivers/nelson/spi_1_bind.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"
#include "src/devices/lib/fidl-metadata/spi.h"

#define HHI_SPICC_CLK_CNTL (0xf7 * 4)

#define spicc0_clk_sel_fclk_div4 (2 << 7)
#define spicc0_clk_en (1 << 6)
#define spicc0_clk_div(x) ((x)-1)

#define spicc1_clk_sel_fclk_div3 (3 << 23)
#define spicc1_clk_en (1 << 22)
#define spicc1_clk_div(x) (((x)-1) << 16)

namespace nelson {
namespace fpbus = fuchsia_hardware_platform_bus;
using spi_channel_t = fidl_metadata::spi::Channel;

zx_status_t Nelson::SpiInit() {
  constexpr uint32_t kSpiccClkValue =
      // SPICC0 clock enable (500 MHz)
      spicc0_clk_sel_fclk_div4 | spicc0_clk_en | spicc0_clk_div(1) |

      // SPICC1 clock enable (666 MHz)
      spicc1_clk_sel_fclk_div3 | spicc1_clk_en | spicc1_clk_div(1);

  // TODO(fxbug.dev/34010): fix this clock enable block when the clock driver can handle the
  // dividers
  {
    // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
    zx::unowned_resource resource(get_root_resource());
    std::optional<fdf::MmioBuffer> buf;
    zx_status_t status = fdf::MmioBuffer::Create(S905D3_HIU_BASE, S905D3_HIU_LENGTH, *resource,
                                                 ZX_CACHE_POLICY_UNCACHED_DEVICE, &buf);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: MmioBuffer::Create failed %d", __func__, status);
      return status;
    }

    buf->Write32(kSpiccClkValue, HHI_SPICC_CLK_CNTL);
  }

  zx_status_t status0 = Spi0Init();
  zx_status_t status1 = Spi1Init();
  return status0 == ZX_OK ? status1 : status0;
}

zx_status_t Nelson::Spi0Init() {
  static const std::vector<fpbus::Mmio> spi_0_mmios{
      {{
          .base = S905D3_SPICC0_BASE,
          .length = S905D3_SPICC0_LENGTH,
      }},
  };

  static const std::vector<fpbus::Irq> spi_0_irqs{
      {{
          .irq = S905D3_SPICC0_IRQ,
          .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
      }},
  };

  static const spi_channel_t spi_0_channels[] = {
      {
          .bus_id = NELSON_SPICC0,
          .cs = 0,  // index into matching chip-select map
          .vid = PDEV_VID_NORDIC,
          .pid = PDEV_PID_NORDIC_NRF52811,
          .did = PDEV_DID_NORDIC_THREAD,
      },
  };

  static const amlogic_spi::amlspi_config_t spi_0_config = {
      .bus_id = NELSON_SPICC0,
      .cs_count = 1,
      .cs = {0},                                       // index into fragments list
      .clock_divider_register_value = (500 >> 1) - 1,  // SCLK = core clock / 500 = 1.0 MHz
      .use_enhanced_clock_mode = true,
  };

  fpbus::Node spi_0_dev;
  spi_0_dev.name() = "spi-0";
  spi_0_dev.vid() = PDEV_VID_AMLOGIC;
  spi_0_dev.pid() = PDEV_PID_GENERIC;
  spi_0_dev.did() = PDEV_DID_AMLOGIC_SPI;
  spi_0_dev.instance_id() = 0;
  spi_0_dev.mmio() = spi_0_mmios;
  spi_0_dev.irq() = spi_0_irqs;

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_A_MOSI, 5);  // MOSI
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_A_MOSI, 2500, nullptr);

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_A_MISO, 5);  // MISO
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_A_MISO, 2500, nullptr);

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_A_SS0, 0);
  gpio_impl_.ConfigOut(GPIO_SOC_SPI_A_SS0, 1);  // SS0

  // SCLK must be pulled down to prevent SPI bit errors.
  gpio_impl_.ConfigIn(GPIO_SOC_SPI_A_SCLK, GPIO_PULL_DOWN);
  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_A_SCLK, 5);  // SCLK
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_A_SCLK, 2500, nullptr);

  std::vector<fpbus::Metadata> spi_0_metadata;
  spi_0_metadata.emplace_back([]() {
    fpbus::Metadata ret;
    ret.type() = DEVICE_METADATA_AMLSPI_CONFIG;
    ret.data() = std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(&spi_0_config),
        reinterpret_cast<const uint8_t*>(&spi_0_config) + sizeof(spi_0_config));
    return ret;
  }());

  auto spi_status = fidl_metadata::spi::SpiChannelsToFidl(spi_0_channels);
  if (spi_status.is_error()) {
    zxlogf(ERROR, "%s: failed to encode spi channels to fidl: %d", __func__,
           spi_status.error_value());
    return spi_status.error_value();
  }
  auto& data = spi_status.value();

  spi_0_metadata.emplace_back([&]() {
    fpbus::Metadata ret;
    ret.type() = DEVICE_METADATA_SPI_CHANNELS;
    ret.data() = std::move(data);
    return ret;
  }());

  spi_0_dev.metadata() = std::move(spi_0_metadata);

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('SPI0');
  auto result = pbus_.buffer(arena)->AddComposite(
      fidl::ToWire(fidl_arena, spi_0_dev),
      platform_bus_composite::MakeFidlFragment(fidl_arena, spi_0_fragments,
                                               std::size(spi_0_fragments)),
      "gpio-cs-0");
  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddComposite Spi0(spi_0_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddComposite Spi0(spi_0_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

zx_status_t Nelson::Spi1Init() {
  static const std::vector<fpbus::Mmio> spi_1_mmios{
      {{
          .base = S905D3_SPICC1_BASE,
          .length = S905D3_SPICC1_LENGTH,
      }},
  };

  static const std::vector<fpbus::Irq> spi_1_irqs{
      {{
          .irq = S905D3_SPICC1_IRQ,
          .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
      }},
  };

  static const std::vector<fpbus::Bti> spi_1_btis{
      {{
          .iommu_index = 0,
          .bti_id = BTI_SPI1,
      }},
  };

  static const spi_channel_t spi_1_channels[] = {
      // Radar sensor head.
      {
          .bus_id = NELSON_SPICC1,
          .cs = 0,  // index into matching chip-select map
          .vid = PDEV_VID_INFINEON,
          .pid = PDEV_PID_INFINEON_BGT60TR13C,
          .did = PDEV_DID_RADAR_SENSOR,
      },
  };

  constexpr uint32_t kMoNoDelay = 0 << 0;
  constexpr uint32_t kMiDelay3Cycles = 3 << 2;
  constexpr uint32_t kMiCapAhead2Cycles = 0 << 4;

  static const amlogic_spi::amlspi_config_t spi_1_config = {
      .bus_id = NELSON_SPICC1,
      .cs_count = 1,
      .cs = {0},                                      // index into fragments list
      .clock_divider_register_value = (22 >> 1) - 1,  // SCLK = core clock / 22 = 30.3 MHz
      .use_enhanced_clock_mode = true,
      .client_reverses_dma_transfers = true,
      .delay_control = kMoNoDelay | kMiDelay3Cycles | kMiCapAhead2Cycles,
  };

  fpbus::Node spi_1_dev;
  spi_1_dev.name() = "spi-1";
  spi_1_dev.vid() = PDEV_VID_AMLOGIC;
  spi_1_dev.pid() = PDEV_PID_GENERIC;
  spi_1_dev.did() = PDEV_DID_AMLOGIC_SPI;
  spi_1_dev.instance_id() = 1;
  spi_1_dev.mmio() = spi_1_mmios;
  spi_1_dev.irq() = spi_1_irqs;
  spi_1_dev.bti() = spi_1_btis;

  // setup pinmux for SPICC1 bus arbiter.
  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_B_MOSI, 3);  // MOSI
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_B_MOSI, 2500, nullptr);

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_B_MISO, 3);  // MISO
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_B_MISO, 2500, nullptr);

  gpio_impl_.ConfigOut(GPIO_SOC_SPI_B_SS0, 1);  // SS0

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_B_SCLK, 3);  // SCLK
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_B_SCLK, 2500, nullptr);

  std::vector<fpbus::Metadata> spi_1_metadata;
  spi_1_metadata.emplace_back([]() {
    fpbus::Metadata ret;
    ret.type() = DEVICE_METADATA_AMLSPI_CONFIG,
    ret.data() = std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(&spi_1_config),
        reinterpret_cast<const uint8_t*>(&spi_1_config) + sizeof(spi_1_config));
    return ret;
  }());

  auto spi_status = fidl_metadata::spi::SpiChannelsToFidl(spi_1_channels);
  if (spi_status.is_error()) {
    zxlogf(ERROR, "%s: failed to encode spi channels to fidl: %d", __func__,
           spi_status.error_value());
    return spi_status.error_value();
  }
  auto& data = spi_status.value();

  spi_1_metadata.emplace_back([&]() {
    fpbus::Metadata ret;
    ret.type() = DEVICE_METADATA_SPI_CHANNELS;
    ret.data() = std::move(data);
    return ret;
  }());

  spi_1_dev.metadata() = std::move(spi_1_metadata);

  fdf::Arena arena('SPI1');
  fidl::Arena<> fidl_arena;
  auto result = pbus_.buffer(arena)->AddComposite(
      fidl::ToWire(fidl_arena, spi_1_dev),
      platform_bus_composite::MakeFidlFragment(fidl_arena, spi_1_fragments,
                                               std::size(spi_1_fragments)),
      "gpio-cs-0");
  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddComposite Spi0(spi_1_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddComposite Spi0(spi_1_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace nelson
