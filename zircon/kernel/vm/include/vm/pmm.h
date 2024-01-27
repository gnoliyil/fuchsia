// Copyright 2017 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_VM_INCLUDE_VM_PMM_H_
#define ZIRCON_KERNEL_VM_INCLUDE_VM_PMM_H_

#include <zircon/compiler.h>
#include <zircon/types.h>

#include <vm/evictor.h>
#include <vm/page.h>
#include <vm/page_queues.h>

// physical allocator
typedef struct pmm_arena_info {
  char name[16];

  uint flags;

  paddr_t base;
  size_t size;
} pmm_arena_info_t;

class VmCompression;
class PhysicalPageBorrowingConfig;
class LoanSweeper;

#define PMM_ARENA_FLAG_LO_MEM \
  (0x1)  // this arena is contained within architecturally-defined 'low memory'

// Add a pre-filled memory arena to the physical allocator.
// The arena data will be copied.
zx_status_t pmm_add_arena(const pmm_arena_info_t* arena) __NONNULL((1));

// Returns the number of arenas.
size_t pmm_num_arenas();

// Copies |count| pmm_arena_info_t objects into |buffer| starting with the |i|-th arena ordered by
// base address.  For example, passing an |i| of 1 would skip the 1st arena.
//
// The objects will be sorted in ascending order by arena base address.
//
// Returns ZX_ERR_OUT_OF_RANGE if |count| is 0 or |i| and |count| specify an invalid range.
//
// Returns ZX_ERR_BUFFER_TOO_SMALL if the buffer is too small.
zx_status_t pmm_get_arena_info(size_t count, uint64_t i, pmm_arena_info_t* buffer,
                               size_t buffer_size);

// flags for allocation routines below
#define PMM_ALLOC_FLAG_ANY (0 << 0)     // no restrictions on which arena to allocate from
#define PMM_ALLOC_FLAG_LO_MEM (1 << 0)  // allocate only from arenas marked LO_MEM
// The caller is able to wait and retry this allocation and so pmm allocation functions are allowed
// to return ZX_ERR_SHOULD_WAIT, as opposed to ZX_ERR_NO_MEMORY, to indicate that the caller should
// wait and try again. This is intended for the PMM to tell callers who are able to wait that memory
// is low. The caller should not infer anything about memory state if it is told to wait, as the PMM
// may tell it to wait for any reason.
#define PMM_ALLOC_FLAG_CAN_WAIT (1 << 1)
// The default (flag not set) is to not allocate a loaned page, so that we don't end up with loaned
// pages allocated for arbitrary purposes that prevent us from getting the loaned page back quickly.
#define PMM_ALLOC_FLAG_CAN_BORROW (1 << 2)
// Require a loaned page, and fail to allocate if a loaned page isn't available.
#define PMM_ALLOC_FLAG_MUST_BORROW (1 << 3)

// Allocate count pages of physical memory, adding to the tail of the passed list.
// The list must be initialized.
// Note that if PMM_ALLOC_FLAG_CAN_WAIT is passed in then this could always return
// ZX_ERR_SHOULD_WAIT. Since there is no way to wait until an arbitrary number of pages can be
// allocated (see comment on |pmm_wait_till_should_retry_single_alloc|) passing
// PMM_ALLOC_FLAG_CAN_WAIT here should be used as an optimistic fast path, and the caller should
// have a fallback of allocating single pages.
zx_status_t pmm_alloc_pages(size_t count, uint alloc_flags, list_node* list) __NONNULL((3));

// Allocate a single page of physical memory.
zx_status_t pmm_alloc_page(uint alloc_flags, vm_page** p) __NONNULL((2));
zx_status_t pmm_alloc_page(uint alloc_flags, paddr_t* pa) __NONNULL((2));
zx_status_t pmm_alloc_page(uint alloc_flags, vm_page** p, paddr_t* pa) __NONNULL((2, 3));

// Allocate a specific range of physical pages, adding to the tail of the passed list.
zx_status_t pmm_alloc_range(paddr_t address, size_t count, list_node* list) __NONNULL((3));

// Allocate a run of contiguous pages, aligned on log2 byte boundary (0-31).
// Return the base address of the run in the physical address pointer and
// append the allocate page structures to the tail of the passed in list.
zx_status_t pmm_alloc_contiguous(size_t count, uint alloc_flags, uint8_t align_log2, paddr_t* pa,
                                 list_node* list) __NONNULL((4, 5));

// Mark contiguous pages as "loaned", and free the pages so they can be borrowed by page allocations
// that specify PMM_ALLOC_FLAG_CAN_BORROW.  The caller must eventually call pmm_cancel_loan() +
// pmm_end_loan(), or pmm_delete_lender(), on ranges that cover exactly all the loaned pages.
void pmm_begin_loan(list_node* page_list);

// All pages in the range must be currently loaned.  This call must be made before pmm_end_loan().
// This call has no restrictions on whether the pages are presently in use or presently FREE.
//
// This call prevents the pages from being re-used for any new purpose until pmm_end_loan().  For
// presently-FREE pages, this removes the pages from free_loaned_list_.  For presently-used pages,
// this specifies that the page will not be added to free_loaned_list_ when later freed.  Once these
// pages are all FREE (to be ensured by the caller via PhysicalPageProvider reclaim of the pages),
// the loan can be ended with pmm_end_loan().
void pmm_cancel_loan(paddr_t address, size_t count);

// All the pages in the range must be currently loaned, and must be currently loan_cancelled, and
// must be FREE.  End loaning of these contiguous pages, and return the pages via page_list as
// non-loaned pages allocated by/for the caller.
void pmm_end_loan(paddr_t address, size_t count, list_node* page_list);

// All the pages in the range must be currently loaned.  Make the pages no longer loaned without
// allocating them to the caller or changing the pages' current state().  This is used when deleting
// a contiguous VMO which may have previously loaned pages.  After this, the pages are normal
// (non-loaned) pages.
void pmm_delete_lender(paddr_t address, size_t count);

// Free a list of physical pages.
void pmm_free(list_node* list) __NONNULL((1));

// Free a single page.
void pmm_free_page(vm_page_t* page) __NONNULL((1));

// Return count of unallocated physical pages in system.
uint64_t pmm_count_free_pages();

// Return count of unallocated loaned physical pages in system.
uint64_t pmm_count_loaned_free_pages();

uint64_t pmm_count_loaned_used_pages();

// Return count of loaned pages, including both allocated and unallocated.
uint64_t pmm_count_loaned_pages();

// Return count of pages which are presently loaned with the loan cancelled.  This is a transient
// state so we shouldn't see a non-zero value persisting for long unless the system is constantly
// seeing loan/cancel churn.
uint64_t pmm_count_loan_cancelled_pages();

// Return amount of physical memory in system, in bytes.
uint64_t pmm_count_total_bytes();

// Return the PageQueues.
PageQueues* pmm_page_queues();

// Return the Evictor.
Evictor* pmm_evictor();

// Retrieve any page compression instance. If this returns non-null then it's return value will not
// change and the result can be cached.
VmCompression* pmm_page_compression();

// Set the page compression instance. Returns an error if one has already been set.
zx_status_t pmm_set_page_compression(fbl::RefPtr<VmCompression> compression);

// Return the singleton PhysicalPageBorrowingConfig.
PhysicalPageBorrowingConfig* pmm_physical_page_borrowing_config();

// virtual to physical
paddr_t vaddr_to_paddr(const void* va);

// paddr to vm_page_t
vm_page_t* paddr_to_vm_page(paddr_t addr);

#define MAX_WATERMARK_COUNT 8

typedef void (*mem_avail_state_updated_callback_t)(void* context, uint8_t cur_state);

// Function to initialize PMM memory reclamation.
//
// |watermarks| is an array of values that delineate the memory availability states. The values
// should be monotonically increasing with intervals of at least PAGE_SIZE and its first entry must
// be larger than |debounce|. Its length is given by |watermark_count|, with a maximum of
// MAX_WATERMARK_COUNT. The pointer is not retained after this function returns.
//
// When the system has a given amount of free memory available, the memory availability state is
// defined as the index of the smallest watermark which is greater than that amount of free
// memory.  Whenever the amount of memory enters a new state, |mem_avail_state_updated_callback|
// will be invoked with the registered context and the index of the new state.
//
// Transitions are debounced by not leaving a state until the amount of memory is at least
// |debounce| bytes outside of the state. Note that large |debounce| values can cause states
// to be entirely skipped. Also note that this means that at least |debounce| bytes must be
// reclaimed before the system can transition into a healthier memory availability state.
//
// To give an example of state transitions with the watermarks [20MB, 40MB, 45MB, 55MB] and
// debounce 15MB.
//   75MB:4 -> 41MB:4 -> 40MB:2 -> 26MB:2 -> 25MB:1 -> 6MB:1 ->
//   5MB:0 -> 34MB:0 -> 35MB:1 -> 54MB:1 -> 55MB:4
//
// Invocations of |mem_avail_state_updated_callback| are fully serialized, but they occur on
// arbitrary threads when pages are being freed. As this likely occurs under important locks, the
// callback itself should not perform memory reclamation; instead it should communicate the memory
// level to a separate thread that is responsible for reclaiming memory. Furthermore, the callback
// is immediately invoked during the execution of this function with the index of the initial memory
// state.
zx_status_t pmm_init_reclamation(const uint64_t* watermarks, uint8_t watermark_count,
                                 uint64_t debounce, void* context,
                                 mem_avail_state_updated_callback_t callback);

// This is intended to be used if an allocation function returns ZX_ERR_SHOULD_WAIT and blocks
// until such a time as it is appropriate to retry a single allocation for a single page. Due to
// current implementation limitations, this only waits until single page allocations should be
// retried, and cannot be used to wait for multi page allocations.
// Returns the same set of values as Event::Wait.
zx_status_t pmm_wait_till_should_retry_single_alloc(const Deadline& deadline);

// Tells the PMM that it should never return ZX_ERR_SHOULD_WAIT (even in the presence of
// PMM_ALLOC_FLAG_CAN_WAIT) and from now on must either succeed an allocation, or fail with
// ZX_ERR_NO_MEMORY.
// There is no way to re-enable this as disabling is intended for use in the panic/shutdown path.
void pmm_stop_returning_should_wait();

// Should be called after the kernel command line has been parsed.
void pmm_checker_init_from_cmdline();

// Synchronously walk the PMM's free list and validate each page.  This is an incredibly expensive
// operation and should only be used for debugging purposes.
void pmm_checker_check_all_free_pages();

// Synchronously walk the PMM's free list and poison (via kASAN) each page. This is an
// incredibly expensive operation and should be used with care.
void pmm_asan_poison_all_free_pages();

int64_t pmm_get_alloc_failed_count();

// Returns true if the PMM has ever failed an allocation with ZX_ERR_NO_MEMORY.
bool pmm_has_alloc_failed_no_mem();

void pmm_print_physical_page_borrowing_stats();

#endif  // ZIRCON_KERNEL_VM_INCLUDE_VM_PMM_H_
