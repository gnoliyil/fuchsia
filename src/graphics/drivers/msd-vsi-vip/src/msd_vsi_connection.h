// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MSD_VSI_CONNECTION_H
#define MSD_VSI_CONNECTION_H

#include <lib/fit/thread_safety.h>

#include <memory>

#include "address_space.h"
#include "magma_util/macros.h"
#include "mapped_batch.h"
#include "msd.h"
#include "ringbuffer.h"

class MsdVsiConnection {
 public:
  class Owner {
   public:
    virtual Ringbuffer* GetRingbuffer() = 0;

    // If |do_flush| is true, a flush TLB command will be queued before the batch commands.
    virtual magma::Status SubmitBatch(std::unique_ptr<MappedBatch> batch, bool do_flush) = 0;
  };

  MsdVsiConnection(Owner* owner, std::shared_ptr<AddressSpace> address_space,
                   msd_client_id_t client_id)
      : owner_(owner), address_space_(std::move(address_space)), client_id_(client_id) {}

  magma::Status MapBufferGpu(std::shared_ptr<MsdVsiBuffer> buffer, uint64_t gpu_va,
                             uint64_t page_offset, uint64_t page_count);

  Ringbuffer* GetRingbuffer() { return owner_->GetRingbuffer(); }

  magma::Status SubmitBatch(std::unique_ptr<MappedBatch> mapped_batch, bool do_flush = false) {
    do_flush |= address_space_dirty_;
    address_space_dirty_ = false;
    return owner_->SubmitBatch(std::move(mapped_batch), do_flush);
  }

  bool address_space_dirty() { return address_space_dirty_; }

  void SetNotificationCallback(msd_connection_notification_callback_t callback, void* token) {
    notifications_.Set(callback, token);
  }

  void SendContextKilled() { notifications_.SendContextKilled(); }

  bool ReleaseMapping(magma::PlatformBuffer* buffer, uint64_t gpu_va);
  void ReleaseBuffer(magma::PlatformBuffer* buffer);

  // Submit pending release mappings on the given context
  bool SubmitPendingReleaseMappings(std::shared_ptr<MsdVsiContext> context);

  msd_client_id_t client_id() { return client_id_; }

  std::shared_ptr<AddressSpace> address_space() { return address_space_; }

 private:
  // Saves the released bus mappings to |mappings_to_release_|, to be transferred to the next
  // created |MappingReleaseBatch|.
  // Sends a ContextKilled notification if a mapping is still in use.
  void QueueReleasedMappings(std::vector<std::shared_ptr<GpuMapping>> mappings);

  const std::vector<std::unique_ptr<magma::PlatformBusMapper::BusMapping>>& mappings_to_release()
      const {
    return mappings_to_release_;
  }

  Owner* owner_;
  std::shared_ptr<AddressSpace> address_space_;
  msd_client_id_t client_id_;

  std::vector<std::unique_ptr<magma::PlatformBusMapper::BusMapping>> mappings_to_release_;
  bool address_space_dirty_ = false;

  class Notifications {
   public:
    void SendContextKilled() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (callback_ && token_) {
        msd_notification_t notification = {};
        notification.type = MSD_CONNECTION_NOTIFICATION_CONTEXT_KILLED;
        callback_(token_, &notification);
      }
    }

    void Set(msd_connection_notification_callback_t callback, void* token) {
      std::lock_guard<std::mutex> lock(mutex_);
      callback_ = callback;
      token_ = token;
    }

   private:
    FIT_GUARDED(mutex_) msd_connection_notification_callback_t callback_ = nullptr;
    FIT_GUARDED(mutex_) void* token_ = nullptr;
    std::mutex mutex_;
  };

  Notifications notifications_;

  friend class TestMsdVsiConnection_ReleaseMapping_Test;
  friend class TestMsdVsiConnection_ReleaseBuffer_Test;
  friend class TestMsdVsiConnection_ReleaseBufferWhileMapped_Test;
};

class MsdVsiAbiConnection : public msd_connection_t {
 public:
  MsdVsiAbiConnection(std::shared_ptr<MsdVsiConnection> ptr) : ptr_(std::move(ptr)) {
    magic_ = kMagic;
  }

  static MsdVsiAbiConnection* cast(msd_connection_t* connection) {
    DASSERT(connection);
    DASSERT(connection->magic_ == kMagic);
    return static_cast<MsdVsiAbiConnection*>(connection);
  }

  std::shared_ptr<MsdVsiConnection> ptr() { return ptr_; }

 private:
  std::shared_ptr<MsdVsiConnection> ptr_;
  static const uint32_t kMagic = 0x636f6e6e;  // "conn" (Connection)
};

#endif  // MSD_VSI_CONNECTION_H
