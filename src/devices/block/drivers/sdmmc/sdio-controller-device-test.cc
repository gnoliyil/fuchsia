// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sdio-controller-device.h"

#include <lib/fit/defer.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/sdio/hw.h>
#include <lib/zx/vmo.h>

#include <memory>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <zxtest/zxtest.h>

#include "fake-sdmmc-device.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {

constexpr uint32_t OpCondFunctions(uint32_t functions) {
  return SDIO_SEND_OP_COND_RESP_IORDY | (functions << SDIO_SEND_OP_COND_RESP_NUM_FUNC_LOC);
}

}  // namespace

namespace sdmmc {

class SdioControllerDeviceTest : public zxtest::Test {
 public:
  SdioControllerDeviceTest() {
    dut_ = std::make_unique<SdioControllerDevice>(parent_.get(), SdmmcDevice(sdmmc_.GetClient()));
  }

  void SetUp() override {
    sdmmc_.Reset();

    // Set all function block sizes (and the host max transfer size) to 1 so that the initialization
    // checks pass. Individual test cases can override these by overwriting the CIS or creating a
    // new one and overwriting the CIS pointer.
    sdmmc_.Write(0x0009, std::vector<uint8_t>{0x00, 0x20, 0x00}, 0);

    sdmmc_.Write(0x2000,
                 std::vector<uint8_t>{
                     0x22,        // Function extensions tuple.
                     0x04,        // Function extensions tuple size.
                     0x00,        // Type of extended data.
                     0x01, 0x00,  // Function 0 block size.
                 },
                 0);

    sdmmc_.Write(0x1000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
    sdmmc_.Write(0x100e, std::vector<uint8_t>{0x01, 0x00}, 0);

    sdmmc_.Write(0x0109, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0209, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0309, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0409, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0509, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0609, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0709, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);

    sdmmc_.set_host_info({
        .caps = 0,
        .max_transfer_size = 1,
        .max_transfer_size_non_dma = 1,
        .prefs = 0,
    });

    dut_->Init();
  }

 protected:
  std::shared_ptr<MockDevice> parent_ = MockDevice::FakeRootParent();
  FakeSdmmcDevice sdmmc_;
  std::unique_ptr<SdioControllerDevice> dut_;
};

class SdioScatterGatherTest : public zxtest::Test {
 public:
  SdioScatterGatherTest() {
    dut_ = std::make_unique<SdioControllerDevice>(parent_.get(), SdmmcDevice(sdmmc_.GetClient()));
  }

  void SetUp() override { sdmmc_.Reset(); }

  void Init(const uint8_t function, const bool multiblock) {
    sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
      out_response[0] = OpCondFunctions(5);
    });
    sdmmc_.Write(
        SDIO_CIA_CCCR_CARD_CAPS_ADDR,
        std::vector<uint8_t>{static_cast<uint8_t>(multiblock ? SDIO_CIA_CCCR_CARD_CAP_SMB : 0)}, 0);

    sdmmc_.Write(0x0009, std::vector<uint8_t>{0x00, 0x20, 0x00}, 0);
    sdmmc_.Write(0x2000, std::vector<uint8_t>{0x22, 0x04, 0x00, 0x00, 0x02}, 0);

    // Set the maximum block size for function 1-5 to eight bytes.
    sdmmc_.Write(0x0109, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0209, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0309, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0409, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x0509, std::vector<uint8_t>{0x00, 0x10, 0x00}, 0);
    sdmmc_.Write(0x1000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
    sdmmc_.Write(0x100e, std::vector<uint8_t>{0x08, 0x00}, 0);

    sdmmc_.set_host_info({
        .caps = 0,
        .max_transfer_size = 1024,
        .max_transfer_size_non_dma = 1024,
        .prefs = 0,
    });
    EXPECT_OK(dut_->Init());

    EXPECT_OK(dut_->Probe());
    EXPECT_OK(dut_->SdioUpdateBlockSize(function, 4, false));

    sdmmc_.requests().clear();

    zx::vmo vmo1, vmo3;
    ASSERT_OK(mapper1_.CreateAndMap(zx_system_get_page_size(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                    nullptr, &vmo1));
    ASSERT_OK(mapper2_.CreateAndMap(zx_system_get_page_size(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                    nullptr, &vmo2_));
    ASSERT_OK(mapper3_.CreateAndMap(zx_system_get_page_size(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                    nullptr, &vmo3));

    const uint32_t vmo_rights = SDMMC_VMO_RIGHT_READ | SDMMC_VMO_RIGHT_WRITE;
    EXPECT_OK(dut_->SdioRegisterVmo(function, 1, std::move(vmo1), 0, zx_system_get_page_size(),
                                    vmo_rights));
    EXPECT_OK(dut_->SdioRegisterVmo(function, 3, std::move(vmo3), 8, zx_system_get_page_size() - 8,
                                    vmo_rights));
  }

 protected:
  static constexpr uint8_t kTestData1[] = {0x17, 0xc6, 0xf4, 0x4a, 0x92, 0xc6, 0x09, 0x0a,
                                           0x8c, 0x54, 0x08, 0x07, 0xde, 0x5f, 0x8d, 0x59};
  static constexpr uint8_t kTestData2[] = {0x0d, 0x90, 0x85, 0x6a, 0xe2, 0xa9, 0x00, 0x0e,
                                           0xdf, 0x26, 0xe2, 0x17, 0x88, 0x4d, 0x3a, 0x72};
  static constexpr uint8_t kTestData3[] = {0x34, 0x83, 0x15, 0x31, 0x29, 0xa8, 0x4b, 0xe8,
                                           0xd9, 0x1f, 0xa4, 0xf4, 0x8d, 0x3a, 0x27, 0x0c};

  static sdmmc_buffer_region_t MakeBufferRegion(const zx::vmo& vmo, uint64_t offset,
                                                uint64_t size) {
    return {
        .buffer =
            {
                .vmo = vmo.get(),
            },
        .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
        .offset = offset,
        .size = size,
    };
  }

  static sdmmc_buffer_region_t MakeBufferRegion(uint32_t vmo_id, uint64_t offset, uint64_t size) {
    return {
        .buffer =
            {
                .vmo_id = vmo_id,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = offset,
        .size = size,
    };
  }

  struct SdioCmd53 {
    static SdioCmd53 FromArg(uint32_t arg) {
      SdioCmd53 ret = {};
      ret.blocks_or_bytes = arg & SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_MASK;
      ret.address = (arg & SDIO_IO_RW_EXTD_REG_ADDR_MASK) >> SDIO_IO_RW_EXTD_REG_ADDR_LOC;
      ret.op_code = (arg & SDIO_IO_RW_EXTD_OP_CODE_INCR) ? 1 : 0;
      ret.block_mode = (arg & SDIO_IO_RW_EXTD_BLOCK_MODE) ? 1 : 0;
      ret.function_number = (arg & SDIO_IO_RW_EXTD_FN_IDX_MASK) >> SDIO_IO_RW_EXTD_FN_IDX_LOC;
      ret.rw_flag = (arg & SDIO_IO_RW_EXTD_RW_FLAG) ? 1 : 0;
      return ret;
    }

    uint32_t blocks_or_bytes;
    uint32_t address;
    uint32_t op_code;
    uint32_t block_mode;
    uint32_t function_number;
    uint32_t rw_flag;
  };

  std::shared_ptr<MockDevice> parent_ = MockDevice::FakeRootParent();
  FakeSdmmcDevice sdmmc_;
  std::unique_ptr<SdioControllerDevice> dut_;

  zx::vmo vmo2_;
  fzl::VmoMapper mapper1_, mapper2_, mapper3_;
};

TEST_F(SdioControllerDeviceTest, MultiplexInterrupts) {
  EXPECT_OK(dut_->Init());

  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(7);
  });
  EXPECT_OK(dut_->Probe());

  auto stop_thread = fit::defer([&]() { dut_->StopSdioIrqThread(); });

  zx::port port;
  ASSERT_OK(zx::port::create(ZX_PORT_BIND_TO_INTERRUPT, &port));

  zx::interrupt interrupt1, interrupt2, interrupt4, interrupt7;
  ASSERT_OK(dut_->SdioGetInBandIntr(1, &interrupt1));
  ASSERT_OK(dut_->SdioGetInBandIntr(2, &interrupt2));
  ASSERT_OK(dut_->SdioGetInBandIntr(4, &interrupt4));
  ASSERT_OK(dut_->SdioGetInBandIntr(7, &interrupt7));

  ASSERT_OK(interrupt1.bind(port, 1, 0));
  ASSERT_OK(interrupt2.bind(port, 2, 0));
  ASSERT_OK(interrupt4.bind(port, 4, 0));
  ASSERT_OK(interrupt7.bind(port, 7, 0));

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0000'0010}, 0);
  sdmmc_.TriggerInBandInterrupt();

  zx_port_packet_t packet;
  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 1);
  EXPECT_OK(interrupt1.ack());
  dut_->SdioAckInBandIntr(1);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b1111'1110}, 0);
  sdmmc_.TriggerInBandInterrupt();

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 1);
  EXPECT_OK(interrupt1.ack());
  dut_->SdioAckInBandIntr(1);

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 2);
  EXPECT_OK(interrupt2.ack());
  dut_->SdioAckInBandIntr(2);

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 4);
  EXPECT_OK(interrupt4.ack());
  dut_->SdioAckInBandIntr(4);

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 7);
  EXPECT_OK(interrupt7.ack());
  dut_->SdioAckInBandIntr(7);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b1010'0010}, 0);
  sdmmc_.TriggerInBandInterrupt();

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 1);
  EXPECT_OK(interrupt1.ack());
  dut_->SdioAckInBandIntr(1);

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 7);
  EXPECT_OK(interrupt7.ack());
  dut_->SdioAckInBandIntr(7);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0011'0110}, 0);
  sdmmc_.TriggerInBandInterrupt();

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 1);
  EXPECT_OK(interrupt1.ack());
  dut_->SdioAckInBandIntr(1);

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 2);
  EXPECT_OK(interrupt2.ack());
  dut_->SdioAckInBandIntr(2);

  EXPECT_OK(port.wait(zx::time::infinite(), &packet));
  EXPECT_EQ(packet.key, 4);
  EXPECT_OK(interrupt4.ack());
}

TEST_F(SdioControllerDeviceTest, SdioDoRwTxn) {
  // Report five IO functions.
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5);
  });
  sdmmc_.Write(SDIO_CIA_CCCR_CARD_CAPS_ADDR, std::vector<uint8_t>{0x00}, 0);

  // Set the maximum block size for function three to eight bytes.
  sdmmc_.Write(0x0309, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);
  sdmmc_.Write(0x3000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
  sdmmc_.Write(0x300e, std::vector<uint8_t>{0x08, 0x00}, 0);

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = 16,
      .max_transfer_size_non_dma = 16,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());
  EXPECT_OK(dut_->SdioUpdateBlockSize(3, 0, true));

  uint16_t block_size = 0;
  EXPECT_OK(dut_->SdioGetBlockSize(3, &block_size));
  EXPECT_EQ(block_size, 8);

  constexpr uint8_t kTestData[52] = {
      0xff, 0x7c, 0xa6, 0x24, 0x6f, 0x69, 0x7a, 0x39, 0x63, 0x68, 0xef, 0x28, 0xf3,
      0x18, 0x91, 0xf1, 0x68, 0x48, 0x78, 0x2f, 0xbb, 0xb2, 0x9a, 0x63, 0x51, 0xd4,
      0xe1, 0x94, 0xb4, 0x5c, 0x81, 0x94, 0xc7, 0x86, 0x50, 0x33, 0x61, 0xf8, 0x97,
      0x4c, 0x68, 0x71, 0x7f, 0x17, 0x59, 0x82, 0xc5, 0x36, 0xe0, 0x20, 0x0b, 0x56,
  };

  sdmmc_.requests().clear();

  zx::vmo vmo;
  EXPECT_OK(zx::vmo::create(sizeof(kTestData), 0, &vmo));
  EXPECT_OK(vmo.write(kTestData, 0, sizeof(kTestData)));

  sdmmc_buffer_region_t region = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 16,
      .size = 36,
  };
  sdio_rw_txn_t txn = {
      .addr = 0x1ab08,
      .incr = false,
      .write = true,
      .buffers_list = &region,
      .buffers_count = 1,
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  ASSERT_EQ(sdmmc_.requests().size(), 5);

  EXPECT_EQ(sdmmc_.requests()[0].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[1].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[2].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[3].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[4].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);

  sdmmc_.requests().clear();

  // The write sequence should be: four writes of blocks of eight, one write of four bytes. This is
  // a FIFO write, meaning the data will get overwritten each time. Verify the final state of the
  // device.
  const std::vector<uint8_t> read_data = sdmmc_.Read(0x1ab08, 16, 3);
  EXPECT_BYTES_EQ(read_data.data(), kTestData + sizeof(kTestData) - 4, 4);
  EXPECT_BYTES_EQ(read_data.data() + 4, kTestData + sizeof(kTestData) - 8, 4);

  sdmmc_.Write(0x12308, cpp20::span<const uint8_t>(kTestData, sizeof(kTestData)), 3);

  uint8_t buffer[sizeof(kTestData)] = {};
  EXPECT_OK(vmo.write(buffer, 0, sizeof(buffer)));

  region = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 16,
      .size = 36,
  };
  txn = {
      .addr = 0x12308,
      .incr = true,
      .write = false,
      .buffers_list = &region,
      .buffers_count = 1,
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  ASSERT_EQ(sdmmc_.requests().size(), 5);

  EXPECT_EQ(sdmmc_.requests()[0].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[1].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[2].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[3].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);
  EXPECT_EQ(sdmmc_.requests()[4].cmd_flags & (SDMMC_CMD_BLKCNT_EN | SDMMC_CMD_MULTI_BLK), 0);

  EXPECT_OK(vmo.read(buffer, 0, sizeof(buffer)));
  EXPECT_BYTES_EQ(buffer + 16, kTestData, 36);
}

TEST_F(SdioControllerDeviceTest, SdioDoRwTxnMultiBlock) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(7);
  });

  sdmmc_.Write(SDIO_CIA_CCCR_CARD_CAPS_ADDR, std::vector<uint8_t>{SDIO_CIA_CCCR_CARD_CAP_SMB}, 0);

  // Set the maximum block size for function seven to eight bytes.
  sdmmc_.Write(0x709, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);
  sdmmc_.Write(0x3000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
  sdmmc_.Write(0x300e, std::vector<uint8_t>{0x08, 0x00}, 0);

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = 32,
      .max_transfer_size_non_dma = 32,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());
  EXPECT_OK(dut_->SdioUpdateBlockSize(7, 0, true));

  uint16_t block_size = 0;
  EXPECT_OK(dut_->SdioGetBlockSize(7, &block_size));
  EXPECT_EQ(block_size, 8);

  constexpr uint8_t kTestData[132] = {
      // clang-format off
      0x94, 0xfa, 0x41, 0x93, 0x40, 0x81, 0xae, 0x83, 0x85, 0x88, 0x98, 0x6d,
      0x52, 0x1c, 0x53, 0x9c, 0xa7, 0x7a, 0x19, 0x74, 0xc9, 0xa9, 0x47, 0xd9,
      0x64, 0x2b, 0x76, 0x47, 0x55, 0x0b, 0x3d, 0x34, 0xd6, 0xfc, 0xca, 0x7b,
      0xae, 0xe0, 0xff, 0xe3, 0xa2, 0xd3, 0xe5, 0xb6, 0xbc, 0xa4, 0x3d, 0x01,
      0x99, 0x92, 0xdc, 0xac, 0x68, 0xb1, 0x88, 0x22, 0xc4, 0xf4, 0x1a, 0x45,
      0xe9, 0xd3, 0x5e, 0x8c, 0x24, 0x98, 0x7b, 0xf5, 0x32, 0x6d, 0xe5, 0x01,
      0x36, 0x03, 0x9b, 0xee, 0xfa, 0x23, 0x2f, 0xdd, 0xc6, 0xa4, 0x34, 0x58,
      0x23, 0xaa, 0xc9, 0x00, 0x73, 0xb8, 0xe0, 0xd8, 0xde, 0xc4, 0x59, 0x66,
      0x76, 0xd3, 0x65, 0xe0, 0xfa, 0xf7, 0x89, 0x40, 0x3a, 0xa8, 0x83, 0x53,
      0x63, 0xf4, 0x36, 0xea, 0xb3, 0x94, 0xe7, 0x5f, 0x3c, 0xed, 0x8d, 0x3e,
      0xee, 0x1b, 0x75, 0xea, 0xb3, 0x95, 0xd2, 0x25, 0x7c, 0xb9, 0x6d, 0x37,
      // clang-format on
  };

  zx::vmo vmo;
  EXPECT_OK(zx::vmo::create(sizeof(kTestData), 0, &vmo));
  EXPECT_OK(vmo.write(kTestData, 0, sizeof(kTestData)));

  sdmmc_.Write(0x1ab08, cpp20::span<const uint8_t>(kTestData, sizeof(kTestData)), 7);

  sdmmc_buffer_region_t region = {
      .buffer = {.vmo = vmo.get()},
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 64,
      .size = 68,
  };
  sdio_rw_txn_t txn = {
      .addr = 0x1ab08,
      .incr = false,
      .write = false,
      .buffers_list = &region,
      .buffers_count = 1,
  };
  EXPECT_OK(dut_->SdioDoRwTxn(7, &txn));

  uint8_t buffer[sizeof(kTestData)];
  EXPECT_OK(vmo.read(buffer, 0, sizeof(buffer)));

  EXPECT_BYTES_EQ(buffer + 64, kTestData, 64);
  EXPECT_BYTES_EQ(buffer + 128, kTestData, 4);

  EXPECT_OK(vmo.write(kTestData, 0, sizeof(kTestData)));

  region = {
      .buffer = {.vmo = vmo.get()},
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 64,
      .size = 68,
  };
  txn = {
      .addr = 0x12308,
      .incr = true,
      .write = true,
      .buffers_list = &region,
      .buffers_count = 1,
  };
  EXPECT_OK(dut_->SdioDoRwTxn(7, &txn));

  EXPECT_BYTES_EQ(sdmmc_.Read(0x12308, 68, 7).data(), kTestData + 64, 68);
}

TEST_F(SdioControllerDeviceTest, DdkLifecycle) {
  // The interrupt thread is started by AddDevice.
  auto stop_thread = fit::defer([&]() { dut_->StopSdioIrqThread(); });

  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  EXPECT_OK(dut_->Probe());

  EXPECT_OK(dut_->AddDevice());

  EXPECT_EQ(parent_->descendant_count(), 5);

  dut_->DdkAsyncRemove();
  [[maybe_unused]] auto ptr = dut_.release();
  EXPECT_OK(mock_ddk::ReleaseFlaggedDevices(parent_.get()));
  stop_thread.cancel();
}

TEST_F(SdioControllerDeviceTest, SdioIntrPending) {
  bool pending;

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0011'0010}, 0);
  EXPECT_OK(dut_->SdioIntrPending(4, &pending));
  EXPECT_TRUE(pending);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0010'0010}, 0);
  EXPECT_OK(dut_->SdioIntrPending(4, &pending));
  EXPECT_FALSE(pending);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b1000'0000}, 0);
  EXPECT_OK(dut_->SdioIntrPending(7, &pending));
  EXPECT_TRUE(pending);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0000'0000}, 0);
  EXPECT_OK(dut_->SdioIntrPending(7, &pending));
  EXPECT_FALSE(pending);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0000'1110}, 0);
  EXPECT_OK(dut_->SdioIntrPending(1, &pending));
  EXPECT_TRUE(pending);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0000'1110}, 0);
  EXPECT_OK(dut_->SdioIntrPending(2, &pending));
  EXPECT_TRUE(pending);

  sdmmc_.Write(SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, std::vector<uint8_t>{0b0000'1110}, 0);
  EXPECT_OK(dut_->SdioIntrPending(3, &pending));
  EXPECT_TRUE(pending);
}

TEST_F(SdioControllerDeviceTest, EnableDisableFnIntr) {
  sdmmc_.Write(0x04, std::vector<uint8_t>{0b0000'0000}, 0);

  EXPECT_OK(dut_->SdioEnableFnIntr(4));
  EXPECT_EQ(sdmmc_.Read(0x04, 1, 0)[0], 0b0001'0001);

  EXPECT_OK(dut_->SdioEnableFnIntr(7));
  EXPECT_EQ(sdmmc_.Read(0x04, 1, 0)[0], 0b1001'0001);

  EXPECT_OK(dut_->SdioEnableFnIntr(4));
  EXPECT_EQ(sdmmc_.Read(0x04, 1, 0)[0], 0b1001'0001);

  EXPECT_OK(dut_->SdioDisableFnIntr(4));
  EXPECT_EQ(sdmmc_.Read(0x04, 1, 0)[0], 0b1000'0001);

  EXPECT_OK(dut_->SdioDisableFnIntr(7));
  EXPECT_EQ(sdmmc_.Read(0x04, 1, 0)[0], 0b0000'0000);

  EXPECT_NOT_OK(dut_->SdioDisableFnIntr(7));
}

TEST_F(SdioControllerDeviceTest, ProcessCccr) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(0);
  });

  sdmmc_.Write(0x00, std::vector<uint8_t>{0x43}, 0);  // CCCR/SDIO revision.
  sdmmc_.Write(0x08, std::vector<uint8_t>{0xc2}, 0);  // Card capability.
  sdmmc_.Write(0x13, std::vector<uint8_t>{0xa9}, 0);  // Bus speed select.
  sdmmc_.Write(0x14, std::vector<uint8_t>{0x3f}, 0);  // UHS-I support.
  sdmmc_.Write(0x15, std::vector<uint8_t>{0xb7}, 0);  // Driver strength.

  EXPECT_OK(dut_->Probe());
  sdio_hw_info_t info = {};
  EXPECT_OK(dut_->SdioGetDevHwInfo(0, &info));
  EXPECT_EQ(info.dev_hw_info.caps,
            SDIO_CARD_MULTI_BLOCK | SDIO_CARD_LOW_SPEED | SDIO_CARD_FOUR_BIT_BUS |
                SDIO_CARD_HIGH_SPEED | SDIO_CARD_UHS_SDR50 | SDIO_CARD_UHS_SDR104 |
                SDIO_CARD_UHS_DDR50 | SDIO_CARD_TYPE_A | SDIO_CARD_TYPE_B | SDIO_CARD_TYPE_D);

  sdmmc_.Write(0x08, std::vector<uint8_t>{0x00}, 0);
  sdmmc_.Write(0x13, std::vector<uint8_t>{0x00}, 0);
  sdmmc_.Write(0x14, std::vector<uint8_t>{0x00}, 0);
  sdmmc_.Write(0x15, std::vector<uint8_t>{0x00}, 0);

  EXPECT_OK(dut_->Probe());
  EXPECT_OK(dut_->SdioGetDevHwInfo(0, &info));
  EXPECT_EQ(info.dev_hw_info.caps, 0);

  sdmmc_.Write(0x00, std::vector<uint8_t>{0x41}, 0);
  EXPECT_NOT_OK(dut_->Probe());

  sdmmc_.Write(0x00, std::vector<uint8_t>{0x33}, 0);
  EXPECT_NOT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, ProcessCis) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5);
  });

  sdmmc_.Write(0x0000'0509, std::vector<uint8_t>{0xa2, 0xc2, 0x00}, 0);  // CIS pointer.

  sdmmc_.Write(0x0000'c2a2,
               std::vector<uint8_t>{
                   0x20,        // Manufacturer ID tuple.
                   0x04,        // Manufacturer ID tuple size.
                   0x01, 0xc0,  // Manufacturer code.
                   0xce, 0xfa,  // Manufacturer information (part number/revision).
                   0x00,        // Null tuple.
                   0x22,        // Function extensions tuple.
                   0x2a,        // Function extensions tuple size.
                   0x01,        // Type of extended data.
               },
               0);
  sdmmc_.Write(0x0000'c2b7, std::vector<uint8_t>{0x00, 0x01}, 0);  // Function block size.
  sdmmc_.Write(0x0000'c2d5, std::vector<uint8_t>{0x00}, 0);        // End-of-chain tuple.

  EXPECT_OK(dut_->Probe());

  sdio_hw_info_t info = {};
  EXPECT_OK(dut_->SdioGetDevHwInfo(5, &info));

  EXPECT_EQ(info.dev_hw_info.num_funcs, 6);
  EXPECT_EQ(info.func_hw_info.max_blk_size, 256);
  EXPECT_EQ(info.func_hw_info.manufacturer_id, 0xc001);
  EXPECT_EQ(info.func_hw_info.product_id, 0xface);
}

TEST_F(SdioControllerDeviceTest, ProcessCisFunction0) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5);
  });

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = 1024,
      .max_transfer_size_non_dma = 1024,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  sdmmc_.Write(0x0000'0009, std::vector<uint8_t>{0xf5, 0x61, 0x01}, 0);  // CIS pointer.

  sdmmc_.Write(0x0001'61f5,
               std::vector<uint8_t>{
                   0x22,        // Function extensions tuple.
                   0x04,        // Function extensions tuple size.
                   0x00,        // Type of extended data.
                   0x00, 0x02,  // Function 0 block size.
                   0x32,        // Max transfer speed.
                   0x00,        // Null tuple.
                   0x20,        // Manufacturer ID tuple.
                   0x04,        // Manufacturer ID tuple size.
                   0xef, 0xbe,  // Manufacturer code.
                   0xfe, 0xca,  // Manufacturer information (part number/revision).
                   0xff,        // End-of-chain tuple.
               },
               0);

  EXPECT_OK(dut_->Probe());

  sdio_hw_info_t info = {};
  EXPECT_OK(dut_->SdioGetDevHwInfo(0, &info));

  EXPECT_EQ(info.dev_hw_info.num_funcs, 6);
  EXPECT_EQ(info.func_hw_info.max_blk_size, 512);
  EXPECT_EQ(info.func_hw_info.max_tran_speed, 25000);
  EXPECT_EQ(info.func_hw_info.manufacturer_id, 0xbeef);
  EXPECT_EQ(info.func_hw_info.product_id, 0xcafe);
}

TEST_F(SdioControllerDeviceTest, ProcessFbr) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(7);
  });

  sdmmc_.Write(0x100, std::vector<uint8_t>{0x83}, 0);
  sdmmc_.Write(0x500, std::vector<uint8_t>{0x00}, 0);
  sdmmc_.Write(0x600, std::vector<uint8_t>{0xcf}, 0);
  sdmmc_.Write(0x601, std::vector<uint8_t>{0xab}, 0);
  sdmmc_.Write(0x700, std::vector<uint8_t>{0x4e}, 0);

  EXPECT_OK(dut_->Probe());

  sdio_hw_info_t info = {};
  EXPECT_OK(dut_->SdioGetDevHwInfo(0, &info));
  EXPECT_EQ(info.dev_hw_info.num_funcs, 8);

  EXPECT_OK(dut_->SdioGetDevHwInfo(1, &info));
  EXPECT_EQ(info.func_hw_info.fn_intf_code, 0x03);

  EXPECT_OK(dut_->SdioGetDevHwInfo(5, &info));
  EXPECT_EQ(info.func_hw_info.fn_intf_code, 0x00);

  EXPECT_OK(dut_->SdioGetDevHwInfo(6, &info));
  EXPECT_EQ(info.func_hw_info.fn_intf_code, 0xab);

  EXPECT_OK(dut_->SdioGetDevHwInfo(7, &info));
  EXPECT_EQ(info.func_hw_info.fn_intf_code, 0x0e);
}

TEST_F(SdioControllerDeviceTest, ProbeFail) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5);
  });

  // Set the function 3 CIS pointer to zero. This should cause InitFunc and subsequently Probe
  // to fail.
  sdmmc_.Write(0x0309, std::vector<uint8_t>{0x00, 0x00, 0x00}, 0);

  EXPECT_NOT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, ProbeSdr104) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });

  sdmmc_.Write(0x0014, std::vector<uint8_t>{0x07}, 0);

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_VOLTAGE_330 | SDMMC_HOST_CAP_SDR104 | SDMMC_HOST_CAP_SDR50 |
              SDMMC_HOST_CAP_DDR50,
      .max_transfer_size = 0x1000,
      .max_transfer_size_non_dma = 0x1000,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());

  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
  EXPECT_EQ(sdmmc_.bus_width(), SDMMC_BUS_WIDTH_FOUR);
  EXPECT_EQ(sdmmc_.bus_freq(), 208'000'000);
  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_SDR104);
}

TEST_F(SdioControllerDeviceTest, ProbeSdr50LimitedByHost) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });

  sdmmc_.Write(0x0014, std::vector<uint8_t>{0x07}, 0);

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_VOLTAGE_330 | SDMMC_HOST_CAP_SDR50,
      .max_transfer_size = 0x1000,
      .max_transfer_size_non_dma = 0x1000,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());

  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
  EXPECT_EQ(sdmmc_.bus_width(), SDMMC_BUS_WIDTH_FOUR);
  EXPECT_EQ(sdmmc_.bus_freq(), 100'000'000);
  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_SDR50);
}

TEST_F(SdioControllerDeviceTest, ProbeSdr50LimitedByCard) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });

  sdmmc_.Write(0x0014, std::vector<uint8_t>{0x01}, 0);

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_VOLTAGE_330 | SDMMC_HOST_CAP_SDR104 | SDMMC_HOST_CAP_SDR50 |
              SDMMC_HOST_CAP_DDR50,
      .max_transfer_size = 0x1000,
      .max_transfer_size_non_dma = 0x1000,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());

  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
  EXPECT_EQ(sdmmc_.bus_width(), SDMMC_BUS_WIDTH_FOUR);
  EXPECT_EQ(sdmmc_.bus_freq(), 100'000'000);
  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_SDR50);
}

TEST_F(SdioControllerDeviceTest, ProbeFallBackToHs) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });

  sdmmc_.Write(0x0008, std::vector<uint8_t>{0x00}, 0);
  sdmmc_.Write(0x0014, std::vector<uint8_t>{0x07}, 0);

  sdmmc_.set_perform_tuning_status(ZX_ERR_IO);
  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_VOLTAGE_330 | SDMMC_HOST_CAP_SDR104 | SDMMC_HOST_CAP_SDR50 |
              SDMMC_HOST_CAP_DDR50,
      .max_transfer_size = 0x1000,
      .max_transfer_size_non_dma = 0x1000,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());

  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
  EXPECT_EQ(sdmmc_.bus_width(), SDMMC_BUS_WIDTH_FOUR);
  EXPECT_EQ(sdmmc_.bus_freq(), 50'000'000);
  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_HS);
}

TEST_F(SdioControllerDeviceTest, ProbeSetVoltage) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5);
  });

  EXPECT_OK(dut_->Probe());
  // Card does not report 1.8V support so we don't request a change from the host.
  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_MAX);

  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });

  EXPECT_OK(dut_->Probe());
  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
}

TEST_F(SdioControllerDeviceTest, ProbeRetriesRequests) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });
  uint32_t tries = 0;
  sdmmc_.set_command_callback(SDIO_IO_RW_DIRECT, [&](const sdmmc_req_t& req) -> zx_status_t {
    const bool write = req.arg & SDIO_IO_RW_DIRECT_RW_FLAG;
    const uint32_t fn_idx =
        (req.arg & SDIO_IO_RW_DIRECT_FN_IDX_MASK) >> SDIO_IO_RW_DIRECT_FN_IDX_LOC;
    const uint32_t addr =
        (req.arg & SDIO_IO_RW_DIRECT_REG_ADDR_MASK) >> SDIO_IO_RW_DIRECT_REG_ADDR_LOC;

    const bool read_fn0_fbr = !write && (fn_idx == 0) && (addr == SDIO_CIA_FBR_CIS_ADDR);
    return (read_fn0_fbr && tries++ < 7) ? ZX_ERR_IO : ZX_OK;
  });

  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, IoAbortSetsAbortFlag) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5);
  });

  EXPECT_OK(dut_->Probe());

  sdmmc_.set_command_callback(SDIO_IO_RW_DIRECT, [](const sdmmc_req_t& req) -> void {
    EXPECT_EQ(req.cmd_idx, SDIO_IO_RW_DIRECT);
    EXPECT_FALSE(req.cmd_flags & SDMMC_CMD_TYPE_ABORT);
    EXPECT_EQ(req.arg, 0xb024'68ab);
  });
  EXPECT_OK(dut_->SdioDoRwByte(true, 3, 0x1234, 0xab, nullptr));

  sdmmc_.set_command_callback(SDIO_IO_RW_DIRECT, [](const sdmmc_req_t& req) -> void {
    EXPECT_EQ(req.cmd_idx, SDIO_IO_RW_DIRECT);
    EXPECT_TRUE(req.cmd_flags & SDMMC_CMD_TYPE_ABORT);
    EXPECT_EQ(req.arg, 0x8000'0c03);
  });
  EXPECT_OK(dut_->SdioIoAbort(3));
}

TEST_F(SdioControllerDeviceTest, DifferentManufacturerProductIds) {
  auto stop_thread = fit::defer([&]() { dut_->StopSdioIrqThread(); });

  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  EXPECT_OK(dut_->Init());

  // Function 0-4 CIS pointers.
  sdmmc_.Write(0x0000'0009, std::vector<uint8_t>{0xf5, 0x61, 0x01}, 0);
  sdmmc_.Write(0x0000'0109, std::vector<uint8_t>{0xa0, 0x56, 0x00}, 0);
  sdmmc_.Write(0x0000'0209, std::vector<uint8_t>{0xe9, 0xc3, 0x00}, 0);
  sdmmc_.Write(0x0000'0309, std::vector<uint8_t>{0xb7, 0x6e, 0x01}, 0);
  sdmmc_.Write(0x0000'0409, std::vector<uint8_t>{0x86, 0xb7, 0x00}, 0);

  // clang-format off
  sdmmc_.Write(0x0001'61f5,
               std::vector<uint8_t>{
                   0x22, 0x04,
                   0x00, 0x01, 0x00, 32,
                   0x20,        // Manufacturer ID tuple.
                   0x04,        // Manufacturer ID tuple size.
                   0xef, 0xbe,  // Manufacturer code.
                   0xfe, 0xca,  // Manufacturer information (part number/revision).
                   0xff,        // End-of-chain tuple.
               },
               0);

  sdmmc_.Write(0x0000'56a0,
               std::vector<uint8_t>{
                   0x20, 0x04,            // Manufacturer ID tuple.
                   0x7b, 0x31,
                   0x8f, 0xa8,
                   0x22, 0x2a,            // Function extensions tuple.
                   0x01,
                   0, 0, 0, 0,0, 0, 0, 0, // Padding to max block size field.
                   0x01, 0x00,            // Max block size.
               },
               0);

  sdmmc_.Write(0x0000'c3e9,
               std::vector<uint8_t>{
                   0x20, 0x04,
                   0xbd, 0x6d,
                   0x0d, 0x24,
                   0x22, 0x2a,
                   0x01,
                   0, 0, 0, 0,0, 0, 0, 0,
                   0x01, 0x00,
               },
               0);

  sdmmc_.Write(0x0001'6eb7,
               std::vector<uint8_t>{
                   0x20, 0x04,
                   0xca, 0xb8,
                   0x52, 0x98,
                   0x22, 0x2a,
                   0x01,
                   0, 0, 0, 0,0, 0, 0, 0,
                   0x01, 0x00,
               },
               0);

  sdmmc_.Write(0x0000'b786,
               std::vector<uint8_t>{
                   0x20, 0x04,
                   0xee, 0xf5,
                   0xde, 0x30,
                   0x22, 0x2a,
                   0x01,
                   0, 0, 0, 0,0, 0, 0, 0,
                   0x01, 0x00,
               },
               0);
  // clang-format on

  EXPECT_OK(dut_->Probe());

  sdio_hw_info_t info = {};
  EXPECT_OK(dut_->SdioGetDevHwInfo(0, &info));

  EXPECT_EQ(info.dev_hw_info.num_funcs, 5);
  EXPECT_EQ(info.func_hw_info.manufacturer_id, 0xbeef);
  EXPECT_EQ(info.func_hw_info.product_id, 0xcafe);

  EXPECT_OK(dut_->AddDevice());

  constexpr zx_device_prop_t kExpectedProps[4][3] = {
      {
          {BIND_SDIO_VID, 0, 0x317b},
          {BIND_SDIO_PID, 0, 0xa88f},
          {BIND_SDIO_FUNCTION, 0, 1},
      },
      {
          {BIND_SDIO_VID, 0, 0x6dbd},
          {BIND_SDIO_PID, 0, 0x240d},
          {BIND_SDIO_FUNCTION, 0, 2},
      },
      {
          {BIND_SDIO_VID, 0, 0xb8ca},
          {BIND_SDIO_PID, 0, 0x9852},
          {BIND_SDIO_FUNCTION, 0, 3},
      },
      {
          {BIND_SDIO_VID, 0, 0xf5ee},
          {BIND_SDIO_PID, 0, 0x30de},
          {BIND_SDIO_FUNCTION, 0, 4},
      },
  };

  EXPECT_EQ(parent_->descendant_count(), std::size(kExpectedProps) + 1);

  auto controller = parent_->GetLatestChild();
  ASSERT_NE(controller, nullptr);
  auto iter = controller->children().begin();
  for (size_t i = 0; i < std::size(kExpectedProps); i++) {
    ASSERT_NE(iter, controller->children().end());
    cpp20::span child = (*iter)->GetProperties();
    ASSERT_EQ(child.size(), std::size(kExpectedProps[0]));
    for (size_t j = 0; j < std::size(kExpectedProps[0]); j++) {
      const zx_device_prop_t& prop = child[j];
      EXPECT_EQ(prop.id, kExpectedProps[i][j].id);
      EXPECT_EQ(prop.reserved, kExpectedProps[i][j].reserved);
      EXPECT_EQ(prop.value, kExpectedProps[i][j].value);
    }
    iter++;
  }

  dut_->DdkAsyncRemove();
  EXPECT_OK(mock_ddk::ReleaseFlaggedDevices(parent_.get()));
  [[maybe_unused]] auto ptr = dut_.release();
  stop_thread.cancel();
}

TEST_F(SdioControllerDeviceTest, RunDiagnostics) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_SDR104,  // Make the SDIO driver call PerformTuning.
      .max_transfer_size = 16,
      .max_transfer_size_non_dma = 16,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());
  EXPECT_OK(dut_->Probe());

  dut_->SdioRunDiagnostics();
}

TEST_F(SdioControllerDeviceTest, FunctionZeroInvalidBlockSize) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  sdmmc_.Write(0x2000, std::vector<uint8_t>{0x22, 0x04, 0x00, 0x00, 0x00}, 0);

  sdmmc_.Write(0x0009, std::vector<uint8_t>{0x00, 0x20, 0x00}, 0);

  EXPECT_NOT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, IOFunctionInvalidBlockSize) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  sdmmc_.Write(0x3000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
  sdmmc_.Write(0x300e, std::vector<uint8_t>{0x00, 0x00}, 0);

  sdmmc_.Write(0x0209, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);

  EXPECT_NOT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, FunctionZeroNoBlockSize) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  sdmmc_.Write(0x3000, std::vector<uint8_t>{0xff}, 0);

  sdmmc_.Write(0x0009, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);

  EXPECT_NOT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, IOFunctionNoBlockSize) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });

  sdmmc_.Write(0x3000, std::vector<uint8_t>{0xff}, 0);

  sdmmc_.Write(0x0209, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);

  EXPECT_NOT_OK(dut_->Probe());
}

TEST_F(SdioControllerDeviceTest, UpdateBlockSizeMultiBlock) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });
  sdmmc_.Write(SDIO_CIA_CCCR_CARD_CAPS_ADDR, std::vector<uint8_t>{SDIO_CIA_CCCR_CARD_CAP_SMB}, 0);

  sdmmc_.Write(0x3000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
  sdmmc_.Write(0x300e, std::vector<uint8_t>{0x00, 0x02}, 0);

  sdmmc_.Write(0x0209, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = 2048,
      .max_transfer_size_non_dma = 2048,
      .prefs = 0,
  });

  dut_->Init();

  sdmmc_.Write(0x210, std::vector<uint8_t>{0x00, 0x00}, 0);

  EXPECT_OK(dut_->Probe());
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0x00);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0x02);

  uint16_t block_size = 0;
  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 512);

  EXPECT_OK(dut_->SdioUpdateBlockSize(2, 128, false));
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0x80);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0x00);

  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 128);

  EXPECT_OK(dut_->SdioUpdateBlockSize(2, 0, true));
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0x00);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0x02);

  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 512);

  EXPECT_NOT_OK(dut_->SdioUpdateBlockSize(2, 0, false));
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0x00);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0x02);

  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 512);

  EXPECT_NOT_OK(dut_->SdioUpdateBlockSize(2, 1024, false));
}

TEST_F(SdioControllerDeviceTest, UpdateBlockSizeNoMultiBlock) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(4);
  });
  sdmmc_.Write(SDIO_CIA_CCCR_CARD_CAPS_ADDR, std::vector<uint8_t>{0}, 0);

  sdmmc_.Write(0x3000, std::vector<uint8_t>{0x22, 0x2a, 0x01}, 0);
  sdmmc_.Write(0x300e, std::vector<uint8_t>{0x00, 0x02}, 0);

  sdmmc_.Write(0x0209, std::vector<uint8_t>{0x00, 0x30, 0x00}, 0);

  sdmmc_.set_host_info({
      .caps = 0,
      .max_transfer_size = 2048,
      .max_transfer_size_non_dma = 2048,
      .prefs = 0,
  });

  dut_->Init();

  // Placeholder value that should not get written or returned.
  sdmmc_.Write(0x210, std::vector<uint8_t>{0xa5, 0xa5}, 0);

  EXPECT_OK(dut_->Probe());
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0xa5);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0xa5);

  uint16_t block_size = 0;
  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 512);

  EXPECT_OK(dut_->SdioUpdateBlockSize(2, 128, false));
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0xa5);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0xa5);

  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 128);

  EXPECT_OK(dut_->SdioUpdateBlockSize(2, 0, true));
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0xa5);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0xa5);

  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 512);

  EXPECT_NOT_OK(dut_->SdioUpdateBlockSize(2, 0, false));
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[0], 0xa5);
  EXPECT_EQ(sdmmc_.Read(0x210, 2)[1], 0xa5);

  EXPECT_OK(dut_->SdioGetBlockSize(2, &block_size));
  EXPECT_EQ(block_size, 512);

  EXPECT_NOT_OK(dut_->SdioUpdateBlockSize(2, 1024, false));
}

TEST_F(SdioScatterGatherTest, ScatterGatherByteMode) {
  Init(3, true);

  memcpy(mapper1_.start(), kTestData1, sizeof(kTestData1));
  memcpy(mapper2_.start(), kTestData2, sizeof(kTestData2));
  memcpy(mapper3_.start(), kTestData3, sizeof(kTestData3));

  sdmmc_buffer_region_t buffers[3];
  buffers[0] = MakeBufferRegion(1, 8, 2);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 1);
  buffers[2] = MakeBufferRegion(3, 0, 2);

  sdio_rw_txn_t txn = {
      .addr = 0x1000,
      .incr = true,
      .write = true,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  std::vector<uint8_t> actual = sdmmc_.Read(0x1000, 6, 3);
  EXPECT_BYTES_EQ(actual.data(), kTestData1 + 8, 2);
  EXPECT_BYTES_EQ(actual.data() + 2, kTestData2 + 4, 1);
  EXPECT_BYTES_EQ(actual.data() + 3, kTestData3 + 8, 2);
  EXPECT_EQ(actual[5], 0xff);

  ASSERT_EQ(sdmmc_.requests().size(), 2);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 4);
  EXPECT_EQ(req1.address, 0x1000);
  EXPECT_EQ(req1.op_code, 1);
  EXPECT_EQ(req1.block_mode, 0);
  EXPECT_EQ(req1.function_number, 3);
  EXPECT_EQ(req1.rw_flag, 1);

  const SdioCmd53 req2 = SdioCmd53::FromArg(sdmmc_.requests()[1].arg);
  EXPECT_EQ(req2.blocks_or_bytes, 1);
  EXPECT_EQ(req2.address, 0x1000 + 4);
  EXPECT_EQ(req2.op_code, 1);
  EXPECT_EQ(req2.block_mode, 0);
  EXPECT_EQ(req2.function_number, 3);
  EXPECT_EQ(req2.rw_flag, 1);
}

TEST_F(SdioScatterGatherTest, ScatterGatherBlockMode) {
  Init(3, true);

  sdmmc_buffer_region_t buffers[3];
  buffers[0] = MakeBufferRegion(1, 8, 7);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 3);
  buffers[2] = MakeBufferRegion(3, 10, 5);

  sdmmc_.Write(0x5000, cpp20::span(kTestData1, std::size(kTestData1)), 3);

  sdio_rw_txn_t txn = {
      .addr = 0x5000,
      .incr = false,
      .write = false,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper1_.start()) + 8, kTestData1, 7);
  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper2_.start()) + 4, kTestData1 + 7, 3);
  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper3_.start()) + 18, kTestData1 + 10, 2);

  ASSERT_EQ(sdmmc_.requests().size(), 2);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 3);
  EXPECT_EQ(req1.address, 0x5000);
  EXPECT_EQ(req1.op_code, 0);
  EXPECT_EQ(req1.block_mode, 1);
  EXPECT_EQ(req1.function_number, 3);
  EXPECT_EQ(req1.rw_flag, 0);

  const SdioCmd53 req2 = SdioCmd53::FromArg(sdmmc_.requests()[1].arg);
  EXPECT_EQ(req2.blocks_or_bytes, 3);
  EXPECT_EQ(req2.address, 0x5000);
  EXPECT_EQ(req2.op_code, 0);
  EXPECT_EQ(req2.block_mode, 0);
  EXPECT_EQ(req2.function_number, 3);
  EXPECT_EQ(req2.rw_flag, 0);
}

TEST_F(SdioScatterGatherTest, ScatterGatherBlockModeNoMultiBlock) {
  Init(5, false);

  memcpy(mapper1_.start(), kTestData1, sizeof(kTestData1));
  memcpy(mapper2_.start(), kTestData2, sizeof(kTestData2));
  memcpy(mapper3_.start(), kTestData3, sizeof(kTestData3));

  sdmmc_buffer_region_t buffers[3];
  buffers[0] = MakeBufferRegion(1, 8, 7);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 3);
  buffers[2] = MakeBufferRegion(3, 0, 5);

  sdio_rw_txn_t txn = {
      .addr = 0x1000,
      .incr = true,
      .write = true,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(5, &txn));

  std::vector<uint8_t> actual = sdmmc_.Read(0x1000, 16, 5);
  EXPECT_BYTES_EQ(actual.data(), kTestData1 + 8, 7);
  EXPECT_BYTES_EQ(actual.data() + 7, kTestData2 + 4, 3);
  EXPECT_BYTES_EQ(actual.data() + 10, kTestData3 + 8, 5);
  EXPECT_EQ(actual[15], 0xff);

  ASSERT_EQ(sdmmc_.requests().size(), 4);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 4);
  EXPECT_EQ(req1.address, 0x1000);
  EXPECT_EQ(req1.op_code, 1);
  EXPECT_EQ(req1.block_mode, 0);
  EXPECT_EQ(req1.function_number, 5);
  EXPECT_EQ(req1.rw_flag, 1);

  const SdioCmd53 req2 = SdioCmd53::FromArg(sdmmc_.requests()[1].arg);
  EXPECT_EQ(req2.blocks_or_bytes, 4);
  EXPECT_EQ(req2.address, 0x1000 + 4);
  EXPECT_EQ(req2.op_code, 1);
  EXPECT_EQ(req2.block_mode, 0);
  EXPECT_EQ(req2.function_number, 5);
  EXPECT_EQ(req2.rw_flag, 1);

  const SdioCmd53 req3 = SdioCmd53::FromArg(sdmmc_.requests()[2].arg);
  EXPECT_EQ(req3.blocks_or_bytes, 4);
  EXPECT_EQ(req3.address, 0x1000 + 8);
  EXPECT_EQ(req3.op_code, 1);
  EXPECT_EQ(req3.block_mode, 0);
  EXPECT_EQ(req3.function_number, 5);
  EXPECT_EQ(req3.rw_flag, 1);

  const SdioCmd53 req4 = SdioCmd53::FromArg(sdmmc_.requests()[3].arg);
  EXPECT_EQ(req4.blocks_or_bytes, 3);
  EXPECT_EQ(req4.address, 0x1000 + 12);
  EXPECT_EQ(req4.op_code, 1);
  EXPECT_EQ(req4.block_mode, 0);
  EXPECT_EQ(req4.function_number, 5);
  EXPECT_EQ(req4.rw_flag, 1);
}

TEST_F(SdioScatterGatherTest, ScatterGatherBlockModeMultipleFinalBuffers) {
  Init(1, true);

  sdmmc_.Write(0x3000, cpp20::span(kTestData1, std::size(kTestData1)), 1);

  sdmmc_buffer_region_t buffers[4];
  buffers[0] = MakeBufferRegion(1, 8, 7);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 3);
  buffers[2] = MakeBufferRegion(3, 0, 3);
  buffers[3] = MakeBufferRegion(1, 0, 2);

  sdio_rw_txn_t txn = {
      .addr = 0x3000,
      .incr = true,
      .write = false,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(1, &txn));

  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper1_.start()) + 8, kTestData1, 7);
  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper2_.start()) + 4, kTestData1 + 7, 3);
  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper3_.start()) + 8, kTestData1 + 10, 3);
  EXPECT_BYTES_EQ(static_cast<uint8_t*>(mapper1_.start()), kTestData1 + 13, 2);

  ASSERT_EQ(sdmmc_.requests().size(), 2);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 3);
  EXPECT_EQ(req1.address, 0x3000);
  EXPECT_EQ(req1.op_code, 1);
  EXPECT_EQ(req1.block_mode, 1);
  EXPECT_EQ(req1.function_number, 1);
  EXPECT_EQ(req1.rw_flag, 0);

  const SdioCmd53 req2 = SdioCmd53::FromArg(sdmmc_.requests()[1].arg);
  EXPECT_EQ(req2.blocks_or_bytes, 3);
  EXPECT_EQ(req2.address, 0x3000 + 12);
  EXPECT_EQ(req2.op_code, 1);
  EXPECT_EQ(req2.block_mode, 0);
  EXPECT_EQ(req2.function_number, 1);
  EXPECT_EQ(req2.rw_flag, 0);
}

TEST_F(SdioScatterGatherTest, ScatterGatherBlockModeLastAligned) {
  Init(3, true);

  memcpy(mapper1_.start(), kTestData1, sizeof(kTestData1));
  memcpy(mapper2_.start(), kTestData2, sizeof(kTestData2));
  memcpy(mapper3_.start(), kTestData3, sizeof(kTestData3));

  sdmmc_buffer_region_t buffers[3];
  buffers[0] = MakeBufferRegion(1, 8, 7);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 5);
  buffers[2] = MakeBufferRegion(3, 0, 3);

  sdio_rw_txn_t txn = {
      .addr = 0x1000,
      .incr = true,
      .write = true,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  std::vector<uint8_t> actual = sdmmc_.Read(0x1000, 16, 3);
  EXPECT_BYTES_EQ(actual.data(), kTestData1 + 8, 7);
  EXPECT_BYTES_EQ(actual.data() + 7, kTestData2 + 4, 5);
  EXPECT_BYTES_EQ(actual.data() + 12, kTestData3 + 8, 3);
  EXPECT_EQ(actual[15], 0xff);

  ASSERT_EQ(sdmmc_.requests().size(), 2);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 3);
  EXPECT_EQ(req1.address, 0x1000);
  EXPECT_EQ(req1.op_code, 1);
  EXPECT_EQ(req1.block_mode, 1);
  EXPECT_EQ(req1.function_number, 3);
  EXPECT_EQ(req1.rw_flag, 1);

  const SdioCmd53 req2 = SdioCmd53::FromArg(sdmmc_.requests()[1].arg);
  EXPECT_EQ(req2.blocks_or_bytes, 3);
  EXPECT_EQ(req2.address, 0x1000 + 12);
  EXPECT_EQ(req2.op_code, 1);
  EXPECT_EQ(req2.block_mode, 0);
  EXPECT_EQ(req2.function_number, 3);
  EXPECT_EQ(req2.rw_flag, 1);
}

TEST_F(SdioScatterGatherTest, ScatterGatherOnlyFullBlocks) {
  Init(3, true);

  memcpy(mapper1_.start(), kTestData1, sizeof(kTestData1));
  memcpy(mapper2_.start(), kTestData2, sizeof(kTestData2));
  memcpy(mapper3_.start(), kTestData3, sizeof(kTestData3));

  sdmmc_buffer_region_t buffers[3];
  buffers[0] = MakeBufferRegion(1, 8, 7);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 5);
  buffers[2] = MakeBufferRegion(3, 0, 4);

  sdio_rw_txn_t txn = {
      .addr = 0x1000,
      .incr = true,
      .write = true,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  std::vector<uint8_t> actual = sdmmc_.Read(0x1000, 17, 3);
  EXPECT_BYTES_EQ(actual.data(), kTestData1 + 8, 7);
  EXPECT_BYTES_EQ(actual.data() + 7, kTestData2 + 4, 5);
  EXPECT_BYTES_EQ(actual.data() + 12, kTestData3 + 8, 4);
  EXPECT_EQ(actual[16], 0xff);

  ASSERT_EQ(sdmmc_.requests().size(), 1);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 4);
  EXPECT_EQ(req1.address, 0x1000);
  EXPECT_EQ(req1.op_code, 1);
  EXPECT_EQ(req1.block_mode, 1);
  EXPECT_EQ(req1.function_number, 3);
  EXPECT_EQ(req1.rw_flag, 1);
}

TEST_F(SdioScatterGatherTest, ScatterGatherOverMaxTransferSize) {
  Init(3, true);

  memcpy(mapper1_.start(), kTestData1, sizeof(kTestData1));
  memcpy(mapper2_.start(), kTestData2, sizeof(kTestData2));
  memcpy(mapper3_.start(), kTestData3, sizeof(kTestData3));

  sdmmc_buffer_region_t buffers[3];
  buffers[0] = MakeBufferRegion(1, 8, 300 * 4);
  buffers[1] = MakeBufferRegion(vmo2_, 4, 800 * 4);
  buffers[2] = MakeBufferRegion(3, 0, 100);

  sdio_rw_txn_t txn = {
      .addr = 0x1000,
      .incr = true,
      .write = true,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  EXPECT_OK(dut_->SdioDoRwTxn(3, &txn));

  ASSERT_EQ(sdmmc_.requests().size(), 3);

  const SdioCmd53 req1 = SdioCmd53::FromArg(sdmmc_.requests()[0].arg);
  EXPECT_EQ(req1.blocks_or_bytes, 511);
  EXPECT_EQ(req1.address, 0x1000);
  EXPECT_EQ(req1.op_code, 1);
  EXPECT_EQ(req1.block_mode, 1);
  EXPECT_EQ(req1.function_number, 3);
  EXPECT_EQ(req1.rw_flag, 1);

  const SdioCmd53 req2 = SdioCmd53::FromArg(sdmmc_.requests()[1].arg);
  EXPECT_EQ(req2.blocks_or_bytes, 511);
  EXPECT_EQ(req2.address, 0x1000 + (511 * 4));
  EXPECT_EQ(req2.op_code, 1);
  EXPECT_EQ(req2.block_mode, 1);
  EXPECT_EQ(req2.function_number, 3);
  EXPECT_EQ(req2.rw_flag, 1);

  const SdioCmd53 req3 = SdioCmd53::FromArg(sdmmc_.requests()[2].arg);
  EXPECT_EQ(req3.blocks_or_bytes, 103);
  EXPECT_EQ(req3.address, 0x1000 + (511 * 4 * 2));
  EXPECT_EQ(req3.op_code, 1);
  EXPECT_EQ(req3.block_mode, 1);
  EXPECT_EQ(req3.function_number, 3);
  EXPECT_EQ(req3.rw_flag, 1);
}

TEST_F(SdioControllerDeviceTest, RequestCardReset) {
  sdmmc_.set_command_callback(SDIO_SEND_OP_COND, [](uint32_t out_response[4]) -> void {
    out_response[0] = OpCondFunctions(5) | SDIO_SEND_OP_COND_RESP_S18A;
  });

  sdmmc_.Write(0x0014, std::vector<uint8_t>{0x07}, 0);

  sdmmc_.set_host_info({
      .caps = SDMMC_HOST_CAP_VOLTAGE_330 | SDMMC_HOST_CAP_SDR104 | SDMMC_HOST_CAP_SDR50 |
              SDMMC_HOST_CAP_DDR50,
      .max_transfer_size = 0x1000,
      .max_transfer_size_non_dma = 0x1000,
      .prefs = 0,
  });
  EXPECT_OK(dut_->Init());

  EXPECT_OK(dut_->Probe());

  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
  EXPECT_EQ(sdmmc_.bus_width(), SDMMC_BUS_WIDTH_FOUR);
  EXPECT_EQ(sdmmc_.bus_freq(), 208'000'000);
  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_SDR104);

  zx_status_t status = ZX_ERR_IO;
  dut_->SdioRequestCardReset(
      [](void* ctx, zx_status_t reset_status) {
        *reinterpret_cast<zx_status_t*>(ctx) = reset_status;
      },
      &status);

  EXPECT_OK(status);
  EXPECT_EQ(sdmmc_.signal_voltage(), SDMMC_VOLTAGE_V180);
  EXPECT_EQ(sdmmc_.bus_width(), SDMMC_BUS_WIDTH_FOUR);
  EXPECT_EQ(sdmmc_.bus_freq(), 208'000'000);
  EXPECT_EQ(sdmmc_.timing(), SDMMC_TIMING_SDR104);
}

}  // namespace sdmmc
