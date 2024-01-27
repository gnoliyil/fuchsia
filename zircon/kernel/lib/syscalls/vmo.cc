// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <inttypes.h>
#include <lib/fit/defer.h>
#include <lib/syscalls/forward.h>
#include <lib/user_copy/user_ptr.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <fbl/ref_ptr.h>
#include <object/handle.h>
#include <object/process_dispatcher.h>
#include <object/resource.h>
#include <object/vm_object_dispatcher.h>
#include <vm/vm_object.h>
#include <vm/vm_object_paged.h>

#define LOCAL_TRACE 0

static_assert(ZX_CACHE_POLICY_CACHED == ARCH_MMU_FLAG_CACHED,
              "Cache policy constant mismatch - CACHED");
static_assert(ZX_CACHE_POLICY_UNCACHED == ARCH_MMU_FLAG_UNCACHED,
              "Cache policy constant mismatch - UNCACHED");
static_assert(ZX_CACHE_POLICY_UNCACHED_DEVICE == ARCH_MMU_FLAG_UNCACHED_DEVICE,
              "Cache policy constant mismatch - UNCACHED_DEVICE");
static_assert(ZX_CACHE_POLICY_WRITE_COMBINING == ARCH_MMU_FLAG_WRITE_COMBINING,
              "Cache policy constant mismatch - WRITE_COMBINING");
static_assert(ZX_CACHE_POLICY_MASK == ARCH_MMU_FLAG_CACHE_MASK,
              "Cache policy constant mismatch - CACHE_MASK");

// zx_status_t zx_vmo_create
zx_status_t sys_vmo_create(uint64_t size, uint32_t options, user_out_handle* out) {
  LTRACEF("size %#" PRIx64 "\n", size);

  auto up = ProcessDispatcher::GetCurrent();
  zx_status_t res = up->EnforceBasicPolicy(ZX_POL_NEW_VMO);
  if (res != ZX_OK)
    return res;

  uint32_t vmo_options = 0;
  res = VmObjectDispatcher::parse_create_syscall_flags(options, &vmo_options);
  if (res != ZX_OK)
    return res;

  // create a vm object
  fbl::RefPtr<VmObjectPaged> vmo;
  res =
      VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY | PMM_ALLOC_FLAG_CAN_WAIT, vmo_options, size, &vmo);
  if (res != ZX_OK)
    return res;

  fbl::RefPtr<ContentSizeManager> content_size_manager;
  res = ContentSizeManager::Create(size, &content_size_manager);
  if (res != ZX_OK) {
    return res;
  }

  // create a Vm Object dispatcher
  KernelHandle<VmObjectDispatcher> kernel_handle;
  zx_rights_t rights;
  zx_status_t result = VmObjectDispatcher::Create(ktl::move(vmo), ktl::move(content_size_manager),
                                                  VmObjectDispatcher::InitialMutability::kMutable,
                                                  &kernel_handle, &rights);
  if (result != ZX_OK)
    return result;

  // create a handle and attach the dispatcher to it
  return out->make(ktl::move(kernel_handle), rights);
}

// zx_status_t zx_vmo_read
zx_status_t sys_vmo_read(zx_handle_t handle, user_out_ptr<void> _data, uint64_t offset,
                         size_t len) {
  LTRACEF("handle %x, data %p, offset %#" PRIx64 ", len %#zx\n", handle, _data.get(), offset, len);

  auto up = ProcessDispatcher::GetCurrent();

  // lookup the dispatcher from handle
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_status_t status = up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_READ, &vmo);
  if (status != ZX_OK)
    return status;

  return vmo->Read(Thread::Current::Get()->aspace(), _data.reinterpret<char>(), len, offset,
                   nullptr);
}

// zx_status_t zx_vmo_write
zx_status_t sys_vmo_write(zx_handle_t handle, user_in_ptr<const void> _data, uint64_t offset,
                          size_t len) {
  LTRACEF("handle %x, data %p, offset %#" PRIx64 ", len %#zx\n", handle, _data.get(), offset, len);

  auto up = ProcessDispatcher::GetCurrent();

  // lookup the dispatcher from handle
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_status_t status =
      up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_WRITE, &vmo);
  if (status != ZX_OK)
    return status;

  return vmo->Write(Thread::Current::Get()->aspace(), _data.reinterpret<const char>(), len, offset,
                    nullptr);
}

// zx_status_t zx_vmo_get_size
zx_status_t sys_vmo_get_size(zx_handle_t handle, user_out_ptr<uint64_t> _size) {
  LTRACEF("handle %x, sizep %p\n", handle, _size.get());

  auto up = ProcessDispatcher::GetCurrent();

  // lookup the dispatcher from handle
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_status_t status = up->handle_table().GetDispatcher(*up, handle, &vmo);
  if (status != ZX_OK)
    return status;

  // no rights check, anyone should be able to get the size

  // do the operation
  uint64_t size = 0;
  status = vmo->GetSize(&size);

  if (status != ZX_OK)
    return status;

  return _size.copy_to_user(size);
}

// zx_status_t zx_vmo_set_size
zx_status_t sys_vmo_set_size(zx_handle_t handle, uint64_t size) {
  LTRACEF("handle %x, size %#" PRIx64 "\n", handle, size);

  auto up = ProcessDispatcher::GetCurrent();

  // lookup the dispatcher from handle
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_status_t status =
      up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_WRITE, &vmo);
  if (status != ZX_OK)
    return status;

  // do the operation
  return vmo->SetSize(size);
}

// zx_status_t zx_vmo_op_range
zx_status_t sys_vmo_op_range(zx_handle_t handle, uint32_t op, uint64_t offset, uint64_t size,
                             user_inout_ptr<void> _buffer, size_t buffer_size) {
  LTRACEF("handle %x op %u offset %#" PRIx64 " size %#" PRIx64 " buffer %p buffer_size %zu\n",
          handle, op, offset, size, _buffer.get(), buffer_size);

  auto up = ProcessDispatcher::GetCurrent();

  // lookup the dispatcher from handle
  // save the rights and pass down into the dispatcher for further testing
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_rights_t rights;
  zx_status_t status = up->handle_table().GetDispatcherAndRights(*up, handle, &vmo, &rights);
  if (status != ZX_OK) {
    return status;
  }

  return vmo->RangeOp(op, offset, size, _buffer, buffer_size, rights);
}

// zx_status_t zx_vmo_set_cache_policy
zx_status_t sys_vmo_set_cache_policy(zx_handle_t handle, uint32_t cache_policy) {
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_status_t status = ZX_OK;
  auto up = ProcessDispatcher::GetCurrent();

  // Sanity check the cache policy.
  if (cache_policy & ~ZX_CACHE_POLICY_MASK) {
    return ZX_ERR_INVALID_ARGS;
  }

  // lookup the dispatcher from handle.
  status = up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_MAP, &vmo);
  if (status != ZX_OK) {
    return status;
  }

  return vmo->SetMappingCachePolicy(cache_policy);
}

// zx_status_t zx_vmo_create_child
zx_status_t sys_vmo_create_child(zx_handle_t handle, uint32_t options, uint64_t offset,
                                 uint64_t size, user_out_handle* out_handle) {
  LTRACEF("handle %x options %#x offset %#" PRIx64 " size %#" PRIx64 "\n", handle, options, offset,
          size);

  auto up = ProcessDispatcher::GetCurrent();

  zx_status_t status;
  fbl::RefPtr<VmObject> child_vmo;
  bool no_write = false;

  // Resizing a VMO requires the WRITE permissions, but NO_WRITE forbids the WRITE permissions, as
  // such it does not make sense to create a VMO with both of these.
  if ((options & ZX_VMO_CHILD_NO_WRITE) && (options & ZX_VMO_CHILD_RESIZABLE)) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Writable is a property of the handle, not the object, so we consume this option here before
  // calling CreateChild.
  if (options & ZX_VMO_CHILD_NO_WRITE) {
    no_write = true;
    options &= ~ZX_VMO_CHILD_NO_WRITE;
  }

  // lookup the dispatcher from handle, save a copy of the rights for later. We must hold onto
  // the refptr of this VMO up until we create the dispatcher. The reason for this is that
  // VmObjectDispatcher::Create sets the user_id and page_attribution_id in the created child
  // vmo. Should the vmo destroyed between creating the child and setting the id in the dispatcher
  // the currently unset user_id may be used to re-attribute a parent. Holding the refptr prevents
  // any destruction from occurring.
  fbl::RefPtr<VmObjectDispatcher> vmo;
  zx_rights_t in_rights;
  status = up->handle_table().GetDispatcherWithRights(
      *up, handle, ZX_RIGHT_DUPLICATE | ZX_RIGHT_READ, &vmo, &in_rights);
  if (status != ZX_OK)
    return status;

  // clone the vmo into a new one
  status = vmo->CreateChild(options, offset, size, in_rights & ZX_RIGHT_GET_PROPERTY, &child_vmo);
  if (status != ZX_OK)
    return status;

  DEBUG_ASSERT(child_vmo);

  // This checks that the child VMO is explicitly created with ZX_VMO_CHILD_SNAPSHOT.
  // There are other ways that VMOs can be effectively immutable, for instance if the VMO is
  // created with ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE and meets certain criteria it will be
  // "upgraded" to a snapshot. However this behavior is not guaranteed at the API level.
  // A choice was made to conservatively only mark VMOs as immutable when the user explicitly
  // creates a VMO in a way that is guaranteed at the API level to always output an immutable VMO.
  auto initial_mutability = VmObjectDispatcher::InitialMutability::kMutable;
  if (no_write && (options & ZX_VMO_CHILD_SNAPSHOT)) {
    initial_mutability = VmObjectDispatcher::InitialMutability::kImmutable;
  }

  fbl::RefPtr<ContentSizeManager> content_size_manager;
  status = ContentSizeManager::Create(size, &content_size_manager);
  if (status != ZX_OK) {
    return status;
  }

  // create a Vm Object dispatcher
  KernelHandle<VmObjectDispatcher> kernel_handle;
  zx_rights_t default_rights;
  zx_status_t result =
      VmObjectDispatcher::Create(ktl::move(child_vmo), ktl::move(content_size_manager),
                                 initial_mutability, &kernel_handle, &default_rights);
  if (result != ZX_OK)
    return result;

  // Set the rights to the new handle to no greater than the input
  // handle, and always allow GET/SET_PROPERTY so the user can set ZX_PROP_NAME on the new clone.
  // Unless it was explicitly requested to be removed, Write can be added to CoW clones at the
  // expense of executability.
  zx_rights_t rights = in_rights | ZX_RIGHT_GET_PROPERTY | ZX_RIGHT_SET_PROPERTY;
  if (no_write) {
    rights &= ~ZX_RIGHT_WRITE;
  } else if (options & (ZX_VMO_CHILD_SNAPSHOT | ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE)) {
    rights &= ~ZX_RIGHT_EXECUTE;
    rights |= ZX_RIGHT_WRITE;
  }

  // make sure we're somehow not elevating rights beyond what a new vmo should have
  DEBUG_ASSERT(((default_rights | ZX_RIGHT_EXECUTE) & rights) == rights);

  // create a handle and attach the dispatcher to it
  return out_handle->make(ktl::move(kernel_handle), rights);
}

// zx_status_t zx_vmo_replace_as_executable
zx_status_t sys_vmo_replace_as_executable(zx_handle_t handle, zx_handle_t vmex,
                                          user_out_handle* out) {
  LTRACEF("repexec %x %x\n", handle, vmex);

  auto up = ProcessDispatcher::GetCurrent();

  zx_status_t vmex_status = ZX_OK;
  if (vmex != ZX_HANDLE_INVALID) {
    vmex_status = validate_ranged_resource(vmex, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_VMEX_BASE, 1);
  } else {
    vmex_status = up->EnforceBasicPolicy(ZX_POL_AMBIENT_MARK_VMO_EXEC);
  }

  Guard<BrwLockPi, BrwLockPi::Writer> guard{up->handle_table().get_lock()};
  auto source = up->handle_table().GetHandleLocked(*up, handle);
  if (!source)
    return ZX_ERR_BAD_HANDLE;

  auto handle_cleanup = fit::defer([up, source]() TA_NO_THREAD_SAFETY_ANALYSIS {
    up->handle_table().RemoveHandleLocked(source);
  });

  if (vmex_status != ZX_OK)
    return vmex_status;
  if (source->dispatcher()->get_type() != ZX_OBJ_TYPE_VMO)
    return ZX_ERR_BAD_HANDLE;

  return out->dup(source, source->rights() | ZX_RIGHT_EXECUTE);
}
