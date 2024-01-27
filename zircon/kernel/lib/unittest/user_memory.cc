// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/fit/defer.h>
#include <lib/unittest/unittest.h>
#include <lib/unittest/user_memory.h>

namespace testing {

UserMemory::~UserMemory() {
  zx_status_t status = mapping_->Destroy();
  DEBUG_ASSERT(status == ZX_OK);
}

// static
ktl::unique_ptr<UserMemory> UserMemory::CreateInAspace(fbl::RefPtr<VmObject> vmo,
                                                       fbl::RefPtr<VmAspace> &aspace, uint8_t tag) {
  size_t size = vmo->size();

  DEBUG_ASSERT(aspace);
  DEBUG_ASSERT(aspace->is_user());
  fbl::RefPtr<VmAddressRegion> root_vmar = aspace->RootVmar();
  DEBUG_ASSERT(root_vmar);
  constexpr uint32_t vmar_flags =
      VMAR_FLAG_CAN_MAP_READ | VMAR_FLAG_CAN_MAP_WRITE | VMAR_FLAG_CAN_MAP_EXECUTE;
  fbl::RefPtr<VmMapping> mapping;
  constexpr uint arch_mmu_flags =
      ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;
  zx_status_t status =
      root_vmar->CreateVmMapping(/* offset= */ 0, size, /* align_pow2= */ 0, vmar_flags, vmo, 0,
                                 arch_mmu_flags, "unittest", &mapping);
  if (status != ZX_OK) {
    unittest_printf("CreateVmMapping failed: %d\n", status);
    return nullptr;
  }
  auto unmap = fit::defer([&]() {
    if (mapping) {
      zx_status_t status = mapping->Destroy();
      DEBUG_ASSERT(status == ZX_OK);
    }
  });

  fbl::AllocChecker ac;
  ktl::unique_ptr<UserMemory> mem(new (&ac) UserMemory(mapping, vmo, tag));
  if (!ac.check()) {
    unittest_printf("failed to allocate from heap\n");
    return nullptr;
  }
  // Unmapping is now UserMemory's responsibility.
  unmap.cancel();

  return mem;
}

// static
ktl::unique_ptr<UserMemory> UserMemory::Create(fbl::RefPtr<VmObject> vmo, uint8_t tag) {
  fbl::RefPtr<VmAspace> aspace(Thread::Current::Get()->aspace());
  DEBUG_ASSERT(aspace);

  return CreateInAspace(ktl::move(vmo), aspace, tag);
}

// static
ktl::unique_ptr<UserMemory> UserMemory::Create(size_t size) {
  size = ROUNDUP_PAGE_SIZE(size);

  fbl::RefPtr<VmObjectPaged> vmo;
  zx_status_t status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, 0u, size,
                                             AttributionObject::GetKernelAttribution(), &vmo);
  if (status != ZX_OK) {
    unittest_printf("VmObjectPaged::Create failed: %d\n", status);
    return nullptr;
  }
  return Create(ktl::move(vmo));
}

}  // namespace testing
