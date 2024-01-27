// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_VM_ADDRESS_REGION_DISPATCHER_H_
#define ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_VM_ADDRESS_REGION_DISPATCHER_H_

#include <sys/types.h>
#include <zircon/rights.h>
#include <zircon/types.h>

#include <object/dispatcher.h>
#include <object/handle.h>

class VmAddressRegion;
class VmMapping;
class VmObject;

class VmAddressRegionDispatcher final
    : public SoloDispatcher<VmAddressRegionDispatcher, ZX_DEFAULT_VMAR_RIGHTS> {
 public:
  static zx_status_t Create(fbl::RefPtr<VmAddressRegion> vmar, uint base_arch_mmu_flags,
                            KernelHandle<VmAddressRegionDispatcher>* handle, zx_rights_t* rights);

  ~VmAddressRegionDispatcher() final;
  zx_obj_type_t get_type() const final { return ZX_OBJ_TYPE_VMAR; }

  // TODO(teisenbe): Make this the planned batch interface
  zx_status_t Allocate(size_t offset, size_t size, uint32_t flags,
                       KernelHandle<VmAddressRegionDispatcher>* handle, zx_rights_t* rights);

  zx_status_t Destroy();

  zx_status_t Map(size_t vmar_offset, fbl::RefPtr<VmObject> vmo, uint64_t vmo_offset, size_t len,
                  uint32_t flags, fbl::RefPtr<VmMapping>* out);

  zx_status_t Protect(vaddr_t base, size_t len, uint32_t flags,
                      VmAddressRegionOpChildren op_children);

  zx_status_t RangeOp(uint32_t op, uint64_t offset, uint64_t size, zx_rights_t rights,
                      user_inout_ptr<void> buffer, size_t buffer_size);

  zx_status_t Unmap(vaddr_t base, size_t len, VmAddressRegionOpChildren op_children);

  zx_status_t SetMemoryPriority(VmAddressRegion::MemoryPriority priority);

  const fbl::RefPtr<VmAddressRegion>& vmar() const { return vmar_; }

  // Check if the given flags define an allowed combination of RWX
  // protections.
  static bool is_valid_mapping_protection(uint32_t flags);

  static VmAddressRegionOpChildren op_children_from_rights(zx_rights_t rights) {
    return (rights & ZX_RIGHT_OP_CHILDREN) == 0 ? VmAddressRegionOpChildren::No
                                                : VmAddressRegionOpChildren::Yes;
  }

 private:
  explicit VmAddressRegionDispatcher(fbl::RefPtr<VmAddressRegion> vmar, uint base_arch_mmu_flags);

  fbl::RefPtr<VmAddressRegion> vmar_;
  const uint base_arch_mmu_flags_;
};

#endif  // ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_VM_ADDRESS_REGION_DISPATCHER_H_
