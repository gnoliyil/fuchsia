// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.kernel/cpp/fidl.h>
#include <lib/sys/component/cpp/service_client.h>

#include "lib/zx/time.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/virtualization/tests/lib/guest_test.h"

namespace {

constexpr size_t kVirtioBalloonPageCount = 256;

template <class T>
class BalloonGuestTest : public GuestTest<T> {
 public:
  BalloonGuestTest() {
    auto client_end = component::Connect<fuchsia_kernel::Stats>();
    if (!client_end.is_ok()) {
      FX_PLOGS(ERROR, client_end.status_value()) << "Failed to connect to kernel stats";
    }
    stats.Bind(std::move(*client_end));
  }

  void TestGetMemStats(
      const char* trace_context,
      const fuchsia::virtualization::BalloonControllerSyncPtr& balloon_controller) {
    SCOPED_TRACE(trace_context);
    // 5.5.6.4 Memory Statistics Tags
    constexpr uint16_t VIRTIO_BALLOON_S_MEMFREE = 4;
    constexpr uint16_t VIRTIO_BALLOON_S_MEMTOT = 5;
    constexpr uint16_t VIRTIO_BALLOON_S_AVAIL = 6;
    ::fidl::VectorPtr<::fuchsia::virtualization::MemStat> mem_stats;
    int32_t mem_stats_status = 0;
    zx_status_t status = balloon_controller->GetMemStats(&mem_stats_status, &mem_stats);
    EXPECT_EQ(status, ZX_OK);
    EXPECT_EQ(mem_stats_status, ZX_OK);
    std::unordered_map<uint16_t, uint64_t> stats;
    for (auto& el : mem_stats.value()) {
      stats[el.tag] = el.val;
    }
    EXPECT_GT(stats[VIRTIO_BALLOON_S_MEMTOT], 0u);
    EXPECT_GT(stats[VIRTIO_BALLOON_S_MEMFREE], 0u);
    EXPECT_GT(stats[VIRTIO_BALLOON_S_AVAIL], 0u);
    EXPECT_LE(stats[VIRTIO_BALLOON_S_MEMFREE], stats[VIRTIO_BALLOON_S_MEMTOT]);
    EXPECT_LE(stats[VIRTIO_BALLOON_S_AVAIL], stats[VIRTIO_BALLOON_S_MEMTOT]);
  }

  void PrintKernelMemoryStats(std::string_view header) {
    auto memory_stats = stats->GetMemoryStats();
    FX_LOGS(INFO) << header
                  << ": total_bytes=" << memory_stats->stats().total_bytes().value() / 1024 / 1024
                  << " MiB free_bytes=" << memory_stats->stats().free_bytes().value() / 1024 / 1024
                  << " MiB";
  }

  fidl::SyncClient<fuchsia_kernel::Stats> stats;
};

// Zircon does not yet have a virtio balloon driver.
using GuestTypes = ::testing::Types<DebianEnclosedGuest, TerminaEnclosedGuest>;
TYPED_TEST_SUITE(BalloonGuestTest, GuestTypes, GuestTestNameGenerator);

TYPED_TEST(BalloonGuestTest, InflateDeflate) {
  std::string result;
  EXPECT_EQ(this->Execute({"echo", "test"}, &result), ZX_OK);
  EXPECT_EQ(result, "test\n");

  fuchsia::virtualization::BalloonControllerSyncPtr balloon_controller;
  ASSERT_TRUE(this->ConnectToBalloon(balloon_controller.NewRequest()));

  uint32_t initial_num_pages;
  uint32_t requested_num_pages;
  zx_status_t status = balloon_controller->GetBalloonSize(&initial_num_pages, &requested_num_pages);
  ASSERT_EQ(status, ZX_OK);
  EXPECT_EQ(requested_num_pages, initial_num_pages);
  this->TestGetMemStats("Before inflate", balloon_controller);

  // Request an increase to the number of pages in the balloon.
  status = balloon_controller->RequestNumPages(initial_num_pages + kVirtioBalloonPageCount);
  ASSERT_EQ(status, ZX_OK);

  // Verify that the number of pages eventually equals the requested number. The
  // guest may not respond to the request immediately so we call GetBalloonSize in
  // a loop.
  uint32_t current_num_pages;
  while (true) {
    status = balloon_controller->GetBalloonSize(&current_num_pages, &requested_num_pages);
    ASSERT_EQ(status, ZX_OK);
    EXPECT_EQ(requested_num_pages, initial_num_pages + kVirtioBalloonPageCount);
    if (current_num_pages == initial_num_pages + kVirtioBalloonPageCount) {
      break;
    }
  }
  this->TestGetMemStats("After inflate", balloon_controller);

  // Request a decrease to the number of pages in the balloon back to the
  // initial value.
  status = balloon_controller->RequestNumPages(initial_num_pages);
  ASSERT_EQ(status, ZX_OK);

  while (true) {
    status = balloon_controller->GetBalloonSize(&current_num_pages, &requested_num_pages);
    ASSERT_EQ(status, ZX_OK);
    EXPECT_EQ(requested_num_pages, initial_num_pages);
    if (current_num_pages == initial_num_pages) {
      break;
    }
  }
  this->TestGetMemStats("After deflate", balloon_controller);
}

TYPED_TEST(BalloonGuestTest, VirtioBalloonFreePageReporting) {
  const uint64_t starting_free_memory_mib =
      this->stats->GetMemoryStats()->stats().free_bytes().value() / 1024 / 1024;

  // Allocate 128MiB or 60% of the free memory, whatever is the smallest. Asan
  // builds can ran quite low on memory. We don't want to stress the system too
  // much and cause memory pressure warning by allocating too much memory
  const uint64_t max_alloc_amount_mib = 128;
  const uint64_t alloc_amount_mib =
      std::min(max_alloc_amount_mib, starting_free_memory_mib / 10 * 6);
  this->PrintKernelMemoryStats("Before the guest alloc");
  FX_LOGS(INFO) << fxl::StringPrintf("Allocate and release %lu MiB in the guest", alloc_amount_mib);

  std::string result;
  ASSERT_EQ(this->RunUtil(
                fxl::StringPrintf("memory_test_util alloc --size-mb 1 --num %lu", alloc_amount_mib),
                {}, &result),
            ZX_OK);

  this->PrintKernelMemoryStats("After the guest alloc");

  // Wait until at least half of the allocated memory get reclaimed. We cannot
  // wait for the free memory amount to get exactly the same or higher than the
  // original amount of free memory when the test was started. Guest OS can decide
  // to use some memory for multiple reasons. Waiting for half of the memory to
  // get reclaimed allows to eliminate false positives and false negatives.
  FX_LOGS(INFO) << "Waiting for the virtio balloon to reclaim memory";
  while (starting_free_memory_mib - alloc_amount_mib / 2 >
         this->stats->GetMemoryStats()->stats().free_bytes().value() / 1024 / 1024) {
    zx::nanosleep(zx::deadline_after(zx::msec(100)));
  }

  this->PrintKernelMemoryStats("After the memory reclaim");
}

}  // namespace
