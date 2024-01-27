// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <fuchsia/hardware/shareddma/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <limits.h>

#include <ddktl/metadata/audio.h>
#include <fbl/algorithm.h>
#include <soc/as370/as370-audio.h>
#include <soc/as370/as370-clk.h>
#include <soc/as370/as370-gpio.h>
#include <soc/as370/as370-hw.h>
#include <soc/as370/as370-i2c.h>
#include <ti/ti-audio.h>

#include "pinecrest-gpio.h"
#include "pinecrest.h"
#include "src/devices/board/drivers/pinecrest/pinecrest-bind.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

#ifdef TAS5825M_CONFIG_PATH
#include TAS5825M_CONFIG_PATH
#endif

namespace board_pinecrest {
namespace fpbus = fuchsia_hardware_platform_bus;

static const zx_bind_inst_t ref_out_i2c_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    BI_ABORT_IF(NE, BIND_I2C_BUS_ID, 1),
    BI_MATCH_IF(EQ, BIND_I2C_ADDRESS, 0x4c),
};
static const zx_bind_inst_t ref_out_codec_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_CODEC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TI),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TI_TAS58xx),
};
static const zx_bind_inst_t dma_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SHARED_DMA),
};
static const zx_bind_inst_t ref_out_clk0_match[] = {
    BI_ABORT_IF(NE, BIND_FIDL_PROTOCOL, ZX_FIDL_PROTOCOL_CLOCK),
    BI_MATCH_IF(EQ, BIND_CLOCK_ID, as370::As370Clk::kClkAvpll0),
};

static const device_fragment_part_t ref_out_i2c_fragment[] = {
    {std::size(ref_out_i2c_match), ref_out_i2c_match},
};
static const device_fragment_part_t ref_out_codec_fragment[] = {
    {std::size(ref_out_codec_match), ref_out_codec_match},
};
static const device_fragment_part_t dma_fragment[] = {
    {std::size(dma_match), dma_match},
};

static const device_fragment_part_t ref_out_clk0_fragment[] = {
    {std::size(ref_out_clk0_match), ref_out_clk0_match},
};

static const zx_bind_inst_t controller_pdev_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_SYNAPTICS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_SYNAPTICS_AS370),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_DID, PDEV_DID_AS370_AUDIO_OUT),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_INSTANCE_ID, 0),
};
constexpr device_fragment_part_t controller_pdev_fragment[] = {
    {std::size(controller_pdev_match), controller_pdev_match},
};

static const zx_bind_inst_t in_pdev_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_SYNAPTICS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_SYNAPTICS_AS370),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_DID, PDEV_DID_AS370_AUDIO_IN),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_INSTANCE_ID, 0),
};
constexpr device_fragment_part_t in_pdev_fragment[] = {
    {std::size(in_pdev_match), in_pdev_match},
};

static const device_fragment_t codec_fragments[] = {
    {"i2c", std::size(ref_out_i2c_fragment), ref_out_i2c_fragment},
};
static const device_fragment_t controller_fragments[] = {
    {"pdev", std::size(controller_pdev_fragment), controller_pdev_fragment},
    {"dma", std::size(dma_fragment), dma_fragment},
    {"codec", std::size(ref_out_codec_fragment), ref_out_codec_fragment},
    {"clock", std::size(ref_out_clk0_fragment), ref_out_clk0_fragment},
};
static const device_fragment_t in_fragments[] = {
    {"pdev", std::size(in_pdev_fragment), in_pdev_fragment},
    {"dma", std::size(dma_fragment), dma_fragment},
    {"clock", std::size(ref_out_clk0_fragment), ref_out_clk0_fragment},
};

zx_status_t Pinecrest::AudioInit() {
  static const std::vector<fpbus::Mmio> mmios_out{
      {{
          .base = as370::kAudioGlobalBase,
          .length = as370::kAudioGlobalSize,
      }},
      {{
          .base = as370::kAudioI2sBase,
          .length = as370::kAudioI2sSize,
      }},
  };

  metadata::As370Config controller_metadata = {};
  snprintf(controller_metadata.manufacturer, sizeof(controller_metadata.manufacturer), "Google");
  snprintf(controller_metadata.product_name, sizeof(controller_metadata.product_name), "Pinecrest");
  controller_metadata.is_input = false;
  controller_metadata.ring_buffer.number_of_channels = 2;
  // This product uses 2 speakers, one for lower frequencies and one for higher frequencies.
  controller_metadata.ring_buffer.frequency_ranges[0].min_frequency = 20;
  controller_metadata.ring_buffer.frequency_ranges[0].max_frequency = 2'000;
  controller_metadata.ring_buffer.frequency_ranges[1].min_frequency = 2'000;
  controller_metadata.ring_buffer.frequency_ranges[1].max_frequency = 48'000;
  std::vector<fpbus::Metadata> controller_metadata2{
      {{
          .type = DEVICE_METADATA_PRIVATE,
          .data = std::vector<uint8_t>(
              reinterpret_cast<const uint8_t*>(&controller_metadata),
              reinterpret_cast<const uint8_t*>(&controller_metadata) + sizeof(controller_metadata)),
      }},
  };

  fpbus::Node controller_out;
  controller_out.name() = "pinecrest-audio-out";
  controller_out.vid() = PDEV_VID_SYNAPTICS;
  controller_out.pid() = PDEV_PID_SYNAPTICS_AS370;
  controller_out.did() = PDEV_DID_AS370_AUDIO_OUT;
  controller_out.mmio() = mmios_out;
  controller_out.metadata() = controller_metadata2;

  static const std::vector<fpbus::Mmio> mmios_in{
      {{
          .base = as370::kAudioGlobalBase,
          .length = as370::kAudioGlobalSize,
      }},
      {{
          .base = as370::kAudioI2sBase,
          .length = as370::kAudioI2sSize,
      }},
  };

  fpbus::Node dev_in;
  dev_in.name() = "pinecrest-audio-in";
  dev_in.vid() = PDEV_VID_SYNAPTICS;
  dev_in.pid() = PDEV_PID_SYNAPTICS_AS370;
  dev_in.did() = PDEV_DID_AS370_AUDIO_IN;
  dev_in.mmio() = mmios_in;

  static const std::vector<fpbus::Mmio> mmios_dhub{
      {{
          .base = as370::kAudioDhubBase,
          .length = as370::kAudioDhubSize,
      }},
  };

  static const std::vector<fpbus::Irq> irqs_dhub{
      {{
          .irq = as370::kDhubIrq,
          .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
      }},
  };
  static const std::vector<fpbus::Bti> btis_dhub{
      {{
          .iommu_index = 0,
          .bti_id = BTI_AUDIO_DHUB,
      }},
  };

  fpbus::Node dhub;
  dhub.name() = "pinecrest-dhub";
  dhub.vid() = PDEV_VID_SYNAPTICS;
  dhub.pid() = PDEV_PID_SYNAPTICS_AS370;
  dhub.did() = PDEV_DID_AS370_DHUB;
  dhub.mmio() = mmios_dhub;
  dhub.irq() = irqs_dhub;
  dhub.bti() = btis_dhub;

  // Output pin assignments.
  gpio_impl_.SetAltFunction(17, 0);  // AMP_EN, mode 0 to set as GPIO.
  gpio_impl_.ConfigOut(17, 1);

  gpio_impl_.SetAltFunction(0, 1);  // mode 1 to set as I2S1_BCLKIO.
  gpio_impl_.SetAltFunction(1, 1);  // mode 1 to set as I2S1_LRLKIO.
  gpio_impl_.SetAltFunction(2, 1);  // mode 3 to set as I2S1_DO.

  // Input pin assignments.
  gpio_impl_.SetAltFunction(13, 1);  // mode 1 to set as PDM_CLKO.
  gpio_impl_.SetAltFunction(14, 1);  // mode 1 to set as PDM_DI[0].
  gpio_impl_.SetAltFunction(15, 1);  // mode 1 to set as PDM_DI[1].

  // DMA device.
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('AUDI');
  auto result = pbus_.buffer(arena)->NodeAdd(fidl::ToWire(fidl_arena, dhub));
  if (!result.ok()) {
    zxlogf(ERROR, "NodeAdd Audio(dhub) request failed: %s", result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "NodeAdd Audio(dhub) failed: %s", zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  // Output devices.
  constexpr zx_device_prop_t props[] = {{BIND_PLATFORM_DEV_VID, 0, PDEV_VID_TI},
                                        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_TI_TAS58xx}};
  metadata::ti::TasConfig metadata = {};
#ifdef TAS5825M_CONFIG_PATH
  constexpr size_t kNumberOfWrites1 = sizeof(tas5825m_init_sequence1) / sizeof(cfg_reg);
  metadata.number_of_writes1 = kNumberOfWrites1;
  static_assert(kNumberOfWrites1 <= metadata::ti::kMaxNumberOfRegisterWrites);
  for (size_t i = 0; i < metadata.number_of_writes1; ++i) {
    metadata.init_sequence1[i].address = tas5825m_init_sequence1[i].offset;
    metadata.init_sequence1[i].value = tas5825m_init_sequence1[i].value;
  }
  constexpr size_t kNumberOfWrites2 = sizeof(tas5825m_init_sequence2) / sizeof(cfg_reg);
  metadata.number_of_writes2 = kNumberOfWrites2;
  static_assert(kNumberOfWrites2 <= metadata::ti::kMaxNumberOfRegisterWrites);
  for (size_t i = 0; i < metadata.number_of_writes2; ++i) {
    metadata.init_sequence2[i].address = tas5825m_init_sequence2[i].offset;
    metadata.init_sequence2[i].value = tas5825m_init_sequence2[i].value;
  }
#endif
  const device_metadata_t codec_metadata[] = {
      {
          .type = DEVICE_METADATA_PRIVATE,
          .data = reinterpret_cast<uint8_t*>(&metadata),
          .length = sizeof(metadata),
      },
  };

  const composite_device_desc_t comp_desc = {
      .props = props,
      .props_count = std::size(props),
      .fragments = codec_fragments,
      .fragments_count = std::size(codec_fragments),
      .primary_fragment = "i2c",
      .spawn_colocated = false,
      .metadata_list = codec_metadata,
      .metadata_count = std::size(codec_metadata),
  };

  zx_status_t status = DdkAddComposite("audio-tas5805", &comp_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "DdkAddComposite failed %s", zx_status_get_string(status));
    return status;
  }

  // Share devhost with DHub.
  {
    auto result = pbus_.buffer(arena)->AddComposite(
        fidl::ToWire(fidl_arena, controller_out),
        platform_bus_composite::MakeFidlFragment(fidl_arena, controller_fragments,
                                                 std::size(controller_fragments)),
        "dma");
    if (!result.ok()) {
      zxlogf(ERROR, "AddCompositeImplicitPbusFragment Audio(controller_out) request failed: %s",
             result.FormatDescription().data());
      return result.status();
    }
    if (result->is_error()) {
      zxlogf(ERROR, "AddCompositeImplicitPbusFragment Audio(controller_out) failed: %s",
             zx_status_get_string(result->error_value()));
      return result->error_value();
    }

    // Input device.
    // Share devhost with DHub.
    result = pbus_.buffer(arena)->AddComposite(
        fidl::ToWire(fidl_arena, dev_in),
        platform_bus_composite::MakeFidlFragment(fidl_arena, in_fragments, std::size(in_fragments)),
        "dma");
    if (!result.ok()) {
      zxlogf(ERROR, "AddCompositeImplicitPbusFragment Audio(dev_in) request failed: %s",
             result.FormatDescription().data());
      return result.status();
    }
    if (result->is_error()) {
      zxlogf(ERROR, "AddCompositeImplicitPbusFragment Audio(dev_in) failed: %s",
             zx_status_get_string(result->error_value()));
      return result->error_value();
    }
  }
  return ZX_OK;
}

}  // namespace board_pinecrest
