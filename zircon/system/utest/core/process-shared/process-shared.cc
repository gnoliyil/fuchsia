// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fit/defer.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <zircon/errors.h>
#include <zircon/syscalls-next.h>
#include <zircon/syscalls.h>

#include <mini-process/mini-process.h>
#include <zxtest/zxtest.h>

TEST(ProcessShared, MapInPrototype) {
  zx::process prototype_process;
  zx::vmar shared_vmar;
  constexpr const char kPrototypeName[] = "prototype_process";
  ASSERT_OK(zx::process::create(*zx::job::default_job(), kPrototypeName, sizeof(kPrototypeName),
                                ZX_PROCESS_SHARED, &prototype_process, &shared_vmar));

  zx::process process;
  zx::vmar restricted_vmar;
  constexpr const char kProcessName[] = "process";
  ASSERT_OK(zx_process_create_shared(prototype_process.get(), 0, kProcessName, sizeof(kProcessName),
                                     process.reset_and_get_address(),
                                     restricted_vmar.reset_and_get_address()));

  // Map a vmo into the shared vmar using the prototype handle.
  zx_handle_t vmo;
  ASSERT_OK(zx_vmo_create(zx_system_get_page_size(), 0, &vmo));
  uintptr_t addr;
  ASSERT_OK(zx_vmar_map(shared_vmar.get(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0,
                        zx_system_get_page_size(), &addr));

  // Write some data to the vmo.
  std::vector<char> shared_data = {'a', 'b', 'c'};
  ASSERT_OK(zx_vmo_write(vmo, shared_data.data(), 0, shared_data.size()));

  // Read process memory from both processes to make sure they have the data.
  std::vector<char> read_data = {'d', 'e', 'f'};
  size_t actual = 0;
  ASSERT_EQ(prototype_process.read_memory(addr, read_data.data(), read_data.size(), &actual),
            ZX_OK);
  ASSERT_EQ(read_data, shared_data);

  // Now read the same address from the second process.
  read_data = {'d', 'e', 'f'};
  ASSERT_EQ(process.read_memory(addr, read_data.data(), read_data.size(), &actual), ZX_OK);
  ASSERT_EQ(read_data, shared_data);
}

TEST(ProcessShared, RestrictedVmarNotShared) {
  zx::process prototype_process;
  zx::vmar shared_vmar;
  constexpr const char kPrototypeName[] = "prototype_process";
  ASSERT_OK(zx::process::create(*zx::job::default_job(), kPrototypeName, sizeof(kPrototypeName),
                                ZX_PROCESS_SHARED, &prototype_process, &shared_vmar));

  zx::process process;
  zx::vmar restricted_vmar;
  constexpr const char kProcessName[] = "process";
  ASSERT_OK(zx_process_create_shared(prototype_process.get(), 0, kProcessName, sizeof(kProcessName),
                                     process.reset_and_get_address(),
                                     restricted_vmar.reset_and_get_address()));

  // Map a vmo into the restricted vmar of the second process.
  zx_handle_t vmo;
  ASSERT_OK(zx_vmo_create(zx_system_get_page_size(), 0, &vmo));
  uintptr_t addr;
  ASSERT_OK(zx_vmar_map(restricted_vmar.get(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0,
                        zx_system_get_page_size(), &addr));

  // Write some data to the vmo.
  std::vector<char> restricted_data = {'a', 'b', 'c'};
  ASSERT_OK(zx_vmo_write(vmo, restricted_data.data(), 0, restricted_data.size()));

  std::vector<char> read_data = {'d', 'e', 'f'};
  size_t actual = 0;
  // Now try to read the same address from the first process, which should fail.
  if (prototype_process.read_memory(addr, read_data.data(), read_data.size(), &actual) == ZX_OK) {
    // If `addr` happened to be valid, make sure that the read data was not the restricted data from
    // the prototype.
    ASSERT_NE(read_data, restricted_data);
  }
}

TEST(ProcessShared, InvalidPrototype) {
  zx::process prototype_process;
  zx::vmar prototype_vmar;
  constexpr const char kPrototypeName[] = "prototype_process";
  ASSERT_OK(zx::process::create(*zx::job::default_job(), kPrototypeName, sizeof(kPrototypeName), 0,
                                &prototype_process, &prototype_vmar));

  zx::process process;
  zx::vmar restricted_vmar;
  constexpr const char kProcessName[] = "process";
  ASSERT_EQ(zx_process_create_shared(prototype_process.get(), 0, kProcessName, sizeof(kProcessName),
                                     process.reset_and_get_address(),
                                     restricted_vmar.reset_and_get_address()),
            ZX_ERR_INVALID_ARGS);
}

TEST(ProcessShared, InfoProcessVmos) {
  // Build a shareable process.
  static constexpr char kSharedProcessName[] = "object-info-shar-proc";
  zx::process shared_process;
  zx::vmar shared_vmar;
  ASSERT_OK(zx::process::create(*zx::job::default_job(), kSharedProcessName,
                                sizeof(kSharedProcessName), ZX_PROCESS_SHARED, &shared_process,
                                &shared_vmar));

  // Build another process that shares its address space.
  static constexpr char kPrivateProcessName[] = "object-info-priv-proc";
  zx::process private_process;
  zx::vmar private_vmar;
  ASSERT_OK(zx_process_create_shared(
      shared_process.get(), 0, kPrivateProcessName, sizeof(kPrivateProcessName),
      private_process.reset_and_get_address(), private_vmar.reset_and_get_address()));

  // Map a single VMO that contains some code (a busy loop) and a stack into the shared address
  // space.
  zx_vaddr_t stack_base, sp;
  ASSERT_OK(mini_process_load_stack(shared_vmar.get(), true, &stack_base, &sp));

  // Allocate two extra VMOs.
  const size_t kVmoSize = zx_system_get_page_size();
  zx::vmo vmo1, vmo2;
  ASSERT_OK(zx::vmo::create(kVmoSize, 0u, &vmo1));
  ASSERT_OK(zx::vmo::create(kVmoSize, 0u, &vmo2));

  // Start the shared process and pass a handle to vmo1 to it.
  zx::thread shared_thread;
  static constexpr char kSharedThreadName[] = "object-info-shar-thrd";
  ASSERT_OK(zx::thread::create(shared_process, kSharedThreadName, sizeof(kSharedThreadName), 0u,
                               &shared_thread));
  ASSERT_OK(shared_process.start(shared_thread, stack_base, sp, std::move(vmo1), 0));
  auto shared_cleanup = fit::defer([&] { shared_process.kill(); });

  // Start the private process.
  zx::thread private_thread;
  static constexpr char kPrivateThreadName[] = "object-info-priv-thrd";
  ASSERT_OK(zx::thread::create(private_process, kPrivateThreadName, sizeof(kPrivateThreadName), 0u,
                               &private_thread));
  ASSERT_OK(private_process.start(private_thread, stack_base, sp, zx::handle(), 0));
  auto private_cleanup = fit::defer([&] { private_process.kill(); });

  // Map vmo2 into the private address space only.
  zx_vaddr_t vaddr;
  ASSERT_OK(private_vmar.map(ZX_VM_PERM_READ, 0u, vmo2, 0, kVmoSize, &vaddr));

  // Buffer big enough to read all of the test processes' VMO entries: the mini-process VMO, vmo1
  // and vmo2.
  const size_t buf_size = 3;
  std::unique_ptr<zx_info_vmo_t[]> buf(new zx_info_vmo_t[buf_size]);

  // Read the VMO entries of the shared process.
  size_t actual, available_shared;
  ASSERT_OK(shared_process.get_info(ZX_INFO_PROCESS_VMOS, buf.get(),
                                    buf_size * sizeof(zx_info_vmo_t), &actual, &available_shared));
  ASSERT_EQ(actual, available_shared);
  ASSERT_EQ(2, available_shared);

  // Read the VMO entries of the private process.
  size_t available_private;
  ASSERT_OK(private_process.get_info(ZX_INFO_PROCESS_VMOS, buf.get(),
                                     buf_size * sizeof(zx_info_vmo_t), &actual,
                                     &available_private));
  ASSERT_EQ(actual, available_private);
  ASSERT_EQ(available_shared + 1, available_private);

  // Verify that the VMO entries from the private address space are accounted correctly if they
  // don't fit in the buffer.
  const size_t smallbuf_size = available_shared;
  std::unique_ptr<zx_info_vmo_t[]> smallbuf(new zx_info_vmo_t[smallbuf_size]);
  size_t actual_smallbuf, available_smallbuf;
  ASSERT_OK(private_process.get_info(ZX_INFO_PROCESS_VMOS, smallbuf.get(),
                                     smallbuf_size * sizeof(zx_info_vmo_t), &actual_smallbuf,
                                     &available_smallbuf));
  ASSERT_EQ(smallbuf_size, actual_smallbuf);
  ASSERT_EQ(available_private, available_smallbuf);
}

// Verify mappings in shared processes are properly accounted for in zx_info_task_stats_t.
//
// See also fxbug.dev/123525.
TEST(ProcessShared, InfoTaskStats) {
  // We're going to create 3 processes, proc1, proc2, and proc3, with a total of 4 VMARs.  proc1 and
  // proc2 will share a region (vmar_shared) and each have their own private region (vmar1 and
  // vmar2).  proc3 will have one (unshared) region (vmar3).
  //
  // We'll then map 6 VMOs (m[0] through m[5]) and verify that each process's zx_info_task_stats_t
  // accurately counts the private/shared pages and that the mem_scaled_shared_bytes "sums to 1".

  // First, we'll need to create a process with a shared region that we can use to create proc1 and
  // proc2.  We're only creating this process, proc0, so that we can create the region that proc1
  // and proc2 will share.  We'll destroy proc0 before we create any mappings.
  zx::process proc0;
  zx::vmar vmar_shared;
  static constexpr char kNameProc0[] = "proc0";
  ASSERT_OK(zx::process::create(*zx::job::default_job(), kNameProc0, sizeof(kNameProc0),
                                ZX_PROCESS_SHARED, &proc0, &vmar_shared));

  // Create proc1 and proc2.
  zx::process proc1;
  zx::vmar vmar1;
  static constexpr char kNameProc1[] = "proc1";
  ASSERT_OK(zx_process_create_shared(proc0.get(), 0, kNameProc1, sizeof(kNameProc1),
                                     proc1.reset_and_get_address(), vmar1.reset_and_get_address()));
  zx::process proc2;
  zx::vmar vmar2;
  static constexpr char kNameProc2[] = "proc2";
  ASSERT_OK(zx_process_create_shared(proc0.get(), 0, kNameProc2, sizeof(kNameProc2),
                                     proc2.reset_and_get_address(), vmar2.reset_and_get_address()));

  // We can now destroy proc0 since it has served its purpose (to facilitate creation of proc1 and
  // proc2).
  proc0.reset();

  // Create proc3, a regular old process.
  zx::process proc3;
  zx::vmar vmar3;
  static constexpr char kNameProc3[] = "proc3";
  ASSERT_OK(zx::process::create(*zx::job::default_job(), kNameProc3, sizeof(kNameProc3), 0, &proc3,
                                &vmar3));

  // Now create the 6 VMOs of 1 page each.
  const size_t kSize = zx_system_get_page_size();
  zx::vmo m[6];
  const char buffer[1] = {'A'};
  for (zx::vmo& i : m) {
    ASSERT_OK(zx::vmo::create(kSize, 0u, &i));
    // Write into the VMO to ensure a page is committed.
    ASSERT_OK(i.write(buffer, 0, sizeof(buffer)));
  }

  // Make sure all the task stats start as 0.
  auto assertProcStatsAreZero = [](zx::process& proc) {
    zx_info_task_stats_t stats{};
    size_t actual{};
    size_t avail{};
    ASSERT_OK(proc.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
    ASSERT_EQ(0, stats.mem_mapped_bytes);
    ASSERT_EQ(0, stats.mem_private_bytes);
    ASSERT_EQ(0, stats.mem_shared_bytes);
    ASSERT_EQ(0, stats.mem_scaled_shared_bytes);
  };
  zx::process* procs[] = {&proc1, &proc2, &proc3};
  for (zx::process* p : procs) {
    ASSERT_NO_FAILURES(assertProcStatsAreZero(*p));
  }

  // Now we'll start mapping the VMOs and verify the stats as we go.

  // vmar1 will get m[0] and m[1].
  zx_vaddr_t vaddr{};
  ASSERT_OK(vmar1.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[0], 0, kSize, &vaddr));
  ASSERT_OK(vmar1.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[1], 0, kSize, &vaddr));
  zx_info_task_stats_t stats{};
  size_t actual{};
  size_t avail{};
  ASSERT_OK(proc1.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(1, actual);
  ASSERT_EQ(2 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(0, stats.mem_shared_bytes);
  ASSERT_EQ(0, stats.mem_scaled_shared_bytes);
  ASSERT_NO_FAILURES(assertProcStatsAreZero(proc2));
  ASSERT_NO_FAILURES(assertProcStatsAreZero(proc3));

  // vmar_shared will get m[2] and m[3].
  ASSERT_OK(vmar_shared.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[2], 0, kSize, &vaddr));
  ASSERT_OK(vmar_shared.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[3], 0, kSize, &vaddr));

  // At this point, we have a total of 4 pages mapped.  2 are private and 2 are shared, with proc1
  // and proc2 each being fractionally-responsible for 1 of the 2 shared pages.

  ASSERT_OK(proc1.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(4 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ(kSize, stats.mem_scaled_shared_bytes);

  ASSERT_OK(proc2.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(2 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(0, stats.mem_private_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ(kSize, stats.mem_scaled_shared_bytes);

  // vmar2 will get m[4] and m[5].
  ASSERT_OK(vmar2.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[4], 0, kSize, &vaddr));
  ASSERT_OK(vmar2.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[5], 0, kSize, &vaddr));

  // We've now got 6 pages mapped.  2 are private to proc1, 2 are private to proc2 and 2 are shared
  // between them.  Each is fractionally responsible for 1 of the shared pages.

  ASSERT_OK(proc2.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(4 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ(kSize, stats.mem_scaled_shared_bytes);

  // Now bring proc3 into the mix.

  // vmar3 will get m[1], m[2], and m[4].
  ASSERT_OK(vmar3.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[1], 0, kSize, &vaddr));
  ASSERT_OK(vmar3.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[2], 0, kSize, &vaddr));
  ASSERT_OK(vmar3.map(ZX_VM_PERM_READ | ZX_VM_MAP_RANGE, 0u, m[4], 0, kSize, &vaddr));

  // Still 6 mapped pages.  proc1 and proc2 each have 1 private page and 3 shared pages.  proc3 has
  // 0 private pages and 3 shared pages.  The important thing here is that the
  // mem_scaled_shared_bytes of the three processes plus the private pages sum to 6 pages.
  //
  // See that there are 2 private pages and that proc1 and proc2 are each fractionally-responsible
  // for 1.25 pages, while proc3 is fractionally-responsible for 1.5 pages.
  //
  // 2 + 2 * 1.25 + 1.5 == 6
  ASSERT_OK(proc1.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(4 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(1 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(3 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ((kSize / 2) + (kSize / 2) + (kSize / 4), stats.mem_scaled_shared_bytes);

  ASSERT_OK(proc2.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(4 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(1 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(3 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ((kSize / 2) + (kSize / 2) + (kSize / 4), stats.mem_scaled_shared_bytes);

  ASSERT_OK(proc3.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(3 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(0, stats.mem_private_bytes);
  ASSERT_EQ(3 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ((kSize / 2) + (kSize / 2) + (kSize / 2), stats.mem_scaled_shared_bytes);

  // Kill off proc2.  We're now down to 5 pages.  See that it sums up properly.
  //
  // 2 + 1 + (0.5 + 0.5) + (0.5 + 0.5) == 5
  proc2.reset();
  ASSERT_OK(proc1.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(4 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ((kSize / 2) + (kSize / 2), stats.mem_scaled_shared_bytes);

  ASSERT_OK(proc3.get_info(ZX_INFO_TASK_STATS, &stats, sizeof(stats), &actual, &avail));
  ASSERT_EQ(3 * kSize, stats.mem_mapped_bytes);
  ASSERT_EQ(1 * kSize, stats.mem_private_bytes);
  ASSERT_EQ(2 * kSize, stats.mem_shared_bytes);
  ASSERT_EQ((kSize / 2) + (kSize / 2), stats.mem_scaled_shared_bytes);
}
