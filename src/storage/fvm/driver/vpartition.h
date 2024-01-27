// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FVM_DRIVER_VPARTITION_H_
#define SRC_STORAGE_FVM_DRIVER_VPARTITION_H_

#include <fuchsia/hardware/block/driver/cpp/banjo.h>
#include <fuchsia/hardware/block/partition/cpp/banjo.h>
#include <fuchsia/hardware/block/volume/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <zircon/types.h>

#include <cstdint>
#include <memory>

#include <ddktl/device.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/mutex.h>

#include "src/storage/fvm/driver/slice_extent.h"

namespace fvm {

// Forward Declaration
class VPartitionManager;
class VPartition;

using PartitionDeviceType = ddk::Device<VPartition, ddk::GetProtocolable>;

class VPartition : public PartitionDeviceType,
                   public ddk::BlockImplProtocol<VPartition, ddk::base_protocol>,
                   public ddk::BlockPartitionProtocol<VPartition>,
                   public ddk::BlockVolumeProtocol<VPartition> {
 public:
  using SliceMap = fbl::WAVLTree<uint64_t, std::unique_ptr<SliceExtent>>;

  static zx_status_t Create(VPartitionManager* vpm, size_t entry_index,
                            std::unique_ptr<VPartition>* out);
  // Device Protocol
  // TODO(https://fxbug.dev/126961): NOTE!! We are currently reliant on VPartition NOT implementing
  // DdkInit to ensure that child partitions of the VPartitionManager are visible in devfs on the
  // return of GetInfo(). If VPartition implements DdkInit, we can no longer rely on the
  // completion of GetInfo() to know when it is safe to enumerate child partitions in devfs.
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);
  void DdkRelease();

  // Block Protocol
  void BlockImplQuery(block_info_t* info_out, size_t* block_op_size_out);
  void BlockImplQueue(block_op_t* txn, block_impl_queue_callback completion_cb, void* cookie);

  // Partition Protocol
  zx_status_t BlockPartitionGetGuid(guidtype_t guid_type, guid_t* out_guid);
  zx_status_t BlockPartitionGetName(char* out_name, size_t capacity);

  // Volume Protocol
  zx_status_t BlockVolumeExtend(const slice_extent_t* extent);
  zx_status_t BlockVolumeShrink(const slice_extent_t* extent);
  zx_status_t BlockVolumeGetInfo(volume_manager_info_t* out_manager, volume_info_t* out_volume);
  zx_status_t BlockVolumeQuerySlices(const uint64_t* start_list, size_t start_count,
                                     slice_region_t* out_responses_list, size_t responses_count,
                                     size_t* out_responses_actual);
  zx_status_t BlockVolumeDestroy();
  SliceMap::iterator ExtentBegin() TA_REQ(lock_) { return slice_map_.begin(); }

  // Returns true if the respective |vslice| is mapped to a physical slice, and sets |*pslice| to
  // the mapped physical slice. Returns false if the |vslice| is unallocated.
  bool SliceGetLocked(uint64_t vslice, uint64_t* out_pslice) const TA_REQ(lock_);

  // Check slices starting from |vslice_start|.
  // Sets |*count| to the number of contiguous allocated or unallocated slices found.
  // Sets |*allocated| to true if the vslice range is allocated, and false otherwise.
  zx_status_t CheckSlices(uint64_t vslice_start, size_t* count, bool* allocated) TA_EXCL(lock_);

  void SliceSetUnsafe(uint64_t vslice, uint64_t pslice) TA_NO_THREAD_SAFETY_ANALYSIS {
    SliceSetLocked(vslice, pslice);
  }
  // Maps the respective |vslice| to the given |pslice| and it will be considered as allocated.
  void SliceSetLocked(uint64_t vslice, uint64_t pslice) TA_REQ(lock_);

  // Returns true if the respective vslice is considered allocated by this partition.
  bool SliceCanFree(uint64_t vslice) const TA_REQ(lock_) {
    auto extent = --slice_map_.upper_bound(vslice);
    return extent.IsValid() && extent->contains(vslice);
  }

  // Marks |vslice| free from this partition. |vslice| is required to be allocated in the
  // partition, users should call SliceCanFree(vslice) before calling this method.
  void SliceFreeLocked(uint64_t vslice) TA_REQ(lock_);

  // Returns the number of slices which are assigned to the vpartition.
  size_t NumSlicesLocked() TA_REQ(lock_);

  // Destroy the extent containing the vslice.
  void ExtentDestroyLocked(uint64_t vslice) TA_REQ(lock_);

  size_t BlockSize() const TA_NO_THREAD_SAFETY_ANALYSIS { return info_.block_size; }
  void AddBlocksLocked(ssize_t nblocks) TA_REQ(lock_) { info_.block_count += nblocks; }

  size_t entry_index() const { return entry_index_; }

  void KillLocked() TA_REQ(lock_) { entry_index_ = 0; }
  bool IsKilledLocked() const TA_REQ(lock_) { return entry_index_ == 0; }

  VPartition(VPartitionManager* vpm, size_t entry_index, size_t block_op_size);
  ~VPartition();
  fbl::Mutex lock_;

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(VPartition);

  zx_device_t* GetParent() const;

  VPartitionManager* mgr_;
  size_t entry_index_;

  // Mapping of virtual slice number (index) to physical slice number (value).
  // Physical slice zero is reserved to mean "unmapped", so a zeroed slice_map
  // indicates that the vpartition is completely unmapped, and uses no
  // physical slices.
  SliceMap slice_map_ TA_GUARDED(lock_);
  block_info_t info_ TA_GUARDED(lock_);
};

}  // namespace fvm

#endif  // SRC_STORAGE_FVM_DRIVER_VPARTITION_H_
