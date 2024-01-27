// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gt6853.h"

#include <endian.h>
#include <fuchsia/hardware/gpio/cpp/banjo-mock.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/ddk/metadata.h>
#include <lib/device-protocol/i2c-channel.h>
#include <lib/fake-i2c/fake-i2c.h>
#include <lib/inspect/testing/cpp/zxtest/inspect.h>
#include <lib/zx/clock.h>

#include <array>
#include <vector>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <zxtest/zxtest.h>

#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {

zx::vmo* config_vmo = nullptr;
size_t config_size = 0;

zx::vmo* firmware_vmo = nullptr;
size_t firmware_size = 0;

const char* config_path = nullptr;

}  // namespace

zx_status_t load_firmware_from_driver(zx_driver_t* drv, zx_device_t* device, const char* path,
                                      zx_handle_t* fw, size_t* size) {
  const std::string_view path_str(path);
  if ((path_str == GT6853_CONFIG_9364_PATH || path_str == GT6853_CONFIG_9365_PATH ||
       path_str == GT6853_CONFIG_7703_PATH) &&
      config_vmo && config_vmo->is_valid()) {
    config_path = path;
    *fw = config_vmo->get();
    *size = config_size;
    return ZX_OK;
  }
  if (path_str == GT6853_FIRMWARE_PATH && firmware_vmo && firmware_vmo->is_valid()) {
    *fw = firmware_vmo->get();
    *size = firmware_size;
    return ZX_OK;
  }

  return ZX_ERR_NOT_FOUND;
}

namespace touch {

class FakeTouchDevice : public fake_i2c::FakeI2c {
 public:
  struct FirmwarePacket {
    uint8_t type;
    uint16_t size;
    uint16_t flash_addr;
  };

  void WaitForTouchDataRead() {
    sync_completion_wait(&read_completion_, ZX_TIME_INFINITE);
    sync_completion_reset(&read_completion_);
  }

  bool ok() const { return event_reset_; }

  void set_sensor_id(const uint16_t sensor_id) { sensor_id_ = sensor_id; }
  cpp20::span<const uint8_t> get_config_data() const {
    return {config_data_, std::size(config_data_)};
  }
  const std::vector<FirmwarePacket>& get_firmware_packets() const { return firmware_packets_; }

 protected:
  zx_status_t Transact(const uint8_t* write_buffer, size_t write_buffer_size, uint8_t* read_buffer,
                       size_t* read_buffer_size) override {
    constexpr uint8_t kTouchData[] = {
        // clang-format off
        0x80, 0x5a, 0x00, 0xb9, 0x03, 0xae, 0x00, 0x00,
        0xc2, 0xf2, 0x01, 0x44, 0x00, 0x6c, 0x00, 0x00,
        0x01, 0x72, 0x00, 0x14, 0x01, 0x13, 0x00, 0x00,
        0xc3, 0x38, 0x01, 0xbe, 0x00, 0xdf, 0x00, 0x00,
        // clang-format on
    };

    if (write_buffer_size < 2) {
      return ZX_ERR_NOT_SUPPORTED;
    }

    const auto address = static_cast<uint16_t>((write_buffer[0] << 8) | write_buffer[1]);
    write_buffer += 2;
    write_buffer_size -= 2;

    using Register = Gt6853Device::Register;

    if (address == static_cast<uint16_t>(Register::kEventStatusReg)) {
      if (write_buffer_size >= 1 && write_buffer[0] == 0x00) {
        event_reset_ = true;
      } else {
        read_buffer[0] = current_state_ == kIdle ? 0x80 : 0x00;
        *read_buffer_size = 1;
      }
    } else if (address == static_cast<uint16_t>(Register::kContactsReg)) {
      read_buffer[0] = current_state_ == kIdle ? 0x34 : 0x00;
      *read_buffer_size = 1;
    } else if (address == static_cast<uint16_t>(Register::kContactsStartReg)) {
      // The interrupt has been received and the driver is reading out the data registers.
      if (current_state_ == kIdle) {
        memcpy(read_buffer, kTouchData, sizeof(kTouchData));
      } else {
        memset(read_buffer, 0x00, sizeof(kTouchData));
      }
      *read_buffer_size = sizeof(kTouchData);
      sync_completion_signal(&read_completion_);
    } else if (address == static_cast<uint16_t>(Register::kSensorIdReg)) {
      memcpy(read_buffer, &sensor_id_, sizeof(sensor_id_));
      *read_buffer_size = sizeof(sensor_id_);
    } else if (address == static_cast<uint16_t>(Register::kCommandReg) && write_buffer_size == 0) {
      // Reading the device command.
      read_buffer[0] = current_state_ == kWaitingForConfig ? 0x82 : 0xff;
      *read_buffer_size = 1;
    } else if (address == static_cast<uint16_t>(Register::kCommandReg) && write_buffer_size == 3) {
      // Writing the host command. Must write all three registers in one transfer.

      uint8_t checksum = write_buffer[0];
      checksum += write_buffer[1];
      checksum += write_buffer[2];
      if (checksum != 0) {
        return ZX_ERR_IO_DATA_INTEGRITY;
      }

      if (write_buffer[0] == 0x80 && current_state_ == kIdle) {
        current_state_ = kWaitingForConfig;
      } else if (write_buffer[0] == 0x83 && current_state_ == kWaitingForConfig) {
        current_state_ = kIdle;
      } else {
        return ZX_ERR_IO;
      }
    } else if (current_state_ == kWaitingForConfig &&
               address >= static_cast<uint16_t>(Register::kConfigDataReg) &&
               address < static_cast<uint16_t>(Register::kConfigDataReg) + sizeof(config_data_)) {
      const size_t offset = address - static_cast<uint16_t>(Register::kConfigDataReg);
      if (write_buffer_size > 0) {
        memcpy(config_data_ + offset, write_buffer, write_buffer_size);
      } else {
        memcpy(read_buffer, config_data_ + offset, sizeof(config_data_));
        *read_buffer_size = sizeof(config_data_);
      }
    } else if (address >= static_cast<uint16_t>(Register::kIspBuffer) &&
               address < (static_cast<uint16_t>(Register::kIspBuffer) + 4096)) {
      const uint16_t offset = address - static_cast<uint16_t>(Register::kIspBuffer);
      if (write_buffer_size > 0) {
        if (offset + write_buffer_size > 4096) {
          return ZX_ERR_IO;
        }
        memcpy(flash_packet_ + offset, write_buffer, write_buffer_size);
      } else {
        memcpy(read_buffer, flash_packet_ + offset, 4096 - offset);
        *read_buffer_size = 4096 - offset;
      }
    } else if (address >= static_cast<uint16_t>(Register::kIspAddr) &&
               address < (static_cast<uint16_t>(Register::kIspAddr) + 4096)) {
      const uint16_t offset = address - static_cast<uint16_t>(Register::kIspAddr);
      if (write_buffer_size > 0) {
        if (offset + write_buffer_size > 4096) {
          return ZX_ERR_IO;
        }
        memcpy(flash_packet_ + offset, write_buffer, write_buffer_size);
      } else {
        memcpy(read_buffer, flash_packet_ + offset, 4096 - offset);
        *read_buffer_size = 4096 - offset;
      }
    } else if (address == static_cast<uint16_t>(Register::kSubsysType)) {
      if (write_buffer_size == 0) {
        read_buffer[0] = subsys_type_;
        read_buffer[1] = subsys_type_;
        *read_buffer_size = 2;
      } else if (write_buffer_size == 2 && write_buffer[0] == write_buffer[1]) {
        subsys_type_ = write_buffer[0];

        FirmwarePacket packet = {};
        packet.type = write_buffer[0];

        memcpy(&packet.size, flash_packet_, sizeof(packet.size));
        packet.size = be16toh(packet.size);

        memcpy(&packet.flash_addr, flash_packet_ + sizeof(packet.size), sizeof(packet.flash_addr));
        packet.flash_addr = be16toh(packet.flash_addr);

        firmware_packets_.push_back(packet);
      } else {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kFlashFlag)) {
      if (write_buffer_size == 0) {
        // The flash state is read twice, report success in two states to handle this.
        if (current_state_ == kFlashingFirmware || current_state_ == kFlashingFirmwareDone) {
          read_buffer[0] = 0xbb;
          read_buffer[1] = 0xbb;
          current_state_ = current_state_ == kFlashingFirmware ? kFlashingFirmwareDone : kIdle;
        } else {
          read_buffer[0] = 0;
          read_buffer[1] = 0;
          current_state_ = kFlashingFirmware;
        }
        *read_buffer_size = 2;
      } else if (write_buffer_size != 2) {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kIspRunFlag)) {
      if (write_buffer_size == 0) {
        read_buffer[0] = 0xaa;
        read_buffer[1] = 0xbb;
        *read_buffer_size = 2;
      } else if (write_buffer_size != 2) {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kAccessPatch0)) {
      if (write_buffer_size == 0) {
        read_buffer[0] = access_patch0;
        *read_buffer_size = 1;
      } else if (write_buffer_size == 1) {
        access_patch0 = write_buffer[0];
      } else {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kCpuCtrl)) {
      if (write_buffer_size == 0) {
        read_buffer[0] = 0x24;  // kCpuCtrlHoldSs51
        *read_buffer_size = 1;
      } else if (write_buffer_size != 1) {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kDspMcuPower)) {
      if (write_buffer_size == 0) {
        read_buffer[0] = 0;
        *read_buffer_size = 1;
      } else if (write_buffer_size != 1) {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kBankSelect) ||
               address == static_cast<uint16_t>(Register::kCache) ||
               address == static_cast<uint16_t>(Register::kEsdKey) ||
               address == static_cast<uint16_t>(Register::kWtdTimer) ||
               address == static_cast<uint16_t>(Register::kScramble)) {
      if (write_buffer_size != 1) {
        return ZX_ERR_IO;
      }
    } else if (address == static_cast<uint16_t>(Register::kCpuRunFrom)) {
      if (write_buffer_size != 8) {
        return ZX_ERR_IO;
      }
    } else {
      return ZX_ERR_IO;
    }

    return ZX_OK;
  }

 private:
  enum State {
    kIdle,
    kWaitingForConfig,
    kFlashingFirmware,
    kFlashingFirmwareDone,
  };

  sync_completion_t read_completion_;
  bool event_reset_ = false;
  uint16_t sensor_id_ = UINT16_MAX;
  State current_state_ = kIdle;
  uint8_t config_data_[651];  // All panels have configs of this size.
  uint8_t flash_packet_[4096];
  uint8_t subsys_type_ = 0;
  uint8_t access_patch0 = 0;
  std::vector<FirmwarePacket> firmware_packets_;
};

class Gt6853Test : public zxtest::Test {
 public:
  Gt6853Test()
      : fake_parent_(MockDevice::FakeRootParent()),
        loop_(&kAsyncLoopConfigNeverAttachToThread),
        outgoing_(loop_.dispatcher()) {}

  void SetUp() override {
    ASSERT_OK(zx::interrupt::create(zx::resource(ZX_HANDLE_INVALID), 0, ZX_INTERRUPT_VIRTUAL,
                                    &gpio_interrupt_));

    zx::interrupt gpio_interrupt;
    ASSERT_OK(gpio_interrupt_.duplicate(ZX_RIGHT_SAME_RIGHTS, &gpio_interrupt));

    mock_gpio_.ExpectConfigIn(ZX_OK, GPIO_NO_PULL);
    mock_gpio_.ExpectGetInterrupt(ZX_OK, ZX_INTERRUPT_MODE_EDGE_LOW, std::move(gpio_interrupt));

    EXPECT_OK(loop_.StartThread());
  }

  void TearDown() override {
    if (device_) {
      device_async_remove(device_);
      EXPECT_OK(mock_ddk::ReleaseFlaggedDevices(fake_parent_.get()));
    }
    device_ = nullptr;
    loop_.Quit();
    loop_.JoinThreads();
    loop_.RunUntilIdle();
  }

  zx_status_t Init(uint32_t panel_type_id = 1) {
    panel_type_id_ = panel_type_id;

    fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
    fake_parent_->AddProtocol(ZX_PROTOCOL_GPIO, mock_gpio_.GetProto()->ops,
                              mock_gpio_.GetProto()->ctx, "gpio-int");
    fake_parent_->AddProtocol(ZX_PROTOCOL_GPIO, mock_gpio_.GetProto()->ops,
                              mock_gpio_.GetProto()->ctx, "gpio-reset");

    auto service_result = outgoing_.AddService<fuchsia_hardware_i2c::Service>(
        fuchsia_hardware_i2c::Service::InstanceHandler(
            {.device = fake_i2c_.bind_handler(loop_.dispatcher())}));
    ZX_ASSERT(service_result.is_ok());

    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ZX_ASSERT(endpoints.is_ok());
    ZX_ASSERT(outgoing_.Serve(std::move(endpoints->server)).is_ok());

    fake_parent_->AddFidlService(fuchsia_hardware_i2c::Service::Name, std::move(endpoints->client),
                                 "i2c");

    fake_parent_->SetMetadata(DEVICE_METADATA_BOARD_PRIVATE, &panel_type_id_,
                              sizeof(panel_type_id_));

    config_vmo = &config_vmo_;
    firmware_vmo = &firmware_vmo_;

    zx_status_t status = Gt6853Device::Create(nullptr, fake_parent_.get());
    device_ = fake_parent_->GetLatestChild();
    return status;
  }

 protected:
  zx_status_t WriteConfigData(const std::vector<uint8_t>& data, uint64_t offset) {
    return config_vmo_.write(data.data(), offset, data.size());
  }

  zx_status_t WriteConfigString(const char* data, uint64_t offset) {
    return config_vmo_.write(data, offset, strlen(data) + 1);
  }

  zx_status_t WriteFirmwareData(const std::vector<uint8_t>& data, uint64_t offset) {
    return firmware_vmo_.write(data.data(), offset, data.size());
  }

  void AddDefaultConfig() {
    config_size = 2338;
    ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

    const uint32_t config_size_le = htole32(config_size);
    ASSERT_OK(config_vmo_.write(&config_size_le, 0, sizeof(config_size_le)));
    ASSERT_OK(WriteConfigData({0x2b}, 4));
    ASSERT_OK(WriteConfigData({0x03}, 9));
    ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));
    ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));
    ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));
    ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));
    ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));
    ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
    ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
    ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
    ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
    ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

    fake_i2c_.set_sensor_id(0);
  }

  void WaitForNextReader() { device_->GetDeviceContext<Gt6853Device>()->WaitForNextReader(); }

  fidl::ClientEnd<fuchsia_input_report::InputDevice> GetInputDeviceClient() {
    auto endpoints = fidl::CreateEndpoints<fuchsia_input_report::InputDevice>();
    EXPECT_OK(endpoints);

    fidl::BindServer(loop_.dispatcher(), std::move(endpoints->server),
                     device_->GetDeviceContext<Gt6853Device>());

    return std::move(endpoints->client);
  }

  FakeTouchDevice fake_i2c_;
  zx::interrupt gpio_interrupt_;
  MockDevice* device_ = nullptr;
  uint32_t panel_type_id_ = 0;
  zx::vmo config_vmo_;
  zx::vmo firmware_vmo_;
  ddk::MockGpio mock_gpio_;

 private:
  std::shared_ptr<MockDevice> fake_parent_;
  async::Loop loop_;
  component::OutgoingDirectory outgoing_;
};

TEST_F(Gt6853Test, GetDescriptor) {
  AddDefaultConfig();
  ASSERT_OK(Init());

  fidl::WireSyncClient client(GetInputDeviceClient());

  auto response = client->GetDescriptor();

  ASSERT_TRUE(response.ok());
  ASSERT_TRUE(response.value().descriptor.has_device_info());
  ASSERT_TRUE(response.value().descriptor.has_touch());
  ASSERT_TRUE(response.value().descriptor.touch().has_input());
  ASSERT_TRUE(response.value().descriptor.touch().input().has_contacts());
  ASSERT_TRUE(response.value().descriptor.touch().input().has_max_contacts());
  ASSERT_TRUE(response.value().descriptor.touch().input().has_touch_type());
  ASSERT_EQ(response.value().descriptor.touch().input().contacts().count(), 10);

  EXPECT_EQ(response.value().descriptor.device_info().vendor_id,
            static_cast<uint32_t>(fuchsia_input_report::wire::VendorId::kGoogle));
  EXPECT_EQ(response.value().descriptor.device_info().product_id,
            static_cast<uint32_t>(
                fuchsia_input_report::wire::VendorGoogleProductId::kFocaltechTouchscreen));

  for (size_t i = 0; i < 10; i++) {
    const auto& contact = response.value().descriptor.touch().input().contacts()[i];
    ASSERT_TRUE(contact.has_position_x());
    ASSERT_TRUE(contact.has_position_y());

    EXPECT_EQ(contact.position_x().range.min, 0);
    EXPECT_EQ(contact.position_x().range.max, 600);
    EXPECT_EQ(contact.position_x().unit.type, fuchsia_input_report::wire::UnitType::kNone);
    EXPECT_EQ(contact.position_x().unit.exponent, 0);

    EXPECT_EQ(contact.position_y().range.min, 0);
    EXPECT_EQ(contact.position_y().range.max, 1024);
    EXPECT_EQ(contact.position_y().unit.type, fuchsia_input_report::wire::UnitType::kNone);
    EXPECT_EQ(contact.position_y().unit.exponent, 0);
  }

  EXPECT_EQ(response.value().descriptor.touch().input().max_contacts(), 10);
  EXPECT_EQ(response.value().descriptor.touch().input().touch_type(),
            fuchsia_input_report::wire::TouchType::kTouchscreen);
}

TEST_F(Gt6853Test, ReadReport) {
  AddDefaultConfig();
  ASSERT_OK(Init());

  fidl::WireSyncClient client(GetInputDeviceClient());

  auto reader_endpoints = fidl::CreateEndpoints<fuchsia_input_report::InputReportsReader>();
  ASSERT_TRUE(reader_endpoints.is_ok());
  auto [reader_client, reader_server] = std::move(reader_endpoints.value());
  // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
  (void)client->GetInputReportsReader(std::move(reader_server));
  fidl::WireSyncClient<fuchsia_input_report::InputReportsReader> reader(std::move(reader_client));
  WaitForNextReader();

  EXPECT_OK(gpio_interrupt_.trigger(0, zx::clock::get_monotonic()));

  fake_i2c_.WaitForTouchDataRead();

  const auto response = reader->ReadInputReports();
  ASSERT_TRUE(response.ok());
  ASSERT_TRUE(response->is_ok());

  const auto& reports = response->value()->reports;

  ASSERT_EQ(reports.count(), 1);
  ASSERT_TRUE(reports[0].has_touch());
  ASSERT_TRUE(reports[0].touch().has_contacts());
  ASSERT_EQ(reports[0].touch().contacts().count(), 4);

  EXPECT_EQ(reports[0].touch().contacts()[0].contact_id(), 0);
  EXPECT_EQ(reports[0].touch().contacts()[0].position_x(), 0x005a);
  EXPECT_EQ(reports[0].touch().contacts()[0].position_y(), 0x03b9);

  EXPECT_EQ(reports[0].touch().contacts()[1].contact_id(), 2);
  EXPECT_EQ(reports[0].touch().contacts()[1].position_x(), 0x01f2);
  EXPECT_EQ(reports[0].touch().contacts()[1].position_y(), 0x0044);

  EXPECT_EQ(reports[0].touch().contacts()[2].contact_id(), 1);
  EXPECT_EQ(reports[0].touch().contacts()[2].position_x(), 0x0072);
  EXPECT_EQ(reports[0].touch().contacts()[2].position_y(), 0x0114);

  EXPECT_EQ(reports[0].touch().contacts()[3].contact_id(), 3);
  EXPECT_EQ(reports[0].touch().contacts()[3].position_x(), 0x0138);
  EXPECT_EQ(reports[0].touch().contacts()[3].position_y(), 0x00be);

  EXPECT_TRUE(fake_i2c_.ok());
}

TEST_F(Gt6853Test, ConfigDownloadPanelType9364) {
  config_size = 2338;
  ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

  const uint32_t config_size_le = htole32(config_size);
  ASSERT_OK(config_vmo_.write(&config_size_le, 0, sizeof(config_size_le)));
  ASSERT_OK(WriteConfigData({0x2b}, 4));  // Checksum
  ASSERT_OK(WriteConfigData({0x03}, 9));  // Number of config entries in the table
  ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));  // Entry offsets
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));          // Entry 0 size
  ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));                       // Entry 0 sensor ID
  ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));       // Entry 0 config data
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));          // Repeat for entries 1, 2
  ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
  ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
  ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
  ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

  fake_i2c_.set_sensor_id(1);

  ASSERT_OK(Init());

  EXPECT_STREQ(reinterpret_cast<const char*>(fake_i2c_.get_config_data().data()),
               "Config number one");
  EXPECT_STREQ(config_path, GT6853_CONFIG_9364_PATH);
  EXPECT_EQ(fake_i2c_.get_config_data().size(), 0x0304 - 121);
}

TEST_F(Gt6853Test, ConfigDownloadPanelType9365) {
  config_size = 2338;
  ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

  const uint32_t config_size_le = htole32(config_size);
  ASSERT_OK(config_vmo_.write(&config_size_le, 0, sizeof(config_size_le)));
  ASSERT_OK(WriteConfigData({0x2b}, 4));
  ASSERT_OK(WriteConfigData({0x03}, 9));
  ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));
  ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));
  ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));
  ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
  ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
  ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
  ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

  fake_i2c_.set_sensor_id(0);

  ASSERT_OK(Init(4));  // kPanelTypeKdFiti9365

  EXPECT_STREQ(reinterpret_cast<const char*>(fake_i2c_.get_config_data().data()),
               "Config number zero");
  EXPECT_STREQ(config_path, GT6853_CONFIG_9365_PATH);
  EXPECT_EQ(fake_i2c_.get_config_data().size(), 0x0304 - 121);
}

TEST_F(Gt6853Test, ConfigDownloadPanelType7703) {
  config_size = 2338;
  ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

  const uint32_t config_size_le = htole32(config_size);
  ASSERT_OK(config_vmo_.write(&config_size_le, 0, sizeof(config_size_le)));
  ASSERT_OK(WriteConfigData({0x2b}, 4));
  ASSERT_OK(WriteConfigData({0x03}, 9));
  ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));
  ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));
  ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));
  ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
  ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
  ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
  ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

  fake_i2c_.set_sensor_id(0);

  ASSERT_OK(Init(6));  // kPanelTypeBoeSit7703

  EXPECT_STREQ(reinterpret_cast<const char*>(fake_i2c_.get_config_data().data()),
               "Config number zero");
  EXPECT_STREQ(config_path, GT6853_CONFIG_7703_PATH);
  EXPECT_EQ(fake_i2c_.get_config_data().size(), 0x0304 - 121);
}

TEST_F(Gt6853Test, ConfigDownloadUnableToLoadConfig) { EXPECT_NOT_OK(Init()); }

TEST_F(Gt6853Test, NoConfigEntry) {
  config_size = 2338;
  ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

  const uint32_t config_size_le = htole32(config_size);
  ASSERT_OK(config_vmo_.write(&config_size_le, 0, sizeof(config_size_le)));
  ASSERT_OK(WriteConfigData({0x2b}, 4));
  ASSERT_OK(WriteConfigData({0x03}, 9));
  ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));
  ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));
  ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));
  ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
  ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
  ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
  ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

  fake_i2c_.set_sensor_id(4);

  EXPECT_NOT_OK(Init());
}

TEST_F(Gt6853Test, InvalidConfigEntry) {
  config_size = 2338;
  ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

  ASSERT_OK(WriteConfigData({0x1c, 0x03, 0x00, 0x00, 0x2b}, 0));
  ASSERT_OK(WriteConfigData({0x03}, 9));
  ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));
  ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));
  ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));
  ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
  ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
  ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
  ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

  fake_i2c_.set_sensor_id(1);

  config_size = 0x031a + 2;
  EXPECT_NOT_OK(Init());
}

TEST_F(Gt6853Test, BadConfigChecksum) {
  config_size = 2338;
  ASSERT_OK(zx::vmo::create(fbl::round_up(config_size, ZX_PAGE_SIZE), 0, &config_vmo_));

  const uint32_t config_size_le = htole32(config_size);
  ASSERT_OK(config_vmo_.write(&config_size_le, 0, sizeof(config_size_le)));
  ASSERT_OK(WriteConfigData({0x2b + 1}, 4));
  ASSERT_OK(WriteConfigData({0x03}, 9));
  ASSERT_OK(WriteConfigData({0x16, 0x00, 0x1a, 0x03, 0x1e, 0x06}, 16));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x0016));
  ASSERT_OK(WriteConfigData({0x02}, 0x0016 + 20));
  ASSERT_OK(WriteConfigString("Config number two", 0x0016 + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x031a));
  ASSERT_OK(WriteConfigData({0x00}, 0x031a + 20));
  ASSERT_OK(WriteConfigString("Config number zero", 0x031a + 121));
  ASSERT_OK(WriteConfigData({0x04, 0x03, 0x00, 0x00}, 0x061e));
  ASSERT_OK(WriteConfigData({0x01}, 0x061e + 20));
  ASSERT_OK(WriteConfigString("Config number one", 0x061e + 121));

  fake_i2c_.set_sensor_id(1);

  EXPECT_EQ(Init(), ZX_ERR_IO_DATA_INTEGRITY);
}

TEST_F(Gt6853Test, FirmwareDownload) {
  firmware_size = 2048;
  ASSERT_OK(zx::vmo::create(fbl::round_up(firmware_size, ZX_PAGE_SIZE), 0, &firmware_vmo_));
  ASSERT_OK(WriteFirmwareData({0x00, 0x00, 0x07, 0xfa, 0x02, 0x98}, 0));
  ASSERT_OK(WriteFirmwareData({0x03}, 27));
  ASSERT_OK(WriteFirmwareData({0x01, 0x00, 0x00, 0x01, 0x00, 0xab, 0xcd}, 32));
  ASSERT_OK(WriteFirmwareData({0x02, 0x00, 0x00, 0x01, 0x00, 0x12, 0x34}, 40));
  ASSERT_OK(WriteFirmwareData({0x03, 0x00, 0x00, 0x01, 0x00, 0x56, 0x78}, 48));

  AddDefaultConfig();

  mock_gpio_.ExpectConfigOut(ZX_OK, 0);
  mock_gpio_.ExpectWrite(ZX_OK, 1);
  mock_gpio_.ExpectWrite(ZX_OK, 0);
  mock_gpio_.ExpectWrite(ZX_OK, 1);

  EXPECT_OK(Init());

  ASSERT_EQ(fake_i2c_.get_firmware_packets().size(), 2);

  EXPECT_EQ(fake_i2c_.get_firmware_packets()[0].type, 2);
  EXPECT_EQ(fake_i2c_.get_firmware_packets()[0].size, 256);
  EXPECT_EQ(fake_i2c_.get_firmware_packets()[0].flash_addr, 0x1234);

  EXPECT_EQ(fake_i2c_.get_firmware_packets()[1].type, 3);
  EXPECT_EQ(fake_i2c_.get_firmware_packets()[1].size, 256);
  EXPECT_EQ(fake_i2c_.get_firmware_packets()[1].flash_addr, 0x5678);
}

TEST_F(Gt6853Test, FirmwareDownloadInvalidCrc) {
  firmware_size = 2048;
  ASSERT_OK(zx::vmo::create(fbl::round_up(firmware_size, ZX_PAGE_SIZE), 0, &firmware_vmo_));
  ASSERT_OK(WriteFirmwareData({0x00, 0x00, 0x07, 0xfa, 0x02, 0x99}, 0));
  ASSERT_OK(WriteFirmwareData({0x03}, 27));
  ASSERT_OK(WriteFirmwareData({0x01, 0x00, 0x00, 0x01, 0x00, 0xab, 0xcd}, 32));
  ASSERT_OK(WriteFirmwareData({0x02, 0x00, 0x00, 0x01, 0x00, 0x12, 0x34}, 40));
  ASSERT_OK(WriteFirmwareData({0x03, 0x00, 0x00, 0x01, 0x00, 0x56, 0x78}, 48));

  EXPECT_NOT_OK(Init());
}

TEST_F(Gt6853Test, FirmwareDownloadNoIspEntry) {
  firmware_size = 2048;
  ASSERT_OK(zx::vmo::create(fbl::round_up(firmware_size, ZX_PAGE_SIZE), 0, &firmware_vmo_));
  ASSERT_OK(WriteFirmwareData({0x00, 0x00, 0x07, 0xfa, 0x00, 0x00}, 0));
  ASSERT_OK(WriteFirmwareData({0x00}, 27));

  EXPECT_NOT_OK(Init());
}

TEST_F(Gt6853Test, LatencyMeasurements) {
  AddDefaultConfig();
  ASSERT_OK(Init());

  fidl::WireSyncClient client(GetInputDeviceClient());

  auto reader_endpoints = fidl::CreateEndpoints<fuchsia_input_report::InputReportsReader>();
  ASSERT_TRUE(reader_endpoints.is_ok());
  auto [reader_client, reader_server] = std::move(reader_endpoints.value());
  // TODO(fxbug.dev/97955) Consider handling the error instead of ignoring it.
  (void)client->GetInputReportsReader(std::move(reader_server));
  fidl::WireSyncClient<fuchsia_input_report::InputReportsReader> reader(std::move(reader_client));
  WaitForNextReader();

  for (int i = 0; i < 5; i++) {
    EXPECT_OK(gpio_interrupt_.trigger(0, zx::clock::get_monotonic()));
    fake_i2c_.WaitForTouchDataRead();
  }

  for (size_t reports = 0; reports < 5;) {
    const auto response = reader->ReadInputReports();
    if (response.ok() && response->is_ok()) {
      reports += response->value()->reports.count();
    }
  }

  const zx::vmo& inspect_vmo = device_->GetInspectVmo();
  ASSERT_TRUE(inspect_vmo.is_valid());

  inspect::InspectTestHelper inspector;
  inspector.ReadInspect(inspect_vmo);

  const inspect::Hierarchy* root = inspector.hierarchy().GetByPath({"hid-input-report-touch"});
  ASSERT_NOT_NULL(root);

  const auto* average_latency =
      root->node().get_property<inspect::UintPropertyValue>("average_latency_usecs");
  ASSERT_NOT_NULL(average_latency);

  const auto* max_latency =
      root->node().get_property<inspect::UintPropertyValue>("max_latency_usecs");
  ASSERT_NOT_NULL(max_latency);

  EXPECT_GE(max_latency->value(), average_latency->value());
}

}  // namespace touch
