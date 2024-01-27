// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_GPIO_DRIVERS_AML_GPIO_AML_GPIO_H_
#define SRC_DEVICES_GPIO_DRIVERS_AML_GPIO_AML_GPIO_H_

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fuchsia/hardware/gpioimpl/cpp/banjo.h>
#include <lib/ddk/hw/reg.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/mmio/mmio.h>
#include <lib/zircon-internal/thread_annotations.h>

#include <array>
#include <cstdint>

#include <ddktl/device.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>

namespace gpio {

struct AmlGpioBlock {
  uint32_t start_pin;
  uint32_t pin_block;
  uint32_t pin_count;
  uint32_t mux_offset;
  uint32_t oen_offset;
  uint32_t input_offset;
  uint32_t output_offset;
  uint32_t output_shift;  // Used for GPIOAO block
  uint32_t pull_offset;
  uint32_t pull_en_offset;
  uint32_t mmio_index;
  uint32_t pin_start;
  uint32_t ds_offset;
};

struct AmlGpioInterrupt {
  uint32_t pin_select_offset;
  uint32_t edge_polarity_offset;
  uint32_t filter_select_offset;
};

class AmlGpio;
using DeviceType = ddk::Device<AmlGpio>;

class AmlGpio : public DeviceType, public ddk::GpioImplProtocol<AmlGpio, ddk::base_protocol> {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  zx_status_t GpioImplConfigIn(uint32_t index, uint32_t flags);
  zx_status_t GpioImplConfigOut(uint32_t index, uint8_t initial_value);
  zx_status_t GpioImplSetAltFunction(uint32_t index, uint64_t function);
  zx_status_t GpioImplSetDriveStrength(uint32_t index, uint64_t ua, uint64_t* out_actual_ua);
  zx_status_t GpioImplGetDriveStrength(uint32_t pin, uint64_t* out);
  zx_status_t GpioImplRead(uint32_t index, uint8_t* out_value);
  zx_status_t GpioImplWrite(uint32_t index, uint8_t value);
  zx_status_t GpioImplGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq);
  zx_status_t GpioImplReleaseInterrupt(uint32_t index);
  zx_status_t GpioImplSetPolarity(uint32_t index, gpio_polarity_t polarity);

  void DdkRelease() { delete this; }

 protected:
  // for AmlGpioTest
  explicit AmlGpio(pdev_protocol_t* proto, fdf::MmioBuffer mmio_gpio, fdf::MmioBuffer mmio_gpio_a0,
                   fdf::MmioBuffer mmio_interrupt, const AmlGpioBlock* gpio_blocks,
                   const AmlGpioInterrupt* gpio_interrupt, size_t block_count,
                   pdev_device_info_t info, fbl::Array<uint16_t> irq_info)
      : DeviceType(nullptr),
        pdev_(proto),
        mmios_{std::move(mmio_gpio), std::move(mmio_gpio_a0)},
        mmio_interrupt_(std::move(mmio_interrupt)),
        gpio_blocks_(gpio_blocks),
        gpio_interrupt_(gpio_interrupt),
        block_count_(block_count),
        info_(std::move(info)),
        irq_info_(std::move(irq_info)),
        irq_status_(0) {}

 private:
  explicit AmlGpio(zx_device_t* parent, fdf::MmioBuffer mmio_gpio, fdf::MmioBuffer mmio_gpio_a0,
                   fdf::MmioBuffer mmio_interrupt, const AmlGpioBlock* gpio_blocks,
                   const AmlGpioInterrupt* gpio_interrupt, size_t block_count,
                   pdev_device_info_t info, fbl::Array<uint16_t> irq_info)
      : DeviceType(parent),
        pdev_(parent),
        mmios_{std::move(mmio_gpio), std::move(mmio_gpio_a0)},
        mmio_interrupt_(std::move(mmio_interrupt)),
        gpio_blocks_(gpio_blocks),
        gpio_interrupt_(gpio_interrupt),
        block_count_(block_count),
        info_(std::move(info)),
        irq_info_(std::move(irq_info)),
        irq_status_(0) {}

  zx_status_t AmlPinToBlock(uint32_t pin, const AmlGpioBlock** out_block,
                            uint32_t* out_pin_index) const;

  void Bind(fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus);

  ddk::PDev pdev_;
  fbl::Mutex mmio_lock_;
  std::array<fdf::MmioBuffer, 2> mmios_ TA_GUARDED(mmio_lock_);  // separate MMIO for AO domain
  fdf::MmioBuffer mmio_interrupt_ TA_GUARDED(mmio_lock_);
  const AmlGpioBlock* gpio_blocks_;
  const AmlGpioInterrupt* gpio_interrupt_;
  size_t block_count_;
  const pdev_device_info_t info_;
  fbl::Mutex irq_lock_ TA_ACQ_BEFORE(mmio_lock_);  // Protects content in fields below
  fbl::Array<uint16_t> irq_info_ TA_GUARDED(irq_lock_);
  uint8_t irq_status_ TA_GUARDED(irq_lock_);
};

}  // namespace gpio

#endif  // SRC_DEVICES_GPIO_DRIVERS_AML_GPIO_AML_GPIO_H_
