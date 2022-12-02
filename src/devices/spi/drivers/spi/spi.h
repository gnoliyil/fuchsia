// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SPI_DRIVERS_SPI_SPI_H_
#define SRC_DEVICES_SPI_DRIVERS_SPI_SPI_H_

#include <fuchsia/hardware/spiimpl/cpp/banjo.h>

#include <ddktl/device.h>

namespace spi {

class SpiDevice;
using SpiDeviceType = ddk::Device<SpiDevice>;

class SpiDevice : public SpiDeviceType {
 public:
  SpiDevice(zx_device_t* parent, uint32_t bus_id) : SpiDeviceType(parent), bus_id_(bus_id) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent, async_dispatcher_t* dispatcher);

  void DdkRelease();

 private:
  void AddChildren(const ddk::SpiImplProtocolClient& spi, async_dispatcher_t* dispatcher);

  const uint32_t bus_id_;
};

}  // namespace spi

#endif  // SRC_DEVICES_SPI_DRIVERS_SPI_SPI_H_
