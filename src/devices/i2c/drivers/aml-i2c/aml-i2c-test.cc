// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-i2c.h"

#include <fidl/fuchsia.hardware.i2c.businfo/cpp/wire.h>
#include <fuchsia/hardware/i2cimpl/cpp/banjo.h>
#include <lib/ddk/metadata.h>
#include <lib/zx/clock.h>

#include <span>
#include <vector>

#include <fake-mmio-reg/fake-mmio-reg.h>
#include <soc/aml-common/aml-i2c.h>
#include <zxtest/zxtest.h>

#include "aml-i2c-regs.h"
#include "src/devices/bus/testing/fake-pdev/fake-pdev.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace aml_i2c {

class FakeAmlI2cController {
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

  explicit FakeAmlI2cController(zx::unowned_interrupt irq)
      : mmio_(regs_, sizeof(reg_values_[0]), std::size(regs_)), irq_(std::move(irq)) {
    for (size_t i = 0; i < std::size(reg_values_); i++) {
      regs_[i].SetReadCallback(ReadRegCallback(i));
      regs_[i].SetWriteCallback(WriteRegCallback(i));
    }
    mmio_[kControlReg].SetWriteCallback([&](uint64_t value) { WriteControlReg(value); });
  }

  FakeAmlI2cController() : FakeAmlI2cController(zx::unowned_interrupt{}) {}

  // A move constructor must exist in order call emplace_back() or reserve() on an
  // std::vector<FakeAmlI2cController>. It should not be called however as we make sure the that
  // vector never needs to resize.
  FakeAmlI2cController(FakeAmlI2cController&& other) noexcept
      : FakeAmlI2cController(std::move(other.irq_)) {
    EXPECT_TRUE(false, "Move constructor called");
  }
  FakeAmlI2cController& operator=(FakeAmlI2cController&& other) = delete;

  FakeAmlI2cController(const FakeAmlI2cController& other) = delete;
  FakeAmlI2cController& operator=(const FakeAmlI2cController& other) = delete;

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

  ddk_fake::FakeMmioReg regs_[8];
  ddk_fake::FakeMmioRegRegion mmio_;
  uint32_t reg_values_[8]{};
  zx::unowned_interrupt irq_;
  std::vector<Transfer> transfers_;
  cpp20::span<const uint8_t> read_data_;
};

class AmlI2cTest : public zxtest::Test {
 public:
  AmlI2cTest() {
    EXPECT_NULL(instance_);
    instance_ = this;
  }
  ~AmlI2cTest() override {
    EXPECT_EQ(instance_, this);
    instance_ = nullptr;
  }

  static zx_status_t MakeMmioBuffer(const pdev_mmio_t& pdev_mmio,
                                    std::optional<fdf::MmioBuffer>* mmio) {
    ZX_ASSERT(instance_ != nullptr);
    return instance_->MakeFakeMmioBuffer(pdev_mmio, mmio);
  }

 protected:
  static constexpr size_t kMmioSize = sizeof(uint32_t) * 8;

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

  cpp20::span<uint32_t> mmio(size_t index) { return controllers_[index].mmio(); }

  void InitResources(uint32_t bus_count) {
    root_->AddProtocol(ZX_PROTOCOL_PDEV, pdev_.proto()->ops, pdev_.proto()->ctx);

    controllers_.reserve(bus_count);
    for (uint32_t i = 0; i < bus_count; i++) {
      pdev_.set_mmio(i, {.offset = i, .size = kMmioSize});
      controllers_.emplace_back(pdev_.CreateVirtualInterrupt(i));
    }

    pdev_.set_device_info({{.mmio_count = bus_count, .irq_count = bus_count}});
  }

  void Init(uint32_t bus_count) {
    EXPECT_NO_FATAL_FAILURE(InitResources(bus_count));

    EXPECT_OK(AmlI2c::Bind(nullptr, root_.get()));
    ASSERT_EQ(root_->child_count(), 1);

    AmlI2c* const i2c = root_->GetLatestChild()->GetDeviceContext<AmlI2c>();
    ASSERT_EQ(i2c->i2c_devs_.size(), bus_count);
    for (auto& i2c_dev : i2c->i2c_devs_) {
      i2c_dev.timeout_ = zx::duration(ZX_TIME_INFINITE);
    }
  }

  fake_pdev::FakePDev pdev_;
  std::shared_ptr<MockDevice> root_ = MockDevice::FakeRootParent();
  std::vector<FakeAmlI2cController> controllers_;

 private:
  inline static AmlI2cTest* instance_ = nullptr;

  zx_status_t MakeFakeMmioBuffer(const pdev_mmio_t& pdev_mmio,
                                 std::optional<fdf::MmioBuffer>* mmio) {
    if (pdev_mmio.offset >= controllers_.size()) {
      return ZX_ERR_NOT_FOUND;
    }
    if (pdev_mmio.size != kMmioSize) {
      return ZX_ERR_INVALID_ARGS;
    }

    mmio->emplace(controllers_[pdev_mmio.offset].GetMmioBuffer());
    return ZX_OK;
  }
};

TEST_F(AmlI2cTest, SmallWrite) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

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

  EXPECT_OK(i2c.Transact(0, &op, 1));

  const std::vector transfers = controllers_[0].GetTransfers();
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

TEST_F(AmlI2cTest, BigWrite) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

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

  EXPECT_OK(i2c.Transact(0, &op, 1));

  const std::vector transfers = controllers_[0].GetTransfers();
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

TEST_F(AmlI2cTest, SmallRead) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  constexpr uint8_t kExpectedReadData[]{0xf0, 0xdb, 0xdf, 0x6b, 0xb9, 0x3e, 0xa6, 0xfa};
  controllers_[0].SetReadData({kExpectedReadData, std::size(kExpectedReadData)});

  uint8_t read_buffer[std::size(kExpectedReadData)];
  memset(read_buffer, 0xaa, sizeof(read_buffer));
  const i2c_impl_op_t op{
      .address = 0x41,
      .data_buffer = read_buffer,
      .data_size = sizeof(read_buffer),
      .is_read = true,
      .stop = true,
  };

  EXPECT_OK(i2c.Transact(0, &op, 1));

  const std::vector transfers = controllers_[0].GetTransfers();
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

TEST_F(AmlI2cTest, BigRead) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  constexpr uint8_t kExpectedReadData[]{0xb9, 0x17, 0x32, 0xba, 0x8e, 0xf7, 0x19, 0xf2, 0x78, 0xbf,
                                        0xcb, 0xd3, 0xdc, 0xad, 0xbd, 0x78, 0x1b, 0xa8, 0xef, 0x1a};
  controllers_[0].SetReadData({kExpectedReadData, std::size(kExpectedReadData)});

  uint8_t read_buffer[std::size(kExpectedReadData)];
  memset(read_buffer, 0xaa, sizeof(read_buffer));
  const i2c_impl_op_t op{
      .address = 0x29,
      .data_buffer = read_buffer,
      .data_size = sizeof(read_buffer),
      .is_read = true,
      .stop = true,
  };

  EXPECT_OK(i2c.Transact(0, &op, 1));

  const std::vector transfers = controllers_[0].GetTransfers();
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

TEST_F(AmlI2cTest, NoStopFlag) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  uint8_t buffer[4];
  const i2c_impl_op_t op{
      .data_buffer = buffer,
      .data_size = sizeof(buffer),
      .is_read = false,
      .stop = false,
  };

  EXPECT_OK(i2c.Transact(0, &op, 1));

  const std::vector transfers = controllers_[0].GetTransfers();
  ASSERT_EQ(transfers.size(), 1);

  transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kData, kData, kData, kEnd});
}

TEST_F(AmlI2cTest, TransferError) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  uint8_t buffer[4];
  controllers_[0].SetReadData({buffer, std::size(buffer)});
  const i2c_impl_op_t op{
      .data_buffer = buffer,
      .data_size = sizeof(buffer),
      .is_read = true,
      .stop = false,
  };

  mmio(0)[kControlReg / sizeof(uint32_t)] = 1 << 3;

  EXPECT_NOT_OK(i2c.Transact(0, &op, 1));
}

TEST_F(AmlI2cTest, ManyTransactions) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  constexpr uint8_t kExpectedReadData[]{0x85, 0xb0, 0xd0, 0x1c, 0xc6, 0x8a, 0x35, 0xfc,
                                        0xcf, 0xca, 0x95, 0x01, 0x61, 0x42, 0x60, 0x8c,
                                        0xa6, 0x01, 0xd6, 0x2e, 0x38, 0x20, 0x09, 0xfa};
  controllers_[0].SetReadData({kExpectedReadData, std::size(kExpectedReadData)});

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

  EXPECT_OK(i2c.Transact(0, ops, std::size(ops)));

  const std::vector transfers = controllers_[0].GetTransfers();
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

TEST_F(AmlI2cTest, MultipleControllers) {
  EXPECT_NO_FATAL_FAILURE(Init(4));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  uint8_t write_buffer[]{0x45};
  const i2c_impl_op_t op{
      .address = 0x56,
      .data_buffer = write_buffer,
      .data_size = sizeof(write_buffer),
      .is_read = false,
  };

  EXPECT_OK(i2c.Transact(0, &op, 1));
  {
    const std::vector transfers = controllers_[0].GetTransfers();
    ASSERT_EQ(transfers.size(), 1);
    EXPECT_EQ(transfers[0].target_addr, 0x56);
    ASSERT_EQ(transfers[0].write_data.size(), 1);
    EXPECT_EQ(transfers[0].write_data[0], 0x45);
    transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kEnd});
  }

  EXPECT_OK(i2c.Transact(1, &op, 1));
  {
    const std::vector transfers = controllers_[1].GetTransfers();
    ASSERT_EQ(transfers.size(), 1);
    EXPECT_EQ(transfers[0].target_addr, 0x56);
    ASSERT_EQ(transfers[0].write_data.size(), 1);
    EXPECT_EQ(transfers[0].write_data[0], 0x45);
    transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kEnd});
  }

  EXPECT_OK(i2c.Transact(2, &op, 1));
  {
    const std::vector transfers = controllers_[2].GetTransfers();
    ASSERT_EQ(transfers.size(), 1);
    EXPECT_EQ(transfers[0].target_addr, 0x56);
    ASSERT_EQ(transfers[0].write_data.size(), 1);
    EXPECT_EQ(transfers[0].write_data[0], 0x45);
    transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kEnd});
  }

  EXPECT_OK(i2c.Transact(3, &op, 1));
  {
    const std::vector transfers = controllers_[3].GetTransfers();
    ASSERT_EQ(transfers.size(), 1);
    EXPECT_EQ(transfers[0].target_addr, 0x56);
    ASSERT_EQ(transfers[0].write_data.size(), 1);
    EXPECT_EQ(transfers[0].write_data[0], 0x45);
    transfers[0].ExpectTokenListEq({kStart, kTargetAddrWr, kData, kEnd});
  }

  EXPECT_NOT_OK(i2c.Transact(4, &op, 1));
}

TEST_F(AmlI2cTest, TransactionTooBig) {
  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  uint8_t buffer[513];
  i2c_impl_op_t op{
      .data_buffer = buffer,
      .data_size = 512,
      .is_read = false,
  };
  EXPECT_OK(i2c.Transact(0, &op, 1));

  op.data_size = 513;
  EXPECT_NOT_OK(i2c.Transact(0, &op, 1));
}

TEST_F(AmlI2cTest, Metadata) {
  aml_i2c_delay_values metadata[]{
      {.quarter_clock_delay = 0, .clock_low_delay = 0},
      {.quarter_clock_delay = 0x3cd, .clock_low_delay = 0xf12},
      {.quarter_clock_delay = 0, .clock_low_delay = 0},
  };

  root_->SetMetadata(DEVICE_METADATA_PRIVATE, &metadata, sizeof(metadata));

  EXPECT_NO_FATAL_FAILURE(Init(3));

  EXPECT_EQ(mmio(0)[kControlReg / sizeof(uint32_t)], 0);
  EXPECT_EQ(mmio(0)[kTargetAddrReg / sizeof(uint32_t)], 0);

  EXPECT_EQ(mmio(1)[kControlReg / sizeof(uint32_t)], 0x3cd << 12);
  EXPECT_EQ(mmio(1)[kTargetAddrReg / sizeof(uint32_t)], (0xf12 << 16) | (1 << 28));

  EXPECT_EQ(mmio(2)[kControlReg / sizeof(uint32_t)], 0);
  EXPECT_EQ(mmio(2)[kTargetAddrReg / sizeof(uint32_t)], 0);
}

TEST_F(AmlI2cTest, NoMetadata) {
  EXPECT_NO_FATAL_FAILURE(Init(3));

  EXPECT_EQ(mmio(0)[kControlReg / sizeof(uint32_t)], 0);
  EXPECT_EQ(mmio(0)[kTargetAddrReg / sizeof(uint32_t)], 0);

  EXPECT_EQ(mmio(1)[kControlReg / sizeof(uint32_t)], 0);
  EXPECT_EQ(mmio(1)[kTargetAddrReg / sizeof(uint32_t)], 0);

  EXPECT_EQ(mmio(2)[kControlReg / sizeof(uint32_t)], 0);
  EXPECT_EQ(mmio(2)[kTargetAddrReg / sizeof(uint32_t)], 0);
}

TEST_F(AmlI2cTest, MetadataTooBig) {
  aml_i2c_delay_values metadata[]{
      {.quarter_clock_delay = 0, .clock_low_delay = 0},
      {.quarter_clock_delay = 0x3cd, .clock_low_delay = 0xf12},
      {.quarter_clock_delay = 0, .clock_low_delay = 0},
      {.quarter_clock_delay = 0, .clock_low_delay = 0},
  };

  root_->SetMetadata(DEVICE_METADATA_PRIVATE, &metadata, sizeof(metadata));

  EXPECT_NO_FATAL_FAILURE(InitResources(3));

  EXPECT_NOT_OK(AmlI2c::Bind(nullptr, root_.get()));
}

TEST_F(AmlI2cTest, MetadataTooSmall) {
  aml_i2c_delay_values metadata[]{
      {.quarter_clock_delay = 0, .clock_low_delay = 0},
      {.quarter_clock_delay = 0x3cd, .clock_low_delay = 0xf12},
  };

  root_->SetMetadata(DEVICE_METADATA_PRIVATE, &metadata, sizeof(metadata));

  EXPECT_NO_FATAL_FAILURE(InitResources(3));

  EXPECT_NOT_OK(AmlI2c::Bind(nullptr, root_.get()));
}

TEST_F(AmlI2cTest, CanUsePDevFragment) {
  root_->AddProtocol(ZX_PROTOCOL_PDEV, pdev_.proto()->ops, pdev_.proto()->ctx, "pdev");

  pdev_.set_mmio(0, {.offset = 0, .size = kMmioSize});
  controllers_.emplace_back(pdev_.CreateVirtualInterrupt(0));

  pdev_.set_device_info({{.mmio_count = 1, .irq_count = 1}});

  EXPECT_OK(AmlI2c::Bind(nullptr, root_.get()));
  ASSERT_EQ(root_->child_count(), 1);
}

TEST_F(AmlI2cTest, MmioIrqMismatch) {
  root_->AddProtocol(ZX_PROTOCOL_PDEV, pdev_.proto()->ops, pdev_.proto()->ctx, "pdev");

  pdev_.set_device_info({{.mmio_count = 2, .irq_count = 1}});

  EXPECT_NOT_OK(AmlI2c::Bind(nullptr, root_.get()));
}

TEST_F(AmlI2cTest, BusBaseSetByChannelMetadata) {
  using fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata;
  using fuchsia_hardware_i2c_businfo::wire::I2CChannel;

  fidl::Arena arena;

  fidl::VectorView<I2CChannel> channels(arena, 3);
  channels[0] = I2CChannel::Builder(arena).bus_id(2).Build();
  channels[1] = I2CChannel::Builder(arena).bus_id(2).Build();
  channels[2] = I2CChannel::Builder(arena).bus_id(2).Build();

  auto metadata = I2CBusMetadata::Builder(arena).channels(channels).Build();

  auto result = fidl::Persist(metadata);
  ASSERT_TRUE(result.is_ok());

  root_->SetMetadata(DEVICE_METADATA_I2C_CHANNELS, result->data(), result->size());

  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  EXPECT_EQ(i2c.GetBusBase(), 2);
  EXPECT_EQ(i2c.GetBusCount(), 1);

  uint8_t buffer;
  const i2c_impl_op_t op{.data_buffer = &buffer, .data_size = 1, .is_read = false};
  // Do one transaction on this bus to make sure the ID is recognized.
  EXPECT_OK(i2c.Transact(2, &op, 1));

  EXPECT_NOT_OK(i2c.Transact(0, &op, 1));
  EXPECT_NOT_OK(i2c.Transact(1, &op, 1));
  EXPECT_NOT_OK(i2c.Transact(3, &op, 1));
}

TEST_F(AmlI2cTest, BusBaseSetByBusIdIgnoreChannels) {
  using fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata;
  using fuchsia_hardware_i2c_businfo::wire::I2CChannel;

  fidl::Arena arena;

  fidl::VectorView<I2CChannel> channels(arena, 3);
  channels[0] = I2CChannel::Builder(arena).bus_id(1).Build();
  channels[1] = I2CChannel::Builder(arena).bus_id(1).Build();
  channels[2] = I2CChannel::Builder(arena).bus_id(1).Build();

  auto metadata = I2CBusMetadata::Builder(arena).channels(channels).bus_id(2).Build();

  auto result = fidl::Persist(metadata);
  ASSERT_TRUE(result.is_ok());

  root_->SetMetadata(DEVICE_METADATA_I2C_CHANNELS, result->data(), result->size());

  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  EXPECT_EQ(i2c.GetBusBase(), 2);
  EXPECT_EQ(i2c.GetBusCount(), 1);

  uint8_t buffer;
  const i2c_impl_op_t op{.data_buffer = &buffer, .data_size = 1, .is_read = false};
  // Do one transaction on this bus to make sure the ID is recognized.
  EXPECT_OK(i2c.Transact(2, &op, 1));

  EXPECT_NOT_OK(i2c.Transact(0, &op, 1));
  EXPECT_NOT_OK(i2c.Transact(1, &op, 1));
  EXPECT_NOT_OK(i2c.Transact(3, &op, 1));
}

TEST_F(AmlI2cTest, BusMetadataIgnoredMultipleControllers) {
  using fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata;
  using fuchsia_hardware_i2c_businfo::wire::I2CChannel;

  fidl::Arena arena;

  fidl::VectorView<I2CChannel> channels(arena, 3);
  channels[0] = I2CChannel::Builder(arena).bus_id(2).Build();
  channels[1] = I2CChannel::Builder(arena).bus_id(2).Build();
  channels[2] = I2CChannel::Builder(arena).bus_id(2).Build();

  auto metadata = I2CBusMetadata::Builder(arena).channels(channels).Build();

  auto result = fidl::Persist(metadata);
  ASSERT_TRUE(result.is_ok());

  root_->SetMetadata(DEVICE_METADATA_I2C_CHANNELS, result->data(), result->size());

  // Initialize the device with three controllers, which causes the channel bus IDs to be ignored
  // when determining the bus base value.
  EXPECT_NO_FATAL_FAILURE(Init(3));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  // Having multiple controllers causes the bus metadata to be ignored.
  EXPECT_EQ(i2c.GetBusBase(), 0);
  EXPECT_EQ(i2c.GetBusCount(), 3);

  uint8_t buffer;
  const i2c_impl_op_t op{.data_buffer = &buffer, .data_size = 1, .is_read = false};
  EXPECT_OK(i2c.Transact(0, &op, 1));
  EXPECT_OK(i2c.Transact(1, &op, 1));
  EXPECT_OK(i2c.Transact(2, &op, 1));
}

TEST_F(AmlI2cTest, BusMetadataIgnoredMultipleBusIds) {
  using fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata;
  using fuchsia_hardware_i2c_businfo::wire::I2CChannel;

  fidl::Arena arena;

  fidl::VectorView<I2CChannel> channels(arena, 3);
  channels[0] = I2CChannel::Builder(arena).bus_id(2).Build();
  channels[1] = I2CChannel::Builder(arena).bus_id(2).Build();
  channels[2] = I2CChannel::Builder(arena).bus_id(5).Build();

  auto metadata = I2CBusMetadata::Builder(arena).channels(channels).Build();

  auto result = fidl::Persist(metadata);
  ASSERT_TRUE(result.is_ok());

  root_->SetMetadata(DEVICE_METADATA_I2C_CHANNELS, result->data(), result->size());

  EXPECT_NO_FATAL_FAILURE(Init(1));

  ddk::I2cImplProtocolClient i2c(root_->GetLatestChild());
  EXPECT_TRUE(i2c.is_valid());

  EXPECT_EQ(i2c.GetBusBase(), 0);
  EXPECT_EQ(i2c.GetBusCount(), 1);

  uint8_t buffer;
  const i2c_impl_op_t op{.data_buffer = &buffer, .data_size = 1, .is_read = false};
  EXPECT_OK(i2c.Transact(0, &op, 1));
  EXPECT_NOT_OK(i2c.Transact(2, &op, 1));
  EXPECT_NOT_OK(i2c.Transact(5, &op, 1));
}
}  // namespace aml_i2c

namespace ddk {

zx_status_t PDevMakeMmioBufferWeak(const pdev_mmio_t& pdev_mmio,
                                   std::optional<fdf::MmioBuffer>* mmio, uint32_t cache_policy) {
  return aml_i2c::AmlI2cTest::MakeMmioBuffer(pdev_mmio, mmio);
}

}  // namespace ddk
