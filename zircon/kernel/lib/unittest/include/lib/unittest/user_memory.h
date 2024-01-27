// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_UNITTEST_INCLUDE_LIB_UNITTEST_USER_MEMORY_H_
#define ZIRCON_KERNEL_LIB_UNITTEST_INCLUDE_LIB_UNITTEST_USER_MEMORY_H_

#include <lib/user_copy/user_ptr.h>

#include <ktl/move.h>
#include <ktl/unique_ptr.h>
#include <vm/pmm.h>
#include <vm/scanner.h>
#include <vm/vm.h>
#include <vm/vm_address_region.h>
#include <vm/vm_aspace.h>
#include <vm/vm_object_paged.h>

namespace testing {

// UserMemory facilitates testing code that requires user memory.
//
// Example:
//    unique_ptr<UserMemory> mem = UserMemory::Create(sizeof(thing));
//    auto mem_out = make_user_out_ptr(mem->out());
//    mem_out.copy_array_to_user(&thing, sizeof(thing));
//
class UserMemory {
 public:
  static ktl::unique_ptr<UserMemory> Create(size_t size);
  static ktl::unique_ptr<UserMemory> Create(fbl::RefPtr<VmObject> vmo, uint8_t tag = 0);
  static ktl::unique_ptr<UserMemory> CreateInAspace(fbl::RefPtr<VmObject> vmo,
                                                    fbl::RefPtr<VmAspace>& aspace, uint8_t tag = 0);
  virtual ~UserMemory();

  vaddr_t base() const {
    Guard<CriticalMutex> guard{mapping_->lock()};
    vaddr_t base = mapping_->base_locked();
#if defined(__aarch64__)
    base |= static_cast<vaddr_t>(tag_) << kTbiBit;
#endif
    return base;
  }

  uint8_t tag() const { return tag_; }

  const fbl::RefPtr<VmObject>& vmo() const { return vmo_; }

  const fbl::RefPtr<VmAspace>& aspace() const { return mapping_->aspace(); }

  template <typename T>
  void put(const T& value, size_t i = 0) {
    zx_status_t status = user_out<T>().element_offset(i).copy_to_user(value);
    ASSERT(status == ZX_OK);
  }

  template <typename T>
  T get(size_t i = 0) {
    T value;
    zx_status_t status = user_in<T>().element_offset(i).copy_from_user(&value);
    ASSERT(status == ZX_OK);
    return value;
  }

  template <typename T>
  user_out_ptr<T> user_out() {
    return make_user_out_ptr(reinterpret_cast<T*>(base()));
  }

  template <typename T>
  user_in_ptr<const T> user_in() {
    return make_user_in_ptr(reinterpret_cast<const T*>(base()));
  }

  // Ensures the mapping is committed and mapped such that usages will cause no faults.
  zx_status_t CommitAndMap(size_t size, uint64_t offset = 0) {
    return mapping_->MapRange(offset, size, true);
  }

  // Changes the mapping permissions to be a Read-Only Executable mapping.
  zx_status_t MakeRX() {
    return mapping_->Protect(
        mapping_->base_locking(), mapping_->size_locking(),
        ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_EXECUTE);
  }

  // Read or write to the underlying VMO directly, bypassing the mapping.
  zx_status_t VmoRead(void* ptr, uint64_t offset, uint64_t len) {
    ASSERT(vmo_);
    return vmo_->Read(ptr, offset, len);
  }
  zx_status_t VmoWrite(const void* ptr, uint64_t offset, uint64_t len) {
    ASSERT(vmo_);
    return vmo_->Write(ptr, offset, len);
  }

 private:
  UserMemory(fbl::RefPtr<VmMapping> mapping, fbl::RefPtr<VmObject> vmo, uint8_t tag)
      : mapping_(ktl::move(mapping)), vmo_(ktl::move(vmo)), tag_(tag) {}

  fbl::RefPtr<VmMapping> mapping_;
  fbl::RefPtr<VmObject> vmo_;

  // This is really only used on aarch64.
  uint8_t tag_;

  // User memory here is going to be touched directly by the kernel and will not have the option to
  // fault in memory that should get reclaimed by the scanner. Therefore as long as we are using any
  // UserMemory we should disable the scanner.
  AutoVmScannerDisable scanner_disable_;
};

}  // namespace testing

#endif  // ZIRCON_KERNEL_LIB_UNITTEST_INCLUDE_LIB_UNITTEST_USER_MEMORY_H_
