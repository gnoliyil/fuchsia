// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include "vm/vm_page_list.h"

#include <align.h>
#include <inttypes.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <fbl/alloc_checker.h>
#include <ktl/move.h>
#include <vm/compression.h>
#include <vm/pmm.h>
#include <vm/vm.h>
#include <vm/vm_object_paged.h>

#include "vm_priv.h"

#include <ktl/enforce.h>

#define LOCAL_TRACE VM_GLOBAL_TRACE(0)

namespace {

inline uint64_t offset_to_node_offset(uint64_t offset, uint64_t skew) {
  return ROUNDDOWN(offset + skew, PAGE_SIZE * VmPageListNode::kPageFanOut);
}

inline uint64_t offset_to_node_index(uint64_t offset, uint64_t skew) {
  return ((offset + skew) >> PAGE_SIZE_SHIFT) % VmPageListNode::kPageFanOut;
}

inline void move_vm_page_list_node(VmPageListNode* dest, VmPageListNode* src) {
  // Called by move ctor/assignment. Move assignment clears the dest node first.
  ASSERT(dest->IsEmpty());

  for (unsigned i = 0; i < VmPageListNode::kPageFanOut; i++) {
    dest->Lookup(i) = ktl::move(src->Lookup(i));
  }
}

}  // namespace

VmPageListNode::VmPageListNode(uint64_t offset) : obj_offset_(offset) {
  LTRACEF("%p offset %#" PRIx64 "\n", this, obj_offset_);
}

VmPageListNode::~VmPageListNode() {
  LTRACEF("%p offset %#" PRIx64 "\n", this, obj_offset_);
  canary_.Assert();

  DEBUG_ASSERT(HasNoPageOrRef());
}

VmPageList::VmPageList() { LTRACEF("%p\n", this); }

VmPageList::VmPageList(VmPageList&& other) : list_(ktl::move(other.list_)) {
  LTRACEF("%p\n", this);
  list_skew_ = other.list_skew_;
}

VmPageList::~VmPageList() {
  LTRACEF("%p\n", this);
  DEBUG_ASSERT(HasNoPageOrRef());
}

VmPageList& VmPageList::operator=(VmPageList&& other) {
  list_ = ktl::move(other.list_);
  list_skew_ = other.list_skew_;
  return *this;
}

VmPageOrMarker* VmPageList::LookupOrAllocate(uint64_t offset) {
  uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  size_t index = offset_to_node_index(offset, list_skew_);

  if (node_offset >= VmPageList::MAX_SIZE) {
    return nullptr;
  }

  LTRACEF_LEVEL(2, "%p offset %#" PRIx64 " node_offset %#" PRIx64 " index %zu\n", this, offset,
                node_offset, index);

  // lookup the tree node that holds this page
  auto pln = list_.find(node_offset);
  if (pln.IsValid()) {
    return &pln->Lookup(index);
  }

  fbl::AllocChecker ac;
  ktl::unique_ptr<VmPageListNode> pl =
      ktl::unique_ptr<VmPageListNode>(new (&ac) VmPageListNode(node_offset));
  if (!ac.check()) {
    return nullptr;
  }

  LTRACEF("allocating new inner node %p\n", pl.get());

  VmPageOrMarker& p = pl->Lookup(index);

  list_.insert(ktl::move(pl));
  return &p;
}

void VmPageList::ReturnEmptySlot(uint64_t offset) {
  uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  size_t index = offset_to_node_index(offset, list_skew_);

  LTRACEF_LEVEL(2, "%p offset %#" PRIx64 " node_offset %#" PRIx64 " index %zu\n", this, offset,
                node_offset, index);

  // lookup the tree node that holds this offset
  auto pln = list_.find(node_offset);
  DEBUG_ASSERT(pln.IsValid());

  // check that the slot was empty
  [[maybe_unused]] VmPageOrMarker page = ktl::move(pln->Lookup(index));
  DEBUG_ASSERT(page.IsEmpty());
  if (pln->IsEmpty()) {
    // node is empty, erase it.
    list_.erase(*pln);
  }
}

const VmPageOrMarker* VmPageList::Lookup(uint64_t offset) const {
  uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  size_t index = offset_to_node_index(offset, list_skew_);

  LTRACEF_LEVEL(2, "%p offset %#" PRIx64 " node_offset %#" PRIx64 " index %zu\n", this, offset,
                node_offset, index);

  // lookup the tree node that holds this page
  auto pln = list_.find(node_offset);
  if (!pln.IsValid()) {
    return nullptr;
  }

  return &pln->Lookup(index);
}

VmPageOrMarkerRef VmPageList::LookupMutable(uint64_t offset) {
  uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  size_t index = offset_to_node_index(offset, list_skew_);

  LTRACEF_LEVEL(2, "%p offset %#" PRIx64 " node_offset %#" PRIx64 " index %zu\n", this, offset,
                node_offset, index);

  // lookup the tree node that holds this page
  auto pln = list_.find(node_offset);
  if (!pln.IsValid()) {
    return VmPageOrMarkerRef(nullptr);
  }

  return VmPageOrMarkerRef(&pln->Lookup(index));
}

VMPLCursor VmPageList::LookupMutableCursor(uint64_t offset) {
  uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  size_t index = offset_to_node_index(offset, list_skew_);

  LTRACEF_LEVEL(2, "%p offset %#" PRIx64 " node_offset %#" PRIx64 " index %zu\n", this, offset,
                node_offset, index);

  // lookup the tree node that holds this page
  auto pln = list_.find(node_offset);
  if (!pln.IsValid()) {
    return VMPLCursor();
  }

  return VMPLCursor(ktl::move(pln), static_cast<uint>(index));
}

VmPageOrMarker VmPageList::RemoveContent(uint64_t offset) {
  uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  size_t index = offset_to_node_index(offset, list_skew_);

  LTRACEF_LEVEL(2, "%p offset %#" PRIx64 " node_offset %#" PRIx64 " index %zu\n", this, offset,
                node_offset, index);

  // lookup the tree node that holds this page
  auto pln = list_.find(node_offset);
  if (!pln.IsValid()) {
    return VmPageOrMarker::Empty();
  }

  // free this page
  VmPageOrMarker page = ktl::move(pln->Lookup(index));
  if (!page.IsEmpty() && pln->IsEmpty()) {
    // if it was the last item in the node, remove the node from the tree
    LTRACEF_LEVEL(2, "%p freeing the list node\n", this);
    list_.erase(*pln);
  }
  return page;
}

bool VmPageList::IsEmpty() const { return list_.is_empty(); }

bool VmPageList::HasNoPageOrRef() const {
  bool no_pages = true;
  ForEveryPage([&no_pages](auto* p, uint64_t) {
    if (p->IsPageOrRef()) {
      no_pages = false;
      return ZX_ERR_STOP;
    }
    return ZX_ERR_NEXT;
  });
  return no_pages;
}

ktl::pair<const VmPageOrMarker*, uint64_t> VmPageList::FindIntervalStartForEnd(
    uint64_t end_offset) const {
  // Find the node that would contain the end offset.
  const uint64_t node_offset = offset_to_node_offset(end_offset, list_skew_);
  auto pln = list_.find(node_offset);
  DEBUG_ASSERT(pln.IsValid());
  const size_t node_index = offset_to_node_index(end_offset, list_skew_);
  DEBUG_ASSERT(pln->Lookup(node_index).IsIntervalEnd());

  // The only populated slots in an interval are the start and the end. So the interval start will
  // either be in the same node as the interval end, or the previous populated node to the left.
  size_t index = node_index;
  while (index > 0) {
    index--;
    auto slot = &pln->Lookup(index);
    if (!slot->IsEmpty()) {
      DEBUG_ASSERT(slot->IsIntervalStart());
      return {slot, pln->offset() + index * PAGE_SIZE - list_skew_};
    }
  }

  // We could not find the start in the same node. Check the previous one.
  pln--;
  for (index = VmPageListNode::kPageFanOut - 1; index >= 0; index--) {
    auto slot = &pln->Lookup(index);
    if (!slot->IsEmpty()) {
      DEBUG_ASSERT(slot->IsIntervalStart());
      return {slot, pln->offset() + index * PAGE_SIZE - list_skew_};
    }
  }

  // Should not reach here.
  ASSERT(false);
  return {nullptr, UINT64_MAX};
}

ktl::pair<const VmPageOrMarker*, uint64_t> VmPageList::FindIntervalEndForStart(
    uint64_t start_offset) const {
  // Find the node that would contain the start offset.
  const uint64_t node_offset = offset_to_node_offset(start_offset, list_skew_);
  auto pln = list_.find(node_offset);
  DEBUG_ASSERT(pln.IsValid());
  const size_t node_index = offset_to_node_index(start_offset, list_skew_);
  DEBUG_ASSERT(pln->Lookup(node_index).IsIntervalStart());

  // The only populated slots in an interval are the start and the end. So the interval end will
  // either be in the same node as the interval start, or the next populated node to the right.
  size_t index = node_index;
  while (index < VmPageListNode::kPageFanOut - 1) {
    index++;
    auto slot = &pln->Lookup(index);
    if (!slot->IsEmpty()) {
      DEBUG_ASSERT(slot->IsIntervalEnd());
      return {slot, pln->offset() + index * PAGE_SIZE - list_skew_};
    }
  }

  // We could not find the end in the same node. Check the next one.
  pln++;
  for (index = 0; index < VmPageListNode::kPageFanOut; index++) {
    auto slot = &pln->Lookup(index);
    if (!slot->IsEmpty()) {
      DEBUG_ASSERT(slot->IsIntervalEnd());
      return {slot, pln->offset() + index * PAGE_SIZE - list_skew_};
    }
  }

  // Should not reach here.
  ASSERT(false);
  return {nullptr, UINT64_MAX};
}

ktl::pair<VmPageOrMarker*, bool> VmPageList::LookupOrAllocateCheckForInterval(uint64_t offset,
                                                                              bool split_interval) {
  // Find the node that would contain this offset.
  const uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  const size_t node_index = offset_to_node_index(offset, list_skew_);
  if (node_offset >= VmPageList::MAX_SIZE) {
    return {nullptr, false};
  }

  // If the node containing offset is populated, the lower bound will return that node. If not
  // populated, it will return the next populated node.
  //
  // The overall intent with this function is to keep the number of tree lookups similar to
  // LookupOrAllocate for both empty and non-empty slots. So we hold on to the looked up node and
  // walk left or right in the tree only if required. The same principle is followed for traversal
  // within a node as well, the ordering of operations is chosen such that we can exit the traversal
  // as soon as possible or avoid it entirely.
  auto pln = list_.lower_bound(node_offset);

  // The slot that will eventually hold offset.
  VmPageOrMarker* slot = nullptr;

  // If offset falls in an interval, this will hold an interval sentinel for the interval that
  // offset is found in. It will be used to mint new sentinel values if we were also asked to split
  // the interval.
  const VmPageOrMarker* found_interval = nullptr;
  bool is_in_interval = false;
  // For the offset to lie in an interval, it is going to have an interval end so we should have
  // found some valid node if the offset falls in an interval. If we could not find a valid node,
  // we know that offset cannot lie in an interval, so skip the check.
  if (pln.IsValid()) {
    if (pln->offset() == node_offset) {
      // We found the node containing offset. Get the slot.
      slot = &pln->Lookup(node_index);
      // Short circuit the IfOffsetInIntervalHelper call below if the slot itself is an interval
      // sentinel. This is purely an optimization, and it would be okay to call
      // IfOffsetInIntervalHelper for this case too.
      if (slot->IsInterval()) {
        is_in_interval = true;
        found_interval = slot;
      }
    }

    if (!is_in_interval) {
      is_in_interval = IfOffsetInIntervalHelper(offset, *pln, &found_interval);
      // If we found an interval, we should have found a valid interval sentinel too.
      DEBUG_ASSERT(!is_in_interval || found_interval->IsInterval());
    }

    // If we are in an interval but cannot split it, we cannot return a slot. The caller should not
    // be able to manipulate the slot freely without correctly handling the interval(s) around it.
    if (is_in_interval && !split_interval) {
      return {nullptr, true};
    }
  }

  // We won't have a valid slot if the node we looked up did not contain the required offset.
  if (!slot) {
    // Allocate the node that would contain offset and then get the slot.
    fbl::AllocChecker ac;
    ktl::unique_ptr<VmPageListNode> pl =
        ktl::unique_ptr<VmPageListNode>(new (&ac) VmPageListNode(node_offset));
    if (!ac.check()) {
      return {nullptr, is_in_interval};
    }
    VmPageListNode& raw_node = *pl;
    slot = &pl->Lookup(node_index);
    list_.insert(ktl::move(pl));
    pln = list_.make_iterator(raw_node);
  }

  // If offset does not lie in an interval, or if the slot is already a single page interval, there
  // is nothing more to be done. Return the slot.
  if (!is_in_interval || slot->IsIntervalSlot()) {
    // We currently only support zero intervals.
    DEBUG_ASSERT(!is_in_interval || slot->IsIntervalZero());
    return {slot, is_in_interval};
  }

  // If we reached here, we know that we are in an interval and we need to split it in order to
  // return the required slot.
  DEBUG_ASSERT(is_in_interval && split_interval);
  DEBUG_ASSERT(pln.IsValid());

  // Depending on whether the slot is empty or not, we might need to insert a new interval
  // start, a new interval end, or both. Figure out which slots are needed first.
  //  - If the slot is empty, we need to insert an end to the left, a start to the right and a
  //  single page interval slot at offset. So we need both a new start and a new end.
  //  - If the slot is populated, since we know that offset falls in an interval, it could only be
  //  an interval start or an interval end. We will either need a new start or a new end but not
  //  both.
  bool need_new_end = true, need_new_start = true;
  if (slot->IsIntervalStart()) {
    // We can move the interval start to the right, and replace the old start with a slot. Don't
    // need a new end.
    need_new_end = false;
  } else if (slot->IsIntervalEnd()) {
    // We can move the interval end to the left, and replace the old end with a slot. Don't need a
    // new start.
    need_new_start = false;
  }

  // Now find the previous and next slots as needed for the new end and new start respectively.
  // Note that the node allocations below are mutually exclusive. If we allocate the previous node,
  // we won't need to allocate the next node and vice versa, since the allocation logic depends on
  // the value of node_index. So if the required node allocation fails, all the cleanup that's
  // required is returning the slot at offset if it is empty, as we might have allocated a new node
  // previously to hold offset.
  VmPageOrMarker* new_end = nullptr;
  if (need_new_end) {
    if (node_index > 0) {
      // The previous slot is in the same node.
      new_end = &pln->Lookup(node_index - 1);
    } else {
      // The previous slot is in the node to the left. We might need to allocate a new node to the
      // left if it does not exist. Try to walk left and see if we find the previous node we're
      // looking for.
      auto iter = pln;
      iter--;
      // We are here because slot was either an empty slot inside an interval or it was an interval
      // end. Additionally, the slot was the left-most slot in its node, which means we are
      // guaranteed to find a node to the left which holds the start of the interval.
      DEBUG_ASSERT(iter.IsValid());
      const uint64_t prev_node_offset = node_offset - VmPageListNode::kPageFanOut * PAGE_SIZE;
      if (iter->offset() == prev_node_offset) {
        new_end = &iter->Lookup(VmPageListNode::kPageFanOut - 1);
      } else {
        DEBUG_ASSERT(iter->offset() < prev_node_offset);
        fbl::AllocChecker ac;
        ktl::unique_ptr<VmPageListNode> pl =
            ktl::unique_ptr<VmPageListNode>(new (&ac) VmPageListNode(prev_node_offset));
        if (!ac.check()) {
          if (slot->IsEmpty()) {
            ReturnEmptySlot(offset);
          }
          return {nullptr, true};
        }
        new_end = &pl->Lookup(VmPageListNode::kPageFanOut - 1);
        list_.insert(ktl::move(pl));
      }
    }
    DEBUG_ASSERT(new_end);
  }

  VmPageOrMarker* new_start = nullptr;
  if (need_new_start) {
    if (node_index < VmPageListNode::kPageFanOut - 1) {
      // The next slot is in the same node.
      new_start = &pln->Lookup(node_index + 1);
    } else {
      // The next slot is in the node to the right. We might need to allocate a new node to the
      // right if it does not exist. Try to walk right and see if we find the next node we're
      // looking for.
      auto iter = pln;
      iter++;
      // We are here because slot was either empty or it was an interval start. Additionally, the
      // slot was the right-most slot in its node, which means we are guaranteed to find a node to
      // the right which holds the end of the interval.
      DEBUG_ASSERT(iter.IsValid());
      const uint64_t next_node_offset = node_offset + VmPageListNode::kPageFanOut * PAGE_SIZE;
      if (iter->offset() == next_node_offset) {
        new_start = &iter->Lookup(0);
      } else {
        DEBUG_ASSERT(iter->offset() > next_node_offset);
        fbl::AllocChecker ac;
        ktl::unique_ptr<VmPageListNode> pl =
            ktl::unique_ptr<VmPageListNode>(new (&ac) VmPageListNode(next_node_offset));
        if (!ac.check()) {
          if (slot->IsEmpty()) {
            ReturnEmptySlot(offset);
          }
          return {nullptr, true};
        }
        new_start = &pl->Lookup(0);
        list_.insert(ktl::move(pl));
      }
    }
    DEBUG_ASSERT(new_start);
  }

  // Helper to mint new sentinel values for the split. Only creates zero ranges. If we support
  // other page interval types in the future, we will need to modify this to support them.
  auto mint_new_sentinel =
      [&found_interval](VmPageOrMarker::IntervalSentinel sentinel) -> VmPageOrMarker {
    // We only support zero intervals for now.
    DEBUG_ASSERT(found_interval->IsIntervalZero());
    // Preserve dirty state across the split.
    return VmPageOrMarker::ZeroInterval(sentinel, found_interval->GetZeroIntervalDirtyState());
  };

  // Now that we've looked up the relevant slots after performing any required allocations, make
  // the actual change. Install new end and start sentinels on the left and right of offset
  // respectively.
  if (new_start) {
    if (new_start->IsIntervalEnd()) {
      // If an interval was ending at the next slot, change it into a Slot sentinel.
      new_start->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Slot);
    } else {
      DEBUG_ASSERT(new_start->IsEmpty());
      *new_start = mint_new_sentinel(VmPageOrMarker::IntervalSentinel::Start);
    }
  }
  if (new_end) {
    if (new_end->IsIntervalStart()) {
      // If an interval was starting at the previous slot, change it into a Slot sentinel.
      new_end->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Slot);
    } else {
      DEBUG_ASSERT(new_end->IsEmpty());
      *new_end = mint_new_sentinel(VmPageOrMarker::IntervalSentinel::End);
    }
  }

  // Finally, install a slot sentinel at offset.
  if (slot->IsEmpty()) {
    *slot = mint_new_sentinel(VmPageOrMarker::IntervalSentinel::Slot);
  } else {
    DEBUG_ASSERT(slot->IsIntervalStart() || slot->IsIntervalEnd());
    slot->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Slot);
  }

  return {slot, true};
}

void VmPageList::ReturnIntervalSlot(uint64_t offset) {
  // We should be able to lookup a pre-existing interval slot.
  auto slot = LookupOrAllocate(offset);
  DEBUG_ASSERT(slot);
  DEBUG_ASSERT(slot->IsIntervalSlot());

  // We only support zero intervals for now. If more interval types are added in the future, handle
  // them here.
  DEBUG_ASSERT(slot->IsIntervalZero());
  auto dirty_state = slot->GetZeroIntervalDirtyState();
  // Temporarily free up the slot and then add a zero interval back in at the same spot using
  // AddZeroInterval, which will ensure that the slot is merged to the left and/or right as
  // applicable.
  *slot = VmPageOrMarker::Empty();
  [[maybe_unused]] zx_status_t status = AddZeroInterval(offset, offset + PAGE_SIZE, dirty_state);
  DEBUG_ASSERT(status == ZX_OK);
}

zx_status_t VmPageList::PopulateSlotsInInterval(uint64_t start_offset, uint64_t end_offset) {
  DEBUG_ASSERT(IS_PAGE_ALIGNED(start_offset));
  DEBUG_ASSERT(IS_PAGE_ALIGNED(end_offset));
  DEBUG_ASSERT(end_offset > start_offset);
  // Change the end_offset to an inclusive offset for convenience.
  end_offset -= PAGE_SIZE;

#if DEBUG_ASSERT_IMPLEMENTED
  // The start_offset and end_offset should lie in an interval.
  ASSERT(IsOffsetInInterval(start_offset));
  ASSERT(IsOffsetInInterval(end_offset));
  // All the remaining offsets should be empty and lie in the same interval. So we should find no
  // pages or gaps in the range [start_offset + PAGE_SIZE, end_offset - PAGE_SIZE].
  if (start_offset + PAGE_SIZE < end_offset) {
    zx_status_t status =
        ForEveryPageAndGapInRange([](auto* p, uint64_t off) { return ZX_ERR_BAD_STATE; },
                                  [](uint64_t start, uint64_t end) { return ZX_ERR_BAD_STATE; },
                                  start_offset + PAGE_SIZE, end_offset);
    ASSERT(status == ZX_OK);
  }
#endif

  // First allocate slots at start_offset and end_offset, splitting the interval around them if
  // required. If any of the subsequent operations fail, we should return these interval slots. This
  // function should either be able to populate all the slots requested, or the interval should be
  // returned to its original state before the call.
  auto [start_slot, is_start_in_interval] = LookupOrAllocateCheckForInterval(start_offset, true);
  if (!start_slot) {
    return ZX_ERR_NO_MEMORY;
  }
  DEBUG_ASSERT(is_start_in_interval);
  DEBUG_ASSERT(start_slot->IsIntervalSlot());
  // If only asked to populate single slot, nothing more to do.
  if (start_offset == end_offset) {
    return ZX_OK;
  }

  auto [end_slot, is_end_in_interval] = LookupOrAllocateCheckForInterval(end_offset, true);
  if (!end_slot) {
    // Return the start slot before returning.
    ReturnIntervalSlot(start_offset);
    return ZX_ERR_NO_MEMORY;
  }
  DEBUG_ASSERT(is_end_in_interval);
  DEBUG_ASSERT(end_slot->IsIntervalSlot());

  // If there are no more empty slots to consider between start and end, return early.
  if (end_offset == start_offset + PAGE_SIZE) {
    return ZX_OK;
  }

  // Now we need to walk all page offsets from start_offset to end_offset and convert them all to
  // interval slots. Before we can do that, we will first allocate any page list nodes required in
  // the middle. After splitting the interval around start_offset, we know that the node containing
  // |start_offset + PAGE_SIZE| will be populated in order for it to hold the interval start
  // sentinel at that offset. Similarly, we know that the node containing |end_offset - PAGE_SIZE|
  // will be populated. So all the unpopulated nodes (if any) will lie between these two nodes.
  const uint64_t first_node_offset = offset_to_node_offset(start_offset + PAGE_SIZE, list_skew_);
  const size_t first_node_index = offset_to_node_index(start_offset + PAGE_SIZE, list_skew_);
  const uint64_t last_node_offset = offset_to_node_offset(end_offset - PAGE_SIZE, list_skew_);
  const size_t last_node_index = offset_to_node_index(end_offset - PAGE_SIZE, list_skew_);
  DEBUG_ASSERT(last_node_offset >= first_node_offset);
  if (last_node_offset > first_node_offset + VmPageListNode::kPageFanOut * PAGE_SIZE) {
    const uint64_t first_unpopulated = first_node_offset + VmPageListNode::kPageFanOut * PAGE_SIZE;
    const uint64_t last_unpopulated = last_node_offset - VmPageListNode::kPageFanOut * PAGE_SIZE;
    for (uint64_t node_offset = first_unpopulated; node_offset <= last_unpopulated;
         node_offset += VmPageListNode::kPageFanOut * PAGE_SIZE) {
      fbl::AllocChecker ac;
      ktl::unique_ptr<VmPageListNode> pl =
          ktl::unique_ptr<VmPageListNode>(new (&ac) VmPageListNode(node_offset));
      if (!ac.check()) {
        // If allocating a new node fails, clean up all the new nodes we might have installed until
        // this point, which is all the empty nodes starting at first_unpopulated to before the node
        // that failed.
        for (uint64_t off = first_unpopulated; off < node_offset;
             off += VmPageListNode::kPageFanOut * PAGE_SIZE) {
          [[maybe_unused]] auto node = list_.erase(off);
          DEBUG_ASSERT(node->IsEmpty());
        }
        // Also return the start and end slots that we split above.
        ReturnIntervalSlot(start_offset);
        ReturnIntervalSlot(end_offset);
        return ZX_ERR_NO_MEMORY;
      }
      list_.insert(ktl::move(pl));
    }
  }

  // Helper to mint new slot sentinels to insert. Currently we only support zero intervals. If we
  // support other page interval types in the future, we will need to modify this to support them.
  const VmPageOrMarker* start = start_slot;
  const VmPageOrMarker* end = end_slot;
  auto mint_new_slot = [start, end]() -> VmPageOrMarker {
    DEBUG_ASSERT(start->GetZeroIntervalDirtyState() == end->GetZeroIntervalDirtyState());
    return VmPageOrMarker::ZeroInterval(VmPageOrMarker::IntervalSentinel::Slot,
                                        start->GetZeroIntervalDirtyState());
  };

  // Now that all allocations have succeeded, we know that the rest of the operation cannot fail.
  // Walk all offsets after start_offset and before end_offset, overwriting all the slots as
  // interval slots.
  uint64_t node_offset = first_node_offset;
  auto pln = list_.find(node_offset);
  while (node_offset <= last_node_offset) {
    DEBUG_ASSERT(pln.IsValid());
    DEBUG_ASSERT(pln->offset() == node_offset);
    for (size_t index = (node_offset == first_node_offset ? first_node_index : 0);
         index <=
         (node_offset == last_node_offset ? last_node_index : VmPageListNode::kPageFanOut - 1);
         index++) {
      pln->Lookup(index) = mint_new_slot();
    }
    pln++;
    node_offset += VmPageListNode::kPageFanOut * PAGE_SIZE;
  }

#if DEBUG_ASSERT_IMPLEMENTED
  // All offsets in the range [start_offset, end_offset] should contain interval slots.
  uint64_t next_off = start_offset;
  zx_status_t status = ForEveryPageInRange(
      [&next_off](auto* p, uint64_t off) {
        if (off != next_off || !p->IsIntervalSlot()) {
          return ZX_ERR_BAD_STATE;
        }
        next_off += PAGE_SIZE;
        return ZX_ERR_NEXT;
      },
      start_offset, end_offset + PAGE_SIZE);
  ASSERT(status == ZX_OK);
#endif

  return ZX_OK;
}

bool VmPageList::IsOffsetInZeroInterval(uint64_t offset) const {
  // Find the node that would contain this offset.
  const uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  // If the node containing offset is populated, the lower bound will return that node. If not
  // populated, it will return the next populated node.
  auto pln = list_.lower_bound(node_offset);

  // Could not find a valid node >= node_offset. So offset cannot be part of an interval, an
  // interval would have an end slot.
  if (!pln.IsValid()) {
    return false;
  }
  // The page list shouldn't have any empty nodes.
  DEBUG_ASSERT(!pln->IsEmpty());

  // Check if offset is in an interval also querying the associated sentinel.
  const VmPageOrMarker* interval = nullptr;
  bool in_interval = IfOffsetInIntervalHelper(offset, *pln, &interval);
  DEBUG_ASSERT(!in_interval || interval->IsInterval());
  return in_interval ? interval->IsIntervalZero() : false;
}

bool VmPageList::IsOffsetInInterval(uint64_t offset) const {
  // Find the node that would contain this offset.
  const uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  // If the node containing offset is populated, the lower bound will return that node. If not
  // populated, it will return the next populated node.
  auto pln = list_.lower_bound(node_offset);

  // Could not find a valid node >= node_offset. So offset cannot be part of an interval, an
  // interval would have an end slot.
  if (!pln.IsValid()) {
    return false;
  }
  // The page list shouldn't have any empty nodes.
  DEBUG_ASSERT(!pln->IsEmpty());
  return IfOffsetInIntervalHelper(offset, *pln);
}

bool VmPageList::IfOffsetInIntervalHelper(uint64_t offset, const VmPageListNode& lower_bound,
                                          const VmPageOrMarker** interval_out) const {
  DEBUG_ASSERT(!lower_bound.IsEmpty());
  if (interval_out) {
    *interval_out = nullptr;
  }

  // Helper to return success if an interval sentinel is found.
  auto found_interval = [&interval_out](const VmPageOrMarker* interval) {
    if (interval_out) {
      *interval_out = interval;
    }
    return true;
  };

  const uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
  const size_t node_index = offset_to_node_index(offset, list_skew_);

  // For the offset to lie in an interval, it is going to have an interval end so we should have
  // found some valid node if the offset falls in an interval. See if offset falls in an interval.

  // We found the node containing offset.
  if (lower_bound.offset() == node_offset) {
    auto& slot = lower_bound.Lookup(node_index);
    // Check if the slot is an interval sentinel.
    if (slot.IsInterval()) {
      return found_interval(&slot);
    }
    if (!slot.IsEmpty()) {
      // For any other non-empty slot, we know this is not an interval.
      return false;
    }
    // The slot is empty. Walk to the left and right and lookup the closest populated
    // slots. We should find either an interval start to the left or an end to the right. Try to
    // find a start to the left first.
    DEBUG_ASSERT(slot.IsEmpty());
    for (size_t i = node_index; i > 0; i--) {
      auto& p = lower_bound.Lookup(i - 1);
      // Found the first populated slot to the left.
      if (!p.IsEmpty()) {
        if (p.IsIntervalStart()) {
          return found_interval(&p);
        }
        // Found a non-empty slot to the left that wasn't a start. We cannot be in an interval.
        return false;
      }
    }
  }

  // We could end up here under two conditions.
  //  1. The lower_bound node contained offset, offset was empty, and we could not find a non-empty
  //  slot to the left. So we have to walk right to try to find an interval end.
  //  2. The lower_bound node contained offsets larger than offset. For this case too we have to
  //  walk right and see if the first populated slot is an interval end.
  // Only the start index for the traversal is different for the two cases.
  size_t index;
  if (lower_bound.offset() == node_offset) {
    index = node_index + 1;
  } else {
    DEBUG_ASSERT(lower_bound.offset() > node_offset);
    index = 0;
  }

  for (; index < VmPageListNode::kPageFanOut; index++) {
    auto& p = lower_bound.Lookup(index);
    // Found the first populated slot to the right.
    if (!p.IsEmpty()) {
      if (p.IsIntervalEnd()) {
        return found_interval(&p);
      }
      // Found a non-empty slot to the right that wasn't an end. We cannot be in an interval.
      return false;
    }
  }

  return false;
}

zx_status_t VmPageList::AddZeroInterval(uint64_t start_offset, uint64_t end_offset,
                                        VmPageOrMarker::IntervalDirtyState dirty_state) {
  DEBUG_ASSERT(IS_PAGE_ALIGNED(start_offset));
  DEBUG_ASSERT(IS_PAGE_ALIGNED(end_offset));
  DEBUG_ASSERT(start_offset < end_offset);
  DEBUG_ASSERT(!AnyPagesOrIntervalsInRange(start_offset, end_offset));

  const uint64_t interval_start = start_offset;
  const uint64_t interval_end = end_offset - PAGE_SIZE;
  const uint64_t prev_offset = interval_start - PAGE_SIZE;
  const uint64_t next_offset = interval_end + PAGE_SIZE;

  // Helper to look up a slot at an offset and return a mutable VmPageOrMarker*. Only finds an
  // existing slot and does not perform any allocations.
  auto lookup_slot = [this](uint64_t offset) -> VmPageOrMarker* {
    const uint64_t node_offset = offset_to_node_offset(offset, list_skew_);
    const size_t index = offset_to_node_index(offset, list_skew_);
    auto pln = list_.find(node_offset);
    if (!pln.IsValid()) {
      return nullptr;
    }
    return &pln->Lookup(index);
  };

  // Check if we can merge this zero interval with a preceding one.
  bool merge_with_prev = false;
  VmPageOrMarker* prev_slot = nullptr;
  if (interval_start > 0) {
    prev_slot = lookup_slot(prev_offset);
    // We can merge to the left if we find a zero interval end or slot, and the dirty state matches.
    if (prev_slot && prev_slot->IsIntervalZero() &&
        (prev_slot->IsIntervalEnd() || prev_slot->IsIntervalSlot()) &&
        prev_slot->GetZeroIntervalDirtyState() == dirty_state) {
      merge_with_prev = true;
    }
  }

  // Check if we can merge this zero interval with a following one.
  bool merge_with_next = false;
  VmPageOrMarker* next_slot = lookup_slot(next_offset);
  // We can merge to the right if we find a zero interval start or slot, and the dirty state
  // matches.
  if (next_slot && next_slot->IsIntervalZero() &&
      (next_slot->IsIntervalStart() || next_slot->IsIntervalSlot()) &&
      next_slot->GetZeroIntervalDirtyState() == dirty_state) {
    merge_with_next = true;
  }

  // First allocate any slots that might be needed to insert the interval.
  VmPageOrMarker* new_start = nullptr;
  VmPageOrMarker* new_end = nullptr;
  // If we could not merge with an interval to the left, we're going to need a new start sentinel.
  if (!merge_with_prev) {
    new_start = LookupOrAllocate(interval_start);
    if (!new_start) {
      return ZX_ERR_NO_MEMORY;
    }
    DEBUG_ASSERT(new_start->IsEmpty());
  }
  // If we could not merge with an interval to the right, we're going to need a new end sentinel.
  if (!merge_with_next) {
    new_end = LookupOrAllocate(interval_end);
    if (!new_end) {
      // Clean up any slot we allocated for new_start before returning.
      if (new_start) {
        DEBUG_ASSERT(new_start->IsEmpty());
        ReturnEmptySlot(interval_start);
      }
      return ZX_ERR_NO_MEMORY;
    }
    DEBUG_ASSERT(new_end->IsEmpty());
  }

  // Now that we've checked for all error conditions perform the actual update.
  if (merge_with_prev) {
    if (prev_slot->IsIntervalEnd()) {
      // If the prev_slot was an interval end, we can simply extend that interval to include the new
      // interval. Free up the old interval end.
      *prev_slot = VmPageOrMarker::Empty();
    } else {
      // If the prev_slot was interval slot, we can extend the interval in that case too. Change the
      // old interval slot into an interval start.
      DEBUG_ASSERT(prev_slot->IsIntervalSlot());
      DEBUG_ASSERT(prev_slot->GetZeroIntervalDirtyState() == dirty_state);
      prev_slot->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Start);
    }
  } else {
    // We could not merge with an interval to the left. Start a new interval.
    DEBUG_ASSERT(new_start->IsEmpty());
    *new_start = VmPageOrMarker::ZeroInterval(VmPageOrMarker::IntervalSentinel::Start, dirty_state);
  }

  if (merge_with_next) {
    if (next_slot->IsIntervalStart()) {
      // If the next_slot was an interval start, we can move back the start to include the new
      // interval. Free up the old start.
      *next_slot = VmPageOrMarker::Empty();
    } else {
      // If the next_slot was an interval slot, we can move back the start in that case too. Change
      // the old interval slot into an interval end.
      DEBUG_ASSERT(next_slot->IsIntervalSlot());
      DEBUG_ASSERT(next_slot->GetZeroIntervalDirtyState() == dirty_state);
      next_slot->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::End);
    }
  } else {
    // We could not merge with an interval to the right. Install an interval end sentinel.
    // If the new zero interval spans a single page, we will already have installed a start above,
    // so change it to a slot sentinel.
    if (new_end->IsIntervalStart()) {
      DEBUG_ASSERT(new_end->GetZeroIntervalDirtyState() == dirty_state);
      new_end->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Slot);
    } else {
      DEBUG_ASSERT(new_end->IsEmpty());
      *new_end = VmPageOrMarker::ZeroInterval(VmPageOrMarker::IntervalSentinel::End, dirty_state);
    }
  }

  // If we ended up removing the prev_slot or next_slot, return the now empty slots.
  bool return_prev_slot = merge_with_prev && prev_slot->IsEmpty();
  bool return_next_slot = merge_with_next && next_slot->IsEmpty();
  if (return_prev_slot) {
    ReturnEmptySlot(prev_offset);
    // next_slot and prev_slot could have come from the same node, in which case we've already
    // freed up the node containing next_slot when returning prev_slot.
    if (return_next_slot && offset_to_node_offset(prev_offset, list_skew_) ==
                                offset_to_node_offset(next_offset, list_skew_)) {
      return_next_slot = false;
    }
  }
  if (return_next_slot) {
    DEBUG_ASSERT(!return_prev_slot || offset_to_node_offset(prev_offset, list_skew_) !=
                                          offset_to_node_offset(next_offset, list_skew_));
    ReturnEmptySlot(next_offset);
  }

  return ZX_OK;
}

zx_status_t VmPageList::ClipIntervalStart(uint64_t interval_start, uint64_t len) {
  DEBUG_ASSERT(IS_PAGE_ALIGNED(interval_start));
  DEBUG_ASSERT(IS_PAGE_ALIGNED(len));
  if (len == 0) {
    return ZX_OK;
  }
  uint64_t new_interval_start;
  ASSERT(!add_overflow(interval_start, len, &new_interval_start));

  const VmPageOrMarker* old_start = Lookup(interval_start);
  DEBUG_ASSERT(old_start->IsIntervalStart());

#if DEBUG_ASSERT_IMPLEMENTED
  // There should only be empty slots between the old and new start.
  zx_status_t status =
      ForEveryPageAndGapInRange([](auto* p, uint64_t off) { return ZX_ERR_BAD_STATE; },
                                [](uint64_t start, uint64_t end) { return ZX_ERR_BAD_STATE; },
                                interval_start + PAGE_SIZE, new_interval_start);
  ASSERT(status == ZX_OK);
#endif

  VmPageOrMarker* new_start = LookupOrAllocate(new_interval_start);
  if (!new_start) {
    return ZX_ERR_NO_MEMORY;
  }

  // Helper to create the new start sentinel using the old start sentinel.
  auto create_new_start = [old_start]() -> VmPageOrMarker {
    // We only support zero intervals for now.
    DEBUG_ASSERT(old_start->IsIntervalZero());
    return VmPageOrMarker::ZeroInterval(VmPageOrMarker::IntervalSentinel::Start,
                                        old_start->GetZeroIntervalDirtyState());
  };

  // It is possible that we are moving the start all the way to the end, leaving behind a single
  // interval slot.
  if (new_start->IsIntervalEnd()) {
    new_start->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Slot);
  } else {
    DEBUG_ASSERT(new_start->IsEmpty());
    *new_start = create_new_start();
  }
  // Free up the old start.
  RemoveContent(interval_start);
  return ZX_OK;
}

zx_status_t VmPageList::ClipIntervalEnd(uint64_t interval_end, uint64_t len) {
  DEBUG_ASSERT(IS_PAGE_ALIGNED(interval_end));
  DEBUG_ASSERT(IS_PAGE_ALIGNED(len));
  if (len == 0) {
    return ZX_OK;
  }
  uint64_t new_interval_end;
  ASSERT(!sub_overflow(interval_end, len, &new_interval_end));

  const VmPageOrMarker* old_end = Lookup(interval_end);
  DEBUG_ASSERT(old_end->IsIntervalEnd());

#if DEBUG_ASSERT_IMPLEMENTED
  // There should only be empty slots between the new and old end.
  zx_status_t status =
      ForEveryPageAndGapInRange([](auto* p, uint64_t off) { return ZX_ERR_BAD_STATE; },
                                [](uint64_t start, uint64_t end) { return ZX_ERR_BAD_STATE; },
                                new_interval_end + PAGE_SIZE, interval_end);
  ASSERT(status == ZX_OK);
#endif

  VmPageOrMarker* new_end = LookupOrAllocate(new_interval_end);
  if (!new_end) {
    return ZX_ERR_NO_MEMORY;
  }

  // Helper to create the new end sentinel using the old end sentinel.
  auto create_new_end = [old_end]() -> VmPageOrMarker {
    // We only support zero intervals for now.
    DEBUG_ASSERT(old_end->IsIntervalZero());
    return VmPageOrMarker::ZeroInterval(VmPageOrMarker::IntervalSentinel::End,
                                        old_end->GetZeroIntervalDirtyState());
  };

  // It is possible that we are moving the end all the way to the start, leaving behind a single
  // interval slot.
  if (new_end->IsIntervalStart()) {
    new_end->ChangeIntervalSentinel(VmPageOrMarker::IntervalSentinel::Slot);
  } else {
    DEBUG_ASSERT(new_end->IsEmpty());
    *new_end = create_new_end();
  }
  // Free up the old end.
  RemoveContent(interval_end);
  return ZX_OK;
}

void VmPageList::MergeFrom(
    VmPageList& other, const uint64_t offset, const uint64_t end_offset,
    fit::inline_function<void(VmPageOrMarker&&, uint64_t offset), 3 * sizeof(void*)> release_fn,
    fit::inline_function<void(VmPageOrMarker*, uint64_t offset)> migrate_fn) {
  constexpr uint64_t kNodeSize = PAGE_SIZE * VmPageListNode::kPageFanOut;
  // The skewed |offset| in |other| must be equal to 0 skewed in |this|. This allows
  // nodes to moved directly between the lists, without having to worry about allocations.
  DEBUG_ASSERT((other.list_skew_ + offset) % kNodeSize == list_skew_);

  // We should not be starting or ending partway inside an interval, either for the source or the
  // target. Note that entire intervals that fall completely inside the range will be checked for
  // later while we're doing the migration.
  DEBUG_ASSERT(!other.IsOffsetInInterval(offset));
  DEBUG_ASSERT(!other.IsOffsetInInterval(end_offset - 1));
  DEBUG_ASSERT(!IsOffsetInInterval(end_offset - offset - 1));

  auto release_fn_wrapper = [&release_fn](VmPageOrMarker* page_or_marker, uint64_t offset) {
    if (!page_or_marker->IsEmpty()) {
      DEBUG_ASSERT(!page_or_marker->IsInterval());
      release_fn(ktl::move(*page_or_marker), offset);
    }
    return ZX_ERR_NEXT;
  };

  // Free pages outside of [|offset|, |end_offset|) so that the later code
  // doesn't have to worry about dealing with this.
  if (offset) {
    other.RemovePages(release_fn_wrapper, 0, offset);
  }
  other.RemovePages(release_fn_wrapper, end_offset, MAX_SIZE);

  // Calculate how much we need to shift nodes so that the node in |other| which contains
  // |offset| gets mapped to offset 0 in |this|.
  const uint64_t node_shift = offset_to_node_offset(offset, other.list_skew_);

  auto other_iter = other.list_.lower_bound(node_shift);
  while (other_iter.IsValid()) {
    DEBUG_ASSERT(other_iter->HasNoIntervalSentinel());
    uint64_t other_offset = other_iter->GetKey();
    // Any such nodes should have already been freed.
    DEBUG_ASSERT(other_offset < (end_offset + other.list_skew_));

    auto cur = other_iter++;
    auto other_node = other.list_.erase(cur);
    other_node->set_offset(other_offset - node_shift);

    auto target = list_.find(other_offset - node_shift);
    if (target.IsValid()) {
      DEBUG_ASSERT(target->HasNoIntervalSentinel());
      // If there's already a node at the desired location, then merge the two nodes.
      for (unsigned i = 0; i < VmPageListNode::kPageFanOut; i++) {
        uint64_t src_offset = other_offset - other.list_skew_ + i * PAGE_SIZE;
        VmPageOrMarker page = ktl::move(other_node->Lookup(i));
        VmPageOrMarker& target_page = target->Lookup(i);
        if (target_page.IsEmpty()) {
          if (page.IsPageOrRef()) {
            migrate_fn(&page, src_offset);
          }
          target_page = ktl::move(page);
        } else if (!page.IsEmpty()) {
          release_fn(ktl::move(page), src_offset);
        }

        // In all cases if we still have a page add it to the free list.
        DEBUG_ASSERT(!page.IsPageOrRef());
      }
    } else {
      // If there's no node at the desired location, then directly insert the node.
      list_.insert_or_find(ktl::move(other_node), &target);
      bool has_page = false;
      for (unsigned i = 0; i < VmPageListNode::kPageFanOut; i++) {
        VmPageOrMarker& page = target->Lookup(i);
        if (page.IsPageOrRef()) {
          migrate_fn(&page, other_offset - other.list_skew_ + i * PAGE_SIZE);
          if (page.IsPageOrRef()) {
            has_page = true;
          }
        } else if (page.IsMarker()) {
          has_page = true;
        }
      }
      if (!has_page) {
        list_.erase(target);
      }
    }
  }
}

void VmPageList::MergeOnto(VmPageList& other,
                           fit::inline_function<void(VmPageOrMarker&&)> release_fn) {
  DEBUG_ASSERT(other.list_skew_ == list_skew_);

  auto iter = list_.begin();
  while (iter.IsValid()) {
    DEBUG_ASSERT(iter->HasNoIntervalSentinel());
    auto node = list_.erase(iter++);
    auto target = other.list_.find(node->GetKey());
    if (target.IsValid()) {
      DEBUG_ASSERT(target->HasNoIntervalSentinel());
      // If there's already a node at the desired location, then merge the two nodes.
      for (unsigned i = 0; i < VmPageListNode::kPageFanOut; i++) {
        VmPageOrMarker page = ktl::move(node->Lookup(i));
        if (page.IsEmpty()) {
          continue;
        }
        VmPageOrMarker& old_page = target->Lookup(i);
        VmPageOrMarker removed = ktl::move(old_page);
        old_page = ktl::move(page);
        if (!removed.IsEmpty()) {
          release_fn(ktl::move(removed));
        }
      }
    } else {
      other.list_.insert(ktl::move(node));
    }
  }
}

VmPageSpliceList VmPageList::TakePages(uint64_t offset, uint64_t length) {
  VmPageSpliceList res(offset, length);
  const uint64_t end = offset + length;

  // Taking pages from children isn't supported, so list_skew_ should be 0.
  DEBUG_ASSERT(list_skew_ == 0);

  // We should not be starting or ending partway inside an interval. Entire intervals that fall
  // completely inside the range will be checked for later while we're taking the pages below.
  DEBUG_ASSERT(!IsOffsetInInterval(offset));
  DEBUG_ASSERT(!IsOffsetInInterval(end - 1));

  // If we can't take the whole node at the start of the range,
  // the shove the pages into the splice list head_ node.
  while (offset_to_node_index(offset, 0) != 0 && offset < end) {
    res.head_.Lookup(offset_to_node_index(offset, 0)) = RemoveContent(offset);
    offset += PAGE_SIZE;
  }
  DEBUG_ASSERT(res.head_.HasNoIntervalSentinel());

  // As long as the current and end node offsets are different, we
  // can just move the whole node into the splice list.
  while (offset_to_node_offset(offset, 0) != offset_to_node_offset(end, 0)) {
    ktl::unique_ptr<VmPageListNode> node = list_.erase(offset_to_node_offset(offset, 0));
    if (node) {
      DEBUG_ASSERT(node->HasNoIntervalSentinel());
      res.middle_.insert(ktl::move(node));
    }
    offset += (PAGE_SIZE * VmPageListNode::kPageFanOut);
  }

  // Move any remaining pages into the splice list tail_ node.
  while (offset < end) {
    res.tail_.Lookup(offset_to_node_index(offset, 0)) = RemoveContent(offset);
    offset += PAGE_SIZE;
  }
  DEBUG_ASSERT(res.tail_.HasNoIntervalSentinel());

  return res;
}

VmPageSpliceList::VmPageSpliceList() : VmPageSpliceList(0, 0) {}

VmPageSpliceList::VmPageSpliceList(uint64_t offset, uint64_t length)
    : offset_(offset), length_(length) {}

VmPageSpliceList::VmPageSpliceList(VmPageSpliceList&& other)
    : offset_(other.offset_),
      length_(other.length_),
      pos_(other.pos_),
      middle_(ktl::move(other.middle_)) {
  move_vm_page_list_node(&head_, &other.head_);
  move_vm_page_list_node(&tail_, &other.tail_);

  other.offset_ = other.length_ = other.pos_ = 0;
}

VmPageSpliceList& VmPageSpliceList::operator=(VmPageSpliceList&& other) {
  FreeAllPages();

  offset_ = other.offset_;
  length_ = other.length_;
  pos_ = other.pos_;
  move_vm_page_list_node(&head_, &other.head_);
  move_vm_page_list_node(&tail_, &other.tail_);
  middle_ = ktl::move(other.middle_);

  other.offset_ = other.length_ = other.pos_ = 0;

  return *this;
}

VmPageSpliceList::~VmPageSpliceList() { FreeAllPages(); }

// static
VmPageSpliceList VmPageSpliceList::CreateFromPageList(uint64_t offset, uint64_t length,
                                                      list_node* pages) {
  // TODO(fxbug.dev/88859): This method needs coverage in vmpl_unittests.
  DEBUG_ASSERT(pages);
  DEBUG_ASSERT(list_length(pages) == length / PAGE_SIZE);
  VmPageSpliceList res(offset, length);
  DEBUG_ASSERT(list_is_empty(&res.raw_pages_));
  list_move(pages, &res.raw_pages_);
  return res;
}

void VmPageSpliceList::FreeAllPages() {
  // Free any pages owned by the splice list.
  while (!IsDone()) {
    VmPageOrMarker page = Pop();
    if (page.IsPage()) {
      pmm_free_page(page.ReleasePage());
    } else if (page.IsReference()) {
      auto compression = pmm_page_compression();
      DEBUG_ASSERT(compression);
      compression->Free(page.ReleaseReference());
    }
  }
}

VmPageOrMarker VmPageSpliceList::Pop() {
  if (IsDone()) {
    DEBUG_ASSERT_MSG(false, "Popped from empty splice list");
    return VmPageOrMarker::Empty();
  }

  VmPageOrMarker res;
  if (!list_is_empty(&raw_pages_)) {
    // TODO(fxbug.dev/88859): This path and CreateFromPageList() need coverage in vmpl_unittests.
    vm_page_t* head = list_remove_head_type(&raw_pages_, vm_page, queue_node);
    res = VmPageOrMarker::Page(head);
  } else {
    const uint64_t cur_offset = offset_ + pos_;
    const auto cur_node_idx = offset_to_node_index(cur_offset, 0);
    const auto cur_node_offset = offset_to_node_offset(cur_offset, 0);

    if (offset_to_node_index(offset_, 0) != 0 &&
        offset_to_node_offset(offset_, 0) == cur_node_offset) {
      // If the original offset means that pages were placed in head_
      // and the current offset points to the same node, look there.
      res = ktl::move(head_.Lookup(cur_node_idx));
    } else if (cur_node_offset != offset_to_node_offset(offset_ + length_, 0)) {
      // If the current offset isn't pointing to the tail node,
      // look in the middle tree.
      auto middle_node = middle_.find(cur_node_offset);
      if (middle_node.IsValid()) {
        res = ktl::move(middle_node->Lookup(cur_node_idx));
      }
    } else {
      // If none of the other cases, we're in the tail_.
      res = ktl::move(tail_.Lookup(cur_node_idx));
    }
  }
  DEBUG_ASSERT(!res.IsInterval());

  pos_ += PAGE_SIZE;
  return res;
}
