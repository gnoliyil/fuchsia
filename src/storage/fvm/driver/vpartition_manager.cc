// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fvm/driver/vpartition_manager.h"

#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fit/defer.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/vmo.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <utility>

#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <safemath/clamped_math.h>

#include "src/lib/uuid/uuid.h"
#include "src/storage/fvm/driver/slice_extent.h"
#include "src/storage/fvm/driver/vpartition.h"
#include "src/storage/fvm/format.h"
#include "src/storage/fvm/fvm.h"
#include "src/storage/fvm/metadata.h"
#include "src/storage/fvm/vmo_metadata_buffer.h"

namespace fvm {
namespace {

zx_status_t FvmLoadThread(void* arg) {
  return reinterpret_cast<fvm::VPartitionManager*>(arg)->Load();
}

// Our GUIDs come from several places which all must agree on the size.
static_assert(kGuidSize == BLOCK_GUID_LEN);

// Comparison for Guids. Each pointer should be a buffer of kGuidSize length.
bool GuidsEqual(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, kGuidSize) == 0; }

}  // namespace

VPartitionManager::VPartitionManager(zx_device_t* parent, const block_info_t& info,
                                     size_t block_op_size, const block_impl_protocol_t* bp)
    : ManagerDeviceType(parent), info_(info), block_op_size_(block_op_size) {
  memcpy(&bp_, bp, sizeof(*bp));
}

VPartitionManager::~VPartitionManager() = default;

// static
zx_status_t VPartitionManager::Bind(void* /*unused*/, zx_device_t* dev) {
  block_info_t block_info;
  block_impl_protocol_t bp;
  size_t block_op_size = 0;
  if (device_get_protocol(dev, ZX_PROTOCOL_BLOCK, &bp) != ZX_OK) {
    zxlogf(ERROR, "block device: does not support block protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }
  bp.ops->query(bp.ctx, &block_info, &block_op_size);

  auto vpm = std::make_unique<VPartitionManager>(dev, block_info, block_op_size, &bp);

  zx_status_t status = vpm->DdkAdd(ddk::DeviceAddArgs("fvm")
                                       .set_flags(DEVICE_ADD_NON_BINDABLE)
                                       .set_inspect_vmo(vpm->diagnostics().DuplicateVmo()));
  if (status != ZX_OK) {
    zxlogf(ERROR, "block device: failed to DdkAdd: %s", zx_status_get_string(status));
    return status;
  }
  // The VPartitionManager object is owned by the DDK, now that it has been
  // added. It will be deleted when the device is released.
  [[maybe_unused]] auto ptr = vpm.release();
  return ZX_OK;
}

fvm::Header VPartitionManager::GetHeader() const {
  fbl::AutoLock lock(&const_cast<VPartitionManager*>(this)->lock_);
  return *GetHeaderLocked();
}

void VPartitionManager::DdkInit(ddk::InitTxn txn) {
  init_txn_ = std::move(txn);

  // Read vpartition table asynchronously.
  int rc = thrd_create_with_name(&initialization_thread_, FvmLoadThread, this, "fvm-init");
  if (rc < 0) {
    zxlogf(ERROR, "block device: Could not load initialization thread");
    sync_completion_signal(&worker_completed_);
    // This will schedule the device to be unbound.
    return init_txn_->Reply(ZX_ERR_NO_MEMORY);
  }
  // The initialization thread will reply to |init_txn_| once it is ready to make the
  // device visible and able to be unbound.
}

zx_status_t VPartitionManager::AddPartition(std::unique_ptr<VPartition> vp) {
  const std::string name =
      GetAllocatedVPartEntry(vp->entry_index())->name() + "-p-" + std::to_string(vp->entry_index());

  zx_status_t status;
  if ((status = vp->DdkAdd(name.c_str())) != ZX_OK) {
    return status;
  }
  fbl::AutoLock lock(&lock_);
  device_bound_at_entry_[vp->entry_index()] = true;

  // The VPartition object was added to the DDK and is now owned by it. It will be deleted when the
  // device is released.
  [[maybe_unused]] auto ptr = vp.release();
  return ZX_OK;
}

struct VpmIoCookie {
  std::atomic<size_t> num_txns;
  std::atomic<zx_status_t> status;
  sync_completion_t signal;
};

static void IoCallback(void* cookie, zx_status_t status, block_op_t* op) {
  VpmIoCookie* c = reinterpret_cast<VpmIoCookie*>(cookie);
  if (status != ZX_OK) {
    c->status.store(status);
  }
  if (c->num_txns.fetch_sub(1) - 1 == 0) {
    sync_completion_signal(&c->signal);
  }
}

zx_status_t VPartitionManager::DoIoLocked(zx_handle_t vmo, size_t off, size_t len,
                                          uint32_t command) const {
  const size_t block_size = info_.block_size;
  size_t len_remaining = len / block_size;
  size_t vmo_offset = 0;
  size_t dev_offset = off / block_size;

  // The operation may need to be chuncked according to the block device's limits. We don't check
  // explicitly for fuchsia_hardware_block::wire::kMaxTransferUnbounded because the transfers are
  // still limited to 32-bits and that constant is the largest 32-bit value.
  const size_t max_transfer = info_.max_transfer_size / block_size;
  const size_t num_data_txns = fbl::round_up(len_remaining, max_transfer) / max_transfer;

  // Add a "FLUSH" operation to write requests.
  const bool flushing = command == BLOCK_OP_WRITE;
  const size_t num_txns = num_data_txns + (flushing ? 1 : 0);

  fbl::Array<uint8_t> buffer(new uint8_t[block_op_size_ * num_txns], block_op_size_ * num_txns);

  VpmIoCookie cookie;
  cookie.num_txns.store(num_txns);
  cookie.status.store(ZX_OK);
  sync_completion_reset(&cookie.signal);

  for (size_t i = 0; i < num_data_txns; i++) {
    size_t length = std::min(len_remaining, max_transfer);
    len_remaining -= length;

    block_op_t* bop = reinterpret_cast<block_op_t*>(buffer.data() + (block_op_size_ * i));

    bop->command = command;
    bop->rw.vmo = vmo;
    bop->rw.length = static_cast<uint32_t>(length);
    bop->rw.offset_dev = dev_offset;
    bop->rw.offset_vmo = vmo_offset;
    memset(buffer.data() + (block_op_size_ * i) + sizeof(block_op_t), 0,
           block_op_size_ - sizeof(block_op_t));
    vmo_offset += length;
    dev_offset += length;

    Queue(bop, IoCallback, &cookie);
  }

  if (flushing) {
    block_op_t* bop =
        reinterpret_cast<block_op_t*>(buffer.data() + (block_op_size_ * num_data_txns));
    memset(bop, 0, sizeof(*bop));
    bop->command = BLOCK_OP_FLUSH;
    Queue(bop, IoCallback, &cookie);
  }

  ZX_DEBUG_ASSERT(len_remaining == 0);
  sync_completion_wait(&cookie.signal, ZX_TIME_INFINITE);
  return static_cast<zx_status_t>(cookie.status.load());
}

zx_status_t VPartitionManager::Load() {
  fbl::AutoLock lock(&lock_);

  // Let DdkRelease know the thread was successfully created. It is guaranteed
  // that DdkRelease will not be run until after we reply to |init_txn_|.
  initialization_thread_started_ = true;

  // Signal all threads blocked on this thread completion. Join Only happens in DdkRelease, but we
  // need to block earlier to avoid races between DdkRemove and any API call.
  auto singal_completion = fit::defer([this]() { sync_completion_signal(&worker_completed_); });

  auto auto_detach = fit::defer([&]() TA_NO_THREAD_SAFETY_ANALYSIS {
    zxlogf(ERROR, "Aborting Driver Load");
    // This will schedule the device to be unbound.
    if (init_txn_)
      init_txn_->Reply(ZX_ERR_INTERNAL);
  });

  // Sanity check the device info passed to the constructor.
  if (info_.block_size == 0) {
    zxlogf(ERROR, "Can't have a zero block size.");
    return ZX_ERR_BAD_STATE;
  }

  zx::vmo vmo;
  zx_status_t status;
  if ((status = zx::vmo::create(fvm::kBlockSize, 0, &vmo)) != ZX_OK) {
    return status;
  }

  // Read the superblock first to find the secondary superblock.
  //
  // If the primary superblock is corrupt enough to cause us not to find the secondary one (the
  // partition table allocation table sizes are incorrect), we won't be able to find the secondary
  // and the drive will be lost. In the current design the A/B metadata primarily provides atomic
  // update rather than full backup capabilities.
  if ((status = DoIoLocked(vmo.get(), 0, fvm::kBlockSize, BLOCK_OP_READ)) != ZX_OK) {
    zxlogf(ERROR, "Failed to read first block from underlying device");
    return status;
  }

  Header sb;  // Use only to find the secondary superblock.
  status = vmo.read(&sb, 0, sizeof(sb));
  if (status != ZX_OK) {
    return status;
  }

  // Cancelled before we return ZX_OK at the end of Load().
  auto dump_header = fit::defer([&sb]() { zxlogf(ERROR, "%s\n", sb.ToString().c_str()); });

  // Allocate a buffer big enough for the allocated metadata.
  size_t metadata_vmo_size = sb.GetMetadataAllocatedBytes();

  auto load_metadata = [&](size_t offset) -> zx::result<std::unique_ptr<VmoMetadataBuffer>> {
    fzl::OwnedVmoMapper mapper;
    zx_status_t status;
    if (status = mapper.CreateAndMap(metadata_vmo_size, "fvm-metadata"); status != ZX_OK) {
      return zx::error(status);
    }
    if (status = DoIoLocked(mapper.vmo().get(), offset, metadata_vmo_size, BLOCK_OP_READ);
        status != ZX_OK) {
      zxlogf(ERROR, "Failed to read %lu bytes from offset %lu: %s", metadata_vmo_size, offset,
             zx_status_get_string(status));
      return zx::error(status);
    }
    return zx::ok(std::make_unique<VmoMetadataBuffer>(std::move(mapper)));
  };

  auto buffer_a_or = load_metadata(sb.GetSuperblockOffset(SuperblockType::kPrimary));
  if (buffer_a_or.is_error()) {
    return buffer_a_or.status_value();
  }
  auto buffer_b_or = load_metadata(sb.GetSuperblockOffset(SuperblockType::kSecondary));
  if (buffer_b_or.is_error()) {
    return buffer_b_or.status_value();
  }

  zx::result<Metadata> metadata_or = Metadata::Create(
      DiskSize(), info_.block_size, std::move(buffer_a_or.value()), std::move(buffer_b_or.value()));
  if (metadata_or.is_error()) {
    zxlogf(ERROR, "Failed to parse fvm metadata.");
    return metadata_or.status_value();
  }
  metadata_ = std::move(metadata_or.value());
  slice_size_ = metadata_.GetHeader().slice_size;

  // See if we need to grow the metadata to cover more of the underlying disk.
  Header* header = GetHeaderLocked();
  size_t slices_for_disk = header->GetMaxAllocationTableEntriesForDiskSize(DiskSize());
  if (slices_for_disk > header->GetAllocationTableUsedEntryCount()) {
    header->SetSliceCount(slices_for_disk);

    // Persist the growth.
    if ((status = WriteFvmLocked()) != ZX_OK) {
      zxlogf(ERROR, "Persisting updated header failed.");
      return status;
    }
  }

  // Begin initializing the underlying partitions

  // This will make the device visible and able to be unbound.
  if (init_txn_)
    init_txn_->Reply(ZX_OK);
  auto_detach.cancel();

  // 0th vpartition is invalid
  std::unique_ptr<VPartition> vpartitions[fvm::kMaxVPartitions] = {};
  bool has_updated_partitions = false;

  size_t reserved_slices = 0;
  // Iterate through FVM Entry table, allocating the VPartitions which
  // claim to have slices.
  for (size_t i = 1; i < fvm::kMaxVPartitions; i++) {
    auto* entry = GetVPartEntryLocked(i);
    if (entry->IsFree()) {
      continue;
    }
    if (entry->IsInternalReservationPartition()) {
      zxlogf(INFO, "Found reserved partition with %u slices", entry->slices);
      reserved_slices = entry->slices;
    }

    // Update instance placeholder GUIDs to a newly generated guid.
    if (GuidsEqual(entry->guid, kPlaceHolderInstanceGuid.data())) {
      uuid::Uuid uuid = uuid::Uuid::Generate();
      memcpy(entry->guid, uuid.bytes(), uuid::kUuidSize);
      has_updated_partitions = true;
    }

    if ((status = VPartition::Create(this, i, &vpartitions[i])) != ZX_OK) {
      zxlogf(ERROR, "Failed to Create vpartition %zu", i);
      return status;
    }
  }

  if (has_updated_partitions) {
    if ((status = WriteFvmLocked()) != ZX_OK) {
      return status;
    }
  }

  // Iterate through the Slice Allocation table, filling the slice maps
  // of VPartitions.
  for (uint32_t i = 1; i <= GetHeaderLocked()->pslice_count; i++) {
    const SliceEntry* entry = GetSliceEntryLocked(i);
    if (entry->IsFree()) {
      continue;
    }
    if (vpartitions[entry->VPartition()] == nullptr) {
      continue;
    }

    // It's fine to load the slices while not holding the vpartition
    // lock; no VPartition devices exist yet.
    vpartitions[entry->VPartition()]->SliceSetUnsafe(entry->VSlice(), i);
    pslice_allocated_count_++;
  }

  LogPartitionInfoLocked();

  lock.release();

  // Iterate through 'valid' VPartitions, and create their devices.
  size_t device_count = 0;
  std::vector<Diagnostics::OnMountArgs::Partition> partitions = {};
  for (size_t i = 0; i < fvm::kMaxVPartitions; i++) {
    if (vpartitions[i] == nullptr) {
      continue;
    }
    VPartitionEntry* entry = GetAllocatedVPartEntry(i);
    if (entry->IsInactive()) {
      zxlogf(ERROR, "Freeing %u slices from inactive partition %zu (%s)", entry->slices, i,
             entry->name().c_str());
      FreeSlices(vpartitions[i].get(), 0, VSliceMax());
      continue;
    }
    if ((status = AddPartition(std::move(vpartitions[i]))) != ZX_OK) {
      zxlogf(ERROR, "Failed to add partition: %s", zx_status_get_string(status));
      continue;
    }
    partitions.push_back({.name = entry->name(), .num_slices = entry->slices});
    device_count++;
  }

  diagnostics().OnMount({
      .major_version = header->major_version,
      .oldest_minor_version = header->oldest_minor_version,
      .slice_size = header->slice_size,
      .num_slices = header->pslice_count,
      .partition_table_entries = header->GetPartitionTableEntryCount(),
      // TODO(fxbug.dev/40192): Set to the actual value when partition table size is configurable
      .partition_table_reserved_entries = header->GetPartitionTableEntryCount(),
      .allocation_table_entries = header->GetAllocationTableUsedEntryCount(),
      .allocation_table_reserved_entries = header->GetAllocationTableAllocatedEntryCount(),
      .num_reserved_slices = reserved_slices,
      .partitions = std::move(partitions),
  });
  zxlogf(INFO, "Loaded %lu partitions, slice size=%zu", device_count, slice_size_);

  dump_header.cancel();

  return ZX_OK;
}

zx_status_t VPartitionManager::WriteFvmLocked() {
  fvm::Header* header = GetHeaderLocked();
  header->generation++;

  // Track the oldest minor version of the driver that has written to this FVM metadata.
  if (header->oldest_minor_version > fvm::kCurrentMinorVersion)
    header->oldest_minor_version = fvm::kCurrentMinorVersion;

  metadata_.UpdateHash();

  // This is safe as long as metadata_ was constructed with a VmoMetadataBuffer, which is the case
  // in VPartitionManager::Load.
  const VmoMetadataBuffer* buffer = reinterpret_cast<const VmoMetadataBuffer*>(metadata_.Get());

  // Persist the changes to the inactive metadata. The active metadata is not modified.
  if (zx_status_t status = DoIoLocked(buffer->vmo().get(), metadata_.GetInactiveHeaderOffset(),
                                      header->GetMetadataUsedBytes(), BLOCK_OP_WRITE);
      status != ZX_OK) {
    zxlogf(ERROR, "Failed to write metadata: %s", zx_status_get_string(status));
    return status;
  }

  metadata_.SwitchActiveHeaders();

  return ZX_OK;
}

zx_status_t VPartitionManager::FindFreeVPartEntryLocked(size_t* out) const {
  for (size_t i = 1; i < fvm::kMaxVPartitions; i++) {
    const VPartitionEntry* entry = GetVPartEntryLocked(i);
    if (entry->IsFree() && !device_bound_at_entry_[i]) {
      *out = i;
      return ZX_OK;
    }
  }
  return ZX_ERR_NO_SPACE;
}

zx_status_t VPartitionManager::FindFreeSliceLocked(size_t* out, size_t hint) const {
  hint = std::max(hint, 1lu);
  size_t slice_count = GetHeaderLocked()->GetAllocationTableUsedEntryCount();
  for (size_t i = hint; i <= slice_count; i++) {
    if (GetSliceEntryLocked(i)->IsFree()) {
      *out = i;
      return ZX_OK;
    }
  }
  for (size_t i = 1; i < hint; i++) {
    if (GetSliceEntryLocked(i)->IsFree()) {
      *out = i;
      return ZX_OK;
    }
  }
  return ZX_ERR_NO_SPACE;
}

zx_status_t VPartitionManager::AllocateSlices(VPartition* vp, size_t vslice_start, size_t count) {
  fbl::AutoLock lock(&lock_);
  return AllocateSlicesLocked(vp, vslice_start, count);
}

zx_status_t VPartitionManager::AllocateSlicesLocked(VPartition* vp, size_t vslice_start,
                                                    size_t count) {
  if (count == 0) {
    return ZX_OK;  // Nothing to do.
  }
  if (safemath::ClampAdd(vslice_start, count) > VSliceMax()) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status = ZX_OK;
  size_t hint = 0;
  size_t total_slices_reserved = 0;

  {
    fbl::AutoLock lock(&vp->lock_);
    if (vp->IsKilledLocked()) {
      return ZX_ERR_BAD_STATE;
    }

    ZX_DEBUG_ASSERT_MSG(vp->entry_index() >= 1 && vp->entry_index() < fvm::kMaxVPartitions,
                        "VPartition entry index out of range.");
    if (uint64_t max_slices = max_partition_sizes_[vp->entry_index()]) {
      // Enforce the max partition size.
      auto existing_slices = GetVPartEntryLocked(vp->entry_index())->slices;
      uint64_t requested_slices = safemath::ClampAdd(existing_slices, count);
      if (requested_slices > max_slices) {
        return ZX_ERR_NO_SPACE;
      }
    }

    for (size_t i = 0; i < count; i++) {
      size_t pslice;
      auto vslice = vslice_start + i;
      if (vp->SliceGetLocked(vslice, &pslice)) {
        zxlogf(ERROR, "FVM: Attempting to allocate vslice %zu that is already allocated.", vslice);
        status = ZX_ERR_INVALID_ARGS;
      }

      // If the vslice is invalid, or there are no more free physical slices, undo all
      // previous allocations.
      if ((status != ZX_OK) || ((status = FindFreeSliceLocked(&pslice, hint)) != ZX_OK)) {
        for (int j = safemath::checked_cast<int>(i) - 1; j >= 0; j--) {
          vslice = vslice_start + j;
          vp->SliceGetLocked(vslice, &pslice);
          FreePhysicalSlice(vp, pslice);
          vp->SliceFreeLocked(vslice);
        }

        return status;
      }

      // Allocate the slice in the partition then mark as allocated.
      vp->SliceSetLocked(vslice, pslice);
      AllocatePhysicalSlice(vp, pslice, vslice);
      hint = pslice + 1;
    }

    total_slices_reserved = vp->NumSlicesLocked();
  }

  if ((status = WriteFvmLocked()) == ZX_OK) {
    VPartitionEntry* entry = GetVPartEntryLocked(vp->entry_index());
    diagnostics().UpdatePartitionMetrics(entry->name(), total_slices_reserved);
  } else {
    // Undo allocation in the event of failure; avoid holding VPartition lock while writing to fvm.
    fbl::AutoLock lock(&vp->lock_);
    for (int j = safemath::checked_cast<int>(count) - 1; j >= 0; j--) {
      auto vslice = vslice_start + j;
      uint64_t pslice;
      // Will always return true, because partition slice allocation is synchronized.
      if (vp->SliceGetLocked(vslice, &pslice)) {
        FreePhysicalSlice(vp, pslice);
        vp->SliceFreeLocked(vslice);
      }
    }
  }

  return status;
}

zx_status_t VPartitionManager::Upgrade(const uint8_t* old_guid, const uint8_t* new_guid) {
  fbl::AutoLock lock(&lock_);
  size_t old_index = 0;
  size_t new_index = 0;

  if (GuidsEqual(old_guid, new_guid)) {
    old_guid = nullptr;
  }

  for (size_t i = 1; i < fvm::kMaxVPartitions; i++) {
    auto entry = GetVPartEntryLocked(i);
    if (entry->slices != 0) {
      if (old_guid && entry->IsActive() && GuidsEqual(entry->guid, old_guid)) {
        old_index = i;
      } else if (entry->IsInactive() && GuidsEqual(entry->guid, new_guid)) {
        new_index = i;
      }
    }
  }

  if (!new_index) {
    return ZX_ERR_NOT_FOUND;
  }

  if (old_index) {
    GetVPartEntryLocked(old_index)->SetActive(false);
  }
  GetVPartEntryLocked(new_index)->SetActive(true);

  return WriteFvmLocked();
}

zx_status_t VPartitionManager::FreeSlices(VPartition* vp, size_t vslice_start, size_t count) {
  fbl::AutoLock lock(&lock_);
  return FreeSlicesLocked(vp, safemath::strict_cast<uint64_t>(vslice_start), count);
}

zx_status_t VPartitionManager::FreeSlicesLocked(VPartition* vp, uint64_t vslice_start,
                                                size_t count) {
  if (count == 0) {
    return ZX_OK;  // Nothing to do.
  }
  if (safemath::ClampAdd(vslice_start, count) > VSliceMax()) {
    return ZX_ERR_INVALID_ARGS;
  }

  bool valid_range = false;
  std::string partition_name;
  size_t total_slices_reserved = 0;
  {
    fbl::AutoLock lock(&vp->lock_);
    if (vp->IsKilledLocked())
      return ZX_ERR_BAD_STATE;

    auto entry = GetVPartEntryLocked(vp->entry_index());
    partition_name = entry->name();

    if (vslice_start == 0) {
      // Special case: Freeing entire VPartition
      for (auto extent = vp->ExtentBegin(); extent.IsValid(); extent = vp->ExtentBegin()) {
        for (size_t i = extent->start(); i < extent->end(); i++) {
          uint64_t pslice;
          vp->SliceGetLocked(i, &pslice);
          FreePhysicalSlice(vp, pslice);
        }
        vp->ExtentDestroyLocked(extent->start());
      }

      // Remove device, VPartition if this was a request to release all slices.
      if (vp->zxdev()) {
        vp->DdkAsyncRemove();
      }
      entry->Release();
      vp->KillLocked();
      valid_range = true;
    } else {
      for (int i = safemath::checked_cast<int>(count - 1); i >= 0; i--) {
        auto vslice = vslice_start + i;
        if (vp->SliceCanFree(vslice)) {
          uint64_t pslice;
          vp->SliceGetLocked(vslice, &pslice);
          vp->SliceFreeLocked(vslice);
          FreePhysicalSlice(vp, pslice);
          valid_range = true;
        }
      }
    }
    total_slices_reserved = vp->NumSlicesLocked();
  }

  if (!valid_range) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status = WriteFvmLocked();
  if (status == ZX_OK) {
    diagnostics().UpdatePartitionMetrics(partition_name, total_slices_reserved);
  }
  return status;
}

void VPartitionManager::GetInfoInternal(VolumeManagerInfo* info) {
  fbl::AutoLock lock(&lock_);

  info->slice_size = slice_size_;
  info->slice_count = GetHeaderLocked()->pslice_count;
  info->assigned_slice_count = pslice_allocated_count_;
  info->maximum_slice_count = GetHeaderLocked()->GetAllocationTableAllocatedEntryCount();
  info->max_virtual_slice = VSliceMax();
}

uint64_t VPartitionManager::GetPartitionLimitInternal(size_t index) const {
  ZX_DEBUG_ASSERT(index >= 1);
  ZX_DEBUG_ASSERT(index < fvm::kMaxVPartitions);

  fbl::AutoLock lock(&lock_);
  return max_partition_sizes_[index];
}

zx_status_t VPartitionManager::GetPartitionLimitInternal(const uint8_t* guid,
                                                         uint64_t* slice_count) const {
  fbl::AutoLock lock(&lock_);

  if (size_t partition = GetPartitionNumberLocked(guid)) {
    *slice_count = max_partition_sizes_[partition];
    return ZX_OK;
  }

  // The bad GUID will already have been logged by GetPartitionNumberLocked().
  zxlogf(ERROR, "Unable to get partition limit, partition not found.");
  *slice_count = 0;
  return ZX_ERR_NOT_FOUND;
}

zx_status_t VPartitionManager::SetPartitionLimitInternal(const uint8_t* guid,
                                                         uint64_t slice_count) {
  fbl::AutoLock lock(&lock_);

  if (size_t partition = GetPartitionNumberLocked(guid)) {
    zxlogf(INFO, "Setting partition limit to 0x%" PRIx64 " slices for partition #%zu", slice_count,
           partition);
    max_partition_sizes_[partition] = slice_count;
    // Update Inspect diagnostic.
    diagnostics().UpdateMaxBytes(GetVPartEntryLocked(partition)->name(), slice_count * slice_size_);
    return ZX_OK;
  }

  // The partition GUID will already have been logged by GetPartitionNumberLocked().
  zxlogf(ERROR, "Unable set partition limit to %" PRIu64 " slices, partition not found.",
         slice_count);
  // This additional logging by LogPartitionInfoLocked() about each partition was added due to
  // reports of failures of this function. In the future it can be removed if we find this function
  // fails in expected cases and the logging is excessive.
  LogPartitionInfoLocked();

  return ZX_ERR_NOT_FOUND;
}

zx_status_t VPartitionManager::SetPartitionNameInternal(const uint8_t* guid,
                                                        std::string_view name) {
  fbl::AutoLock lock(&lock_);
  const size_t partition = GetPartitionNumberLocked(guid);
  if (!partition) {
    // The partition GUID will already have been logged by GetPartitionNumberLocked().
    zxlogf(ERROR, "Unable set partition name to %s, partition not found.",
           std::string(name).c_str());
    return ZX_ERR_NOT_FOUND;
  }
  VPartitionEntry* entry = GetVPartEntryLocked(partition);
  zxlogf(INFO, "Renaming partition #%zu from %s to %s", partition, entry->name().c_str(),
         std::string(name).c_str());
  entry->set_name(name);
  return WriteFvmLocked();
}

void VPartitionManager::FreePhysicalSlice(VPartition* vp, uint64_t pslice) {
  auto entry = GetSliceEntryLocked(pslice);
  ZX_DEBUG_ASSERT_MSG(entry->IsAllocated(), "Freeing already-free slice");
  entry->Release();
  GetVPartEntryLocked(vp->entry_index())->slices--;
  pslice_allocated_count_--;
}

void VPartitionManager::AllocatePhysicalSlice(VPartition* vp, uint64_t pslice, uint64_t vslice) {
  uint64_t vpart = vp->entry_index();
  ZX_DEBUG_ASSERT(vpart <= fvm::kMaxVPartitions);
  ZX_DEBUG_ASSERT(vslice <= fvm::kMaxVSlices);
  auto entry = GetSliceEntryLocked(pslice);
  ZX_DEBUG_ASSERT_MSG(entry->IsFree(), "Allocating previously allocated slice");
  entry->Set(vpart, vslice);
  GetVPartEntryLocked(vpart)->slices++;
  pslice_allocated_count_++;
}

SliceEntry* VPartitionManager::GetSliceEntryLocked(size_t index) const {
  const Header* header = GetHeaderLocked();
  ZX_DEBUG_ASSERT(index >= 1);
  ZX_DEBUG_ASSERT(index <= header->GetAllocationTableUsedEntryCount());

  return &metadata_.GetSliceEntry(index);
}

VPartitionEntry* VPartitionManager::GetVPartEntryLocked(size_t index) const {
  Header* header = GetHeaderLocked();
  ZX_DEBUG_ASSERT(index >= 1);
  ZX_DEBUG_ASSERT(index <= header->GetPartitionTableEntryCount());

  return &metadata_.GetPartitionEntry(index);
}

size_t VPartitionManager::GetPartitionNumberLocked(const uint8_t* guid) const {
  for (size_t i = 1; i < fvm::kMaxVPartitions; i++) {
    auto* entry = GetVPartEntryLocked(i);
    if (entry->IsAllocated() && GuidsEqual(entry->guid, guid))
      return i;
  }

  zxlogf(ERROR, "Partition not found for GUID %s", uuid::Uuid(guid).ToString().c_str());
  return 0;
}

void VPartitionManager::LogPartitionInfoLocked() const {
  std::stringstream out;
  const Header* header = GetHeaderLocked();
  out << "FVM INFO: Header ";
  out << (metadata_.active_header() == SuperblockType::kPrimary ? "A " : "B ");
  out << *header;

  // Additionally log the unallocated slices so we know how much we can grow the partitions.
  size_t free_slices = 0;
  for (size_t i = 1; i <= header->GetAllocationTableUsedEntryCount(); i++) {
    if (GetSliceEntryLocked(i)->IsFree())
      ++free_slices;
  }
  out << " free_slices:" << free_slices;

  zxlogf(INFO, "%s", out.str().c_str());

  for (size_t i = 1; i < fvm::kMaxVPartitions; i++) {
    if (auto* entry = GetVPartEntryLocked(i); entry->IsAllocated()) {
      std::stringstream out;
      out << " #" << i << ": " << *entry << " limit:" << max_partition_sizes_[i];
      zxlogf(INFO, "%s", out.str().c_str());
    }
  }
}

// Device protocol (FVM)

zx::result<std::unique_ptr<VPartition>> VPartitionManager::AllocatePartition(
    uint64_t slice_count, const fuchsia_hardware_block_partition::wire::Guid& type,
    const fuchsia_hardware_block_partition::wire::Guid& instance, fidl::StringView name,
    uint32_t flags) {
  if (slice_count >= std::numeric_limits<uint32_t>::max()) {
    return zx::error(ZX_ERR_OUT_OF_RANGE);
  }
  if (slice_count == 0) {
    return zx::error(ZX_ERR_OUT_OF_RANGE);
  }

  // Validate the name. It should fit and not have any NULL terminators in it.
  constexpr size_t kMaxNameLen = std::min<size_t>(
      fuchsia_hardware_block_partition::wire::kNameLength, kMaxVPartitionNameLength);
  if (name.size() > kMaxNameLen) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  std::string name_str(name.get());
  if (name_str.find('\0') != std::string::npos) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  std::unique_ptr<VPartition> vpart;
  {
    fbl::AutoLock lock(&lock_);
    size_t vpart_entry;
    if (zx_status_t status = FindFreeVPartEntryLocked(&vpart_entry); status != ZX_OK) {
      return zx::error(status);
    }

    if (zx_status_t status = VPartition::Create(this, vpart_entry, &vpart); status != ZX_OK) {
      return zx::error(status);
    }

    auto* entry = GetVPartEntryLocked(vpart_entry);
    *entry =
        VPartitionEntry(type.value.data(), instance.value.data(), 0, std::move(name_str), flags);

    // Each partition starts off with a 0 max length ("no limit").
    max_partition_sizes_[vpart_entry] = 0;

    if (zx_status_t status = AllocateSlicesLocked(vpart.get(), 0, slice_count); status != ZX_OK) {
      entry->slices = 0;  // Undo VPartition allocation
      return zx::error(status);
    }
  }

  return zx::ok(std::move(vpart));
}

void VPartitionManager::AllocatePartition(AllocatePartitionRequestView request,
                                          AllocatePartitionCompleter::Sync& completer) {
  auto partition_or = AllocatePartition(request->slice_count, request->type, request->instance,
                                        request->name, request->flags);
  zx_status_t status = partition_or.status_value();
  if (partition_or.is_ok()) {
    // Register the created partition with the device manager.
    status = AddPartition(std::move(partition_or.value()));
  }

  completer.Reply(status);
}

void VPartitionManager::GetInfo(GetInfoCompleter::Sync& completer) {
  fidl::Arena allocator;
  fidl::ObjectView<fuchsia_hardware_block_volume::wire::VolumeManagerInfo> info(allocator);
  GetInfoInternal(info.get());
  completer.Reply(ZX_OK, info);
}

void VPartitionManager::Activate(ActivateRequestView request, ActivateCompleter::Sync& completer) {
  completer.Reply(Upgrade(request->old_guid.value.data(), request->new_guid.value.data()));
}

void VPartitionManager::GetPartitionLimit(GetPartitionLimitRequestView request,
                                          GetPartitionLimitCompleter::Sync& completer) {
  uint64_t slice_count = 0;
  zx_status_t status = GetPartitionLimitInternal(request->guid.value.data(), &slice_count);
  completer.Reply(status, slice_count);
}

void VPartitionManager::SetPartitionLimit(SetPartitionLimitRequestView request,
                                          SetPartitionLimitCompleter::Sync& completer) {
  completer.Reply(SetPartitionLimitInternal(request->guid.value.data(), request->slice_count));
}

void VPartitionManager::SetPartitionName(SetPartitionNameRequestView request,
                                         SetPartitionNameCompleter::Sync& completer) {
  zx_status_t status = SetPartitionNameInternal(request->guid.value.data(), request->name.get());
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(status);
  }
}

void VPartitionManager::DdkUnbind(ddk::UnbindTxn txn) {
  // Wait untill all work has been completed, before removing the device.
  sync_completion_wait(&worker_completed_, zx::duration::infinite().get());

  txn.Reply();
}

void VPartitionManager::DdkRelease() {
  if (initialization_thread_started_) {
    // Wait until the worker thread exits before freeing the resources.
    thrd_join(initialization_thread_, nullptr);
  }
  delete this;
}

void VPartitionManager::DdkChildPreRelease(void* child) {
  VPartition* vp = static_cast<VPartition*>(child);
  fbl::AutoLock lock(&lock_);
  device_bound_at_entry_[vp->entry_index()] = false;
}

zx_driver_ops_t driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = VPartitionManager::Bind,
};

}  // namespace fvm

ZIRCON_DRIVER(fvm, fvm::driver_ops, "zircon", "0.1");
