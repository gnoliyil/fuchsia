// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <align.h>
#include <lib/syscalls/forward.h>
#include <zircon/syscalls-next.h>

#include <fbl/ref_ptr.h>
#include <ktl/move.h>
#include <object/pager_dispatcher.h>
#include <object/vm_object_dispatcher.h>
#include <vm/vm_object_paged.h>

#include <ktl/enforce.h>

namespace {

// Split out the pager vmo creation flags between PageSource and VmObjectPaged flags.
void split_syscall_flags(uint32_t flags, uint32_t* source_flags, uint32_t* vmo_flags) {
  // Extract any option flags relevant to creation of the page source.
  uint32_t src_flags = 0;
  if (flags & ZX_VMO_TRAP_DIRTY) {
    src_flags |= ZX_VMO_TRAP_DIRTY;
  }

  // Mask out any source flags. The remaining flags are the vmo creation flags; vmo creation will
  // perform validation on them.
  flags &= ~(ZX_VMO_TRAP_DIRTY);
  *vmo_flags = flags;
  *source_flags = src_flags;
}

}  // namespace

// zx_status_t zx_pager_create
zx_status_t sys_pager_create(uint32_t options, user_out_handle* out) {
  auto up = ProcessDispatcher::GetCurrent();

  zx_status_t status = up->EnforceBasicPolicy(ZX_POL_NEW_PAGER);
  if (status != ZX_OK) {
    return status;
  }

  if (options) {
    return ZX_ERR_INVALID_ARGS;
  }

  KernelHandle<PagerDispatcher> handle;
  zx_rights_t rights;
  status = PagerDispatcher::Create(&handle, &rights);
  if (status != ZX_OK) {
    return status;
  }

  return out->make(ktl::move(handle), rights);
}

// zx_status_t zx_pager_create_vmo
zx_status_t sys_pager_create_vmo(zx_handle_t pager, uint32_t options, zx_handle_t port,
                                 uint64_t key, uint64_t size, user_out_handle* out) {
  auto up = ProcessDispatcher::GetCurrent();

  zx_status_t status = up->EnforceBasicPolicy(ZX_POL_NEW_VMO);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<PagerDispatcher> pager_dispatcher;
  status = up->handle_table().GetDispatcher(*up, pager, &pager_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<PortDispatcher> port_dispatcher;
  status = up->handle_table().GetDispatcherWithRights(*up, port, ZX_RIGHT_WRITE, &port_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  uint32_t source_flags = 0;
  uint32_t vmo_flags = 0;
  split_syscall_flags(options, &source_flags, &vmo_flags);

  fbl::RefPtr<PageSource> src;
  status = pager_dispatcher->CreateSource(ktl::move(port_dispatcher), key, source_flags, &src);
  if (status != ZX_OK) {
    return status;
  }

  uint32_t vmo_options = 0;
  status = VmObjectDispatcher::parse_create_syscall_flags(vmo_flags, &vmo_options);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<VmObjectPaged> vmo;
  status = VmObjectPaged::CreateExternal(ktl::move(src), vmo_options, size, &vmo);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<ContentSizeManager> content_size_manager;
  status = ContentSizeManager::Create(size, &content_size_manager);
  if (status != ZX_OK) {
    return status;
  }

  KernelHandle<VmObjectDispatcher> kernel_handle;
  zx_rights_t rights;
  status = VmObjectDispatcher::Create(
      vmo, ktl::move(content_size_manager), pager_dispatcher->get_koid(),
      VmObjectDispatcher::InitialMutability::kMutable, &kernel_handle, &rights);
  if (status != ZX_OK) {
    return status;
  }

  return out->make(ktl::move(kernel_handle), rights);
}

// zx_status_t zx_pager_detach_vmo
zx_status_t sys_pager_detach_vmo(zx_handle_t pager, zx_handle_t vmo) {
  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<PagerDispatcher> pager_dispatcher;
  // TODO: Consider rights on the pager dispatcher.
  zx_status_t status = up->handle_table().GetDispatcher(*up, pager, &pager_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<VmObjectDispatcher> vmo_dispatcher;
  status = up->handle_table().GetDispatcher(*up, vmo, &vmo_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  if (vmo_dispatcher->pager_koid() != pager_dispatcher->get_koid()) {
    return ZX_ERR_INVALID_ARGS;
  }

  vmo_dispatcher->vmo()->DetachSource();
  return ZX_OK;
}

// zx_status_t zx_pager_supply_pages
zx_status_t sys_pager_supply_pages(zx_handle_t pager, zx_handle_t pager_vmo, uint64_t offset,
                                   uint64_t size, zx_handle_t aux_vmo_handle, uint64_t aux_offset) {
  if (!IS_PAGE_ALIGNED(offset) || !IS_PAGE_ALIGNED(size) || !IS_PAGE_ALIGNED(aux_offset)) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<PagerDispatcher> pager_dispatcher;
  zx_status_t status = up->handle_table().GetDispatcher(*up, pager, &pager_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<VmObjectDispatcher> pager_vmo_dispatcher;
  status = up->handle_table().GetDispatcher(*up, pager_vmo, &pager_vmo_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  if (pager_vmo_dispatcher->pager_koid() != pager_dispatcher->get_koid()) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::RefPtr<VmObjectDispatcher> aux_vmo_dispatcher;
  status = up->handle_table().GetDispatcherWithRights(
      *up, aux_vmo_handle, ZX_RIGHT_READ | ZX_RIGHT_WRITE, &aux_vmo_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  VmPageSpliceList pages;
  status = aux_vmo_dispatcher->vmo()->TakePages(aux_offset, size, &pages);
  if (status != ZX_OK) {
    return status;
  }

  return pager_vmo_dispatcher->vmo()->SupplyPages(offset, size, &pages);
}

// zx_status_t zx_pager_op_range
zx_status_t sys_pager_op_range(zx_handle_t pager, uint32_t op, zx_handle_t pager_vmo,
                               uint64_t offset, uint64_t length, uint64_t data) {
  if (!IS_PAGE_ALIGNED(offset) || !IS_PAGE_ALIGNED(length)) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<PagerDispatcher> pager_dispatcher;
  zx_status_t status = up->handle_table().GetDispatcher(*up, pager, &pager_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<VmObjectDispatcher> pager_vmo_dispatcher;
  status = up->handle_table().GetDispatcher(*up, pager_vmo, &pager_vmo_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  if (pager_vmo_dispatcher->pager_koid() != pager_dispatcher->get_koid()) {
    return ZX_ERR_INVALID_ARGS;
  }

  return pager_dispatcher->RangeOp(op, pager_vmo_dispatcher->vmo(), offset, length, data);
}

// zx_status_t zx_pager_query_dirty_ranges
zx_status_t sys_pager_query_dirty_ranges(zx_handle_t pager, zx_handle_t pager_vmo, uint64_t offset,
                                         uint64_t length, user_out_ptr<void> buffer,
                                         size_t buffer_size, user_out_ptr<size_t> actual,
                                         user_out_ptr<size_t> avail) {
  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<PagerDispatcher> pager_dispatcher;
  zx_status_t status = up->handle_table().GetDispatcher(*up, pager, &pager_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<VmObjectDispatcher> pager_vmo_dispatcher;
  status = up->handle_table().GetDispatcher(*up, pager_vmo, &pager_vmo_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  if (pager_vmo_dispatcher->pager_koid() != pager_dispatcher->get_koid()) {
    return ZX_ERR_INVALID_ARGS;
  }

  return pager_dispatcher->QueryDirtyRanges(Thread::Current::Get()->aspace(),
                                            pager_vmo_dispatcher->vmo(), offset, length, buffer,
                                            buffer_size, actual, avail);
}

// zx_status_t zx_pager_query_vmo_stats
zx_status_t sys_pager_query_vmo_stats(zx_handle_t pager, zx_handle_t pager_vmo, uint32_t options,
                                      user_out_ptr<void> buffer, size_t buffer_size) {
  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<PagerDispatcher> pager_dispatcher;
  zx_status_t status = up->handle_table().GetDispatcher(*up, pager, &pager_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<VmObjectDispatcher> pager_vmo_dispatcher;
  status = up->handle_table().GetDispatcher(*up, pager_vmo, &pager_vmo_dispatcher);
  if (status != ZX_OK) {
    return status;
  }

  if (pager_vmo_dispatcher->pager_koid() != pager_dispatcher->get_koid()) {
    return ZX_ERR_INVALID_ARGS;
  }

  return pager_dispatcher->QueryPagerVmoStats(
      Thread::Current::Get()->aspace(), pager_vmo_dispatcher->vmo(), options, buffer, buffer_size);
}
