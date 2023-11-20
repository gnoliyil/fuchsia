// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDMMC_BLOCK_DEVICE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDMMC_BLOCK_DEVICE_H_

#include <fidl/fuchsia.hardware.sdmmc/cpp/wire.h>
#include <fuchsia/hardware/block/driver/cpp/banjo.h>
#include <lib/driver/component/cpp/driver_base.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/inspect/component/cpp/component.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/operation/block.h>
#include <lib/sdmmc/hw.h>
#include <lib/trace/event.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <threads.h>
#include <zircon/types.h>

#include <array>
#include <atomic>
#include <cinttypes>
#include <deque>
#include <memory>
#include <semaphore>

#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>

#include "sdmmc-device.h"
#include "sdmmc-partition-device.h"
#include "sdmmc-rpmb-device.h"
#include "sdmmc-types.h"

namespace sdmmc {

class SdmmcRootDevice;

// This class serves two purposes:
// 1. Maintain metadata for IO while the IO is in progress: When using Banjo, this is a hard
//    requirement, as only object handles are passed through the call. When using FIDL, objects can
//    be transferred through the call. However for performance, we avoid repeatedly creating and
//    initializing objects.
// 2. Control the number of pending IOs in progress: When using synchronous Banjo, only one IO is
//    ever pending. When using asynchronous FIDL, restricting the number of pending IOs encourages
//    block operations to queue and form a large packed command (batch). Without this measure, the
//    throughput when using FIDL is as if command packing is disabled.
class ReadWriteMetadata {
 public:
  struct Entry {
    // For non-packed commands, only this is needed, as initialized here.
    std::unique_ptr<sdmmc_buffer_region_t[]> buffer_regions =
        std::make_unique<sdmmc_buffer_region_t[]>(1);

    // For packed commands, the following are also needed.
    zx::vmo packed_command_header_vmo;
    fzl::VmoMapper packed_command_header_mapper;
    PackedCommand* packed_command_header_data;
  };

  // This initialization is only required if packed commands are used.
  zx_status_t InitForPackedCommands(uint32_t buffer_region_count, uint32_t block_size) {
    for (int i = 0; i < kMaxPendingReadWrites; i++) {
      entries_[i].buffer_regions = std::make_unique<sdmmc_buffer_region_t[]>(buffer_region_count);
      memset(entries_[i].buffer_regions.get(), 0,
             sizeof(sdmmc_buffer_region_t) * buffer_region_count);

      zx_status_t status = zx::vmo::create(block_size, 0, &entries_[i].packed_command_header_vmo);
      if (status != ZX_OK) {
        FDF_LOG(ERROR, "Failed to create packed command header vmo: %s",
                zx_status_get_string(status));
        return status;
      }

      status = entries_[i].packed_command_header_mapper.Map(entries_[i].packed_command_header_vmo);
      if (status != ZX_OK) {
        FDF_LOG(ERROR, "Failed to map packed command header vmo: %s", zx_status_get_string(status));
        return status;
      }

      entries_[i].packed_command_header_data =
          static_cast<PackedCommand*>(entries_[i].packed_command_header_mapper.start());
      memset(entries_[i].packed_command_header_data, 0, block_size);
      entries_[i].packed_command_header_data->version = 1;
    }
    return ZX_OK;
  }

  Entry* WaitForEntry() {
    pending_readwrites_.acquire();
    Entry* entry = &entries_[entry_index_++];
    if (entry_index_ == kMaxPendingReadWrites) {
      entry_index_ = 0;
    }
    return entry;
  }

  // No need to say which entry, since IOs are handled in order.
  void DoneWithEntry() { pending_readwrites_.release(); }

 private:
  // Balanced between keeping the sdmmc server busy and encouraging command packing.
  static constexpr int kMaxPendingReadWrites = 3;

  std::counting_semaphore<> pending_readwrites_ = std::counting_semaphore<>(kMaxPendingReadWrites);

  Entry entries_[kMaxPendingReadWrites];
  int entry_index_ = 0;
};

class SdmmcBlockDevice {
 public:
  SdmmcBlockDevice(SdmmcRootDevice* parent, std::unique_ptr<SdmmcDevice> sdmmc)
      : parent_(parent), sdmmc_(std::move(sdmmc)) {
    block_info_.max_transfer_size = static_cast<uint32_t>(sdmmc_->host_info().max_transfer_size);
  }
  ~SdmmcBlockDevice() {
    StopWorkerThread();
    txn_list_.CompleteAll(ZX_ERR_INTERNAL);
  }

  static zx_status_t Create(SdmmcRootDevice* parent, std::unique_ptr<SdmmcDevice> sdmmc,
                            bool use_fidl, std::unique_ptr<SdmmcBlockDevice>* out_dev);
  // Returns the SdmmcDevice. Used if this SdmmcBlockDevice fails to probe (i.e., no eligible device
  // present).
  std::unique_ptr<SdmmcDevice> TakeSdmmcDevice() { return std::move(sdmmc_); }

  // Probe for SD first, then MMC.
  zx_status_t Probe(const fuchsia_hardware_sdmmc::wire::SdmmcMetadata& metadata) {
    return ProbeSd(metadata) == ZX_OK ? ZX_OK : ProbeMmc(metadata);
  }
  zx_status_t ProbeSd(const fuchsia_hardware_sdmmc::wire::SdmmcMetadata& metadata);
  zx_status_t ProbeMmc(const fuchsia_hardware_sdmmc::wire::SdmmcMetadata& metadata);

  zx_status_t AddDevice() TA_EXCL(lock_);

  // Called by children of this device.
  void Queue(BlockOperation txn) TA_EXCL(lock_);
  void RpmbQueue(RpmbRequestInfo info) TA_EXCL(lock_);
  fidl::WireSyncClient<fuchsia_driver_framework::Node>& block_node() { return block_node_; }
  std::string_view block_name() const { return block_name_; }
  SdmmcRootDevice* parent() { return parent_; }

  // Visible for testing.
  zx_status_t Init(bool use_fidl = false) { return sdmmc_->Init(use_fidl); }
  void StopWorkerThread() TA_EXCL(lock_);
  void SetBlockInfo(uint32_t block_size, uint64_t block_count);

 private:
  // An arbitrary limit to prevent RPMB clients from flooding us with requests.
  static constexpr size_t kMaxOutstandingRpmbRequests = 16;

  // The worker thread will handle this many block ops then this many RPMB requests, and will repeat
  // until both queues are empty.
  static constexpr size_t kRoundRobinRequestCount = 16;

  void ReadWrite(std::vector<BlockOperation>& btxns, EmmcPartition partition,
                 ReadWriteMetadata::Entry* entry);
  zx_status_t Flush();
  zx_status_t Trim(const block_trim_t& txn, const EmmcPartition partition);
  zx_status_t SetPartition(const EmmcPartition partition);
  zx_status_t RpmbRequest(const RpmbRequestInfo& request);

  void HandleBlockOps(block::BorrowedOperationQueue<PartitionInfo>& txn_list);
  void HandleRpmbRequests(std::deque<RpmbRequestInfo>& rpmb_list);

  int WorkerThread();

  zx_status_t WaitForTran();

  zx_status_t MmcDoSwitch(uint8_t index, uint8_t value);
  zx_status_t MmcWaitForSwitch(uint8_t index, uint8_t value);
  zx_status_t MmcSetBusWidth(sdmmc_bus_width_t bus_width, uint8_t mmc_ext_csd_bus_width);
  sdmmc_bus_width_t MmcSelectBusWidth();
  // The host is expected to switch the timing from HS200 to HS as part of HS400 initialization.
  // Checking the status of the switch requires special handling to avoid a temporary mismatch
  // between the host and device timings.
  zx_status_t MmcSwitchTiming(sdmmc_timing_t new_timing);
  zx_status_t MmcSwitchTimingHs200ToHs();
  zx_status_t MmcSwitchFreq(uint32_t new_freq);
  zx_status_t MmcDecodeExtCsd();
  bool MmcSupportsHs();
  bool MmcSupportsHsDdr();
  bool MmcSupportsHs200();
  bool MmcSupportsHs400();
  void MmcSetInspectProperties();

  SdmmcRootDevice* const parent_;
  std::unique_ptr<SdmmcDevice> sdmmc_;  // Only accessed by ProbeSd, ProbeMmc, and WorkerThread.

  sdmmc_bus_width_t bus_width_;
  sdmmc_timing_t timing_;

  uint32_t clock_rate_;  // Bus clock rate

  // mmc
  std::array<uint8_t, SDMMC_CID_SIZE> raw_cid_;
  std::array<uint8_t, SDMMC_CSD_SIZE> raw_csd_;
  std::array<uint8_t, MMC_EXT_CSD_SIZE> raw_ext_csd_;

  fbl::Mutex lock_;
  fbl::ConditionVariable worker_event_ TA_GUARDED(lock_);

  // blockio requests
  block::BorrowedOperationQueue<PartitionInfo> txn_list_ TA_GUARDED(lock_);
  std::deque<RpmbRequestInfo> rpmb_list_ TA_GUARDED(lock_);

  thrd_t worker_thread_ = 0;

  std::atomic<bool> dead_ = false;

  block_info_t block_info_{};

  bool is_sd_ = false;
  bool cache_enabled_ = false;

  uint32_t max_packed_reads_effective_ = 0;   // Use command packing up to this many reads.
  uint32_t max_packed_writes_effective_ = 0;  // Use command packing up to this many writes.
  ReadWriteMetadata readwrite_metadata_;

  inspect::Inspector inspector_;
  inspect::Node root_;
  struct InspectProperties {
    inspect::UintProperty io_errors_;                    // Only updated from the worker thread.
    inspect::UintProperty io_retries_;                   // Only updated from the worker thread.
    inspect::UintProperty type_a_lifetime_used_;         // Set once by the init thread.
    inspect::UintProperty type_b_lifetime_used_;         // Set once by the init thread.
    inspect::UintProperty max_lifetime_used_;            // Set once by the init thread.
    inspect::UintProperty cache_size_bits_;              // Set once by the init thread.
    inspect::BoolProperty cache_enabled_;                // Set once by the init thread.
    inspect::BoolProperty trim_enabled_;                 // Set once by the init thread.
    inspect::UintProperty max_packed_reads_;             // Set once by the init thread.
    inspect::UintProperty max_packed_writes_;            // Set once by the init thread.
    inspect::UintProperty max_packed_reads_effective_;   // Set once by the init thread.
    inspect::UintProperty max_packed_writes_effective_;  // Set once by the init thread.
    inspect::BoolProperty using_fidl_;                   // Set once by the init thread.
  } properties_;

  std::optional<inspect::ComponentInspector> exposed_inspector_;

  std::string_view block_name_;
  fidl::WireSyncClient<fuchsia_driver_framework::Node> block_node_;
  fidl::WireSyncClient<fuchsia_driver_framework::NodeController> controller_;

  std::vector<std::unique_ptr<PartitionDevice>> child_partition_devices_;
  std::unique_ptr<RpmbDevice> child_rpmb_device_;
};

}  // namespace sdmmc

#endif  // SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDMMC_BLOCK_DEVICE_H_
