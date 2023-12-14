// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.i2c.businfo/cpp/wire.h>
#include <fuchsia/hardware/i2cimpl/cpp/banjo.h>
#include <lib/async-loop/default.h>
#include <lib/async_patterns/testing/cpp/dispatcher_bound.h>
#include <lib/ddk/metadata.h>
#include <lib/driver/testing/cpp/driver_lifecycle.h>
#include <lib/driver/testing/cpp/driver_runtime.h>
#include <lib/driver/testing/cpp/test_environment.h>
#include <lib/driver/testing/cpp/test_node.h>
#include <lib/zx/clock.h>
#include <zircon/errors.h>

#include <optional>
#include <span>
#include <vector>

#include <fake-mmio-reg/fake-mmio-reg.h>
#include <soc/aml-common/aml-i2c.h>
#include <zxtest/zxtest.h>

#include "aml-i2c-regs.h"
#include "dfv2-driver.h"
#include "src/devices/bus/testing/fake-pdev/fake-pdev.h"

namespace aml_i2c {

class FakeDfv2DriverController {
 public:
  struct Transfer {
    std::vector<uint8_t> write_data;
    std::vector<uint8_t> token_list;
    uint32_t target_addr;
    bool is_read;

    void ExpectTokenListEq(const std::vector<uint8_t>& expected) const {
      ASSERT_EQ(token_list.size(), expected.size());
      EXPECT_BYTES_EQ(token_list.data(), expected.data(), expected.size());
    }
  };

  explicit FakeDfv2DriverController(zx::unowned_interrupt irq)
      : mmio_(sizeof(reg_values_[0]), 8), irq_(std::move(irq)) {
    for (size_t i = 0; i < std::size(reg_values_); i++) {
      mmio_[i * sizeof(reg_values_[0])].SetReadCallback(ReadRegCallback(i));
      mmio_[i * sizeof(reg_values_[0])].SetWriteCallback(WriteRegCallback(i));
    }
    mmio_[kControlReg].SetWriteCallback([&](uint64_t value) { WriteControlReg(value); });
  }

  FakeDfv2DriverController(FakeDfv2DriverController&& other) = delete;
  FakeDfv2DriverController& operator=(FakeDfv2DriverController&& other) = delete;

  FakeDfv2DriverController(const FakeDfv2DriverController& other) = delete;
  FakeDfv2DriverController& operator=(const FakeDfv2DriverController& other) = delete;

  fdf::MmioBuffer GetMmioBuffer() { return mmio_.GetMmioBuffer(); }

  void SetReadData(cpp20::span<const uint8_t> read_data) { read_data_ = read_data; }

  std::vector<Transfer> GetTransfers() { return std::move(transfers_); }

  cpp20::span<uint32_t> mmio() { return {reg_values_, std::size(reg_values_)}; }

 private:
  std::function<uint64_t(void)> ReadRegCallback(size_t offset) {
    return [this, offset]() { return reg_values_[offset]; };
  }

  std::function<void(uint64_t)> WriteRegCallback(size_t offset) {
    return [this, offset](uint64_t value) {
      EXPECT_EQ(value & 0xffff'ffff, value);
      reg_values_[offset] = static_cast<uint32_t>(value);
    };
  }

  void WriteControlReg(uint64_t value) {
    EXPECT_EQ(value & 0xffff'ffff, value);
    if (value & 1) {
      // Start flag -- process the token list (saving the target address and/or data if needed),
      // then trigger the interrupt.
      ProcessTokenList();
      irq_->trigger(0, zx::clock::get_monotonic());
    }
    reg_values_[kControlReg / sizeof(uint32_t)] = static_cast<uint32_t>(value);
  }

  void ProcessTokenList() {
    uint64_t token_list = Get64BitReg(kTokenList0Reg);
    uint64_t write_data = Get64BitReg(kWriteData0Reg);
    uint64_t read_data = 0;
    size_t data_count = 0;

    for (uint64_t token = token_list & 0xf;; token_list >>= 4, token = token_list & 0xf) {
      // Skip most token validation as test cases can check against the expected token sequence.
      switch (static_cast<TokenList::Token>(token)) {
        case TokenList::Token::kEnd:
        case TokenList::Token::kStop:
          break;
        case TokenList::Token::kStart: {
          const uint32_t target_addr = reg_values_[kTargetAddrReg / sizeof(uint32_t)];
          EXPECT_EQ(target_addr & 1, 0);
          transfers_.push_back({.target_addr = (target_addr >> 1) & 0x7f});
          break;
        }
        case TokenList::Token::kTargetAddrWr:
          ASSERT_FALSE(transfers_.empty());
          transfers_.back().is_read = false;
          break;
        case TokenList::Token::kTargetAddrRd:
          ASSERT_FALSE(transfers_.empty());
          transfers_.back().is_read = true;
          break;
        case TokenList::Token::kData:
        case TokenList::Token::kDataLast:
          ASSERT_FALSE(transfers_.empty());
          if (transfers_.back().is_read) {
            ASSERT_FALSE(read_data_.empty());
            read_data |= static_cast<uint64_t>(read_data_[0]) << (8 * data_count++);
            read_data_ = read_data_.subspan(1);
          } else {
            transfers_.back().write_data.push_back(write_data & 0xff);
            write_data >>= 8;
          }
          break;
        default:
          ASSERT_TRUE(false, "Invalid token %lu", token_list & 0xf);
          break;
      }

      ASSERT_FALSE(transfers_.empty());
      transfers_.back().token_list.push_back(token_list & 0xf);

      if (static_cast<TokenList::Token>(token) == TokenList::Token::kEnd) {
        break;
      }
    }

    EXPECT_EQ(token_list, 0);  // There should be no tokens after the end token.

    reg_values_[kReadData0Reg / sizeof(uint32_t)] = read_data & 0xffff'ffff;
    reg_values_[kReadData1Reg / sizeof(uint32_t)] = read_data >> 32;
  }

  uint64_t Get64BitReg(size_t offset) const {
    return reg_values_[offset / sizeof(uint32_t)] |
           (static_cast<uint64_t>(reg_values_[(offset / sizeof(uint32_t)) + 1]) << 32);
  }

  ddk_fake::FakeMmioRegRegion mmio_;
  uint32_t reg_values_[8]{};
  zx::unowned_interrupt irq_;
  std::vector<Transfer> transfers_;
  cpp20::span<const uint8_t> read_data_;
};

class Environment {
 public:
  fdf::DriverStartArgs Init(fake_pdev::FakePDevFidl::Config pdev_config,
                            std::optional<aml_i2c_delay_values> metadata) {
    zx::result start_args_result = test_node_.CreateStartArgsAndServe();
    ZX_ASSERT(start_args_result.is_ok());

    zx::result init_result =
        test_environment_.Initialize(std::move(start_args_result->incoming_directory_server));
    ZX_ASSERT(init_result.is_ok());

    pdev_server_.SetConfig(std::move(pdev_config));

    async_dispatcher_t* dispatcher = fdf::Dispatcher::GetCurrent()->async_dispatcher();
    std::string instance_name = "pdev";

    zx::result add_service_result =
        test_environment_.incoming_directory()
            .AddService<fuchsia_hardware_platform_device::Service>(
                pdev_server_.GetInstanceHandler(dispatcher), instance_name);
    ZX_ASSERT(add_service_result.is_ok());

    compat_server_.Init("default", "topo");
    if (metadata.has_value()) {
      compat_server_.AddMetadata(DEVICE_METADATA_PRIVATE, &metadata.value(),
                                 sizeof(metadata.value()));
    }
    zx_status_t status = compat_server_.Serve(dispatcher, &test_environment_.incoming_directory());
    ZX_ASSERT(status == ZX_OK);

    return std::move(start_args_result.value().start_args);
  }

 private:
  fdf_testing::TestNode test_node_{"root"};
  fake_pdev::FakePDevFidl pdev_server_;
  fdf_testing::TestEnvironment test_environment_;
  compat::DeviceServer compat_server_;
};

class AmlI2cDfv2Test : public zxtest::Test {
 public:
  // Convenience definitions that don't require casting.
  enum Token : uint8_t {
    kEnd,
    kStart,
    kTargetAddrWr,
    kTargetAddrRd,
    kData,
    kDataLast,
    kStop,
  };

  void TearDown() override {
    zx::result run_result = runtime_.RunToCompletion(aml_i2c_driver_.PrepareStop());
    ZX_ASSERT(run_result.is_ok());
  }

  void InitDriver(std::optional<aml_i2c_delay_values> metadata = std::nullopt,
                  uint32_t mmio_count = 1, uint32_t irq_count = 1,
                  bool start_should_succeed = true) {
    zx::result pdev_config = InitController(mmio_count, irq_count);
    ASSERT_OK(pdev_config.status_value());
    fdf::DriverStartArgs start_args =
        env_.SyncCall(&Environment::Init, std::move(pdev_config.value()), std::move(metadata));

    zx::result start_result =
        runtime_.RunToCompletion(aml_i2c_driver_.Start(std::move(start_args)));
    if (start_should_succeed) {
      ASSERT_OK(start_result.status_value());
      aml_i2c_driver_->SetTimeout(zx::duration(ZX_TIME_INFINITE));
    } else {
      ASSERT_NOT_OK(start_result.status_value());
    }
  }

  // `InitDriver` must be called before using this method.
  FakeDfv2DriverController& controller() {
    EXPECT_TRUE(controller_.has_value());
    return controller_.value();
  }

  cpp20::span<uint32_t> mmio() { return controller().mmio(); }

  zx::result<ddk::I2cImplProtocolClient> I2cImplClient() {
    ddk::I2cImplProtocolClient i2c_impl;
    zx_status_t status = aml_i2c_driver_->GetProtocol(ZX_PROTOCOL_I2C_IMPL, &i2c_impl);
    if (status != ZX_OK) {
      return zx::error(status);
    }
    return zx::ok(i2c_impl);
  }

 private:
  static constexpr size_t kMmioSize = sizeof(uint32_t) * 8;

  zx::result<fake_pdev::FakePDevFidl::Config> InitController(uint32_t mmio_count,
                                                             uint32_t irq_count) {
    fake_pdev::FakePDevFidl::Config config;
    config.irqs[0] = {};
    zx_status_t status =
        zx::interrupt::create(zx::resource(), 0, ZX_INTERRUPT_VIRTUAL, &config.irqs[0]);
    if (status != ZX_OK) {
      return zx::error(status);
    }
    controller_.emplace(config.irqs[0].borrow());
    config.mmios[0] = controller().GetMmioBuffer();

    config.device_info = {
        .mmio_count = mmio_count,
        .irq_count = irq_count,
    };
    return zx::ok(std::move(config));
  }

  std::optional<FakeDfv2DriverController> controller_;
  fdf_testing::DriverRuntime runtime_;
  fdf::UnownedSynchronizedDispatcher env_dispatcher_ = runtime_.StartBackgroundDispatcher();
  async_patterns::TestDispatcherBound<Environment> env_{env_dispatcher_->async_dispatcher(),
                                                        std::in_place};
  fdf_testing::DriverUnderTest<aml_i2c::Dfv2Driver> aml_i2c_driver_;
};

TEST_F(AmlI2cDfv2Test, SmallWrite) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  constexpr uint8_t kWriteData[]{0x45, 0xd9, 0x65, 0xbc, 0x31, 0x26, 0xd7, 0xe5};

  uint8_t write_buffer[std::size(kWriteData)];
  memcpy(write_buffer, kWriteData, sizeof(write_buffer));
  const i2c_impl_op_t op{
      .address = 0x13,
      .data_buffer = write_buffer,
      .data_size = sizeof(write_buffer),
      .is_read = false,
      .stop = true,
  };

  EXPECT_OK(i2c->Transact(&op, 1));

  const std::vector transfers = controller().GetTransfers();
  ASSERT_EQ(transfers.size(), 1);

  EXPECT_EQ(transfers[0].target_addr, 0x13);
  ASSERT_EQ(transfers[0].write_data.size(), std::size(kWriteData));
  EXPECT_BYTES_EQ(transfers[0].write_data.data(), kWriteData, sizeof(kWriteData));
  transfers[0].ExpectTokenListEq({
      kStart,
      kTargetAddrWr,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kStop,
      kEnd,
  });
}

TEST_F(AmlI2cDfv2Test, BigWrite) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  constexpr uint8_t kWriteData[]{0xb9, 0x17, 0x32, 0xba, 0x8e, 0xf7, 0x19, 0xf2, 0x78, 0xbf,
                                 0xcb, 0xd3, 0xdc, 0xad, 0xbd, 0x78, 0x1b, 0xa8, 0xef, 0x1a};

  uint8_t write_buffer[std::size(kWriteData)];
  memcpy(write_buffer, kWriteData, sizeof(write_buffer));
  const i2c_impl_op_t op{
      .address = 0x5f,
      .data_buffer = write_buffer,
      .data_size = sizeof(write_buffer),
      .is_read = false,
      .stop = true,
  };

  EXPECT_OK(i2c->Transact(&op, 1));

  const std::vector transfers = controller().GetTransfers();
  ASSERT_EQ(transfers.size(), 1);

  EXPECT_EQ(transfers[0].target_addr, 0x5f);
  ASSERT_EQ(transfers[0].write_data.size(), std::size(kWriteData));
  EXPECT_BYTES_EQ(transfers[0].write_data.data(), kWriteData, sizeof(kWriteData));
  transfers[0].ExpectTokenListEq({
      // First transfer
      kStart,
      kTargetAddrWr,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kEnd,

      // Second transfer
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kEnd,

      // Third transfer
      kData,
      kData,
      kData,
      kData,
      kStop,
      kEnd,
  });
}

TEST_F(AmlI2cDfv2Test, SmallRead) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  constexpr uint8_t kExpectedReadData[]{0xf0, 0xdb, 0xdf, 0x6b, 0xb9, 0x3e, 0xa6, 0xfa};
  controller().SetReadData({kExpectedReadData, std::size(kExpectedReadData)});

  uint8_t read_buffer[std::size(kExpectedReadData)];
  memset(read_buffer, 0xaa, sizeof(read_buffer));
  const i2c_impl_op_t op{
      .address = 0x41,
      .data_buffer = read_buffer,
      .data_size = sizeof(read_buffer),
      .is_read = true,
      .stop = true,
  };

  EXPECT_OK(i2c->Transact(&op, 1));

  const std::vector transfers = controller().GetTransfers();
  ASSERT_EQ(transfers.size(), 1);

  EXPECT_EQ(transfers[0].target_addr, 0x41);
  EXPECT_BYTES_EQ(read_buffer, kExpectedReadData, sizeof(read_buffer));
  transfers[0].ExpectTokenListEq({
      kStart,
      kTargetAddrRd,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kDataLast,
      kStop,
      kEnd,
  });
}

TEST_F(AmlI2cDfv2Test, BigRead) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  constexpr uint8_t kExpectedReadData[]{0xb9, 0x17, 0x32, 0xba, 0x8e, 0xf7, 0x19, 0xf2, 0x78, 0xbf,
                                        0xcb, 0xd3, 0xdc, 0xad, 0xbd, 0x78, 0x1b, 0xa8, 0xef, 0x1a};
  controller().SetReadData({kExpectedReadData, std::size(kExpectedReadData)});

  uint8_t read_buffer[std::size(kExpectedReadData)];
  memset(read_buffer, 0xaa, sizeof(read_buffer));
  const i2c_impl_op_t op{
      .address = 0x29,
      .data_buffer = read_buffer,
      .data_size = sizeof(read_buffer),
      .is_read = true,
      .stop = true,
  };

  EXPECT_OK(i2c->Transact(&op, 1));

  const std::vector transfers = controller().GetTransfers();
  ASSERT_EQ(transfers.size(), 1);

  EXPECT_EQ(transfers[0].target_addr, 0x29);
  EXPECT_BYTES_EQ(read_buffer, kExpectedReadData, sizeof(read_buffer));
  transfers[0].ExpectTokenListEq({
      // First transfer
      kStart,
      kTargetAddrRd,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kEnd,

      // Second transfer
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kEnd,

      // Third transfer
      kData,
      kData,
      kData,
      kDataLast,
      kStop,
      kEnd,
  });
}

TEST_F(AmlI2cDfv2Test, NoStopFlag) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  uint8_t buffer[4];
  const i2c_impl_op_t op{
      .data_buffer = buffer,
      .data_size = sizeof(buffer),
      .is_read = false,
      .stop = false,
  };

  EXPECT_OK(i2c->Transact(&op, 1));

  const std::vector transfers = controller().GetTransfers();
  ASSERT_EQ(transfers.size(), 1);

  transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kData, kData, kData, kEnd});
}

TEST_F(AmlI2cDfv2Test, TransferError) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  uint8_t buffer[4];
  controller().SetReadData({buffer, std::size(buffer)});
  const i2c_impl_op_t op{
      .data_buffer = buffer,
      .data_size = sizeof(buffer),
      .is_read = true,
      .stop = false,
  };

  mmio()[kControlReg / sizeof(uint32_t)] = 1 << 3;

  EXPECT_NOT_OK(i2c->Transact(&op, 1));
}

TEST_F(AmlI2cDfv2Test, ManyTransactions) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  constexpr uint8_t kExpectedReadData[]{0x85, 0xb0, 0xd0, 0x1c, 0xc6, 0x8a, 0x35, 0xfc,
                                        0xcf, 0xca, 0x95, 0x01, 0x61, 0x42, 0x60, 0x8c,
                                        0xa6, 0x01, 0xd6, 0x2e, 0x38, 0x20, 0x09, 0xfa};
  controller().SetReadData({kExpectedReadData, std::size(kExpectedReadData)});

  constexpr uint8_t kExpectedWriteData[]{0x39, 0xf0, 0xf9, 0x17, 0xad, 0x51, 0xdc, 0x30, 0xe5};

  uint8_t read_buffer_1[20];
  uint8_t read_buffer_2[4];
  memset(read_buffer_1, 0xaa, sizeof(read_buffer_1));
  memset(read_buffer_2, 0xaa, sizeof(read_buffer_2));

  uint8_t write_buffer_1[1]{kExpectedWriteData[0]};
  uint8_t write_buffer_2[8];
  static_assert(sizeof(kExpectedWriteData) == sizeof(write_buffer_1) + sizeof(write_buffer_2));
  memcpy(write_buffer_2, kExpectedWriteData + 1, sizeof(write_buffer_2));

  const i2c_impl_op_t ops[]{
      {
          .address = 0x1c,
          .data_buffer = write_buffer_1,
          .data_size = sizeof(write_buffer_1),
          .is_read = false,
          .stop = false,
      },
      {
          .address = 0x2d,
          .data_buffer = read_buffer_1,
          .data_size = sizeof(read_buffer_1),
          .is_read = true,
          .stop = true,
      },
      {
          .address = 0x3e,
          .data_buffer = write_buffer_2,
          .data_size = sizeof(write_buffer_2),
          .is_read = false,
          .stop = true,
      },
      {
          .address = 0x4f,
          .data_buffer = read_buffer_2,
          .data_size = sizeof(read_buffer_2),
          .is_read = true,
          .stop = false,
      },
  };

  EXPECT_OK(i2c->Transact(ops, std::size(ops)));

  const std::vector transfers = controller().GetTransfers();
  ASSERT_EQ(transfers.size(), 4);

  EXPECT_EQ(transfers[0].target_addr, 0x1c);
  ASSERT_EQ(transfers[0].write_data.size(), 1);
  EXPECT_EQ(transfers[0].write_data[0], kExpectedWriteData[0]);
  transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kEnd});

  EXPECT_EQ(transfers[1].target_addr, 0x2d);
  EXPECT_BYTES_EQ(read_buffer_1, kExpectedReadData, sizeof(read_buffer_1));
  transfers[1].ExpectTokenListEq({
      // First transfer
      kStart,
      kTargetAddrRd,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kEnd,

      // Second transfer
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kEnd,

      // Third transfer
      kData,
      kData,
      kData,
      kDataLast,
      kStop,
      kEnd,
  });

  EXPECT_EQ(transfers[2].target_addr, 0x3e);
  ASSERT_EQ(transfers[2].write_data.size(), std::size(write_buffer_2));
  EXPECT_BYTES_EQ(transfers[2].write_data.data(), kExpectedWriteData + 1, sizeof(write_buffer_2));
  transfers[2].ExpectTokenListEq({
      kStart,
      kTargetAddrWr,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kData,
      kStop,
      kEnd,
  });

  EXPECT_EQ(transfers[3].target_addr, 0x4f);
  EXPECT_BYTES_EQ(read_buffer_2, kExpectedReadData + sizeof(read_buffer_1), sizeof(read_buffer_2));
  transfers[3].ExpectTokenListEq({
      Token::kStart,
      Token::kTargetAddrRd,
      Token::kData,
      Token::kData,
      Token::kData,
      Token::kDataLast,
      Token::kEnd,
  });
}

TEST_F(AmlI2cDfv2Test, TransactionTooBig) {
  InitDriver();

  zx::result i2c = I2cImplClient();
  ASSERT_OK(i2c.status_value());

  uint8_t buffer[513];
  i2c_impl_op_t op{
      .data_buffer = buffer,
      .data_size = 512,
      .is_read = false,
  };
  EXPECT_OK(i2c->Transact(&op, 1));

  op.data_size = 513;
  EXPECT_NOT_OK(i2c->Transact(&op, 1));
}

TEST_F(AmlI2cDfv2Test, Metadata) {
  constexpr aml_i2c_delay_values kMetadata{.quarter_clock_delay = 0x3cd, .clock_low_delay = 0xf12};

  InitDriver(kMetadata);

  EXPECT_EQ(mmio()[kControlReg / sizeof(uint32_t)], 0x3cd << 12);
  EXPECT_EQ(mmio()[kTargetAddrReg / sizeof(uint32_t)], (0xf12 << 16) | (1 << 28));
}

TEST_F(AmlI2cDfv2Test, NoMetadata) {
  InitDriver(std::nullopt);

  EXPECT_EQ(mmio()[kControlReg / sizeof(uint32_t)], 0);
  EXPECT_EQ(mmio()[kTargetAddrReg / sizeof(uint32_t)], 0);
}

TEST_F(AmlI2cDfv2Test, MmioIrqCountInvalid) { InitDriver(std::nullopt, 2, 2, false); }

}  // namespace aml_i2c
