// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#ifndef ZIRCON_KERNEL_VM_PMM_NODE_H_
#define ZIRCON_KERNEL_VM_PMM_NODE_H_

#include <fbl/canary.h>
#include <fbl/intrusive_double_list.h>
#include <kernel/event.h>
#include <kernel/lockdep.h>
#include <kernel/mutex.h>
#include <vm/compression.h>
#include <vm/loan_sweeper.h>
#include <vm/physical_page_borrowing_config.h>
#include <vm/pmm.h>
#include <vm/pmm_checker.h>

#include "pmm_arena.h"

// per numa node collection of pmm arenas and worker threads
class PmmNode {
 public:
  PmmNode();
  ~PmmNode();

  DISALLOW_COPY_ASSIGN_AND_MOVE(PmmNode);

  paddr_t PageToPaddr(const vm_page_t* page) TA_NO_THREAD_SAFETY_ANALYSIS;
  vm_page_t* PaddrToPage(paddr_t addr) TA_NO_THREAD_SAFETY_ANALYSIS;

  // main allocator routines
  zx_status_t AllocPage(uint alloc_flags, vm_page_t** page, paddr_t* pa);
  zx_status_t AllocPages(size_t count, uint alloc_flags, list_node* list);
  zx_status_t AllocRange(paddr_t address, size_t count, list_node* list);
  zx_status_t AllocContiguous(size_t count, uint alloc_flags, uint8_t alignment_log2, paddr_t* pa,
                              list_node* list);
  void FreePage(vm_page* page);
  // The list can be a combination of loaned and non-loaned pages.
  void FreeList(list_node* list);

  // Contiguous page loaning routines
  void BeginLoan(list_node* page_list);
  void CancelLoan(paddr_t address, size_t count);
  void EndLoan(paddr_t address, size_t count, list_node* page_list);
  void DeleteLender(paddr_t address, size_t count);

  // For purposes of signalling memory pressure level, only free_list_ pages are counted, not
  // free_loaned_list_ pages.
  zx_status_t InitReclamation(const uint64_t* watermarks, uint8_t watermark_count,
                              uint64_t debounce, void* context,
                              mem_avail_state_updated_callback_t callback);
  zx_status_t WaitTillShouldRetrySingleAlloc(const Deadline& deadline) {
    return free_pages_evt_.Wait(deadline);
  }

  void StopReturningShouldWait();

  uint64_t CountFreePages() const;
  uint64_t CountLoanedFreePages() const;
  uint64_t CountLoanCancelledPages() const;
  // Not actually used and cancelled is still not free, since the page can't be allocated in that
  // state.
  uint64_t CountLoanedNotFreePages() const;
  uint64_t CountLoanedPages() const;
  uint64_t CountTotalBytes() const;

  // printf free and overall state of the internal arenas
  // NOTE: both functions skip mutexes and can be called inside timer or crash context
  // though the data they return may be questionable
  void DumpFree() const TA_NO_THREAD_SAFETY_ANALYSIS;
  void Dump(bool is_panic) const TA_NO_THREAD_SAFETY_ANALYSIS;

  void DumpMemAvailState() const;
  void DebugMemAvailStateCallback(uint8_t mem_state_idx) const;
  uint64_t DebugNumPagesTillMemState(uint8_t mem_state_idx) const;
  uint8_t DebugMaxMemAvailState() const;

  zx_status_t AddArena(const pmm_arena_info_t* info);

  // Returns the number of arenas.
  size_t NumArenas() const;

  // Copies |count| pmm_arena_info_t objects into |buffer| starting with the |i|-th arena ordered by
  // base address.  For example, passing an |i| of 1 would skip the 1st arena.
  //
  // The objects will be sorted in ascending order by arena base address.
  //
  // Returns ZX_ERR_OUT_OF_RANGE if |count| is 0 or |i| and |count| specificy an invalid range.
  //
  // Returns ZX_ERR_BUFFER_TOO_SMALL if the buffer is too small.
  zx_status_t GetArenaInfo(size_t count, uint64_t i, pmm_arena_info_t* buffer, size_t buffer_size);

  // add new pages to the free queue. used when boostrapping a PmmArena
  void AddFreePages(list_node* list);

  PageQueues* GetPageQueues() { return &page_queues_; }

  // See |pmm_get_page_compression|
  VmCompression* GetPageCompression() {
    Guard<Mutex> guard{&compression_lock_};
    return page_compression_.get();
  }

  // See |pmm_set_page_compression|.
  zx_status_t SetPageCompression(fbl::RefPtr<VmCompression> compression);

  // Fill all free pages (both non-loaned and loaned) with a pattern and arm the checker.  See
  // |PmmChecker|.
  //
  // This is a no-op if the checker is not enabled.  See |EnableFreePageFilling|
  void FillFreePagesAndArm();

  // Synchronously walk the PMM's free list (and free loaned list) and validate each page.  This is
  // an incredibly expensive operation and should only be used for debugging purposes.
  void CheckAllFreePages();

#if __has_feature(address_sanitizer)
  // Synchronously walk the PMM's free list (and free loaned list) and poison each page.
  void PoisonAllFreePages();
#endif

  // Enable the free fill checker with the specified fill size and action, and begin filling freed
  // pages (including freed loaned pages) going forward.  See |PmmChecker| for definition of fill
  // size.
  //
  // Note, pages freed piror to calling this method will remain unfilled.  To fill them, call
  // |FillFreePagesAndArm|.
  void EnableFreePageFilling(size_t fill_size, PmmChecker::Action action);

  // Disarm and disable the free fill checker.
  void DisableChecker();

  // Return a pointer to this object's free fill checker.
  //
  // For test and diagnostic purposes.
  PmmChecker* Checker() { return &checker_; }

  static int64_t get_alloc_failed_count();

  // See |pmm_has_alloc_failed_no_mem|.
  static bool has_alloc_failed_no_mem();

  Evictor* GetEvictor() { return &evictor_; }

  // If randomly waiting on allocations is enabled, this re-seeds from the global prng, otherwise it
  // does nothing.
  void SeedRandomShouldWait();

 private:
  void FreePageHelperLocked(vm_page* page) TA_REQ(lock_);
  void FreeListLocked(list_node* list) TA_REQ(lock_);

  void UpdateMemAvailStateLocked() TA_REQ(lock_);
  void SetMemAvailStateLocked(uint8_t mem_avail_state) TA_REQ(lock_);

  void IncrementFreeCountLocked(uint64_t amount) TA_REQ(lock_) {
    free_count_.fetch_add(amount, ktl::memory_order_relaxed);

    if (unlikely(free_count_.load(ktl::memory_order_relaxed) >= mem_avail_state_upper_bound_)) {
      UpdateMemAvailStateLocked();
    }
  }
  void DecrementFreeCountLocked(uint64_t amount) TA_REQ(lock_) {
    DEBUG_ASSERT(free_count_.load(ktl::memory_order_relaxed) >= amount);
    free_count_.fetch_sub(amount, ktl::memory_order_relaxed);

    if (unlikely(free_count_.load(ktl::memory_order_relaxed) <= mem_avail_state_lower_bound_)) {
      UpdateMemAvailStateLocked();
    }
  }

  void IncrementFreeLoanedCountLocked(uint64_t amount) TA_REQ(lock_) {
    free_loaned_count_.fetch_add(amount, ktl::memory_order_relaxed);
  }
  void DecrementFreeLoanedCountLocked(uint64_t amount) TA_REQ(lock_) {
    DEBUG_ASSERT(free_loaned_count_.load(ktl::memory_order_relaxed) >= amount);
    free_loaned_count_.fetch_sub(amount, ktl::memory_order_relaxed);
  }

  void IncrementLoanedCountLocked(uint64_t amount) TA_REQ(lock_) {
    loaned_count_.fetch_add(amount, ktl::memory_order_relaxed);
  }
  void DecrementLoanedCountLocked(uint64_t amount) TA_REQ(lock_) {
    DEBUG_ASSERT(loaned_count_.load(ktl::memory_order_relaxed) >= amount);
    loaned_count_.fetch_sub(amount, ktl::memory_order_relaxed);
  }

  void IncrementLoanCancelledCountLocked(uint64_t amount) TA_REQ(lock_) {
    loan_cancelled_count_.fetch_add(amount, ktl::memory_order_relaxed);
  }
  void DecrementLoanCancelledCountLocked(uint64_t amount) TA_REQ(lock_) {
    DEBUG_ASSERT(loan_cancelled_count_.load(ktl::memory_order_relaxed) >= amount);
    loan_cancelled_count_.fetch_sub(amount, ktl::memory_order_relaxed);
  }

  bool InOomStateLocked() TA_REQ(lock_);

  void AllocPageHelperLocked(vm_page_t* page) TA_REQ(lock_);

  template <typename F>
  void ForPagesInPhysRangeLocked(paddr_t start, size_t count, F func) TA_REQ(lock_);

  // This method should be called when the PMM fails to allocate in a user-visible way and will
  // (optionally) trigger an asynchronous OOM response.
  void ReportAllocFailure() TA_REQ(lock_);

  fbl::Canary<fbl::magic("PNOD")> canary_;

  mutable DECLARE_MUTEX(PmmNode) lock_;

  uint64_t arena_cumulative_size_ TA_GUARDED(lock_) = 0;
  // This is both an atomic and guarded by lock_ as we would like modifications to require the lock,
  // as logic in the system relies on the free_count_ not changing whilst the lock is held, but also
  // be an atomic so it can be correctly read without the lock.
  ktl::atomic<uint64_t> free_count_ TA_GUARDED(lock_) = 0;
  ktl::atomic<uint64_t> free_loaned_count_ TA_GUARDED(lock_) = 0;
  ktl::atomic<uint64_t> loaned_count_ TA_GUARDED(lock_) = 0;
  ktl::atomic<uint64_t> loan_cancelled_count_ TA_GUARDED(lock_) = 0;

  fbl::SizedDoublyLinkedList<PmmArena*> arena_list_ TA_GUARDED(lock_);

  // Free pages where !loaned.
  list_node free_list_ TA_GUARDED(lock_) = LIST_INITIAL_VALUE(free_list_);
  // Free pages where loaned && !loan_cancelled.
  list_node free_loaned_list_ TA_GUARDED(lock_) = LIST_INITIAL_VALUE(free_loaned_list_);

  // Tracks whether we should honor PMM_ALLOC_FLAG_CAN_WAIT requests or not. When true we will
  // always attempt to perform the allocation, or fail with ZX_ERR_NO_MEMORY.
  bool never_return_should_wait_ TA_GUARDED(lock_) = false;

  // Event is signaled either when there are pages available that can be allocated without entering
  // the OOM state, or if |never_return_should_wait_| is true.
  Event free_pages_evt_;

  uint64_t mem_avail_state_watermarks_[MAX_WATERMARK_COUNT] TA_GUARDED(lock_);
  uint8_t mem_avail_state_watermark_count_ TA_GUARDED(lock_);
  uint8_t mem_avail_state_cur_index_ TA_GUARDED(lock_);
  uint64_t mem_avail_state_debounce_ TA_GUARDED(lock_);
  uint64_t mem_avail_state_upper_bound_ TA_GUARDED(lock_);
  uint64_t mem_avail_state_lower_bound_ TA_GUARDED(lock_);
  void* mem_avail_state_context_ TA_GUARDED(lock_);
  mem_avail_state_updated_callback_t mem_avail_state_callback_ TA_GUARDED(lock_);

  PageQueues page_queues_;

  Evictor evictor_;

  // The page_compression_ is a lazily initialized RefPtr to keep the PmmNode constructor simple, at
  // the cost needing to hold a lock to read the RefPtr. To avoid unnecessarily contending on the
  // main pmm lock_, use a separate one.
  DECLARE_MUTEX(PmmNode) compression_lock_;
  fbl::RefPtr<VmCompression> page_compression_ TA_GUARDED(compression_lock_);

  bool free_fill_enabled_ TA_GUARDED(lock_) = false;
  PmmChecker checker_ TA_GUARDED(lock_);

  // The rng state for random waiting on allocations. This allows us to use rand_r, which requires
  // no further thread synchronization, unlike rand().
  uintptr_t random_should_wait_seed_ TA_GUARDED(lock_) = 0;
};

// We don't need to hold the arena lock while executing this, since it is
// only accesses values that are set once during system initialization.
inline vm_page_t* PmmNode::PaddrToPage(paddr_t addr) TA_NO_THREAD_SAFETY_ANALYSIS {
  for (auto& a : arena_list_) {
    if (a.address_in_arena(addr)) {
      size_t index = (addr - a.base()) / PAGE_SIZE;
      return a.get_page(index);
    }
  }
  return nullptr;
}

#endif  // ZIRCON_KERNEL_VM_PMM_NODE_H_
