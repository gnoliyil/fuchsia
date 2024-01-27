// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_STREAM_DISPATCHER_H_
#define ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_STREAM_DISPATCHER_H_

#include <lib/user_copy/user_iovec.h>
#include <zircon/types.h>

#include <fbl/ref_counted.h>
#include <object/dispatcher.h>
#include <object/handle.h>
#include <object/vm_object_dispatcher.h>
#include <vm/content_size_manager.h>
#include <vm/vm_aspace.h>

class StreamDispatcher final : public SoloDispatcher<StreamDispatcher, ZX_DEFAULT_STREAM_RIGHTS> {
 public:
  static constexpr uint32_t kModeRead = (1u << 0);
  static constexpr uint32_t kModeWrite = (1u << 1);
  static constexpr uint32_t kModeAppend = (1u << 2);
  static constexpr uint32_t kCanResizeVmo = (1u << 3);

  static zx_status_t parse_create_syscall_flags(uint32_t flags, uint32_t* out_flags,
                                                zx_rights_t* out_required_vmo_rights);

  static zx_status_t Create(uint32_t options, fbl::RefPtr<VmObjectDispatcher> vmo, zx_off_t seek,
                            KernelHandle<StreamDispatcher>* handle, zx_rights_t* rights);
  ~StreamDispatcher();

  zx_obj_type_t get_type() const { return ZX_OBJ_TYPE_STREAM; }

  zx_status_t ReadVector(VmAspace* current_aspace, user_out_iovec_t user_data, size_t* out_actual);
  zx_status_t ReadVectorAt(VmAspace* current_aspace, user_out_iovec_t user_data, zx_off_t offset,
                           size_t* out_actual);
  zx_status_t WriteVector(VmAspace* current_aspace, user_in_iovec_t user_data, size_t* out_actual);
  zx_status_t WriteVectorAt(VmAspace* current_aspace, user_in_iovec_t user_data, zx_off_t offset,
                            size_t* out_actual);
  zx_status_t AppendVector(VmAspace* current_aspace, user_in_iovec_t user_data, size_t* out_actual);
  zx_status_t Seek(zx_stream_seek_origin_t whence, int64_t offset, zx_off_t* out_seek);
  zx_status_t SetAppendMode(bool value);
  bool IsInAppendMode() const;
  bool CanResizeVmo() const;
  void GetInfo(zx_info_stream_t* info) const;

 private:
  explicit StreamDispatcher(uint32_t options, fbl::RefPtr<VmObjectDispatcher> vmo, zx_off_t seek);

  zx_status_t CreateWriteOpAndExpandVmo(size_t total_capacity, zx_off_t offset,
                                        uint64_t* out_length,
                                        ktl::optional<uint64_t>* out_prev_content_size,
                                        ContentSizeManager::Operation* out_op);

  uint32_t options_ TA_GUARDED(get_lock());

  const fbl::RefPtr<VmObjectDispatcher> vmo_;

  // The seek_lock_ is used to make vmo_ operations and updates to seek atomic.
  mutable DECLARE_MUTEX(StreamDispatcher, lockdep::LockFlagsActiveListDisabled) seek_lock_;
  zx_off_t seek_ TA_GUARDED(seek_lock_) = 0u;
};

#endif  // ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_STREAM_DISPATCHER_H_
