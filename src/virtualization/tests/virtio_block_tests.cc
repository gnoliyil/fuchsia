// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/syslog/cpp/macros.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <iterator>

#include <fbl/unique_fd.h>
#include <gmock/gmock.h>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/virtualization/bin/vmm/device/block.h"
#include "src/virtualization/bin/vmm/device/qcow_test_data.h"
#include "src/virtualization/tests/lib/guest_test.h"

using namespace qcow_test_data;
using testing::HasSubstr;

static constexpr char kVirtioBlockUtil[] = "virtio_block_test_util";
static constexpr uint32_t kVirtioBlockCount = 32;
static constexpr uint32_t kVirtioQcowBlockCount = kDefaultHeaderV2.size / kBlockSectorSize;

// We test reading and writing at the first and last offsets of the block device, and an arbitrary
// offset in between.
static constexpr off_t kMiddleOffset = 17;
static constexpr off_t kBlockTestOffsets[] = {0, kMiddleOffset, kVirtioBlockCount - 1};
static constexpr off_t kQcowBlockTestOffsets[] = {0, kMiddleOffset,
                                                  (kClusterSize / kBlockSectorSize) - 1};

// Ensure that the offset we chose is less than the last offset.
static_assert(kMiddleOffset < kVirtioBlockCount - 1, "Virtio raw test offset is too large.");
static_assert(kMiddleOffset < (kClusterSize / kBlockSectorSize) - 1,
              "Virtio qcow test offset is too large.");

static constexpr off_t kQcowUnmappedClusterOffset = ClusterOffset(2) / kBlockSectorSize;

static zx_status_t write_raw_file(int fd) {
  int ret = ftruncate(fd, kVirtioBlockCount * kBlockSectorSize);
  if (ret != 0) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

template <typename T>
static bool write_at(int fd, const T* ptr, off_t off) {
  ssize_t written = pwrite(fd, ptr, sizeof(T), off);
  return written == static_cast<ssize_t>(sizeof(T));
}

// Writes an array of T values at the current file location.
template <typename T>
static bool write_at(int fd, const T* ptr, size_t len, off_t off) {
  ssize_t written = pwrite(fd, ptr, len * sizeof(T), off);
  return written == static_cast<ssize_t>(len * sizeof(T));
}

static zx_status_t write_qcow_file(int fd) {
  QcowHeader header = kDefaultHeaderV2.HostToBigEndian();
  bool write_success = write_at(fd, &header, 0);
  if (!write_success) {
    return ZX_ERR_IO;
  }

  // Convert L1 entries to big-endian
  uint64_t be_table[std::size(kL2TableClusterOffsets)];
  for (size_t i = 0; i < std::size(kL2TableClusterOffsets); ++i) {
    be_table[i] = HostToBigEndianTraits::Convert(kL2TableClusterOffsets[i]);
  }

  // Write L1 table.
  write_success =
      write_at(fd, be_table, std::size(kL2TableClusterOffsets), kDefaultHeaderV2.l1_table_offset);
  if (!write_success) {
    return ZX_ERR_IO;
  }

  // Initialize empty L2 tables.
  for (unsigned long kL2TableClusterOffset : kL2TableClusterOffsets) {
    write_success = write_at(fd, kZeroCluster, sizeof(kZeroCluster), kL2TableClusterOffset);
    if (!write_success) {
      return ZX_ERR_IO;
    }
  }

  // Write L2 entry
  uint64_t l2_offset = kL2TableClusterOffsets[0];
  uint64_t data_cluster_offset = ClusterOffset(kFirstDataCluster);
  uint64_t l2_entry = HostToBigEndianTraits::Convert(data_cluster_offset);
  write_success = write_at(fd, &l2_entry, l2_offset);
  if (!write_success) {
    return ZX_ERR_IO;
  }

  // Write data to cluster.
  uint8_t cluster_data[kClusterSize];
  memset(cluster_data, 0xab, sizeof(cluster_data));
  write_success = write_at(fd, cluster_data, kClusterSize, data_cluster_offset);
  if (!write_success) {
    return ZX_ERR_IO;
  }

  return ZX_OK;
}

enum class Format {
  kFile,
  kQcow,
};

struct TestDevice {
  std::string id;
  Format format;
  fuchsia::virtualization::BlockMode mode;
  uint8_t pci_bus;
  uint8_t pci_device;
  std::string file_path = "/tmp/guest-test.XXXXXX";
};

static zx_status_t create_test_device(TestDevice* test_device,
                                      fuchsia::virtualization::BlockSpec* block_spec) {
  fbl::unique_fd fd(mkstemp(test_device->file_path.data()));
  if (!fd) {
    FX_LOGS(ERROR) << "Failed to create temporary file";
    return ZX_ERR_IO;
  }

  switch (test_device->format) {
    case Format::kFile:
      if (zx_status_t status = write_raw_file(fd.get()); status != ZX_OK) {
        return status;
      }
      break;
    case Format::kQcow:
      if (zx_status_t status = write_qcow_file(fd.get()); status != ZX_OK) {
        return status;
      }
      break;
  }

  zx::channel channel;
  zx_status_t status = fdio_get_service_handle(fd.release(), channel.reset_and_get_address());
  if (status != ZX_OK) {
    return status;
  }

  switch (test_device->format) {
    case Format::kFile:
      block_spec->format = fuchsia::virtualization::BlockFormat::WithFile(
          fidl::InterfaceHandle<fuchsia::io::File>(std::move(channel)));
      break;
    case Format::kQcow:
      block_spec->format = fuchsia::virtualization::BlockFormat::WithQcow(std::move(channel));
      break;
  }

  block_spec->id = test_device->id;
  block_spec->mode = test_device->mode;

  return ZX_OK;
}

class VirtioBlockTestGuest {
 public:
  zx_status_t CreateBlockDevices(uint8_t device_count, fuchsia::virtualization::GuestConfig* cfg) {
    zx_status_t status;

    test_devices_.push_back({
        .id = "raw_read_write",
        .format = Format::kFile,
        .mode = fuchsia::virtualization::BlockMode::READ_WRITE,
        .pci_bus = 0,
        .pci_device = device_count++,
    });
    test_devices_.push_back({
        .id = "raw_read_only",
        .format = Format::kFile,
        .mode = fuchsia::virtualization::BlockMode::READ_ONLY,
        .pci_bus = 0,
        .pci_device = device_count++,
    });
    test_devices_.push_back({
        .id = "raw_volatile_write",
        .format = Format::kFile,
        .mode = fuchsia::virtualization::BlockMode::VOLATILE_WRITE,
        .pci_bus = 0,
        .pci_device = device_count++,
    });
    test_devices_.push_back({
        .id = "qcow_read_only",
        .format = Format::kQcow,
        .mode = fuchsia::virtualization::BlockMode::READ_ONLY,
        .pci_bus = 0,
        .pci_device = device_count++,
    });
    test_devices_.push_back({
        .id = "qcow_volatile_write",
        .format = Format::kQcow,
        .mode = fuchsia::virtualization::BlockMode::VOLATILE_WRITE,
        .pci_bus = 0,
        .pci_device = device_count++,
    });

    std::vector<fuchsia::virtualization::BlockSpec> block_specs;
    for (auto& test_device : test_devices_) {
      fuchsia::virtualization::BlockSpec block_spec;
      status = create_test_device(&test_device, &block_spec);
      if (status != ZX_OK) {
        return status;
      }
      block_specs.push_back(std::move(block_spec));
    }

    cfg->set_block_devices(std::move(block_specs));

    return ZX_OK;
  }

  const std::vector<TestDevice>& TestDevices() const { return test_devices_; }

 private:
  std::vector<TestDevice> test_devices_;
};

class VirtioBlockZirconGuest : public ZirconEnclosedGuest, public VirtioBlockTestGuest {
 public:
  VirtioBlockZirconGuest(async_dispatcher_t* dispatcher, RunLoopUntilFunc run_loop_until)
      : ZirconEnclosedGuest(dispatcher, std::move(run_loop_until)) {}

  zx_status_t BuildLaunchInfo(GuestLaunchInfo* launch_info) override {
    zx_status_t status = ZirconEnclosedGuest::BuildLaunchInfo(launch_info);
    if (status != ZX_OK) {
      return status;
    }

    // Disable other virtio devices to ensure there's enough space on the PCI
    // bus, and to simplify slot assignment.
    launch_info->config.set_default_net(false);
    launch_info->config.set_virtio_balloon(false);
    launch_info->config.set_virtio_gpu(false);
    launch_info->config.set_virtio_rng(false);
    launch_info->config.set_virtio_vsock(false);

    // Device count starts at 2: root device, block-0, then the test devices.
    return CreateBlockDevices(/*device_count=*/2, &launch_info->config);
  }
};

class VirtioBlockDebianGuest : public DebianEnclosedGuest, public VirtioBlockTestGuest {
 public:
  VirtioBlockDebianGuest(async_dispatcher_t* dispatcher, RunLoopUntilFunc run_loop_until)
      : DebianEnclosedGuest(dispatcher, std::move(run_loop_until)) {}

  zx_status_t BuildLaunchInfo(GuestLaunchInfo* launch_info) override {
    zx_status_t status = DebianEnclosedGuest::BuildLaunchInfo(launch_info);
    if (status != ZX_OK) {
      return status;
    }

    // Disable other virtio devices to ensure there's enough space on the PCI
    // bus, and to simplify slot assignment.
    launch_info->config.set_default_net(false);
    launch_info->config.set_virtio_balloon(false);
    launch_info->config.set_virtio_gpu(false);
    launch_info->config.set_virtio_rng(false);
    launch_info->config.set_virtio_vsock(false);

    // Device count starts at 4: root device, block-0, block-1, block-2, then
    // the test devices.
    return CreateBlockDevices(/*device_count=*/4, &launch_info->config);
  }
};

template <class T>
class VirtioBlockGuestTest : public GuestTest<T> {
 public:
  const std::vector<TestDevice>& TestDevices() { return this->GetEnclosedGuest().TestDevices(); }
};

using GuestTypes = ::testing::Types<VirtioBlockZirconGuest, VirtioBlockDebianGuest>;

TYPED_TEST_SUITE(VirtioBlockGuestTest, GuestTypes, GuestTestNameGenerator);

TYPED_TEST(VirtioBlockGuestTest, CheckSize) {
  for (const auto& device : this->TestDevices()) {
    FX_LOGS(INFO) << "Device: " << device.id;
    size_t expected_size = 0;
    switch (device.format) {
      case Format::kFile:
        expected_size = kVirtioBlockCount;
        break;
      case Format::kQcow:
        expected_size = kVirtioQcowBlockCount;
        break;
    }
    ASSERT_GT(expected_size, 0u);

    std::string result;
    EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                            {
                                fxl::StringPrintf("%lu", kBlockSectorSize),
                                fxl::StringPrintf("%u", device.pci_bus),
                                fxl::StringPrintf("%u", device.pci_device),
                                "check",
                                fxl::StringPrintf("%d", static_cast<int>(expected_size)),
                            },
                            &result),
              ZX_OK);
    EXPECT_THAT(result, HasSubstr("PASS"));
  }
}

TYPED_TEST(VirtioBlockGuestTest, ReadRaw) {
  for (const auto& device : this->TestDevices()) {
    if (device.format != Format::kFile) {
      continue;
    }
    FX_LOGS(INFO) << "Device: " << device.id;

    fbl::unique_fd fd(open(device.file_path.c_str(), O_RDWR));
    ASSERT_TRUE(fd);

    uint8_t data[kBlockSectorSize];
    memset(data, 0xab, kBlockSectorSize);
    for (off_t offset : kBlockTestOffsets) {
      ASSERT_EQ(pwrite(fd.get(), &data, kBlockSectorSize, offset * kBlockSectorSize),
                static_cast<ssize_t>(kBlockSectorSize));
      std::string result;
      EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                              {
                                  fxl::StringPrintf("%lu", kBlockSectorSize),
                                  fxl::StringPrintf("%u", device.pci_bus),
                                  fxl::StringPrintf("%u", device.pci_device),
                                  "read",
                                  fxl::StringPrintf("%d", static_cast<int>(offset)),
                                  fxl::StringPrintf("%d", 0xab),
                              },
                              &result),
                ZX_OK);
      EXPECT_THAT(result, HasSubstr("PASS"));
    }
  }
}

TYPED_TEST(VirtioBlockGuestTest, WriteRaw) {
  for (const auto& device : this->TestDevices()) {
    if (device.format != Format::kFile) {
      continue;
    }
    FX_LOGS(INFO) << "Device: " << device.id;

    fbl::unique_fd fd(open(device.file_path.c_str(), O_RDWR));
    ASSERT_TRUE(fd);

    uint8_t data[kBlockSectorSize];
    memset(data, 0, kBlockSectorSize);
    for (off_t offset : kBlockTestOffsets) {
      // Write the block to zero.
      ASSERT_EQ(pwrite(fd.get(), &data, kBlockSectorSize, offset * kBlockSectorSize),
                static_cast<ssize_t>(kBlockSectorSize));

      // Tell the guest to write bytes to the block.
      std::string result;
      EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                              {
                                  fxl::StringPrintf("%lu", kBlockSectorSize),
                                  fxl::StringPrintf("%u", device.pci_bus),
                                  fxl::StringPrintf("%u", device.pci_device),
                                  "write",
                                  fxl::StringPrintf("%d", static_cast<int>(offset)),
                                  fxl::StringPrintf("%d", 0xab),
                              },
                              &result),
                ZX_OK);

      if (device.mode == fuchsia::virtualization::BlockMode::READ_ONLY) {
        if (this->GetGuestKernel() == GuestKernel::LINUX) {
          EXPECT_THAT(result, HasSubstr("PermissionDenied"));
        } else {
          EXPECT_THAT(result, HasSubstr("Error: IO"));
        }
      } else {
        EXPECT_THAT(result, HasSubstr("PASS"));
      }

      int expected_guest_read, expected_host_read;
      switch (device.mode) {
        case fuchsia::virtualization::BlockMode::READ_ONLY:
          expected_guest_read = 0;
          expected_host_read = 0;
          break;
        case fuchsia::virtualization::BlockMode::READ_WRITE:
          expected_guest_read = 0xab;
          expected_host_read = 0xab;
          break;
        case fuchsia::virtualization::BlockMode::VOLATILE_WRITE:
          expected_guest_read = 0xab;
          expected_host_read = 0;
          break;
      }

      // Check the value when read from the guest.
      EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                              {
                                  fxl::StringPrintf("%lu", kBlockSectorSize),
                                  fxl::StringPrintf("%u", device.pci_bus),
                                  fxl::StringPrintf("%u", device.pci_device),
                                  "read",
                                  fxl::StringPrintf("%d", static_cast<int>(offset)),
                                  fxl::StringPrintf("%d", expected_guest_read),
                              },
                              &result),
                ZX_OK);
      EXPECT_THAT(result, HasSubstr("PASS"));

      // Check the value when read from the host file.
      ASSERT_EQ(pread(fd.get(), &data, kBlockSectorSize, offset * kBlockSectorSize),
                static_cast<ssize_t>(kBlockSectorSize));
      for (unsigned char i : data) {
        EXPECT_EQ(i, expected_host_read);
      }
    }
  }
}

TYPED_TEST(VirtioBlockGuestTest, ReadMappedCluster) {
  for (const auto& device : this->TestDevices()) {
    if (device.format != Format::kQcow) {
      continue;
    }
    FX_LOGS(INFO) << "Device: " << device.id;

    for (off_t offset : kQcowBlockTestOffsets) {
      std::string result;
      EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                              {
                                  fxl::StringPrintf("%lu", kBlockSectorSize),
                                  fxl::StringPrintf("%u", device.pci_bus),
                                  fxl::StringPrintf("%u", device.pci_device),
                                  "read",
                                  fxl::StringPrintf("%d", static_cast<int>(offset)),
                                  fxl::StringPrintf("%d", 0xab),
                              },
                              &result),
                ZX_OK);
      EXPECT_THAT(result, HasSubstr("PASS"));
    }
  }
}

TYPED_TEST(VirtioBlockGuestTest, ReadUnmappedCluster) {
  for (const auto& device : this->TestDevices()) {
    if (device.format != Format::kQcow) {
      continue;
    }
    FX_LOGS(INFO) << "Device: " << device.id;

    std::string result;
    EXPECT_EQ(
        this->RunUtil(kVirtioBlockUtil,
                      {
                          fxl::StringPrintf("%lu", kBlockSectorSize),
                          fxl::StringPrintf("%u", device.pci_bus),
                          fxl::StringPrintf("%u", device.pci_device),
                          "read",
                          fxl::StringPrintf("%d", static_cast<int>(kQcowUnmappedClusterOffset)),
                          fxl::StringPrintf("%d", 0),
                      },
                      &result),
        ZX_OK);
    EXPECT_THAT(result, HasSubstr("PASS"));
  }
}

TYPED_TEST(VirtioBlockGuestTest, WriteQcow) {
  for (const auto& device : this->TestDevices()) {
    if (device.format != Format::kQcow) {
      continue;
    }
    FX_LOGS(INFO) << "Device: " << device.id;

    for (off_t offset : kQcowBlockTestOffsets) {
      std::string result;
      EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                              {
                                  fxl::StringPrintf("%lu", kBlockSectorSize),
                                  fxl::StringPrintf("%u", device.pci_bus),
                                  fxl::StringPrintf("%u", device.pci_device),
                                  "write",
                                  fxl::StringPrintf("%d", static_cast<int>(offset)),
                                  fxl::StringPrintf("%d", 0xba),
                              },
                              &result),
                ZX_OK);

      if (device.mode == fuchsia::virtualization::BlockMode::READ_ONLY) {
        if (this->GetGuestKernel() == GuestKernel::LINUX) {
          EXPECT_THAT(result, HasSubstr("PermissionDenied"));
        } else {
          EXPECT_THAT(result, HasSubstr("Error: IO"));
        }
      } else {
        EXPECT_THAT(result, HasSubstr("PASS"));
      }

      int expected_read;
      switch (device.mode) {
        case fuchsia::virtualization::BlockMode::READ_ONLY:
          expected_read = 0xab;
          break;
        case fuchsia::virtualization::BlockMode::VOLATILE_WRITE:
          expected_read = 0xba;
          break;
        case fuchsia::virtualization::BlockMode::READ_WRITE:
          // READ_WRITE not supported for QCOW.
          expected_read = -1;
          break;
      }

      EXPECT_EQ(this->RunUtil(kVirtioBlockUtil,
                              {
                                  fxl::StringPrintf("%lu", kBlockSectorSize),
                                  fxl::StringPrintf("%u", device.pci_bus),
                                  fxl::StringPrintf("%u", device.pci_device),
                                  "read",
                                  fxl::StringPrintf("%d", static_cast<int>(offset)),
                                  fxl::StringPrintf("%d", expected_read),
                              },
                              &result),
                ZX_OK);
      EXPECT_THAT(result, HasSubstr("PASS"));
    }
  }
}
