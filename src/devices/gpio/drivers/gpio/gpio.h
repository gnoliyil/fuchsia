// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_GPIO_DRIVERS_GPIO_GPIO_H_
#define SRC_DEVICES_GPIO_DRIVERS_GPIO_GPIO_H_

#include <fidl/fuchsia.hardware.gpio/cpp/wire.h>
#include <fidl/fuchsia.hardware.gpioimpl/cpp/driver/wire.h>
#include <fuchsia/hardware/gpio/cpp/banjo.h>
#include <fuchsia/hardware/gpioimpl/cpp/banjo.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/ddk/platform-defs.h>
#include <lib/zircon-internal/thread_annotations.h>

#include <string_view>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/mutex.h>

namespace gpio {

class GpioDevice;
using GpioDeviceType =
    ddk::Device<GpioDevice, ddk::Messageable<fuchsia_hardware_gpio::Gpio>::Mixin, ddk::Unbindable>;

static_assert(GPIO_PULL_DOWN ==
                  static_cast<uint32_t>(fuchsia_hardware_gpio::wire::GpioFlags::kPullDown),
              "ConfigIn PULL_DOWN flag doesn't match.");
static_assert(GPIO_PULL_UP ==
                  static_cast<uint32_t>(fuchsia_hardware_gpio::wire::GpioFlags::kPullUp),
              "ConfigIn PULL_UP flag doesn't match.");
static_assert(GPIO_NO_PULL ==
                  static_cast<uint32_t>(fuchsia_hardware_gpio::wire::GpioFlags::kNoPull),
              "ConfigIn NO_PULL flag doesn't match.");

class GpioImplProxy {
 public:
  GpioImplProxy(const ddk::GpioImplProtocolClient& gpio_banjo,
                fdf::WireSyncClient<fuchsia_hardware_gpioimpl::GpioImpl> gpio_fidl)
      : gpio_banjo_(gpio_banjo), gpio_fidl_(std::move(gpio_fidl)) {}

  zx_status_t ConfigIn(uint32_t index, uint32_t flags) const;
  zx_status_t ConfigOut(uint32_t index, uint8_t initial_value) const;
  zx_status_t SetAltFunction(uint32_t index, uint64_t function) const;
  zx_status_t SetDriveStrength(uint32_t index, uint64_t ua, uint64_t* out_actual_ua) const;
  zx_status_t GetDriveStrength(uint32_t index, uint64_t* out_value) const;
  zx_status_t Read(uint32_t index, uint8_t* out_value) const;
  zx_status_t Write(uint32_t index, uint8_t value) const;
  zx_status_t GetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq) const;
  zx_status_t ReleaseInterrupt(uint32_t index) const;
  zx_status_t SetPolarity(uint32_t index, gpio_polarity_t polarity) const;

 private:
  ddk::GpioImplProtocolClient gpio_banjo_;
  fdf::WireSyncClient<fuchsia_hardware_gpioimpl::GpioImpl> gpio_fidl_;
};

class GpioDevice : public GpioDeviceType, public ddk::GpioProtocol<GpioDevice, ddk::base_protocol> {
 public:
  GpioDevice(zx_device_t* parent, GpioImplProxy gpio, uint32_t pin, std::string_view name)
      : GpioDeviceType(parent), gpio_(std::move(gpio)), pin_(pin), name_(name) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  zx_status_t InitAddDevice(uint32_t controller_id);

  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  zx_status_t GpioGetPin(uint32_t* pin);
  zx_status_t GpioGetName(char* out_name);
  zx_status_t GpioConfigIn(uint32_t flags);
  zx_status_t GpioConfigOut(uint8_t initial_value);
  zx_status_t GpioSetAltFunction(uint64_t function);
  zx_status_t GpioRead(uint8_t* out_value);
  zx_status_t GpioWrite(uint8_t value);
  zx_status_t GpioGetInterrupt(uint32_t flags, zx::interrupt* out_irq);
  zx_status_t GpioReleaseInterrupt();
  zx_status_t GpioSetPolarity(gpio_polarity_t polarity);
  zx_status_t GpioSetDriveStrength(uint64_t ds_ua, uint64_t* out_actual_ds_ua);
  zx_status_t GpioGetDriveStrength(uint64_t* ds_ua);

  // FIDL

  void GetPin(GetPinCompleter::Sync& completer) override { completer.ReplySuccess(pin_); }
  void GetName(GetNameCompleter::Sync& completer) override {
    completer.ReplySuccess(::fidl::StringView::FromExternal(name_));
  }
  void ConfigIn(ConfigInRequestView request, ConfigInCompleter::Sync& completer) override {
    zx_status_t status = GpioConfigIn(static_cast<uint32_t>(request->flags));
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }
  void ConfigOut(ConfigOutRequestView request, ConfigOutCompleter::Sync& completer) override {
    zx_status_t status = GpioConfigOut(request->initial_value);
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }
  void Read(ReadCompleter::Sync& completer) override {
    uint8_t value = 0;
    zx_status_t status = GpioRead(&value);
    if (status == ZX_OK) {
      completer.ReplySuccess(value);
    } else {
      completer.ReplyError(status);
    }
  }
  void Write(WriteRequestView request, WriteCompleter::Sync& completer) override {
    zx_status_t status = GpioWrite(request->value);
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }
  void SetDriveStrength(SetDriveStrengthRequestView request,
                        SetDriveStrengthCompleter::Sync& completer) override {
    uint64_t actual = 0;
    zx_status_t status = GpioSetDriveStrength(request->ds_ua, &actual);
    if (status == ZX_OK) {
      completer.ReplySuccess(actual);
    } else {
      completer.ReplyError(status);
    }
  }
  void GetDriveStrength(GetDriveStrengthCompleter::Sync& completer) override {
    uint64_t result_ua = 0;
    zx_status_t status = GpioGetDriveStrength(&result_ua);
    if (status == ZX_OK) {
      completer.ReplySuccess(result_ua);
    } else {
      completer.ReplyError(status);
    }
  }
  void GetInterrupt(GetInterruptRequestView request,
                    GetInterruptCompleter::Sync& completer) override {
    zx::interrupt interrupt;
    zx_status_t status = GpioGetInterrupt(request->flags, &interrupt);
    if (status == ZX_OK) {
      completer.ReplySuccess(std::move(interrupt));
    } else {
      completer.ReplyError(status);
    }
  }
  void ReleaseInterrupt(ReleaseInterruptCompleter::Sync& completer) override {
    zx_status_t status = GpioReleaseInterrupt();
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }

  void SetAltFunction(SetAltFunctionRequestView request,
                      SetAltFunctionCompleter::Sync& completer) override {
    zx_status_t status = GpioSetAltFunction(request->function);
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }

  void SetPolarity(SetPolarityRequestView request, SetPolarityCompleter::Sync& completer) override {
    zx_status_t status = GpioSetPolarity(static_cast<uint32_t>(request->polarity));
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }

 private:
  const GpioImplProxy gpio_ TA_GUARDED(lock_);
  const uint32_t pin_;
  const std::string name_;
  using Binding = struct {
    fidl::ServerBindingRef<fuchsia_hardware_gpio::Gpio> binding;
    std::optional<ddk::UnbindTxn> unbind_txn;
  };

  std::optional<component::OutgoingDirectory> outgoing_;
  fidl::ServerBindingGroup<fuchsia_hardware_gpio::Gpio> bindings_;

  std::optional<Binding> binding_;
  fbl::Mutex lock_;
};

class GpioInitDevice;
using GpioInitDeviceType = ddk::Device<GpioInitDevice>;

class GpioInitDevice : public GpioInitDeviceType {
 public:
  static void Create(zx_device_t* parent, GpioImplProxy gpio, uint32_t controller_id);

  explicit GpioInitDevice(zx_device_t* parent) : GpioInitDeviceType(parent) {}

  void DdkRelease() { delete this; }

 private:
  static zx_status_t ConfigureGpios(const fuchsia_hardware_gpioimpl::wire::InitMetadata& metadata,
                                    const GpioImplProxy& gpio);
};

}  // namespace gpio

#endif  // SRC_DEVICES_GPIO_DRIVERS_GPIO_GPIO_H_
