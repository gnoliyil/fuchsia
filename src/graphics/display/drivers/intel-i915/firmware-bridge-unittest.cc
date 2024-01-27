// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915/firmware-bridge.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/loop.h>
#include <lib/device-protocol/pci.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/result.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "src/devices/pci/testing/pci_protocol_fake.h"

namespace i915_tgl {

namespace {

class PciConfigOpRegionTest : public ::testing::Test {
 public:
  void SetUp() override {
    loop_.StartThread("pci-fidl-server-thread");
    pci_ = fake_pci_.SetUpFidlServer(loop_);
    pci_op_region_.emplace(pci_);
  }

 protected:
  async::Loop loop_{&kAsyncLoopConfigNeverAttachToThread};
  pci::FakePciProtocol fake_pci_;
  ddk::Pci pci_;
  std::optional<PciConfigOpRegion> pci_op_region_;
};

TEST_F(PciConfigOpRegionTest, ReadMemoryOpRegionAddress) {
  pci_.WriteConfig32(0xfc, 0x42424242);

  const zx::result<zx_paddr_t> address = pci_op_region_->ReadMemoryOpRegionAddress();
  ASSERT_TRUE(address.is_ok()) << address.status_string();
  EXPECT_EQ(0x42424242u, address.value());
}

TEST_F(PciConfigOpRegionTest, ReadMemoryOpRegionAddressUnsupported) {
  pci_.WriteConfig32(0xfc, 0);

  const zx::result<zx_paddr_t> address = pci_op_region_->ReadMemoryOpRegionAddress();
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, address.status_value()) << address.status_string();
}

TEST_F(PciConfigOpRegionTest, ReadMemoryOpRegionAddressError) {
  auto pci_endpoints = fidl::CreateEndpoints<fuchsia_hardware_pci::Device>();
  ASSERT_TRUE(pci_endpoints.is_ok());
  auto [pci_client, pci_server] = std::move(pci_endpoints).value();
  pci_server.Close(ZX_OK);

  ddk::Pci disconnected_pci(std::move(pci_client));
  PciConfigOpRegion disconnected_pci_op_region(disconnected_pci);

  const zx::result<zx_paddr_t> address = disconnected_pci_op_region.ReadMemoryOpRegionAddress();
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, address.status_value()) << address.status_string();
}

TEST_F(PciConfigOpRegionTest, IsSystemControlInterruptInUse) {
  {
    pci_.WriteConfig16(0xe8, 0x8000);
    const zx::result<bool> in_use = pci_op_region_->IsSystemControlInterruptInUse();
    ASSERT_TRUE(in_use.is_ok()) << in_use.status_string();
    EXPECT_EQ(false, in_use.value());
  }
  {
    pci_.WriteConfig16(0xe8, 0x8001);
    const zx::result<bool> in_use = pci_op_region_->IsSystemControlInterruptInUse();
    ASSERT_TRUE(in_use.is_ok()) << in_use.status_string();
    EXPECT_EQ(true, in_use.value());
  }
}

TEST_F(PciConfigOpRegionTest, IsSystemControlInterruptInUseUnsupported) {
  {
    pci_.WriteConfig16(0xe8, 0x0000);
    const zx::result<bool> in_use = pci_op_region_->IsSystemControlInterruptInUse();
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, in_use.status_value());
  }
  {
    pci_.WriteConfig16(0xe8, 0x0001);
    const zx::result<bool> in_use = pci_op_region_->IsSystemControlInterruptInUse();
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, in_use.status_value());
  }
}

TEST_F(PciConfigOpRegionTest, IsSystemControlInterruptInUseError) {
  auto pci_endpoints = fidl::CreateEndpoints<fuchsia_hardware_pci::Device>();
  ASSERT_TRUE(pci_endpoints.is_ok());
  auto [pci_client, pci_server] = std::move(pci_endpoints).value();
  pci_server.Close(ZX_OK);

  ddk::Pci disconnected_pci(std::move(pci_client));
  PciConfigOpRegion disconnected_pci_op_region(disconnected_pci);

  const zx::result<bool> in_use = disconnected_pci_op_region.IsSystemControlInterruptInUse();
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, in_use.status_value()) << in_use.status_string();
}

TEST_F(PciConfigOpRegionTest, TriggerSystemControlInterrupt) {
  pci_.WriteConfig16(0xe8, 0x8000);

  const zx::result<> trigger_result = pci_op_region_->TriggerSystemControlInterrupt();
  ASSERT_TRUE(trigger_result.is_ok()) << trigger_result.status_string();

  uint16_t swsci_trigger_register = 0;
  EXPECT_EQ(ZX_OK, pci_.ReadConfig16(0xe8, &swsci_trigger_register));
  EXPECT_EQ(0x8001, swsci_trigger_register);
}

TEST_F(PciConfigOpRegionTest, TriggerSystemControlInterruptInUse) {
  pci_.WriteConfig16(0xe8, 0x8001);
  const zx::result<> result = pci_op_region_->TriggerSystemControlInterrupt();
  EXPECT_EQ(ZX_ERR_BAD_STATE, result.status_value()) << result.status_string();
}

TEST_F(PciConfigOpRegionTest, TriggerSystemControlInterruptUnsupported) {
  {
    pci_.WriteConfig16(0xe8, 0x0000);
    const zx::result<> result = pci_op_region_->TriggerSystemControlInterrupt();
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, result.status_value()) << result.status_string();
  }
  {
    pci_.WriteConfig16(0xe8, 0x0001);
    const zx::result<> result = pci_op_region_->TriggerSystemControlInterrupt();
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, result.status_value()) << result.status_string();
  }
}

TEST_F(PciConfigOpRegionTest, TriggerSystemControlInterruptPciError) {
  auto pci_endpoints = fidl::CreateEndpoints<fuchsia_hardware_pci::Device>();
  ASSERT_TRUE(pci_endpoints.is_ok());
  auto [pci_client, pci_server] = std::move(pci_endpoints).value();
  pci_server.Close(ZX_OK);

  ddk::Pci disconnected_pci(std::move(pci_client));
  PciConfigOpRegion disconnected_pci_op_region(disconnected_pci);

  const zx::result<> result = disconnected_pci_op_region.TriggerSystemControlInterrupt();
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, result.status_value()) << result.status_string();
}

}  // namespace

}  // namespace i915_tgl
