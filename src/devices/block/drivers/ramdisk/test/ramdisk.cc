// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.block.partition/cpp/wire.h>
#include <fidl/fuchsia.hardware.block/cpp/wire.h>
#include <fidl/fuchsia.hardware.ramdisk/cpp/wire.h>
#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/fzl/fifo.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/sync/completion.h>
#include <lib/zx/channel.h>
#include <lib/zx/fifo.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zircon/boot/image.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <climits>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <gtest/gtest.h>
#include <ramdevice-client/ramdisk.h>

#include "src/devices/lib/block/block.h"
#include "src/lib/storage/block_client/cpp/client.h"
#include "src/lib/storage/block_client/cpp/remote_block_device.h"

namespace ramdisk {
namespace {

zx_status_t BRead(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device, void* buffer,
                  size_t buffer_size, size_t offset) {
  return block_client::SingleReadBytes(device, buffer, buffer_size, offset);
}

zx_status_t BWrite(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device, void* buffer,
                   size_t buffer_size, size_t offset) {
  return block_client::SingleWriteBytes(device, buffer, buffer_size, offset);
}

constexpr uint8_t kGuid[ZBI_PARTITION_GUID_LEN] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                                                   0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};

static_assert(sizeof(fuchsia_hardware_block_partition::wire::Guid) == sizeof kGuid,
              "Mismatched GUID size");
// Make sure isolated_devmgr is ready to go before all tests.
class Environment : public testing::Environment {
 public:
  void SetUp() override {
    ASSERT_EQ(
        device_watcher::RecursiveWaitForFile("/dev/sys/platform/00:00:2d/ramctl").status_value(),
        ZX_OK);
  }
};

testing::Environment* const environment = testing::AddGlobalTestEnvironment(new Environment);

void fill_random(uint8_t* buf, uint64_t size) {
  static unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
  // TODO(fxbug.dev/28197): Make this easier to reproduce with reliably generated prng.
  printf("fill_random of %zu bytes with seed: %u\n", size, seed);
  for (size_t i = 0; i < size; i++) {
    buf[i] = static_cast<uint8_t>(rand_r(&seed));
  }
}

ramdisk_client_t* GetRamdisk(uint64_t blk_size, uint64_t blk_count, const uint8_t* guid = nullptr,
                             size_t guid_len = 0) {
  ramdisk_client_t* ramdisk = nullptr;
  zx_status_t rc = guid ? ramdisk_create_with_guid(blk_size, blk_count, guid, guid_len, &ramdisk)
                        : ramdisk_create(blk_size, blk_count, &ramdisk);
  if (rc != ZX_OK) {
    return nullptr;
  }

  return ramdisk;
}

// Small wrapper around the ramdisk which can be used to ensure the device
// is removed, even if the test fails.
class RamdiskTest {
 public:
  static void Create(uint64_t blk_size, uint64_t blk_count, std::unique_ptr<RamdiskTest>* out) {
    ramdisk_client_t* ramdisk = GetRamdisk(blk_size, blk_count);
    ASSERT_NE(ramdisk, nullptr);
    *out = std::unique_ptr<RamdiskTest>(new RamdiskTest(ramdisk));
  }

  static void CreateWithGuid(uint64_t blk_size, uint64_t blk_count, const uint8_t* guid,
                             size_t guid_len, std::unique_ptr<RamdiskTest>* out) {
    ramdisk_client_t* ramdisk = GetRamdisk(blk_size, blk_count, guid, guid_len);
    ASSERT_NE(ramdisk, nullptr);
    *out = std::unique_ptr<RamdiskTest>(new RamdiskTest(ramdisk));
  }

  void Terminate() {
    if (ramdisk_) {
      ASSERT_EQ(ramdisk_destroy(ramdisk_), ZX_OK);
      ramdisk_ = nullptr;
    }
  }

  zx_status_t Rebind() { return ramdisk_rebind(ramdisk_); }

  ~RamdiskTest() { Terminate(); }

  fidl::UnownedClientEnd<fuchsia_hardware_block::Block> block_interface() const {
    return fidl::UnownedClientEnd<fuchsia_hardware_block::Block>(
        ramdisk_get_block_interface(ramdisk_));
  }

  const ramdisk_client_t* ramdisk_client() const { return ramdisk_; }

 private:
  explicit RamdiskTest(ramdisk_client_t* ramdisk) : ramdisk_(ramdisk) {}

  ramdisk_client_t* ramdisk_;
};

TEST(RamdiskTests, RamdiskTestSimple) {
  std::vector<uint8_t> buf(zx_system_get_page_size());
  std::vector<uint8_t> out(zx_system_get_page_size());

  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size() / 2, 512, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  memset(buf.data(), 'a', buf.size());
  memset(out.data(), 0, out.size());

  // Write a page and a half
  ASSERT_EQ(BWrite(block_interface, buf.data(), buf.size(), 0), ZX_OK);
  ASSERT_EQ(BWrite(block_interface, buf.data(), buf.size() / 2, buf.size()), ZX_OK);

  // Seek to the start of the device and read the contents
  ASSERT_EQ(BRead(block_interface, out.data(), out.size(), 0), ZX_OK);
  ASSERT_EQ(memcmp(out.data(), buf.data(), out.size()), 0);
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

TEST(RamdiskTests, RamdiskStatsTest) {
  constexpr size_t kBlockSize = 512;
  constexpr size_t kBlockCount = 512;
  // Set up the initial handshake connection with the ramdisk
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create an arbitrary VMO, fill it with some stuff
  uint64_t vmo_size = zx_system_get_page_size() * 3;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK) << "Failed to create VMO";
  std::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
  fill_random(buf.get(), vmo_size);

  ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

  // Send a handle to the vmo to the block device, get a vmoid which identifies it
  zx::result vmoid_result = client.RegisterVmo(vmo);
  ASSERT_TRUE(vmoid_result.is_ok()) << vmoid_result.status_string();
  vmoid_t vmoid = vmoid_result.value().TakeId();

  const fidl::WireResult clear_stats_result = fidl::WireCall(block_interface)->GetStats(true);
  ASSERT_TRUE(clear_stats_result.ok()) << clear_stats_result.FormatDescription();
  const fit::result clear_stats_response = clear_stats_result.value();
  ASSERT_TRUE(clear_stats_response.is_ok())
      << zx_status_get_string(clear_stats_response.error_value());

  // Batch write the VMO to the ramdisk
  // Split it into two requests, spread across the disk
  block_fifo_request_t requests[] = {
      {
          .opcode = BLOCK_OP_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 1,
          .vmo_offset = 0,
          .dev_offset = 0,
      },
      {
          .opcode = BLOCK_OP_READ,
          .group = group,
          .vmoid = vmoid,
          .length = 1,
          .vmo_offset = 1,
          .dev_offset = 100,
      },
      {
          .opcode = BLOCK_OP_FLUSH,
          .group = group,
          .vmoid = vmoid,
          .length = 0,
          .vmo_offset = 0,
          .dev_offset = 0,
      },
      {
          .opcode = BLOCK_OP_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 1,
          .vmo_offset = 0,
          .dev_offset = 0,
      },
  };

  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_OK);

  const fidl::WireResult stats_result = fidl::WireCall(block_interface)->GetStats(true);
  ASSERT_TRUE(stats_result.ok()) << stats_result.FormatDescription();
  const fit::result stats_response = stats_result.value();
  ASSERT_TRUE(stats_response.is_ok()) << zx_status_get_string(stats_response.error_value());

  const fuchsia_hardware_block::wire::BlockStats& stats = stats_response.value()->stats;
  ASSERT_EQ(stats.write.success.total_calls, 2ul);
  ASSERT_EQ(stats.write.success.bytes_transferred, 2 * kBlockSize);
  ASSERT_GE(stats.read.success.total_calls, 1ul);
  ASSERT_GE(stats.read.success.bytes_transferred, 1 * kBlockSize);
  ASSERT_EQ(stats.flush.success.total_calls, 1ul);
  ASSERT_EQ(stats.flush.success.bytes_transferred, 0ul);

  ASSERT_EQ(stats.read.failure.total_calls, 0ul);
  ASSERT_EQ(stats.read.failure.bytes_transferred, 0ul);
  ASSERT_EQ(stats.write.failure.total_calls, 0ul);
  ASSERT_EQ(stats.write.failure.bytes_transferred, 0ul);

  // Close the current vmo
  requests[0].opcode = BLOCK_OP_CLOSE_VMO;
  ASSERT_EQ(client.Transaction(requests, 1), ZX_OK);
}

TEST(RamdiskTests, RamdiskGrowTestDimensionsChange) {
  constexpr size_t kBlockCount = 512;
  const size_t kBlockSize = zx_system_get_page_size() / 2;
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));

  // Grow the ramdisk.
  ASSERT_EQ(ramdisk_grow(ramdisk->ramdisk_client(), 2 * kBlockSize * kBlockCount), ZX_OK)
      << "Failed to grow ramdisk";

  // Check new block count.
  const fidl::WireResult result = fidl::WireCall(ramdisk->block_interface())->GetInfo();
  ASSERT_TRUE(result.ok()) << result.FormatDescription();
  const fit::result response = result.value();
  ASSERT_TRUE(response.is_ok()) << zx_status_get_string(response.error_value());
  const fuchsia_hardware_block::wire::BlockInfo& info = response.value()->info;
  ASSERT_EQ(info.block_count, 2 * kBlockCount);
  ASSERT_EQ(info.block_size, kBlockSize);
}

TEST(RamdiskTests, RamdiskGrowTestReadFromOldBlocks) {
  std::vector<uint8_t> buf(zx_system_get_page_size());
  std::vector<uint8_t> out(zx_system_get_page_size());
  constexpr size_t kBlockCount = 512;
  const size_t kBlockSize = zx_system_get_page_size() / 2;

  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  memset(buf.data(), 'a', buf.size());
  memset(out.data(), 0, out.size());

  // Write a page and a half
  ASSERT_EQ(BWrite(block_interface, buf.data(), buf.size(), 0), ZX_OK);
  ASSERT_EQ(BWrite(block_interface, buf.data(), buf.size() / 2, buf.size()), ZX_OK);

  // Grow the ramdisk.
  ASSERT_EQ(ramdisk_grow(ramdisk->ramdisk_client(), 2 * kBlockSize * kBlockCount), ZX_OK)
      << "Failed to grow ramdisk";

  // Seek to the start of the device and read the contents
  ASSERT_EQ(BRead(block_interface, out.data(), out.size(), 0), ZX_OK);
  ASSERT_EQ(memcmp(out.data(), buf.data(), out.size()), 0);
}

TEST(RamdiskTests, RamdiskGrowTestWriteToAddedBlocks) {
  std::vector<uint8_t> buf(zx_system_get_page_size());
  std::vector<uint8_t> out(zx_system_get_page_size());
  constexpr size_t kBlockCount = 512;
  const size_t kBlockSize = zx_system_get_page_size() / 2;

  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));
  memset(buf.data(), 'a', buf.size());
  memset(out.data(), 0, out.size());

  // Grow the ramdisk.
  ASSERT_EQ(ramdisk_grow(ramdisk->ramdisk_client(), 2 * kBlockSize * kBlockCount), ZX_OK)
      << "Failed to grow ramdisk";

  // Write a page and a half
  ASSERT_EQ(BWrite(ramdisk->block_interface(), buf.data(), buf.size(), kBlockSize * kBlockCount),
            ZX_OK);
  ASSERT_EQ(BWrite(ramdisk->block_interface(), buf.data(), buf.size() / 2,
                   buf.size() + kBlockSize * kBlockCount),
            ZX_OK);

  // Verify written data is readable from the new blocks.
  ASSERT_EQ(BRead(ramdisk->block_interface(), out.data(), out.size(), kBlockCount * kBlockSize),
            ZX_OK);
  ASSERT_EQ(memcmp(out.data(), buf.data(), out.size()), 0);
}

TEST(RamdiskTests, RamdiskTestGuid) {
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::CreateWithGuid(zx_system_get_page_size() / 2, 512, kGuid,
                                                      sizeof(kGuid), &ramdisk));

  const fidl::WireResult result =
      fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_partition::Partition>(
                         ramdisk->block_interface().channel()))
          ->GetTypeGuid();
  ASSERT_TRUE(result.ok()) << result.FormatDescription();
  const fidl::WireResponse response = result.value();
  ASSERT_EQ(response.status, ZX_OK);
  ASSERT_TRUE(memcmp(response.guid->value.data(), kGuid, response.guid->value.size()) == 0);
}

TEST(RamdiskTests, RamdiskTestVmo) {
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(256 * zx_system_get_page_size(), 0, &vmo), ZX_OK);

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create_from_vmo(vmo.release(), &ramdisk), ZX_OK);

  fidl::UnownedClientEnd<fuchsia_hardware_block::Block> block_interface(
      ramdisk_get_block_interface(ramdisk));

  std::vector<uint8_t> buf(zx_system_get_page_size() * 2);
  std::vector<uint8_t> out(zx_system_get_page_size() * 2);
  memset(buf.data(), 'a', buf.size());
  memset(out.data(), 0, out.size());

  EXPECT_EQ(BWrite(block_interface, buf.data(), buf.size(), 0), ZX_OK);
  EXPECT_EQ(BWrite(block_interface, buf.data(), buf.size() / 2, buf.size()), ZX_OK);

  // Seek to the start of the device and read the contents
  EXPECT_EQ(BRead(block_interface, out.data(), out.size(), 0), ZX_OK);
  EXPECT_EQ(memcmp(out.data(), buf.data(), out.size()), 0);

  EXPECT_GE(ramdisk_destroy(ramdisk), 0) << "Could not unlink ramdisk device";
}

TEST(RamdiskTests, RamdiskTestVmoWithParams) {
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 256;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(kBlockCount * kBlockSize, 0, &vmo), ZX_OK);

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(
      ramdisk_create_from_vmo_with_params(vmo.release(), kBlockSize, kGuid, sizeof kGuid, &ramdisk),
      ZX_OK);

  fidl::UnownedClientEnd<fuchsia_hardware_block::Block> block_interface(
      ramdisk_get_block_interface(ramdisk));

  {
    const fidl::WireResult result = fidl::WireCall(block_interface)->GetInfo();
    ASSERT_TRUE(result.ok()) << result.FormatDescription();
    const fit::result response = result.value();
    ASSERT_TRUE(response.is_ok()) << zx_status_get_string(response.error_value());
    const fuchsia_hardware_block::wire::BlockInfo& info = response.value()->info;
    ASSERT_EQ(info.block_count, kBlockCount);
    ASSERT_EQ(info.block_size, kBlockSize);
  }

  {
    const fidl::WireResult result =
        fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_partition::Partition>(
                           block_interface.channel()))
            ->GetTypeGuid();
    ASSERT_TRUE(result.ok()) << result.FormatDescription();
    const fidl::WireResponse response = result.value();
    ASSERT_EQ(response.status, ZX_OK);
    ASSERT_TRUE(memcmp(response.guid->value.data(), kGuid, response.guid->value.size()) == 0);
  }

  uint8_t buf[kBlockSize * 2];
  uint8_t out[kBlockSize * 2];
  memset(buf, 'a', sizeof(buf));
  memset(out, 0, sizeof(out));

  EXPECT_EQ(BWrite(block_interface, buf, sizeof(buf), 0), ZX_OK);
  EXPECT_EQ(BWrite(block_interface, buf, sizeof(buf) / 2, sizeof(buf)), ZX_OK);

  // Seek to the start of the device and read the contents
  EXPECT_EQ(BRead(block_interface, out, sizeof(out), 0), ZX_OK);
  EXPECT_EQ(memcmp(out, buf, sizeof(out)), 0);

  EXPECT_GE(ramdisk_destroy(ramdisk), 0) << "Could not unlink ramdisk device";
}

// This test creates a ramdisk, verifies it is visible in the filesystem
// (where we expect it to be!) and verifies that it is removed when we
// "unplug" the device.
TEST(RamdiskTests, RamdiskTestFilesystem) {
  // Make a ramdisk
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size() / 2, 512, &ramdisk));
  char ramdisk_path[PATH_MAX];
  strlcpy(ramdisk_path, ramdisk_get_path(ramdisk->ramdisk_client()), sizeof(ramdisk_path));

  // Ramdisk name is of the form: ".../NAME/block"
  // Extract "NAME".
  const char* name_end = strrchr(ramdisk_path, '/');
  const char* name_start = name_end - 1;
  while (*(name_start - 1) != '/')
    name_start--;
  char name[NAME_MAX];
  memcpy(name, name_start, name_end - name_start);
  name[name_end - name_start] = 0;

  // Verify the ramdisk name
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  const fidl::WireResult result =
      fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_partition::Partition>(
                         block_interface.channel()))
          ->GetName();
  ASSERT_TRUE(result.ok()) << result.FormatDescription();
  const fidl::WireResponse response = result.value();
  ASSERT_EQ(response.status, ZX_OK);
  ASSERT_EQ(response.name.get(), std::string_view(name));

  // Find the name of the ramdisk under "/dev/class/block", since it is a block device.
  // Be slightly more lenient with errors during this section, since we might be poking
  // block devices that don't belong to us.
  char blockpath[PATH_MAX];
  strcpy(blockpath, "/dev/class/block/");
  DIR* dir = opendir(blockpath);
  ASSERT_NE(dir, nullptr);
  const auto closer = fit::defer([dir]() { closedir(dir); });

  typedef struct watcher_args {
    const char* expected_name;
    char* blockpath;
    fbl::String filename;
    bool found;
  } watcher_args_t;

  watcher_args_t args;
  args.expected_name = name;
  args.blockpath = blockpath;
  args.found = false;

  auto cb = [](int dirfd, int event, const char* fn, void* cookie) {
    watcher_args_t* args = static_cast<watcher_args_t*>(cookie);
    if (event != WATCH_EVENT_ADD_FILE) {
      return ZX_OK;
    }
    if (std::string_view{fn} == ".") {
      return ZX_OK;
    }
    fdio_cpp::UnownedFdioCaller caller(dirfd);
    zx::result channel =
        component::ConnectAt<fuchsia_hardware_block_partition::Partition>(caller.directory(), fn);
    if (channel.is_error()) {
      return channel.status_value();
    }

    const fidl::WireResult result = fidl::WireCall(channel.value())->GetName();
    if (!result.ok()) {
      return result.status();
    }
    const fidl::WireResponse response = result.value();
    if (zx_status_t status = response.status; status != ZX_OK) {
      return status;
    }
    if (response.name.get() != args->expected_name) {
      return ZX_OK;
    }
    // Found a device under /dev/class/block/XYZ with the name of the
    // ramdisk we originally created.
    strncat(args->blockpath, fn, sizeof(blockpath) - (strlen(args->blockpath) + 1));
    args->filename = fbl::String(fn);
    args->found = true;
    return ZX_ERR_STOP;
  };

  zx_time_t deadline = zx_deadline_after(ZX_SEC(3));
  ASSERT_EQ(fdio_watch_directory(dirfd(dir), cb, deadline, &args), ZX_ERR_STOP);
  ASSERT_TRUE(args.found);

  // Check dev block is accessible before destruction
  int devfd = open(blockpath, O_RDONLY);
  ASSERT_GE(devfd, 0) << "Ramdisk is not visible in /dev/class/block";
  ASSERT_EQ(close(devfd), 0);

  // Start watching for the block device removal.
  std::unique_ptr<device_watcher::DirWatcher> watcher;
  ASSERT_EQ(device_watcher::DirWatcher::Create(dirfd(dir), &watcher), ZX_OK);

  ASSERT_NO_FATAL_FAILURE(ramdisk->Terminate());

  ASSERT_EQ(watcher->WaitForRemoval(args.filename, zx::sec(5)), ZX_OK);

  // Now that we've unlinked the ramdisk, we should notice that it doesn't appear
  // under /dev/class/block.
  ASSERT_EQ(open(blockpath, O_RDONLY), -1) << "Ramdisk is visible in /dev after destruction";
}

TEST(RamdiskTests, RamdiskTestRebind) {
  // Make a ramdisk
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size() / 2, 512, &ramdisk));

  ASSERT_EQ(ramdisk->Rebind(), ZX_OK);
  ASSERT_EQ(
      device_watcher::RecursiveWaitForFile(ramdisk_get_path(ramdisk->ramdisk_client()), zx::sec(3))
          .status_value(),
      ZX_OK);
}

TEST(RamdiskTests, RamdiskTestBadRequests) {
  std::vector<uint8_t> buf(zx_system_get_page_size());

  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk));
  memset(buf.data(), 'a', buf.size());

  // Read / write non-multiples of the block size
  ASSERT_NE(BWrite(ramdisk->block_interface(), buf.data(), zx_system_get_page_size() - 1, 0),
            ZX_OK);
  ASSERT_NE(BWrite(ramdisk->block_interface(), buf.data(), zx_system_get_page_size() / 2, 0),
            ZX_OK);
  ASSERT_NE(BRead(ramdisk->block_interface(), buf.data(), zx_system_get_page_size() - 1, 0), ZX_OK);
  ASSERT_NE(BRead(ramdisk->block_interface(), buf.data(), zx_system_get_page_size() / 2, 0), ZX_OK);

  // Read / write from unaligned offset
  ASSERT_NE(BWrite(ramdisk->block_interface(), buf.data(), zx_system_get_page_size(), 1), ZX_OK);
  ASSERT_NE(BRead(ramdisk->block_interface(), buf.data(), zx_system_get_page_size(), 1), ZX_OK);

  // Read / write at end of device
  off_t offset = zx_system_get_page_size() * 512;
  ASSERT_NE(BWrite(ramdisk->block_interface(), buf.data(), zx_system_get_page_size(), offset),
            ZX_OK);
  ASSERT_NE(BRead(ramdisk->block_interface(), buf.data(), zx_system_get_page_size(), offset),
            ZX_OK);
}

TEST(RamdiskTests, RamdiskTestReleaseDuringAccess) {
  ramdisk_client_t* ramdisk = GetRamdisk(zx_system_get_page_size(), 512);
  ASSERT_NE(ramdisk, nullptr);

  // Spin up a background thread to repeatedly access
  // the first few blocks.
  auto bg_thread = [](void* arg) {
    auto& block_interface =
        *reinterpret_cast<fidl::UnownedClientEnd<fuchsia_hardware_block::Block>*>(arg);
    while (true) {
      uint8_t in[8192];
      memset(in, 'a', sizeof(in));
      if (BWrite(block_interface, in, sizeof(in), 0) != ZX_OK) {
        return 0;
      }
      uint8_t out[8192];
      memset(out, 0, sizeof(out));
      if (BRead(block_interface, out, sizeof(out), 0) != ZX_OK) {
        return 0;
      }
      // If we DID manage to read it, then the data should be valid...
      if (memcmp(in, out, sizeof(in)) != 0) {
        return -1;
      }
    }
  };

  thrd_t thread;
  fidl::UnownedClientEnd<fuchsia_hardware_block::Block> block_interface(
      ramdisk_get_block_interface(ramdisk));
  ASSERT_EQ(thrd_create(&thread, bg_thread, &block_interface), thrd_success);
  // Let the background thread warm up a little bit...
  usleep(10000);
  // ... and close the entire ramdisk from undearneath it!
  ASSERT_EQ(ramdisk_destroy(ramdisk), ZX_OK);

  int res;
  ASSERT_EQ(thrd_join(thread, &res), thrd_success);
  ASSERT_EQ(res, 0) << "Background thread failed";
}

TEST(RamdiskTests, RamdiskTestMultiple) {
  std::vector<uint8_t> buf(zx_system_get_page_size());
  std::vector<uint8_t> out(zx_system_get_page_size());

  std::unique_ptr<RamdiskTest> ramdisk1;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk1));
  std::unique_ptr<RamdiskTest> ramdisk2;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk2));

  // Write 'a' to fd1, write 'b', to fd2
  memset(buf.data(), 'a', buf.size());
  ASSERT_EQ(BWrite(ramdisk1->block_interface(), buf.data(), buf.size(), 0), ZX_OK);
  memset(buf.data(), 'b', buf.size());
  ASSERT_EQ(BWrite(ramdisk2->block_interface(), buf.data(), buf.size(), 0), ZX_OK);

  // Read 'b' from fd2, read 'a' from fd1
  ASSERT_EQ(BRead(ramdisk2->block_interface(), out.data(), buf.size(), 0), ZX_OK);
  ASSERT_EQ(memcmp(out.data(), buf.data(), out.size()), 0);
  ASSERT_NO_FATAL_FAILURE(ramdisk2->Terminate()) << "Could not unlink ramdisk device";

  memset(buf.data(), 'a', buf.size());
  ASSERT_EQ(BRead(ramdisk1->block_interface(), out.data(), buf.size(), 0), ZX_OK);
  ASSERT_EQ(memcmp(out.data(), buf.data(), out.size()), 0);
  ASSERT_NO_FATAL_FAILURE(ramdisk1->Terminate()) << "Could not unlink ramdisk device";
}

TEST(RamdiskTests, RamdiskTestFifoNoOp) {
  // Get a FIFO connection to a ramdisk and immediately close it
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size() / 2, 512, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  auto open_and_close_fifo = [block_interface]() {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_block::Session>();
    ASSERT_TRUE(endpoints.is_ok()) << endpoints.status_string();
    auto& [session, server] = endpoints.value();

    {
      const fidl::Status result = fidl::WireCall(block_interface)->OpenSession(std::move(server));
      ASSERT_TRUE(result.ok()) << result.FormatDescription();
    }

    {
      const fidl::WireResult result = fidl::WireCall(session)->Close();
      ASSERT_TRUE(result.ok()) << result.FormatDescription();
      const fit::result response = result.value();
      ASSERT_TRUE(response.is_ok()) << zx_status_get_string(response.error_value());
    }
  };

  ASSERT_NO_FATAL_FAILURE(open_and_close_fifo());
  ASSERT_NO_FATAL_FAILURE(open_and_close_fifo());

  ASSERT_NO_FATAL_FAILURE(ramdisk->Terminate()) << "Could not unlink ramdisk device";
}

TEST(RamdiskTests, RamdiskTestFifoBasic) {
  // Set up the initial handshake connection with the ramdisk
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create an arbitrary VMO, fill it with some stuff
  uint64_t vmo_size = zx_system_get_page_size() * 3;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK) << "Failed to create VMO";
  std::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
  fill_random(buf.get(), vmo_size);

  ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

  // Send a handle to the vmo to the block device, get a vmoid which identifies it
  zx::vmo xfer_vmo;
  ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
  zx::result vmoid_result = client.RegisterVmo(vmo);
  ASSERT_TRUE(vmoid_result.is_ok()) << vmoid_result.status_string();
  vmoid_t vmoid = vmoid_result.value().TakeId();

  // Batch write the VMO to the ramdisk
  // Split it into two requests, spread across the disk
  block_fifo_request_t requests[] = {
      {
          .opcode = BLOCK_OP_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 1,
          .vmo_offset = 0,
          .dev_offset = 0,
      },
      {
          .opcode = BLOCK_OP_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 2,
          .vmo_offset = 1,
          .dev_offset = 100,
      },
  };

  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_OK);

  // Empty the vmo, then read the info we just wrote to the disk
  std::unique_ptr<uint8_t[]> out(new uint8_t[vmo_size]());

  ASSERT_EQ(vmo.write(out.get(), 0, vmo_size), ZX_OK);
  requests[0].opcode = BLOCK_OP_READ;
  requests[1].opcode = BLOCK_OP_READ;
  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_OK);
  ASSERT_EQ(vmo.read(out.get(), 0, vmo_size), ZX_OK);
  ASSERT_EQ(memcmp(buf.get(), out.get(), vmo_size), 0) << "Read data not equal to written data";

  // Close the current vmo
  requests[0].opcode = BLOCK_OP_CLOSE_VMO;
  ASSERT_EQ(client.Transaction(requests, 1), ZX_OK);
}

TEST(RamdiskTests, RamdiskTestFifoNoGroup) {
  // Set up the initial handshake connection with the ramdisk
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_block::Session>();
  ASSERT_TRUE(endpoints.is_ok()) << endpoints.status_string();
  auto& [session, server] = endpoints.value();

  const fidl::Status result = fidl::WireCall(block_interface)->OpenSession(std::move(server));
  ASSERT_TRUE(result.ok()) << result.FormatDescription();

  const fidl::WireResult fifo_result = fidl::WireCall(session)->GetFifo();
  ASSERT_TRUE(fifo_result.ok()) << fifo_result.FormatDescription();
  const fit::result fifo_response = fifo_result.value();
  ASSERT_TRUE(fifo_response.is_ok()) << zx_status_get_string(fifo_response.error_value());
  fzl::fifo<block_fifo_request_t, block_fifo_response_t> fifo(std::move(fifo_response->fifo));

  // Create an arbitrary VMO, fill it with some stuff
  uint64_t vmo_size = zx_system_get_page_size() * 3;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK) << "Failed to create VMO";
  std::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
  fill_random(buf.get(), vmo_size);

  ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

  // Send a handle to the vmo to the block device, get a vmoid which identifies it
  zx::vmo xfer_vmo;
  ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
  const fidl::WireResult attach_vmo_result =
      fidl::WireCall(session)->AttachVmo(std::move(xfer_vmo));
  ASSERT_TRUE(attach_vmo_result.ok()) << attach_vmo_result.FormatDescription();
  const fit::result attach_vmo_response = attach_vmo_result.value();
  ASSERT_TRUE(attach_vmo_response.is_ok())
      << zx_status_get_string(attach_vmo_response.error_value());

  // Batch write the VMO to the ramdisk
  // Split it into two requests, spread across the disk
  block_fifo_request_t requests[2];
  requests[0].reqid = 0;
  requests[0].vmoid = attach_vmo_response.value()->vmoid.id;
  requests[0].opcode = BLOCK_OP_WRITE;
  requests[0].length = 1;
  requests[0].vmo_offset = 0;
  requests[0].dev_offset = 0;

  requests[1].reqid = 1;
  requests[1].vmoid = attach_vmo_response.value()->vmoid.id;
  requests[1].opcode = BLOCK_OP_WRITE;
  requests[1].length = 2;
  requests[1].vmo_offset = 1;
  requests[1].dev_offset = 100;

  auto write_request = [&fifo](block_fifo_request_t* request) {
    size_t actual;
    ASSERT_EQ(fifo.write(request, 1, &actual), ZX_OK);
    ASSERT_EQ(actual, 1ul);
  };

  auto read_response = [&fifo](reqid_t reqid) {
    zx::time deadline = zx::deadline_after(zx::sec(1));
    block_fifo_response_t response;
    ASSERT_EQ(fifo.wait_one(ZX_FIFO_READABLE, deadline, nullptr), ZX_OK);
    ASSERT_EQ(fifo.read(&response, 1, nullptr), ZX_OK);
    ASSERT_EQ(response.status, ZX_OK);
    ASSERT_EQ(response.reqid, reqid);
  };

  ASSERT_NO_FATAL_FAILURE(write_request(requests));
  ASSERT_NO_FATAL_FAILURE(read_response(0));
  ASSERT_NO_FATAL_FAILURE(write_request(&requests[1]));
  ASSERT_NO_FATAL_FAILURE(read_response(1));

  // Empty the vmo, then read the info we just wrote to the disk
  std::unique_ptr<uint8_t[]> out(new uint8_t[vmo_size]());

  ASSERT_EQ(vmo.write(out.get(), 0, vmo_size), ZX_OK);

  requests[0].opcode = BLOCK_OP_READ;
  requests[1].opcode = BLOCK_OP_READ;

  ASSERT_NO_FATAL_FAILURE(write_request(requests));
  ASSERT_NO_FATAL_FAILURE(read_response(0));
  ASSERT_NO_FATAL_FAILURE(write_request(&requests[1]));
  ASSERT_NO_FATAL_FAILURE(read_response(1));

  ASSERT_EQ(vmo.read(out.get(), 0, vmo_size), ZX_OK);
  ASSERT_EQ(memcmp(buf.get(), out.get(), vmo_size), 0) << "Read data not equal to written data";

  // Close the current vmo
  requests[0].opcode = BLOCK_OP_CLOSE_VMO;
  ASSERT_EQ(fifo.write(requests, 1, nullptr), ZX_OK);
}

using TestVmoObject = struct {
  uint64_t vmo_size;
  zx::vmo vmo;
  fuchsia_hardware_block::wire::VmoId vmoid;
  std::unique_ptr<uint8_t[]> buf;
};

// Creates a VMO, fills it with data, and gives it to the block device.
void CreateVmoHelper(block_client::Client& client, TestVmoObject& obj, size_t kBlockSize) {
  obj.vmo_size = kBlockSize + (rand() % 5) * kBlockSize;
  ASSERT_EQ(zx::vmo::create(obj.vmo_size, 0, &obj.vmo), ZX_OK) << "Failed to create vmo";
  obj.buf.reset(new uint8_t[obj.vmo_size]);
  fill_random(obj.buf.get(), obj.vmo_size);
  ASSERT_EQ(obj.vmo.write(obj.buf.get(), 0, obj.vmo_size), ZX_OK) << "Failed to write to vmo";

  zx::result vmoid = client.RegisterVmo(obj.vmo);
  ASSERT_TRUE(vmoid.is_ok()) << vmoid.status_string();
  obj.vmoid.id = vmoid.value().TakeId();
}

// Write all vmos in a striped pattern on disk.
// For objs.size() == 10,
// i = 0 will write vmo block 0, 1, 2, 3... to dev block 0, 10, 20, 30...
// i = 1 will write vmo block 0, 1, 2, 3... to dev block 1, 11, 21, 31...
void WriteStripedVmoHelper(block_client::Client& client, const TestVmoObject& obj, size_t i,
                           size_t objs, groupid_t group, size_t kBlockSize) {
  // Make a separate request for each block
  size_t blocks = obj.vmo_size / kBlockSize;
  std::vector<block_fifo_request_t> requests(blocks);
  for (size_t b = 0; b < blocks; b++) {
    requests[b].group = group;
    requests[b].vmoid = obj.vmoid.id;
    requests[b].opcode = BLOCK_OP_WRITE;
    requests[b].length = 1;
    requests[b].vmo_offset = b;
    requests[b].dev_offset = i + b * objs;
  }
  // Write entire vmos at once
  ASSERT_EQ(client.Transaction(requests.data(), requests.size()), ZX_OK);
}

// Verifies the result from "WriteStripedVmoHelper"
void ReadStripedVmoHelper(block_client::Client& client, const TestVmoObject& obj, size_t i,
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
    requests[b].opcode = BLOCK_OP_READ;
    requests[b].length = 1;
    requests[b].vmo_offset = b;
    requests[b].dev_offset = i + b * objs;
  }
  // Read entire vmos at once
  ASSERT_EQ(client.Transaction(requests.data(), requests.size()), ZX_OK);

  // Finally, write from the vmo to an out buffer, where we can compare
  // the results with the input buffer.
  ASSERT_EQ(obj.vmo.read(out.get(), 0, obj.vmo_size), ZX_OK);
  ASSERT_EQ(memcmp(obj.buf.get(), out.get(), obj.vmo_size), 0)
      << "Read data not equal to written data";
}

// Tears down an object created by "CreateVmoHelper".
void CloseVmoHelper(block_client::Client& client, const TestVmoObject& obj, groupid_t group) {
  block_fifo_request_t request;
  request.group = group;
  request.vmoid = obj.vmoid.id;
  request.opcode = BLOCK_OP_CLOSE_VMO;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_OK);
}

TEST(RamdiskTests, RamdiskTestFifoMultipleVmo) {
  // Set up the initial handshake connection with the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create multiple VMOs
  std::vector<TestVmoObject> objs(10);
  for (auto& obj : objs) {
    ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBlockSize));
  }

  for (size_t i = 0; i < objs.size(); i++) {
    ASSERT_NO_FATAL_FAILURE(
        WriteStripedVmoHelper(client, objs[i], i, objs.size(), group, kBlockSize));
  }

  for (size_t i = 0; i < objs.size(); i++) {
    ASSERT_NO_FATAL_FAILURE(
        ReadStripedVmoHelper(client, objs[i], i, objs.size(), group, kBlockSize));
  }

  for (const auto& obj : objs) {
    ASSERT_NO_FATAL_FAILURE(CloseVmoHelper(client, obj, group));
  }
}

TEST(RamdiskTests, RamdiskTestFifoMultipleVmoMultithreaded) {
  // Set up the initial handshake connection with the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  // Create multiple VMOs
  constexpr size_t kNumThreads = MAX_TXN_GROUP_COUNT;
  std::vector<TestVmoObject> objs(kNumThreads);

  std::vector<std::thread> threads;
  for (size_t i = 0; i < kNumThreads; i++) {
    // Capture i by value to get the updated version each loop iteration.
    threads.emplace_back([&, i]() {
      groupid_t group = static_cast<groupid_t>(i);
      ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, objs[i], kBlockSize));
      ASSERT_NO_FATAL_FAILURE(
          WriteStripedVmoHelper(client, objs[i], i, objs.size(), group, kBlockSize));
      ASSERT_NO_FATAL_FAILURE(
          ReadStripedVmoHelper(client, objs[i], i, objs.size(), group, kBlockSize));
      ASSERT_NO_FATAL_FAILURE(CloseVmoHelper(client, objs[i], group));
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }
}

TEST(RamdiskTests, RamdiskTestFifoLargeOpsCount) {
  // Set up the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  // Create a vmo
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBlockSize));

  for (size_t num_ops = 1; num_ops <= 32; num_ops++) {
    groupid_t group = 0;

    std::vector<block_fifo_request_t> requests(num_ops);

    for (size_t b = 0; b < num_ops; b++) {
      requests[b].group = group;
      requests[b].vmoid = obj.vmoid.id;
      requests[b].opcode = BLOCK_OP_WRITE;
      requests[b].length = 1;
      requests[b].vmo_offset = 0;
      requests[b].dev_offset = 0;
    }

    ASSERT_EQ(client.Transaction(requests.data(), requests.size()), ZX_OK);
  }
}

TEST(RamdiskTests, RamdiskTestFifoLargeOpsCountShutdown) {
  // Set up the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_block::Session>();
  ASSERT_TRUE(endpoints.is_ok()) << endpoints.status_string();
  auto& [session, server] = endpoints.value();

  const fidl::Status result = fidl::WireCall(block_interface)->OpenSession(std::move(server));
  ASSERT_TRUE(result.ok()) << result.FormatDescription();

  // Get the FIFO twice since the client doesn't expose it after construction.
  zx::fifo fifo;
  {
    const fidl::WireResult fifo_result = fidl::WireCall(session)->GetFifo();
    ASSERT_TRUE(fifo_result.ok()) << fifo_result.FormatDescription();
    const fit::result fifo_response = fifo_result.value();
    ASSERT_TRUE(fifo_response.is_ok()) << zx_status_get_string(fifo_response.error_value());
    fifo = std::move(fifo_response.value()->fifo);
  }

  const fidl::WireResult fifo_result = fidl::WireCall(session)->GetFifo();
  ASSERT_TRUE(fifo_result.ok()) << fifo_result.FormatDescription();
  const fit::result fifo_response = fifo_result.value();
  ASSERT_TRUE(fifo_response.is_ok()) << zx_status_get_string(fifo_response.error_value());

  block_client::Client client(std::move(session), std::move(fifo_response.value()->fifo));

  // Create a vmo
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBlockSize));

  const size_t kNumOps = BLOCK_FIFO_MAX_DEPTH;
  groupid_t group = 0;

  std::vector<block_fifo_request_t> requests(kNumOps);

  for (size_t b = 0; b < kNumOps; b++) {
    requests[b].group = group;
    requests[b].vmoid = obj.vmoid.id;
    requests[b].opcode = BLOCK_OP_WRITE | BLOCK_GROUP_ITEM;
    requests[b].length = 1;
    requests[b].vmo_offset = 0;
    requests[b].dev_offset = b;
  }

  // Enqueue multiple barrier-based operations without waiting
  // for completion. The intention here is for the block device
  // server to be busy processing multiple pending operations
  // when the FIFO is suddenly closed, causing "server termination
  // with pending work".
  //
  // It's obviously hit-or-miss whether the server will actually
  // be processing work when we shut down the fifo, but run in a
  // loop, this test was able to trigger deadlocks in a buggy
  // version of the server; as a consequence, it is preserved
  // to help detect regressions.
  size_t actual;
  ASSERT_EQ(fifo.write(sizeof(block_fifo_request_t), requests.data(), requests.size(), &actual),
            ZX_OK);
  usleep(100);
}

TEST(RamdiskTests, RamdiskTestFifoIntermediateOpFailure) {
  // Set up the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  constexpr size_t kRequestCount = 3;
  const size_t kBufferSize = kRequestCount * kBlockSize;

  // Create a vmo
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBufferSize));

  // Store the original value of the VMO
  std::unique_ptr<uint8_t[]> originalbuf;
  originalbuf.reset(new uint8_t[kBufferSize]);

  ASSERT_EQ(obj.vmo.read(originalbuf.get(), 0, kBufferSize), ZX_OK);

  // Test that we can use regular transactions (writing)
  block_fifo_request_t requests[kRequestCount];
  for (size_t i = 0; i < std::size(requests); i++) {
    requests[i].group = group;
    requests[i].vmoid = obj.vmoid.id;
    requests[i].opcode = BLOCK_OP_WRITE;
    requests[i].length = 1;
    requests[i].vmo_offset = i;
    requests[i].dev_offset = i;
  }
  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_OK);

  std::unique_ptr<uint8_t[]> tmpbuf;
  tmpbuf.reset(new uint8_t[kBufferSize]);

  for (size_t bad_arg = 0; bad_arg < std::size(requests); bad_arg++) {
    // Empty out the VMO so we can test reading it
    memset(tmpbuf.get(), 0, kBufferSize);
    ASSERT_EQ(obj.vmo.write(tmpbuf.get(), 0, kBufferSize), ZX_OK);

    // Test that invalid intermediate operations cause:
    // - Previous operations to continue anyway
    // - Later operations to fail
    for (size_t i = 0; i < std::size(requests); i++) {
      requests[i].group = group;
      requests[i].vmoid = obj.vmoid.id;
      requests[i].opcode = BLOCK_OP_READ;
      requests[i].length = 1;
      requests[i].vmo_offset = i;
      requests[i].dev_offset = i;
    }
    // Inserting "bad argument".
    requests[bad_arg].length = 0;
    ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_ERR_INVALID_ARGS);

    // Test that all operations up the bad argument completed, but the later
    // ones did not.
    ASSERT_EQ(obj.vmo.read(tmpbuf.get(), 0, kBufferSize), ZX_OK);

    // First few (successful) operations
    ASSERT_EQ(memcmp(tmpbuf.get(), originalbuf.get(), kBlockSize * bad_arg), 0);
    // Later (failed) operations
    for (size_t i = kBlockSize * (bad_arg + 1); i < kBufferSize; i++) {
      ASSERT_EQ(tmpbuf[i], 0);
    }
  }
}

TEST(RamdiskTests, RamdiskTestFifoBadClientVmoid) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create a vmo
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBlockSize));

  // Bad request: Writing to the wrong vmoid
  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id + 5);
  request.opcode = BLOCK_OP_WRITE;
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = 0;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_IO) << "Expected IO error with bad vmoid";
}

TEST(RamdiskTests, RamdiskTestFifoBadClientUnalignedRequest) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create a vmo of at least size "kBlockSize * 2", since we'll
  // be reading "kBlockSize" bytes from an offset below, and we want it
  // to fit within the bounds of the VMO.
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBlockSize * 2));

  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
  request.opcode = BLOCK_OP_WRITE;

  // Send a request that has zero length
  request.length = 0;
  request.vmo_offset = 0;
  request.dev_offset = 0;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_INVALID_ARGS);
}

TEST(RamdiskTests, RamdiskTestFifoBadClientOverflow) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the ramdisk
  const uint64_t kBlockSize = zx_system_get_page_size();
  const uint64_t kBlockCount = 1 << 18;
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create a vmo of at least size "kBlockSize * 2", since we'll
  // be reading "kBlockSize" bytes from an offset below, and we want it
  // to fit within the bounds of the VMO.
  TestVmoObject obj;
  ASSERT_NO_FATAL_FAILURE(CreateVmoHelper(client, obj, kBlockSize * 2));

  block_fifo_request_t request;
  request.group = group;
  request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
  request.opcode = BLOCK_OP_WRITE;

  // Send a request that is barely out-of-bounds for the device
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = kBlockCount;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that is half out-of-bounds for the device
  request.length = 2;
  request.vmo_offset = 0;
  request.dev_offset = kBlockCount - 1;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that is very out-of-bounds for the device
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = kBlockCount + 1;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that tries to overflow the VMO
  request.length = 2;
  request.vmo_offset = std::numeric_limits<uint64_t>::max();
  request.dev_offset = 0;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

  // Send a request that tries to overflow the device
  request.length = 2;
  request.vmo_offset = 0;
  request.dev_offset = std::numeric_limits<uint64_t>::max();
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
}

TEST(RamdiskTests, RamdiskTestFifoBadClientBadVmo) {
  // Try to flex the server's error handling by sending 'malicious' client requests.
  // Set up the ramdisk
  const size_t kBlockSize = zx_system_get_page_size();
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

  // Create a connection to the ramdisk
  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // create a VMO of 1 block, which will round up to zx_system_get_page_size()
  TestVmoObject obj;
  obj.vmo_size = kBlockSize;
  ASSERT_EQ(zx::vmo::create(obj.vmo_size, 0, &obj.vmo), ZX_OK) << "Failed to create vmo";
  obj.buf.reset(new uint8_t[obj.vmo_size]);
  fill_random(obj.buf.get(), obj.vmo_size);
  ASSERT_EQ(obj.vmo.write(obj.buf.get(), 0, obj.vmo_size), ZX_OK) << "Failed to write to vmo";

  zx::result vmoid = client.RegisterVmo(obj.vmo);
  ASSERT_TRUE(vmoid.is_ok()) << vmoid.status_string();
  obj.vmoid.id = vmoid.value().TakeId();

  // Send a request to write to write 2 blocks -- even though that's larger than the VMO
  block_fifo_request_t request = {
      .opcode = BLOCK_OP_WRITE,
      .group = group,
      .vmoid = obj.vmoid.id,
      .length = 2,
      .vmo_offset = 0,
      .dev_offset = 0,
  };
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
  // Do the same thing, but for reading
  request.opcode = BLOCK_OP_READ;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
  request.length = 2;
  ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
}

TEST(RamdiskTests, RamdiskTestFifoSleepUnavailable) {
  // Set up the initial handshake connection with the ramdisk
  std::unique_ptr<RamdiskTest> ramdisk;
  ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk));

  fidl::UnownedClientEnd block_interface = ramdisk->block_interface();

  zx::result client_ptr = CreateSession(block_interface);
  ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
  block_client::Client& client = *client_ptr.value();

  groupid_t group = 0;

  // Create an arbitrary VMO, fill it with some stuff
  uint64_t vmo_size = zx_system_get_page_size() * 3;
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK) << "Failed to create VMO";
  std::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
  fill_random(buf.get(), vmo_size);

  ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

  // Send a handle to the vmo to the block device, get a vmoid which identifies it
  zx::result vmoid_result = client.RegisterVmo(vmo);
  ASSERT_TRUE(vmoid_result.is_ok()) << vmoid_result.status_string();
  vmoid_t vmoid = vmoid_result.value().TakeId();

  // Put the ramdisk to sleep after 1 block (complete transaction).
  uint64_t one = 1;
  ASSERT_EQ(ramdisk_sleep_after(ramdisk->ramdisk_client(), one), ZX_OK);

  // Batch write the VMO to the ramdisk
  // Split it into two requests, spread across the disk
  block_fifo_request_t requests[] = {
      {
          .opcode = BLOCK_OP_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 1,
          .vmo_offset = 0,
          .dev_offset = 0,
      },
      {
          .opcode = BLOCK_OP_WRITE,
          .group = group,
          .vmoid = vmoid,
          .length = 2,
          .vmo_offset = 1,
          .dev_offset = 100,
      },
  };

  // Send enough requests for the ramdisk to fall asleep before completing.
  // Other callers (e.g. block_watcher) may also send requests without affecting this test.
  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_ERR_UNAVAILABLE);

  ramdisk_block_write_counts_t counts;
  ASSERT_EQ(ramdisk_get_block_counts(ramdisk->ramdisk_client(), &counts), ZX_OK);
  ASSERT_EQ(counts.received, 3ul);
  ASSERT_EQ(counts.successful, 1ul);
  ASSERT_EQ(counts.failed, 2ul);

  // Wake the ramdisk back up
  ASSERT_EQ(ramdisk_wake(ramdisk->ramdisk_client()), ZX_OK);
  requests[0].opcode = BLOCK_OP_READ;
  requests[1].opcode = BLOCK_OP_READ;
  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_OK);

  // Put the ramdisk to sleep after 1 block (partial transaction).
  ASSERT_EQ(ramdisk_sleep_after(ramdisk->ramdisk_client(), one), ZX_OK);

  // Batch write the VMO to the ramdisk.
  // Split it into two requests, spread across the disk.
  requests[0].opcode = BLOCK_OP_WRITE;
  requests[0].length = 2;

  requests[1].opcode = BLOCK_OP_WRITE;
  requests[1].length = 1;
  requests[1].vmo_offset = 2;

  // Send enough requests for the ramdisk to fall asleep before completing.
  // Other callers (e.g. block_watcher) may also send requests without affecting this test.
  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_ERR_UNAVAILABLE);

  ASSERT_EQ(ramdisk_get_block_counts(ramdisk->ramdisk_client(), &counts), ZX_OK);

  // Depending on timing, the second request might not get sent to the ramdisk because the first one
  // fails quickly before it has been sent (and the block driver will handle it), so there are two
  // possible cases we might see.
  if (counts.received == 2) {
    EXPECT_EQ(counts.successful, 1ul);
    EXPECT_EQ(counts.failed, 1ul);
  } else {
    EXPECT_EQ(counts.received, 3ul);
    EXPECT_EQ(counts.successful, 1ul);
    EXPECT_EQ(counts.failed, 2ul);
  }

  // Wake the ramdisk back up
  ASSERT_EQ(ramdisk_wake(ramdisk->ramdisk_client()), ZX_OK);
  requests[0].opcode = BLOCK_OP_READ;
  requests[1].opcode = BLOCK_OP_READ;
  ASSERT_EQ(client.Transaction(requests, std::size(requests)), ZX_OK);

  // Close the current vmo
  requests[0].opcode = BLOCK_OP_CLOSE_VMO;
  ASSERT_EQ(client.Transaction(requests, 1), ZX_OK);
}

// This thread and its arguments can be used to wake a ramdisk that sleeps with deferred writes.
// The correct calling sequence in the calling thread is:
//   thrd_create(&thread, fifo_wake_thread, &wake);
//   ramdisk_sleep_after(wake->fd, &one);
//   sync_completion_signal(&wake.start);
//   block_fifo_txn(client, requests, std::size(requests));
//   thrd_join(thread, &res);
//
// This order matters!
// * |sleep_after| must be called from the same thread as |fifo_txn| (or they may be reordered, and
//   the txn counts zeroed).
// * The do-while loop below must not be started before |sleep_after| has been called (hence the
//   'start' signal).
// * This thread must not be waiting when the calling thread blocks in |fifo_txn| (i.e. 'start' must
//   have been signaled.)

using wake_args_t = struct wake_args {
  const ramdisk_client_t* ramdisk_client;
  uint64_t after;
  sync_completion_t start;
  zx_time_t deadline;
};

zx_status_t fifo_wake_thread(void* arg) {
  // Always send a wake-up call; even if we failed to go to sleep.
  wake_args_t* wake = static_cast<wake_args_t*>(arg);
  auto cleanup = fit::defer([&] { ramdisk_wake(wake->ramdisk_client); });

  // Wait for the start-up signal
  {
    zx_status_t status = sync_completion_wait_deadline(&wake->start, wake->deadline);
    sync_completion_reset(&wake->start);
    if (status != ZX_OK) {
      return status;
    }
  }

  // Loop until timeout, |wake_after| txns received, or error getting counts
  ramdisk_block_write_counts_t counts;
  do {
    zx::nanosleep(zx::deadline_after(zx::msec(100)));
    if (wake->deadline < zx_clock_get_monotonic()) {
      return ZX_ERR_TIMED_OUT;
    }
    if (zx_status_t status = ramdisk_get_block_counts(wake->ramdisk_client, &counts);
        status != ZX_OK) {
      return status;
    }
  } while (counts.received < wake->after);
  return ZX_OK;
}

class RamdiskTestWithClient : public testing::Test {
 public:
  void SetUp() override {
    // Set up the initial handshake connection with the ramdisk
    ASSERT_NO_FATAL_FAILURE(RamdiskTest::Create(zx_system_get_page_size(), 512, &ramdisk_));

    fidl::UnownedClientEnd block_interface = ramdisk_->block_interface();

    zx::result client_ptr = CreateSession(block_interface);
    ASSERT_TRUE(client_ptr.is_ok()) << client_ptr.status_string();
    client_ = std::move(client_ptr.value());

    // Create an arbitrary VMO, fill it with some stuff.
    ASSERT_EQ(ZX_OK,
              mapping_.CreateAndMap(vmo_size_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr, &vmo_));

    buf_ = std::make_unique<uint8_t[]>(vmo_size_);
    fill_random(buf_.get(), vmo_size_);

    ASSERT_EQ(vmo_.write(buf_.get(), 0, vmo_size_), ZX_OK);

    // Send a handle to the vmo to the block device, get a vmoid which identifies it
    zx::result vmoid = client_->RegisterVmo(vmo_);
    ASSERT_TRUE(vmoid.is_ok()) << vmoid.status_string();
    vmoid_.id = vmoid.value().TakeId();
  }

  RamdiskTestWithClient() : vmo_size_(zx_system_get_page_size() * 16) {}

 protected:
  std::unique_ptr<RamdiskTest> ramdisk_;
  std::unique_ptr<block_client::Client> client_;
  std::unique_ptr<uint8_t[]> buf_;
  zx::vmo vmo_;
  fzl::VmoMapper mapping_;
  fuchsia_hardware_block::wire::VmoId vmoid_;
  const uint64_t vmo_size_;
};

TEST_F(RamdiskTestWithClient, RamdiskTestFifoSleepDeferred) {
  // Create a bunch of requests, some of which are guaranteed to block.
  block_fifo_request_t requests[16];
  for (size_t i = 0; i < std::size(requests); ++i) {
    requests[i].group = 0;
    requests[i].vmoid = vmoid_.id;
    requests[i].opcode = BLOCK_OP_WRITE;
    requests[i].length = 1;
    requests[i].vmo_offset = i;
    requests[i].dev_offset = i;
  }

  // Sleep and wake parameters
  uint32_t flags =
      static_cast<uint32_t>(fuchsia_hardware_ramdisk::wire::RamdiskFlag::kResumeOnWake);
  thrd_t thread;
  wake_args_t wake;
  wake.ramdisk_client = ramdisk_->ramdisk_client();
  wake.after = std::size(requests);
  sync_completion_reset(&wake.start);
  wake.deadline = zx_deadline_after(ZX_SEC(3));
  uint64_t blks_before_sleep = 1;
  int res;

  // Send enough requests to put the ramdisk to sleep and then be awoken wake thread. The ordering
  // below matters!  See the comment on |ramdisk_wake_thread| for details.
  ASSERT_EQ(thrd_create(&thread, fifo_wake_thread, &wake), thrd_success);
  ASSERT_EQ(ramdisk_set_flags(ramdisk_->ramdisk_client(), flags), ZX_OK);
  ASSERT_EQ(ramdisk_sleep_after(ramdisk_->ramdisk_client(), blks_before_sleep), ZX_OK);
  sync_completion_signal(&wake.start);
  ASSERT_EQ(client_->Transaction(requests, std::size(requests)), ZX_OK);
  ASSERT_EQ(thrd_join(thread, &res), thrd_success);

  // Check that the wake thread succeeded.
  ASSERT_EQ(res, 0) << "Background thread failed";

  for (auto& request : requests) {
    request.opcode = BLOCK_OP_READ;
  }

  // Read data we wrote to disk back into the VMO.
  ASSERT_EQ(client_->Transaction(requests, std::size(requests)), ZX_OK);

  // Verify that the contents of the vmo match the buffer.
  ASSERT_EQ(memcmp(mapping_.start(), buf_.get(), vmo_size_), 0);

  // Now send 1 transaction with the full length of the VMO.
  requests[0].opcode = BLOCK_OP_WRITE;
  requests[0].length = 16;
  requests[0].vmo_offset = 0;
  requests[0].dev_offset = 0;

  // Restart the wake thread and put ramdisk to sleep again.
  wake.after = 1;
  sync_completion_reset(&wake.start);
  ASSERT_EQ(thrd_create(&thread, fifo_wake_thread, &wake), thrd_success);
  ASSERT_EQ(ramdisk_sleep_after(ramdisk_->ramdisk_client(), blks_before_sleep), ZX_OK);
  sync_completion_signal(&wake.start);
  ASSERT_EQ(client_->Transaction(requests, 1), ZX_OK);
  ASSERT_EQ(thrd_join(thread, &res), thrd_success);

  // Check the wake thread succeeded, and that the contents of the ramdisk match the buffer.
  ASSERT_EQ(res, 0) << "Background thread failed";
  requests[0].opcode = BLOCK_OP_READ;
  ASSERT_EQ(client_->Transaction(requests, 1), ZX_OK);
  ASSERT_EQ(memcmp(mapping_.start(), buf_.get(), vmo_size_), 0);

  // Check that we can do I/O normally again.
  requests[0].opcode = BLOCK_OP_WRITE;
  ASSERT_EQ(client_->Transaction(requests, 1), ZX_OK);
}

TEST(RamdiskTests, RamdiskCreateAt) {
  fbl::unique_fd devfs_fd(open("/dev", O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(devfs_fd);
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create_at(devfs_fd.get(), zx_system_get_page_size() / 2, 512, &ramdisk), ZX_OK);

  ASSERT_EQ(device_watcher::RecursiveWaitForFile(
                (std::string("/dev/") + ramdisk_get_path(ramdisk)).c_str())
                .status_value(),
            ZX_OK);
  ramdisk_destroy(ramdisk);
}

TEST(RamdiskTests, RamdiskCreateAtGuid) {
  fbl::unique_fd devfs_fd(open("/dev", O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(devfs_fd);

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create_at_with_guid(devfs_fd.get(), zx_system_get_page_size() / 2, 512, kGuid,
                                        sizeof(kGuid), &ramdisk),
            ZX_OK);

  ASSERT_EQ(device_watcher::RecursiveWaitForFile(
                (std::string("/dev/") + ramdisk_get_path(ramdisk)).c_str())
                .status_value(),
            ZX_OK);
  ramdisk_destroy(ramdisk);
}

TEST(RamdiskTests, RamdiskCreateAtVmo) {
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(256 * zx_system_get_page_size(), 0, &vmo), ZX_OK);

  fbl::unique_fd devfs_fd(open("/dev", O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(devfs_fd);
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create_at_from_vmo(devfs_fd.get(), vmo.release(), &ramdisk), ZX_OK);

  ASSERT_EQ(device_watcher::RecursiveWaitForFile(
                (std::string("/dev/") + ramdisk_get_path(ramdisk)).c_str())
                .status_value(),
            ZX_OK);
  ramdisk_destroy(ramdisk);
}

TEST_F(RamdiskTestWithClient, DiscardOnWake) {
  ASSERT_EQ(
      ramdisk_set_flags(ramdisk_->ramdisk_client(),
                        static_cast<uint32_t>(
                            fuchsia_hardware_ramdisk::wire::RamdiskFlag::kDiscardNotFlushedOnWake)),
      ZX_OK);
  ASSERT_EQ(ramdisk_sleep_after(ramdisk_->ramdisk_client(), 100), ZX_OK);

  block_fifo_request_t requests[5];
  for (size_t i = 0; i < std::size(requests); ++i) {
    if (i == 2) {
      // Insert a flush midway through.
      requests[i].opcode = BLOCK_OP_FLUSH;
    } else {
      requests[i].group = 0;
      requests[i].vmoid = vmoid_.id;
      requests[i].opcode = BLOCK_OP_WRITE;
      requests[i].length = 1;
      requests[i].vmo_offset = i;
      requests[i].dev_offset = i;
    }
  }
  ASSERT_EQ(client_->Transaction(requests, std::size(requests)), ZX_OK);

  // Wake the device and it should discard the last two blocks.
  ASSERT_EQ(ramdisk_wake(ramdisk_->ramdisk_client()), ZX_OK);

  memset(mapping_.start(), 0, vmo_size_);

  // Read back all the blocks. The extra flush shouldn't matter.
  for (size_t i = 0; i < std::size(requests); ++i) {
    if (i != 2)
      requests[i].opcode = BLOCK_OP_READ;
  }
  ASSERT_EQ(client_->Transaction(requests, std::size(requests)), ZX_OK);

  // Verify that the first two blocks went through but the last two did not.
  for (size_t i = 0; i < std::size(requests); ++i) {
    if (i < 2) {
      EXPECT_EQ(memcmp(static_cast<uint8_t*>(mapping_.start()) + zx_system_get_page_size() * i,
                       &buf_[zx_system_get_page_size() * i], zx_system_get_page_size()),
                0);
    } else if (i > 2) {
      EXPECT_NE(memcmp(static_cast<uint8_t*>(mapping_.start()) + zx_system_get_page_size() * i,
                       &buf_[zx_system_get_page_size() * i], zx_system_get_page_size()),
                0)
          << i;
    }
  }
}

TEST_F(RamdiskTestWithClient, DiscardRandomOnWake) {
  ASSERT_EQ(
      ramdisk_set_flags(ramdisk_->ramdisk_client(),
                        static_cast<uint32_t>(
                            fuchsia_hardware_ramdisk::wire::RamdiskFlag::kDiscardNotFlushedOnWake |
                            fuchsia_hardware_ramdisk::wire::RamdiskFlag::kDiscardRandom)),
      ZX_OK);

  int found = 0;
  do {
    ASSERT_EQ(ramdisk_sleep_after(ramdisk_->ramdisk_client(), 100), ZX_OK);
    fill_random(buf_.get(), vmo_size_);
    ASSERT_EQ(vmo_.write(buf_.get(), 0, vmo_size_), ZX_OK);

    block_fifo_request_t requests[5];
    for (size_t i = 0; i < std::size(requests); ++i) {
      if (i == 2) {
        // Insert a flush midway through.
        requests[i].opcode = BLOCK_OP_FLUSH;
      } else {
        requests[i].group = 0;
        requests[i].vmoid = vmoid_.id;
        requests[i].opcode = BLOCK_OP_WRITE;
        requests[i].length = 1;
        requests[i].vmo_offset = i;
        requests[i].dev_offset = i;
      }
    }
    ASSERT_EQ(client_->Transaction(requests, std::size(requests)), ZX_OK);

    // Wake the device and it should discard the last two blocks.
    ASSERT_EQ(ramdisk_wake(ramdisk_->ramdisk_client()), ZX_OK);

    memset(mapping_.start(), 0, vmo_size_);

    // Read back all the blocks. The extra flush shouldn't matter.
    for (size_t i = 0; i < std::size(requests); ++i) {
      if (i != 2)
        requests[i].opcode = BLOCK_OP_READ;
    }
    ASSERT_EQ(client_->Transaction(requests, std::size(requests)), ZX_OK);

    // Verify that the first two blocks went through but the last two might not.
    int different = 0;
    for (size_t i = 0; i < std::size(requests); ++i) {
      if (i < 2) {
        EXPECT_EQ(memcmp(static_cast<uint8_t*>(mapping_.start()) + zx_system_get_page_size() * i,
                         &buf_[zx_system_get_page_size() * i], zx_system_get_page_size()),
                  0);
      } else if (i > 2) {
        if (memcmp(static_cast<uint8_t*>(mapping_.start()) + zx_system_get_page_size() * i,
                   &buf_[zx_system_get_page_size() * i], zx_system_get_page_size()) != 0) {
          different |= 1 << (i - 3);
        }
      }
    }

    // There are 4 different combinations we expect and we keep looping until we've seen all of
    // them.
    found |= 1 << different;
  } while (found != 0xf);
}

}  // namespace
}  // namespace ramdisk
