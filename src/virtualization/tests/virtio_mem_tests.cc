// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.kernel/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>

#include "src/virtualization/tests/lib/guest_test.h"

namespace {

template <class T>
class MemGuestTest : public GuestTest<T> {
 public:
  MemGuestTest() = default;
};

constexpr uint64_t kOneMebibyte = 1024u * 1024;
constexpr uint64_t kDefaultBlockSize = 4u * kOneMebibyte;
constexpr uint64_t kDefaultMemoryRegionSize = static_cast<uint64_t>(1) * 1024u * kOneMebibyte;

// Memory block size here is the minimal DIMM size configured to 128MiB on Linux x86 and 2MiB on
// Windows
// If virtio mem start address is not aligned to 128MiB, virtio mem would spit out a warning and
// then fails to reserve the given memory range.
// 128MiB is mentioned a few times in virtio-mem patchsets
// See
// https://lore.kernel.org/kvm/c244851d-ef0d-f680-090d-e90b5be3103e@redhat.com/
// https://lists.oasis-open.org/archives/virtio-dev/202005/msg00039.html
constexpr uint64_t kLinuxMinimalDIMMSize = 128u * kOneMebibyte;

class VirtioMemTerminaGuest : public TerminaEnclosedGuest {
 public:
  explicit VirtioMemTerminaGuest(async::Loop& loop) : TerminaEnclosedGuest(loop) {}

  zx_status_t BuildLaunchInfo(GuestLaunchInfo* launch_info) override {
    zx_status_t status = TerminaEnclosedGuest::BuildLaunchInfo(launch_info);
    if (status != ZX_OK) {
      return status;
    }
    // TODO(fxbug.dev/100514): Bump up the number of PCI slots
    //
    // Not enough PCI slots, turn off a few devices
    launch_info->config.set_guest_memory(3u * 1024 * kOneMebibyte);
    launch_info->config.set_virtio_rng(false);
    launch_info->config.set_virtio_mem(true);
    launch_info->config.set_virtio_mem_block_size(kDefaultBlockSize);
    launch_info->config.set_virtio_mem_region_size(kDefaultMemoryRegionSize);
    launch_info->config.set_virtio_mem_region_alignment(kLinuxMinimalDIMMSize);
    return ZX_OK;
  }
};

class VirtioMemDebianGuest : public DebianEnclosedGuest {
 public:
  explicit VirtioMemDebianGuest(async::Loop& loop) : DebianEnclosedGuest(loop) {}

  zx_status_t BuildLaunchInfo(GuestLaunchInfo* launch_info) override {
    zx_status_t status = DebianEnclosedGuest::BuildLaunchInfo(launch_info);
    if (status != ZX_OK) {
      return status;
    }
    // TODO(fxbug.dev/100514): Bump up the number of PCI slots
    //
    // Not enough PCI slots, turn off a few devices
    launch_info->config.set_virtio_rng(false);
    launch_info->config.set_virtio_mem(true);
    launch_info->config.set_virtio_mem_block_size(kDefaultBlockSize);
    launch_info->config.set_virtio_mem_region_size(kDefaultMemoryRegionSize);
    launch_info->config.set_virtio_mem_region_alignment(kLinuxMinimalDIMMSize);
    return ZX_OK;
  }
};

// TODO(fxbug.dev/100514): Re-enable Debian after finding out why it doesn't update the total system
// memory after the successful hotplug
using GuestTypes = ::testing::Types<VirtioMemTerminaGuest>;
TYPED_TEST_SUITE(MemGuestTest, GuestTypes, GuestTestNameGenerator);

TYPED_TEST(MemGuestTest, Placeholder) {
  fuchsia::virtualization::MemControllerSyncPtr mem_controller;
  ASSERT_TRUE(this->ConnectToMem(mem_controller.NewRequest()));

  uint64_t block_size;
  uint64_t region_size;
  uint64_t usable_region_size;
  uint64_t plugged_size;
  uint64_t requested_size;

  zx_status_t status = mem_controller->GetMemSize(&block_size, &region_size, &usable_region_size,
                                                  &plugged_size, &requested_size);
  ASSERT_EQ(status, ZX_OK);
  ASSERT_EQ(block_size, kDefaultBlockSize);
  ASSERT_EQ(region_size, kDefaultMemoryRegionSize);
  ASSERT_EQ(usable_region_size, kDefaultMemoryRegionSize);
  ASSERT_EQ(plugged_size, 0u);
  ASSERT_EQ(requested_size, 0u);
}

}  // namespace
