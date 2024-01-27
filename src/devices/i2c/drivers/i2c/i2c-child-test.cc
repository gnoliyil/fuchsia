// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "i2c-child.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/loop.h>

#include <zxtest/zxtest.h>

#include "i2c-bus.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace i2c {

constexpr uint8_t kTestWrite0 = 0x99;
constexpr uint8_t kTestWrite1 = 0x88;
constexpr uint8_t kTestWrite2 = 0x77;
constexpr uint8_t kTestRead0 = 0x12;
constexpr uint8_t kTestRead1 = 0x34;
constexpr uint8_t kTestRead2 = 0x56;

class I2cTestChild : public I2cChild {
 public:
  I2cTestChild(zx_device_t* parent, fbl::RefPtr<I2cBus> bus, uint16_t address,
               async_dispatcher_t* dispatcher, std::string name)
      : I2cChild(parent, std::move(bus), address, dispatcher, name) {
    ZX_ASSERT(DdkAdd("Test-device") == ZX_OK);
  }
};

class I2cBusTest : public I2cBus {
 public:
  explicit I2cBusTest(
      zx_device_t* fake_root, ddk::I2cImplProtocolClient i2c, uint32_t bus_id,
      std::function<void(uint16_t, const I2cBus::TransactOp*, size_t, TransactCallback, void*)>
          transact)
      : I2cBus(fake_root, i2c, bus_id), transact_(std::move(transact)) {}

  void Transact(uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
                TransactCallback callback, void* cookie) override {
    transact_(address, op_list, op_count, callback, cookie);
  }

 private:
  std::function<void(uint16_t, const I2cBus::TransactOp*, size_t, TransactCallback, void*)>
      transact_;
};

class I2cChildTest : public zxtest::Test {
 public:
  I2cChildTest() : loop_(&kAsyncLoopConfigNeverAttachToThread) {}
  void SetUp() override {
    fake_root_ = MockDevice::FakeRootParent();
    ASSERT_OK(loop_.StartThread("i2c-child-test-fidl"));
  }

  void StartFidl(I2cTestChild* child, fidl::WireSyncClient<fuchsia_hardware_i2c::Device>* device) {
    auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_i2c::Device>();
    ASSERT_OK(endpoints.status_value());

    fidl::BindServer(loop_.dispatcher(), std::move(endpoints->server), child);

    *device = fidl::WireSyncClient<fuchsia_hardware_i2c::Device>(std::move(endpoints->client));
  }

  void TearDown() override {
    for (auto& device : fake_root_->children()) {
      device_async_remove(device.get());
    }
    mock_ddk::ReleaseFlaggedDevices(fake_root_.get());
  }

 protected:
  std::shared_ptr<zx_device> fake_root_;
  async::Loop loop_;
};

TEST_F(I2cChildTest, Write3BytesOnce) {
  ddk::I2cImplProtocolClient i2c = {};
  auto bus = fbl::AdoptRef(new I2cBusTest(
      fake_root_.get(), i2c, 0,
      [](uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
         I2cBus::TransactCallback callback, void* cookie) {
        if (op_count != 1) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        auto p0 = op_list[0].data_buffer;
        if (p0[0] != kTestWrite0 || p0[1] != kTestWrite1 || p0[2] != kTestWrite2 ||
            op_list[0].data_size != 3 || op_list[0].is_read != false || op_list[0].stop != true) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        callback(cookie, ZX_OK, nullptr, 0);
      }));
  // Allocate using new as the mock DDK takes ownership of the child.
  auto server = new I2cTestChild(fake_root_.get(), std::move(bus), 0, loop_.dispatcher(), "");
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  // 3 bytes in 1 write transaction.
  size_t n_write_bytes = 3;
  auto write_buffer = std::make_unique<uint8_t[]>(n_write_bytes);
  write_buffer[0] = kTestWrite0;
  write_buffer[1] = kTestWrite1;
  write_buffer[2] = kTestWrite2;
  auto write_data = fidl::VectorView<uint8_t>::FromExternal(write_buffer.get(), n_write_bytes);

  fidl::Arena arena;
  auto write_transfer = fidl_i2c::wire::DataTransfer::WithWriteData(arena, write_data);

  auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 1);
  transactions[0] =
      fidl_i2c::wire::Transaction::Builder(arena).data_transfer(write_transfer).Build();

  auto read = client_wrap->Transfer(transactions);

  ASSERT_OK(read.status());
  ASSERT_FALSE(read->is_error());
}

TEST_F(I2cChildTest, Read3BytesOnce) {
  ddk::I2cImplProtocolClient i2c = {};
  auto bus = fbl::AdoptRef(new I2cBusTest(
      fake_root_.get(), i2c, 0,
      [](uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
         I2cBus::TransactCallback callback, void* cookie) {
        if (op_count != 1) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        if (op_list[0].data_size != 3 || op_list[0].is_read != true || op_list[0].stop != true) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        uint8_t reply0 = kTestRead0;
        uint8_t reply1 = kTestRead1;
        uint8_t reply2 = kTestRead2;
        I2cBus::TransactOp replies[3] = {
            {&reply0, 1, true, false},
            {&reply1, 1, true, false},
            {&reply2, 1, true, false},
        };
        callback(cookie, ZX_OK, replies, std::size(replies));
      }));
  // Allocate using new as the mock DDK takes ownership of the child.
  auto server = new I2cTestChild(fake_root_.get(), std::move(bus), 0, loop_.dispatcher(), "");
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  // 1 read transaction expecting 3 bytes.
  constexpr size_t n_bytes = 3;

  fidl::Arena arena;
  auto read_transfer = fidl_i2c::wire::DataTransfer::WithReadSize(n_bytes);

  auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 1);
  transactions[0] =
      fidl_i2c::wire::Transaction::Builder(arena).data_transfer(read_transfer).Build();

  auto read = client_wrap->Transfer(transactions);
  ASSERT_OK(read.status());
  ASSERT_FALSE(read->is_error());

  auto& read_data = read->value()->read_data;
  ASSERT_EQ(read_data[0].data()[0], kTestRead0);
  ASSERT_EQ(read_data[1].data()[0], kTestRead1);
  ASSERT_EQ(read_data[2].data()[0], kTestRead2);
}

TEST_F(I2cChildTest, Write1ByteOnceRead1Byte3Times) {
  ddk::I2cImplProtocolClient i2c = {};
  auto bus = fbl::AdoptRef(new I2cBusTest(
      fake_root_.get(), i2c, 0,
      [](uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
         I2cBus::TransactCallback callback, void* cookie) {
        if (op_count != 4) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        auto p0 = op_list[0].data_buffer;
        if (p0[0] != kTestWrite0 || op_list[0].data_size != 1 || op_list[0].is_read != false ||
            op_list[0].stop != false || op_list[1].data_size != 1 || op_list[1].is_read != true ||
            op_list[1].stop != false || op_list[2].data_size != 1 || op_list[2].is_read != true ||
            op_list[2].stop != false || op_list[3].data_size != 1 || op_list[3].is_read != true ||
            op_list[3].stop != true) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        uint8_t reply0 = kTestRead0;
        uint8_t reply1 = kTestRead1;
        uint8_t reply2 = kTestRead2;
        I2cBus::TransactOp replies[3] = {
            {&reply0, 1, true, false},
            {&reply1, 1, true, false},
            {&reply2, 1, true, false},
        };
        callback(cookie, ZX_OK, replies, std::size(replies));
      }));

  // Allocate using new as the mock DDK takes ownership of the child.
  auto server = new I2cTestChild(fake_root_.get(), std::move(bus), 0, loop_.dispatcher(), "");
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  // 1 byte in 1 write transaction.
  size_t n_write_bytes = 1;
  auto write_buffer = std::make_unique<uint8_t[]>(n_write_bytes);
  write_buffer[0] = kTestWrite0;
  auto write_data = fidl::VectorView<uint8_t>::FromExternal(write_buffer.get(), n_write_bytes);

  fidl::Arena arena;
  auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 4);

  transactions[0] =
      fidl_i2c::wire::Transaction::Builder(arena)
          .data_transfer(fidl_i2c::wire::DataTransfer::WithWriteData(arena, write_data))
          .Build();

  // 3 read transaction expecting 1 byte each.
  transactions[1] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .Build();

  transactions[2] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .Build();

  transactions[3] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .Build();

  auto read = client_wrap->Transfer(transactions);
  ASSERT_OK(read.status());
  ASSERT_FALSE(read->is_error());

  auto& read_data = read->value()->read_data;
  ASSERT_EQ(read_data[0].data()[0], kTestRead0);
  ASSERT_EQ(read_data[1].data()[0], kTestRead1);
  ASSERT_EQ(read_data[2].data()[0], kTestRead2);
}

TEST_F(I2cChildTest, StopFlagPropagates) {
  ddk::I2cImplProtocolClient i2c = {};
  auto bus = fbl::AdoptRef(new I2cBusTest(
      fake_root_.get(), i2c, 0,
      [](uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
         I2cBus::TransactCallback callback, void* cookie) {
        if (op_count != 4) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        // Verify that the I2C child driver set the stop flags correctly based on the transaction
        // list passed in below.
        if (!op_list[0].stop || op_list[1].stop || op_list[2].stop || !op_list[3].stop) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        uint8_t reply0 = kTestRead0;
        uint8_t reply1 = kTestRead1;
        uint8_t reply2 = kTestRead2;
        I2cBus::TransactOp replies[4] = {
            {&reply0, 1, true, false},
            {&reply1, 1, true, false},
            {&reply2, 1, true, false},
            {&reply0, 1, true, false},
        };
        callback(cookie, ZX_OK, replies, std::size(replies));
      }));

  auto server = new I2cTestChild(fake_root_.get(), std::move(bus), 0, loop_.dispatcher(), "");
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  fidl::Arena arena;
  auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 4);

  // Specified and set to true: the stop flag should be set to true.
  transactions[0] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .stop(true)
                        .Build();

  // Specified and set to false: the stop flag should be set to false.
  transactions[1] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .stop(false)
                        .Build();

  // Unspecified: the stop flag should be set to false.
  transactions[2] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .Build();

  // Final transaction: the stop flag should be set to true.
  transactions[3] = fidl_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                        .stop(false)
                        .Build();

  auto read = client_wrap->Transfer(transactions);
  ASSERT_OK(read.status());
  ASSERT_FALSE(read.value().is_error());
}

TEST_F(I2cChildTest, BadTransfers) {
  ddk::I2cImplProtocolClient i2c = {};
  // Allocate using new as the mock DDK takes ownership of the child.
  auto server = new I2cTestChild(fake_root_.get(),
                                 fbl::AdoptRef<I2cBus>(new I2cBus(fake_root_.get(), i2c, 0)), 0,
                                 loop_.dispatcher(), "");
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  {
    // There must be at least one Transaction.
    fidl::Arena arena;
    auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 0);

    auto read = client_wrap->Transfer(transactions);
    ASSERT_OK(read.status());
    ASSERT_TRUE(read->is_error());
  }

  {
    // Each Transaction must have data_transfer set.
    fidl::Arena arena;
    auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 2);

    transactions[0] = fidl_i2c::wire::Transaction::Builder(arena)
                          .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                          .Build();

    transactions[1] = fidl_i2c::wire::Transaction::Builder(arena).stop(true).Build();

    auto read = client_wrap->Transfer(transactions);
    ASSERT_OK(read.status());
    ASSERT_TRUE(read->is_error());
  }

  {
    // Read transfers must be at least one byte.
    fidl::Arena arena;
    auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 2);

    transactions[0] = fidl_i2c::wire::Transaction::Builder(arena)
                          .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(1))
                          .Build();

    transactions[1] = fidl_i2c::wire::Transaction::Builder(arena)
                          .data_transfer(fidl_i2c::wire::DataTransfer::WithReadSize(0))
                          .Build();

    auto read = client_wrap->Transfer(transactions);
    ASSERT_OK(read.status());
    ASSERT_TRUE(read->is_error());
  }

  {
    // Each Transaction must have data_transfer set.
    fidl::Arena arena;
    auto transactions = fidl::VectorView<fidl_i2c::wire::Transaction>(arena, 2);

    auto write0 = fidl::VectorView<uint8_t>(arena, 1);
    write0[0] = 0xff;

    auto write1 = fidl::VectorView<uint8_t>(arena, 0);

    transactions[0] = fidl_i2c::wire::Transaction::Builder(arena)
                          .data_transfer(fidl_i2c::wire::DataTransfer::WithWriteData(arena, write0))
                          .Build();

    transactions[1] = fidl_i2c::wire::Transaction::Builder(arena)
                          .data_transfer(fidl_i2c::wire::DataTransfer::WithWriteData(arena, write1))
                          .Build();

    auto read = client_wrap->Transfer(transactions);
    ASSERT_OK(read.status());
    ASSERT_TRUE(read->is_error());
  }
}

TEST_F(I2cChildTest, GetNameTest) {
  const std::string kTestName = "foo";
  ddk::I2cImplProtocolClient i2c = {};
  auto bus = fbl::AdoptRef(new I2cBusTest(
      fake_root_.get(), i2c, 0,
      [](uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
         I2cBus::TransactCallback callback, void* cookie) {
        if (op_count != 1) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        if (op_list[0].data_size != 3 || op_list[0].is_read != true || op_list[0].stop != true) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        uint8_t reply0 = kTestRead0;
        uint8_t reply1 = kTestRead1;
        uint8_t reply2 = kTestRead2;
        I2cBus::TransactOp replies[3] = {
            {&reply0, 1, true, false},
            {&reply1, 1, true, false},
            {&reply2, 1, true, false},
        };
        callback(cookie, ZX_OK, replies, std::size(replies));
      }));
  // Allocate using new as the mock DDK takes ownership of the child.
  auto server =
      new I2cTestChild(fake_root_.get(), std::move(bus), 0, loop_.dispatcher(), kTestName);
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  auto name = client_wrap->GetName();
  ASSERT_OK(name.status());
  ASSERT_FALSE(name->is_error());

  ASSERT_EQ(std::string(name->value()->name.get()), kTestName);
}

TEST_F(I2cChildTest, GetEmptyNameTest) {
  ddk::I2cImplProtocolClient i2c = {};
  auto bus = fbl::AdoptRef(new I2cBusTest(
      fake_root_.get(), i2c, 0,
      [](uint16_t address, const I2cBus::TransactOp* op_list, size_t op_count,
         I2cBus::TransactCallback callback, void* cookie) {
        if (op_count != 1) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        if (op_list[0].data_size != 3 || op_list[0].is_read != true || op_list[0].stop != true) {
          callback(cookie, ZX_ERR_INTERNAL, nullptr, 0);
          return;
        }
        uint8_t reply0 = kTestRead0;
        uint8_t reply1 = kTestRead1;
        uint8_t reply2 = kTestRead2;
        I2cBus::TransactOp replies[3] = {
            {&reply0, 1, true, false},
            {&reply1, 1, true, false},
            {&reply2, 1, true, false},
        };
        callback(cookie, ZX_OK, replies, std::size(replies));
      }));
  // Allocate using new as the mock DDK takes ownership of the child.
  auto server = new I2cTestChild(fake_root_.get(), std::move(bus), 0, loop_.dispatcher(), "");
  fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client_wrap;
  ASSERT_NO_FATAL_FAILURE(StartFidl(server, &client_wrap));

  auto name = client_wrap->GetName();
  ASSERT_OK(name.status());

  // Empty string here means this endpoint returns an error.
  ASSERT_TRUE(name->is_error());
}

}  // namespace i2c
