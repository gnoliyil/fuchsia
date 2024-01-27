// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <align.h>
#include <assert.h>
#include <lib/fit/defer.h>
#include <stdio.h>
#include <string.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <new>

#include <fbl/arena.h>
#include <vm/vm.h>
#include <vm/vm_aspace.h>
#include <vm/vm_object_paged.h>

#define LOCAL_TRACE 0

namespace fbl {

Arena::~Arena() {
  free_.clear();
  if (vmar_ != nullptr) {
    // Unmap all of our memory.
    // The VMAR's parent holds a ref, so it won't be destroyed
    // automatically when we're destroyed.
    vmar_->Destroy();
    vmar_.reset();
  }
}

zx_status_t Arena::Init(const char* name, size_t ob_size, size_t count) {
  if ((ob_size == 0) || (ob_size > PAGE_SIZE))
    return ZX_ERR_INVALID_ARGS;
  if (!count)
    return ZX_ERR_INVALID_ARGS;
  LTRACEF("Arena '%s': ob_size %zu, count %zu\n", name, ob_size, count);

  // Carve out the memory:
  // - Kernel root VMAR
  //   + Sub VMAR
  //     + Control pool mapping
  //     + Unmapped guard page
  //     + Data pool mapping
  // Both mappings are backed by a single VMO.
  const size_t control_mem_sz = ROUNDUP(count * sizeof(Node), PAGE_SIZE);
  const size_t data_mem_sz = ROUNDUP(count * ob_size, PAGE_SIZE);
  const size_t guard_sz = PAGE_SIZE;
  const size_t vmo_sz = control_mem_sz + data_mem_sz;
  const size_t vmar_sz = vmo_sz + guard_sz;

  // Create the VMO.
  fbl::RefPtr<VmObjectPaged> control_vmo;
  fbl::RefPtr<VmObjectPaged> data_vmo;
  zx_status_t status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, 0u, control_mem_sz, &control_vmo);
  if (status != ZX_OK) {
    LTRACEF("Arena '%s': can't create %zu-byte VMO\n", name, vmo_sz);
    return status;
  }
  status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, 0u, data_mem_sz, &data_vmo);
  if (status != ZX_OK) {
    LTRACEF("Arena '%s': can't create %zu-byte VMO\n", name, vmo_sz);
    return status;
  }

  auto kspace = VmAspace::kernel_aspace();
  DEBUG_ASSERT(kspace != nullptr);
  auto root_vmar = kspace->RootVmar();
  DEBUG_ASSERT(root_vmar != nullptr);

  char vname[32];
  snprintf(vname, sizeof(vname), "arena:%s/control", name);
  control_vmo->set_name(vname, sizeof(vname));
  snprintf(vname, sizeof(vname), "arena:%s/data", name);
  data_vmo->set_name(vname, sizeof(vname));

  // Create the VMAR.
  fbl::RefPtr<VmAddressRegion> vmar;
  zx_status_t st = root_vmar->CreateSubVmar(
      0,  // offset (ignored)
      vmar_sz,
      false,  // align_pow2
      VMAR_FLAG_CAN_MAP_READ | VMAR_FLAG_CAN_MAP_WRITE | VMAR_FLAG_CAN_MAP_SPECIFIC, vname, &vmar);
  if (st != ZX_OK || vmar == nullptr) {
    LTRACEF("Arena '%s': can't create %zu-byte VMAR (%d)\n", name, vmar_sz, st);
    return ZX_ERR_NO_MEMORY;
  }
  // The VMAR's parent holds a ref, so it won't be destroyed
  // automatically when we return.
  auto destroy_vmar = fit::defer([&vmar]() { vmar->Destroy(); });

  // Create a mapping for the control pool.
  fbl::RefPtr<VmMapping> control_mapping;
  st = vmar->CreateVmMapping(
      0,  // mapping_offset
      control_mem_sz,
      false,  // align_pow2
      VMAR_FLAG_SPECIFIC | VMAR_FLAG_DEBUG_DYNAMIC_KERNEL_MAPPING, control_vmo,
      0,  // vmo_offset
      ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE, "control", &control_mapping);
  if (st != ZX_OK || control_mapping == nullptr) {
    LTRACEF("Arena '%s': can't create %zu-byte control mapping (%d)\n", name, control_mem_sz, st);
    return ZX_ERR_NO_MEMORY;
  }

  // Create a mapping for the data pool, leaving an unmapped gap
  // between it and the control pool.
  fbl::RefPtr<VmMapping> data_mapping;
  st = vmar->CreateVmMapping(control_mem_sz + guard_sz,  // mapping_offset
                             data_mem_sz,
                             false,  // align_pow2
                             VMAR_FLAG_SPECIFIC | VMAR_FLAG_DEBUG_DYNAMIC_KERNEL_MAPPING, data_vmo,
                             0,  // vmo_offset
                             ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE, "data",
                             &data_mapping);
  if (st != ZX_OK || data_mapping == nullptr) {
    LTRACEF("Arena '%s': can't create %zu-byte data mapping (%d)\n", name, data_mem_sz, st);
    return ZX_ERR_NO_MEMORY;
  }

  // TODO(dbort): Add a VmMapping flag that says "do not demand page",
  // requiring and ensuring that we commit our pages manually.

  control_.Init("control", control_vmo, control_mapping, sizeof(Node));
  data_.Init("data", data_vmo, data_mapping, ob_size);

  count_ = 0u;

  vmar_ = ktl::move(vmar);
  destroy_vmar.cancel();

  if (LOCAL_TRACE) {
    Dump();
  }
  return ZX_OK;
}

Arena::Pool::~Pool() {
  if (vmo_) {
    const size_t committed_len = committed_ - start_;
    if (committed_len > 0) {
      vmo_->Unpin(0, committed_len);
    }
  }
}

void Arena::Pool::Init(const char* name, fbl::RefPtr<VmObject> vmo, fbl::RefPtr<VmMapping> mapping,
                       size_t slot_size) {
  DEBUG_ASSERT(mapping != nullptr);
  DEBUG_ASSERT(slot_size > 0);

  strlcpy(const_cast<char*>(name_), name, sizeof(name_));
  slot_size_ = slot_size;
  mapping_ = ktl::move(mapping);
  vmo_ = ktl::move(vmo);
  {
    Guard<CriticalMutex> guard{mapping_->lock()};
    committed_max_ = committed_ = top_ = start_ = reinterpret_cast<char*>(mapping_->base_locked());
    end_ = start_ + mapping_->size_locked();
    mapping_base = mapping_->base_locked();
    mapping_size = mapping_->size_locked();
  }

  DEBUG_ASSERT(IS_PAGE_ALIGNED(start_));
  DEBUG_ASSERT(IS_PAGE_ALIGNED(end_));
}

// Pick values that avoid lots of commits + decommits when
// right on the edge.
static const size_t kPoolCommitIncrease = 4 * PAGE_SIZE;
static const size_t kPoolDecommitThreshold = 8 * PAGE_SIZE;
static_assert(kPoolCommitIncrease < kPoolDecommitThreshold, "");

void* Arena::Pool::Pop() {
  if (static_cast<size_t>(end_ - top_) < slot_size_) {
    LTRACEF("%s: no room\n", name_);
    return nullptr;
  }
  if (top_ + slot_size_ > committed_) {
    // We've hit the end of our committed pages; commit some more.
    char* nc = committed_ + kPoolCommitIncrease;
    if (nc > end_) {
      nc = end_;
    }
    LTRACEF("%s: commit 0x%p..0x%p\n", name_, committed_, nc);
    const size_t offset = reinterpret_cast<vaddr_t>(committed_) - mapping_base;
    const size_t len = nc - committed_;
    zx_status_t st = vmo_->CommitRangePinned(offset, len, true);
    if (st != ZX_OK) {
      return nullptr;
    }
    st = mapping_->MapRange(offset, len, /* commit */ true);
    if (st != ZX_OK) {
      // Unpin the range.
      vmo_->Unpin(offset, len);
      LTRACEF("%s: can't map range 0x%p..0x%p: %d\n", name_, committed_, nc, st);
      // Try to clean up any committed pages, but don't require
      // that it succeeds.
      mapping_->DecommitRange(offset, len);
      return nullptr;
    }
    committed_ = nc;
    if (committed_ > committed_max_) {
      committed_max_ = committed_;
    }
  }
  char* slot = top_;
  top_ += slot_size_;
  return slot;
}

void Arena::Pool::Push(void* p) {
  // Can only push the most-recently-popped slot.
  ASSERT(reinterpret_cast<char*>(p) + slot_size_ == top_);
  top_ -= slot_size_;
  if (static_cast<size_t>(committed_ - top_) >= kPoolDecommitThreshold) {
    char* nc = reinterpret_cast<char*>(
        ROUNDUP(reinterpret_cast<uintptr_t>(top_ + kPoolCommitIncrease), PAGE_SIZE));
    if (nc > end_) {
      nc = end_;
    }
    if (nc >= committed_) {
      return;
    }
    LTRACEF("%s: decommit 0x%p..0x%p\n", name_, nc, committed_);
    const size_t offset = reinterpret_cast<vaddr_t>(nc) - mapping_base;
    const size_t len = committed_ - nc;
    vmo_->Unpin(offset, len);
    // If this fails or decommits less than we asked for, oh well.
    mapping_->DecommitRange(offset, len);
    committed_ = nc;
  }
}

void Arena::Pool::Dump() const {
  printf("  pool '%s' slot size %zu, %zu pages committed:\n", name_, slot_size_,
         mapping_->AllocatedPages().uncompressed);
  printf("  |     start 0x%p\n", start_);
  size_t nslots = static_cast<size_t>(top_ - start_) / slot_size_;
  printf("  |       top 0x%p (%zu slots popped)\n", top_, nslots);
  const size_t np = static_cast<size_t>(committed_ - start_) / PAGE_SIZE;
  const size_t npmax = static_cast<size_t>(committed_max_ - start_) / PAGE_SIZE;
  printf("  | committed 0x%p (%zu pages; %zu pages max)\n", committed_, np, npmax);
  nslots = static_cast<size_t>(end_ - start_) / slot_size_;
  printf("  \\       end 0x%p (%zu slots total)\n", end_, nslots);
}

void* Arena::Alloc() {
  DEBUG_ASSERT(vmar_ != nullptr);
  // Prefers to return the most-recently-freed slot in the hopes that
  // it is still hot in the cache.
  void* allocation;
  if (!free_.is_empty()) {
    // TODO(dbort): Replace this linked list with a stack of offsets into
    // the data pool; simpler, and uses less memory.
    Node* node = free_.pop_front();
    auto slot = node->slot;
    control_.Push(node);
    allocation = slot;
  } else {
    allocation = data_.Pop();
  }
  if (allocation != nullptr) {
    ++count_;
  }
  return allocation;
}

void Arena::Free(void* addr) {
  DEBUG_ASSERT(vmar_ != nullptr);
  if (addr == nullptr) {
    return;
  }
  --count_;
  DEBUG_ASSERT(data_.InRange(addr));
  Node* node = new (reinterpret_cast<void*>(control_.Pop())) Node{addr};
  free_.push_front(node);
}

void Arena::Dump() const {
  DEBUG_ASSERT(vmar_ != nullptr);
  printf("%s mappings:\n", vmar_->name());
  {
    Guard<CriticalMutex> guard{vmar_->lock()};
    vmar_->DumpLocked(/* depth */ 1, /* verbose */ true);
  }
  printf("%s pools:\n", vmar_->name());
  control_.Dump();
  data_.Dump();
  printf("%s free list: %zu nodes\n", vmar_->name(), free_.size_slow());
}

}  // namespace fbl
