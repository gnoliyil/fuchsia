// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <bits.h>
#include <lib/fit/defer.h>
#include <lib/unittest/unittest.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <arch/aspace.h>
#include <ktl/iterator.h>
#include <vm/arch_vm_aspace.h>
#include <vm/pmm.h>
#include <vm/vm_address_region.h>
#include <vm/vm_aspace.h>
#include <vm/vm_object_paged.h>

#include <ktl/enforce.h>

#ifdef __x86_64__
#include <arch/x86/mmu.h>
#define PGTABLE_L1_SHIFT PDP_SHIFT
#define PGTABLE_L2_SHIFT PD_SHIFT
#elif defined(__aarch64__)
#define PGTABLE_L1_SHIFT MMU_LX_X(MMU_KERNEL_PAGE_SIZE_SHIFT, 1)
#define PGTABLE_L2_SHIFT MMU_LX_X(MMU_KERNEL_PAGE_SIZE_SHIFT, 2)
#elif defined(__riscv)
// sv39
#define PGTABLE_L1_SHIFT 30
#define PGTABLE_L2_SHIFT 21
#endif

// Most mmu tests want a 'sufficiently large' aspace to play in, these constants define an aspace
// that is large without having a discontinuity over the sign extended canonical addresses.
constexpr vaddr_t kAspaceBase = 1UL << 20;
constexpr size_t kAspaceSize = (1UL << 47) - kAspaceBase - (1UL << 20);

static bool test_large_unaligned_region() {
  BEGIN_TEST;
  ArchVmAspace aspace(kAspaceBase, kAspaceSize, 0);
  zx_status_t err = aspace.Init();
  EXPECT_EQ(err, ZX_OK, "init aspace");

  const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

  // We want our region to be misaligned by at least a page, and for
  // it to straddle the PDP.
  vaddr_t va = (1UL << PGTABLE_L1_SHIFT) - (1UL << PGTABLE_L2_SHIFT) + 2 * PAGE_SIZE;
  // Make sure alloc_size is less than 1 PD page, to exercise the
  // non-terminal code path.
  static const size_t alloc_size = (1UL << PGTABLE_L2_SHIFT) - PAGE_SIZE;

  // Map a single page to force the lower PDP of the target region
  // to be created
  size_t mapped;
  err = aspace.MapContiguous(va - 3 * PAGE_SIZE, 0, 1, arch_rw_flags, &mapped);
  EXPECT_EQ(err, ZX_OK, "map single page");
  EXPECT_EQ(mapped, 1u, "map single page");

  // Map the last page of the region
  err = aspace.MapContiguous(va + alloc_size - PAGE_SIZE, 0, 1, arch_rw_flags, &mapped);
  EXPECT_EQ(err, ZX_OK, "map last page");
  EXPECT_EQ(mapped, 1u, "map single page");

  paddr_t pa;
  uint flags;
  err = aspace.Query(va + alloc_size - PAGE_SIZE, &pa, &flags);
  EXPECT_EQ(err, ZX_OK, "last entry is mapped");

  // Attempt to unmap the target region (analogous to unmapping a demand
  // paged region that has only had its last page touched)
  size_t unmapped;
  err = aspace.Unmap(va, alloc_size / PAGE_SIZE, ArchVmAspace::EnlargeOperation::Yes, &unmapped);
  EXPECT_EQ(err, ZX_OK, "unmap unallocated region");
  EXPECT_EQ(unmapped, alloc_size / PAGE_SIZE, "unmap unallocated region");

  err = aspace.Query(va + alloc_size - PAGE_SIZE, &pa, &flags);
  EXPECT_EQ(err, ZX_ERR_NOT_FOUND, "last entry is not mapped anymore");

  // Unmap the single page from earlier
  err = aspace.Unmap(va - 3 * PAGE_SIZE, 1, ArchVmAspace::EnlargeOperation::Yes, &unmapped);
  EXPECT_EQ(err, ZX_OK, "unmap single page");
  EXPECT_EQ(unmapped, 1u, "unmap unallocated region");

  err = aspace.Destroy();
  EXPECT_EQ(err, ZX_OK, "destroy aspace");

  END_TEST;
}

static bool test_large_unaligned_region_without_map() {
  BEGIN_TEST;

  {
    ArchVmAspace aspace(kAspaceBase, kAspaceSize, 0);
    zx_status_t err = aspace.Init();
    EXPECT_EQ(err, ZX_OK, "init aspace");

    const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

    // We want our region to be misaligned by a page, and for it to
    // straddle the PDP
    vaddr_t va = (1UL << PGTABLE_L1_SHIFT) - (1UL << PGTABLE_L2_SHIFT) + PAGE_SIZE;
    // Make sure alloc_size is bigger than 1 PD page, to exercise the
    // non-terminal code path.
    static const size_t alloc_size = 3UL << PGTABLE_L2_SHIFT;

    // Map a single page to force the lower PDP of the target region
    // to be created
    size_t mapped;
    err = aspace.MapContiguous(va - 2 * PAGE_SIZE, 0, 1, arch_rw_flags, &mapped);
    EXPECT_EQ(err, ZX_OK, "map single page");
    EXPECT_EQ(mapped, 1u, "map single page");

    // Attempt to unmap the target region (analogous to unmapping a demand
    // paged region that has not been touched)
    size_t unmapped;
    err = aspace.Unmap(va, alloc_size / PAGE_SIZE, ArchVmAspace::EnlargeOperation::Yes, &unmapped);
    EXPECT_EQ(err, ZX_OK, "unmap unallocated region");
    EXPECT_EQ(unmapped, alloc_size / PAGE_SIZE, "unmap unallocated region");

    // Unmap the single page from earlier
    err = aspace.Unmap(va - 2 * PAGE_SIZE, 1, ArchVmAspace::EnlargeOperation::Yes, &unmapped);
    EXPECT_EQ(err, ZX_OK, "unmap single page");
    EXPECT_EQ(unmapped, 1u, "unmap single page");

    err = aspace.Destroy();
    EXPECT_EQ(err, ZX_OK, "destroy aspace");
  }

  END_TEST;
}

static bool test_large_region_protect() {
  BEGIN_TEST;

  static const vaddr_t va = 1UL << PGTABLE_L1_SHIFT;
  // Force a large page.
  static const size_t alloc_size = 1UL << PGTABLE_L2_SHIFT;
  static const vaddr_t alloc_end = va + alloc_size;

  vaddr_t target_vaddrs[] = {
      va,
      va + PAGE_SIZE,
      va + 2 * PAGE_SIZE,
      alloc_end - 3 * PAGE_SIZE,
      alloc_end - 2 * PAGE_SIZE,
      alloc_end - PAGE_SIZE,
  };

  for (unsigned i = 0; i < ktl::size(target_vaddrs); i++) {
    ArchVmAspace aspace(kAspaceBase, kAspaceSize, 0);
    zx_status_t err = aspace.Init();
    EXPECT_EQ(err, ZX_OK, "init aspace");

    const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

    size_t mapped;
    err = aspace.MapContiguous(va, 0, alloc_size / PAGE_SIZE, arch_rw_flags, &mapped);
    EXPECT_EQ(err, ZX_OK, "map large page");
    EXPECT_EQ(mapped, 512u, "map large page");

    err = aspace.Protect(target_vaddrs[i], 1, ARCH_MMU_FLAG_PERM_READ);
    EXPECT_EQ(err, ZX_OK, "protect single page");

    for (unsigned j = 0; j < ktl::size(target_vaddrs); j++) {
      uint retrieved_flags = 0;
      paddr_t pa;
      EXPECT_EQ(ZX_OK, aspace.Query(target_vaddrs[j], &pa, &retrieved_flags));
      EXPECT_EQ(target_vaddrs[j] - va, pa);

      EXPECT_EQ(i == j ? ARCH_MMU_FLAG_PERM_READ : arch_rw_flags, retrieved_flags);
    }

    err = aspace.Unmap(va, alloc_size / PAGE_SIZE, ArchVmAspace::EnlargeOperation::Yes, &mapped);
    EXPECT_EQ(err, ZX_OK, "unmap large page");
    EXPECT_EQ(mapped, 512u, "unmap large page");
    err = aspace.Destroy();
    EXPECT_EQ(err, ZX_OK, "destroy aspace");
  }

  END_TEST;
}

// Since toggle_page_alloc_fn needs global state to operate, define a lock to ensure we are only
// running a single instance of these tests at a time.
DECLARE_SINGLETON_MUTEX(TogglePageAllocLock);
static bool fail_page_allocs = false;
static zx_status_t toggle_page_alloc_fn(uint mmu_flags, vm_page** p, paddr_t* pa) {
  if (fail_page_allocs) {
    return ZX_ERR_NO_MEMORY;
  }
  return pmm_alloc_page(mmu_flags, p, pa);
}

static bool test_large_region_unmap() {
  BEGIN_TEST;

  Guard<Mutex> guard{TogglePageAllocLock::Get()};

  static const vaddr_t va = 1UL << PGTABLE_L1_SHIFT;
  // Force a large page.
  static const size_t alloc_size = 1UL << PGTABLE_L2_SHIFT;
  static const vaddr_t alloc_end = va + alloc_size;

  vaddr_t target_vaddrs[] = {
      va,
      va + PAGE_SIZE,
      va + 2 * PAGE_SIZE,
      alloc_end - 3 * PAGE_SIZE,
      alloc_end - 2 * PAGE_SIZE,
      alloc_end - PAGE_SIZE,
  };

  for (unsigned i = 0; i < ktl::size(target_vaddrs); i++) {
    fail_page_allocs = false;
    ArchVmAspace aspace(kAspaceBase, kAspaceSize, 0, toggle_page_alloc_fn);
    zx_status_t err = aspace.Init();
    EXPECT_EQ(err, ZX_OK, "init aspace");

    const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

    size_t mapped;
    // Use MapContiguous to create a mapping that should get backed by a large page.
    err = aspace.MapContiguous(va, 0, alloc_size / PAGE_SIZE, arch_rw_flags, &mapped);
    EXPECT_EQ(err, ZX_OK, "map large page");
    EXPECT_EQ(mapped, 512u, "map large page");

    // Unmap a single small page out of the larger page.
    err = aspace.Unmap(target_vaddrs[i], 1, ArchVmAspace::EnlargeOperation::Yes, &mapped);
    EXPECT_EQ(err, ZX_OK, "unmap single page");
    EXPECT_EQ(mapped, 1u, "unmap single page");

    // Ensure the single page was unmapped, but the rest of the large page is still present.
    for (unsigned j = 0; j < ktl::size(target_vaddrs); j++) {
      uint retrieved_flags = 0;
      paddr_t pa;
      zx_status_t result = aspace.Query(target_vaddrs[j], &pa, &retrieved_flags);
      EXPECT_EQ(i == j ? ZX_ERR_NOT_FOUND : ZX_OK, result, "query page");
    }

    err = aspace.Unmap(va, alloc_size / PAGE_SIZE, ArchVmAspace::EnlargeOperation::Yes, &mapped);
    EXPECT_EQ(err, ZX_OK, "unmap remaining pages");
    EXPECT_EQ(mapped, 512u, "unmap remaining pages");

    // Map in the large page again.
    err = aspace.MapContiguous(va, 0, alloc_size / PAGE_SIZE, arch_rw_flags, &mapped);
    EXPECT_EQ(err, ZX_OK, "map large page");
    EXPECT_EQ(mapped, 512u, "map large page");

    // Simulate OOM by failing allocations.
    fail_page_allocs = true;
    // Attempt to unmap a single small page, but allow over unmapping.
    err = aspace.Unmap(target_vaddrs[i], 1, ArchVmAspace::EnlargeOperation::Yes, &mapped);
    EXPECT_EQ(err, ZX_OK, "unmap single page");
    EXPECT_EQ(mapped, 1u, "unmap single page");

    // The entire large page should have ended up unmapped.
    for (unsigned j = 0; j < ktl::size(target_vaddrs); j++) {
      uint retrieved_flags = 0;
      paddr_t pa;
      zx_status_t result = aspace.Query(target_vaddrs[j], &pa, &retrieved_flags);
      EXPECT_EQ(ZX_ERR_NOT_FOUND, result, "query page");
    }

    err = aspace.Unmap(va, alloc_size / PAGE_SIZE, ArchVmAspace::EnlargeOperation::Yes, &mapped);
    EXPECT_EQ(err, ZX_OK, "unmap remaining pages");
    EXPECT_EQ(mapped, 512u, "unmap remaining pages");

    // Map in the large page again.
    fail_page_allocs = false;
    err = aspace.MapContiguous(va, 0, alloc_size / PAGE_SIZE, arch_rw_flags, &mapped);
    EXPECT_EQ(err, ZX_OK, "map large page");
    EXPECT_EQ(mapped, 512u, "map large page");

    // Simulate OOM by failing allocations.
    fail_page_allocs = true;
    // Attempt to unmap a single small page, but disallow over unmapping.
    err = aspace.Unmap(target_vaddrs[i], 1, ArchVmAspace::EnlargeOperation::No, &mapped);
    EXPECT_EQ(err, ZX_ERR_NO_MEMORY, "unmap single page");

    // All mappings should still be present.
    // The entire large page should have ended up unmapped.
    for (unsigned j = 0; j < ktl::size(target_vaddrs); j++) {
      uint retrieved_flags = 0;
      paddr_t pa;
      zx_status_t result = aspace.Query(target_vaddrs[j], &pa, &retrieved_flags);
      EXPECT_EQ(ZX_OK, result, "query page");
    }

    err = aspace.Unmap(va, alloc_size / PAGE_SIZE, ArchVmAspace::EnlargeOperation::Yes, &mapped);
    EXPECT_EQ(err, ZX_OK, "unmap remaining pages");
    EXPECT_EQ(mapped, 512u, "unmap remaining pages");
    err = aspace.Destroy();
    EXPECT_EQ(err, ZX_OK, "destroy aspace");
  }

  END_TEST;
}

static list_node node = LIST_INITIAL_VALUE(node);
static zx_status_t test_page_alloc_fn(uint unused, vm_page** p, paddr_t* pa) {
  if (list_is_empty(&node)) {
    return ZX_ERR_NO_MEMORY;
  }
  vm_page_t* page = list_remove_head_type(&node, vm_page_t, queue_node);
  if (p) {
    *p = page;
  }
  if (pa) {
    *pa = page->paddr();
  }
  return ZX_OK;
}

static bool test_mapping_oom() {
  BEGIN_TEST;

  constexpr uint64_t kMappingPageCount = 8;
  constexpr uint64_t kMappingSize = kMappingPageCount * PAGE_SIZE;
  constexpr vaddr_t kMappingStart = (1UL << PGTABLE_L1_SHIFT) - kMappingSize / 2;

  // Allocate the pages which will be mapped into the test aspace.
  vm_page_t* mapping_pages[kMappingPageCount] = {};
  paddr_t mapping_paddrs[kMappingPageCount] = {};

  auto undo = fit::defer([&]() {
    for (vm_page_t* mapping_page : mapping_pages) {
      if (mapping_page) {
        pmm_free_page(mapping_page);
      }
    }
  });

  for (unsigned i = 0; i < kMappingPageCount; i++) {
    ASSERT_EQ(pmm_alloc_page(0, mapping_pages + i, mapping_paddrs + i), ZX_OK);
  }

  // Try to create the mapping with a limited number of pages available to
  // the aspace. Start with only 1 available and continue until the map operation
  // succeeds without running out of memory.
  bool map_success = false;
  uint64_t avail_mmu_pages = 1;
  while (!map_success) {
    for (unsigned i = 0; i < avail_mmu_pages; i++) {
      vm_page_t* page;
      ASSERT_EQ(pmm_alloc_page(0, &page), ZX_OK, "alloc fail");
      list_add_head(&node, &page->queue_node);
    }

    ArchVmAspace aspace(kAspaceBase, kAspaceSize, 0, test_page_alloc_fn);
    zx_status_t err = aspace.Init();
    ASSERT_EQ(err, ZX_OK, "init aspace");

    const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

    size_t mapped;
    err = aspace.Map(kMappingStart, mapping_paddrs, kMappingPageCount, arch_rw_flags,
                     ArchVmAspace::ExistingEntryAction::Error, &mapped);
    if (err == ZX_OK) {
      map_success = true;
      size_t unmapped;
      EXPECT_EQ(aspace.Unmap(kMappingStart, kMappingPageCount, ArchVmAspace::EnlargeOperation::Yes,
                             &unmapped),
                ZX_OK);
      EXPECT_EQ(unmapped, kMappingPageCount);
    } else {
      EXPECT_EQ(err, ZX_ERR_NO_MEMORY);
      avail_mmu_pages++;
      // validate that all of the pages were consumed
      EXPECT_TRUE(list_is_empty(&node));
    }

    // Destroying the aspace verifies that everything was cleaned up
    // when the mapping failed part way through.
    err = aspace.Destroy();
    ASSERT_EQ(err, ZX_OK, "destroy aspace");
    ASSERT_TRUE(list_is_empty(&node));
  }

  END_TEST;
}

static bool test_skip_existing_mapping() {
  BEGIN_TEST;

  constexpr vaddr_t kMapBase = kAspaceBase;
  constexpr paddr_t kPhysBase = 0;
  constexpr size_t kNumPages = 8;
  constexpr size_t kMidPage = kNumPages / 2;

  ArchVmAspace aspace(kAspaceBase, kAspaceSize, 0);
  zx_status_t err = aspace.Init();
  EXPECT_EQ(err, ZX_OK);

  const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

  paddr_t page_addresses[kNumPages];
  for (size_t i = 0; i < kNumPages; i++) {
    page_addresses[i] = kPhysBase + (PAGE_SIZE * i);
  }

  size_t mapped;

  // Map in the middle page by itself first, using the final settings.
  err = aspace.Map(kMapBase + kMidPage * PAGE_SIZE, &page_addresses[kMidPage], 1, arch_rw_flags,
                   ArchVmAspace::ExistingEntryAction::Error, &mapped);
  EXPECT_EQ(err, ZX_OK);

  // Now map in all the pages.
  err = aspace.Map(kMapBase, page_addresses, kNumPages, arch_rw_flags,
                   ArchVmAspace::ExistingEntryAction::Skip, &mapped);
  EXPECT_EQ(err, ZX_OK);

  // Validate all the pages.
  for (size_t i = 0; i < kNumPages; i++) {
    paddr_t paddr;
    uint flags;
    err = aspace.Query(kMapBase + i * PAGE_SIZE, &paddr, &flags);
    EXPECT_EQ(err, ZX_OK);
    EXPECT_EQ(paddr, page_addresses[i]);
    EXPECT_EQ(flags, arch_rw_flags);
  }
  err = aspace.Unmap(kMapBase, kNumPages, ArchVmAspace::EnlargeOperation::Yes, &mapped);
  EXPECT_EQ(err, ZX_OK);

  // Now try mapping in the midle page with different permissions.
  err = aspace.Map(kMapBase + kMidPage * PAGE_SIZE, &page_addresses[kMidPage], 1,
                   ARCH_MMU_FLAG_PERM_READ, ArchVmAspace::ExistingEntryAction::Error, &mapped);
  EXPECT_EQ(err, ZX_OK);
  err = aspace.Map(kMapBase, page_addresses, kNumPages, arch_rw_flags,
                   ArchVmAspace::ExistingEntryAction::Skip, &mapped);
  EXPECT_EQ(err, ZX_OK);
  for (size_t i = 0; i < kNumPages; i++) {
    paddr_t paddr;
    uint flags;
    err = aspace.Query(kMapBase + i * PAGE_SIZE, &paddr, &flags);
    EXPECT_EQ(err, ZX_OK);
    EXPECT_EQ(paddr, page_addresses[i]);
    if (i == kMidPage) {
      EXPECT_EQ(flags, ARCH_MMU_FLAG_PERM_READ);
    } else {
      EXPECT_EQ(flags, arch_rw_flags);
    }
  }
  err = aspace.Unmap(kMapBase, kNumPages, ArchVmAspace::EnlargeOperation::Yes, &mapped);
  EXPECT_EQ(err, ZX_OK);

  // Now map the middle page using a completely different physical address.
  paddr_t other_paddr = PAGE_SIZE * 42;
  err = aspace.Map(kMapBase + kMidPage * PAGE_SIZE, &other_paddr, 1, ARCH_MMU_FLAG_PERM_READ,
                   ArchVmAspace::ExistingEntryAction::Error, &mapped);
  EXPECT_EQ(err, ZX_OK);
  err = aspace.Map(kMapBase, page_addresses, kNumPages, arch_rw_flags,
                   ArchVmAspace::ExistingEntryAction::Skip, &mapped);
  EXPECT_EQ(err, ZX_OK);
  for (size_t i = 0; i < kNumPages; i++) {
    paddr_t paddr;
    uint flags;
    err = aspace.Query(kMapBase + i * PAGE_SIZE, &paddr, &flags);
    EXPECT_EQ(err, ZX_OK);
    if (i == kMidPage) {
      EXPECT_EQ(flags, ARCH_MMU_FLAG_PERM_READ);
      EXPECT_EQ(paddr, other_paddr);
    } else {
      EXPECT_EQ(flags, arch_rw_flags);
      EXPECT_EQ(paddr, page_addresses[i]);
    }
  }
  err = aspace.Unmap(kMapBase, kNumPages, ArchVmAspace::EnlargeOperation::Yes, &mapped);
  EXPECT_EQ(err, ZX_OK);

  err = aspace.Destroy();
  EXPECT_EQ(err, ZX_OK);

  END_TEST;
}

// Attempts to validate that unmapping part of a large page will not cause parallel threads
// accessing other parts of the page to fault. This test is only probabilistic and is heavily timing
// and micro architectural dependent, but could serve as a canary.
static bool test_large_region_atomic() {
  BEGIN_TEST;

  // Force a large page.
  static constexpr size_t alloc_size = 1UL << PGTABLE_L2_SHIFT;

  static constexpr size_t target_offsets[] = {
      0,
      PAGE_SIZE,
      2 * PAGE_SIZE,
      alloc_size - 3 * PAGE_SIZE,
      alloc_size - 2 * PAGE_SIZE,
      alloc_size - PAGE_SIZE,
  };

  for (unsigned i = 0; i < ktl::size(target_offsets); i++) {
    // Allocate a large page in the current kernel aspace. Need to allocate in the current aspace
    // and not a test aspace so that we can directly access the mappings.
    auto kaspace = VmAspace::kernel_aspace();
    fbl::RefPtr<VmAddressRegion> vmar = kaspace->RootVmar();
    fbl::RefPtr<VmObjectPaged> vmo;

    zx_status_t status =
        VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, VmObjectPaged::kAlwaysPinned, alloc_size, &vmo);
    ASSERT_OK(status);

    const uint arch_rw_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;

    fbl::RefPtr<VmMapping> mapping;
    status = vmar->CreateVmMapping(
        0, alloc_size, 0,
        VMAR_FLAG_CAN_MAP_READ | VMAR_FLAG_CAN_MAP_WRITE | VMAR_FLAG_DEBUG_DYNAMIC_KERNEL_MAPPING,
        vmo, 0, arch_rw_flags, "test", &mapping);
    ASSERT_OK(status);

    status = mapping->MapRange(0, alloc_size, false);
    ASSERT_OK(status);

    const vaddr_t va = mapping->base();

    auto cleanup_mapping = fit::defer([&] { mapping->Destroy(); });

    // Spin up a thread to start touching pages in the mapping.
    struct State {
      vaddr_t va;
      uint current_offset;
      ktl::atomic<bool> running;
    } state = {va, i, true};
    auto thread_body = [](void* arg) -> int {
      State* state = static_cast<State*>(arg);

      while (state->running) {
        for (unsigned i = 0; i < ktl::size(target_offsets); i++) {
          if (state->current_offset == i) {
            continue;
          }
          volatile uint64_t* addr = reinterpret_cast<uint64_t*>(state->va + target_offsets[i]);
          // Force read from the address
          asm volatile("" ::"r"(*addr));
        }
      }
      return 0;
    };

    Thread* thread = Thread::Create("test-thread", thread_body, &state, DEFAULT_PRIORITY);
    ASSERT_NONNULL(thread);
    thread->Resume();

    auto cleanup_thread = fit::defer([&]() {
      state.running = false;
      thread->Join(nullptr, ZX_TIME_INFINITE);
    });

    // Wait a moment to let the other thread start touching.
    Thread::Current::SleepRelative(ZX_MSEC(50));

    // Unmap a single page.
    status = kaspace->arch_aspace().Unmap(va + target_offsets[i], 1,
                                          ArchVmAspace::EnlargeOperation::No, nullptr);
    EXPECT_EQ(status, ZX_OK, "unmap single page");

    // If the other thread didn't cause a kernel panic by having a page fault, then success.
  }

  END_TEST;
}

UNITTEST_START_TESTCASE(mmu_tests)
UNITTEST("create large unaligned region and ensure it can be unmapped", test_large_unaligned_region)
UNITTEST("create large unaligned region without mapping and ensure it can be unmapped",
         test_large_unaligned_region_without_map)
UNITTEST("creating large vm region, and change permissions", test_large_region_protect)
UNITTEST("trigger oom failures when creating a mapping", test_mapping_oom)
UNITTEST("skip existing entry when mapping multiple pages", test_skip_existing_mapping)
UNITTEST("create large vm region and unmap single pages", test_large_region_unmap)
UNITTEST("splitting a large page is atomic", test_large_region_atomic)
UNITTEST_END_TESTCASE(mmu_tests, "mmu", "mmu tests")
