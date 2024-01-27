// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <inttypes.h>
#include <lib/syscalls/forward.h>
#include <lib/user_copy/user_iovec.h>
#include <lib/user_copy/user_ptr.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <fbl/ref_ptr.h>
#include <object/handle.h>
#include <object/stream_dispatcher.h>
#include <vm/vm_aspace.h>

#define LOCAL_TRACE 0

// zx_status_t zx_stream_create
zx_status_t sys_stream_create(uint32_t options, zx_handle_t vmo_handle, zx_off_t seek,
                              user_out_handle* out_stream) {
  if ((options & ~ZX_STREAM_CREATE_MASK) != 0)
    return ZX_ERR_INVALID_ARGS;

  uint32_t stream_options = 0;
  zx_rights_t desired_vmo_rights = ZX_RIGHT_NONE;
  zx_status_t status =
      StreamDispatcher::parse_create_syscall_flags(options, &stream_options, &desired_vmo_rights);
  if (status != ZX_OK) {
    return status;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<VmObjectDispatcher> vmo;
  status = up->handle_table().GetDispatcherWithRights(*up, vmo_handle, desired_vmo_rights, &vmo);
  if (status != ZX_OK)
    return status;

  KernelHandle<StreamDispatcher> new_handle;
  zx_rights_t rights;
  status = StreamDispatcher::Create(stream_options, ktl::move(vmo), seek, &new_handle, &rights);
  if (status != ZX_OK)
    return status;
  return out_stream->make(ktl::move(new_handle), rights);
}

// zx_status_t zx_stream_writev
zx_status_t sys_stream_writev(zx_handle_t handle, uint32_t options,
                              user_in_ptr<const zx_iovec_t> vector, size_t vector_count,
                              user_out_ptr<size_t> out_actual) {
  LTRACEF("handle %x\n", handle);

  if (options & ~ZX_STREAM_APPEND) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!vector) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<StreamDispatcher> stream;
  zx_status_t status =
      up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_WRITE, &stream);
  if (status != ZX_OK) {
    return status;
  }

  size_t actual = 0;
  if (options & ZX_STREAM_APPEND) {
    status = stream->AppendVector(Thread::Current::Get()->aspace(),
                                  make_user_in_iovec(vector, vector_count), &actual);
  } else {
    status = stream->WriteVector(Thread::Current::Get()->aspace(),
                                 make_user_in_iovec(vector, vector_count), &actual);
  }

  if (status == ZX_OK && out_actual) {
    status = out_actual.copy_to_user(actual);
  }

  return status;
}

// zx_status_t zx_stream_writev_at
zx_status_t sys_stream_writev_at(zx_handle_t handle, uint32_t options, zx_off_t offset,
                                 user_in_ptr<const zx_iovec_t> vector, size_t vector_count,
                                 user_out_ptr<size_t> out_actual) {
  LTRACEF("handle %x\n", handle);

  if (options != 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!vector) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<StreamDispatcher> stream;
  zx_status_t status =
      up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_WRITE, &stream);
  if (status != ZX_OK) {
    return status;
  }

  size_t actual = 0;
  status = stream->WriteVectorAt(Thread::Current::Get()->aspace(),
                                 make_user_in_iovec(vector, vector_count), offset, &actual);

  if (status == ZX_OK && out_actual) {
    status = out_actual.copy_to_user(actual);
  }

  return status;
}

// zx_status_t zx_stream_readv
zx_status_t sys_stream_readv(zx_handle_t handle, uint32_t options, user_out_ptr<zx_iovec_t> vector,
                             size_t vector_count, user_out_ptr<size_t> out_actual) {
  LTRACEF("handle %x\n", handle);

  if (options != 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!vector) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<StreamDispatcher> stream;
  zx_status_t status =
      up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_READ, &stream);
  if (status != ZX_OK) {
    return status;
  }

  size_t actual = 0;
  status = stream->ReadVector(Thread::Current::Get()->aspace(),
                              make_user_out_iovec(vector, vector_count), &actual);

  if (status == ZX_OK && out_actual) {
    status = out_actual.copy_to_user(actual);
  }

  return status;
}

// zx_status_t zx_stream_readv_at
zx_status_t sys_stream_readv_at(zx_handle_t handle, uint32_t options, zx_off_t offset,
                                user_out_ptr<zx_iovec_t> vector, size_t vector_count,
                                user_out_ptr<size_t> out_actual) {
  LTRACEF("handle %x\n", handle);

  if (options != 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!vector) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<StreamDispatcher> stream;
  zx_status_t status =
      up->handle_table().GetDispatcherWithRights(*up, handle, ZX_RIGHT_READ, &stream);
  if (status != ZX_OK) {
    return status;
  }

  size_t actual = 0;
  status = stream->ReadVectorAt(Thread::Current::Get()->aspace(),
                                make_user_out_iovec(vector, vector_count), offset, &actual);

  if (status == ZX_OK && out_actual) {
    status = out_actual.copy_to_user(actual);
  }

  return status;
}

// zx_status_t zx_stream_seek
zx_status_t sys_stream_seek(zx_handle_t handle, zx_stream_seek_origin_t whence, int64_t offset,
                            user_out_ptr<zx_off_t> out_seek) {
  LTRACEF("handle %x\n", handle);

  auto up = ProcessDispatcher::GetCurrent();
  fbl::RefPtr<StreamDispatcher> stream;
  zx_rights_t rights;
  zx_status_t status = up->handle_table().GetDispatcherAndRights(*up, handle, &stream, &rights);
  if (status != ZX_OK) {
    return status;
  }
  if ((rights & (ZX_RIGHT_READ | ZX_RIGHT_WRITE)) == 0) {
    return ZX_ERR_ACCESS_DENIED;
  }
  zx_off_t seek = 0u;
  status = stream->Seek(whence, offset, &seek);

  if (status == ZX_OK && out_seek) {
    status = out_seek.copy_to_user(seek);
  }
  return status;
}
