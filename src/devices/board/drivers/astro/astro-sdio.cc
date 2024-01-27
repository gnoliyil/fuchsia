// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/hw/reg.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <lib/zircon-internal/align.h>

#include <fbl/algorithm.h>
#include <hwreg/bitfields.h>
#include <soc/aml-common/aml-sdmmc.h>
#include <soc/aml-s905d2/s905d2-gpio.h>
#include <soc/aml-s905d2/s905d2-hw.h>
#include <wifi/wifi-config.h>

#include "astro-gpios.h"
#include "astro.h"
#include "src/devices/board/drivers/astro/astro-aml-sdio-bind.h"
#include "src/devices/board/drivers/astro/astro-wifi-bind.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace astro {
namespace fpbus = fuchsia_hardware_platform_bus;

namespace {

constexpr uint32_t kGpioBase = fbl::round_down<uint32_t, uint32_t>(S905D2_GPIO_BASE, PAGE_SIZE);
constexpr uint32_t kGpioBaseOffset = S905D2_GPIO_BASE - kGpioBase;

class PadDsReg2A : public hwreg::RegisterBase<PadDsReg2A, uint32_t> {
 public:
  static constexpr uint32_t kDriveStrengthMax = 3;

  static auto Get() { return hwreg::RegisterAddr<PadDsReg2A>((0xd2 * 4) + kGpioBaseOffset); }

  DEF_FIELD(1, 0, gpiox_0_select);
  DEF_FIELD(3, 2, gpiox_1_select);
  DEF_FIELD(5, 4, gpiox_2_select);
  DEF_FIELD(7, 6, gpiox_3_select);
  DEF_FIELD(9, 8, gpiox_4_select);
  DEF_FIELD(11, 10, gpiox_5_select);
};

}  // namespace

static const std::vector<fpbus::BootMetadata> wifi_boot_metadata{
    {{
        .zbi_type = DEVICE_METADATA_MAC_ADDRESS,
        .zbi_extra = MACADDR_WIFI,
    }},
};

static const std::vector<fpbus::Mmio> sd_emmc_mmios{
    {{
        .base = S905D2_EMMC_B_SDIO_BASE,
        .length = S905D2_EMMC_B_SDIO_LENGTH,
    }},
    {{
        .base = S905D2_GPIO_BASE,
        .length = S905D2_GPIO_LENGTH,
    }},
    {{
        .base = S905D2_HIU_BASE,
        .length = S905D2_HIU_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> sd_emmc_irqs{
    {{
        .irq = S905D2_EMMC_B_SDIO_IRQ,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    }},
};

static const std::vector<fpbus::Bti> sd_emmc_btis{
    {{
        .iommu_index = 0,
        .bti_id = BTI_SDIO,
    }},
};

static aml_sdmmc_config_t config = {
    .supports_dma = true,
    .min_freq = 400'000,
    .max_freq = 208'000'000,
    .version_3 = true,
    .prefs = 0,
    .use_new_tuning = true,
};

static const wifi_config_t wifi_config = {
    .oob_irq_mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    .iovar_table =
        {
            {IOVAR_STR_TYPE, {"ampdu_ba_wsize"}, 32},
            {IOVAR_CMD_TYPE, {.iovar_cmd = BRCMF_C_SET_PM}, 0},
            {IOVAR_CMD_TYPE, {.iovar_cmd = BRCMF_C_SET_FAKEFRAG}, 1},
            {IOVAR_LIST_END_TYPE, {{0}}, 0},
        },
    .cc_table =
        {
            {"WW", 0},   {"AU", 922}, {"CA", 900}, {"US", 842}, {"GB", 888}, {"BE", 888},
            {"BG", 888}, {"CZ", 888}, {"DK", 888}, {"DE", 888}, {"EE", 888}, {"IE", 888},
            {"GR", 888}, {"ES", 888}, {"FR", 888}, {"HR", 888}, {"IT", 888}, {"CY", 888},
            {"LV", 888}, {"LT", 888}, {"LU", 888}, {"HU", 888}, {"MT", 888}, {"NL", 888},
            {"AT", 888}, {"PL", 888}, {"PT", 888}, {"RO", 888}, {"SI", 888}, {"SK", 888},
            {"FI", 888}, {"SE", 888}, {"EL", 888}, {"IS", 888}, {"LI", 888}, {"TR", 888},
            {"JP", 1},   {"KR", 1},   {"TW", 1},   {"NO", 1},   {"IN", 1},   {"SG", 1},
            {"MX", 1},   {"NZ", 1},   {"CH", 1},   {"", 0},
        },
};

static const std::vector<fpbus::Metadata> sd_emmc_metadata{
    {{
        .type = DEVICE_METADATA_PRIVATE,
        .data = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&config),
                                     reinterpret_cast<const uint8_t*>(&config) + sizeof(config)),
    }},
    {{
        .type = DEVICE_METADATA_WIFI_CONFIG,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&wifi_config),
            reinterpret_cast<const uint8_t*>(&wifi_config) + sizeof(wifi_config)),
    }},
};

static const fpbus::Node sd_emmc_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "aml-sdio";
  dev.vid() = PDEV_VID_AMLOGIC;
  dev.pid() = PDEV_PID_GENERIC;
  dev.did() = PDEV_DID_AMLOGIC_SDMMC_B;
  dev.mmio() = sd_emmc_mmios;
  dev.irq() = sd_emmc_irqs;
  dev.bti() = sd_emmc_btis;
  dev.metadata() = sd_emmc_metadata;
  dev.boot_metadata() = wifi_boot_metadata;
  return dev;
}();

zx_status_t Astro::SdEmmcConfigurePortB() {
  // Clear GPIO_X
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_D0, 0);
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_D1, 0);
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_D2, 0);
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_D3, 0);
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_CLK, 0);
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_CMD, 0);
  gpio_impl_.SetAltFunction(S905D2_WIFI_SDIO_WAKE_HOST, 0);
  // Clear GPIO_C
  gpio_impl_.SetAltFunction(S905D2_GPIOC(0), 0);
  gpio_impl_.SetAltFunction(S905D2_GPIOC(1), 0);
  gpio_impl_.SetAltFunction(S905D2_GPIOC(2), 0);
  gpio_impl_.SetAltFunction(S905D2_GPIOC(3), 0);
  gpio_impl_.SetAltFunction(S905D2_GPIOC(4), 0);
  gpio_impl_.SetAltFunction(S905D2_GPIOC(5), 0);

  zx_status_t status;
  std::optional<fdf::MmioBuffer> gpio_base;
  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  zx::unowned_resource resource(get_root_resource());

  size_t aligned_size = ZX_ROUNDUP((S905D2_GPIO_BASE - kGpioBase) + S905D2_GPIO_LENGTH, PAGE_SIZE);

  status = fdf::MmioBuffer::Create(kGpioBase, aligned_size, *resource,
                                   ZX_CACHE_POLICY_UNCACHED_DEVICE, &gpio_base);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Create(gpio) error: %d", __func__, status);
  }

  // TODO(ravoorir): Figure out if we need gpio protocol ops to modify these
  // gpio registers.
  uint32_t preg_pad_gpio5_val =
      gpio_base->Read32(kGpioBaseOffset + (S905D2_PREG_PAD_GPIO5_O << 2)) |
      AML_SDIO_PORTB_GPIO_REG_5_VAL;
  gpio_base->Write32(preg_pad_gpio5_val, kGpioBaseOffset + (S905D2_PREG_PAD_GPIO5_O << 2));

  uint32_t periphs_pin_mux2_val =
      gpio_base->Read32(kGpioBaseOffset + (S905D2_PERIPHS_PIN_MUX_2 << 2)) |
      AML_SDIO_PORTB_PERIPHS_PINMUX2_VAL;
  gpio_base->Write32(periphs_pin_mux2_val, kGpioBaseOffset + (S905D2_PERIPHS_PIN_MUX_2 << 2));

  uint32_t gpio2_en_n_val = gpio_base->Read32(kGpioBaseOffset + (S905D2_PREG_PAD_GPIO2_EN_N << 2)) &
                            AML_SDIO_PORTB_PERIPHS_GPIO2_EN;
  gpio_base->Write32(gpio2_en_n_val, kGpioBaseOffset + (S905D2_PREG_PAD_GPIO2_EN_N << 2));

  PadDsReg2A::Get()
      .ReadFrom(&(*gpio_base))
      .set_gpiox_0_select(PadDsReg2A::kDriveStrengthMax)
      .set_gpiox_1_select(PadDsReg2A::kDriveStrengthMax)
      .set_gpiox_2_select(PadDsReg2A::kDriveStrengthMax)
      .set_gpiox_3_select(PadDsReg2A::kDriveStrengthMax)
      .set_gpiox_4_select(PadDsReg2A::kDriveStrengthMax)
      .set_gpiox_5_select(PadDsReg2A::kDriveStrengthMax)
      .WriteTo(&(*gpio_base));

  // Configure clock settings
  std::optional<fdf::MmioBuffer> hiu_base;
  status = fdf::MmioBuffer::Create(S905D2_HIU_BASE, S905D2_HIU_LENGTH, *resource,
                                   ZX_CACHE_POLICY_UNCACHED_DEVICE, &hiu_base);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Create(hiu) error: %d", __func__, status);
  }

  uint32_t hhi_gclock_val =
      hiu_base->Read32(HHI_GCLK_MPEG0_OFFSET << 2) | AML_SDIO_PORTB_HHI_GCLK_MPEG0_VAL;
  hiu_base->Write32(hhi_gclock_val, HHI_GCLK_MPEG0_OFFSET << 2);

  uint32_t hh1_sd_emmc_clock_val =
      hiu_base->Read32(HHI_SD_EMMC_CLK_CNTL_OFFSET << 2) & AML_SDIO_PORTB_SDMMC_CLK_VAL;
  hiu_base->Write32(hh1_sd_emmc_clock_val, HHI_SD_EMMC_CLK_CNTL_OFFSET << 2);

  return status;
}

zx_status_t Astro::SdioInit() {
  zx_status_t status;

  SdEmmcConfigurePortB();

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('SDIO');
  auto result = pbus_.buffer(arena)->AddComposite(
      fidl::ToWire(fidl_arena, sd_emmc_dev),
      platform_bus_composite::MakeFidlFragment(fidl_arena, aml_sdio_fragments,
                                               std::size(aml_sdio_fragments)),
      "pdev");
  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddComposite Sdio(sd_emmc_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddComposite Sdio(sd_emmc_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  // Add a composite device for wifi driver.
  const zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_BROADCOM},
      {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_BCM43458},
      {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_BCM_WIFI},
  };

  const composite_device_desc_t comp_desc = {
      .props = props,
      .props_count = std::size(props),
      .fragments = wifi_fragments,
      .fragments_count = std::size(wifi_fragments),
      .primary_fragment = "sdio-function-1",  // ???
      .spawn_colocated = true,
      .metadata_list = nullptr,
      .metadata_count = 0,
  };

  status = DdkAddComposite("wifi", &comp_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAddComposite failed: %d", __func__, status);
    return status;
  }

  return ZX_OK;
}

}  // namespace astro
