// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-gpio.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <cstdint>
#include <memory>

#include <fbl/alloc_checker.h>

#include "a1-blocks.h"
#include "a113-blocks.h"
#include "a5-blocks.h"
#include "fidl/fuchsia.hardware.platform.bus/cpp/markers.h"
#include "s905d2-blocks.h"
#include "src/devices/gpio/drivers/aml-gpio/aml-gpio-bind.h"

namespace {

constexpr int kAltFnMax = 15;
constexpr int kMaxPinsInDSReg = 16;
constexpr int kGpioInterruptPolarityShift = 16;
constexpr int kMaxGpioIndex = 255;
constexpr int kBitsPerGpioInterrupt = 8;
constexpr int kBitsPerFilterSelect = 4;

uint32_t GetUnusedIrqIndex(uint8_t status) {
  // First isolate the rightmost 0-bit
  auto zero_bit_set = static_cast<uint8_t>(~status & (status + 1));
  // Count no. of leading zeros
  return __builtin_ctz(zero_bit_set);
}

// Supported Drive Strengths
enum DriveStrength {
  DRV_500UA,
  DRV_2500UA,
  DRV_3000UA,
  DRV_4000UA,
};

}  // namespace

namespace gpio {

// MMIO indices (based on aml-gpio.c gpio_mmios)
enum {
  MMIO_GPIO = 0,
  MMIO_GPIO_AO = 1,
  MMIO_GPIO_INTERRUPTS = 2,
};

zx_status_t AmlGpio::Create(void* ctx, zx_device_t* parent) {
  zx_status_t status;

  auto endpoints = fdf::CreateEndpoints<fuchsia_hardware_platform_bus::PlatformBus>();
  if (endpoints.is_error()) {
    zxlogf(ERROR, " create endpoints failed");
    return endpoints.error_value();
  }

  bool has_pbus = true;

  status = device_connect_runtime_protocol(
      parent, fuchsia_hardware_platform_bus::Service::PlatformBus::ServiceName,
      fuchsia_hardware_platform_bus::Service::PlatformBus::Name,
      endpoints->server.TakeHandle().release());
  if (status != ZX_OK) {
    zxlogf(WARNING, "Failed to connect to platform bus");
    has_pbus = false;
  }

  ddk::PDev pdev(parent);
  std::optional<fdf::MmioBuffer> mmio_gpio, mmio_gpio_a0, mmio_interrupt;
  if ((status = pdev.MapMmio(MMIO_GPIO, &mmio_gpio)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::Create: MapMmio failed");
    return status;
  }

  if ((status = pdev.MapMmio(MMIO_GPIO_AO, &mmio_gpio_a0)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::Create: MapMmio failed");
    return status;
  }

  if ((status = pdev.MapMmio(MMIO_GPIO_INTERRUPTS, &mmio_interrupt)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::Create: MapMmio failed");
    return status;
  }

  pdev_device_info_t info;
  if ((status = pdev.GetDeviceInfo(&info)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::Create: GetDeviceInfo failed");
    return status;
  }

  const AmlGpioBlock* gpio_blocks;
  const AmlGpioInterrupt* gpio_interrupt;
  size_t block_count;

  switch (info.pid) {
    case PDEV_PID_AMLOGIC_A113:
      gpio_blocks = a113_gpio_blocks;
      block_count = std::size(a113_gpio_blocks);
      gpio_interrupt = &a113_interrupt_block;
      break;
    case PDEV_PID_AMLOGIC_S905D2:
    case PDEV_PID_AMLOGIC_T931:
    case PDEV_PID_AMLOGIC_A311D:
    case PDEV_PID_AMLOGIC_S905D3:
      // S905D2, T931, A311D, S905D3 are identical.
      gpio_blocks = s905d2_gpio_blocks;
      block_count = std::size(s905d2_gpio_blocks);
      gpio_interrupt = &s905d2_interrupt_block;
      break;
    case PDEV_PID_AMLOGIC_A5:
      gpio_blocks = a5_gpio_blocks;
      block_count = std::size(a5_gpio_blocks);
      gpio_interrupt = &a5_interrupt_block;
      break;
    case PDEV_PID_AMLOGIC_A1:
      gpio_blocks = a1_gpio_blocks;
      block_count = std::size(a1_gpio_blocks);
      gpio_interrupt = &a1_interrupt_block;
      break;
    default:
      zxlogf(ERROR, "AmlGpio::Create: unsupported SOC PID %u", info.pid);
      return ZX_ERR_INVALID_ARGS;
  }

  fbl::AllocChecker ac;

  fbl::Array<uint16_t> irq_info(new (&ac) uint16_t[info.irq_count], info.irq_count);
  if (!ac.check()) {
    zxlogf(ERROR, "AmlGpio::Create: irq_info alloc failed");
    return ZX_ERR_NO_MEMORY;
  }
  for (uint32_t i = 0; i < info.irq_count; i++) {
    irq_info[i] = kMaxGpioIndex + 1;
  }  // initialize irq_info

  std::unique_ptr<AmlGpio> device(new (&ac) AmlGpio(
      parent, *std::move(mmio_gpio), *std::move(mmio_gpio_a0), *std::move(mmio_interrupt),
      gpio_blocks, gpio_interrupt, block_count, std::move(info), std::move(irq_info)));
  if (!ac.check()) {
    zxlogf(ERROR, "AmlGpio::Create: device object alloc failed");
    return ZX_ERR_NO_MEMORY;
  }

  if (has_pbus) {
    device->Bind(fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus>(
        std::move(endpoints->client)));
  }

  if (auto status = device->DdkAdd(ddk::DeviceAddArgs("aml-gpio")
                                       .set_proto_id(ZX_PROTOCOL_GPIO_IMPL)
                                       .forward_metadata(parent, DEVICE_METADATA_GPIO_PINS));
      status != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::Create: DdkAdd failed");
    return status;
  }

  [[maybe_unused]] auto* unused = device.release();

  return ZX_OK;
}

void AmlGpio::Bind(fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus) {
  gpio_impl_protocol_t gpio_proto = {
      .ops = &gpio_impl_protocol_ops_,
      .ctx = this,
  };

  fdf::Arena arena('GPIO');
  [[maybe_unused]] auto unused = pbus.buffer(arena)->RegisterProtocol(
      ZX_PROTOCOL_GPIO_IMPL, fidl::VectorView<uint8_t>::FromExternal(
                                 reinterpret_cast<uint8_t*>(&gpio_proto), sizeof(gpio_proto)));
}

zx_status_t AmlGpio::AmlPinToBlock(const uint32_t pin, const AmlGpioBlock** out_block,
                                   uint32_t* out_pin_index) const {
  ZX_DEBUG_ASSERT(out_block && out_pin_index);

  for (size_t i = 0; i < block_count_; i++) {
    const AmlGpioBlock& gpio_block = gpio_blocks_[i];
    const uint32_t end_pin = gpio_block.start_pin + gpio_block.pin_count;
    if (pin >= gpio_block.start_pin && pin < end_pin) {
      *out_block = &gpio_block;
      *out_pin_index = pin - gpio_block.pin_block + gpio_block.output_shift;
      return ZX_OK;
    }
  }

  return ZX_ERR_NOT_FOUND;
}

zx_status_t AmlGpio::GpioImplConfigIn(uint32_t index, uint32_t flags) {
  zx_status_t status;

  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(index, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplConfigIn: pin not found %u", index);
    return status;
  }

  const uint32_t pinmask = 1 << pinindex;

  {
    fbl::AutoLock al(&mmio_lock_);

    uint32_t regval = mmios_[block->mmio_index].Read32(block->oen_offset * sizeof(uint32_t));
    // Set the GPIO as pull-up or pull-down
    uint32_t pull = flags & GPIO_PULL_MASK;
    uint32_t pull_reg_val = mmios_[block->mmio_index].Read32(block->pull_offset * sizeof(uint32_t));
    uint32_t pull_en_reg_val =
        mmios_[block->mmio_index].Read32(block->pull_en_offset * sizeof(uint32_t));
    if (pull & GPIO_NO_PULL) {
      pull_en_reg_val &= ~pinmask;
    } else {
      if (pull & GPIO_PULL_UP) {
        pull_reg_val |= pinmask;
      } else {
        pull_reg_val &= ~pinmask;
      }
      pull_en_reg_val |= pinmask;
    }

    mmios_[block->mmio_index].Write32(pull_reg_val, block->pull_offset * sizeof(uint32_t));
    mmios_[block->mmio_index].Write32(pull_en_reg_val, block->pull_en_offset * sizeof(uint32_t));
    regval |= pinmask;
    mmios_[block->mmio_index].Write32(regval, block->oen_offset * sizeof(uint32_t));
  }

  return ZX_OK;
}

zx_status_t AmlGpio::GpioImplConfigOut(uint32_t index, uint8_t initial_value) {
  zx_status_t status;

  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(index, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplConfigOut: pin not found %u", index);
    return status;
  }

  const uint32_t pinmask = 1 << pinindex;

  {
    fbl::AutoLock al(&mmio_lock_);

    // Set value before configuring for output
    uint32_t regval = mmios_[block->mmio_index].Read32(block->output_offset * sizeof(uint32_t));
    if (initial_value) {
      regval |= pinmask;
    } else {
      regval &= ~pinmask;
    }
    mmios_[block->mmio_index].Write32(regval, block->output_offset * sizeof(uint32_t));

    regval = mmios_[block->mmio_index].Read32(block->oen_offset * sizeof(uint32_t));
    regval &= ~pinmask;
    mmios_[block->mmio_index].Write32(regval, block->oen_offset * sizeof(uint32_t));
  }

  return ZX_OK;
}

// Configure a pin for an alternate function specified by fn
zx_status_t AmlGpio::GpioImplSetAltFunction(const uint32_t pin, const uint64_t fn) {
  if (fn > kAltFnMax) {
    zxlogf(ERROR, "AmlGpio::GpioImplSetAltFunction: pin mux alt config out of range %lu", fn);
    return ZX_ERR_OUT_OF_RANGE;
  }

  zx_status_t status;

  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(pin, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplSetAltFunction: pin not found %u", pin);
    return status;
  }

  // Sanity Check: pin_to_block must return a block that contains `pin`
  //               therefore `pin` must be greater than or equal to the first
  //               pin of the block.
  ZX_DEBUG_ASSERT(pin >= block->start_pin);

  // Each Pin Mux is controlled by a 4 bit wide field in `reg`
  // Compute the offset for this pin.
  uint32_t pin_shift = (pin - block->start_pin) * 4;
  pin_shift += block->output_shift;
  const uint32_t mux_mask = ~(0x0F << pin_shift);
  const auto fn_val = static_cast<uint32_t>(fn << pin_shift);

  {
    fbl::AutoLock al(&mmio_lock_);

    uint32_t regval = mmios_[block->mmio_index].Read32(block->mux_offset * sizeof(uint32_t));
    regval &= mux_mask;  // Remove the previous value for the mux
    regval |= fn_val;    // Assign the new value to the mux
    mmios_[block->mmio_index].Write32(regval, block->mux_offset * sizeof(uint32_t));
  }

  return ZX_OK;
}

zx_status_t AmlGpio::GpioImplRead(uint32_t index, uint8_t* out_value) {
  zx_status_t status;

  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(index, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplRead: pin not found %u", index);
    return status;
  }

  uint32_t regval = 0;
  {
    fbl::AutoLock al(&mmio_lock_);
    regval = mmios_[block->mmio_index].Read32(block->input_offset * sizeof(uint32_t));
  }

  const uint32_t readmask = 1 << pinindex;
  if (regval & readmask) {
    *out_value = 1;
  } else {
    *out_value = 0;
  }

  return ZX_OK;
}

zx_status_t AmlGpio::GpioImplWrite(uint32_t index, uint8_t value) {
  zx_status_t status;

  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(index, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplWrite: pin not found %u", index);
    return status;
  }

  {
    fbl::AutoLock al(&mmio_lock_);

    uint32_t regval = mmios_[block->mmio_index].Read32(block->output_offset * sizeof(uint32_t));
    if (value) {
      regval |= 1 << pinindex;
    } else {
      regval &= ~(1 << pinindex);
    }
    mmios_[block->mmio_index].Write32(regval, block->output_offset * sizeof(uint32_t));
  }

  return ZX_OK;
}

zx_status_t AmlGpio::GpioImplGetInterrupt(uint32_t pin, uint32_t flags, zx::interrupt* out_irq) {
  zx_status_t status = ZX_OK;

  if (pin > kMaxGpioIndex) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock al(&irq_lock_);

  uint32_t index = GetUnusedIrqIndex(irq_status_);
  if (index > info_.irq_count) {
    zxlogf(ERROR, "No free IRQ indicies %u, irq_count = %u", (int)index, (int)info_.irq_count);
    return ZX_ERR_NO_RESOURCES;
  }

  for (uint32_t i = 0; i < info_.irq_count; i++) {
    if (irq_info_[i] == pin) {
      zxlogf(ERROR, "GPIO Interrupt already configured for this pin %u", (int)index);
      return ZX_ERR_ALREADY_EXISTS;
    }
  }
  zxlogf(DEBUG, "GPIO Interrupt index %d allocated", (int)index);
  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(pin, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplGetInterrupt: pin not found %u", pin);
    return status;
  }
  uint32_t flags_ = flags;
  if (flags == ZX_INTERRUPT_MODE_EDGE_LOW) {
    // GPIO controller sets the polarity
    flags_ = ZX_INTERRUPT_MODE_EDGE_HIGH;
  } else if (flags == ZX_INTERRUPT_MODE_LEVEL_LOW) {
    flags_ = ZX_INTERRUPT_MODE_LEVEL_HIGH;
  }

  {
    fbl::AutoLock al(&mmio_lock_);
    // Configure GPIO Interrupt EDGE and Polarity
    uint32_t mode_reg_val =
        mmio_interrupt_.Read32(gpio_interrupt_->edge_polarity_offset * sizeof(uint32_t));

    switch (flags & ZX_INTERRUPT_MODE_MASK) {
      case ZX_INTERRUPT_MODE_EDGE_LOW:
        mode_reg_val |= (1 << index);
        mode_reg_val |= ((1 << index) << kGpioInterruptPolarityShift);
        break;
      case ZX_INTERRUPT_MODE_EDGE_HIGH:
        mode_reg_val |= (1 << index);
        mode_reg_val &= ~((1 << index) << kGpioInterruptPolarityShift);
        break;
      case ZX_INTERRUPT_MODE_LEVEL_LOW:
        mode_reg_val &= ~(1 << index);
        mode_reg_val |= ((1 << index) << kGpioInterruptPolarityShift);
        break;
      case ZX_INTERRUPT_MODE_LEVEL_HIGH:
        mode_reg_val &= ~(1 << index);
        mode_reg_val &= ~((1 << index) << kGpioInterruptPolarityShift);
        break;
      default:
        return ZX_ERR_INVALID_ARGS;
    }
    mmio_interrupt_.Write32(mode_reg_val, gpio_interrupt_->edge_polarity_offset * sizeof(uint32_t));

    // Configure Interrupt Select Filter
    mmio_interrupt_.SetBits32(0x7 << (index * kBitsPerFilterSelect),
                              gpio_interrupt_->filter_select_offset * sizeof(uint32_t));

    // Configure GPIO interrupt
    const uint32_t pin_select_bit = index * kBitsPerGpioInterrupt;
    const uint32_t pin_select_offset = gpio_interrupt_->pin_select_offset + (pin_select_bit / 32);
    const uint32_t pin_select_index = pin_select_bit % 32;
    // Select GPIO IRQ(index) and program it to the requested GPIO PIN
    mmio_interrupt_.ModifyBits32((pin - block->pin_block) + block->pin_start, pin_select_index,
                                 kBitsPerGpioInterrupt, pin_select_offset * sizeof(uint32_t));
  }

  // Create Interrupt Object
  if ((status = pdev_.GetInterrupt(index, flags_, out_irq)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplGetInterrupt: pdev_get_interrupt failed %d", status);
    return status;
  }

  irq_status_ |= static_cast<uint8_t>(1 << index);
  irq_info_[index] = static_cast<uint16_t>(pin);

  return status;
}

zx_status_t AmlGpio::GpioImplReleaseInterrupt(uint32_t pin) {
  fbl::AutoLock al(&irq_lock_);
  for (uint32_t i = 0; i < info_.irq_count; i++) {
    if (irq_info_[i] == pin) {
      irq_status_ &= static_cast<uint8_t>(~(1 << i));
      irq_info_[i] = kMaxGpioIndex + 1;
      return ZX_OK;
    }
  }
  return ZX_ERR_NOT_FOUND;
}

zx_status_t AmlGpio::GpioImplSetPolarity(uint32_t pin, uint32_t polarity) {
  int irq_index = -1;
  if (pin > kMaxGpioIndex) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock al(&irq_lock_);
  for (uint32_t i = 0; i < info_.irq_count; i++) {
    if (irq_info_[i] == pin) {
      irq_index = i;
      break;
    }
  }
  if (irq_index == -1) {
    return ZX_ERR_NOT_FOUND;
  }

  {
    fbl::AutoLock al(&mmio_lock_);
    // Configure GPIO Interrupt EDGE and Polarity
    if (polarity) {
      mmio_interrupt_.ClearBits32(((1 << irq_index) << kGpioInterruptPolarityShift),
                                  gpio_interrupt_->edge_polarity_offset * sizeof(uint32_t));
    } else {
      mmio_interrupt_.SetBits32(((1 << irq_index) << kGpioInterruptPolarityShift),
                                gpio_interrupt_->edge_polarity_offset * sizeof(uint32_t));
    }
  }
  return ZX_OK;
}

zx_status_t AmlGpio::GpioImplGetDriveStrength(uint32_t pin, uint64_t* out) {
  zx_status_t st;

  if (info_.pid == PDEV_PID_AMLOGIC_A113) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (!out) {
    zxlogf(ERROR, "AmlGpio::GpioImplGetDriveStrength: Missing required parameter `out`");
    return ZX_ERR_INVALID_ARGS;
  }

  const AmlGpioBlock* block;
  uint32_t pinindex;

  if ((st = AmlPinToBlock(pin, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplGetDriveStrength: pin not found %u", pin);
    return st;
  }

  pinindex = pin - block->pin_block;
  if (pinindex >= kMaxPinsInDSReg) {
    pinindex = pinindex % kMaxPinsInDSReg;
  }

  const uint32_t shift = pinindex * 2;
  uint32_t value = 0;

  {
    fbl::AutoLock al(&mmio_lock_);
    uint32_t regval = mmios_[block->mmio_index].Read32(block->ds_offset * sizeof(uint32_t));
    value = (regval >> shift) & 0x3;
  }

  switch (value) {
    case DRV_500UA:
      *out = 500;
      break;
    case DRV_2500UA:
      *out = 2500;
      break;
    case DRV_3000UA:
      *out = 3000;
      break;
    case DRV_4000UA:
      *out = 4000;
      break;
    default:
      zxlogf(ERROR, "AmlGpio::GpioImplGetDriveStrength: Unexpected drive strength value: %u",
             value);
      return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t AmlGpio::GpioImplSetDriveStrength(uint32_t pin, uint64_t ua, uint64_t* out_actual_ua) {
  zx_status_t status;

  if (info_.pid == PDEV_PID_AMLOGIC_A113) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  const AmlGpioBlock* block;
  uint32_t pinindex;
  if ((status = AmlPinToBlock(pin, &block, &pinindex)) != ZX_OK) {
    zxlogf(ERROR, "AmlGpio::GpioImplSetDriveStrength: pin not found %u", pin);
    return status;
  }

  DriveStrength ds_val = DRV_4000UA;
  if (ua <= 500) {
    ds_val = DRV_500UA;
    ua = 500;
  } else if (ua <= 2500) {
    ds_val = DRV_2500UA;
    ua = 2500;
  } else if (ua <= 3000) {
    ds_val = DRV_3000UA;
    ua = 3000;
  } else if (ua <= 4000) {
    ds_val = DRV_4000UA;
    ua = 4000;
  } else {
    zxlogf(ERROR,
           "AmlGpio::GpioImplSetDriveStrength: invalid drive strength %lu, default to 4000 uA\n",
           ua);
    ds_val = DRV_4000UA;
    ua = 4000;
  }

  pinindex = pin - block->pin_block;
  if (pinindex >= kMaxPinsInDSReg) {
    pinindex = pinindex % kMaxPinsInDSReg;
  }

  // 2 bits for each pin
  const uint32_t shift = pinindex * 2;
  const uint32_t mask = ~(0x3 << shift);
  {
    fbl::AutoLock al(&mmio_lock_);
    uint32_t regval = mmios_[block->mmio_index].Read32(block->ds_offset * sizeof(uint32_t));
    regval = (regval & mask) | (ds_val << shift);
    mmios_[block->mmio_index].Write32(regval, block->ds_offset * sizeof(uint32_t));
  }
  if (out_actual_ua) {
    *out_actual_ua = ua;
  }

  return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AmlGpio::Create;
  return ops;
}();

}  // namespace gpio

ZIRCON_DRIVER(aml_gpio, gpio::driver_ops, "zircon", "0.1");
