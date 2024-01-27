// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "i2c.h"

#include <fidl/fuchsia.hardware.i2c.businfo/cpp/wire.h>
#include <fuchsia/hardware/i2cimpl/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <ddktl/device.h>
#include <zxtest/zxtest.h>

#include "lib/ddk/metadata.h"
#include "src/devices/i2c/drivers/i2c/i2c-child.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {
namespace fi2c = fuchsia_hardware_i2c_businfo::wire;

class FakeI2cImpl;
using DeviceType = ddk::Device<FakeI2cImpl>;

class FakeI2cImpl : public DeviceType,
                    public ddk::I2cImplProtocol<FakeI2cImpl, ddk::base_protocol> {
 public:
  explicit FakeI2cImpl(zx_device_t* parent, std::vector<uint8_t> metadata)
      : DeviceType(parent), metadata_(std::move(metadata)) {
    ASSERT_OK(DdkAdd("fake-i2c-impl"));
    zxdev()->SetMetadata(DEVICE_METADATA_I2C_CHANNELS, metadata_.data(), metadata_.size());
  }

  static FakeI2cImpl* Create(zx_device_t* parent, std::vector<fi2c::I2CChannel> channels) {
    auto channels_view = fidl::VectorView<fi2c::I2CChannel>::FromExternal(channels);

    fidl::Arena<> arena;
    auto metadata = fi2c::I2CBusMetadata::Builder(arena).channels(channels_view).Build();

    auto bytes = fidl::Persist(metadata);
    ZX_ASSERT(bytes.is_ok());
    auto impl = new FakeI2cImpl(parent, std::move(bytes.value()));
    return impl;
  }
  uint32_t I2cImplGetBusBase() { return 0; }
  uint32_t I2cImplGetBusCount() { return 1; }
  zx_status_t I2cImplGetMaxTransferSize(uint32_t bus_id, uint64_t* out_size) {
    *out_size = 64;
    return ZX_OK;
  }
  zx_status_t I2cImplSetBitrate(uint32_t bus_id, uint32_t bitrate) { return ZX_OK; }

  zx_status_t I2cImplTransact(uint32_t bus_id, const i2c_impl_op_t* op_list, size_t op_count) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  void DdkRelease() { delete this; }

 private:
  void AddMetadata() {}
  std::vector<uint8_t> metadata_;
};

fi2c::I2CChannel MakeChannel(fidl::AnyArena& arena, uint32_t bus_id, uint16_t addr) {
  return fi2c::I2CChannel::Builder(arena).address(addr).bus_id(bus_id).Build();
}

}  // namespace

class I2cMetadataTest : public zxtest::Test {
 public:
  I2cMetadataTest() : loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

  void SetUp() override {
    loop_.StartThread();
    fake_root_ = MockDevice::FakeRootParent();
  }

  void TearDown() override {
    for (auto& device : fake_root_->children()) {
      device_async_remove(device.get());
    }

    mock_ddk::ReleaseFlaggedDevices(fake_root_.get());
  }

 protected:
  async::Loop loop_;
  std::shared_ptr<zx_device> fake_root_;
};

TEST_F(I2cMetadataTest, ProvidesMetadataToChildren) {
  std::vector<fi2c::I2CChannel> channels;
  fidl::Arena<> arena;
  channels.emplace_back(MakeChannel(arena, 0, 0xa));
  channels.emplace_back(MakeChannel(arena, 0, 0xb));

  auto impl = FakeI2cImpl::Create(fake_root_.get(), std::move(channels));
  // Make the fake I2C driver.
  ASSERT_OK(i2c::I2cDevice::Create(nullptr, impl->zxdev(), loop_.dispatcher()));

  // Check the number of devices we have makes sense.
  ASSERT_EQ(impl->zxdev()->child_count(), 1);
  zx_device_t* i2c = impl->zxdev()->GetLatestChild();
  // There should be two devices per channel.
  ASSERT_EQ(i2c->child_count(), 2);

  for (auto& child : i2c->children()) {
    uint16_t expected_addr = 0xff;
    for (const auto& prop : child->GetProperties()) {
      if (prop.id == BIND_I2C_ADDRESS) {
        expected_addr = static_cast<uint16_t>(prop.value);
      }
    }

    auto decoded =
        ddk::GetEncodedMetadata<fi2c::I2CChannel>(child.get(), DEVICE_METADATA_I2C_DEVICE);
    ASSERT_TRUE(decoded.is_ok());
    ASSERT_EQ(decoded->address(), expected_addr);
  }
}
