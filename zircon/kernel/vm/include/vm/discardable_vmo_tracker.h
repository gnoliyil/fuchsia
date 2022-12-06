// Copyright 2022 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_VM_INCLUDE_VM_DISCARDABLE_VMO_TRACKER_H_
#define ZIRCON_KERNEL_VM_INCLUDE_VM_DISCARDABLE_VMO_TRACKER_H_

#include <vm/vm_cow_pages.h>

// Tracks state relevant for discardable VMOs. This class offers separation of the logic required
// for discardable VMO management; the members are still protected by the owning VmCowPages' lock.
class DiscardableVmoTracker final {
 public:
  DiscardableVmoTracker() = default;
  ~DiscardableVmoTracker() = default;

  void InitCowPages(VmCowPages* cow) {
    // Should be initializing the back reference exactly once.
    ASSERT(!cow_);
    ASSERT(cow);
    cow_ = cow;
  }

  // See comment near discardable_state_ for details.
  enum class DiscardableState : uint8_t {
    kUnset = 0,
    kReclaimable,
    kUnreclaimable,
    kDiscarded,
  };

  // Updates the |discardable_state_| of a discardable vmo, and moves it from one discardable list
  // to another.
  void UpdateDiscardableStateLocked(DiscardableState state) TA_REQ(cow_->lock())
      TA_EXCL(DiscardableVmosLock::Get());

  // Remove a discardable object from whichever global discardable list it is in. Called from the
  // VmCowPages destructor. Also resets the cow_ back reference.
  void RemoveFromDiscardableListLocked() TA_REQ(cow_->lock()) TA_EXCL(DiscardableVmosLock::Get());

  // Lock and unlock functions.
  zx_status_t LockDiscardableLocked(bool try_lock, bool* was_discarded_out) TA_REQ(cow_->lock())
      TA_EXCL(DiscardableVmosLock::Get());
  zx_status_t UnlockDiscardableLocked() TA_REQ(cow_->lock()) TA_EXCL(DiscardableVmosLock::Get());

  // Returns the total number of pages locked and unlocked across all discardable vmos.
  // Note that this might not be exact and we might miss some vmos, because the
  // |DiscardableVmosLock| is dropped after processing each vmo on the global discardable lists.
  // That is fine since these numbers are only used for accounting.
  using DiscardablePageCounts = VmCowPages::DiscardablePageCounts;
  static DiscardablePageCounts DebugDiscardablePageCounts() TA_EXCL(DiscardableVmosLock::Get());

  // Walks through the LRU reclaimable list of discardable vmos and discards pages from each, until
  // |target_pages| have been discarded, or the list of candidates is exhausted. Only vmos that have
  // become reclaimable more than |min_duration_since_reclaimable| in the past will be discarded;
  // this prevents discarding reclaimable vmos that were recently accessed. The discarded pages are
  // appended to the |freed_list| passed in; the caller takes ownership of the discarded pages and
  // is responsible for freeing them. Returns the total number of pages discarded.
  static uint64_t ReclaimPagesFromDiscardableVmos(uint64_t target_pages,
                                                  zx_duration_t min_duration_since_reclaimable,
                                                  list_node_t* freed_list)
      TA_EXCL(DiscardableVmosLock::Get());

  // Accessors for private members.
  DiscardableState discardable_state_locked() const TA_REQ(cow_->lock()) {
    return discardable_state_;
  }
  zx_time_t last_unlock_timestamp_locked() const TA_REQ(cow_->lock()) {
    return last_unlock_timestamp_;
  }
  uint64_t lock_count_locked() const TA_REQ(cow_->lock()) { return lock_count_; }

  // Debug functions exposed for testing.
  uint64_t DebugGetLockCount() const {
    Guard<CriticalMutex> guard{cow_->lock()};
    return lock_count_;
  }
  bool DebugIsReclaimable() const;
  bool DebugIsUnreclaimable() const;
  bool DebugIsDiscarded() const;

  // Helper to assert that the owning cow_'s lock is held on code paths where thread annotations
  // cannot express the locking requirements.
  void assert_cow_pages_locked() TA_ASSERT(cow_->lock()) { AssertHeld(cow_->lock_ref()); }

 private:
  // Helper function to move an object from the |discardable_non_reclaim_candidates_| list to the
  // |discardable_reclaim_candidates_| list.
  void MoveToReclaimCandidatesListLocked() TA_REQ(cow_->lock()) TA_REQ(DiscardableVmosLock::Get());

  // Helper function to move an object from the |discardable_reclaim_candidates_| list to the
  // |discardable_non_reclaim_candidates_| list. If |new_candidate| is true, that indicates that the
  // object was not yet being tracked on any list, and should only be inserted into the
  // |discardable_non_reclaim_candidates_| list without a corresponding list removal.
  void MoveToNonReclaimCandidatesListLocked(bool new_candidate = false) TA_REQ(cow_->lock())
      TA_REQ(DiscardableVmosLock::Get());

  // Returns whether the vmo is in either one of the |discardable_reclaim_candidates_| or
  // |discardable_reclaim_candidates_| lists, depending on whether it is a |reclaim_candidate|
  // or not.
  bool DebugIsInDiscardableListLocked(bool reclaim_candidate) const TA_REQ(cow_->lock())
      TA_EXCL(DiscardableVmosLock::Get());

  // Lock that protects the global discardable lists.
  // This lock can be acquired with the vmo's |lock_| held. To prevent deadlocks, if both locks are
  // required the order of locking should always be 1) vmo's lock, and then 2) DiscardableVmosLock.
  DECLARE_SINGLETON_CRITICAL_MUTEX(DiscardableVmosLock);

  using DiscardableList = fbl::TaggedDoublyLinkedList<VmCowPages*, internal::DiscardableListTag>;

  // Two global lists of discardable vmos:
  // - |discardable_reclaim_candidates_| tracks discardable vmos that are eligible for reclamation
  // and haven't been reclaimed yet.
  // - |discardable_non_reclaim_candidates_| tracks all other discardable VMOs.
  // The lists are protected by the |DiscardableVmosLock|, and updated based on a discardable vmo's
  // state changes (lock, unlock, or discard).
  static DiscardableList discardable_reclaim_candidates_ TA_GUARDED(DiscardableVmosLock::Get());
  static DiscardableList discardable_non_reclaim_candidates_ TA_GUARDED(DiscardableVmosLock::Get());

  // Count of outstanding lock operations. A non-zero count prevents the kernel from discarding /
  // evicting pages from the VMO to relieve memory pressure (currently only applicable if
  // |kDiscardable| is set). Note that this does not prevent removal of pages by other means, like
  // decommitting or resizing, since those are explicit actions driven by the user, not by the
  // kernel directly.
  uint64_t lock_count_ TA_GUARDED(cow_->lock()) = 0;

  // Timestamp of the last unlock operation that changed a discardable vmo's state to
  // |kReclaimable|. Used to determine whether the vmo was accessed too recently to be discarded.
  zx_time_t last_unlock_timestamp_ TA_GUARDED(cow_->lock()) = ZX_TIME_INFINITE;

  // The current state of a discardable vmo, depending on the lock count and whether it has been
  // discarded.
  // State transitions work as follows:
  // 1. kUnreclaimable -> kReclaimable: When the lock count changes from 1 to 0.
  // 2. kReclaimable -> kUnreclaimable: When the lock count changes from 0 to 1. The vmo remains
  // kUnreclaimable for any non-zero lock count.
  // 3. kReclaimable -> kDiscarded: When a vmo with lock count 0 is discarded.
  // 4. kDiscarded -> kUnreclaimable: When a discarded vmo is locked again.
  //
  // We start off with state kUnset, so a discardable vmo must be locked at least once to opt into
  // the above state transitions.
  DiscardableState discardable_state_ TA_GUARDED(cow_->lock()) = DiscardableState::kUnset;

  using Cursor =
      VmoCursor<VmCowPages, DiscardableVmosLock, DiscardableList, DiscardableList::iterator>;

  // The list of all outstanding cursors iterating over the discardable lists:
  // |discardable_reclaim_candidates_| and |discardable_non_reclaim_candidates_|. The cursors should
  // be advanced (by calling AdvanceIf()) before removing any element from the discardable lists.
  static fbl::DoublyLinkedList<Cursor*> discardable_vmos_cursors_
      TA_GUARDED(DiscardableVmosLock::Get());

  // Back reference to the owning VmCowPages. Set at creation time.
  VmCowPages* cow_ = nullptr;
};
#endif  // ZIRCON_KERNEL_VM_INCLUDE_VM_DISCARDABLE_VMO_TRACKER_H_
