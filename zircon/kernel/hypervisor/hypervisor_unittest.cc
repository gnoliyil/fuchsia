// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <lib/unittest/unittest.h>
#include <zircon/errors.h>
#include <zircon/syscalls/hypervisor.h>
#include <zircon/types.h>

#include <hypervisor/guest_physical_address_space.h>
#include <hypervisor/id_allocator.h>
#include <hypervisor/interrupt_tracker.h>
#include <hypervisor/trap_map.h>
#include <vm/pmm.h>
#include <vm/scanner.h>
#include <vm/vm.h>
#include <vm/vm_address_region.h>
#include <vm/vm_aspace.h>
#include <vm/vm_object.h>
#include <vm/vm_object_paged.h>

static constexpr uint kMmuFlags =
    ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE | ARCH_MMU_FLAG_PERM_EXECUTE;

static bool hypervisor_supported() {
#if ARCH_ARM64
  if (arm64_get_boot_el() < 2) {
    unittest_printf("Hypervisor not supported\n");
    return false;
  }
#endif
  return true;
}

static zx::status<hypervisor::GuestPhysicalAddressSpace> create_gpas() {
#if ARCH_ARM64
  return hypervisor::GuestPhysicalAddressSpace::Create(1 /* vmid */);
#elif ARCH_X86
  return hypervisor::GuestPhysicalAddressSpace::Create();
#endif
}

static zx_status_t create_vmo(size_t vmo_size, fbl::RefPtr<VmObjectPaged>* vmo) {
  return VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, 0u, vmo_size, vmo);
}

static zx_status_t commit_vmo(fbl::RefPtr<VmObjectPaged> vmo) {
  return vmo->CommitRange(0, vmo->size());
}

static zx_status_t create_mapping(fbl::RefPtr<VmAddressRegion> vmar, fbl::RefPtr<VmObjectPaged> vmo,
                                  zx_gpaddr_t addr, uint mmu_flags = kMmuFlags) {
  fbl::RefPtr<VmMapping> mapping;
  return vmar->CreateVmMapping(addr, vmo->size(), 0 /* align_pow2 */, VMAR_FLAG_SPECIFIC, vmo,
                               0 /* vmo_offset */, mmu_flags, "vmo", &mapping);
}

static zx_status_t create_sub_vmar(fbl::RefPtr<VmAddressRegion> vmar, size_t offset, size_t size,
                                   fbl::RefPtr<VmAddressRegion>* sub_vmar) {
  return vmar->CreateSubVmar(offset, size, 0 /* align_pow2 */, vmar->flags() | VMAR_FLAG_SPECIFIC,
                             "vmar", sub_vmar);
}

static bool guest_physical_address_space_unmap_range() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  // Unmap page.
  auto result = gpas->UnmapRange(0, PAGE_SIZE);
  EXPECT_EQ(ZX_OK, result.status_value(), "Failed to unmap page from GuestPhysicalAddressSpace\n");

  // Verify IsMapped for unmapped address fails.
  EXPECT_FALSE(gpas->IsMapped(0), "Expected address to be unmapped\n");

  END_TEST;
}

static bool guest_physical_address_space_unmap_range_outside_of_mapping() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  // Unmap page.
  auto result = gpas->UnmapRange(PAGE_SIZE * 8, PAGE_SIZE);
  EXPECT_EQ(ZX_OK, result.status_value(), "Failed to unmap page from GuestPhysicalAddressSpace\n");

  END_TEST;
}

static bool guest_physical_address_space_unmap_range_multiple_mappings() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");

  fbl::RefPtr<VmObjectPaged> vmo1;
  zx_status_t status = create_vmo(PAGE_SIZE * 2, &vmo1);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(gpas->RootVmar(), vmo1, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  fbl::RefPtr<VmObjectPaged> vmo2;
  status = create_vmo(PAGE_SIZE * 2, &vmo2);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(gpas->RootVmar(), vmo2, PAGE_SIZE * 3);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  // Unmap pages.
  auto result = gpas->UnmapRange(PAGE_SIZE, PAGE_SIZE * 3);
  EXPECT_EQ(ZX_OK, result.status_value(),
            "Failed to multiple unmap pages from GuestPhysicalAddressSpace\n");

  // Verify IsMapped for unmapped addresses fails.
  for (zx_gpaddr_t addr = PAGE_SIZE; addr < PAGE_SIZE * 4; addr += PAGE_SIZE) {
    EXPECT_FALSE(gpas->IsMapped(addr), "Expected address to be unmapped\n");
  }

  // Verify IsMapped for mapped addresses succeeds.
  EXPECT_TRUE(gpas->IsMapped(0), "Expected address to be mapped\n");
  EXPECT_TRUE(gpas->IsMapped(PAGE_SIZE * 4), "Expected address to be mapped\n");

  END_TEST;
}

static bool guest_physical_address_space_unmap_range_sub_region() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  fbl::RefPtr<VmAddressRegion> root_vmar = gpas->RootVmar();
  // To test partial unmapping within sub-VMAR:
  // Sub-VMAR from [0, PAGE_SIZE * 2).
  // Map within sub-VMAR from [PAGE_SIZE, PAGE_SIZE * 2).
  fbl::RefPtr<VmAddressRegion> sub_vmar1;
  zx_status_t status = create_sub_vmar(root_vmar, 0, PAGE_SIZE * 2, &sub_vmar1);
  EXPECT_EQ(ZX_OK, status, "Failed to create sub-VMAR\n");
  EXPECT_TRUE(sub_vmar1->has_parent(), "Sub-VMAR does not have a parent");
  fbl::RefPtr<VmObjectPaged> vmo1;
  status = create_vmo(PAGE_SIZE, &vmo1);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(sub_vmar1, vmo1, PAGE_SIZE);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");
  // To test destroying of sub-VMAR:
  // Sub-VMAR from [PAGE_SIZE * 2, PAGE_SIZE * 3).
  // Map within sub-VMAR from [0, PAGE_SIZE).
  fbl::RefPtr<VmAddressRegion> sub_vmar2;
  status = create_sub_vmar(root_vmar, PAGE_SIZE * 2, PAGE_SIZE, &sub_vmar2);
  EXPECT_EQ(ZX_OK, status, "Failed to create sub-VMAR\n");
  EXPECT_TRUE(sub_vmar2->has_parent(), "Sub-VMAR does not have a parent");
  fbl::RefPtr<VmObjectPaged> vmo2;
  status = create_vmo(PAGE_SIZE, &vmo2);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(sub_vmar2, vmo2, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");
  // To test partial unmapping within root-VMAR:
  // Map within root-VMAR from [PAGE_SIZE * 3, PAGE_SIZE * 5).
  fbl::RefPtr<VmObjectPaged> vmo3;
  status = create_vmo(PAGE_SIZE * 2, &vmo3);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(root_vmar, vmo3, PAGE_SIZE * 3);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  // Unmap pages from [PAGE_SIZE, PAGE_SIZE * 4).
  auto result = gpas->UnmapRange(PAGE_SIZE, PAGE_SIZE * 3);
  EXPECT_EQ(ZX_OK, result.status_value(),
            "Failed to multiple unmap pages from GuestPhysicalAddressSpace\n");

  // Verify IsMapped for unmapped addresses fails.
  for (zx_gpaddr_t addr = 0; addr < PAGE_SIZE * 4; addr += PAGE_SIZE) {
    EXPECT_FALSE(gpas->IsMapped(addr), "Expected address to be unmapped\n");
  }

  // Verify IsMapped for mapped addresses succeeds.
  EXPECT_TRUE(gpas->IsMapped(PAGE_SIZE * 4), "Expected address to be mapped\n");

  // Verify that sub-VMARs still have a parent.
  EXPECT_TRUE(sub_vmar1->has_parent(), "Sub-VMAR does not have a parent");
  EXPECT_TRUE(sub_vmar2->has_parent(), "Sub-VMAR does not have a parent");

  END_TEST;
}

static bool guest_phyiscal_address_single_vmo_multiple_mappings() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  AutoVmScannerDisable scanner_disable;

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");

  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE * 4, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");

  // Map a single page of this four page VMO at offset 0x1000 and offset 0x3000.
  fbl::RefPtr<VmMapping> mapping;
  status = gpas->RootVmar()->CreateVmMapping(PAGE_SIZE, PAGE_SIZE, 0 /* align_pow2 */,
                                             VMAR_FLAG_SPECIFIC, vmo, PAGE_SIZE, kMmuFlags, "vmo",
                                             &mapping);
  EXPECT_EQ(ZX_OK, status, "Failed to create first mapping\n");
  status = gpas->RootVmar()->CreateVmMapping(PAGE_SIZE * 3, PAGE_SIZE, 0 /* align_pow2 */,
                                             VMAR_FLAG_SPECIFIC, vmo, PAGE_SIZE * 3, kMmuFlags,
                                             "vmo", &mapping);
  EXPECT_EQ(ZX_OK, status, "Failed to create second mapping\n");

  status = commit_vmo(vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to commit VMO\n");

  // No mapping at 0x0 or 0x2000.
  EXPECT_FALSE(gpas->IsMapped(0), "Expected address to be unmapped\n");
  EXPECT_FALSE(gpas->IsMapped(PAGE_SIZE * 2), "Expected address to be unmapped\n");

  // There is a mapping at 0x1000 and 0x3000.
  EXPECT_TRUE(gpas->IsMapped(PAGE_SIZE), "Expected address to be mapped\n");
  EXPECT_TRUE(gpas->IsMapped(PAGE_SIZE * 3), "Expected address to be mapped\n");

  END_TEST;
}

static bool guest_physical_address_space_page_fault() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");
  status = create_mapping(gpas->RootVmar(), vmo, PAGE_SIZE, ARCH_MMU_FLAG_PERM_READ);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");
  status = create_mapping(gpas->RootVmar(), vmo, PAGE_SIZE * 2,
                          ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");
  status = create_mapping(gpas->RootVmar(), vmo, PAGE_SIZE * 3,
                          ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_EXECUTE);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  // Fault in each page.
  for (zx_gpaddr_t addr = 0; addr < PAGE_SIZE * 4; addr += PAGE_SIZE) {
    auto result = gpas->PageFault(addr);
    EXPECT_EQ(ZX_OK, result.status_value(), "Failed to fault page\n");
  }

  END_TEST;
}

static bool guest_physical_address_space_map_interrupt_controller() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  // Allocate a page to use as the interrupt controller.
  paddr_t paddr = 0;
  vm_page* vm_page;
  status = pmm_alloc_page(0, &vm_page, &paddr);
  EXPECT_EQ(ZX_OK, status, "Unable to allocate a page\n");

  // Map interrupt controller page in an arbitrary location.
  const vaddr_t kGicvAddress = 0x800001000;
  auto result = gpas->MapInterruptController(kGicvAddress, paddr, PAGE_SIZE);
  EXPECT_EQ(ZX_OK, result.status_value(), "Failed to map APIC page\n");

  // Cleanup
  pmm_free_page(vm_page);
  END_TEST;
}

static bool guest_physical_address_space_uncached() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = vmo->SetMappingCachePolicy(ZX_CACHE_POLICY_UNCACHED);
  EXPECT_EQ(ZX_OK, status, "Failed to set cache policy\n");

  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  END_TEST;
}

static bool guest_physical_address_space_uncached_device() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = vmo->SetMappingCachePolicy(ZX_CACHE_POLICY_UNCACHED_DEVICE);
  EXPECT_EQ(ZX_OK, status, "Failed to set cache policy\n");

  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  END_TEST;
}

static bool guest_physical_address_space_write_combining() {
  BEGIN_TEST;

  if (!hypervisor_supported()) {
    return true;
  }

  // Setup.
  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = create_vmo(PAGE_SIZE, &vmo);
  EXPECT_EQ(ZX_OK, status, "Failed to create VMO\n");
  status = vmo->SetMappingCachePolicy(ZX_CACHE_POLICY_WRITE_COMBINING);
  EXPECT_EQ(ZX_OK, status, "Failed to set cache policy\n");

  auto gpas = create_gpas();
  EXPECT_EQ(ZX_OK, gpas.status_value(), "Failed to create GuestPhysicalAddressSpace\n");
  status = create_mapping(gpas->RootVmar(), vmo, 0);
  EXPECT_EQ(ZX_OK, status, "Failed to create mapping\n");

  END_TEST;
}

static bool id_allocator_reset_to_size() {
  BEGIN_TEST;

  constexpr uint8_t kMaxId = sizeof(size_t);
  constexpr uint8_t kMinId = 1;
  hypervisor::IdAllocator<uint8_t, UINT8_MAX, kMinId> allocator;
  allocator.Reset(kMaxId);

  // Allocate until all IDs are used.
  for (uint8_t i = kMinId; i < kMaxId; i++) {
    auto id = allocator.Alloc();
    EXPECT_EQ(ZX_OK, id.status_value());
    EXPECT_EQ(i, *id);
  }

  // Allocate when no IDs are free.
  auto id = allocator.Alloc();
  EXPECT_EQ(ZX_ERR_NO_RESOURCES, id.status_value());

  // Free an ID that was never allocated.
  auto result = allocator.Free(kMaxId + 1);
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, result.status_value());

  // Free a random ID, and then check that it gets reallocated.
  constexpr uint8_t kIdx = kMaxId / 2;
  result = allocator.Free(kIdx);
  EXPECT_EQ(ZX_OK, result.status_value());
  id = allocator.Alloc();
  EXPECT_EQ(ZX_OK, id.status_value());
  EXPECT_EQ(kIdx, *id);

  END_TEST;
}

static bool interrupt_bitmap() {
  BEGIN_TEST;

  hypervisor::InterruptBitmap<8> bitmap;

  uint32_t vector = UINT32_MAX;
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(0));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(1));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Scan(&vector));
  EXPECT_EQ(UINT32_MAX, vector);

  // Index 0.
  vector = UINT32_MAX;
  bitmap.Set(0u, hypervisor::InterruptType::VIRTUAL);
  EXPECT_EQ(hypervisor::InterruptType::VIRTUAL, bitmap.Get(0));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(1));
  EXPECT_EQ(hypervisor::InterruptType::VIRTUAL, bitmap.Scan(&vector));
  EXPECT_EQ(0u, vector);

  vector = UINT32_MAX;
  bitmap.Set(0u, hypervisor::InterruptType::PHYSICAL);
  EXPECT_EQ(hypervisor::InterruptType::PHYSICAL, bitmap.Get(0u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(1u));
  EXPECT_EQ(hypervisor::InterruptType::PHYSICAL, bitmap.Scan(&vector));
  EXPECT_EQ(0u, vector);

  vector = UINT32_MAX;
  bitmap.Set(0u, hypervisor::InterruptType::INACTIVE);
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(0u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(1u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Scan(&vector));
  EXPECT_EQ(UINT32_MAX, vector);

  // Index 1.
  vector = UINT32_MAX;
  bitmap.Set(1u, hypervisor::InterruptType::VIRTUAL);
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(0u));
  EXPECT_EQ(hypervisor::InterruptType::VIRTUAL, bitmap.Get(1u));
  EXPECT_EQ(hypervisor::InterruptType::VIRTUAL, bitmap.Scan(&vector));
  EXPECT_EQ(1u, vector);

  vector = UINT32_MAX;
  bitmap.Set(1u, hypervisor::InterruptType::PHYSICAL);
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(0u));
  EXPECT_EQ(hypervisor::InterruptType::PHYSICAL, bitmap.Get(1u));
  EXPECT_EQ(hypervisor::InterruptType::PHYSICAL, bitmap.Scan(&vector));
  EXPECT_EQ(1u, vector);

  vector = UINT32_MAX;
  bitmap.Set(1u, hypervisor::InterruptType::INACTIVE);
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(0u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(1u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Scan(&vector));
  EXPECT_EQ(UINT32_MAX, vector);

  // Clear
  bitmap.Set(0u, hypervisor::InterruptType::VIRTUAL);
  bitmap.Set(1u, hypervisor::InterruptType::VIRTUAL);
  bitmap.Set(2u, hypervisor::InterruptType::PHYSICAL);
  bitmap.Set(3u, hypervisor::InterruptType::PHYSICAL);
  bitmap.Clear(1u, 3u);
  EXPECT_EQ(hypervisor::InterruptType::VIRTUAL, bitmap.Get(0u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(1u));
  EXPECT_EQ(hypervisor::InterruptType::INACTIVE, bitmap.Get(2u));
  EXPECT_EQ(hypervisor::InterruptType::PHYSICAL, bitmap.Get(3u));

  END_TEST;
}

static bool trap_map_insert_trap_intersecting() {
  BEGIN_TEST;

  hypervisor::TrapMap trap_map;
  // Add traps:
  // 1. [10, 19]
  // 2. [20, 29]
  // 3. [35, 5]
  EXPECT_EQ(ZX_OK, trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 10, 10, nullptr, 0).status_value());
  EXPECT_EQ(ZX_OK, trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 20, 10, nullptr, 0).status_value());
  EXPECT_EQ(ZX_OK, trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 35, 5, nullptr, 0).status_value());
  // Trap at [0, 10] intersects with trap 1.
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 0, 11, nullptr, 0).status_value());
  // Trap at [10, 19] intersects with trap 1.
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 10, 10, nullptr, 0).status_value());
  // Trap at [11, 18] intersects with trap 1.
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 11, 8, nullptr, 0).status_value());
  // Trap at [15, 24] intersects with trap 1 and trap 2.
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 15, 10, nullptr, 0).status_value());
  // Trap at [30, 39] intersects with trap 3.
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 30, 10, nullptr, 0).status_value());
  // Trap at [36, 40] intersects with trap 3.
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 36, 5, nullptr, 0).status_value());

  // Add a trap at the beginning.
  EXPECT_EQ(ZX_OK, trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 0, 10, nullptr, 0).status_value());
  // In the gap.
  EXPECT_EQ(ZX_OK, trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 30, 5, nullptr, 0).status_value());
  // And at the end.
  EXPECT_EQ(ZX_OK, trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 40, 10, nullptr, 0).status_value());

  END_TEST;
}

static bool trap_map_insert_trap_out_of_range() {
  BEGIN_TEST;

  hypervisor::TrapMap trap_map;
  EXPECT_EQ(ZX_ERR_OUT_OF_RANGE,
            trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, 0, 0, nullptr, 0).status_value());
  EXPECT_EQ(
      ZX_ERR_OUT_OF_RANGE,
      trap_map.InsertTrap(ZX_GUEST_TRAP_MEM, UINT32_MAX, UINT64_MAX, nullptr, 0).status_value());
#ifdef ARCH_X86
  EXPECT_EQ(ZX_ERR_OUT_OF_RANGE,
            trap_map.InsertTrap(ZX_GUEST_TRAP_IO, 0, UINT32_MAX, nullptr, 0).status_value());
#endif  // ARCH_X86

  END_TEST;
}

// Use the function name as the test name
#define HYPERVISOR_UNITTEST(fname) UNITTEST(#fname, fname)

UNITTEST_START_TESTCASE(hypervisor)
HYPERVISOR_UNITTEST(guest_physical_address_space_unmap_range)
HYPERVISOR_UNITTEST(guest_physical_address_space_unmap_range_outside_of_mapping)
HYPERVISOR_UNITTEST(guest_physical_address_space_unmap_range_multiple_mappings)
HYPERVISOR_UNITTEST(guest_physical_address_space_unmap_range_sub_region)
HYPERVISOR_UNITTEST(guest_phyiscal_address_single_vmo_multiple_mappings)
HYPERVISOR_UNITTEST(guest_physical_address_space_page_fault)
HYPERVISOR_UNITTEST(guest_physical_address_space_map_interrupt_controller)
HYPERVISOR_UNITTEST(guest_physical_address_space_uncached)
HYPERVISOR_UNITTEST(guest_physical_address_space_uncached_device)
HYPERVISOR_UNITTEST(guest_physical_address_space_write_combining)
HYPERVISOR_UNITTEST(id_allocator_reset_to_size)
HYPERVISOR_UNITTEST(interrupt_bitmap)
HYPERVISOR_UNITTEST(trap_map_insert_trap_intersecting)
HYPERVISOR_UNITTEST(trap_map_insert_trap_out_of_range)
UNITTEST_END_TESTCASE(hypervisor, "hypervisor", "Hypervisor unit tests.")
