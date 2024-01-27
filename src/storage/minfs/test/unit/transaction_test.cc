// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tests Transaction behavior.

#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/storage/minfs/format.h"
#include "src/storage/minfs/minfs_private.h"
#include "src/storage/minfs/unowned_vmo_buffer.h"
#include "src/storage/minfs/writeback.h"
#include "zircon/errors.h"

namespace minfs {
namespace {

using ::testing::_;

constexpr size_t kTotalElements = 32768;
constexpr size_t kDefaultElements = kTotalElements / 64;
constexpr size_t kDefaultStartBlock = 0;

// Fake Storage class to be used in Transaction tests.
class FakeStorage : public AllocatorStorage {
 public:
  FakeStorage() = delete;
  FakeStorage(const FakeStorage&) = delete;
  FakeStorage& operator=(const FakeStorage&) = delete;

  explicit FakeStorage(uint32_t units) : pool_used_(0), pool_total_(units) {}

  ~FakeStorage() override = default;

  zx::result<> AttachVmo(const zx::vmo& vmo, storage::OwnedVmoid* vmoid) final { return zx::ok(); }

  void Load(fs::BufferedOperationsBuilder* builder, storage::BlockBuffer* data) final {}

  zx::result<> Extend(PendingWork* transaction, WriteData data, GrowMapCallback grow_map) final {
    return zx::error(ZX_ERR_NO_SPACE);
  }

  uint32_t PoolAvailable() const final { return pool_total_ - pool_used_; }

  uint32_t PoolTotal() const final { return pool_total_; }

  // Write back the allocation of the following items to disk.
  void PersistRange(PendingWork* transaction, WriteData data, size_t index, size_t count) final {}

  void PersistAllocate(PendingWork* transaction, size_t count) final {
    ZX_DEBUG_ASSERT(pool_used_ + count <= pool_total_);
    pool_used_ += static_cast<uint32_t>(count);
  }

  void PersistRelease(PendingWork* transaction, size_t count) final {
    ZX_DEBUG_ASSERT(pool_used_ >= count);
    pool_used_ -= static_cast<uint32_t>(count);
  }

 private:
  uint32_t pool_used_;
  uint32_t pool_total_;
};

// Fake BlockDevice class to be used in Transaction tests.
class FakeBlockDevice : public block_client::BlockDevice {
 public:
  FakeBlockDevice() = default;

  zx_status_t FifoTransaction(block_fifo_request_t* requests, size_t count) final { return ZX_OK; }
  zx::result<std::string> GetTopologicalPath() const final { return zx::ok(std::string()); }
  zx::result<> Rebind(std::string_view url_suffix) const override {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
  zx_status_t BlockGetInfo(fuchsia_hardware_block::wire::BlockInfo* out_info) const final {
    return ZX_OK;
  }
  zx_status_t BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out_vmoid) final { return ZX_OK; }
  zx_status_t VolumeGetInfo(
      fuchsia_hardware_block_volume::wire::VolumeManagerInfo* out_manager,
      fuchsia_hardware_block_volume::wire::VolumeInfo* out_volume) const final {
    return ZX_OK;
  }
  zx_status_t VolumeQuerySlices(const uint64_t* slices, size_t slices_count,
                                fuchsia_hardware_block_volume::wire::VsliceRange* out_ranges,
                                size_t* out_ranges_count) const final {
    return ZX_OK;
  }
  zx_status_t VolumeExtend(uint64_t offset, uint64_t length) final { return ZX_OK; }
  zx_status_t VolumeShrink(uint64_t offset, uint64_t length) final { return ZX_OK; }
};

// Mock Minfs class to be used in Transaction tests.
class FakeMinfs : public TransactionalFs {
 public:
  FakeMinfs() {
    info_.inode_count = kTotalElements;
    info_.block_size = kMinfsBlockSize;
  }

  fbl::Mutex* GetLock() const override { return &txn_lock_; }

  zx::result<std::unique_ptr<Transaction>> BeginTransaction(size_t reserve_inodes,
                                                            size_t reserve_blocks) override {
    return zx::ok(nullptr);
  }

  void EnqueueCallback(SyncCallback callback) override {}

  void CommitTransaction(std::unique_ptr<Transaction> transaction) override {}

  Bcache* GetMutableBcache() override { return nullptr; }

  Allocator& GetBlockAllocator() override {
    if (!block_allocator_) {
      std::unique_ptr<FakeStorage> storage(new FakeStorage(kTotalElements));
      auto block_allocator_or = Allocator::Create(&builder_, std::move(storage));
      ZX_ASSERT(block_allocator_or.is_ok());
      block_allocator_ = std::move(block_allocator_or.value());
    }
    return *block_allocator_;
  }

  Allocator& GetInodeAllocator() override { return GetInodeManager().inode_allocator(); }

  InodeManager& GetInodeManager() {
    if (!inode_manager_) {
      // Create superblock manager.
      auto sb_manager_or = SuperblockManager::Create(&block_device_, info_, kDefaultStartBlock,
                                                     IntegrityCheck::kNone);
      ZX_ASSERT(sb_manager_or.is_ok());
      superblock_manager_ = std::move(sb_manager_or.value());

      // Create inode manager.
      AllocatorFvmMetadata fvm_metadata;
      AllocatorMetadata metadata(kDefaultStartBlock, kDefaultStartBlock, false,
                                 std::move(fvm_metadata), superblock_manager_.get(),
                                 SuperblockAllocatorAccess::Inodes());
      auto inode_manager_or =
          InodeManager::Create(&block_device_, superblock_manager_.get(), &builder_,
                               std::move(metadata), kDefaultStartBlock, kTotalElements);
      ZX_ASSERT(inode_manager_or.is_ok());
      inode_manager_ = std::move(inode_manager_or.value());
    }
    return *inode_manager_;
  }

  zx::result<std::unique_ptr<Transaction>> CreateTransaction(size_t inodes, size_t blocks) {
    return Transaction::Create(this, inodes, blocks, &GetInodeManager());
  }

  zx::result<std::unique_ptr<Transaction>> ContinueTransaction(
      size_t blocks, std::unique_ptr<CachedBlockTransaction> cached_transaction) {
    std::unique_ptr<Transaction> transaction =
        Transaction::FromCachedBlockTransaction(this, std::move(cached_transaction));

    if (auto status = transaction->ExtendBlockReservation(blocks); status.is_error()) {
      return status.take_error();
    }

    return zx::ok(std::move(transaction));
  }

 private:
  mutable fbl::Mutex txn_lock_;
  FakeBlockDevice block_device_;
  fs::BufferedOperationsBuilder builder_;
  Superblock info_ = {};
  std::unique_ptr<SuperblockManager> superblock_manager_;
  std::unique_ptr<InodeManager> inode_manager_;
  std::unique_ptr<Allocator> block_allocator_;
};

// Creates a Transaction using the public constructor, which by default contains no reservations.
TEST(TransactionTest, CreateTransactionNoReservationsAlt) {
  FakeMinfs minfs;
  Transaction transaction(&minfs);
}

// Creates a Transaction with no reservations.
TEST(TransactionTest, CreateTransactionNoReservations) {
  FakeMinfs minfs;
  ASSERT_TRUE(minfs.CreateTransaction(0, 0).is_ok());
}

// Creates a Transaction with inode and block reservations.
TEST(TransactionTest, CreateTransactionWithReservations) {
  FakeMinfs minfs;
  ASSERT_TRUE(minfs.CreateTransaction(kDefaultElements, kDefaultElements).is_ok());
}

// Creates a Transaction with inode and block reservations. Try to extend block reservation.
TEST(TransactionTest, ExtendBlockReservation) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(kDefaultElements, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  ASSERT_TRUE(transaction_or->ExtendBlockReservation(kTotalElements - kDefaultElements).is_ok());
}

// Creates a Transaction with inode and block reservations. Try to extend block reservation more
// than available.
TEST(TransactionTest, ExtendBlockReservationFails) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(kDefaultElements, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  ASSERT_TRUE(
      transaction_or->ExtendBlockReservation(kTotalElements + 1 - kDefaultElements).is_error());
}

// Creates a Transaction with the maximum possible number of inodes and blocks reserved.
TEST(TransactionTest, CreateTransactionWithMaxBlockReservations) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(kTotalElements, kTotalElements);
  ASSERT_TRUE(transaction_or.is_ok());
}

TEST(TransactionTest, FromCachedBlockTransaction) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(0, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  auto cached_transaction = std::make_unique<CachedBlockTransaction>(
      Transaction::TakeBlockReservations(std::move(transaction_or.value())));

  transaction_or =
      minfs.ContinueTransaction(kTotalElements - kDefaultElements, std::move(cached_transaction));
  ASSERT_TRUE(transaction_or.is_ok());
  ASSERT_EQ(transaction_or->block_reservation().GetReserved(), kTotalElements);
}

TEST(TransactionTest, FromCachedBlockTransactionFailsToExtend) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(0, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  auto cached_transaction = std::make_unique<CachedBlockTransaction>(
      Transaction::TakeBlockReservations(std::move(transaction_or.value())));

  transaction_or = minfs.ContinueTransaction(kTotalElements + 1 - kDefaultElements,
                                             std::move(cached_transaction));
  ASSERT_TRUE(transaction_or.is_error());
}

// Attempts to create a transaction with more than the maximum available inodes reserved.
TEST(TransactionTest, CreateTransactionTooManyInodesFails) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(kTotalElements + 1, 0);
  ASSERT_EQ(transaction_or.status_value(), ZX_ERR_NO_SPACE);
}

// Attempts to create a transaction with more than the maximum available blocks reserved.
TEST(TransactionTest, CreateTransactionTooManyBlocksFails) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(0, kTotalElements + 1);
  ASSERT_EQ(transaction_or.status_value(), ZX_ERR_NO_SPACE);
}

// Attempts to reserve an blocks and inodes and then try to take only block reservation.
TEST(TransactionDeathTest, TakeBlockReservationsWithInodeReservationDies) {
  FakeMinfs minfs;
  ASSERT_DEATH(
      {
        auto transaction_or = minfs.CreateTransaction(kDefaultElements, kDefaultElements);
        ASSERT_TRUE(transaction_or.is_ok());
        [[maybe_unused]] auto result =
            Transaction::TakeBlockReservations(std::move(transaction_or.value()));
      },
      _);
}

// Tests allocation of a single inode.
TEST(TransactionTest, InodeAllocationSucceeds) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(kDefaultElements, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  transaction_or->AllocateInode();
}

// Tests allocation of a single block.
TEST(TransactionTest, BlockAllocationSucceeds) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(kDefaultElements, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  transaction_or->AllocateBlock();
}

// Attempts to allocate an inode when the transaction was not initialized properly.
TEST(TransactionDeathTest, AllocateInodeWithoutInitializationFails) {
  FakeMinfs minfs;
  Transaction transaction(&minfs);
  ASSERT_DEATH(transaction.AllocateInode(), _);
}

// Attempts to allocate a block when the transaction was not initialized properly.
TEST(TransactionDeathTest, AllocateBlockWithoutInitializationFails) {
  FakeMinfs minfs;
  Transaction transaction(&minfs);
  ASSERT_DEATH(transaction.AllocateBlock(), _);
}

#if ZX_DEBUG_ASSERT_IMPLEMENTED
// Attempts to allocate an inode when none have been reserved.
TEST(TransactionDeathTest, AllocateTooManyInodesFails) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(1, 0);
  ASSERT_TRUE(transaction_or.is_ok());

  // First allocation should succeed.
  transaction_or->AllocateInode();

  // Second allocation should fail.
  ASSERT_DEATH(transaction_or->AllocateInode(), _);
}
#endif

#if ZX_DEBUG_ASSERT_IMPLEMENTED
// Attempts to allocate a block when none have been reserved.
TEST(TransactionDeathTest, AllocateTooManyBlocksFails) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(0, 1);
  ASSERT_TRUE(transaction_or.is_ok());

  // First allocation should succeed.
  transaction_or->AllocateBlock();

  // Second allocation should fail.
  ASSERT_DEATH(transaction_or->AllocateBlock(), _);
}
#endif

// Checks that the Transaction's work is empty before any writes have been enqueued.
TEST(TransactionTest, VerifyNoWorkExistsBeforeEnqueue) {
  FakeMinfs minfs;
  Transaction transaction(&minfs);

  // Metadata operations should be empty.
  std::vector<storage::UnbufferedOperation> meta_operations =
      transaction.RemoveMetadataOperations();
  ASSERT_TRUE(meta_operations.empty());

  // Data work should be empty.
  std::vector<storage::UnbufferedOperation> data_operations = transaction.RemoveDataOperations();
  ASSERT_TRUE(data_operations.empty());
}

// Checks that the Transaction's metadata work is populated after enqueueing metadata writes.
TEST(TransactionTest, EnqueueAndVerifyMetadataWork) {
  FakeMinfs minfs;
  Transaction transaction(&minfs);

  storage::Operation op = {
      .type = storage::OperationType::kWrite,
      .vmo_offset = 2,
      .dev_offset = 3,
      .length = 4,
  };
  UnownedVmoBuffer buffer(zx::unowned_vmo(1));
  transaction.EnqueueMetadata(op, &buffer);

  std::vector<storage::UnbufferedOperation> meta_operations =
      transaction.RemoveMetadataOperations();
  ASSERT_EQ(meta_operations.size(), 1ul);
  ASSERT_EQ(1, meta_operations[0].vmo);
  ASSERT_EQ(meta_operations[0].op.vmo_offset, 2ul);
  ASSERT_EQ(meta_operations[0].op.dev_offset, 3ul);
  ASSERT_EQ(meta_operations[0].op.length, 4ul);
  ASSERT_EQ(storage::OperationType::kWrite, meta_operations[0].op.type);
}

// Checks that the Transaction's data work is populated after enqueueing data writes.
TEST(TransactionTest, EnqueueAndVerifyDataWork) {
  FakeMinfs minfs;
  Transaction transaction(&minfs);

  storage::Operation op = {
      .type = storage::OperationType::kWrite,
      .vmo_offset = 2,
      .dev_offset = 3,
      .length = 4,
  };
  UnownedVmoBuffer buffer(zx::unowned_vmo(1));
  transaction.EnqueueData(op, &buffer);

  std::vector<storage::UnbufferedOperation> data_operations = transaction.RemoveDataOperations();
  ASSERT_EQ(data_operations.size(), 1ul);
  ASSERT_EQ(1, data_operations[0].vmo);
  ASSERT_EQ(data_operations[0].op.vmo_offset, 2ul);
  ASSERT_EQ(data_operations[0].op.dev_offset, 3ul);
  ASSERT_EQ(data_operations[0].op.length, 4ul);
  ASSERT_EQ(storage::OperationType::kWrite, data_operations[0].op.type);
}

class MockVnodeMinfs : public VnodeMinfs, public fbl::Recyclable<MockVnodeMinfs> {
 public:
  explicit MockVnodeMinfs(bool* alive) : VnodeMinfs(nullptr), alive_(alive) { *alive_ = true; }

  ~MockVnodeMinfs() override { *alive_ = false; }

  // Required for memory management, see the class comment above Vnode for more.
  void fbl_recycle() { RecycleNode(); }

 private:
  bool IsDirectory() const final { return false; }
  zx::result<> CanUnlink() const final { return zx::error(ZX_ERR_NOT_SUPPORTED); }

  // minfs::Vnode interface.
  blk_t GetBlockCount() const final { return 0; }
  uint64_t GetSize() const final { return 0; }
  void SetSize(uint32_t new_size) final {}
  void AcquireWritableBlock(Transaction* transaction, blk_t local_bno, blk_t old_bno,
                            blk_t* out_bno) final {}
  void DeleteBlock(PendingWork* transaction, blk_t local_bno, blk_t old_bno, bool indirect) final {}
  void IssueWriteback(Transaction* transaction, blk_t vmo_offset, blk_t dev_offset,
                      blk_t count) final {}
  bool HasPendingAllocation(blk_t vmo_offset) final { return false; }
  void CancelPendingWriteback() final {}
  bool DirtyCacheEnabled() const final { return false; }
  bool IsDirty() const final { return false; }
  zx::result<> FlushCachedWrites() final { return zx::ok(); }
  void DropCachedWrites() final {}

  // fs::Vnode interface.
  fs::VnodeProtocolSet GetProtocols() const final { return fs::VnodeProtocol::kFile; }
  zx_status_t Read(void* data, size_t len, size_t off, size_t* out_actual) final { return ZX_OK; }
  zx_status_t Write(const void* data, size_t len, size_t offset, size_t* out_actual) final {
    return ZX_OK;
  }
  zx_status_t Append(const void* data, size_t len, size_t* out_end, size_t* out_actual) final {
    return ZX_OK;
  }
  zx_status_t Truncate(size_t len) final { return ZX_OK; }

  bool* alive_;
};

// Checks that a pinned vnode is not attached to the transaction's data work.
TEST(TransactionTest, RemovePinnedVnodeContainsVnode) {
  FakeMinfs minfs;
  bool vnode_alive = false;

  fbl::RefPtr<MockVnodeMinfs> vnode(fbl::AdoptRef(new MockVnodeMinfs(&vnode_alive)));
  ASSERT_TRUE(vnode_alive);

  Transaction transaction(&minfs);
  transaction.PinVnode(std::move(vnode));
  ASSERT_EQ(nullptr, vnode);

  std::vector<fbl::RefPtr<VnodeMinfs>> pinned_vnodes = transaction.RemovePinnedVnodes();
  ASSERT_EQ(pinned_vnodes.size(), 1ul);

  pinned_vnodes.clear();
  ASSERT_FALSE(vnode_alive);
}

TEST(TransactionTest, RemovePinnedVnodeContainsManyVnodes) {
  FakeMinfs minfs;
  size_t vnode_count = 4;
  bool vnode_alive[vnode_count];
  fbl::RefPtr<MockVnodeMinfs> vnodes[vnode_count];
  Transaction transaction(&minfs);

  for (size_t i = 0; i < vnode_count; i++) {
    vnode_alive[i] = false;
    vnodes[i] = fbl::AdoptRef(new MockVnodeMinfs(&vnode_alive[i]));
    ASSERT_TRUE(vnode_alive[i]);
    transaction.PinVnode(std::move(vnodes[i]));
    ASSERT_EQ(nullptr, vnodes[i]);
  }

  std::vector<fbl::RefPtr<VnodeMinfs>> pinned_vnodes = transaction.RemovePinnedVnodes();
  ASSERT_EQ(vnode_count, pinned_vnodes.size());

  pinned_vnodes.clear();

  for (size_t i = 0; i < vnode_count; i++) {
    ASSERT_FALSE(vnode_alive[i]);
  }
}

TEST(CachedBlockTransactionTest, FromZeroBlockReservation) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(0, 0);
  ASSERT_TRUE(transaction_or.is_ok());
  CachedBlockTransaction cached_transaction(
      Transaction::TakeBlockReservations(std::move(transaction_or.value())));
  ASSERT_EQ(cached_transaction.TakeBlockReservations()->GetReserved(), 0ul);
}

TEST(CachedBlockTransactionTest, FewBlocksReserved) {
  FakeMinfs minfs;
  auto transaction_or = minfs.CreateTransaction(0, kDefaultElements);
  ASSERT_TRUE(transaction_or.is_ok());
  CachedBlockTransaction cached_transaction(
      Transaction::TakeBlockReservations(std::move(transaction_or.value())));
  ASSERT_EQ(cached_transaction.TakeBlockReservations()->GetReserved(), kDefaultElements);
}

}  // namespace
}  // namespace minfs
