// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_POWER_DRIVERS_TI_INA231_TI_INA231_H_
#define SRC_DEVICES_POWER_DRIVERS_TI_INA231_TI_INA231_H_

#include <fidl/fuchsia.hardware.power.sensor/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/device-protocol/i2c-channel.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/result.h>

#include <ddktl/device.h>
#include <fbl/mutex.h>

#include "ti-ina231-metadata.h"

namespace power_sensor {

namespace power_sensor_fidl = fuchsia_hardware_power_sensor;

class Ina231Device;
using DeviceType =
    ddk::Device<Ina231Device, ddk::Messageable<fuchsia_hardware_power_sensor::Device>::Mixin>;

class Ina231Device : public DeviceType {
 public:
  Ina231Device(zx_device_t* parent, uint32_t shunt_resistor_uohms, ddk::I2cChannel i2c)
      : DeviceType(parent),
        shunt_resistor_uohms_(shunt_resistor_uohms),
        loop_(&kAsyncLoopConfigNeverAttachToThread),
        i2c_(std::move(i2c)) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  void DdkRelease() { delete this; }

  zx_status_t PowerSensorConnectServer(zx::channel server);

  void GetPowerWatts(GetPowerWattsCompleter::Sync& completer) override;
  void GetVoltageVolts(GetVoltageVoltsCompleter::Sync& completer) override;

  // Visible for testing.
  zx_status_t Init(const Ina231Metadata& metadata);

 private:
  enum class Register : uint8_t;

  zx::result<uint16_t> Read16(Register reg) TA_REQ(i2c_lock_);
  zx::result<> Write16(Register reg, uint16_t value) TA_REQ(i2c_lock_);

  const uint32_t shunt_resistor_uohms_;
  async::Loop loop_;
  fbl::Mutex i2c_lock_;
  ddk::I2cChannel i2c_ TA_GUARDED(i2c_lock_);
  fidl::ServerEnd<fuchsia_io::Directory> outgoing_server_end_;
};

}  // namespace power_sensor

#endif  // SRC_DEVICES_POWER_DRIVERS_TI_INA231_TI_INA231_H_
