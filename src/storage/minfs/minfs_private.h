// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file describes the in-memory structures which construct
// a MinFS filesystem.

#ifndef SRC_STORAGE_MINFS_MINFS_PRIVATE_H_
#define SRC_STORAGE_MINFS_MINFS_PRIVATE_H_

#include <inttypes.h>

#include <memory>
#include <utility>

#ifdef __Fuchsia__
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fzl/resizeable-vmo-mapper.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/sync/completion.h>
#include <lib/zx/result.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <zircon/compiler.h>

#include "src/lib/storage/vfs/cpp/journal/journal.h"
#include "src/lib/storage/vfs/cpp/managed_vfs.h"
#include "src/lib/storage/vfs/cpp/watcher.h"
#include "src/storage/minfs/minfs_inspect_tree.h"
#endif

#include <lib/fit/function.h>

#include <fbl/algorithm.h>
#include <fbl/intrusive_hash_table.h>
#include <fbl/intrusive_single_list.h>
#include <fbl/macros.h>

#include "src/lib/storage/vfs/cpp/inspect/node_operations.h"
#include "src/lib/storage/vfs/cpp/journal/inspector_journal.h"
#include "src/lib/storage/vfs/cpp/transaction/transaction_handler.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/lib/storage/vfs/cpp/vnode.h"
#include "src/storage/minfs/format.h"
#include "src/storage/minfs/minfs.h"
#include "src/storage/minfs/superblock.h"
#include "src/storage/minfs/transaction_limits.h"
#include "src/storage/minfs/writeback.h"

#ifdef __Fuchsia__
#include "src/storage/minfs/vnode_allocation.h"
#endif

#include "src/storage/minfs/allocator/allocator.h"
#include "src/storage/minfs/allocator/inode_manager.h"
#include "src/storage/minfs/bcache.h"
#include "src/storage/minfs/vnode.h"

constexpr uint32_t kExtentCount = 6;

namespace minfs {

#ifdef __Fuchsia__
// How frequently we synchronize the journal. Without this, the journal will only get flushed when
// there is no room for a new transaction, or it is explicitly asked to by some other mechanism.
constexpr zx::duration kJournalBackgroundSyncTime = zx::sec(30);
#endif  // __Fuchsia__

// A async_dispatcher_t* is needed for some functions on Fuchsia only. In order to avoid ifdefs on
// every call that is compiled for host Fuchsia and Host, we define this as a nullptr_t type when
// compiling on host where callers should pass null and it's ignored.
//
// Prefer async_dispatcher_t* for Fuchsia-specific functions since it makes the intent more clear.
#ifdef __Fuchsia__
using FuchsiaDispatcher = async_dispatcher_t*;
#else
using FuchsiaDispatcher = std::nullptr_t;
#endif  // __Fuchsia__

// A reference to the vfs is needed for some functions. The specific vfs type is different between
// Fuchsia and Host, so define a PlatformVfs which represents the one for our current platform to
// avoid ifdefs on every call.
//
// Prefer using the appropriate vfs when the function is only used for one or the other.
#ifdef __Fuchsia__
using PlatformVfs = fs::ManagedVfs;
#else
using PlatformVfs = fs::Vfs;
#endif  // __Fuchsia__

// SyncVnode flags
constexpr uint32_t kMxFsSyncDefault = 0;  // default: no implicit time update
constexpr uint32_t kMxFsSyncMtime = (1 << 0);
constexpr uint32_t kMxFsSyncCtime = (1 << 1);

constexpr uint32_t kMinfsBlockCacheSize = 64;

// Used by fsck
class VnodeMinfs;

using SyncCallback = fs::Vnode::SyncCallback;

#ifndef __Fuchsia__

// Store start block + length for all extents. These may differ from info block for
// sparse files.
class BlockOffsets {
 public:
  BlockOffsets(const Bcache& bc, const SuperblockManager& sb);

  blk_t IbmStartBlock() const { return ibm_start_block_; }
  blk_t IbmBlockCount() const { return ibm_block_count_; }

  blk_t AbmStartBlock() const { return abm_start_block_; }
  blk_t AbmBlockCount() const { return abm_block_count_; }

  blk_t InoStartBlock() const { return ino_start_block_; }
  blk_t InoBlockCount() const { return ino_block_count_; }

  blk_t IntegrityStartBlock() const { return integrity_start_block_; }
  blk_t IntegrityBlockCount() const { return integrity_block_count_; }

  blk_t JournalStartBlock() const { return integrity_start_block_ + kBackupSuperblockBlocks; }

  blk_t DatStartBlock() const { return dat_start_block_; }
  blk_t DatBlockCount() const { return dat_block_count_; }

 private:
  blk_t ibm_start_block_;
  blk_t ibm_block_count_;

  blk_t abm_start_block_;
  blk_t abm_block_count_;

  blk_t ino_start_block_;
  blk_t ino_block_count_;

  blk_t integrity_start_block_;
  blk_t integrity_block_count_;

  blk_t dat_start_block_;
  blk_t dat_block_count_;
};
#endif

class TransactionalFs {
 public:
  virtual ~TransactionalFs() = default;

#ifdef __Fuchsia__
  virtual fbl::Mutex* GetLock() const = 0;

  virtual void EnqueueCallback(SyncCallback callback) = 0;
#endif

  // Begin a transaction with |reserve_inodes| inodes and |reserve_blocks| blocks reserved.
  [[nodiscard]] virtual zx::result<std::unique_ptr<Transaction>> BeginTransaction(
      size_t reserve_inodes, size_t reserve_blocks) = 0;

  // Enqueues a metadata transaction by persisting its contents to disk.
  virtual void CommitTransaction(std::unique_ptr<Transaction> transaction) = 0;

  virtual Bcache* GetMutableBcache() = 0;

  virtual Allocator& GetBlockAllocator() = 0;
  virtual Allocator& GetInodeAllocator() = 0;
};

// Constructor is private and we mark the class final because the destructor will call Terminate
// which will join threads and that won't be safe if it's been derived from.
class Minfs final : public TransactionalFs {
 public:
  // Not copyable or movable
  Minfs(const Minfs&) = delete;
  Minfs& operator=(const Minfs&) = delete;
  Minfs(Minfs&&) = delete;
  Minfs& operator=(Minfs&&) = delete;

  ~Minfs() override;

  // Destroys a "minfs" object, but take back ownership of the bcache object.
  static std::unique_ptr<Bcache> Destroy(std::unique_ptr<Minfs> minfs);

  // Terminates Minfs and joins all background threads.  This will not necessarily guarantee all
  // all data has been flushed. Call Sync first if that matters.
  void Terminate();

  [[nodiscard]] static zx::result<std::unique_ptr<Minfs>> Create(FuchsiaDispatcher dispatcher,
                                                                 std::unique_ptr<Bcache> bc,
                                                                 const MountOptions& options,
                                                                 PlatformVfs* vfs);

#ifdef __Fuchsia__
  // Initializes the Minfs journal and writeback queue and resolves any pending disk state (e.g.,
  // resolving unlinked nodes and existing journal entries).
  [[nodiscard]] zx::result<> InitializeJournal(fs::JournalSuperblock journal_superblock);

  // Initializes the Minfs writeback queue and resolves any pending disk state (e.g., resolving
  // unlinked nodes and existing journal entries). Does not enable the journal.
  [[nodiscard]] zx_status_t InitializeUnjournalledWriteback();
#endif

  // instantiate a vnode from an inode
  // the inode must exist in the file system
  [[nodiscard]] zx::result<fbl::RefPtr<VnodeMinfs>> VnodeGet(ino_t ino);

  // instantiate a vnode with a new inode
  [[nodiscard]] zx::result<fbl::RefPtr<VnodeMinfs>> VnodeNew(Transaction* transaction,
                                                             uint32_t type);

  // Insert, lookup, and remove vnode from hash map.
  void VnodeInsert(VnodeMinfs* vn) __TA_EXCLUDES(hash_lock_);
  fbl::RefPtr<VnodeMinfs> VnodeLookup(uint32_t ino) __TA_EXCLUDES(hash_lock_);
  void VnodeRelease(VnodeMinfs* vn) __TA_EXCLUDES(hash_lock_);

  // Opens the root inode. This is a special-case of VnodeGet for filesystem bootstrapping.
  zx::result<fbl::RefPtr<VnodeMinfs>> OpenRootNode();

  // Allocate a new data block.
  void BlockNew(PendingWork* transaction, blk_t* out_bno) const;

  // Set/Unset the flags.
  void UpdateFlags(PendingWork* transaction, uint32_t flags, bool set);

  // Mark |in_bno| for de-allocation (if it is > 0), and return a new block |*out_bno|.
  // The swap will not be persisted until the transaction is commited.
  void BlockSwap(Transaction* transaction, blk_t in_bno, blk_t* out_bno);

  // Free ino in inode bitmap, release all blocks held by inode.
  [[nodiscard]] zx::result<> InoFree(Transaction* transaction, VnodeMinfs* vn);

  // Mark |vn| to be unlinked.
  void AddUnlinked(PendingWork* transaction, VnodeMinfs* vn);

  // Remove |vn| from the list of unlinked vnodes.
  void RemoveUnlinked(PendingWork* transaction, VnodeMinfs* vn);

  // Free resources of all vnodes marked unlinked.
  [[nodiscard]] zx::result<> PurgeUnlinked();

  // Writes back an inode into the inode table on persistent storage.
  // Does not modify inode bitmap.
  void InodeUpdate(PendingWork* transaction, ino_t ino, const Inode* inode) {
    inodes_->Update(transaction, ino, inode);
  }

  // Reads an inode from the inode table into memory.
  void InodeLoad(ino_t ino, Inode* out) const { inodes_->Load(ino, out); }

  void ValidateBno(blk_t bno) const {
    ZX_DEBUG_ASSERT(bno != 0);
    ZX_DEBUG_ASSERT(bno < Info().block_count);
  }

  [[nodiscard]] zx::result<std::unique_ptr<Transaction>> BeginTransaction(
      size_t reserve_inodes, size_t reserve_blocks) final;

  // Converts a cached transaction into a Transaction. Extends block reservation
  // by |reserve_blocks|.
  // On failure to reserve blocks, returns error but |out| will have a transaction
  // that was converted.
  [[nodiscard]] zx::result<> ContinueTransaction(
      size_t reserve_blocks, std::unique_ptr<CachedBlockTransaction> cached_transaction,
      std::unique_ptr<Transaction>* out);
#ifdef __Fuchsia__
  void EnqueueCallback(SyncCallback callback) final;
#endif

  [[nodiscard]] bool IsJournalErrored();
  void EnqueueAllocation(std::unique_ptr<PendingWork> transaction);

  // Complete a transaction by enqueueing its WritebackWork to the WritebackQueue.
  void CommitTransaction(std::unique_ptr<Transaction> transaction) final;

  // Runs fsck at the end of a transaction, just after metadata has been written. Used for testing
  // to be sure that all transactions leave the file system in a good state.
  void FsckAtEndOfTransaction();

#ifdef __Fuchsia__
  // Returns the capacity of the writeback buffer, in blocks.
  size_t WritebackCapacity() const {
    // Hardcoded to 10 MB; may be replaced by a more device-specific option
    // in the future.
    return 10 * (1 << 20) / kMinfsBlockSize;
  }

  zx::result<fs::FilesystemInfo> GetFilesystemInfo();

  // Signals the completion object as soon as the journal has finished synchronizing.
  void Sync(SyncCallback closure = {});
#endif

  // The following methods are used to read one block from the specified extent,
  // from relative block |bno|.
  // |data| is an out parameter that must be a block in size, provided by the caller
  // These functions are single-block and synchronous. On Fuchsia, using the batched read
  // functions is preferred.
  [[nodiscard]] zx::result<> ReadDat(blk_t bno, void* data);

#ifdef __Fuchsia__

  // Return true if the all outstanding block reservations are backed by persistent storage
  // (i.e. the volume has already been extended to cover all reservations).
  // Requires a Transaction to ensure the transaction lock is held.
  [[nodiscard]] bool AllReservationsBacked(const Transaction&) const;

  // Record the location, size, and number of all non-free block regions.
  fbl::Vector<BlockRegion> GetAllocatedRegions() const;

  // Mutable pointer to the MinfsInspectTree this object uses for metrics. Guaranteed non-nullptr.
  MinfsInspectTree* InspectTree() { return &inspect_tree_; }

  fs_inspect::NodeOperations* GetNodeOperations() { return inspect_tree_.GetNodeOperations(); }
#else
  static fs_inspect::NodeOperations* GetNodeOperations() {
    static fs_inspect::NodeOperations stub;
    return &stub;
  }
#endif

  // Returns an immutable reference to the superblock.
  const Superblock& Info() const { return sb_->Info(); }

  uint64_t BlockSize() const {
    // Either intentionally or unintentionally, we do not want to change block
    // size to anything other than kMinfsBlockSize yet. This is because changing
    // block size might lead to format change and also because anything other
    // than 8k is not well tested. So assert when we find block size other
    // than 8k.
    ZX_ASSERT(Info().BlockSize() == kMinfsBlockSize);
    return Info().BlockSize();
  }

  // Gets an immutable reference to the InodeManager.
  const InspectableInodeManager* GetInodeManager() const { return inodes_.get(); }

  // Gets an immutable reference to the block_allocator.
  const Allocator& GetBlockAllocator() const { return *block_allocator_; }
  // Returns number of blocks available.
  size_t BlocksAvailable() const { return GetBlockAllocator().GetAvailable(); }

  // Returns number of reserved blocks but are yet to be allocated.
  // This helps to determine if we should fail incoming writes because we will
  // run out of space.
  size_t BlocksReserved() const { return GetBlockAllocator().GetReserved(); }

#ifndef __Fuchsia__
  // Gets an immutable copy of offsets_.
  BlockOffsets GetBlockOffsets() const { return offsets_; }
#endif

  // Used by the disk inspector.
  zx_status_t ReadBlock(blk_t start_block_num, void* out_data) const;

  const TransactionLimits& Limits() const { return limits_; }

#ifdef __Fuchsia__
  fbl::Mutex* GetLock() const final { return &txn_lock_; }

  // Terminates all writeback queues, and flushes pending operations to the underlying device.
  //
  // If |!IsReadonly()|, also sets the dirty bit to a "clean" status.
  void StopWriteback();

  // Issues a sync to the journal's background thread and waits for it to complete.
  zx::result<> BlockingJournalSync();
#endif

  Bcache* GetMutableBcache() final { return bc_.get(); }

  // TODO(rvargas): Make private.
  std::unique_ptr<Bcache> bc_;

  Allocator& GetBlockAllocator() final { return *block_allocator_; }
  Allocator& GetInodeAllocator() final { return inodes_->inode_allocator(); }

  const MountOptions& mount_options() { return mount_options_; }

  PlatformVfs* vfs() { return vfs_; }

 private:
  using HashTable = fbl::HashTable<ino_t, VnodeMinfs*>;

#ifdef __Fuchsia__
  Minfs(async_dispatcher_t* dispatcher, std::unique_ptr<Bcache> bc,
        std::unique_ptr<SuperblockManager> sb, std::unique_ptr<Allocator> block_allocator,
        std::unique_ptr<InodeManager> inodes, const MountOptions& mount_options,
        fs::ManagedVfs* vfs);
#else
  Minfs(std::unique_ptr<Bcache> bc, std::unique_ptr<SuperblockManager> sb,
        std::unique_ptr<Allocator> block_allocator, std::unique_ptr<InodeManager> inodes,
        BlockOffsets offsets, const MountOptions& mount_options, fs::Vfs* vfs);
#endif

  // Internal version of VnodeLookup which may also return unlinked vnodes.
  fbl::RefPtr<VnodeMinfs> VnodeLookupInternal(uint32_t ino) __TA_EXCLUDES(hash_lock_);

  // Returns a vector of vnodes having one or more blocks that needs to be
  // flushed.
  std::vector<fbl::RefPtr<VnodeMinfs>> GetDirtyVnodes();

  // Find a free inode, allocate it in the inode bitmap, and write it back to disk
  void InoNew(Transaction* transaction, const Inode* inode, ino_t* out_ino);

  // Find an unallocated and unreserved block in the block bitmap starting from block |start|
  [[nodiscard]] zx_status_t FindBlock(size_t start, size_t* blkno_out);

  // Reads blocks from disk. Only to be called during "construction".
  static zx::result<std::pair<std::unique_ptr<Allocator>, std::unique_ptr<InodeManager>>>
  ReadInitialBlocks(const Superblock& info, Bcache& bc, SuperblockManager& superblock_manager,
                    const MountOptions& mount_options);

  // Updates the clean bit and oldest revision in the super block.
  [[nodiscard]] zx::result<> UpdateCleanBitAndOldestRevision(bool is_clean);

#ifndef __Fuchsia__
  [[nodiscard]] zx::result<> ReadBlk(blk_t bno, blk_t start, blk_t soft_max, blk_t hard_max,
                                     void* data) const;
#endif

  // Global information about the filesystem.
  // While Allocator is thread-safe, it is recommended that a valid Transaction object be held
  // while any metadata fields are modified until the time they are enqueued for writeback. This
  // is to avoid modifications from other threads potentially jeopardizing the metadata integrity
  // before it is safely persisted to disk.
  std::unique_ptr<SuperblockManager> sb_;
  std::unique_ptr<Allocator> block_allocator_;
  std::unique_ptr<InodeManager> inodes_;

#ifdef __Fuchsia__
  mutable fbl::Mutex txn_lock_;   // Lock required to start a new Transaction.
  mutable fbl::Mutex hash_lock_;  // Lock required to access the vnode_hash_.
#endif
  // Vnodes exist in the hash table as long as one or more reference exists;
  // when the Vnode is deleted, it is immediately removed from the map.
  HashTable vnode_hash_ __TA_GUARDED(hash_lock_){};

#ifdef __Fuchsia__
  std::unique_ptr<fs::Journal> journal_;

  // This event's koid is used as a unique identifier for this filesystem instance.
  zx::event fs_id_;

  // TODO(fxbug.dev/108131): Stop accessing outside `dispatcher_` thread.
  async::TaskClosure journal_sync_task_;

  MinfsInspectTree inspect_tree_;
  void InitializeInspectTree();
#else
  // Store start block + length for all extents. These may differ from info block for
  // sparse files.
  BlockOffsets offsets_;
#endif

  TransactionLimits limits_;
  MountOptions mount_options_;

#ifdef __Fuchsia__
  async_dispatcher_t* dispatcher_;
#endif
  PlatformVfs* vfs_;
};

#ifdef __Fuchsia__
// Replay the minfs journal, given the sizes provided within the superblock.
[[nodiscard]] zx::result<fs::JournalSuperblock> ReplayJournal(Bcache* bc, const Superblock& info);
#endif

// write the inode data of this vnode to disk (default does not update time values)
void SyncVnode(fbl::RefPtr<VnodeMinfs> vn, uint32_t flags);
void DumpInfo(const Superblock& info);
void DumpInode(const Inode* inode, ino_t ino);
zx_time_t GetTimeUTC();
void InitializeDirectory(void* bdata, ino_t ino_self, ino_t ino_parent);

}  // namespace minfs

#endif  // SRC_STORAGE_MINFS_MINFS_PRIVATE_H_
