// Copyright 2022 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <vm/discardable_vmo_tracker.h>
#include <vm/vm_cow_pages.h>
#include <vm/vm_object.h>

DiscardableVmoTracker::DiscardableList DiscardableVmoTracker::discardable_reclaim_candidates_ = {};
DiscardableVmoTracker::DiscardableList DiscardableVmoTracker::discardable_non_reclaim_candidates_ =
    {};

fbl::DoublyLinkedList<DiscardableVmoTracker::Cursor*>
    DiscardableVmoTracker::discardable_vmos_cursors_ = {};

zx_status_t DiscardableVmoTracker::LockDiscardableLocked(bool try_lock, bool* was_discarded_out) {
  ASSERT(was_discarded_out);
  *was_discarded_out = false;

  if (discardable_state_ == DiscardableState::kDiscarded) {
    DEBUG_ASSERT(lock_count_ == 0);
    *was_discarded_out = true;
    if (try_lock) {
      return ZX_ERR_UNAVAILABLE;
    }
  }

  if (lock_count_ == 0) {
    // Lock count transition from 0 -> 1. Change state to unreclaimable.
    UpdateDiscardableStateLocked(DiscardableState::kUnreclaimable);
  }
  ++lock_count_;

  return ZX_OK;
}

zx_status_t DiscardableVmoTracker::UnlockDiscardableLocked() {
  if (lock_count_ == 0) {
    return ZX_ERR_BAD_STATE;
  }

  if (lock_count_ == 1) {
    // Lock count transition from 1 -> 0. Change state to reclaimable.
    UpdateDiscardableStateLocked(DiscardableState::kReclaimable);
  }
  --lock_count_;

  return ZX_OK;
}

void DiscardableVmoTracker::UpdateDiscardableStateLocked(DiscardableState state) {
  Guard<CriticalMutex> guard{DiscardableVmosLock::Get()};

  DEBUG_ASSERT(state != DiscardableState::kUnset);
  DEBUG_ASSERT(cow_);

  if (state == discardable_state_) {
    return;
  }

  switch (state) {
    case DiscardableState::kReclaimable:
      // The only valid transition into reclaimable is from unreclaimable (lock count 1 -> 0).
      DEBUG_ASSERT(discardable_state_ == DiscardableState::kUnreclaimable);
      DEBUG_ASSERT(lock_count_ == 1);

      // Update the last unlock timestamp.
      last_unlock_timestamp_ = current_time();

      // Move to reclaim candidates list.
      MoveToReclaimCandidatesListLocked();

      break;
    case DiscardableState::kUnreclaimable:
      // The vmo could be reclaimable OR discarded OR not on any list yet. In any case, the lock
      // count should be 0.
      DEBUG_ASSERT(lock_count_ == 0);
      DEBUG_ASSERT(discardable_state_ != DiscardableState::kUnreclaimable);

      if (discardable_state_ == DiscardableState::kDiscarded) {
        // Should already be on the non reclaim candidates list.
        DEBUG_ASSERT(discardable_non_reclaim_candidates_.find_if([this](auto& cow) -> bool {
          return &cow == cow_;
        }) != discardable_non_reclaim_candidates_.end());
      } else {
        // Move to non reclaim candidates list.
        MoveToNonReclaimCandidatesListLocked(discardable_state_ == DiscardableState::kUnset);
      }

      break;
    case DiscardableState::kDiscarded:
      // The only valid transition into discarded is from reclaimable (lock count is 0).
      DEBUG_ASSERT(discardable_state_ == DiscardableState::kReclaimable);
      DEBUG_ASSERT(lock_count_ == 0);

      // Move from reclaim candidates to non reclaim candidates list.
      MoveToNonReclaimCandidatesListLocked();

      break;
    default:
      break;
  }

  // Update the state.
  discardable_state_ = state;
}

void DiscardableVmoTracker::RemoveFromDiscardableListLocked() {
  Guard<CriticalMutex> guard{DiscardableVmosLock::Get()};
  if (discardable_state_ == DiscardableState::kUnset) {
    return;
  }

  DEBUG_ASSERT(cow_);
  DEBUG_ASSERT(fbl::InContainer<internal::DiscardableListTag>(*cow_));

  Cursor::AdvanceCursors(discardable_vmos_cursors_, cow_);

  if (discardable_state_ == DiscardableState::kReclaimable) {
    discardable_reclaim_candidates_.erase(*cow_);
  } else {
    discardable_non_reclaim_candidates_.erase(*cow_);
  }

  discardable_state_ = DiscardableState::kUnset;
  cow_ = nullptr;
}

void DiscardableVmoTracker::MoveToReclaimCandidatesListLocked() {
  DEBUG_ASSERT(cow_);
  DEBUG_ASSERT(fbl::InContainer<internal::DiscardableListTag>(*cow_));

  Cursor::AdvanceCursors(discardable_vmos_cursors_, cow_);
  discardable_non_reclaim_candidates_.erase(*cow_);

  discardable_reclaim_candidates_.push_back(cow_);
}

void DiscardableVmoTracker::MoveToNonReclaimCandidatesListLocked(bool new_candidate) {
  DEBUG_ASSERT(cow_);
  if (new_candidate) {
    DEBUG_ASSERT(!fbl::InContainer<internal::DiscardableListTag>(*cow_));
  } else {
    DEBUG_ASSERT(fbl::InContainer<internal::DiscardableListTag>(*cow_));
    Cursor::AdvanceCursors(discardable_vmos_cursors_, cow_);
    discardable_reclaim_candidates_.erase(*cow_);
  }

  discardable_non_reclaim_candidates_.push_back(cow_);
}

bool DiscardableVmoTracker::DebugIsInDiscardableListLocked(bool reclaim_candidate) const {
  Guard<CriticalMutex> guard{DiscardableVmosLock::Get()};

  // Not on any list yet. Nothing else to verify.
  if (discardable_state_ == DiscardableState::kUnset) {
    return false;
  }

  DEBUG_ASSERT(cow_);
  DEBUG_ASSERT(fbl::InContainer<internal::DiscardableListTag>(*cow_));

  auto iter_c =
      discardable_reclaim_candidates_.find_if([this](auto& cow) -> bool { return &cow == cow_; });
  auto iter_nc = discardable_non_reclaim_candidates_.find_if(
      [this](auto& cow) -> bool { return &cow == cow_; });

  if (reclaim_candidate) {
    // Verify that the vmo is in the |discardable_reclaim_candidates_| list and NOT in the
    // |discardable_non_reclaim_candidates_| list.
    if (iter_c != discardable_reclaim_candidates_.end() &&
        iter_nc == discardable_non_reclaim_candidates_.end()) {
      return true;
    }
  } else {
    // Verify that the vmo is in the |discardable_non_reclaim_candidates_| list and NOT in the
    // |discardable_reclaim_candidates_| list.
    if (iter_nc != discardable_non_reclaim_candidates_.end() &&
        iter_c == discardable_reclaim_candidates_.end()) {
      return true;
    }
  }

  return false;
}

bool DiscardableVmoTracker::DebugIsReclaimable() const {
  Guard<CriticalMutex> guard{cow_->lock()};
  if (discardable_state_ != DiscardableState::kReclaimable) {
    return false;
  }
  return DebugIsInDiscardableListLocked(/*reclaim_candidate=*/true);
}

bool DiscardableVmoTracker::DebugIsUnreclaimable() const {
  Guard<CriticalMutex> guard{cow_->lock()};
  if (discardable_state_ != DiscardableState::kUnreclaimable) {
    return false;
  }
  return DebugIsInDiscardableListLocked(/*reclaim_candidate=*/false);
}

bool DiscardableVmoTracker::DebugIsDiscarded() const {
  Guard<CriticalMutex> guard{cow_->lock()};
  if (discardable_state_ != DiscardableState::kDiscarded) {
    return false;
  }
  return DebugIsInDiscardableListLocked(/*reclaim_candidate=*/false);
}

bool DiscardableVmoTracker::IsEligibleForReclamationLocked(
    zx_duration_t min_duration_since_reclaimable) const {
  // We've raced with a lock operation. Bail without doing anything. The lock operation will have
  // already moved it to the unreclaimable list.
  if (discardable_state_ != DiscardableVmoTracker::DiscardableState::kReclaimable) {
    return false;
  }

  // If the vmo was unlocked less than |min_duration_since_reclaimable| in the past, do not discard
  // from it yet.
  if (zx_time_sub_time(current_time(), last_unlock_timestamp_) < min_duration_since_reclaimable) {
    return false;
  }

  // We've verified that the state is |kReclaimable|, so the lock count should be zero.
  DEBUG_ASSERT(lock_count_ == 0);

  return true;
}

// static
DiscardableVmoTracker::DiscardablePageCounts DiscardableVmoTracker::DebugDiscardablePageCounts() {
  DiscardablePageCounts total_counts = {};
  Guard<CriticalMutex> guard{DiscardableVmosLock::Get()};

  // The union of the two lists should give us a list of all discardable vmos.
  DiscardableList* lists_to_process[] = {&discardable_reclaim_candidates_,
                                         &discardable_non_reclaim_candidates_};

  for (auto list : lists_to_process) {
    Cursor cursor(DiscardableVmosLock::Get(), *list, discardable_vmos_cursors_);
    AssertHeld(cursor.lock_ref());

    VmCowPages* cow;
    while ((cow = cursor.Next())) {
      fbl::RefPtr<VmCowPages> cow_ref = fbl::MakeRefPtrUpgradeFromRaw(cow, guard);
      if (cow_ref) {
        // Get page counts for each vmo outside of the |DiscardableVmosLock|, since
        // DebugGetDiscardablePageCounts() will acquire the VmCowPages lock. Holding the
        // |DiscardableVmosLock| while acquiring the VmCowPages lock will violate lock ordering
        // constraints between the two.
        //
        // Since we upgraded the raw pointer to a RefPtr under the |DiscardableVmosLock|, we know
        // that the object is valid. We will call Next() on our cursor after re-acquiring the
        // |DiscardableVmosLock| to safely iterate to the next element on the list.
        guard.CallUnlocked([&total_counts, cow_ref = ktl::move(cow_ref)]() mutable {
          DiscardablePageCounts counts = cow_ref->DebugGetDiscardablePageCounts();
          total_counts.locked += counts.locked;
          total_counts.unlocked += counts.unlocked;

          // Explicitly reset the RefPtr to force any destructor to run right now and not in the
          // cleanup of the lambda, which might happen after the |DiscardableVmosLock| has been
          // re-acquired.
          cow_ref.reset();
        });
      }
    }
  }

  return total_counts;
}

// static
uint64_t DiscardableVmoTracker::ReclaimPagesFromDiscardableVmos(
    uint64_t target_pages, zx_duration_t min_duration_since_reclaimable, list_node_t* freed_list) {
  uint64_t total_pages_discarded = 0;
  Guard<CriticalMutex> guard{DiscardableVmosLock::Get()};

  Cursor cursor(DiscardableVmosLock::Get(), discardable_reclaim_candidates_,
                discardable_vmos_cursors_);
  AssertHeld(cursor.lock_ref());

  while (total_pages_discarded < target_pages) {
    VmCowPages* cow = cursor.Next();
    // No vmos to reclaim pages from.
    if (cow == nullptr) {
      break;
    }

    fbl::RefPtr<VmCowPages> cow_ref = fbl::MakeRefPtrUpgradeFromRaw(cow, guard);
    if (cow_ref) {
      // We obtained the RefPtr above under the |DiscardableVmosLock|, so we know the object is
      // valid. We could not have raced with destruction, since the object is removed from the
      // discardable list on the destruction path, which requires the |DiscardableVmosLock|.
      //
      // DiscardPages() will acquire the VmCowPages |lock_|, so it needs to be called outside of
      // the |DiscardableVmosLock|. This preserves lock ordering constraints between the two locks
      // - |DiscardableVmosLock| can be acquired while holding the VmCowPages |lock_|, but not the
      // other way around.
      guard.CallUnlocked([&total_pages_discarded, min_duration_since_reclaimable, &freed_list,
                          cow_ref = ktl::move(cow_ref)]() mutable {
        total_pages_discarded += cow_ref->DiscardPages(min_duration_since_reclaimable, freed_list);

        // Explicitly reset the RefPtr to force any destructor to run right now and not in the
        // cleanup of the lambda, which might happen after the |DiscardableVmosLock| has been
        // re-acquired.
        cow_ref.reset();
      });
    }
  }
  return total_pages_discarded;
}
