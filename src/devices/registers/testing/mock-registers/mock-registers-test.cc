// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mock-registers.h"

#include <lib/async-loop/cpp/loop.h>

#include <zxtest/zxtest.h>

namespace mock_registers {

class MockRegistersTest : public zxtest::Test {
 public:
  void SetUp() override {
    loop_.StartThread();

    registers_ = std::make_unique<MockRegisters>(loop_.dispatcher());

    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_registers::Device>();
    ASSERT_OK(endpoints);
    auto& [client_end, server_end] = endpoints.value();
    registers_->Init(std::move(server_end));
    client_.Bind(std::move(client_end));
  }

  void TearDown() override {
    EXPECT_OK(registers_->VerifyAll());
    loop_.Shutdown();
  }

 protected:
  std::unique_ptr<MockRegisters> registers_;

  fidl::WireSyncClient<fuchsia_hardware_registers::Device> client_;
  async::Loop loop_{&kAsyncLoopConfigNeverAttachToThread};
};

TEST_F(MockRegistersTest, ReadTest) {
  // 8-bit
  registers_->ExpectRead<uint8_t>(0, 1, 2);
  auto read8_res = client_->ReadRegister8(0, 1);
  EXPECT_TRUE(read8_res.ok());
  EXPECT_TRUE(read8_res->is_ok());
  EXPECT_EQ(read8_res->value()->value, 2);

  // 16-bit
  registers_->ExpectRead<uint16_t>(5, 15, 3);
  auto read16_res = client_->ReadRegister16(5, 15);
  EXPECT_TRUE(read16_res.ok());
  EXPECT_TRUE(read16_res->is_ok());
  EXPECT_EQ(read16_res->value()->value, 3);

  // 32-bit
  registers_->ExpectRead<uint32_t>(145, 127, 25);
  auto read32_res = client_->ReadRegister32(145, 127);
  EXPECT_TRUE(read32_res.ok());
  EXPECT_TRUE(read32_res->is_ok());
  EXPECT_EQ(read32_res->value()->value, 25);

  // 64-bit
  registers_->ExpectRead<uint64_t>(325, 54, 136);
  auto read64_res = client_->ReadRegister64(325, 54);
  EXPECT_TRUE(read64_res.ok());
  EXPECT_TRUE(read64_res->is_ok());
  EXPECT_EQ(read64_res->value()->value, 136);

  // Multiple Reads
  registers_->ExpectRead<uint32_t>(25, 63, 46);
  registers_->ExpectRead<uint32_t>(25, 84, 53);
  registers_->ExpectRead<uint32_t>(102, 57, 7);
  registers_->ExpectRead<uint32_t>(3, 24, 299);
  registers_->ExpectRead<uint32_t>(102, 67, 38);
  auto res1 = client_->ReadRegister32(25, 63);
  EXPECT_TRUE(res1.ok());
  EXPECT_TRUE(res1->is_ok());
  EXPECT_EQ(res1->value()->value, 46);
  auto res2 = client_->ReadRegister32(25, 84);
  EXPECT_TRUE(res2.ok());
  EXPECT_TRUE(res2->is_ok());
  EXPECT_EQ(res2->value()->value, 53);
  auto res3 = client_->ReadRegister32(102, 57);
  EXPECT_TRUE(res3.ok());
  EXPECT_TRUE(res3->is_ok());
  EXPECT_EQ(res3->value()->value, 7);
  auto res4 = client_->ReadRegister32(3, 24);
  EXPECT_TRUE(res4.ok());
  EXPECT_TRUE(res4->is_ok());
  EXPECT_EQ(res4->value()->value, 299);
  auto res5 = client_->ReadRegister32(102, 67);
  EXPECT_TRUE(res5.ok());
  EXPECT_TRUE(res5->is_ok());
  EXPECT_EQ(res5->value()->value, 38);
}

TEST_F(MockRegistersTest, WriteTest) {
  // 8-bit
  registers_->ExpectWrite<uint8_t>(0, 1, 2);
  auto write8_res = client_->WriteRegister8(0, 1, 2);
  EXPECT_TRUE(write8_res.ok());
  EXPECT_TRUE(write8_res->is_ok());

  // 16-bit
  registers_->ExpectWrite<uint16_t>(5, 15, 3);
  auto write16_res = client_->WriteRegister16(5, 15, 3);
  EXPECT_TRUE(write16_res.ok());
  EXPECT_TRUE(write16_res->is_ok());

  // 32-bit
  registers_->ExpectWrite<uint32_t>(145, 127, 25);
  auto write32_res = client_->WriteRegister32(145, 127, 25);
  EXPECT_TRUE(write32_res.ok());
  EXPECT_TRUE(write32_res->is_ok());

  // 64-bit
  registers_->ExpectWrite<uint64_t>(325, 54, 136);
  auto write64_res = client_->WriteRegister64(325, 54, 136);
  EXPECT_TRUE(write64_res.ok());
  EXPECT_TRUE(write64_res->is_ok());

  // Multiple Writes
  registers_->ExpectWrite<uint32_t>(25, 63, 46);
  registers_->ExpectWrite<uint32_t>(25, 84, 53);
  registers_->ExpectWrite<uint32_t>(102, 57, 7);
  registers_->ExpectWrite<uint32_t>(3, 24, 299);
  registers_->ExpectWrite<uint32_t>(102, 67, 38);
  auto res1 = client_->WriteRegister32(25, 63, 46);
  EXPECT_TRUE(res1.ok());
  EXPECT_TRUE(res1->is_ok());
  auto res2 = client_->WriteRegister32(25, 84, 53);
  EXPECT_TRUE(res2.ok());
  EXPECT_TRUE(res2->is_ok());
  auto res3 = client_->WriteRegister32(102, 57, 7);
  EXPECT_TRUE(res3.ok());
  EXPECT_TRUE(res3->is_ok());
  auto res4 = client_->WriteRegister32(3, 24, 299);
  EXPECT_TRUE(res4.ok());
  EXPECT_TRUE(res4->is_ok());
  auto res5 = client_->WriteRegister32(102, 67, 38);
  EXPECT_TRUE(res5.ok());
  EXPECT_TRUE(res5->is_ok());
}

}  // namespace mock_registers
