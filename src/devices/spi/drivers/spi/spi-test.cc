// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "spi.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/fidl/cpp/wire/client.h>
#include <lib/spi/spi.h>
#include <zircon/errors.h>

#include <latch>
#include <map>

#include <zxtest/zxtest.h>

#include "spi-child.h"
#include "src/devices/lib/fidl-metadata/spi.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace spi {
using spi_channel_t = fidl_metadata::spi::Channel;

class FakeDdkSpiImpl;

class FakeDdkSpiImpl : public ddk::SpiImplProtocol<FakeDdkSpiImpl, ddk::base_protocol> {
 public:
  spi_impl_protocol_ops_t* ops() { return &spi_impl_protocol_ops_; }

  uint32_t SpiImplGetChipSelectCount() { return 2; }

  zx_status_t SpiImplExchange(uint32_t cs, const uint8_t* txdata, size_t txdata_size,
                              uint8_t* out_rxdata, size_t rxdata_size, size_t* out_rxdata_actual) {
    EXPECT_EQ(cs, current_test_cs_, "");

    switch (test_mode_) {
      case SpiTestMode::kTransmit:
        EXPECT_NE(txdata, nullptr, "");
        EXPECT_NE(txdata_size, 0, "");
        EXPECT_EQ(out_rxdata, nullptr, "");
        EXPECT_EQ(rxdata_size, 0, "");
        *out_rxdata_actual = 0;
        break;
      case SpiTestMode::kReceive:
        EXPECT_EQ(txdata, nullptr, "");
        EXPECT_EQ(txdata_size, 0, "");
        EXPECT_NE(out_rxdata, nullptr, "");
        EXPECT_NE(rxdata_size, 0, "");
        memset(out_rxdata, 0, rxdata_size);
        memcpy(out_rxdata, kTestData, std::min(rxdata_size, sizeof(kTestData)));
        *out_rxdata_actual = rxdata_size + (corrupt_rx_actual_ ? 1 : 0);
        break;
      case SpiTestMode::kExchange:
        EXPECT_NE(txdata, nullptr, "");
        EXPECT_NE(txdata_size, 0, "");
        EXPECT_NE(out_rxdata, nullptr, "");
        EXPECT_NE(rxdata_size, 0, "");
        EXPECT_EQ(txdata_size, rxdata_size, "");
        memset(out_rxdata, 0, rxdata_size);
        memcpy(out_rxdata, txdata, std::min(rxdata_size, txdata_size));
        *out_rxdata_actual = std::min(rxdata_size, txdata_size) + (corrupt_rx_actual_ ? 1 : 0);
        break;
    }

    return ZX_OK;
  }

  zx_status_t SpiImplRegisterVmo(uint32_t chip_select, uint32_t vmo_id, zx::vmo vmo,
                                 uint64_t offset, uint64_t size, uint32_t rights) {
    if (chip_select > 1) {
      return ZX_ERR_OUT_OF_RANGE;
    }

    std::map<uint32_t, zx::vmo>& map = chip_select == 0 ? cs0_vmos : cs1_vmos;
    if (map.find(vmo_id) != map.end()) {
      return ZX_ERR_ALREADY_EXISTS;
    }

    map[vmo_id] = std::move(vmo);
    return ZX_OK;
  }

  zx_status_t SpiImplUnregisterVmo(uint32_t chip_select, uint32_t vmo_id, zx::vmo* out_vmo) {
    if (chip_select > 1) {
      return ZX_ERR_OUT_OF_RANGE;
    }

    std::map<uint32_t, zx::vmo>& map = chip_select == 0 ? cs0_vmos : cs1_vmos;
    auto it = map.find(vmo_id);
    if (it == map.end()) {
      return ZX_ERR_NOT_FOUND;
    }

    if (out_vmo) {
      out_vmo->reset(std::get<1>(*it).release());
    }

    map.erase(it);
    return ZX_OK;
  }

  void SpiImplReleaseRegisteredVmos(uint32_t chip_select) { vmos_released_since_last_call_ = true; }

  zx_status_t SpiImplTransmitVmo(uint32_t chip_select, uint32_t vmo_id, uint64_t offset,
                                 uint64_t size) {
    if (chip_select > 1) {
      return ZX_ERR_OUT_OF_RANGE;
    }

    std::map<uint32_t, zx::vmo>& map = chip_select == 0 ? cs0_vmos : cs1_vmos;
    auto it = map.find(vmo_id);
    if (it == map.end()) {
      return ZX_ERR_NOT_FOUND;
    }

    uint8_t buf[sizeof(kTestData)];
    zx_status_t status = std::get<1>(*it).read(buf, offset, std::max(size, sizeof(buf)));
    if (status != ZX_OK) {
      return status;
    }

    return memcmp(buf, kTestData, std::max(size, sizeof(buf))) == 0 ? ZX_OK : ZX_ERR_IO;
  }

  zx_status_t SpiImplReceiveVmo(uint32_t chip_select, uint32_t vmo_id, uint64_t offset,
                                uint64_t size) {
    if (chip_select > 1) {
      return ZX_ERR_OUT_OF_RANGE;
    }

    std::map<uint32_t, zx::vmo>& map = chip_select == 0 ? cs0_vmos : cs1_vmos;
    auto it = map.find(vmo_id);
    if (it == map.end()) {
      return ZX_ERR_NOT_FOUND;
    }

    return std::get<1>(*it).write(kTestData, offset, std::max(size, sizeof(kTestData)));
  }

  zx_status_t SpiImplExchangeVmo(uint32_t chip_select, uint32_t tx_vmo_id, uint64_t tx_offset,
                                 uint32_t rx_vmo_id, uint64_t rx_offset, uint64_t size) {
    if (chip_select > 1) {
      return ZX_ERR_OUT_OF_RANGE;
    }

    std::map<uint32_t, zx::vmo>& map = chip_select == 0 ? cs0_vmos : cs1_vmos;
    auto tx_it = map.find(tx_vmo_id);
    auto rx_it = map.find(rx_vmo_id);

    if (tx_it == map.end() || rx_it == map.end()) {
      return ZX_ERR_NOT_FOUND;
    }

    uint8_t buf[8];
    zx_status_t status = std::get<1>(*tx_it).read(buf, tx_offset, std::max(size, sizeof(buf)));
    if (status != ZX_OK) {
      return status;
    }

    return std::get<1>(*rx_it).write(buf, rx_offset, std::max(size, sizeof(buf)));
  }

  bool vmos_released_since_last_call() {
    const bool value = vmos_released_since_last_call_;
    vmos_released_since_last_call_ = false;
    return value;
  }

  zx_status_t SpiImplLockBus(uint32_t chip_select) { return ZX_OK; }
  zx_status_t SpiImplUnlockBus(uint32_t chip_select) { return ZX_OK; }

  SpiDevice* bus_device_;
  uint32_t current_test_cs_ = 0;
  bool corrupt_rx_actual_ = false;
  bool vmos_released_since_last_call_ = false;

  enum class SpiTestMode {
    kTransmit,
    kReceive,
    kExchange,
  } test_mode_;

  static constexpr uint32_t kTestBusId = 0;
  static constexpr spi_channel_t kSpiChannels[] = {
      {.bus_id = 0, .cs = 0, .vid = 0, .pid = 0, .did = 0},
      {.bus_id = 0, .cs = 1, .vid = 0, .pid = 0, .did = 0}};

  std::map<uint32_t, zx::vmo> cs0_vmos;
  std::map<uint32_t, zx::vmo> cs1_vmos;

 private:
  static constexpr uint8_t kTestData[] = {1, 2, 3, 4, 5, 6, 7};
};

class SpiDeviceTest : public zxtest::Test {
 protected:
  SpiDeviceTest() : loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

  static constexpr uint32_t kTestBusId = 0;
  static constexpr spi_channel_t kSpiChannels[] = {
      {.bus_id = 0, .cs = 0, .vid = 0, .pid = 0, .did = 0},
      {.bus_id = 0, .cs = 1, .vid = 0, .pid = 0, .did = 0},
  };

  void SetUp() override {
    parent_ = MockDevice::FakeRootParent();
    ASSERT_OK(loop_.StartThread("spi-test-thread"));

    parent_->AddProtocol(ZX_PROTOCOL_SPI_IMPL, spi_impl_.ops(), &spi_impl_);

    SetSpiChannelMetadata(kSpiChannels, std::size(kSpiChannels));
    parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kTestBusId, sizeof(kTestBusId));
  }

  void CreateSpiDevice() {
    std::latch done(1);
    async::PostTask(loop_.dispatcher(), [&]() {
      SpiDevice::Create(nullptr, parent_.get(), loop_.dispatcher());
      done.count_down();
    });
    done.wait();
  }

  void RemoveDevice(MockDevice* device) {
    device_async_remove(device);
    ASSERT_OK(mock_ddk::ReleaseFlaggedDevices(parent_.get(), loop_.dispatcher()));
  }

  zx::result<fidl::ClientEnd<fuchsia_hardware_spi::Device>> BindServer(
      const std::shared_ptr<MockDevice>& child) {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    if (endpoints.is_error()) {
      return endpoints.take_error();
    }
    auto& [client, server] = endpoints.value();
    SpiFidlChild* fidl = child->children().front()->GetDeviceContext<SpiFidlChild>();
    // The outgoing directory isn't thread safe; we must interact with it from the dispatcher
    // thread.
    async::PostTask(loop_.dispatcher(), [fidl, server = std::move(server)]() mutable {
      ASSERT_OK(fidl->ServeOutgoingDirectory(std::move(server)));
    });
    return component::ConnectAt<fuchsia_hardware_spi::Device>(
        client, fidl::DiscoverableProtocolDefaultPath<fuchsia_hardware_spi::Device>);
  }

  void SetSpiChannelMetadata(const spi_channel_t* channels, size_t count) {
    const auto result =
        fidl_metadata::spi::SpiChannelsToFidl(cpp20::span<const spi_channel_t>(channels, count));
    ASSERT_OK(result.status_value());
    parent_->SetMetadata(DEVICE_METADATA_SPI_CHANNELS, result->data(), result->size());
  }

  std::shared_ptr<MockDevice> parent_;
  FakeDdkSpiImpl spi_impl_;
  async::Loop loop_;
};

TEST_F(SpiDeviceTest, SpiTest) {
  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), std::size(kSpiChannels));

  // test it
  uint8_t txbuf[] = {0, 1, 2, 3, 4, 5, 6};
  uint8_t rxbuf[sizeof txbuf];

  uint32_t i = 0;
  for (auto it = spi_bus->children().begin(); it != spi_bus->children().end(); it++, i++) {
    spi_impl_.current_test_cs_ = i;

    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_spi::Device>();
    ASSERT_OK(endpoints);
    auto& [client, server] = endpoints.value();
    fidl::BindServer(loop_.dispatcher(), std::move(server), (*it)->GetDeviceContext<SpiChild>());

    spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kTransmit;
    EXPECT_OK(spilib_transmit(client, txbuf, sizeof txbuf));

    spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kReceive;
    EXPECT_OK(spilib_receive(client, rxbuf, sizeof rxbuf));

    spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kExchange;
    EXPECT_OK(spilib_exchange(client, txbuf, rxbuf, sizeof txbuf));
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, SpiFidlVmoTest) {
  using fuchsia_hardware_sharedmemory::wire::SharedVmoRight;

  constexpr uint8_t kTestData[] = {1, 2, 3, 4, 5, 6, 7};

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), std::size(kSpiChannels));

  fidl::WireSharedClient<fuchsia_hardware_spi::Device> cs0_client, cs1_client;

  {
    const auto& child0 = spi_bus->children().front();
    zx::result client = BindServer(child0);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  {
    const auto& child1 = *++spi_bus->children().begin();
    zx::result client = BindServer(child1);
    ASSERT_OK(client);
    cs1_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  zx::vmo cs0_vmo, cs1_vmo;
  ASSERT_OK(zx::vmo::create(4096, 0, &cs0_vmo));
  ASSERT_OK(zx::vmo::create(4096, 0, &cs1_vmo));

  {
    fuchsia_mem::wire::Range vmo = {.offset = 0, .size = 4096};
    ASSERT_OK(cs0_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo.vmo));
    auto result = cs0_client.sync()->RegisterVmo(1, std::move(vmo),
                                                 SharedVmoRight::kRead | SharedVmoRight::kWrite);
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());
  }

  {
    fuchsia_mem::wire::Range vmo = {.offset = 0, .size = 4096};
    ASSERT_OK(cs1_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo.vmo));
    auto result = cs1_client.sync()->RegisterVmo(2, std::move(vmo),
                                                 SharedVmoRight::kRead | SharedVmoRight::kWrite);
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());
  }

  ASSERT_OK(cs0_vmo.write(kTestData, 1024, sizeof(kTestData)));
  {
    auto result = cs0_client.sync()->Exchange(
        {
            .vmo_id = 1,
            .offset = 1024,
            .size = sizeof(kTestData),
        },
        {
            .vmo_id = 1,
            .offset = 2048,
            .size = sizeof(kTestData),
        });
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());

    uint8_t buf[sizeof(kTestData)];
    ASSERT_OK(cs0_vmo.read(buf, 2048, sizeof(buf)));
    EXPECT_BYTES_EQ(buf, kTestData, sizeof(buf));
  }

  ASSERT_OK(cs1_vmo.write(kTestData, 1024, sizeof(kTestData)));
  {
    auto result = cs1_client.sync()->Transmit({
        .vmo_id = 2,
        .offset = 1024,
        .size = sizeof(kTestData),
    });
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());
  }

  {
    auto result = cs0_client.sync()->Receive({
        .vmo_id = 1,
        .offset = 1024,
        .size = sizeof(kTestData),
    });
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());

    uint8_t buf[sizeof(kTestData)];
    ASSERT_OK(cs0_vmo.read(buf, 1024, sizeof(buf)));
    EXPECT_BYTES_EQ(buf, kTestData, sizeof(buf));
  }

  {
    auto result = cs0_client.sync()->UnregisterVmo(1);
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());
  }

  {
    auto result = cs1_client.sync()->UnregisterVmo(2);
    ASSERT_OK(result.status());
    EXPECT_TRUE(result.value().is_ok());
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, SpiFidlVectorTest) {
  fidl::WireSharedClient<fuchsia_hardware_spi::Device> cs0_client, cs1_client;

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), std::size(kSpiChannels));

  {
    const auto& child0 = spi_bus->children().front();
    zx::result client = BindServer(child0);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  {
    const auto& child1 = *++spi_bus->children().begin();
    zx::result client = BindServer(child1);
    ASSERT_OK(client);
    cs1_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  uint8_t test_data[] = {1, 2, 3, 4, 5, 6, 7};

  spi_impl_.current_test_cs_ = 0;
  spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kTransmit;
  {
    auto tx_buffer = fidl::VectorView<uint8_t>::FromExternal(test_data);
    auto result = cs0_client.sync()->TransmitVector(tx_buffer);
    ASSERT_OK(result.status());
    EXPECT_OK(result.value().status);
  }

  spi_impl_.current_test_cs_ = 1;
  spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kReceive;
  {
    auto result = cs1_client.sync()->ReceiveVector(sizeof(test_data));
    ASSERT_OK(result.status());
    EXPECT_OK(result.value().status);
    ASSERT_EQ(result.value().data.count(), std::size(test_data));
    EXPECT_BYTES_EQ(result.value().data.data(), test_data, sizeof(test_data));
  }

  spi_impl_.current_test_cs_ = 0;
  spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kExchange;
  {
    auto tx_buffer = fidl::VectorView<uint8_t>::FromExternal(test_data);
    auto result = cs0_client.sync()->ExchangeVector(tx_buffer);
    ASSERT_OK(result.status());
    EXPECT_OK(result.value().status);
    ASSERT_EQ(result.value().rxdata.count(), std::size(test_data));
    EXPECT_BYTES_EQ(result.value().rxdata.data(), test_data, sizeof(test_data));
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, SpiFidlVectorErrorTest) {
  fidl::WireSharedClient<fuchsia_hardware_spi::Device> cs0_client, cs1_client;

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), std::size(kSpiChannels));

  {
    const auto& child0 = spi_bus->children().front();
    zx::result client = BindServer(child0);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  {
    const auto& child1 = *++spi_bus->children().begin();
    zx::result client = BindServer(child1);
    ASSERT_OK(client);
    cs1_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  spi_impl_.corrupt_rx_actual_ = true;

  uint8_t test_data[] = {1, 2, 3, 4, 5, 6, 7};

  spi_impl_.current_test_cs_ = 0;
  spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kTransmit;
  {
    auto tx_buffer = fidl::VectorView<uint8_t>::FromExternal(test_data);
    auto result = cs0_client.sync()->TransmitVector(tx_buffer);
    ASSERT_OK(result.status());
    EXPECT_OK(result.value().status);
  }

  spi_impl_.current_test_cs_ = 1;
  spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kReceive;
  {
    auto result = cs1_client.sync()->ReceiveVector(sizeof(test_data));
    ASSERT_OK(result.status());
    EXPECT_EQ(result.value().status, ZX_ERR_INTERNAL);
    EXPECT_EQ(result.value().data.count(), 0);
  }

  spi_impl_.current_test_cs_ = 0;
  spi_impl_.test_mode_ = FakeDdkSpiImpl::SpiTestMode::kExchange;
  {
    auto tx_buffer = fidl::VectorView<uint8_t>::FromExternal(test_data);
    auto result = cs0_client.sync()->ExchangeVector(tx_buffer);
    ASSERT_OK(result.status());
    EXPECT_EQ(result.value().status, ZX_ERR_INTERNAL);
    EXPECT_EQ(result.value().rxdata.count(), 0);
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, AssertCsWithSiblingTest) {
  fidl::WireSharedClient<fuchsia_hardware_spi::Device> cs0_client, cs1_client;

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), std::size(kSpiChannels));

  {
    const auto& child0 = spi_bus->children().front();
    zx::result client = BindServer(child0);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  {
    const auto& child1 = *++spi_bus->children().begin();
    zx::result client = BindServer(child1);
    ASSERT_OK(client);
    cs1_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  {
    auto result = cs0_client.sync()->CanAssertCs();
    ASSERT_OK(result.status());
    ASSERT_FALSE(result.value().can);
  }

  {
    auto result = cs1_client.sync()->CanAssertCs();
    ASSERT_OK(result.status());
    ASSERT_FALSE(result.value().can);
  }

  {
    auto result = cs0_client.sync()->AssertCs();
    ASSERT_OK(result.status());
    ASSERT_STATUS(result.value().status, ZX_ERR_NOT_SUPPORTED);
  }

  {
    auto result = cs1_client.sync()->AssertCs();
    ASSERT_OK(result.status());
    ASSERT_STATUS(result.value().status, ZX_ERR_NOT_SUPPORTED);
  }

  {
    auto result = cs0_client.sync()->DeassertCs();
    ASSERT_OK(result.status());
    ASSERT_STATUS(result.value().status, ZX_ERR_NOT_SUPPORTED);
  }

  {
    auto result = cs1_client.sync()->DeassertCs();
    ASSERT_OK(result.status());
    ASSERT_STATUS(result.value().status, ZX_ERR_NOT_SUPPORTED);
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, AssertCsNoSiblingTest) {
  SetSpiChannelMetadata(kSpiChannels, 1);

  fidl::WireSharedClient<fuchsia_hardware_spi::Device> cs0_client;

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), 1);

  {
    const auto& child0 = spi_bus->children().front();
    zx::result client = BindServer(child0);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()), loop_.dispatcher());
  }

  {
    auto result = cs0_client.sync()->CanAssertCs();
    ASSERT_OK(result.status());
    ASSERT_TRUE(result.value().can);
  }

  {
    auto result = cs0_client.sync()->AssertCs();
    ASSERT_OK(result.status());
    ASSERT_OK(result.value().status);
  }

  {
    auto result = cs0_client.sync()->DeassertCs();
    ASSERT_OK(result.status());
    ASSERT_OK(result.value().status);
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, OneClient) {
  SetSpiChannelMetadata(kSpiChannels, 1);

  fidl::WireSyncClient<fuchsia_hardware_spi::Device> cs0_client;

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), 1);

  ASSERT_EQ(spi_bus->children().front()->child_count(), 2);

  std::shared_ptr<MockDevice> spi_child = nullptr;
  for (auto& child : spi_bus->children()) {
    if (std::string_view(child->name()) == "spi-0-0") {
      spi_child = child;
      break;
    }
  }
  ASSERT_NOT_NULL(spi_child);

  // Establish a FIDL connection and verify that it works.
  {
    zx::result client = BindServer(spi_child);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()));
  }

  {
    auto result = cs0_client->CanAssertCs();
    ASSERT_OK(result.status());
    ASSERT_TRUE(result.value().can);
  }

  // Trying to make a new connection should fail.
  {
    zx::result client = BindServer(spi_child);
    ASSERT_OK(client);
    fidl::WireSyncClient cs0_client_1(std::move(client.value()));
    EXPECT_STATUS(cs0_client_1->CanAssertCs().status(), ZX_ERR_PEER_CLOSED);
  }

  EXPECT_FALSE(spi_impl_.vmos_released_since_last_call());

  // Close the first client so that another one can connect.
  cs0_client = {};

  // We don't know when the driver will be ready for a new client, just loop
  // until the connection is established.
  for (;;) {
    zx::result client = BindServer(spi_child);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()));

    auto result = cs0_client->CanAssertCs();
    if (result.ok()) {
      break;
    }
    EXPECT_STATUS(result.status(), ZX_ERR_PEER_CLOSED);
    cs0_client = {};
  }

  EXPECT_TRUE(spi_impl_.vmos_released_since_last_call());

  // OpenSession should fail when another client is connected.
  {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_spi::Controller>();
    ASSERT_OK(endpoints);
    auto& [controller, server] = endpoints.value();
    fidl::BindServer(loop_.dispatcher(), std::move(server),
                     spi_child->GetDeviceContext<SpiChild>());
    {
      zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_spi::Device>();
      ASSERT_OK(endpoints);
      auto& [device, server] = endpoints.value();
      ASSERT_OK(fidl::WireCall(controller)->OpenSession(std::move(server)));
      ASSERT_STATUS(fidl::WireCall(device)->CanAssertCs(), ZX_ERR_PEER_CLOSED);
    }
  }

  // Close the first client and make sure OpenSession now works.
  cs0_client = {};

  fidl::ClientEnd<fuchsia_hardware_spi::Device> device;
  while (true) {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_spi::Controller>();
    ASSERT_OK(endpoints);
    auto& [controller, server] = endpoints.value();
    fidl::BindServer(loop_.dispatcher(), std::move(server),
                     spi_child->GetDeviceContext<SpiChild>());
    {
      zx::result server = fidl::CreateEndpoints<fuchsia_hardware_spi::Device>(&device);
      ASSERT_OK(server);
      ASSERT_OK(fidl::WireCall(controller)->OpenSession(std::move(server.value())));
      auto result = fidl::WireCall(device)->CanAssertCs();
      if (result.ok()) {
        break;
      }
      ASSERT_STATUS(result, ZX_ERR_PEER_CLOSED);
    }
  }

  EXPECT_TRUE(spi_impl_.vmos_released_since_last_call());

  // FIDL clients shouldn't be able to connect, and calling OpenSession a second time should fail.
  {
    zx::result client = BindServer(spi_child);
    ASSERT_OK(client);
    fidl::WireSyncClient cs0_client_1(std::move(client.value()));
    EXPECT_STATUS(cs0_client_1->CanAssertCs().status(), ZX_ERR_PEER_CLOSED);
  }

  {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_spi::Controller>();
    ASSERT_OK(endpoints);
    auto& [controller, server] = endpoints.value();
    fidl::BindServer(loop_.dispatcher(), std::move(server),
                     spi_child->GetDeviceContext<SpiChild>());
    {
      zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_spi::Device>();
      ASSERT_OK(endpoints);
      auto& [device, server] = endpoints.value();
      ASSERT_OK(fidl::WireCall(controller)->OpenSession(std::move(server)));
      ASSERT_STATUS(fidl::WireCall(device)->CanAssertCs(), ZX_ERR_PEER_CLOSED);
    }
  }

  // Close the open session and make sure that a new client can now connect.
  device = {};

  for (;;) {
    zx::result client = BindServer(spi_child);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()));

    auto result = cs0_client->CanAssertCs();
    if (result.ok()) {
      break;
    }
  }

  EXPECT_TRUE(spi_impl_.vmos_released_since_last_call());

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);
}

TEST_F(SpiDeviceTest, DdkLifecycle) {
  SetSpiChannelMetadata(kSpiChannels, 1);

  fidl::WireSyncClient<fuchsia_hardware_spi::Device> cs0_client;

  CreateSpiDevice();
  auto* const spi_bus = parent_->GetLatestChild();
  ASSERT_NOT_NULL(spi_bus);
  EXPECT_EQ(spi_bus->child_count(), 1);

  {
    const auto& child0 = spi_bus->children().front();
    zx::result client = BindServer(child0);
    ASSERT_OK(client);
    cs0_client.Bind(std::move(client.value()));
  }

  {
    auto result = cs0_client->AssertCs();
    ASSERT_OK(result.status());
    EXPECT_OK(result.value().status);
  }

  RemoveDevice(spi_bus->children().front().get());
  EXPECT_EQ(spi_bus->descendant_count(), 0);

  {
    auto result = cs0_client->DeassertCs();
    EXPECT_STATUS(result.status(), ZX_ERR_PEER_CLOSED);
  }

  RemoveDevice(spi_bus);
  EXPECT_EQ(parent_->descendant_count(), 0);

  {
    auto result = cs0_client->DeassertCs();
    // The parent has stopped its loop, this should now fail.
    EXPECT_STATUS(result.status(), ZX_ERR_PEER_CLOSED);
  }
}

}  // namespace spi
