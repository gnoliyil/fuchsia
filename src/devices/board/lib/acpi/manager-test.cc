// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/lib/acpi/manager.h"

#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>

#include <initializer_list>
#include <map>
#include <optional>

#include <zxtest/zxtest.h>

#include "lib/async-loop/testing/cpp/real_loop.h"
#include "src/devices/board/lib/acpi/acpi.h"
#include "src/devices/board/lib/acpi/manager-fuchsia.h"
#include "src/devices/board/lib/acpi/test/mock-acpi.h"
#include "src/devices/board/lib/acpi/test/mock-pci.h"
#include "src/devices/board/lib/acpi/test/null-iommu-manager.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

using acpi::test::Device;

static void ExpectProps(acpi::DeviceBuilder* b, std::vector<zx_device_prop_t> expected_dev,
                        std::vector<zx_device_str_prop_t> expected_str) {
  auto dev_props = b->GetDevProps();
  zxlogf(INFO, "size: %lu", dev_props.size());
  size_t left = expected_dev.size();
  for (auto& expected_prop : expected_dev) {
    for (auto& actual : dev_props) {
      if (actual.id == expected_prop.id) {
        zxlogf(INFO, "%s 0x%x 0x%x vs 0x%x 0x%x", b->name(), actual.id, actual.value,
               expected_prop.id, expected_prop.value);
        ASSERT_EQ(actual.value, expected_prop.value);
        left--;
      }
    }
  }
  ASSERT_EQ(left, 0);

  auto& str_props = b->GetStrProps();
  left = expected_str.size();

  for (auto& expected_prop : expected_str) {
    for (auto& actual : str_props) {
      if (!strcmp(actual.key, expected_prop.key)) {
        ASSERT_EQ(actual.property_value.data_type, expected_prop.property_value.data_type);
        switch (actual.property_value.data_type) {
          case ZX_DEVICE_PROPERTY_VALUE_INT:
            ASSERT_EQ(actual.property_value.data.int_val,
                      expected_prop.property_value.data.int_val);
            break;
          case ZX_DEVICE_PROPERTY_VALUE_STRING:
            ASSERT_STREQ(actual.property_value.data.str_val,
                         expected_prop.property_value.data.str_val);
            break;
          case ZX_DEVICE_PROPERTY_VALUE_BOOL:
            ASSERT_EQ(actual.property_value.data.bool_val,
                      expected_prop.property_value.data.bool_val);
            break;
          case ZX_DEVICE_PROPERTY_VALUE_ENUM:
            ASSERT_STREQ(actual.property_value.data.enum_val,
                         expected_prop.property_value.data.enum_val);
            break;
          default:
            // This should never happen.
            ASSERT_TRUE(false);
            break;
        }
        left--;
      }
    }
  }
  ASSERT_EQ(left, 0);
}

class AcpiManagerTest : public zxtest::Test, public loop_fixture::RealLoop {
 public:
  AcpiManagerTest()
      : mock_root_(MockDevice::FakeRootParent()), manager_(&acpi_, &iommu_, mock_root_.get()) {}
  void SetUp() override { acpi_.SetDeviceRoot(std::make_unique<Device>("\\")); }

  void InsertDeviceBelow(std::string path, std::unique_ptr<Device> d) {
    Device* parent = acpi_.GetDeviceRoot()->FindByPath(path);
    ASSERT_NE(parent, nullptr);
    parent->AddChild(std::move(d));
  }

  void DiscoverConfigurePublish() {
    auto ret = manager_.DiscoverDevices();
    ASSERT_TRUE(ret.is_ok());

    ret = manager_.ConfigureDiscoveredDevices();
    ASSERT_TRUE(ret.is_ok());

    ret = manager_.PublishDevices(mock_root_.get(), device_loop_.dispatcher());
    ASSERT_TRUE(ret.is_ok());
  }

 protected:
  async::Loop device_loop_{&kAsyncLoopConfigNeverAttachToThread};
  std::shared_ptr<MockDevice> mock_root_;
  acpi::test::MockAcpi acpi_;
  acpi::FuchsiaManager manager_;
  NullIommuManager iommu_;
};

TEST_F(AcpiManagerTest, TestEnumerateEmptyTables) {
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
  ASSERT_EQ(mock_root_->descendant_count(), 0);
}

TEST_F(AcpiManagerTest, TestEnumerateSystemBus) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  ASSERT_EQ(mock_root_->descendant_count(), 2);
}

TEST_F(AcpiManagerTest, TestDevicesOnPciBus) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  auto device = std::make_unique<Device>("PCI0");
  device->SetHid("PNP0A08");
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::move(device)));

  device = std::make_unique<Device>("TEST");
  device->SetAdr(0x00010002);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_.PCI0", std::move(device)));

  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
  ASSERT_EQ(mock_root_->descendant_count(), 6);

  Device* pci_bus = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.PCI0");
  // Check the PCI bus's type was set correctly.
  acpi::DeviceBuilder* builder = manager_.LookupDevice(pci_bus);
  ASSERT_EQ(builder->GetBusType(), acpi::BusType::kPci);
  ASSERT_TRUE(builder->HasBusId());
  ASSERT_EQ(builder->GetBusId(), 0);

  // Check that pci_init was called with the correct bus:device.function.
  std::vector<pci_bdf_t> bdfs = acpi::test::GetAcpiBdfs();
  ASSERT_EQ(bdfs.size(), 1);
  ASSERT_EQ(bdfs[0].bus_id, 0);
  ASSERT_EQ(bdfs[0].device_id, 1);
  ASSERT_EQ(bdfs[0].function_id, 2);
}

TEST_F(AcpiManagerTest, TestDeviceOnPciBusWithNoAdr) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  auto device = std::make_unique<Device>("PCI0");
  device->SetHid("PNP0A08");
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::move(device)));

  device = std::make_unique<Device>("TEST");
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_.PCI0", std::move(device)));

  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
  // We added 3 devices, and then each device gets a corresponding passthrough device.
  ASSERT_EQ(mock_root_->descendant_count(), 6);

  ASSERT_TRUE(acpi::test::GetAcpiBdfs().empty());
}

TEST_F(AcpiManagerTest, TestDeviceWithHidCid) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  auto device = std::make_unique<Device>("TEST");
  device->SetHid("GGGG0000");
  device->SetCids({"AAAA1111"});

  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
  Device* test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.TEST");
  // Check the properties were generated as expected.
  acpi::DeviceBuilder* builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder, {},
                  {
                      zx_device_str_prop_t{.key = "fuchsia.acpi.hid",
                                           .property_value = str_prop_str_val("GGGG0000")},
                      zx_device_str_prop_t{.key = "fuchsia.acpi.first_cid",
                                           .property_value = str_prop_str_val("AAAA1111")},
                  }));
}

static constexpr acpi::Uuid kDevicePropertiesUuid =
    acpi::Uuid::Create(0xdaffd814, 0x6eba, 0x4d8c, 0x8a91, 0xbc9bbf4aa301);

TEST_F(AcpiManagerTest, TestDeviceWithDeviceTreeHid) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  auto device = std::make_unique<Device>("TEST");
  device->SetHid("PRP0001");
  ACPI_OBJECT compatible[2] = {{
                                   .String = {.Type = ACPI_TYPE_STRING,
                                              .Length = sizeof("compatible") - 1,
                                              .Pointer = const_cast<char*>("compatible")},
                               },
                               {
                                   .String = {.Type = ACPI_TYPE_STRING,
                                              .Length = sizeof("google,cr50") - 1,
                                              .Pointer = const_cast<char*>("google,cr50")},
                               }};
  device->AddDsd(kDevicePropertiesUuid, ACPI_OBJECT{.Package = {.Type = ACPI_TYPE_PACKAGE,
                                                                .Count = std::size(compatible),
                                                                .Elements = compatible}});
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
  Device* test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.TEST");
  // Check the properties were generated as expected.
  acpi::DeviceBuilder* builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder, {},
                  {
                      zx_device_str_prop_t{.key = "fuchsia.acpi.first_cid",
                                           .property_value = str_prop_str_val("google,cr50")},
                  }));
}

TEST_F(AcpiManagerTest, TestDeviceWithDeviceTreeCid) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  auto device = std::make_unique<Device>("TEST");
  device->SetCids({"PRP0001"});
  ACPI_OBJECT compatible[2] = {{
                                   .String = {.Type = ACPI_TYPE_STRING,
                                              .Length = sizeof("compatible") - 1,
                                              .Pointer = const_cast<char*>("compatible")},
                               },
                               {
                                   .String = {.Type = ACPI_TYPE_STRING,
                                              .Length = sizeof("google,cr50") - 1,
                                              .Pointer = const_cast<char*>("google,cr50")},
                               }};
  device->AddDsd(kDevicePropertiesUuid, ACPI_OBJECT{.Package = {.Type = ACPI_TYPE_PACKAGE,
                                                                .Count = std::size(compatible),
                                                                .Elements = compatible}});
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
  Device* test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.TEST");
  // Check the properties were generated as expected.
  acpi::DeviceBuilder* builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder, {},
                  {
                      zx_device_str_prop_t{.key = "fuchsia.acpi.first_cid",
                                           .property_value = str_prop_str_val("google,cr50")},
                  }));
}

TEST_F(AcpiManagerTest, TestSpiDevice) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("SPI0")));
  auto device = std::make_unique<Device>("S002");
  ACPI_RESOURCE spi_resource = {
      .Type = ACPI_RESOURCE_TYPE_SERIAL_BUS,
      .Data =
          {
              .SpiSerialBus =
                  {
                      .Type = ACPI_RESOURCE_SERIAL_TYPE_SPI,
                      .SlaveMode = ACPI_CONTROLLER_INITIATED,
                      .ResourceSource =
                          {
                              .Index = 0,
                              .StringLength = sizeof("\\_SB_.SPI0") - 1,
                              .StringPtr = const_cast<char*>("\\_SB_.SPI0"),
                          },

                      .DevicePolarity = ACPI_SPI_ACTIVE_LOW,
                      .DataBitLength = 8,
                      .ClockPhase = ACPI_SPI_FIRST_PHASE,
                      .ClockPolarity = ACPI_SPI_START_HIGH,
                      .DeviceSelection = 2,
                  },
          },
  };
  device->AddResource(spi_resource);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_.SPI0", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  Device* test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.SPI0.S002");
  acpi::DeviceBuilder* builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder,
                  {
                      zx_device_prop_t{.id = BIND_SPI_BUS_ID, .value = 0},
                      zx_device_prop_t{.id = BIND_SPI_CHIP_SELECT, .value = 2},
                      zx_device_prop_t{.id = BIND_ACPI_BUS_TYPE, .value = acpi::BusType::kSpi},
                  },
                  {}));
}

TEST_F(AcpiManagerTest, TestI2cDevice) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("I2C0")));
  auto device = std::make_unique<Device>("H084");
  ACPI_RESOURCE i2c_resource = {
      .Type = ACPI_RESOURCE_TYPE_SERIAL_BUS,
      .Data =
          {
              .I2cSerialBus =
                  {
                      .Type = ACPI_RESOURCE_SERIAL_TYPE_I2C,
                      .SlaveMode = ACPI_CONTROLLER_INITIATED,
                      .ResourceSource =
                          {
                              .Index = 0,
                              .StringLength = sizeof("\\_SB_.I2C0") - 1,
                              .StringPtr = const_cast<char*>("\\_SB_.I2C0"),
                          },
                      .AccessMode = ACPI_I2C_7BIT_MODE,
                      .SlaveAddress = 0x84,
                  },
          },
  };
  device->AddResource(i2c_resource);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_.I2C0", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  Device* test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.I2C0.H084");
  acpi::DeviceBuilder* builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder,
                  {
                      zx_device_prop_t{.id = BIND_I2C_BUS_ID, .value = 0},
                      zx_device_prop_t{.id = BIND_I2C_ADDRESS, .value = 0x84},
                      zx_device_prop_t{.id = BIND_ACPI_BUS_TYPE, .value = acpi::BusType::kI2c},
                  },
                  {}));
}

TEST_F(AcpiManagerTest, TestMultipleI2cDevice) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("I2C0")));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("I2C1")));
  auto device = std::make_unique<Device>("H084");
  ACPI_RESOURCE i2c_resource = {
      .Type = ACPI_RESOURCE_TYPE_SERIAL_BUS,
      .Data =
          {
              .I2cSerialBus =
                  {
                      .Type = ACPI_RESOURCE_SERIAL_TYPE_I2C,
                      .SlaveMode = ACPI_CONTROLLER_INITIATED,
                      .ResourceSource =
                          {
                              .Index = 0,
                              .StringLength = sizeof("\\_SB_.I2C0") - 1,
                              .StringPtr = const_cast<char*>("\\_SB_.I2C0"),
                          },
                      .AccessMode = ACPI_I2C_7BIT_MODE,
                      .SlaveAddress = 0x84,
                  },
          },
  };
  device->AddResource(i2c_resource);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_.I2C0", std::move(device)));
  i2c_resource.Data.I2cSerialBus.ResourceSource = {.Index = 0,
                                                   .StringLength = sizeof("\\_SB_.I2C1") - 1,
                                                   .StringPtr = const_cast<char*>("\\_SB_.I2C1")};
  i2c_resource.Data.I2cSerialBus.SlaveAddress = 0x72;
  device = std::make_unique<Device>("H072");
  device->AddResource(i2c_resource);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_.I2C1", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  Device* test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.I2C0.H084");
  acpi::DeviceBuilder* builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder,
                  {
                      zx_device_prop_t{.id = BIND_I2C_BUS_ID, .value = 0},
                      zx_device_prop_t{.id = BIND_I2C_ADDRESS, .value = 0x84},
                      zx_device_prop_t{.id = BIND_ACPI_BUS_TYPE, .value = acpi::BusType::kI2c},
                  },
                  {}));

  test = acpi_.GetDeviceRoot()->FindByPath("\\_SB_.I2C1.H072");
  builder = manager_.LookupDevice(test);
  ASSERT_NO_FATAL_FAILURE(
      ExpectProps(builder,
                  {
                      zx_device_prop_t{.id = BIND_I2C_BUS_ID, .value = 1},
                      zx_device_prop_t{.id = BIND_I2C_ADDRESS, .value = 0x72},
                      zx_device_prop_t{.id = BIND_ACPI_BUS_TYPE, .value = acpi::BusType::kI2c},
                  },
                  {}));
}

TEST_F(AcpiManagerTest, TestDeviceNotPresentIsIgnored) {
  auto device = std::make_unique<Device>("_SB_");
  device->SetSta(0);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("I2C0")));

  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  // No devices should have been published.
  ASSERT_EQ(mock_root_->descendant_count(), 0);
}

TEST_F(AcpiManagerTest, TestDeviceNotPresentButFunctioning) {
  auto device = std::make_unique<Device>("_SB_");
  device->SetSta(ACPI_STA_DEVICE_FUNCTIONING);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("I2C0")));

  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  // Both devices should have been enumerated, but only I2C0 will have a passthrough device.
  ASSERT_EQ(mock_root_->descendant_count(), 3);
}

TEST_F(AcpiManagerTest, TestDeviceNotEnabled) {
  auto device = std::make_unique<Device>("_SB_");
  device->SetSta(ACPI_STA_DEVICE_PRESENT);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::make_unique<Device>("I2C0")));

  // If the Manager incorrectly tries to access the device's resources, it will trigger an assert in
  // MockAcpi.
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());
}

TEST_F(AcpiManagerTest, TestAddPowerResource) {
  auto device = std::make_unique<Device>("PRIC");
  device->SetPowerResourceMethods(3, 5);
  ACPI_HANDLE power_resource_handle = device.get();
  acpi_.GetDeviceRoot()->AddChild(std::move(device));

  const acpi::PowerResource* power_resource = manager_.AddPowerResource(power_resource_handle);
  ASSERT_NOT_NULL(power_resource);

  ASSERT_EQ(power_resource->system_level(), 3);
  ASSERT_EQ(power_resource->resource_order(), 5);
  // Make sure adding the same power resource returns the existing entry.
  ASSERT_EQ(manager_.AddPowerResource(power_resource_handle), power_resource);
}

TEST_F(AcpiManagerTest, TestReferencePowerResources) {
  auto device = std::make_unique<Device>("POW1");
  device->SetPowerResourceMethods(0, 0);
  ACPI_HANDLE power_resource_handle1 = device.get();
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::move(device)));
  Device* mock_power_resource1 = acpi_.GetDeviceRoot()->FindByPath("\\POW1");
  const acpi::PowerResource* power_resource1 = manager_.AddPowerResource(power_resource_handle1);
  ASSERT_NOT_NULL(power_resource1);

  device = std::make_unique<Device>("POW2");
  device->SetPowerResourceMethods(0, 0);
  ACPI_HANDLE power_resource_handle2 = device.get();
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::move(device)));
  Device* mock_power_resource2 = acpi_.GetDeviceRoot()->FindByPath("\\POW2");
  const acpi::PowerResource* power_resource2 = manager_.AddPowerResource(power_resource_handle2);
  ASSERT_NOT_NULL(power_resource2);

  std::vector<ACPI_HANDLE> power_resource_handles{power_resource_handle1, power_resource_handle2};

  ASSERT_OK(manager_.ReferencePowerResources(power_resource_handles));
  ASSERT_EQ(mock_power_resource1->sta(), 1);
  ASSERT_EQ(mock_power_resource2->sta(), 1);

  ASSERT_OK(manager_.DereferencePowerResources(power_resource_handles));
  ASSERT_EQ(mock_power_resource1->sta(), 0);
  ASSERT_EQ(mock_power_resource2->sta(), 0);
}

TEST_F(AcpiManagerTest, TestInterruptsMakeFragments) {
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\", std::make_unique<Device>("_SB_")));
  auto device = std::make_unique<Device>("IRQT");

  ACPI_RESOURCE irq_resource = {
      .Type = ACPI_RESOURCE_TYPE_IRQ,
      .Data =
          {
              .Irq =
                  {
                      .DescriptorLength = sizeof(ACPI_RESOURCE_IRQ),
                      .Triggering = ACPI_EDGE_SENSITIVE,
                      .Polarity = ACPI_ACTIVE_HIGH,
                      .Shareable = 0,
                      .WakeCapable = 0,
                      .InterruptCount = 1,
                      .Interrupts = {2},
                  },
          },
  };
  device->AddResource(irq_resource);
  ASSERT_NO_FATAL_FAILURE(InsertDeviceBelow("\\_SB_", std::move(device)));
  ASSERT_NO_FATAL_FAILURE(DiscoverConfigurePublish());

  // Validate the device topology we created.
  // We should have:
  // <root> -> acpi-_SB_ -> acpi-IRQT -> acpi-IRQT-irq000, pt
  auto dev = mock_root_->GetLatestChild();
  ASSERT_STREQ("acpi-_SB_", dev->name());
  dev = dev->GetLatestChild();
  ASSERT_STREQ("acpi-IRQT", dev->name());
  ASSERT_EQ(2, dev->children().size());

  std::unordered_set<std::string> expected{"acpi-IRQT-irq000", "pt"};
  for (auto& child : dev->children()) {
    auto pos = expected.find(child->name());
    ASSERT_NE(pos, expected.end(), "Child name %s not expected", child->name());
    expected.erase(pos);
  }

  ASSERT_EQ(0, expected.size());

  device_async_remove(mock_root_.get());
  ASSERT_OK(mock_ddk::ReleaseFlaggedDevices(mock_root_.get()));
  mock_root_.reset();
}
