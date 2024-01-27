// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <debug.h>
#include <lib/boot-options/boot-options.h>
#include <lib/unittest/unittest.h>
#include <platform.h>
#include <pow2.h>
#include <zircon/errors.h>
#include <zircon/time.h>

#include <utility>

#include <fbl/ref_ptr.h>
#include <kernel/thread.h>
#include <object/handle.h>
#include <object/msi_dispatcher.h>
#include <object/msi_interrupt_dispatcher.h>
#include <vm/pmm.h>
#include <vm/vm_address_region.h>
#include <vm/vm_object.h>
#include <vm/vm_object_physical.h>

namespace {

bool MsiIsSupportedTrue() { return true; }
zx_status_t MsiAllocate(uint requested_irqs, bool /*unused*/, bool /*unused*/,
                        msi_block_t* out_block) {
  out_block->allocated = true;
  out_block->base_irq_id = 128;
  out_block->num_irq = requested_irqs;
  out_block->tgt_addr = 0x1234u;
  out_block->tgt_addr = 0x4321u;
  out_block->is_32bit = false;
  return ZX_OK;
}

// These functions are used to verify we properly bail out if MSI isn't supported.
void MsiFree(msi_block_t* block) { block->allocated = false; }
bool MsiIsSupportedFalse() { return false; }
zx_status_t MsiAllocateAssert(uint32_t /* unused */, bool /* unused */, bool /* unused */,
                              msi_block_t* /* unused */) {
  assert(false);
  return ZX_ERR_NOT_SUPPORTED;
}

void MsiFreeAssert(msi_block_t* /* unused */) { assert(false); }

const uint32_t kVectorMax = 256u;

zx_status_t create_allocation(fbl::RefPtr<MsiAllocation>* alloc, uint32_t cnt) {
  return MsiAllocation::Create(cnt, alloc, MsiAllocate, MsiFree, MsiIsSupportedTrue);
}

// Helper function to create a valid vmo / mapping / capability tuple to cut down
// on the duplication within tests.
zx_status_t create_valid_msi_vmo(fbl::RefPtr<VmObject>* out_vmo,
                                 fbl::RefPtr<VmMapping>* out_mapping,
                                 volatile MsiCapability** out_cap) {
  if (!gBootOptions->test_ram_reserve.has_value() ||
      !gBootOptions->test_ram_reserve->paddr.has_value()) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  const zx_paddr_t paddr = *gBootOptions->test_ram_reserve->paddr;
  const size_t vmo_size = PAGE_SIZE;
  fbl::RefPtr<VmObjectPhysical> vmo;
  zx_status_t status = VmObjectPhysical::Create(paddr, vmo_size, &vmo);
  if (status != ZX_OK)
    return status;

  status = vmo->SetMappingCachePolicy(ZX_CACHE_POLICY_UNCACHED_DEVICE);
  if (status != ZX_OK)
    return status;

  fbl::RefPtr<VmMapping> mapping;
  status = VmAspace::kernel_aspace()->RootVmar()->CreateVmMapping(
      /*mapping_offset=*/0, /*size=*/vmo_size, /*align_pow2=*/0, /*vmar_flags=*/0, vmo,
      /*vmo_offset=*/0, ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE,
      /*name=*/nullptr, &mapping);
  if (status == ZX_OK) {
    // Prepopulate the mapping so no page faults are needed in kernel mode.
    status = mapping->MapRange(/*offset=*/0, vmo_size, /*commit=*/true);
  }
  if (status != ZX_OK) {
    return status;
  }

  *out_vmo = ktl::move(vmo);
  *out_mapping = ktl::move(mapping);
  *out_cap = reinterpret_cast<MsiCapability*>(out_mapping->get()->base_locking());
  (*out_cap)->id = kMsiCapabilityId;

  return ZX_OK;
}

bool allocation_creation_and_info_test() {
  BEGIN_TEST;

  const uint32_t test_irq_cnt = 8;
  fbl::RefPtr<MsiAllocation> alloc;
  ASSERT_EQ(ZX_OK, create_allocation(&alloc, test_irq_cnt));

  zx_info_msi_t info = {};
  alloc->GetInfo(&info);

  // Grab the lock and compare the block values and info values to both our test
  // data and info data.
  Guard<SpinLock, IrqSave> guard{&alloc->lock()};
  ASSERT_EQ(test_irq_cnt, alloc->block().num_irq);
  ASSERT_EQ(true, alloc->block().allocated);
  ASSERT_EQ(info.base_irq_id, alloc->block().base_irq_id);
  ASSERT_EQ(info.num_irq, alloc->block().num_irq);
  ASSERT_EQ(info.target_addr, alloc->block().tgt_addr);
  ASSERT_EQ(info.target_data, alloc->block().tgt_data);
  END_TEST;
}

bool allocation_irq_count_test() {
  BEGIN_TEST;

  ResourceDispatcher::ResourceStorage rsrc_storage;
  ASSERT_EQ(ZX_OK, ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_IRQ, 0, kVectorMax,
                                                           &rsrc_storage));
  // Check that the valid range of allocation sizes work. Verifies that only
  // powers of two are valid.
  for (uint32_t cnt = 1; cnt < MsiAllocation::kMsiAllocationCountMax; cnt++) {
    fbl::RefPtr<MsiAllocation> alloc;
    zx_status_t expected = ispow2(cnt) ? ZX_OK : ZX_ERR_INVALID_ARGS;
    ASSERT_EQ(expected,
              MsiAllocation::Create(cnt, &alloc, MsiAllocate, MsiFree, MsiIsSupportedTrue));
  }

  fbl::RefPtr<MsiAllocation> alloc;
  // And check the failure cases.
  ASSERT_EQ(ZX_ERR_INVALID_ARGS,
            MsiAllocation::Create(0, &alloc, MsiAllocate, MsiFree, MsiIsSupportedTrue));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS,
            MsiAllocation::Create(MsiAllocation::kMsiAllocationCountMax + 1, &alloc, MsiAllocate,
                                  MsiFree, MsiIsSupportedTrue));

  END_TEST;
}

bool allocation_reservation_test() {
  BEGIN_TEST;

  fbl::RefPtr<MsiAllocation> alloc;
  ASSERT_EQ(ZX_OK, create_allocation(&alloc, MsiAllocation::kMsiAllocationCountMax));

  // Verify the bounds checking and state of id reservations.
  ASSERT_EQ(ZX_ERR_BAD_STATE, alloc->ReleaseId(0));
  ASSERT_EQ(ZX_OK, alloc->ReserveId(0));
  ASSERT_EQ(ZX_ERR_ALREADY_BOUND, alloc->ReserveId(0));
  ASSERT_EQ(ZX_OK, alloc->ReleaseId(0));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, alloc->ReserveId(MsiAllocation::kMsiAllocationCountMax));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, alloc->ReleaseId(MsiAllocation::kMsiAllocationCountMax));
  END_TEST;
}

bool allocation_support_test() {
  BEGIN_TEST;

  {
    fbl::RefPtr<MsiAllocation> alloc;
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, MsiAllocation::Create(1, &alloc, MsiAllocateAssert,
                                                          MsiFreeAssert, MsiIsSupportedFalse));
  }

  END_TEST;
}

// Use a static var for tracking calls rather than a lambda to avoid storage issues with lambda
// captures and function pointers without having to increase complexity in the dispatcher.
uint32_t register_call_count = 0;
void register_fn(const msi_block_t*, uint, int_handler, void*) { register_call_count++; }

bool interrupt_duplication_test() {
  BEGIN_TEST;
  fbl::RefPtr<MsiAllocation> alloc;
  ASSERT_EQ(ZX_OK, create_allocation(&alloc, MsiAllocation::kMsiAllocationCountMax));

  fbl::RefPtr<VmObject> vmo;
  fbl::RefPtr<VmMapping> mapping;
  volatile MsiCapability* cap;
  if (create_valid_msi_vmo(&vmo, &mapping, &cap) == ZX_ERR_NOT_SUPPORTED) {
    printf("reserved region not available, skipping test");
    END_TEST;
  }

  // Now to the meat of the test. Ensure that two MsiInterruptDispatchers cannot share
  // the same MSI id, and that when a dispatcher is cleaned up it releases the
  // Id reservation in the allocation.
  zx_rights_t rights;
  KernelHandle<InterruptDispatcher> d1, d2;
  ASSERT_EQ(ZX_OK, MsiInterruptDispatcher::Create(alloc, 0, vmo, /*cap_offset=*/0, /*options=*/0,
                                                  &rights, &d1, register_fn));
  ASSERT_EQ(ZX_ERR_ALREADY_BOUND,
            MsiInterruptDispatcher::Create(alloc, 0, vmo, /*cap_offset=*/0,
                                           /*options=*/0, &rights, &d2, register_fn));
  d1.reset();
  ASSERT_EQ(ZX_OK, MsiInterruptDispatcher::Create(alloc, 0, vmo, /*cap_offset=*/0, /*options=*/0,
                                                  &rights, &d2, register_fn));

  END_TEST;
}

bool interrupt_vmo_test() {
  BEGIN_TEST;
  register_call_count = 0;
  fbl::RefPtr<MsiAllocation> alloc;
  ASSERT_EQ(ZX_OK, create_allocation(&alloc, MsiAllocation::kMsiAllocationCountMax));

  // This test emulates a block of MSI interrupts each taking up a given bit in a register for
  // their own masking. It validates that the MsiInterruptDispatcher masks / unmasks the correct
  // bit, and that the operation of the inherited InterruptDispatcher side of things behaves
  // correctly when interrupts are triggered.
  KernelHandle<InterruptDispatcher> interrupt;
  zx_rights_t rights;
  {
    fbl::RefPtr<VmObjectPaged> vmo, vmo_noncontig;
    size_t vmo_size = sizeof(MsiCapability);
    ASSERT_EQ(ZX_OK,
              VmObjectPaged::CreateContiguous(PMM_ALLOC_FLAG_ANY, vmo_size, /*options=*/0, &vmo));
    ASSERT_EQ(ZX_OK,
              VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, /*options=*/0, vmo_size, &vmo_noncontig));

    // This should fail because the VMO is non-contiguous.
    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              MsiInterruptDispatcher::Create(alloc, 0, vmo_noncontig, /*cap_offset=*/0,
                                             /*options=*/0, &rights, &interrupt, register_fn));
    // This should fail because the VMO has not had a cache policy set.
    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              MsiInterruptDispatcher::Create(alloc, 0, vmo, /*cap_offset=*/0, /*options=*/0,
                                             &rights, &interrupt, register_fn));
    ASSERT_EQ(ZX_OK, vmo->SetMappingCachePolicy(ZX_CACHE_POLICY_UNCACHED_DEVICE));
    // Create will still fail because the VMO doesn't look like an MSI capability.
    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              MsiInterruptDispatcher::Create(alloc, 0, vmo, /*cap_offset=*/0, /*options=*/0,
                                             &rights, &interrupt, register_fn));
  }

  fbl::RefPtr<VmObject> vmo;
  fbl::RefPtr<VmMapping> mapping;
  volatile MsiCapability* cap;
  if (create_valid_msi_vmo(&vmo, &mapping, &cap) == ZX_ERR_NOT_SUPPORTED) {
    printf("reserved region not available, skipping test");
    END_TEST;
  }
  // Now Create() should succeed.
  ASSERT_EQ(ZX_OK, MsiInterruptDispatcher::Create(alloc, 0, vmo, /*cap_offset=*/0, /*options=*/0,
                                                  &rights, &interrupt, register_fn));
  ASSERT_EQ(1u, register_call_count);
  END_TEST;
}

bool interrupt_creation_mask_test() {
  BEGIN_TEST;
  fbl::RefPtr<MsiAllocation> alloc;
  ASSERT_EQ(ZX_OK, create_allocation(&alloc, MsiAllocation::kMsiAllocationCountMax));
  fbl::RefPtr<VmObject> vmo;
  fbl::RefPtr<VmMapping> mapping;
  volatile MsiCapability* cap;
  if (create_valid_msi_vmo(&vmo, &mapping, &cap) == ZX_ERR_NOT_SUPPORTED) {
    printf("reserved region not available, skipping test");
    END_TEST;
  }

  zx_rights_t rights;
  // Below test all the mask variants around PVM. This also serves to ensure
  // that the dtor properly releases the msi registration in all cases.
  // Test 32 bit / no pvm
  {
    cap->control = ~(kMsiPvmSupported | kMsi64bitSupported);
    for (uint32_t msi_id = 0; msi_id < MsiAllocation::kMsiAllocationCountMax; msi_id++) {
      cap->mask_bits_32 = 0;
      cap->mask_bits_64 = 0;
      KernelHandle<InterruptDispatcher> interrupt;
      ASSERT_EQ(ZX_OK,
                MsiInterruptDispatcher::Create(alloc, msi_id, vmo, /*cap_offset=*/0, /*options=*/0,
                                               &rights, &interrupt, register_fn));
      auto val_32 = cap->mask_bits_32;
      auto val_64 = cap->mask_bits_64;
      EXPECT_EQ(val_32, 0u);
      EXPECT_EQ(val_64, 0u);
    }
  }

  // Test 32 bit / pvm
  {
    cap->control = ~(kMsi64bitSupported);
    for (uint32_t msi_id = 0; msi_id < MsiAllocation::kMsiAllocationCountMax; msi_id++) {
      cap->mask_bits_32 = 0;
      cap->mask_bits_64 = 0;
      KernelHandle<InterruptDispatcher> interrupt;
      ASSERT_EQ(ZX_OK,
                MsiInterruptDispatcher::Create(alloc, msi_id, vmo, /*cap_offset=*/0, /*options=*/0,
                                               &rights, &interrupt, register_fn));
      auto val_32 = cap->mask_bits_32;
      auto val_64 = cap->mask_bits_64;
      EXPECT_EQ(0u, val_32 & (1u << msi_id));
      EXPECT_EQ(val_64, 0u);
    }
  }

  // Test 64 bit / no pvm
  {
    cap->control = kMsi64bitSupported;
    for (uint32_t msi_id = 0; msi_id < MsiAllocation::kMsiAllocationCountMax; msi_id++) {
      cap->mask_bits_32 = 0;
      cap->mask_bits_64 = 0;
      KernelHandle<InterruptDispatcher> interrupt;
      ASSERT_EQ(ZX_OK,
                MsiInterruptDispatcher::Create(alloc, msi_id, vmo, /*cap_offset=*/0, /*options=*/0,
                                               &rights, &interrupt, register_fn));
      auto val_32 = cap->mask_bits_32;
      auto val_64 = cap->mask_bits_64;
      EXPECT_EQ(val_32, 0u);
      EXPECT_EQ(val_64, 0u);
    }
  }

  // Test 32 bit / pvm
  {
    cap->control = kMsiPvmSupported | kMsi64bitSupported;
    for (uint32_t msi_id = 0; msi_id < MsiAllocation::kMsiAllocationCountMax; msi_id++) {
      cap->mask_bits_32 = 0;
      cap->mask_bits_64 = 0;
      KernelHandle<InterruptDispatcher> interrupt;
      ASSERT_EQ(ZX_OK,
                MsiInterruptDispatcher::Create(alloc, msi_id, vmo, /*cap_offset=*/0, /*options=*/0,
                                               &rights, &interrupt, register_fn));
      auto val_32 = cap->mask_bits_32;
      auto val_64 = cap->mask_bits_64;
      EXPECT_EQ(val_32, 0u);
      EXPECT_EQ(0u, val_64 & (1u << msi_id));
    }
  }

  END_TEST;
}

bool out_of_order_ownership_test() {
  BEGIN_TEST;
  KernelHandle<InterruptDispatcher> interrupt1, interrupt2;
  fbl::RefPtr<VmObject> vmo;
  fbl::RefPtr<VmMapping> mapping;
  volatile MsiCapability* cap;

  if (create_valid_msi_vmo(&vmo, &mapping, &cap) == ZX_ERR_NOT_SUPPORTED) {
    printf("reserved region not available, skipping test");
    END_TEST;
  }
  {
    zx_rights_t rights;
    fbl::RefPtr<MsiAllocation> alloc;
    ASSERT_EQ(ZX_OK, create_allocation(&alloc, MsiAllocation::kMsiAllocationCountMax));
    ASSERT_EQ(ZX_OK,
              MsiInterruptDispatcher::Create(alloc, /*msi_id=*/0, vmo, /*cap_offset=*/0,
                                             /*options=*/0, &rights, &interrupt1, register_fn));
    ASSERT_EQ(ZX_OK,
              MsiInterruptDispatcher::Create(alloc, /*msi_id=*/1, vmo, /*cap_offset=*/0,
                                             /*options=*/0, &rights, &interrupt2, register_fn));
  }

  // This test verifies that although our allocation order was Alloc -> 1 -> 2,
  // freeing in the same order behaves properly due to the reference ownership.
  interrupt1.release();

  END_TEST;
}

}  // namespace

UNITTEST_START_TESTCASE(msi_object)
UNITTEST("Test that Create() and get_info() operate properly", allocation_creation_and_info_test)
UNITTEST("Test allocation limits", allocation_irq_count_test)
UNITTEST("Test reservations for MSI ids", allocation_reservation_test)
UNITTEST("Test for MSI platform support hooks", allocation_support_test)
UNITTEST("Test that MsiInterruptDispatchers cannot overlap on an MSI id",
         interrupt_duplication_test)
UNITTEST("Test that MsiInterruptDispatcher validates the VMO", interrupt_vmo_test)
UNITTEST("Test that MsiInterruptDispatcher unmasks on creation", interrupt_creation_mask_test)
UNITTEST("Test that MsiAllocation & MsiInterruptDispatcher ownership is correct",
         out_of_order_ownership_test)
UNITTEST_END_TESTCASE(msi_object, "msi", "Tests for MSI objects")
