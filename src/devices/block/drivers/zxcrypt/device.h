// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_ZXCRYPT_DEVICE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_ZXCRYPT_DEVICE_H_

#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <fuchsia/hardware/block/driver/cpp/banjo.h>
#include <fuchsia/hardware/block/partition/cpp/banjo.h>
#include <fuchsia/hardware/block/volume/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <lib/zx/port.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/compiler.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

#include <atomic>

#include <bitmap/raw-bitmap.h>
#include <bitmap/storage.h>
#include <ddktl/device.h>
#include <fbl/macros.h>
#include <fbl/mutex.h>

#include "lib/inspect/cpp/inspector.h"
#include "src/devices/block/drivers/zxcrypt/device-info.h"
#include "src/devices/block/drivers/zxcrypt/extra.h"
#include "src/devices/block/drivers/zxcrypt/queue.h"
#include "src/devices/block/drivers/zxcrypt/worker.h"

namespace zxcrypt {

// See ddk::Device in ddktl/device.h
class Device;
using DeviceType = ddk::Device<Device, ddk::GetProtocolable, ddk::Unbindable>;

// |zxcrypt::Device| is an encrypted block device filter driver.  It is created by
// |zxcrypt::DeviceManager::Unseal| and transparently encrypts writes to/decrypts reads from a
// parent block device.  It shadows incoming requests and uses a mapped VMO as working memory for
// cryptographic transformations.
class Device final : public DeviceType,
                     public ddk::BlockImplProtocol<Device, ddk::base_protocol>,
                     public ddk::BlockPartitionProtocol<Device>,
                     public ddk::BlockVolumeProtocol<Device> {
 public:
  Device(zx_device_t* parent, DeviceInfo&& info, inspect::Node inspect);
  ~Device();

  // Publish some constants for the workers
  inline uint32_t block_size() const { return info_.block_size; }
  inline size_t op_size() const { return info_.op_size; }

  // The body of the |Init| thread.  This method uses the unsealed |volume| to start cryptographic
  // workers for normal operation.
  zx_status_t Init(const DdkVolume& volume) __TA_EXCLUDES(mtx_);

  // ddk::Device methods; see ddktl/device.h
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  // ddk::BlockProtocol methods; see fuchsia/hardware/block/driver/cpp/banjo.h
  void BlockImplQuery(block_info_t* out_info, size_t* out_op_size);
  void BlockImplQueue(block_op_t* block, block_impl_queue_callback completion_cb, void* cookie)
      __TA_EXCLUDES(mtx_);

  // ddk::PartitionProtocol methods; see fuchsia/hardware/block/partition/cpp/banjo.h
  zx_status_t BlockPartitionGetGuid(guidtype_t guidtype, guid_t* out_guid);
  zx_status_t BlockPartitionGetName(char* out_name, size_t capacity);

  // ddk:::VolumeProtocol methods; see fuchsia/hardware/block/volume/cpp/banjo.h
  zx_status_t BlockVolumeExtend(const slice_extent_t* extent);
  zx_status_t BlockVolumeShrink(const slice_extent_t* extent);
  zx_status_t BlockVolumeGetInfo(volume_manager_info_t* out_manager, volume_info_t* out_volume);
  zx_status_t BlockVolumeQuerySlices(const uint64_t* start_list, size_t start_count,
                                     slice_region_t* out_responses_list, size_t responses_count,
                                     size_t* out_responses_actual);
  zx_status_t BlockVolumeDestroy();

  // If |status| is |ZX_OK|, sends |block| to the parent block device; otherwise calls
  // |BlockComplete| on the |block|. Uses the extra space following the |block| to save fields
  // which may be modified, including the |completion_cb|, which it sets to |BlockCallback|.
  void BlockForward(block_op_t* block, zx_status_t status) __TA_EXCLUDES(mtx_);

  // Returns a completed |block| request to the caller of |BlockQueue|.
  void BlockComplete(block_op_t* block, zx_status_t status) __TA_EXCLUDES(mtx_);

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Device);

  // TODO(aarongreen): Investigate performance impact of changing this.
  // Number of encrypting/decrypting workers
  static const size_t kNumWorkers = 2;

  // Adds |block| to the write queue if not null, and sends to the workers as many write requests
  // as fit in the space available in the write buffer.
  void EnqueueWrite(block_op_t* block = nullptr) __TA_EXCLUDES(mtx_);

  // Callback used for block ops sent to the parent device.  Restores the fields saved by
  // |BlockForward|.
  static void BlockCallback(void* cookie, zx_status_t status, block_op_t* block);

  // Requests that the workers stop if it the device is inactive and no ops are "in-flight".
  void StopWorkersIfDone();

  // Set if device is active, i.e. |Init| has been called but |DdkUnbind| hasn't. I/O requests to
  // |BlockQueue| are immediately completed with |ZX_ERR_BAD_STATE| if this is not set.
  std::atomic_bool active_;

  // Set if writes are stalled, i.e.  a write request was deferred due to lack of space in the
  // write buffer, and no requests have since completed.
  std::atomic_bool stalled_;

  // the number of operations currently "in-flight".
  std::atomic_uint64_t num_ops_;

  // Device configuration, as provided by the DeviceManager at creation. It's "constness" allows
  // it to be used without holding the lock.
  const DeviceInfo info_;

  // The queue for requests to the workers. This *should* come before `workers_` since the workers
  // will hold a reference to the queue and should therefore be destroyed first.
  Queue<block_op_t*> worker_queue_;

  // Threads that performs encryption/decryption.
  Worker workers_[kNumWorkers];

  // Primary lock for accessing the write queue
  fbl::Mutex mtx_;

  // Indicates which blocks of the write buffer are in use.
  bitmap::RawBitmapGeneric<bitmap::DefaultStorage> map_ __TA_GUARDED(mtx_);

  // Describes a queue of deferred block requests.
  list_node_t queue_ __TA_GUARDED(mtx_);

  // inspect::Node tracking unsealed device GUID.
  inspect::Node inspect_;
  inspect::StringProperty instance_guid_;

  // Hint as to where in the bitmap to begin looking for available space.
  size_t hint_ __TA_GUARDED(mtx_);
};

}  // namespace zxcrypt

#endif  // SRC_DEVICES_BLOCK_DRIVERS_ZXCRYPT_DEVICE_H_
