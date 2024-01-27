// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blktest.h"

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.block/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/zx/fifo.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zircon/device/block.h>
#include <zircon/syscalls.h>

#include <climits>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/unique_fd.h>
#include <pretty/hexdump.h>
#include <zxtest/zxtest.h>

#include "src/lib/storage/block_client/cpp/client.h"
#include "src/lib/storage/block_client/cpp/remote_block_device.h"

namespace tests {

zx_status_t BRead(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device, void* buffer,
                  size_t buffer_size, size_t offset) {
  return block_client::SingleReadBytes(device, buffer, buffer_size, offset);
}

zx_status_t BWrite(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device, void* buffer,
                   size_t buffer_size, size_t offset) {
  return block_client::SingleWriteBytes(device, buffer, buffer_size, offset);
}

static void get_testdev(uint64_t* blk_size, uint64_t* blk_count,
                        fidl::ClientEnd<fuchsia_hardware_block::Block>* client) {
  const char* blkdev_path = getenv(BLKTEST_BLK_DEV);
  ASSERT_NOT_NULL(blkdev_path, "No test device specified");
  // Open the block device
  zx::result block = component::Connect<fuchsia_hardware_block::Block>(blkdev_path);
  ASSERT_OK(block.status_value());

  const fidl::WireResult result = fidl::WireCall(block.value())->GetInfo();
  ASSERT_OK(result.status());
  const fit::result response = result.value();
  ASSERT_TRUE(response.is_ok(), "%s", zx_status_get_string(response.error_value()));

  *blk_size = response.value()->info.block_size;
  *blk_count = response.value()->info.block_count;
  *client = std::move(block.value());
}

TEST(BlkdevTests, blkdev_test_simple) {
  uint64_t blk_size, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&blk_size, &blk_count, &client));
  int64_t buffer_size = blk_size * 2;

  fbl::AllocChecker checker;
  std::unique_ptr<uint8_t[]> buf(new (&checker) uint8_t[buffer_size]);
  ASSERT_TRUE(checker.check());
  std::unique_ptr<uint8_t[]> out(new (&checker) uint8_t[buffer_size]);
  ASSERT_TRUE(checker.check());

  memset(buf.get(), 'a', sizeof(buf));
  memset(out.get(), 0, sizeof(out));

  // Write three blocks.
  ASSERT_EQ(BWrite(client.borrow(), buf.get(), buffer_size, 0), ZX_OK);
  ASSERT_EQ(BWrite(client.borrow(), buf.get(), buffer_size / 2, buffer_size), ZX_OK);

  // Seek to the start of the device and read the contents
  ASSERT_EQ(BRead(client.borrow(), out.get(), buffer_size, 0), ZX_OK);
  ASSERT_EQ(memcmp(out.get(), buf.get(), buffer_size), 0);
  ASSERT_EQ(BRead(client.borrow(), out.get(), buffer_size / 2, buffer_size), ZX_OK);
  ASSERT_EQ(memcmp(out.get(), buf.get(), buffer_size / 2), 0);
}

TEST(BlkdevTests, blkdev_test_bad_requests) {
  uint64_t blk_size, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&blk_size, &blk_count, &client));

  fbl::AllocChecker checker;
  std::unique_ptr<uint8_t[]> buf(new (&checker) uint8_t[blk_size * 4]);
  ASSERT_TRUE(checker.check());
  memset(buf.get(), 'a', blk_size * 4);

  // Read / write non-multiples of the block size
  ASSERT_NE(BWrite(client.borrow(), buf.get(), blk_size - 1, 0), ZX_OK);
  ASSERT_NE(BWrite(client.borrow(), buf.get(), blk_size / 2, 0), ZX_OK);

  ASSERT_NE(BRead(client.borrow(), buf.get(), blk_size - 1, 0), ZX_OK);
  ASSERT_NE(BRead(client.borrow(), buf.get(), blk_size / 2, 0), ZX_OK);

  // Read / write from unaligned offset
  ASSERT_NE(BWrite(client.borrow(), buf.get(), blk_size, 1), ZX_OK);
  ASSERT_NE(BRead(client.borrow(), buf.get(), blk_size, 1), ZX_OK);

  // Read / write from beyond end of device
  off_t dev_size = blk_size * blk_count;
  ASSERT_NE(BWrite(client.borrow(), buf.get(), blk_size, dev_size), ZX_OK);
  ASSERT_NE(BRead(client.borrow(), buf.get(), blk_size, dev_size), ZX_OK);
}

TEST(BlkdevTests, blkdev_test_fifo_no_op) {
  // Get a FIFO connection to a blkdev and immediately close it
  uint64_t blk_size, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&blk_size, &blk_count, &client));

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_block::Session>();
  ASSERT_OK(endpoints);
  auto& [session, server] = endpoints.value();

  const fidl::Status result = fidl::WireCall(client)->OpenSession(std::move(server));
  ASSERT_OK(result.status());
}

static void fill_random(uint8_t* buf, uint64_t size) {
  for (size_t i = 0; i < size; i++) {
    buf[i] = static_cast<uint8_t>(rand());
  }
}

zx::result<std::unique_ptr<block_client::Client>> CreateSession(
    fidl::UnownedClientEnd<fuchsia_hardware_block::Block> block) {
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_block::Session>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  auto& [session, server] = endpoints.value();

  const fidl::Status result = fidl::WireCall(block)->OpenSession(std::move(server));
  if (!result.ok()) {
    return zx::error(result.status());
  }

  const fidl::WireResult fifo_result = fidl::WireCall(session)->GetFifo();
  if (!fifo_result.ok()) {
    return zx::error(fifo_result.status());
  }
  const fit::result fifo_response = fifo_result.value();
  if (fifo_response.is_error()) {
    return zx::error(fifo_response.error_value());
  }

  return zx::ok(
      std::make_unique<block_client::Client>(std::move(session), std::move(fifo_response->fifo)));
}

TEST(BlkdevTests, blkdev_test_fifo_basic) {
  uint64_t blk_size, blk_count;
  // Set up the initial handshake connection with the blkdev
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&blk_size, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create an arbitrary VMO, fill it with some stuff
  const uint64_t vmo_size = blk_size * 3;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK, "Failed to create VMO");
  std::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
  fill_random(buf.get(), vmo_size);

  ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

  // Send a handle to the vmo to the block device, get a vmoid which identifies it
  zx::result vmoid_result = block_client.RegisterVmo(vmo);
  ASSERT_OK(vmoid_result);
  vmoid_t vmoid = vmoid_result.value().TakeId();

  // Batch write the VMO to the blkdev
  // Split it into two requests, spread across the disk
  block_fifo_request_t requests[] = {
      {
          .opcode = BLOCKIO_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 1,
          .vmo_offset = 0,
          .dev_offset = 0,
      },
      {
          .opcode = BLOCKIO_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 2,
          .vmo_offset = 1,
          .dev_offset = 100,
      },
  };

  ASSERT_EQ(block_client.Transaction(requests, std::size(requests)), ZX_OK);

  // Empty the vmo, then read the info we just wrote to the disk
  std::unique_ptr<uint8_t[]> out(new uint8_t[vmo_size]());

  ASSERT_EQ(vmo.write(out.get(), 0, vmo_size), ZX_OK);
  requests[0].opcode = BLOCKIO_READ;
  requests[1].opcode = BLOCKIO_READ;
  ASSERT_EQ(block_client.Transaction(requests, std::size(requests)), ZX_OK);
  ASSERT_EQ(vmo.read(out.get(), 0, vmo_size), ZX_OK);
  ASSERT_EQ(memcmp(buf.get(), out.get(), blk_size * 3), 0, "Read data not equal to written data");

  // Close the current vmo
  requests[0].opcode = BLOCKIO_CLOSE_VMO;
  ASSERT_EQ(block_client.Transaction(requests, 1), ZX_OK);
}

// TODO(https://fxbug.dev/44600): enable.
//
// This test has been disabled since its introduction in 4ef35f3b8366d64cccb0fe6e240fb101238d7dfb.
TEST(BlkdevTests, DISABLED_blkdev_test_fifo_whole_disk) {
  uint64_t blk_size, blk_count;
  // Set up the initial handshake connection with the blkdev
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&blk_size, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create an arbitrary VMO, fill it with some stuff
  uint64_t vmo_size = blk_size * blk_count;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK, "Failed to create VMO");
  std::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
  fill_random(buf.get(), vmo_size);

  ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

  // Send a handle to the vmo to the block device, get a vmoid which identifies it
  zx::result vmoid_result = block_client.RegisterVmo(vmo);
  ASSERT_OK(vmoid_result);
  vmoid_t vmoid = vmoid_result.value().TakeId();

  // Batch write the VMO to the blkdev
  block_fifo_request_t request = {
      .opcode = BLOCKIO_WRITE,
      .group = group,
      .vmoid = vmoid,
      .length = static_cast<uint32_t>(blk_count),
      .vmo_offset = 0,
      .dev_offset = 0,
  };
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_OK);

  // Empty the vmo, then read the info we just wrote to the disk
  std::unique_ptr<uint8_t[]> out(new uint8_t[vmo_size]());

  ASSERT_EQ(vmo.write(out.get(), 0, vmo_size), ZX_OK);
  request.opcode = BLOCKIO_READ;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_OK);
  ASSERT_EQ(vmo.read(out.get(), 0, vmo_size), ZX_OK);
  ASSERT_EQ(memcmp(buf.get(), out.get(), blk_size * 3), 0, "Read data not equal to written data");

  // Close the current vmo
  request.opcode = BLOCKIO_CLOSE_VMO;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_OK);
}

struct TestVmoObject {
  uint64_t vmo_size = 0;
  zx::vmo vmo;
  fuchsia_hardware_block::wire::VmoId vmoid;
  std::unique_ptr<uint8_t[]> buf;
};

// Creates a VMO, fills it with data, and gives it to the block device.
void CreateVmoHelper(block_client::Client& block_client, TestVmoObject& obj, size_t kBlockSize) {
  obj.vmo_size = kBlockSize + (rand() % 5) * kBlockSize;
  ASSERT_EQ(zx::vmo::create(obj.vmo_size, 0, &obj.vmo), ZX_OK, "Failed to create vmo");
  obj.buf.reset(new uint8_t[obj.vmo_size]);
  fill_random(obj.buf.get(), obj.vmo_size);
  ASSERT_EQ(obj.vmo.write(obj.buf.get(), 0, obj.vmo_size), ZX_OK, "Failed to write to vmo");

  zx::result vmoid = block_client.RegisterVmo(obj.vmo);
  ASSERT_OK(vmoid);
  obj.vmoid.id = vmoid.value().TakeId();
}

// Write all vmos in a striped pattern on disk.
// For objs.size() == 10,
// i = 0 will write vmo block 0, 1, 2, 3... to dev block 0, 10, 20, 30...
// i = 1 will write vmo block 0, 1, 2, 3... to dev block 1, 11, 21, 31...
void WriteStripedVmoHelper(block_client::Client& block_client, const TestVmoObject& obj, size_t i,
                           size_t objs, groupid_t group, size_t kBlockSize) {
  // Make a separate request for each block
  size_t blocks = obj.vmo_size / kBlockSize;
  std::vector<block_fifo_request_t> requests(blocks);
  for (size_t b = 0; b < blocks; b++) {
    requests[b].group = group;
    requests[b].vmoid = obj.vmoid.id;
    requests[b].opcode = BLOCKIO_WRITE;
    requests[b].length = 1;
    requests[b].vmo_offset = b;
    requests[b].dev_offset = i + b * objs;
  }
  // Write entire vmos at once
  ASSERT_EQ(block_client.Transaction(requests.data(), requests.size()), ZX_OK);
}

// Verifies the result from "WriteStripedVmoHelper"
void ReadStripedVmoHelper(block_client::Client& block_client, const TestVmoObject& obj, size_t i,
                          size_t objs, groupid_t group, size_t kBlockSize) {
  // First, empty out the VMO
  std::unique_ptr<uint8_t[]> out(new uint8_t[obj.vmo_size]());
  ASSERT_EQ(obj.vmo.write(out.get(), 0, obj.vmo_size), ZX_OK);

  // Next, read to the vmo from the disk
  size_t blocks = obj.vmo_size / kBlockSize;
  std::vector<block_fifo_request_t> requests(blocks);
  for (size_t b = 0; b < blocks; b++) {
    requests[b].group = group;
    requests[b].vmoid = obj.vmoid.id;
    requests[b].opcode = BLOCKIO_READ;
    requests[b].length = 1;
    requests[b].vmo_offset = b;
    requests[b].dev_offset = i + b * objs;
  }

  // Read entire vmos at once
  ASSERT_EQ(block_client.Transaction(requests.data(), requests.size()), ZX_OK);

  // Finally, write from the vmo to an out buffer, where we can compare
  // the results with the input buffer.
  ASSERT_EQ(obj.vmo.read(out.get(), 0, obj.vmo_size), ZX_OK);
  ASSERT_EQ(memcmp(obj.buf.get(), out.get(), obj.vmo_size), 0,
            "Read data not equal to written data");
}

// Tears down an object created by "CreateVmoHelper".
void CloseVmoHelper(block_client::Client& block_client, TestVmoObject& obj, groupid_t group) {
  block_fifo_request_t request;
  request.group = group;
  request.vmoid = obj.vmoid.id;
  request.opcode = BLOCKIO_CLOSE_VMO;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_OK);
  obj.vmo.reset();
}

TEST(BlkdevTests, blkdev_test_fifo_multiple_vmo) {
  // Set up the initial handshake connection with the blkdev
  uint64_t blk_size, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&blk_size, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create multiple VMOs
  std::vector<TestVmoObject> objs(10);
  for (auto& obj : objs) {
    ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, obj, blk_size));
  }

  for (size_t i = 0; i < objs.size(); i++) {
    ASSERT_NO_FATAL_FAILURE(
        WriteStripedVmoHelper(block_client, objs[i], i, objs.size(), group, blk_size));
  }

  for (size_t i = 0; i < objs.size(); i++) {
    ASSERT_NO_FATAL_FAILURE(
        ReadStripedVmoHelper(block_client, objs[i], i, objs.size(), group, blk_size));
  }

  for (auto& obj : objs) {
    ASSERT_NO_FATAL_FAILURE(CloseVmoHelper(block_client, obj, group));
  }
}

TEST(BlkdevTests, blkdev_test_fifo_multiple_vmo_multithreaded) {
  // Set up the initial handshake connection with the blkdev
  uint64_t kBlockSize, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&kBlockSize, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  // Create multiple VMOs
  constexpr size_t kNumThreads = MAX_TXN_GROUP_COUNT;
  std::vector<TestVmoObject> objs(kNumThreads);

  std::vector<std::thread> threads;
  for (size_t i = 0; i < kNumThreads; i++) {
    // Capture i by value to get the updated version each loop iteration.
    threads.emplace_back([&, i]() {
      groupid_t group = static_cast<groupid_t>(i);
      ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, objs[i], kBlockSize));
      ASSERT_NO_FATAL_FAILURE(
          WriteStripedVmoHelper(block_client, objs[i], i, objs.size(), group, kBlockSize));
      ASSERT_NO_FATAL_FAILURE(
          ReadStripedVmoHelper(block_client, objs[i], i, objs.size(), group, kBlockSize));
      ASSERT_NO_FATAL_FAILURE(CloseVmoHelper(block_client, objs[i], group));
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }
}

// TODO(smklein): Test ops across different vmos
//
// TODO(https://fxbug.dev/44600): Re-enable.
TEST(BlkdevTests, DISABLED_blkdev_test_fifo_unclean_shutdown) {
  // Set up the blkdev
  uint64_t kBlockSize, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&kBlockSize, &blk_count, &client));

  std::vector<TestVmoObject> objs(10);
  groupid_t group = 0;
  {
    zx::result block_client_ptr = CreateSession(client);
    ASSERT_OK(block_client_ptr);
    block_client::Client& block_client = *block_client_ptr.value();

    // Create multiple VMOs
    for (auto& obj : objs) {
      ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, obj, kBlockSize));
    }
  }

  // Give the block server a moment to realize our side of the fifo has been closed
  usleep(10000);

  // The block server should still be functioning. We should be able to re-bind to it
  {
    zx::result block_client_ptr = CreateSession(client);
    ASSERT_OK(block_client_ptr);
    block_client::Client& block_client = *block_client_ptr.value();

    for (auto& obj : objs) {
      ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, obj, kBlockSize));
    }
    for (size_t i = 0; i < objs.size(); i++) {
      ASSERT_NO_FATAL_FAILURE(
          WriteStripedVmoHelper(block_client, objs[i], i, objs.size(), group, kBlockSize));
    }
    for (size_t i = 0; i < objs.size(); i++) {
      ASSERT_NO_FATAL_FAILURE(
          ReadStripedVmoHelper(block_client, objs[i], i, objs.size(), group, kBlockSize));
    }
    for (auto& obj : objs) {
      ASSERT_NO_FATAL_FAILURE(CloseVmoHelper(block_client, obj, group));
    }
  }
}

TEST(BlkdevTests, blkdev_test_fifo_bad_client_vmoid) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the blkdev
  uint64_t kBlockSize, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&kBlockSize, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create a vmo
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, obj, kBlockSize));

  // Bad request: Writing to the wrong vmoid
  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id + 5);
  request.opcode = BLOCKIO_WRITE;
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = 0;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_IO, "Expected IO error with bad vmoid");
}

TEST(BlkdevTests, blkdev_test_fifo_bad_client_unaligned_request) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the blkdev
  uint64_t kBlockSize, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&kBlockSize, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create a vmo of at least size "kBlockSize * 2", since we'll
  // be reading "kBlockSize" bytes from an offset below, and we want it
  // to fit within the bounds of the VMO.
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, obj, kBlockSize * 2));

  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
  request.opcode = BLOCKIO_WRITE;

  // Send a request that has zero length
  request.length = 0;
  request.vmo_offset = 0;
  request.dev_offset = 0;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_INVALID_ARGS, "");
}

TEST(BlkdevTests, blkdev_test_fifo_bad_client_overflow) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the blkdev
  uint64_t kBlockSize, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&kBlockSize, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create a vmo of at least size "kBlockSize * 2", since we'll
  // be reading "kBlockSize" bytes from an offset below, and we want it
  // to fit within the bounds of the VMO.
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(block_client, obj, kBlockSize * 2));

  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
  request.opcode = BLOCKIO_WRITE;

  // Send a request that is barely out-of-bounds for the device
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = blk_count;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that is half out-of-bounds for the device
  request.length = 2;
  request.vmo_offset = 0;
  request.dev_offset = blk_count - 1;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that is very out-of-bounds for the device
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = blk_count + 1;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that tries to overflow the VMO
  request.length = 2;
  request.vmo_offset = std::numeric_limits<uint64_t>::max();
  request.dev_offset = 0;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that tries to overflow the device
  request.length = 2;
  request.vmo_offset = 0;
  request.dev_offset = std::numeric_limits<uint64_t>::max();
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
}

TEST(BlkdevTests, blkdev_test_fifo_bad_client_bad_vmo) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the blkdev
  uint64_t kBlockSize, blk_count;
  fidl::ClientEnd<fuchsia_hardware_block::Block> client;
  ASSERT_NO_FATAL_FAILURE(get_testdev(&kBlockSize, &blk_count, &client));

  zx::result block_client_ptr = CreateSession(client);
  ASSERT_OK(block_client_ptr);
  block_client::Client& block_client = *block_client_ptr.value();

  groupid_t group = 0;

  // Create a vmo of one block.
  //
  // The underlying VMO may be rounded up to the nearest zx_system_get_page_size().
  TestVmoObject obj;
  obj.vmo_size = kBlockSize;
  ASSERT_EQ(zx::vmo::create(obj.vmo_size, 0, &obj.vmo), ZX_OK, "Failed to create vmo");
  obj.buf.reset(new uint8_t[obj.vmo_size]);
  fill_random(obj.buf.get(), obj.vmo_size);
  ASSERT_EQ(obj.vmo.write(obj.buf.get(), 0, obj.vmo_size), ZX_OK, "Failed to write to vmo");

  {
    zx::result vmoid = block_client.RegisterVmo(obj.vmo);
    ASSERT_OK(vmoid);
    obj.vmoid.id = vmoid.value().TakeId();
  }

  // Send a request to write to write multiple blocks -- enough that
  // the request is larger than the VMO.
  const uint64_t length =
      1 +
      (fbl::round_up(obj.vmo_size, static_cast<uint64_t>(zx_system_get_page_size())) / kBlockSize);
  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
  request.opcode = BLOCKIO_WRITE;
  request.length = static_cast<uint32_t>(length);
  request.vmo_offset = 0;
  request.dev_offset = 0;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
  // Do the same thing, but for reading
  request.opcode = BLOCKIO_READ;
  ASSERT_EQ(block_client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
}

}  // namespace tests
