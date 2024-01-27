// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SECURITY_LIB_ZXCRYPT_TESTS_TEST_DEVICE_H_
#define SRC_SECURITY_LIB_ZXCRYPT_TESTS_TEST_DEVICE_H_

#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <fidl/fuchsia.hardware.block/cpp/wire.h>
#include <lib/component/incoming/cpp/clone.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <stddef.h>
#include <stdint.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <memory>
#include <optional>

#include <fbl/macros.h>
#include <fbl/mutex.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramdisk.h>

#include "src/lib/storage/block_client/cpp/client.h"
#include "src/lib/storage/block_client/cpp/remote_block_device.h"
#include "src/security/lib/fcrypto/secret.h"
#include "src/security/lib/zxcrypt/client.h"
#include "src/security/lib/zxcrypt/fdio-volume.h"
#include "src/storage/fvm/format.h"

// Value-parameterized tests: Consumers of this file can define an 'EACH_PARAM' macro as follows:
//   #define EACH_PARAM(OP, Test)
//   OP(Test, Class, Param1)
//   OP(Test, Class, Param2)
//   ...
//   OP(Test, Class, ParamN)
// where |Param1| corresponds to an enum constant |Class::kParam1|, etc.
//
// Consumers can then use the following macros to automatically define and run tests for each
// parameter:
//   void TestSomething(Param param) {
//       ...
//   }
//   DEFINE_EACH(FooTest, TestSomething)
#define DEFINE_TEST_PARAM(TestSuite, Test, Class, Param) \
  TEST(TestSuite, Test##_##Param) { return Test(Class::k##Param); }
#define DEFINE_EACH(TestSuite, Test) EACH_PARAM(DEFINE_TEST_PARAM, TestSuite, Test)

#define DEFINE_EACH_DEVICE(TestSuite, Test)                                              \
  void Test##Raw(Volume::Version version) { return Test(version, false /* not FVM */); } \
  DEFINE_EACH(TestSuite, Test##Raw)                                                      \
  void Test##Fvm(Volume::Version version) { return Test(version, true /* FVM */); }      \
  DEFINE_EACH(TestSuite, Test##Fvm)

namespace zxcrypt {
namespace testing {

// Default disk geometry to use when testing device block related code.
const uint32_t kBlockCount = 64;
const uint32_t kBlockSize = 512;
const size_t kDeviceSize = kBlockCount * kBlockSize;
const uint32_t kSliceCount = kDeviceSize / fvm::kBlockSize;

// |zxcrypt::testing::Utils| is a collection of functions designed to make the zxcrypt
// unit test setup and tear down easier.
class TestDevice final {
 public:
  explicit TestDevice();
  ~TestDevice();
  DISALLOW_COPY_ASSIGN_AND_MOVE(TestDevice);

  // ACCESSORS

  // Returns the size of the zxcrypt volume.
  size_t size() const { return block_count_ * block_size_; }

  const fbl::unique_fd& parent() const { return parent_; }

  const fbl::unique_fd& devfs_root() const { return devmgr_.devfs_root(); }

  const fbl::unique_fd& zxcrypt() const { return zxcrypt_; }

  // Returns a connection to the parent device.
  fidl::UnownedClientEnd<fuchsia_device::Controller> parent_controller() const {
    fdio_cpp::UnownedFdioCaller caller(parent_);
    return caller.borrow_as<fuchsia_device::Controller>();
  }

  // Returns a connection to the parent device.
  fidl::UnownedClientEnd<fuchsia_hardware_block::Block> parent_block() const {
    fdio_cpp::UnownedFdioCaller caller(parent_);
    return caller.borrow_as<fuchsia_hardware_block::Block>();
  }

  // Returns a connection to the parent device.
  fidl::UnownedClientEnd<fuchsia_hardware_block_volume::Volume> parent_volume() const {
    fdio_cpp::UnownedFdioCaller caller(parent_);
    return caller.borrow_as<fuchsia_hardware_block_volume::Volume>();
  }

  // Returns a connection to the zxcrypt device.
  fidl::UnownedClientEnd<fuchsia_hardware_block::Block> zxcrypt_block() const {
    fdio_cpp::UnownedFdioCaller caller(zxcrypt_);
    return caller.borrow_as<fuchsia_hardware_block::Block>();
  }

  // Returns a connection to the zxcrypt device.
  fidl::UnownedClientEnd<fuchsia_hardware_block_volume::Volume> zxcrypt_volume() const {
    fdio_cpp::UnownedFdioCaller caller(zxcrypt_);
    return caller.borrow_as<fuchsia_hardware_block_volume::Volume>();
  }

  // Returns the block size of the zxcrypt device.
  size_t block_size() const { return block_size_; }

  // Returns the block size of the zxcrypt device.
  size_t block_count() const { return block_count_; }

  // Returns space reserved for metadata
  zx::result<size_t> reserved_blocks() const {
    zx::result channel = component::Clone(parent_block(), component::AssumeProtocolComposesNode);
    if (channel.is_error()) {
      return channel.take_error();
    }
    std::unique_ptr<FdioVolume> volume;
    if (zx_status_t status = FdioVolume::Unlock(std::move(channel.value()), key_, 0, &volume);
        status) {
      return zx::error(status);
    }
    return zx::ok(volume->reserved_blocks());
  }
  zx::result<size_t> reserved_slices() const {
    zx::result channel = component::Clone(parent_block(), component::AssumeProtocolComposesNode);
    if (channel.is_error()) {
      return channel.take_error();
    }
    std::unique_ptr<FdioVolume> volume;
    if (zx_status_t status = FdioVolume::Unlock(std::move(channel.value()), key_, 0, &volume);
        status) {
      return zx::error(status);
    }
    return zx::ok(volume->reserved_slices());
  }

  // Returns a reference to the root key generated for this device.
  const crypto::Secret& key() const { return key_; }

  // API WRAPPERS

  zx_status_t SingleReadBytes(zx_off_t src_offset, size_t len, size_t dst_offset) {
    return block_client::SingleReadBytes(zxcrypt_block(), as_read_.get() + src_offset, len,
                                         dst_offset);
  }

  zx_status_t SingleWriteBytes(zx_off_t src_offset, size_t len, size_t dst_offset) {
    return block_client::SingleWriteBytes(zxcrypt_block(), to_write_.get() + src_offset, len,
                                          dst_offset);
  }

  // These methods mirror the syscall API, except that the VMO and buffers are provided
  // automatically. |off| and |len| are in bytes.
  zx_status_t vmo_read(zx_off_t off, size_t len) { return vmo_.read(as_read_.get() + off, 0, len); }
  zx_status_t vmo_write(uint64_t off, uint64_t len) {
    return vmo_.write(to_write_.get() + off, 0, len);
  }

  // Sends a request over the block fifo to read or write the blocks given by |off| and |len|,
  // according to the given |opcode|.  The data sent or received can be accessed using |vmo_write|
  // or |vmo_read|, respectively.  |off| and |len| are in blocks.
  zx_status_t block_fifo_txn(uint8_t opcode, uint64_t off, uint64_t len) {
    req_.command = {.opcode = opcode, .flags = 0};
    req_.length = static_cast<uint32_t>(len);
    req_.dev_offset = off;
    req_.vmo_offset = 0;
    return client_->Transaction(&req_, 1);
  }

  // Sends |num| requests over the block fifo to read or write blocks.
  zx_status_t block_fifo_txn(block_fifo_request_t* requests, size_t num) {
    for (size_t i = 0; i < num; ++i) {
      requests[i].group = req_.group;
      requests[i].vmoid = req_.vmoid;
    }
    return client_->Transaction(requests, num);
  }

  // TEST HELPERS

  // Launches an isolated devcoordinator.  Must be called before calling
  // any other methods on TestDevice.
  void SetupDevmgr();

  // Allocates a new block device of at least |device_size| bytes grouped into blocks of
  // |block_size| bytes each.  If |fvm| is true, it will be formatted as an FVM partition with the
  // appropriates number of slices of |fvm::kBlockSize| each.  A file descriptor for the block
  // device is returned via |out_fd|.
  void Create(size_t device_size, size_t block_size, bool fvm, Volume::Version version);

  // Test helper that generates a key and creates a device according to |version| and |fvm|.  It
  // sets up the device as a zxcrypt volume and binds to it.
  void Bind(Volume::Version version, bool fvm);

  // Test helper that rebinds the ramdisk and its children.
  void Rebind();

  // Tells the underlying ramdisk to sleep until |num| transactions have been received.  If
  // |deferred| is true, the transactions will be handled on waking; else they will be failed.
  void SleepUntil(uint64_t num, bool deferred) __TA_EXCLUDES(lock_);

  // Blocks until the ramdisk is awake.
  void WakeUp() __TA_EXCLUDES(lock_);

  void Read(zx_off_t off, size_t len);
  void Write(zx_off_t off, size_t len);

  // Test helpers that perform a |lseek| and a |vmo_read| or |vmo_write| together.  |off| and
  // |len| are in blocks.  |ReadVmo| additionally checks that the data read matches what was
  // written.
  void ReadVmo(zx_off_t off, size_t len);
  void WriteVmo(zx_off_t off, size_t len);

  // Test helper that flips a (pseudo)random bit in the key at the given |slot| in the given
  // |block|. The call to |srand| in main.c guarantees the same bit will be chosen for a given
  // test iteration.
  void Corrupt(uint64_t blkno, key_slot_t slot);

 private:
  // Allocates a new ramdisk of at least |device_size| bytes arranged into blocks of |block_size|
  // bytes, and opens it.
  void CreateRamdisk(size_t device_size, size_t block_size);

  // Destroys the ramdisk, killing any active transactions
  void DestroyRamdisk();

  // Creates a ramdisk of with enough blocks of |block_size| bytes to hold both FVM metadata and
  // an FVM partition of at least |device_size| bytes.  It formats the ramdisk to be an FVM
  // device, and allocates a partition with a single slice of size fvm::kBlockSize.
  void CreateFvmPart(size_t device_size, size_t block_size);

  // Binds the FVM driver to the open ramdisk.
  void BindFvmDriver();

  // Connects the block client to the block server.
  void Connect();

  // Disconnects the block client from the block server.
  void Disconnect();

  // Thread body to wake up the underlying ramdisk.
  static int WakeThread(void* arg);

  // The isolated devmgr instance which we'll talk to in order to create the
  // underlying ramdisk.  We do this so that the system's devmgr's
  // block-watcher doesn't try to bind drivers to/mount/unseal our
  // ramdisk-backed volumes.
  driver_integration_test::IsolatedDevmgr devmgr_;

  // The ramdisk client
  ramdisk_client_t* ramdisk_ = nullptr;

  // The pathname of the FVM partition.
  char fvm_part_path_[PATH_MAX];
  // File descriptor for the (optional) underlying FVM partition.
  fbl::unique_fd parent_;
  // File descriptor for the zxcrypt volume.
  fbl::unique_fd zxcrypt_;
  // The zxcrypt volume
  std::unique_ptr<zxcrypt::VolumeManager> volume_manager_;
  // The cached block count.
  size_t block_count_ = 0;
  // The cached block size.
  size_t block_size_ = 0;
  // The root key for this device.
  crypto::Secret key_;
  // Client for the block I/O protocol to the block server. Created/destroyed on demand.
  std::unique_ptr<block_client::Client> client_;
  // Request structure used to send messages via the block I/O protocol.
  block_fifo_request_t req_;
  // VMO attached to the zxcrypt device for use with the block I/O protocol.
  zx::vmo vmo_;
  // An internal write buffer, initially filled with pseudo-random data
  std::unique_ptr<uint8_t[]> to_write_;
  // An internal write buffer,  initially filled with zeros.
  std::unique_ptr<uint8_t[]> as_read_;
  // Lock to coordinate waking thread
  fbl::Mutex lock_;
  // Thread used to manage sleeping/waking.
  thrd_t tid_ = 0;
  // It would be nice if thrd_t had a reserved invalid value...
  bool need_join_ = false;
  // The number of transactions before waking.
  uint64_t wake_after_ __TA_GUARDED(lock_) = 0;
  // Timeout before waking regardless of transactions
  zx::time wake_deadline_ __TA_GUARDED(lock_);
};

}  // namespace testing
}  // namespace zxcrypt

#endif  // SRC_SECURITY_LIB_ZXCRYPT_TESTS_TEST_DEVICE_H_
