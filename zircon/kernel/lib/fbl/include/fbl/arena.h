// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_FBL_INCLUDE_FBL_ARENA_H_
#define ZIRCON_KERNEL_LIB_FBL_INCLUDE_FBL_ARENA_H_

#include <stddef.h>

#include <new>

#include <fbl/intrusive_single_list.h>
#include <fbl/ref_ptr.h>
#include <ktl/forward.h>
#include <vm/vm_address_region.h>

namespace fbl {

// Arena is a fast memory allocator for objects of a single size.
// Both Alloc() and Free() are always O(1) and memory always comes
// from a single contiguous chunk of page-aligned memory.
//
// The control structures and data are not interleaved so it is
// more resilient to memory bugs than traditional pool allocators.
//
// The overhead per object is two pointers (16 bytes in 64-bits)

class __OWNER(void) Arena {
 public:
  Arena() = default;
  ~Arena();

  zx_status_t Init(const char* name, size_t ob_size, size_t max_count);
  void* Alloc();
  void Free(void* addr);
  bool in_range(uintptr_t addr) const { return data_.InRange(addr); }
  bool in_range(void* addr) const { return in_range(reinterpret_cast<uintptr_t>(addr)); }

  void* start() const { return data_.start(); }
  void* end() const { return data_.end(); }

  // Dumps information about the Arena using printf().
  // TIP: Use "k zx htinfo" to dump the handle table at runtime.
  void Dump() const;

  // Returns the number of outstanding allocations from this arena.
  size_t DiagnosticCount() const { return count_; }

 private:
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  struct Node : public SinglyLinkedListable<Node*> {
    explicit Node(void* s) : slot(s) {}
    void* slot;
  };

  SinglyLinkedList<Node*> free_;

  // A memory pool that manually commits/decommits memory as
  // necessary, avoiding demand-paging.
  class Pool {
   public:
    ~Pool();
    // Initializes the pool. |mapping| must be fully backed by
    // vmo, but should not have any committed pages. |slot_size|
    // is the size of object returned/accepted by Pop/Push.
    void Init(const char* name, fbl::RefPtr<VmObject> vmo, fbl::RefPtr<VmMapping> mapping,
              size_t slot_size);

    // Returns a pointer to a |slot_size| piece of memory, or nullptr
    // if there is no memory available.
    void* Pop();

    // Makes |p| available to be returned by future calls to Pop.
    // |p| must be the pointer most recently returned by Pop, or
    // the method will ASSERT.
    void Push(void* p);

    // Returns true if |addr| could have been returned by Pop and has
    // not been reclaimed by Push.
    bool InRange(uintptr_t addr) const {
      return (addr >= reinterpret_cast<uintptr_t>(start_) &&
              addr < reinterpret_cast<uintptr_t>(top_));
    }
    bool InRange(const void* addr) const { return InRange(reinterpret_cast<uintptr_t>(addr)); }

    // The lowest address of the memory managed by this Pool.
    // Pop will only return values > |start| (besides nullptr).
    char* start() const { return start_; }

    // The highest address of the memory managed by this Pool.
    // Pop will only return values <= |end|-|slot_size| (besides nullptr).
    char* end() const { return end_; }

    // Dumps information about the Pool using printf().
    void Dump() const;

   private:
    const char name_[32] = {};

    fbl::RefPtr<VmMapping> mapping_;
    fbl::RefPtr<VmObject> vmo_;
    size_t slot_size_;
    char* start_;
    char* top_;            // |start|..|top| contains all allocated slots.
    char* committed_;      // |start|..|mapped| is committed.
    char* committed_max_;  // Largest committed_ value seen.
    char* end_;            // |mapped|..|end| is not committed.
    vaddr_t mapping_base;
    size_t mapping_size;
  };

  Pool control_;  // Free list nodes
  Pool data_;     // Objects

  // Parent VMAR of our memory.
  fbl::RefPtr<VmAddressRegion> vmar_;

  // Outstanding allocation count.
  size_t count_;

  friend class ArenaTestFriend;
};

// TypedArena Convenience wrapper that handles:
// 1- C++ type enforcement
// 2- Calls constructors and destructors
// 3- Serializes access according to the Mtx type or
//    use fbl::NullMutex to use external serialization.
//
template <typename T, typename Mtx>
class __OWNER(T) TypedArena {
 public:
  zx_status_t Init(const char* name, size_t max_count) TA_NO_THREAD_SAFETY_ANALYSIS {
    return arena_.Init(name, sizeof(T), max_count);
  }

  template <typename... Args>
  T* New(Args&&... args) {
    mutex_.Acquire();
    void* addr = arena_.Alloc();
    mutex_.Release();
    return addr ? new (addr) T(ktl::forward<Args>(args)...) : nullptr;
  }

  void Delete(T* obj) {
    obj->~T();
    RawFree(obj);
  }

  void RawFree(void* mem) {
    mutex_.Acquire();
    arena_.Free(mem);
    mutex_.Release();
  }

  // Returns the number of outstanding allocations from this arena.
  size_t DiagnosticCount() const {
    mutex_.Acquire();
    auto count = arena_.DiagnosticCount();
    mutex_.Release();
    return count;
  }

 private:
  mutable Mtx mutex_;
  Arena arena_ TA_GUARDED(mutex_);
};
}  // namespace fbl

#endif  // ZIRCON_KERNEL_LIB_FBL_INCLUDE_FBL_ARENA_H_
