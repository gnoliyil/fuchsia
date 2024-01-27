// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/standalone-test/standalone.h>
#include <lib/zx/job.h>
#include <lib/zx/pager.h>
#include <lib/zx/profile.h>
#include <lib/zx/thread.h>
#include <zircon/errors.h>
#include <zircon/syscalls/profile.h>
#include <zircon/syscalls/types.h>
#include <zircon/time.h>

#include <thread>

#include <zxtest/zxtest.h>

namespace profile {
namespace {

zx::unowned_job GetRootJob() {
  zx::unowned_job root_job(zx::job::default_job());
  EXPECT_TRUE(root_job->is_valid());
  return root_job;
}

zx_profile_info_t MakeSchedulerProfileInfo(int32_t priority) {
  zx_profile_info_t info = {};
  info.flags = ZX_PROFILE_INFO_FLAG_PRIORITY;
  info.priority = priority;
  return info;
}

zx_profile_info_t MakeSchedulerProfileInfo(const zx_sched_deadline_params_t& params) {
  zx_profile_info_t info = {};
  info.flags = ZX_PROFILE_INFO_FLAG_DEADLINE;
  info.deadline_params = params;
  return info;
}

zx_profile_info_t MakeCpuMaskProfile(uint64_t mask) {
  zx_profile_info_t info = {};
  info.flags = ZX_PROFILE_INFO_FLAG_CPU_MASK;
  info.cpu_affinity_mask.mask[0] = mask;
  return info;
}

zx_profile_info_t MakeMemoryPriorityProfile(int32_t priority) {
  zx_profile_info_t info = {};
  info.flags = ZX_PROFILE_INFO_FLAG_MEMORY_PRIORITY;
  info.priority = priority;
  return info;
}

size_t GetCpuCount() {
  size_t actual, available;
  zx_status_t status =
      standalone::GetRootResource()->get_info(ZX_INFO_CPU_STATS, nullptr, 0, &actual, &available);
  ZX_ASSERT(status == ZX_OK);
  return available;
}

uint64_t GetAffinityMask(const zx::thread& thread) {
  zx_info_thread_t info;
  zx_status_t status = thread.get_info(ZX_INFO_THREAD, &info, sizeof(info), nullptr, nullptr);
  ZX_ASSERT(status == ZX_OK);
  return info.cpu_affinity_mask.mask[0];
}

uint32_t GetLastScheduledCpu(const zx::thread& thread) {
  zx_info_thread_stats_t info;
  zx_status_t status = thread.get_info(ZX_INFO_THREAD_STATS, &info, sizeof(info), nullptr, nullptr);
  ZX_ASSERT(status == ZX_OK);
  return info.last_scheduled_cpu;
}

// Tests in this file rely that the default job is the root job.
TEST(SchedulerProfileTest, CreateProfileWithDefaultPriorityIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_DEFAULT);
  zx::profile profile;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithLowestPriorityIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_LOWEST);
  zx::profile profile;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithLowPriorityIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_LOW);
  zx::profile profile;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithHighPriorityIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_HIGH);
  zx::profile profile;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithHighestPriorityIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_HIGHEST);
  zx::profile profile;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateFairProfileWithNoInheritIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_DEFAULT);
  zx::profile profile;

  profile_info.flags |= ZX_PROFILE_INFO_FLAG_NO_INHERIT;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithPriorityExceedingHighestIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_HIGHEST + 1);
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithPriorityBelowLowestIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_LOWEST - 1);
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithDeadlineIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo({ZX_MSEC(1), ZX_MSEC(8), ZX_MSEC(10)});
  zx::profile profile;

  ASSERT_OK(zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithZeroCapacityIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo({ZX_MSEC(0), ZX_MSEC(8), ZX_MSEC(10)});
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithDeadlineBelowCapacityIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo({ZX_MSEC(8), ZX_MSEC(1), ZX_MSEC(10)});
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithPeriodBelowDeadlineIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo({ZX_MSEC(8), ZX_MSEC(10), ZX_MSEC(1)});
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileOnNonRootJobIsAccessDenied) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx::job child_job;
  ASSERT_OK(zx::job::create(*root_job, 0u, &child_job));
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_DEFAULT);
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_ACCESS_DENIED, zx::profile::create(child_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithNonZeroOptionsIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx::job child_job;
  ASSERT_OK(zx::job::create(*root_job, 0u, &child_job));
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_DEFAULT);
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 1u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, CreateProfileWithDeadlineAndNoInheritIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo({ZX_MSEC(1), ZX_MSEC(8), ZX_MSEC(10)});
  zx::profile profile;

  profile_info.flags |= ZX_PROFILE_INFO_FLAG_NO_INHERIT;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(SchedulerProfileTest, SetThreadPriorityIsOk) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());

  std::atomic<const char*> error = nullptr;
  std::atomic<zx_status_t> result = ZX_OK;

  zx::profile profile_1;
  zx_profile_info_t info_1 = MakeSchedulerProfileInfo(ZX_PRIORITY_LOWEST);
  ASSERT_OK(zx::profile::create(*root_job, 0u, &info_1, &profile_1));

  zx::profile profile_2;
  zx_profile_info_t info_2 = MakeSchedulerProfileInfo(ZX_PRIORITY_HIGH);
  ASSERT_OK(zx::profile::create(*root_job, 0u, &info_2, &profile_2));

  zx::profile profile_3;
  zx_profile_info_t info_3 = MakeSchedulerProfileInfo({ZX_MSEC(8), ZX_MSEC(16), ZX_MSEC(16)});
  ASSERT_OK(zx::profile::create(*root_job, 0u, &info_3, &profile_3));

  // Operate on a background thread, just in case a failure changes the priority of the main
  // thread.
  std::thread worker(
      [](zx::profile first, zx::profile second, zx::profile third, std::atomic<const char*>* error,
         std::atomic<zx_status_t>* result) {
        *result = zx::thread::self()->set_profile(first, 0);
        if (*result != ZX_OK) {
          *error = "Failed to set first profile on thread";
          return;
        }
        std::this_thread::yield();

        *result = zx::thread::self()->set_profile(second, 0);
        if (*result != ZX_OK) {
          *error = "Failed to set second profile on thread";
          return;
        }
        std::this_thread::yield();

        *result = zx::thread::self()->set_profile(third, 0);
        if (*result != ZX_OK) {
          *error = "Failed to set third profile on thread";
          return;
        }
      },
      std::move(profile_1), std::move(profile_2), std::move(profile_3), &error, &result);

  // Wait until is completed.
  worker.join();

  ASSERT_OK(result.load(), "%s", error.load());
}

TEST(ProfileTest, CreateProfileWithDefaultInitializedProfileInfoIsError) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = {};
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(ProfileTest, CreateProfileWithMutuallyExclusiveFlagsIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = {};
  profile_info.flags = ZX_PROFILE_INFO_FLAG_PRIORITY | ZX_PROFILE_INFO_FLAG_DEADLINE;
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
}

TEST(ProfileTest, CreateProfileWithNoProfileInfoIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, nullptr, &profile));
}

TEST(ProfileTest, CreateProfileWithInvalidHandleIsBadHandle) {
  zx::profile profile;

  ASSERT_EQ(ZX_ERR_BAD_HANDLE, zx::profile::create(zx::job(), 0u, nullptr, &profile));
}

TEST(ProfileTest, CreateProfileWithNullProfileIsInvalidArgs) {
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());
  zx_profile_info_t profile_info = MakeSchedulerProfileInfo(ZX_PRIORITY_DEFAULT);

// Needed to test API coverage of null params in GCC.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_profile_create(root_job->get(), 0u, &profile_info, nullptr));
#pragma GCC diagnostic pop
}

zx_status_t RunThreadWithProfile(const zx::profile& profile,
                                 const std::function<zx_status_t()>& body) {
  zx_status_t result;
  std::thread worker([&body, &result, &profile]() {
    result = zx::thread::self()->set_profile(profile, 0);
    if (result != ZX_OK) {
      return;
    }
    result = body();
  });
  worker.join();
  return result;
}

TEST(CpuMaskProfile, EmptyMaskIsValid) {
  zx::profile profile;
  zx_profile_info_t profile_info = MakeCpuMaskProfile(0);
  ASSERT_OK(zx::profile::create(*GetRootJob(), 0u, &profile_info, &profile));

  // Ensure that the thread can still run, despite the affinity mask
  // having no valid CPUs in it. (The kernel will just fall back to
  // its own choice of CPUs if this mask can't be respected.)
  ASSERT_OK(RunThreadWithProfile(profile, []() {
    EXPECT_EQ(GetAffinityMask(*zx::thread::self()), 0);
    EXPECT_NE(GetLastScheduledCpu(*zx::thread::self()), ZX_INFO_INVALID_CPU);
    return ZX_OK;
  }));
}

TEST(CpuMaskProfile, ApplyProfile) {
  const size_t num_cpus = GetCpuCount();
  ASSERT_LT(num_cpus, ZX_CPU_SET_BITS_PER_WORD,
            "Test assumes system running with less than %d cores.", ZX_CPU_SET_BITS_PER_WORD);
  for (size_t i = 0; i < num_cpus; i++) {
    zx_profile_info_t profile_info = MakeCpuMaskProfile(1 << i);
    zx::profile profile;
    ASSERT_OK(zx::profile::create(*GetRootJob(), 0u, &profile_info, &profile));

    // Ensure that the correct mask was applied.
    ASSERT_OK(RunThreadWithProfile(profile, [i]() {
      EXPECT_EQ(GetAffinityMask(*zx::thread::self()), (1 << i));
      EXPECT_EQ(GetLastScheduledCpu(*zx::thread::self()), i);
      return ZX_OK;
    }));
  }
}

TEST(MemoryPriorityProfile, InvalidPriorities) {
  constexpr int32_t kBadPriorities[] = {ZX_PRIORITY_LOWEST, ZX_PRIORITY_LOW, ZX_PRIORITY_HIGHEST};

  for (const int32_t prio : kBadPriorities) {
    zx::profile profile;

    zx_profile_info_t profile_info = MakeMemoryPriorityProfile(prio);
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*GetRootJob(), 0u, &profile_info, &profile));
  }
}

TEST(MemoryPriorityProfile, MemoryOrThread) {
  const uint32_t kInvalidWith[] = {ZX_PROFILE_INFO_FLAG_PRIORITY, ZX_PROFILE_INFO_FLAG_CPU_MASK,
                                   ZX_PROFILE_INFO_FLAG_DEADLINE};
  zx::unowned_job root_job(zx::job::default_job());
  ASSERT_TRUE(root_job->is_valid());

  for (const uint32_t invalid_with : kInvalidWith) {
    zx_profile_info_t profile_info = {};
    profile_info.flags = ZX_PROFILE_INFO_FLAG_MEMORY_PRIORITY | invalid_with;
    zx::profile profile;

    ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx::profile::create(*root_job, 0u, &profile_info, &profile));
  }
}

TEST(MemoryPriorityProfile, ApplyProfile) {
  // Create the two profiles we will need.
  zx::profile profile_high, profile_default;
  zx_profile_info_t profile_info = MakeMemoryPriorityProfile(ZX_PRIORITY_HIGH);
  ASSERT_OK(zx::profile::create(*GetRootJob(), 0u, &profile_info, &profile_high));
  profile_info = MakeMemoryPriorityProfile(ZX_PRIORITY_DEFAULT);
  ASSERT_OK(zx::profile::create(*GetRootJob(), 0u, &profile_info, &profile_default));

  // To ensure there are some candidate reclaimable pages, create and map in a pager backed VMO.
  zx::port port;
  ASSERT_OK(zx::port::create(0, &port));
  zx::pager pager;
  ASSERT_OK(zx::pager::create(0, &pager));
  zx::vmo pager_vmo;
  ASSERT_OK(pager.create_vmo(0, port, 0, zx_system_get_page_size(), &pager_vmo));
  zx_vaddr_t addr;
  ASSERT_OK(zx::vmar::root_self()->map(0, 0, pager_vmo, 0, zx_system_get_page_size(), &addr));

  // Helper to supply pages to the VMO. Since these pages are reclaimable there is a small chance
  // that they get evicted during the execution of the test so we re-supply at a few different
  // points.
  auto supply = [&]() {
    zx::vmo aux_vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &aux_vmo));
    uint64_t val = 42;
    EXPECT_OK(aux_vmo.write(&val, 0, sizeof(val)));
    EXPECT_OK(pager.supply_pages(pager_vmo, 0, zx_system_get_page_size(), aux_vmo, 0));
  };

  // Start with the pages supplied so that they are there for the initial query.
  supply();

  zx_info_kmem_stats_extended_t stats;
  EXPECT_OK(standalone::GetRootResource()->get_info(ZX_INFO_KMEM_STATS_EXTENDED, &stats,
                                                    sizeof(stats), nullptr, nullptr));

  const uint64_t prev = stats.vmo_reclaim_disabled_bytes;

  EXPECT_OK(zx::vmar::root_self()->set_profile(profile_high, 0));

  // Applying the profile should have caused our pages to no longer be reclaimable. The pager backed
  // VMO we mapped means we know it's definitely non-zero, but we do not know if there are more.
  // In between the previous supply and setting the profile, the pages could have been evicted, so
  // re-supply them.
  supply();
  EXPECT_OK(standalone::GetRootResource()->get_info(ZX_INFO_KMEM_STATS_EXTENDED, &stats,
                                                    sizeof(stats), nullptr, nullptr));
  EXPECT_GT(stats.vmo_reclaim_disabled_bytes, prev);

  // Applying the default priority should undo the reclamation change.
  EXPECT_OK(zx::vmar::root_self()->set_profile(profile_default, 0));

  EXPECT_OK(standalone::GetRootResource()->get_info(ZX_INFO_KMEM_STATS_EXTENDED, &stats,
                                                    sizeof(stats), nullptr, nullptr));
  EXPECT_EQ(stats.vmo_reclaim_disabled_bytes, prev);
}

TEST(MemoryPriorityProfile, Rights) {
  zx::profile profile;
  zx_profile_info_t profile_info = MakeMemoryPriorityProfile(ZX_PRIORITY_DEFAULT);
  ASSERT_OK(zx::profile::create(*GetRootJob(), 0u, &profile_info, &profile));

  // Duplicate the vmar handle to have valid and invalid permissions.
  zx::vmar vmar_valid, vmar_invalid;
  ASSERT_OK(zx::vmar::root_self()->duplicate(ZX_RIGHT_OP_CHILDREN, &vmar_valid));
  ASSERT_OK(zx::vmar::root_self()->duplicate(0, &vmar_invalid));

  EXPECT_OK(vmar_valid.set_profile(profile, 0));
  EXPECT_EQ(ZX_ERR_ACCESS_DENIED, vmar_invalid.set_profile(profile, 0));
}

}  // namespace
}  // namespace profile
