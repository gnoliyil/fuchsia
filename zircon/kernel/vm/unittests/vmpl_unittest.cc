// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "test_helper.h"

namespace vm_unittest {

namespace {
bool AddPage(VmPageList* pl, vm_page_t* page, uint64_t offset) {
  if (!pl) {
    return false;
  }
  auto [slot, is_interval] = pl->LookupOrAllocateCheckForInterval(offset, true);
  if (!slot) {
    return false;
  }
  if (!slot->IsEmpty() && !slot->IsIntervalSlot()) {
    return false;
  }
  ASSERT(slot->IsEmpty() || is_interval);
  *slot = VmPageOrMarker::Page(page);
  return true;
}

bool AddMarker(VmPageList* pl, uint64_t offset) {
  if (!pl) {
    return false;
  }
  auto [slot, is_interval] = pl->LookupOrAllocateCheckForInterval(offset, true);
  if (!slot) {
    return false;
  }
  if (!slot->IsEmpty() && !slot->IsIntervalSlot()) {
    return false;
  }
  ASSERT(slot->IsEmpty() || is_interval);
  *slot = VmPageOrMarker::Marker();
  return true;
}

bool AddReference(VmPageList* pl, VmPageOrMarker::ReferenceValue ref, uint64_t offset,
                  bool left_split, bool right_split) {
  if (!pl) {
    return false;
  }
  auto [slot, is_interval] = pl->LookupOrAllocateCheckForInterval(offset, true);
  if (!slot) {
    return false;
  }
  if (!slot->IsEmpty() && !slot->IsIntervalSlot()) {
    return false;
  }
  ASSERT(slot->IsEmpty() || is_interval);
  *slot = VmPageOrMarker::Reference(ref, left_split, right_split);
  return true;
}

constexpr uint64_t TestReference(uint64_t v) {
  return v << VmPageOrMarker::ReferenceValue::kAlignBits;
}

}  // namespace

// Basic test that checks adding/removing a page
static bool vmpl_add_remove_page_test() {
  BEGIN_TEST;

  VmPageList pl;
  vm_page_t test_page{};

  EXPECT_TRUE(AddPage(&pl, &test_page, 0));

  EXPECT_EQ(&test_page, pl.Lookup(0)->Page(), "unexpected page\n");
  EXPECT_FALSE(pl.IsEmpty());
  EXPECT_FALSE(pl.HasNoPageOrRef());

  vm_page* remove_page = pl.RemoveContent(0).ReleasePage();
  EXPECT_EQ(&test_page, remove_page, "unexpected page\n");
  EXPECT_TRUE(pl.RemoveContent(0).IsEmpty(), "unexpected page\n");

  EXPECT_TRUE(pl.IsEmpty());
  EXPECT_TRUE(pl.HasNoPageOrRef());

  END_TEST;
}

// Basic test of setting and getting markers.
static bool vmpl_basic_marker_test() {
  BEGIN_TEST;

  VmPageList pl;

  EXPECT_TRUE(pl.IsEmpty());
  EXPECT_TRUE(pl.HasNoPageOrRef());

  EXPECT_TRUE(AddMarker(&pl, 0));

  EXPECT_TRUE(pl.Lookup(0)->IsMarker());

  EXPECT_FALSE(pl.IsEmpty());
  EXPECT_TRUE(pl.HasNoPageOrRef());

  VmPageOrMarker removed = pl.RemoveContent(0);
  EXPECT_TRUE(removed.IsMarker());

  EXPECT_TRUE(pl.HasNoPageOrRef());
  EXPECT_TRUE(pl.IsEmpty());

  END_TEST;
}

static bool vmpl_basic_reference_test() {
  BEGIN_TEST;

  VmPageList pl;

  EXPECT_TRUE(pl.IsEmpty());
  EXPECT_TRUE(pl.HasNoPageOrRef());

  // The zero ref is valid.
  const VmPageOrMarker::ReferenceValue ref0(0);
  EXPECT_TRUE(AddReference(&pl, ref0, 0, false, false));

  EXPECT_FALSE(pl.IsEmpty());
  EXPECT_FALSE(pl.HasNoPageOrRef());

  // A non-zero ref.
  const VmPageOrMarker::ReferenceValue ref1(TestReference(1));
  EXPECT_TRUE(AddReference(&pl, ref1, PAGE_SIZE, false, false));

  VmPageOrMarker removed = pl.RemoveContent(0);
  EXPECT_EQ(removed.ReleaseReference().value(), ref0.value());

  EXPECT_FALSE(pl.IsEmpty());
  EXPECT_FALSE(pl.HasNoPageOrRef());

  removed = pl.RemoveContent(PAGE_SIZE);
  EXPECT_EQ(removed.ReleaseReference().value(), ref1.value());

  EXPECT_TRUE(pl.IsEmpty());
  EXPECT_TRUE(pl.HasNoPageOrRef());

  END_TEST;
}

static bool vmpl_content_split_bits_test() {
  BEGIN_TEST;

  VmPageList pl;

  vm_page_t test_page;
  test_page.object.cow_left_split = 0;
  test_page.object.cow_right_split = 0;

  const VmPageOrMarker::ReferenceValue test_ref(TestReference(1));

  EXPECT_TRUE(AddPage(&pl, &test_page, 0));
  EXPECT_TRUE(AddReference(&pl, test_ref, PAGE_SIZE, false, false));

  VmPageOrMarkerRef page_entry = pl.LookupMutable(0);
  VmPageOrMarkerRef ref_entry = pl.LookupMutable(PAGE_SIZE);
  EXPECT_FALSE(page_entry->PageOrRefLeftSplit());
  EXPECT_FALSE(page_entry->PageOrRefRightSplit());
  EXPECT_FALSE(ref_entry->PageOrRefLeftSplit());
  EXPECT_FALSE(ref_entry->PageOrRefRightSplit());

  test_page.object.cow_left_split = 1;
  EXPECT_TRUE(page_entry->PageOrRefLeftSplit());
  EXPECT_FALSE(page_entry->PageOrRefRightSplit());

  page_entry.SetPageOrRefLeftSplit(false);
  EXPECT_EQ(test_page.object.cow_left_split, 0);
  EXPECT_FALSE(page_entry->PageOrRefLeftSplit());
  EXPECT_FALSE(page_entry->PageOrRefRightSplit());

  ref_entry.SetPageOrRefRightSplit(true);
  EXPECT_FALSE(ref_entry->PageOrRefLeftSplit());
  EXPECT_TRUE(ref_entry->PageOrRefRightSplit());

  // Remove and re-add the ref with different initial splits.
  EXPECT_TRUE(
      AddReference(&pl, pl.RemoveContent(PAGE_SIZE).ReleaseReference(), PAGE_SIZE, true, false));
  EXPECT_TRUE(ref_entry->PageOrRefLeftSplit());
  EXPECT_FALSE(ref_entry->PageOrRefRightSplit());

  EXPECT_EQ(pl.RemoveContent(0).ReleasePage(), &test_page);
  EXPECT_EQ(pl.RemoveContent(PAGE_SIZE).ReleaseReference().value(), test_ref.value());

  END_TEST;
}

static bool vmpl_replace_preserves_split_bits() {
  BEGIN_TEST;

  VmPageList pl;

  vm_page_t test_page;
  test_page.object.cow_left_split = 0;
  test_page.object.cow_right_split = 0;

  const VmPageOrMarker::ReferenceValue test_ref(TestReference(1));

  EXPECT_TRUE(AddPage(&pl, &test_page, 0));
  VmPageOrMarkerRef entry = pl.LookupMutable(0);
  test_page.object.cow_left_split = 1;

  EXPECT_EQ(entry.SwapPageForReference(test_ref), &test_page);

  EXPECT_TRUE(entry->PageOrRefLeftSplit());
  EXPECT_FALSE(entry->PageOrRefRightSplit());
  test_page.object.cow_left_split = 0;
  EXPECT_TRUE(entry->PageOrRefLeftSplit());
  EXPECT_FALSE(entry->PageOrRefRightSplit());

  entry.SetPageOrRefRightSplit(true);
  test_page.object.cow_left_split = 1;
  test_page.object.cow_right_split = 1;

  EXPECT_EQ(entry.SwapReferenceForPage(&test_page).value(), test_ref.value());
  EXPECT_TRUE(entry->PageOrRefLeftSplit());
  EXPECT_TRUE(entry->PageOrRefRightSplit());

  EXPECT_EQ(pl.RemoveContent(0).ReleasePage(), &test_page);

  END_TEST;
}

// Test for freeing a range of pages
static bool vmpl_free_pages_test() {
  BEGIN_TEST;

  VmPageList pl;
  constexpr uint32_t kCount = 3 * VmPageListNode::kPageFanOut;
  vm_page_t test_pages[kCount] = {};

  // Install alternating pages and markers.
  for (uint32_t i = 0; i < kCount; i++) {
    EXPECT_TRUE(AddPage(&pl, test_pages + i, i * 2 * PAGE_SIZE));
    EXPECT_TRUE(AddMarker(&pl, (i * 2 + 1) * PAGE_SIZE));
  }

  list_node_t list;
  list_initialize(&list);
  pl.RemovePages(
      [&list](VmPageOrMarker* page_or_marker, uint64_t off) {
        if (page_or_marker->IsPage()) {
          vm_page_t* p = page_or_marker->ReleasePage();
          list_add_tail(&list, &p->queue_node);
        }
        *page_or_marker = VmPageOrMarker::Empty();
        return ZX_ERR_NEXT;
      },
      PAGE_SIZE * 2, (kCount - 1) * 2 * PAGE_SIZE);
  for (unsigned i = 1; i < kCount - 2; i++) {
    EXPECT_TRUE(list_in_list(&test_pages[i].queue_node), "Not in free list");
  }

  for (uint32_t i = 0; i < kCount; i++) {
    VmPageOrMarker remove_page = pl.RemoveContent(i * 2 * PAGE_SIZE);
    VmPageOrMarker remove_marker = pl.RemoveContent((i * 2 + 1) * PAGE_SIZE);
    if (i == 0 || i == kCount - 1) {
      EXPECT_TRUE(remove_page.IsPage(), "missing page\n");
      EXPECT_TRUE(remove_marker.IsMarker(), "missing marker\n");
      EXPECT_EQ(test_pages + i, remove_page.ReleasePage(), "unexpected page\n");
    } else {
      EXPECT_TRUE(remove_page.IsEmpty(), "extra page\n");
      EXPECT_TRUE(remove_marker.IsEmpty(), "extra marker\n");
    }
  }

  END_TEST;
}

// Tests freeing the last page in a list
static bool vmpl_free_pages_last_page_test() {
  BEGIN_TEST;

  vm_page_t page{};

  VmPageList pl;
  EXPECT_TRUE(AddPage(&pl, &page, 0));

  EXPECT_EQ(&page, pl.Lookup(0)->Page(), "unexpected page\n");

  list_node_t list;
  list_initialize(&list);
  pl.RemoveAllContent(
      [&list](VmPageOrMarker&& p) { list_add_tail(&list, &p.ReleasePage()->queue_node); });
  EXPECT_TRUE(pl.IsEmpty(), "not empty\n");

  EXPECT_EQ(list_length(&list), 1u, "too many pages");
  EXPECT_EQ(list_remove_head_type(&list, vm_page_t, queue_node), &page, "wrong page");

  END_TEST;
}

static bool vmpl_near_last_offset_free() {
  BEGIN_TEST;

  vm_page_t page = {};

  bool at_least_one = false;
  for (uint64_t addr = 0xfffffffffff00000; addr != 0; addr += PAGE_SIZE) {
    VmPageList pl;
    if (AddPage(&pl, &page, addr)) {
      at_least_one = true;
      EXPECT_EQ(&page, pl.Lookup(addr)->Page(), "unexpected page\n");

      list_node_t list;
      list_initialize(&list);
      pl.RemoveAllContent(
          [&list](VmPageOrMarker&& p) { list_add_tail(&list, &p.ReleasePage()->queue_node); });

      EXPECT_EQ(list_length(&list), 1u, "too many pages");
      EXPECT_EQ(list_remove_head_type(&list, vm_page_t, queue_node), &page, "wrong page");
      EXPECT_TRUE(pl.IsEmpty(), "non-empty list\n");
    }
  }
  EXPECT_TRUE(at_least_one, "starting address too large");

  VmPageList pl2;
  EXPECT_NULL(pl2.LookupOrAllocate(0xfffffffffffe0000), "unexpected offset addable\n");

  END_TEST;
}

// Tests taking a page from the start of a VmPageListNode
static bool vmpl_take_single_page_even_test() {
  BEGIN_TEST;

  VmPageList pl;
  vm_page_t test_page{};
  vm_page_t test_page2{};
  EXPECT_TRUE(AddPage(&pl, &test_page, 0));
  EXPECT_TRUE(AddPage(&pl, &test_page2, PAGE_SIZE));

  VmPageSpliceList splice = pl.TakePages(0, PAGE_SIZE);

  EXPECT_EQ(&test_page, splice.Pop().ReleasePage(), "wrong page\n");
  EXPECT_TRUE(splice.IsDone(), "extra page\n");
  EXPECT_TRUE(pl.Lookup(0) == nullptr || pl.Lookup(0)->IsEmpty(), "duplicate page\n");

  EXPECT_EQ(&test_page2, pl.RemoveContent(PAGE_SIZE).ReleasePage(), "remove failure\n");

  END_TEST;
}

// Tests taking a page from the middle of a VmPageListNode
static bool vmpl_take_single_page_odd_test() {
  BEGIN_TEST;

  VmPageList pl;
  vm_page_t test_page{};
  vm_page_t test_page2{};
  EXPECT_TRUE(AddPage(&pl, &test_page, 0));
  EXPECT_TRUE(AddPage(&pl, &test_page2, PAGE_SIZE));

  VmPageSpliceList splice = pl.TakePages(PAGE_SIZE, PAGE_SIZE);

  EXPECT_EQ(&test_page2, splice.Pop().ReleasePage(), "wrong page\n");
  EXPECT_TRUE(splice.IsDone(), "extra page\n");
  EXPECT_TRUE(pl.Lookup(PAGE_SIZE) == nullptr || pl.Lookup(PAGE_SIZE)->IsEmpty(),
              "duplicate page\n");

  EXPECT_EQ(&test_page, pl.RemoveContent(0).ReleasePage(), "remove failure\n");

  END_TEST;
}

// Tests taking all the pages from a range of VmPageListNodes
static bool vmpl_take_all_pages_test() {
  BEGIN_TEST;

  VmPageList pl;
  constexpr uint32_t kCount = 3 * VmPageListNode::kPageFanOut;
  vm_page_t test_pages[kCount] = {};
  for (uint32_t i = 0; i < kCount; i++) {
    EXPECT_TRUE(AddPage(&pl, test_pages + i, i * 2 * PAGE_SIZE));
    EXPECT_TRUE(AddMarker(&pl, (i * 2 + 1) * PAGE_SIZE));
  }

  VmPageSpliceList splice = pl.TakePages(0, kCount * 2 * PAGE_SIZE);
  EXPECT_TRUE(pl.IsEmpty(), "non-empty list\n");

  for (uint32_t i = 0; i < kCount; i++) {
    EXPECT_EQ(test_pages + i, splice.Pop().ReleasePage(), "wrong page\n");
    EXPECT_TRUE(splice.Pop().IsMarker(), "expected marker\n");
  }
  EXPECT_TRUE(splice.IsDone(), "extra pages\n");

  END_TEST;
}

// Tests taking the middle pages from a range of VmPageListNodes
static bool vmpl_take_middle_pages_test() {
  BEGIN_TEST;

  VmPageList pl;
  constexpr uint32_t kCount = 3 * VmPageListNode::kPageFanOut;
  vm_page_t test_pages[kCount] = {};
  for (uint32_t i = 0; i < kCount; i++) {
    EXPECT_TRUE(AddPage(&pl, test_pages + i, i * PAGE_SIZE));
  }

  constexpr uint32_t kTakeOffset = VmPageListNode::kPageFanOut - 1;
  constexpr uint32_t kTakeCount = VmPageListNode::kPageFanOut + 2;
  VmPageSpliceList splice = pl.TakePages(kTakeOffset * PAGE_SIZE, kTakeCount * PAGE_SIZE);
  EXPECT_FALSE(pl.IsEmpty(), "non-empty list\n");

  for (uint32_t i = 0; i < kCount; i++) {
    if (kTakeOffset <= i && i < kTakeOffset + kTakeCount) {
      EXPECT_EQ(test_pages + i, splice.Pop().ReleasePage(), "wrong page\n");
    } else {
      EXPECT_EQ(test_pages + i, pl.RemoveContent(i * PAGE_SIZE).ReleasePage(), "remove failure\n");
    }
  }
  EXPECT_TRUE(splice.IsDone(), "extra pages\n");

  END_TEST;
}

// Tests that gaps are preserved in the list
static bool vmpl_take_gap_test() {
  BEGIN_TEST;

  VmPageList pl;
  constexpr uint32_t kCount = VmPageListNode::kPageFanOut;
  constexpr uint32_t kGapSize = 2;
  vm_page_t test_pages[kCount] = {};
  for (uint32_t i = 0; i < kCount; i++) {
    uint64_t offset = (i * (kGapSize + 1)) * PAGE_SIZE;
    EXPECT_TRUE(AddPage(&pl, test_pages + i, offset));
  }

  constexpr uint32_t kListStart = PAGE_SIZE;
  constexpr uint32_t kListLen = (kCount * (kGapSize + 1) - 2) * PAGE_SIZE;
  VmPageSpliceList splice = pl.TakePages(kListStart, kListLen);

  EXPECT_EQ(test_pages, pl.RemoveContent(0).ReleasePage(), "wrong page\n");
  EXPECT_TRUE(pl.Lookup(kListLen) == nullptr || pl.Lookup(kListLen)->IsEmpty(), "wrong page\n");

  for (uint64_t offset = kListStart; offset < kListStart + kListLen; offset += PAGE_SIZE) {
    auto page_idx = offset / PAGE_SIZE;
    if (page_idx % (kGapSize + 1) == 0) {
      EXPECT_EQ(test_pages + (page_idx / (kGapSize + 1)), splice.Pop().ReleasePage(),
                "wrong page\n");
    } else {
      EXPECT_TRUE(splice.Pop().IsEmpty(), "wrong page\n");
    }
  }
  EXPECT_TRUE(splice.IsDone(), "extra pages\n");

  END_TEST;
}

// Tests that an empty page splice list can be created.
static bool vmpl_take_empty_test() {
  BEGIN_TEST;

  VmPageList pl;

  VmPageSpliceList splice = pl.TakePages(PAGE_SIZE, PAGE_SIZE);

  EXPECT_FALSE(splice.IsDone());
  EXPECT_TRUE(splice.Pop().IsEmpty());
  EXPECT_TRUE(splice.IsDone());

  END_TEST;
}

// Tests that cleaning up a splice list doesn't blow up
static bool vmpl_take_cleanup_test() {
  BEGIN_TEST;

  paddr_t pa;
  vm_page_t* page;

  zx_status_t status = pmm_alloc_page(0, &page, &pa);
  ASSERT_EQ(ZX_OK, status, "pmm_alloc single page");
  ASSERT_NONNULL(page, "pmm_alloc single page");
  ASSERT_NE(0u, pa, "pmm_alloc single page");

  page->set_state(vm_page_state::OBJECT);
  page->object.pin_count = 0;

  VmPageList pl;
  EXPECT_TRUE(AddPage(&pl, page, 0));

  VmPageSpliceList splice = pl.TakePages(0, PAGE_SIZE);
  EXPECT_TRUE(!splice.IsDone(), "missing page\n");

  END_TEST;
}

// Helper function which takes an array of pages, builds a VmPageList, and then verifies that
// ForEveryPageInRange is correct when ZX_ERR_NEXT is returned for the |stop_idx|th entry.
static bool vmpl_page_gap_iter_test_body(vm_page_t** pages, uint32_t count, uint32_t stop_idx) {
  BEGIN_TEST;

  VmPageList list;
  for (uint32_t i = 0; i < count; i++) {
    if (pages[i]) {
      EXPECT_TRUE(AddPage(&list, pages[i], i * PAGE_SIZE));
    }
  }

  uint32_t idx = 0;
  zx_status_t s = list.ForEveryPageAndGapInRange(
      [pages, stop_idx, &idx](const VmPageOrMarker* p, auto off) {
        if (off != idx * PAGE_SIZE || !p->IsPage() || pages[idx] != p->Page()) {
          return ZX_ERR_INTERNAL;
        }
        if (idx == stop_idx) {
          return ZX_ERR_STOP;
        }
        idx++;
        return ZX_ERR_NEXT;
      },
      [pages, stop_idx, &idx](uint64_t gap_start, uint64_t gap_end) {
        for (auto o = gap_start; o < gap_end; o += PAGE_SIZE) {
          if (o != idx * PAGE_SIZE || pages[idx] != nullptr) {
            return ZX_ERR_INTERNAL;
          }
          if (idx == stop_idx) {
            return ZX_ERR_STOP;
          }
          idx++;
        }
        return ZX_ERR_NEXT;
      },
      0, count * PAGE_SIZE);
  ASSERT_EQ(ZX_OK, s);
  ASSERT_EQ(stop_idx, idx);

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  ASSERT_TRUE(list.IsEmpty());

  END_TEST;
}

// Test ForEveryPageInRange against all lists of size 4
static bool vmpl_page_gap_iter_test() {
  static constexpr uint32_t kCount = 4;
  static_assert((kCount & (kCount - 1)) == 0);

  vm_page_t pages[kCount] = {};
  vm_page_t* list[kCount] = {};
  for (unsigned i = 0; i < kCount; i++) {
    for (unsigned j = 0; j < (1 << kCount); j++) {
      for (unsigned k = 0; k < kCount; k++) {
        if (j & (1 << k)) {
          // Ensure pages are ready to be added to a list in every iteration.
          list_initialize(&pages[k].queue_node);
          list[k] = pages + k;
        } else {
          list[k] = nullptr;
        }
      }

      if (!vmpl_page_gap_iter_test_body(list, kCount, i)) {
        return false;
      }
    }
  }
  return true;
}

static bool vmpl_merge_offset_test_helper(uint64_t list1_offset, uint64_t list2_offset) {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, list1_offset);
  vm_page_t test_pages[6] = {};
  uint64_t offsets[6] = {
      VmPageListNode::kPageFanOut * PAGE_SIZE + list2_offset - PAGE_SIZE,
      VmPageListNode::kPageFanOut * PAGE_SIZE + list2_offset,
      3 * VmPageListNode::kPageFanOut * PAGE_SIZE + list2_offset - PAGE_SIZE,
      3 * VmPageListNode::kPageFanOut * PAGE_SIZE + list2_offset,
      5 * VmPageListNode::kPageFanOut * PAGE_SIZE + list2_offset - PAGE_SIZE,
      5 * VmPageListNode::kPageFanOut * PAGE_SIZE + list2_offset,
  };

  for (unsigned i = 0; i < 6; i++) {
    EXPECT_TRUE(AddPage(&list, test_pages + i, offsets[i]));
  }

  VmPageList list2;
  list2.InitializeSkew(list1_offset, list2_offset);

  list_node_t free_list;
  list_initialize(&free_list);
  list2.MergeFrom(
      list, offsets[1], offsets[5],
      [&](VmPageOrMarker&& p, uint64_t offset) {
        if (p.IsMarker()) {
          return;
        }
        vm_page_t* page = p.ReleasePage();
        DEBUG_ASSERT(page == test_pages || page == test_pages + 5);
        DEBUG_ASSERT(offset == offsets[0] || offset == offsets[5]);
        list_add_tail(&free_list, &page->queue_node);
      },
      [&](VmPageOrMarker* page_or_marker, uint64_t offset) {
        DEBUG_ASSERT(page_or_marker->IsPage());
        vm_page_t* page = page_or_marker->Page();
        DEBUG_ASSERT(page == test_pages + 1 || page == test_pages + 2 || page == test_pages + 3 ||
                     page == test_pages + 4);
        DEBUG_ASSERT(offset == offsets[1] || offset == offsets[2] || offset == offsets[3] ||
                     offsets[4]);
      });

  EXPECT_EQ(list_length(&free_list), 2ul);

  EXPECT_EQ(list2.RemoveContent(0).ReleasePage(), test_pages + 1);
  EXPECT_EQ(
      list2.RemoveContent(2 * VmPageListNode::kPageFanOut * PAGE_SIZE - PAGE_SIZE).ReleasePage(),
      test_pages + 2);
  EXPECT_EQ(list2.RemoveContent(2 * VmPageListNode::kPageFanOut * PAGE_SIZE).ReleasePage(),
            test_pages + 3);
  EXPECT_EQ(
      list2.RemoveContent(4 * VmPageListNode::kPageFanOut * PAGE_SIZE - PAGE_SIZE).ReleasePage(),
      test_pages + 4);

  EXPECT_TRUE(list2.HasNoPageOrRef());

  END_TEST;
}

static bool vmpl_merge_offset_test() {
  for (unsigned i = 0; i < VmPageListNode::kPageFanOut; i++) {
    for (unsigned j = 0; j < VmPageListNode::kPageFanOut; j++) {
      if (!vmpl_merge_offset_test_helper(i * PAGE_SIZE, j * PAGE_SIZE)) {
        return false;
      }
    }
  }
  return true;
}

static bool vmpl_merge_overlap_test_helper(uint64_t list1_offset, uint64_t list2_offset) {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, list1_offset);
  vm_page_t test_pages[4] = {};

  EXPECT_TRUE(AddPage(&list, test_pages, list2_offset));
  EXPECT_TRUE(AddPage(&list, test_pages + 1, list2_offset + 2 * PAGE_SIZE));

  VmPageList list2;
  list2.InitializeSkew(list1_offset, list2_offset);

  EXPECT_TRUE(AddPage(&list2, test_pages + 2, 0));
  EXPECT_TRUE(AddPage(&list2, test_pages + 3, PAGE_SIZE));

  list_node_t free_list;
  list_initialize(&free_list);
  list2.MergeFrom(
      list, list2_offset, list2_offset + 4 * PAGE_SIZE,
      [&](VmPageOrMarker&& p, uint64_t offset) {
        if (p.IsMarker()) {
          return;
        }
        vm_page_t* page = p.ReleasePage();
        DEBUG_ASSERT(page == test_pages);
        DEBUG_ASSERT(offset == list2_offset);
        list_add_tail(&free_list, &page->queue_node);
      },
      [&](VmPageOrMarker* page_or_marker, uint64_t offset) {
        DEBUG_ASSERT(page_or_marker->IsPage());
        vm_page_t* page = page_or_marker->Page();
        DEBUG_ASSERT(page == test_pages + 1);
        DEBUG_ASSERT(offset == list2_offset + 2 * PAGE_SIZE);
      });

  EXPECT_EQ(list_length(&free_list), 1ul);

  EXPECT_EQ(list2.RemoveContent(0).ReleasePage(), test_pages + 2);
  EXPECT_EQ(list2.RemoveContent(PAGE_SIZE).ReleasePage(), test_pages + 3);
  EXPECT_EQ(list2.RemoveContent(2 * PAGE_SIZE).ReleasePage(), test_pages + 1);

  EXPECT_TRUE(list2.IsEmpty());

  END_TEST;
}

static bool vmpl_merge_overlap_test() {
  for (unsigned i = 0; i < VmPageListNode::kPageFanOut; i++) {
    for (unsigned j = 0; j < VmPageListNode::kPageFanOut; j++) {
      if (!vmpl_merge_overlap_test_helper(i * PAGE_SIZE, j * PAGE_SIZE)) {
        return false;
      }
    }
  }
  return true;
}

static bool vmpl_merge_marker_test() {
  BEGIN_TEST;

  VmPageList list1;
  VmPageList list2;

  // Put markers in our from list and one of marker, page and nothing in our destination list.
  // Should see the markers given in the release calls, but no attempts to migrate.
  EXPECT_TRUE(AddMarker(&list1, 0));
  EXPECT_TRUE(AddMarker(&list1, PAGE_SIZE));
  EXPECT_TRUE(AddMarker(&list1, PAGE_SIZE * 2));
  EXPECT_TRUE(AddMarker(&list2, PAGE_SIZE));
  vm_page_t test_page = {};
  EXPECT_TRUE(AddPage(&list2, &test_page, PAGE_SIZE * 2));

  int release_calls = 0;
  int migrate_calls = 0;
  list2.MergeFrom(
      list1, 0, PAGE_SIZE * 3,
      [&release_calls](VmPageOrMarker&& p, uint64_t offset) {
        ASSERT(p.IsMarker());
        release_calls++;
      },
      [&migrate_calls](VmPageOrMarker* page, uint64_t offset) { migrate_calls++; });

  EXPECT_EQ(2, release_calls);
  EXPECT_EQ(0, migrate_calls);

  // Remove the page from our list as its not a real page.
  EXPECT_EQ(list2.RemoveContent(PAGE_SIZE * 2).ReleasePage(), &test_page);

  END_TEST;
}

static bool vmpl_for_every_page_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, PAGE_SIZE);
  vm_page_t test_pages[5] = {};

  uint64_t offsets[ktl::size(test_pages)] = {
      0,
      PAGE_SIZE,
      VmPageListNode::kPageFanOut * PAGE_SIZE - PAGE_SIZE,
      VmPageListNode::kPageFanOut * PAGE_SIZE,
      VmPageListNode::kPageFanOut * PAGE_SIZE + PAGE_SIZE,
  };

  for (unsigned i = 0; i < ktl::size(test_pages); i++) {
    if (i % 2) {
      EXPECT_TRUE(AddPage(&list, test_pages + i, offsets[i]));
    } else {
      EXPECT_TRUE(AddMarker(&list, offsets[i]));
    }
  }

  uint32_t idx = 0;
  auto iter_fn = [&](const auto* p, uint64_t off) -> zx_status_t {
    EXPECT_EQ(off, offsets[idx]);

    if (idx % 2) {
      EXPECT_TRUE(p->IsPage());
      EXPECT_EQ(p->Page(), test_pages + idx);
    } else {
      EXPECT_TRUE(p->IsMarker());
    }

    idx++;

    return ZX_ERR_NEXT;
  };

  list.ForEveryPage(iter_fn);
  ASSERT_EQ(idx, ktl::size(offsets));

  idx = 1;
  list.ForEveryPageInRange(iter_fn, offsets[1], offsets[ktl::size(test_pages) - 1]);
  ASSERT_EQ(idx, ktl::size(offsets) - 1);

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });

  END_TEST;
}

static bool vmpl_merge_onto_test() {
  BEGIN_TEST;

  VmPageList list1, list2;
  list1.InitializeSkew(0, 0);
  list2.InitializeSkew(0, 0);
  vm_page_t test_pages[4] = {};

  EXPECT_TRUE(AddPage(&list1, test_pages + 0, 0));
  EXPECT_TRUE(
      AddPage(&list1, test_pages + 1, VmPageListNode::kPageFanOut * PAGE_SIZE + 2 * PAGE_SIZE));
  EXPECT_TRUE(AddPage(&list2, test_pages + 2, 0));
  EXPECT_TRUE(
      AddPage(&list2, test_pages + 3, 2 * VmPageListNode::kPageFanOut * PAGE_SIZE + PAGE_SIZE));

  list_node_t free_list;
  list_initialize(&free_list);

  list1.MergeOnto(list2, [&free_list](VmPageOrMarker&& p) {
    if (p.IsPage()) {
      list_add_tail(&free_list, &p.ReleasePage()->queue_node);
    }
  });

  // (test_pages + 0) should have covered this page
  EXPECT_EQ(1ul, list_length(&free_list));
  EXPECT_EQ(test_pages + 2, list_remove_head_type(&free_list, vm_page, queue_node));

  EXPECT_EQ(test_pages + 0, list2.Lookup(0)->Page());
  EXPECT_EQ(test_pages + 1,
            list2.Lookup(VmPageListNode::kPageFanOut * PAGE_SIZE + 2 * PAGE_SIZE)->Page());
  EXPECT_EQ(test_pages + 3,
            list2.Lookup(2 * VmPageListNode::kPageFanOut * PAGE_SIZE + PAGE_SIZE)->Page());

  list2.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(3ul, list_length(&free_list));

  END_TEST;
}

static bool vmpl_contiguous_run_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);
  vm_page_t test_pages[6] = {};

  // Add test pages, some in the same node, and some in different nodes.
  // This is so that the code below adds pages in new nodes as expected.
  ASSERT_GT(VmPageListNode::kPageFanOut, 4u);
  // single page, then gap
  EXPECT_TRUE(AddPage(&list, &test_pages[0], 0));
  // gap in the same node, then two pages
  EXPECT_TRUE(AddPage(&list, &test_pages[1], 2 * PAGE_SIZE));
  EXPECT_TRUE(AddPage(&list, &test_pages[2], 3 * PAGE_SIZE));
  // gap moving to the next node, then three pages spanning the node boundary
  EXPECT_TRUE(AddPage(&list, &test_pages[3], (VmPageListNode::kPageFanOut * 2 - 1) * PAGE_SIZE));
  EXPECT_TRUE(AddPage(&list, &test_pages[4], VmPageListNode::kPageFanOut * 2 * PAGE_SIZE));
  EXPECT_TRUE(AddPage(&list, &test_pages[5], (VmPageListNode::kPageFanOut * 2 + 1) * PAGE_SIZE));

  // Perform a basic iteration to see if we can list the ranges correctly.
  uint64_t range_offsets[6] = {};
  uint64_t expected_offsets[6] = {
      0, 1, 2, 4, VmPageListNode::kPageFanOut * 2 - 1, VmPageListNode::kPageFanOut * 2 + 2};
  size_t index = 0;
  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [](const VmPageOrMarker* p, uint64_t off) { return ZX_ERR_NEXT; },
      [&range_offsets, &index](uint64_t start, uint64_t end, bool is_interval) {
        if (is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        range_offsets[index++] = start;
        range_offsets[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, VmPageListNode::kPageFanOut * 3 * PAGE_SIZE);

  EXPECT_OK(status);
  EXPECT_EQ(6u, index);
  for (size_t i = 0; i < 6; i++) {
    EXPECT_EQ(expected_offsets[i] * PAGE_SIZE, range_offsets[i]);
  }

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(6u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_contiguous_run_compare_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);
  vm_page_t test_pages[5] = {};

  // Add 5 consecutive pages. The ranges will be divided up based on the compare function.
  for (size_t i = 0; i < 5; i++) {
    EXPECT_TRUE(AddPage(&list, &test_pages[i], i * PAGE_SIZE));
  }

  // Random bools to use as results of comparison for each page.
  bool compare_results[5] = {false, true, true, false, true};
  bool page_visited[5] = {};
  // Expected ranges based on the compare function.
  uint64_t expected_offsets[4] = {1, 3, 4, 5};
  uint64_t range_offsets[4] = {};
  size_t index = 0;

  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      [&compare_results](const VmPageOrMarker* p, uint64_t off) {
        return compare_results[off / PAGE_SIZE];
      },
      [&page_visited](const VmPageOrMarker* p, uint64_t off) {
        page_visited[off / PAGE_SIZE] = true;
        return ZX_ERR_NEXT;
      },
      [&range_offsets, &index](uint64_t start, uint64_t end, bool is_interval) {
        if (is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        range_offsets[index++] = start;
        range_offsets[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, VmPageListNode::kPageFanOut * PAGE_SIZE);

  EXPECT_OK(status);

  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(compare_results[i], page_visited[i]);
  }
  EXPECT_EQ(4u, index);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(expected_offsets[i] * PAGE_SIZE, range_offsets[i]);
  }

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(5u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_contiguous_traversal_end_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);
  vm_page_t test_pages[3] = {};

  // Add 3 consecutive pages.
  for (size_t i = 0; i < 3; i++) {
    EXPECT_TRUE(AddPage(&list, &test_pages[i], i * PAGE_SIZE));
  }

  bool page_visited[3] = {};
  uint64_t expected_offsets[2] = {0, 2};
  uint64_t range_offsets[2] = {};
  size_t index = 0;
  // The compare function evaluates to true for all pages, but the traversal ends early due to
  // ZX_ERR_STOP in the per-page function.
  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [&page_visited](const VmPageOrMarker* p, uint64_t off) {
        page_visited[off / PAGE_SIZE] = true;
        // Stop the traversal at page 1. This means the last page processed should be page 1 and
        // should be included in the contiguous range. Traversal will stop *after* this page.
        return (off / PAGE_SIZE < 1) ? ZX_ERR_NEXT : ZX_ERR_STOP;
      },
      [&range_offsets, &index](uint64_t start, uint64_t end, bool is_interval) {
        if (is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        range_offsets[index++] = start;
        range_offsets[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, VmPageListNode::kPageFanOut * PAGE_SIZE);

  EXPECT_OK(status);
  // Should have visited the first two pages.
  EXPECT_TRUE(page_visited[0]);
  EXPECT_TRUE(page_visited[1]);
  EXPECT_FALSE(page_visited[2]);

  EXPECT_EQ(2u, index);
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(expected_offsets[i] * PAGE_SIZE, range_offsets[i]);
  }

  // Attempt another traversal. This time it ends early because of ZX_ERR_STOP in the contiguous
  // range function.
  index = 0;
  page_visited[0] = false;
  page_visited[1] = false;
  page_visited[2] = false;
  status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) {
        // Include even indexed pages in the range.
        return (off / PAGE_SIZE) % 2 == 0;
      },
      [&page_visited](const VmPageOrMarker* p, uint64_t off) {
        page_visited[off / PAGE_SIZE] = true;
        return ZX_ERR_NEXT;
      },
      [&range_offsets, &index](uint64_t start, uint64_t end, bool is_interval) {
        if (is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        range_offsets[index++] = start;
        range_offsets[index++] = end;
        // End traversal after the first range.
        return ZX_ERR_STOP;
      },
      0, VmPageListNode::kPageFanOut * PAGE_SIZE);

  EXPECT_OK(status);
  // Should only have visited the first page.
  EXPECT_TRUE(page_visited[0]);
  EXPECT_FALSE(page_visited[1]);
  EXPECT_FALSE(page_visited[2]);

  expected_offsets[0] = 0;
  expected_offsets[1] = 1;
  EXPECT_EQ(2u, index);
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(expected_offsets[i] * PAGE_SIZE, range_offsets[i]);
  }

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(3u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_contiguous_traversal_error_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);
  vm_page_t test_pages[3] = {};

  // Add 3 consecutive pages.
  for (size_t i = 0; i < 3; i++) {
    EXPECT_TRUE(AddPage(&list, &test_pages[i], i * PAGE_SIZE));
  }

  bool page_visited[3] = {};
  uint64_t range_offsets[2] = {};
  size_t index = 0;
  // The compare function evaluates to true for all pages, but the traversal ends early due to
  // an error returned by the per-page function.
  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [&page_visited](const VmPageOrMarker* p, uint64_t off) {
        page_visited[off / PAGE_SIZE] = true;
        // Only page 0 returns success.
        return (off / PAGE_SIZE < 1) ? ZX_ERR_NEXT : ZX_ERR_BAD_STATE;
      },
      [&range_offsets, &index](uint64_t start, uint64_t end, bool is_interval) {
        if (is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        range_offsets[index++] = start;
        range_offsets[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, VmPageListNode::kPageFanOut * PAGE_SIZE);

  EXPECT_EQ(ZX_ERR_BAD_STATE, status);
  // Should have visited the first two pages.
  EXPECT_TRUE(page_visited[0]);
  EXPECT_TRUE(page_visited[1]);
  EXPECT_FALSE(page_visited[2]);

  EXPECT_EQ(2u, index);
  // Should have been able to process the contiguous range till right before the page that failed.
  uint64_t expected_offsets[2] = {0, 1};
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(expected_offsets[i] * PAGE_SIZE, range_offsets[i]);
  }

  // Attempt another traversal. This time it ends early because of an error returned by the
  // contiguous range function.
  index = 0;
  page_visited[0] = false;
  page_visited[1] = false;
  page_visited[2] = false;
  status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) {
        // Include even indexed pages in the range.
        return (off / PAGE_SIZE) % 2 == 0;
      },
      [&page_visited](const VmPageOrMarker* p, uint64_t off) {
        page_visited[off / PAGE_SIZE] = true;
        return ZX_ERR_NEXT;
      },
      [&range_offsets, &index](uint64_t start, uint64_t end, bool is_interval) {
        if (is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        range_offsets[index++] = start;
        range_offsets[index++] = end;
        // Error after the first range.
        return ZX_ERR_BAD_STATE;
      },
      0, VmPageListNode::kPageFanOut * PAGE_SIZE);

  EXPECT_EQ(ZX_ERR_BAD_STATE, status);
  // Should only have visited the first page.
  EXPECT_TRUE(page_visited[0]);
  EXPECT_FALSE(page_visited[1]);
  EXPECT_FALSE(page_visited[2]);

  expected_offsets[0] = 0;
  expected_offsets[1] = 1;
  EXPECT_EQ(2u, index);
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(expected_offsets[i] * PAGE_SIZE, range_offsets[i]);
  }

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(3u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_interval_single_node_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval [1, 3] in a single page list node.
  constexpr uint64_t expected_start = 1, expected_end = 3;
  constexpr uint64_t size = VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE))

  uint64_t start, end;
  zx_status_t status = list.ForEveryPage([&](const VmPageOrMarker* p, uint64_t off) {
    if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
      return ZX_ERR_BAD_STATE;
    }
    if (!p->IsZeroIntervalDirty()) {
      return ZX_ERR_BAD_STATE;
    }
    if (p->IsIntervalStart()) {
      start = off;
    } else if (p->IsIntervalEnd()) {
      end = off;
    }
    return ZX_ERR_NEXT;
  });
  EXPECT_OK(status);
  EXPECT_EQ(expected_start * PAGE_SIZE, start);
  EXPECT_EQ(expected_end * PAGE_SIZE, end);

  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t gaps[4];
  size_t index = 0;
  start = 0;
  end = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          start = off;
        } else if (p->IsIntervalEnd()) {
          end = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(expected_start * PAGE_SIZE, start);
  EXPECT_EQ(expected_end * PAGE_SIZE, end);

  EXPECT_EQ(4u, index);
  for (size_t i = 0; i < index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_multiple_nodes_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  uint64_t start, end;
  zx_status_t status = list.ForEveryPage([&](const VmPageOrMarker* p, uint64_t off) {
    if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
      return ZX_ERR_BAD_STATE;
    }
    if (!p->IsZeroIntervalDirty()) {
      return ZX_ERR_BAD_STATE;
    }
    if (p->IsIntervalStart()) {
      start = off;
    } else if (p->IsIntervalEnd()) {
      end = off;
    }
    return ZX_ERR_NEXT;
  });
  EXPECT_OK(status);
  EXPECT_EQ(expected_start * PAGE_SIZE, start);
  EXPECT_EQ(expected_end * PAGE_SIZE, end);

  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t gaps[4];
  size_t index = 0;
  start = 0;
  end = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          start = off;
        } else if (p->IsIntervalEnd()) {
          end = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(expected_start * PAGE_SIZE, start);
  EXPECT_EQ(expected_end * PAGE_SIZE, end);

  EXPECT_EQ(4u, index);
  for (size_t i = 0; i < index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_traversal_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // End traversal partway into the interval.
  // Should only see the gap before the interval start.
  uint64_t expected_gaps[2] = {0, expected_start};
  uint64_t gaps[2];
  size_t index = 0;
  uint64_t start = 0, end = 0;
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          start = off;
        } else if (p->IsIntervalEnd()) {
          end = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, (expected_end - 1) * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(expected_start * PAGE_SIZE, start);
  // We should not have seen the end of the interval.
  EXPECT_EQ(0u, end);

  EXPECT_EQ(2u, index);
  for (size_t i = 0; i < index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  // Start traversal partway into the interval.
  // Should only see the gap after the interval end.
  expected_gaps[0] = expected_end + 1;
  expected_gaps[1] = size;
  index = 0;
  start = 0;
  end = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          start = off;
        } else if (p->IsIntervalEnd()) {
          end = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      (expected_start + 1) * PAGE_SIZE, size * PAGE_SIZE);
  EXPECT_OK(status);

  // We should not have seen the start of the interval.
  EXPECT_EQ(0u, start);
  EXPECT_EQ(expected_end * PAGE_SIZE, end);

  EXPECT_EQ(2u, index);
  for (size_t i = 0; i < index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  // Start traversal partway into the interval, and also end before the interval end.
  // Should not see any gaps or pages either.
  index = 0;
  start = 0;
  end = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          start = off;
        } else if (p->IsIntervalEnd()) {
          end = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      (expected_start + 1) * PAGE_SIZE, (expected_end - 1) * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(0u, start);
  EXPECT_EQ(0u, end);
  EXPECT_EQ(0u, index);

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_merge_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval [7, 12].
  constexpr uint64_t expected_start = 7, expected_end = 12;
  constexpr uint64_t size = 2 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE))

  // Add intervals to the left and right of the existing interval and verify that they are merged
  // into a single interval.
  constexpr uint64_t new_expected_start = 3;
  constexpr uint64_t new_expected_end = 20;
  ASSERT_GT(size, new_expected_end);
  // Interval [3, 6].
  ASSERT_OK(list.AddZeroInterval(new_expected_start * PAGE_SIZE, expected_start * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));
  // Interval [13, 20].
  ASSERT_OK(list.AddZeroInterval((expected_end + 1) * PAGE_SIZE, (new_expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  uint64_t start, end;
  zx_status_t status = list.ForEveryPage([&](const VmPageOrMarker* p, uint64_t off) {
    if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
      return ZX_ERR_BAD_STATE;
    }
    if (!p->IsZeroIntervalDirty()) {
      return ZX_ERR_BAD_STATE;
    }
    if (p->IsIntervalStart()) {
      start = off;
    } else if (p->IsIntervalEnd()) {
      end = off;
    }
    return ZX_ERR_NEXT;
  });
  EXPECT_OK(status);
  EXPECT_EQ(new_expected_start * PAGE_SIZE, start);
  EXPECT_EQ(new_expected_end * PAGE_SIZE, end);

  constexpr uint64_t expected_gaps[4] = {0, new_expected_start, new_expected_end + 1, size};
  uint64_t gaps[4];
  size_t index = 0;
  start = 0;
  end = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          start = off;
        } else if (p->IsIntervalEnd()) {
          end = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(new_expected_start * PAGE_SIZE, start);
  EXPECT_EQ(new_expected_end * PAGE_SIZE, end);

  EXPECT_EQ(4u, index);
  for (size_t i = 0; i < index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_add_page_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Adding a page in the interval should split the interval.
  vm_page_t page;
  constexpr uint64_t page_offset = VmPageListNode::kPageFanOut;
  EXPECT_TRUE(AddPage(&list, &page, page_offset * PAGE_SIZE));

  constexpr uint64_t expected_intervals[4] = {expected_start, page_offset - 1, page_offset + 1,
                                              expected_end};
  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t intervals[4];
  uint64_t gaps[4];
  uint64_t interval_index = 0, gap_index = 0;
  uint64_t page_off = 0;

  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd() || p->IsPage())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          if (interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
        } else if (p->IsIntervalEnd()) {
          if (interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
        } else if (p->IsPage()) {
          page_off = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(4u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }

  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  EXPECT_EQ(page_offset * PAGE_SIZE, page_off);

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(1u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_interval_add_page_slots_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // 3 page interval such that adding a page in the middle creates two distinct slots.
  constexpr uint64_t expected_start = 0, expected_end = 2;
  constexpr uint64_t size = VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Adding a page in the interval should split the interval.
  vm_page_t page;
  constexpr uint64_t page_offset = 1;
  EXPECT_TRUE(AddPage(&list, &page, page_offset * PAGE_SIZE));

  constexpr uint64_t expected_intervals[2] = {expected_start, expected_end};
  constexpr uint64_t expected_gaps[2] = {expected_end + 1, size};
  uint64_t intervals[2];
  uint64_t gaps[2];
  uint64_t interval_index = 0, gap_index = 0;
  uint64_t page_off = 0;

  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalSlot() || p->IsPage())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalSlot()) {
          intervals[interval_index++] = off;
        } else if (p->IsPage()) {
          page_off = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(2u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  EXPECT_EQ(page_offset * PAGE_SIZE, page_off);

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(1u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_interval_add_page_start_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  constexpr uint64_t expected_start = 0, expected_end = 2;
  constexpr uint64_t size = VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Add a page at the start of the interval.
  vm_page_t pages[2];
  EXPECT_TRUE(AddPage(&list, &pages[0], expected_start * PAGE_SIZE));

  const uint64_t expected_intervals[2] = {expected_start + 1, expected_end};
  const uint64_t expected_gaps[2] = {expected_end + 1, size};
  uint64_t intervals[2];
  uint64_t gaps[2];
  uint64_t interval_index = 0, gap_index = 0;
  uint64_t page_off = size * PAGE_SIZE;

  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd() || p->IsPage())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          if (interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
        } else if (p->IsIntervalEnd()) {
          if (interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
        } else if (p->IsPage()) {
          page_off = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(2u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  EXPECT_EQ(expected_start * PAGE_SIZE, page_off);

  // Add another page at the start of the new interval.
  EXPECT_TRUE(AddPage(&list, &pages[1], (expected_start + 1) * PAGE_SIZE));

  const uint64_t expected_pages[2] = {expected_start, expected_start + 1};
  uint64_t page_offsets[2] = {};
  uint64_t page_index = 0;
  interval_index = 0;
  gap_index = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalSlot() || p->IsPage())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalSlot()) {
          intervals[interval_index++] = off;
        } else if (p->IsPage()) {
          page_offsets[page_index++] = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(1u, interval_index);
  EXPECT_EQ(expected_end * PAGE_SIZE, intervals[0]);

  EXPECT_EQ(2u, page_index);
  for (size_t i = 0; i < page_index; i++) {
    EXPECT_EQ(expected_pages[i] * PAGE_SIZE, page_offsets[i]);
  }

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(2u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_interval_add_page_end_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  constexpr uint64_t expected_start = 0;
  uint64_t expected_end = 2;
  constexpr uint64_t size = VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Add a page at the end of the interval.
  vm_page_t pages[2];
  EXPECT_TRUE(AddPage(&list, &pages[0], expected_end * PAGE_SIZE));

  const uint64_t expected_intervals[2] = {expected_start, expected_end - 1};
  const uint64_t expected_gaps[2] = {expected_end + 1, size};
  uint64_t intervals[2];
  uint64_t gaps[2];
  uint64_t interval_index = 0, gap_index = 0;
  uint64_t page_off = 0;

  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd() || p->IsPage())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          if (interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
        } else if (p->IsIntervalEnd()) {
          if (interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
        } else if (p->IsPage()) {
          page_off = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(2u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  EXPECT_EQ(expected_end * PAGE_SIZE, page_off);

  // Add another page at the end of the new interval.
  EXPECT_TRUE(AddPage(&list, &pages[1], (expected_end - 1) * PAGE_SIZE));

  const uint64_t expected_pages[2] = {expected_end - 1, expected_end};
  uint64_t page_offsets[2] = {};
  uint64_t page_index = 0;
  interval_index = 0;
  gap_index = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalSlot() || p->IsPage())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalSlot()) {
          intervals[interval_index++] = off;
        } else if (p->IsPage()) {
          page_offsets[page_index++] = off;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(1u, interval_index);
  EXPECT_EQ(expected_start * PAGE_SIZE, intervals[0]);

  EXPECT_EQ(2u, page_index);
  for (size_t i = 0; i < page_index; i++) {
    EXPECT_EQ(expected_pages[i] * PAGE_SIZE, page_offsets[i]);
  }

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(2u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_interval_replace_slot_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  constexpr uint64_t expected_interval = 0;
  constexpr uint64_t size = VmPageListNode::kPageFanOut;
  ASSERT_OK(list.AddZeroInterval(expected_interval * PAGE_SIZE, (expected_interval + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  constexpr uint64_t expected_gaps[2] = {expected_interval + 1, size};
  uint64_t interval = size * PAGE_SIZE;
  uint64_t gaps[2];
  uint64_t gap_index = 0;
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsIntervalSlot()) {
          return ZX_ERR_BAD_STATE;
        }
        interval = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  EXPECT_EQ(expected_interval * PAGE_SIZE, interval);

  // Add a page in the interval slot.
  vm_page_t page;
  EXPECT_TRUE(AddPage(&list, &page, expected_interval * PAGE_SIZE));

  uint64_t page_off = size * PAGE_SIZE;
  gap_index = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsPage()) {
          return ZX_ERR_BAD_STATE;
        }
        page_off = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(2u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  EXPECT_EQ(expected_interval * PAGE_SIZE, page_off);

  list_node_t free_list;
  list_initialize(&free_list);
  list.RemoveAllContent([&free_list](VmPageOrMarker&& p) {
    list_add_tail(&free_list, &p.ReleasePage()->queue_node);
  });
  EXPECT_EQ(1u, list_length(&free_list));

  END_TEST;
}

static bool vmpl_interval_contig_full_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  constexpr uint64_t expected_pages[2] = {expected_start, expected_end};
  constexpr uint64_t expected_contig[2] = {expected_start, expected_end + 1};
  uint64_t pages[2];
  uint64_t contig[2];
  uint64_t page_index = 0, contig_index = 0;
  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart()) {
          if (page_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
        } else if (p->IsIntervalEnd()) {
          if (page_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
        }
        pages[page_index++] = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end, bool is_interval) {
        if (!is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        contig[contig_index++] = begin;
        contig[contig_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);

  EXPECT_EQ(2u, page_index);
  for (size_t i = 0; i < page_index; i++) {
    EXPECT_EQ(expected_pages[i] * PAGE_SIZE, pages[i]);
  }

  EXPECT_EQ(2u, contig_index);
  for (size_t i = 0; i < contig_index; i++) {
    EXPECT_EQ(expected_contig[i] * PAGE_SIZE, contig[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_contig_partial_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  uint64_t page;
  uint64_t contig[2];
  uint64_t contig_index = 0;
  // Start the traversal partway into the interval.
  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsIntervalEnd()) {
          return ZX_ERR_BAD_STATE;
        }
        page = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end, bool is_interval) {
        if (!is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        contig[contig_index++] = begin;
        contig[contig_index++] = end;
        return ZX_ERR_NEXT;
      },
      (expected_start + 1) * PAGE_SIZE, size * PAGE_SIZE);
  EXPECT_OK(status);

  // Should only have visited the end.
  uint64_t expected_page = expected_end;
  uint64_t expected_contig[2] = {expected_start + 1, expected_end + 1};
  EXPECT_EQ(expected_page * PAGE_SIZE, page);
  EXPECT_EQ(2u, contig_index);
  for (size_t i = 0; i < contig_index; i++) {
    EXPECT_EQ(expected_contig[i] * PAGE_SIZE, contig[i]);
  }

  contig_index = 0;
  // End the traversal partway into the interval.
  status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsIntervalStart()) {
          return ZX_ERR_BAD_STATE;
        }
        page = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end, bool is_interval) {
        if (!is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        contig[contig_index++] = begin;
        contig[contig_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, (expected_end - 1) * PAGE_SIZE);
  EXPECT_OK(status);

  // Should only have visited the start.
  expected_page = expected_start;
  expected_contig[0] = expected_start;
  expected_contig[1] = expected_end - 1;
  EXPECT_EQ(expected_page * PAGE_SIZE, page);
  EXPECT_EQ(2u, contig_index);
  for (size_t i = 0; i < contig_index; i++) {
    EXPECT_EQ(expected_contig[i] * PAGE_SIZE, contig[i]);
  }

  contig_index = 0;
  // Start and end the traversal partway into the interval.
  status = list.ForEveryPageAndContiguousRunInRange(
      [](const VmPageOrMarker* p, uint64_t off) { return true; },
      [&](const VmPageOrMarker* p, uint64_t off) {
        // Should not visit any slot.
        return ZX_ERR_BAD_STATE;
      },
      [&](uint64_t begin, uint64_t end, bool is_interval) {
        if (!is_interval) {
          return ZX_ERR_BAD_STATE;
        }
        contig[contig_index++] = begin;
        contig[contig_index++] = end;
        return ZX_ERR_NEXT;
      },
      (expected_start + 1) * PAGE_SIZE, (expected_end - 1) * PAGE_SIZE);
  EXPECT_OK(status);

  // Should have seen the requested contiguous range, even though neither the start nor the end was
  // visited.
  expected_contig[0] = expected_start + 1;
  expected_contig[1] = expected_end - 1;
  EXPECT_EQ(2u, contig_index);
  for (size_t i = 0; i < contig_index; i++) {
    EXPECT_EQ(expected_contig[i] * PAGE_SIZE, contig[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});
  END_TEST;
}

static bool vmpl_interval_contig_compare_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  uint64_t page;
  // Start the traversal partway into the interval.
  zx_status_t status = list.ForEveryPageAndContiguousRunInRange(
      // Interval start evaluates to false.
      [](const VmPageOrMarker* p, uint64_t off) { return !p->IsIntervalStart(); },
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsIntervalEnd()) {
          return ZX_ERR_BAD_STATE;
        }
        page = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end, bool is_interval) {
        // The start does not fulfill the condition, so we should not find a valid contiguous run.
        return ZX_ERR_INVALID_ARGS;
      },
      (expected_start + 1) * PAGE_SIZE, size * PAGE_SIZE);
  EXPECT_OK(status);

  // Should only have visited the end.
  uint64_t expected_page = expected_end;
  EXPECT_EQ(expected_page * PAGE_SIZE, page);

  // End the traversal partway into the interval.
  status = list.ForEveryPageAndContiguousRunInRange(
      // Interval end evaluates to false.
      [](const VmPageOrMarker* p, uint64_t off) { return !p->IsIntervalEnd(); },
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsIntervalStart()) {
          return ZX_ERR_BAD_STATE;
        }
        page = off;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end, bool is_interval) {
        // The end does not fulfill the condition, so we should not find a valid contiguous run.
        return ZX_ERR_INVALID_ARGS;
      },
      0, (expected_end - 1) * PAGE_SIZE);
  EXPECT_OK(status);

  // Should only have visited the start.
  expected_page = expected_start;
  EXPECT_EQ(expected_page * PAGE_SIZE, page);

  list.RemoveAllContent([](VmPageOrMarker&&) {});
  END_TEST;
}

static bool vmpl_interval_populate_full_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 5 nodes, with the middle ones unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 4 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 5 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Populate the entire interval.
  ASSERT_OK(
      list.PopulateSlotsInInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE));

  uint64_t next_off = expected_start * PAGE_SIZE;
  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t gaps[4];
  size_t index = 0;
  // We should only see interval slots.
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsIntervalSlot()) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (off != next_off) {
          return ZX_ERR_OUT_OF_RANGE;
        }
        next_off += PAGE_SIZE;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[index++] = begin;
        gaps[index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ((expected_end + 1) * PAGE_SIZE, next_off);
  EXPECT_EQ(4u, index);
  for (size_t i = 0; i < index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_populate_partial_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Populate some slots in the middle of the interval.
  constexpr uint64_t slot_start = expected_start + 2;
  constexpr uint64_t slot_end = expected_end - 2;
  ASSERT_GT(slot_end, slot_start);
  ASSERT_OK(list.PopulateSlotsInInterval(slot_start * PAGE_SIZE, (slot_end + 1) * PAGE_SIZE));

  constexpr uint64_t expected_intervals[4] = {expected_start, slot_start - 1, slot_end + 1,
                                              expected_end};
  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t intervals[4];
  uint64_t gaps[4];
  size_t interval_index = 0, gap_index = 0;
  uint64_t slot = slot_start * PAGE_SIZE;
  // We should see interval slots in the range we populated.
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsInterval()) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart() || p->IsIntervalEnd()) {
          if (p->IsIntervalStart() && interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          if (p->IsIntervalEnd() && interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
          return ZX_ERR_NEXT;
        }
        if (off != slot) {
          return ZX_ERR_BAD_STATE;
        }
        slot += PAGE_SIZE;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ((slot_end + 1) * PAGE_SIZE, slot);
  EXPECT_EQ(4u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }
  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_populate_start_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Populate some slots beginning at the start of the interval.
  constexpr uint64_t slot_start = expected_start;
  constexpr uint64_t slot_end = expected_end - 2;
  ASSERT_GT(slot_end, slot_start);
  ASSERT_OK(list.PopulateSlotsInInterval(slot_start * PAGE_SIZE, (slot_end + 1) * PAGE_SIZE));

  constexpr uint64_t expected_intervals[2] = {slot_end + 1, expected_end};
  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t intervals[2];
  uint64_t gaps[4];
  size_t interval_index = 0, gap_index = 0;
  uint64_t slot = slot_start * PAGE_SIZE;
  // We should see interval slots in the range we populated.
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsInterval()) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart() || p->IsIntervalEnd()) {
          if (p->IsIntervalStart() && interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          if (p->IsIntervalEnd() && interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
          return ZX_ERR_NEXT;
        }
        if (off != slot) {
          return ZX_ERR_BAD_STATE;
        }
        slot += PAGE_SIZE;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ((slot_end + 1) * PAGE_SIZE, slot);
  EXPECT_EQ(2u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }
  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_populate_end_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Populate some slots ending at the end of the interval.
  constexpr uint64_t slot_start = expected_start + 2;
  constexpr uint64_t slot_end = expected_end;
  ASSERT_GT(slot_end, slot_start);
  ASSERT_OK(list.PopulateSlotsInInterval(slot_start * PAGE_SIZE, (slot_end + 1) * PAGE_SIZE));

  constexpr uint64_t expected_intervals[2] = {expected_start, slot_start - 1};
  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t intervals[2];
  uint64_t gaps[4];
  size_t interval_index = 0, gap_index = 0;
  uint64_t slot = slot_start * PAGE_SIZE;
  // We should see interval slots in the range we populated.
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsInterval()) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart() || p->IsIntervalEnd()) {
          if (p->IsIntervalStart() && interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          if (p->IsIntervalEnd() && interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
          return ZX_ERR_NEXT;
        }
        if (off != slot) {
          return ZX_ERR_BAD_STATE;
        }
        slot += PAGE_SIZE;
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ((slot_end + 1) * PAGE_SIZE, slot);
  EXPECT_EQ(2u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }
  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

static bool vmpl_interval_populate_slot_test() {
  BEGIN_TEST;

  VmPageList list;
  list.InitializeSkew(0, 0);

  // Interval spanning across 3 nodes, with the middle one unpopulated.
  constexpr uint64_t expected_start = 1, expected_end = 2 * VmPageListNode::kPageFanOut;
  constexpr uint64_t size = 3 * VmPageListNode::kPageFanOut;
  ASSERT_GT(size, expected_end);
  ASSERT_OK(list.AddZeroInterval(expected_start * PAGE_SIZE, (expected_end + 1) * PAGE_SIZE,
                                 VmPageOrMarker::IntervalDirtyState::Dirty));

  EXPECT_TRUE(list.AnyPagesOrIntervalsInRange(0, size * PAGE_SIZE));

  // Populate a single slot in the interval.
  constexpr uint64_t single_slot = expected_end - 3;
  ASSERT_OK(list.PopulateSlotsInInterval(single_slot * PAGE_SIZE, (single_slot + 1) * PAGE_SIZE));

  constexpr uint64_t expected_intervals[4] = {expected_start, single_slot - 1, single_slot + 1,
                                              expected_end};
  constexpr uint64_t expected_gaps[4] = {0, expected_start, expected_end + 1, size};
  uint64_t intervals[4];
  uint64_t gaps[4];
  size_t interval_index = 0, gap_index = 0;
  // We should see a single interval slot.
  zx_status_t status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsInterval()) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart() || p->IsIntervalEnd()) {
          if (p->IsIntervalStart() && interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          if (p->IsIntervalEnd() && interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
          return ZX_ERR_NEXT;
        }
        if (off != single_slot * PAGE_SIZE) {
          return ZX_ERR_BAD_STATE;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ(4u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }
  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  // Try to populate a slot over a single sentinel. This should be a no-op.
  ASSERT_OK(list.PopulateSlotsInInterval(single_slot * PAGE_SIZE, (single_slot + 1) * PAGE_SIZE));
  interval_index = 0;
  gap_index = 0;
  // We should see a single interval slot.
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!p->IsInterval()) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart() || p->IsIntervalEnd()) {
          if (p->IsIntervalStart() && interval_index % 2 == 1) {
            return ZX_ERR_BAD_STATE;
          }
          if (p->IsIntervalEnd() && interval_index % 2 == 0) {
            return ZX_ERR_BAD_STATE;
          }
          intervals[interval_index++] = off;
          return ZX_ERR_NEXT;
        }
        if (off != single_slot * PAGE_SIZE) {
          return ZX_ERR_BAD_STATE;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ(4u, interval_index);
  for (size_t i = 0; i < interval_index; i++) {
    EXPECT_EQ(expected_intervals[i] * PAGE_SIZE, intervals[i]);
  }
  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  // Try to return the single slot that we populated. This should return the interval to its
  // original state.
  list.ReturnIntervalSlot(single_slot * PAGE_SIZE);
  gap_index = 0;
  status = list.ForEveryPageAndGapInRange(
      [&](const VmPageOrMarker* p, uint64_t off) {
        if (!(p->IsIntervalStart() || p->IsIntervalEnd())) {
          return ZX_ERR_BAD_STATE;
        }
        if (!p->IsZeroIntervalDirty()) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalStart() && off != expected_start * PAGE_SIZE) {
          return ZX_ERR_BAD_STATE;
        }
        if (p->IsIntervalEnd() && off != expected_end * PAGE_SIZE) {
          return ZX_ERR_BAD_STATE;
        }
        return ZX_ERR_NEXT;
      },
      [&](uint64_t begin, uint64_t end) {
        gaps[gap_index++] = begin;
        gaps[gap_index++] = end;
        return ZX_ERR_NEXT;
      },
      0, size * PAGE_SIZE);
  EXPECT_OK(status);
  EXPECT_EQ(4u, gap_index);
  for (size_t i = 0; i < gap_index; i++) {
    EXPECT_EQ(expected_gaps[i] * PAGE_SIZE, gaps[i]);
  }

  list.RemoveAllContent([](VmPageOrMarker&&) {});

  END_TEST;
}

UNITTEST_START_TESTCASE(vm_page_list_tests)
VM_UNITTEST(vmpl_add_remove_page_test)
VM_UNITTEST(vmpl_basic_marker_test)
VM_UNITTEST(vmpl_basic_reference_test)
VM_UNITTEST(vmpl_content_split_bits_test)
VM_UNITTEST(vmpl_replace_preserves_split_bits)
VM_UNITTEST(vmpl_free_pages_test)
VM_UNITTEST(vmpl_free_pages_last_page_test)
VM_UNITTEST(vmpl_near_last_offset_free)
VM_UNITTEST(vmpl_take_single_page_even_test)
VM_UNITTEST(vmpl_take_single_page_odd_test)
VM_UNITTEST(vmpl_take_all_pages_test)
VM_UNITTEST(vmpl_take_middle_pages_test)
VM_UNITTEST(vmpl_take_gap_test)
VM_UNITTEST(vmpl_take_empty_test)
VM_UNITTEST(vmpl_take_cleanup_test)
VM_UNITTEST(vmpl_page_gap_iter_test)
VM_UNITTEST(vmpl_merge_offset_test)
VM_UNITTEST(vmpl_merge_overlap_test)
VM_UNITTEST(vmpl_for_every_page_test)
VM_UNITTEST(vmpl_merge_onto_test)
VM_UNITTEST(vmpl_merge_marker_test)
VM_UNITTEST(vmpl_contiguous_run_test)
VM_UNITTEST(vmpl_contiguous_run_compare_test)
VM_UNITTEST(vmpl_contiguous_traversal_end_test)
VM_UNITTEST(vmpl_contiguous_traversal_error_test)
VM_UNITTEST(vmpl_interval_single_node_test)
VM_UNITTEST(vmpl_interval_multiple_nodes_test)
VM_UNITTEST(vmpl_interval_traversal_test)
VM_UNITTEST(vmpl_interval_merge_test)
VM_UNITTEST(vmpl_interval_add_page_test)
VM_UNITTEST(vmpl_interval_add_page_slots_test)
VM_UNITTEST(vmpl_interval_add_page_start_test)
VM_UNITTEST(vmpl_interval_add_page_end_test)
VM_UNITTEST(vmpl_interval_replace_slot_test)
VM_UNITTEST(vmpl_interval_contig_full_test)
VM_UNITTEST(vmpl_interval_contig_partial_test)
VM_UNITTEST(vmpl_interval_contig_compare_test)
VM_UNITTEST(vmpl_interval_populate_full_test)
VM_UNITTEST(vmpl_interval_populate_partial_test)
VM_UNITTEST(vmpl_interval_populate_start_test)
VM_UNITTEST(vmpl_interval_populate_end_test)
VM_UNITTEST(vmpl_interval_populate_slot_test)
UNITTEST_END_TESTCASE(vm_page_list_tests, "vmpl", "VmPageList tests")

}  // namespace vm_unittest
