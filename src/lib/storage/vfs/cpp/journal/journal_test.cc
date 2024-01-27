// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/journal/journal.h"

#include <lib/cksum.h>
#include <lib/sync/completion.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <safemath/checked_math.h>
#include <storage/buffer/owned_vmoid.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/lib/storage/vfs/cpp/journal/format.h"
#include "src/lib/storage/vfs/cpp/journal/header_view.h"
#include "src/lib/storage/vfs/cpp/journal/initializer.h"
#include "src/lib/storage/vfs/cpp/journal/replay.h"
#include "src/lib/storage/vfs/cpp/transaction/device_transaction_handler.h"

namespace fs {
namespace {

using ::testing::ElementsAreArray;

const vmoid_t kJournalVmoid = 1;
const vmoid_t kWritebackVmoid = 2;
const vmoid_t kInfoVmoid = 3;
const vmoid_t kOtherVmoid = 4;
const size_t kJournalLength = 10;
const size_t kWritebackLength = 10;
const uint32_t kBlockSize = 8192;

enum class EscapedBlocks {
  kVerified,
  kIgnored,
};

// Verifies that |length| blocks of |expected| exist within |buffer| at block |buffer_offset|.
void CheckCircularBufferContents(const zx::vmo& buffer, size_t buffer_blocks, size_t buffer_offset,
                                 const zx::vmo& expected, size_t expected_offset, size_t length,
                                 EscapedBlocks escape) {
  const size_t buffer_start = kBlockSize * buffer_offset;
  const size_t buffer_capacity = kBlockSize * buffer_blocks;
  for (size_t i = 0; i < length; i++) {
    std::array<char, kBlockSize> buffer_buf{};
    size_t offset = (buffer_start + kBlockSize * i) % buffer_capacity;
    ASSERT_EQ(buffer.read(buffer_buf.data(), offset, kBlockSize), ZX_OK);

    std::array<char, kBlockSize> expected_buf{};
    offset = (expected_offset + i) * kBlockSize;
    ASSERT_EQ(expected.read(expected_buf.data(), offset, kBlockSize), ZX_OK);

    if (escape == EscapedBlocks::kVerified &&
        *reinterpret_cast<uint64_t*>(expected_buf.data()) == kJournalEntryMagic) {
      constexpr size_t kSkip = sizeof(kJournalEntryMagic);
      std::array<char, kSkip> skip_buffer{};
      EXPECT_EQ(memcmp(skip_buffer.data(), buffer_buf.data(), kSkip), 0);
      EXPECT_EQ(memcmp(buffer_buf.data() + kSkip, expected_buf.data() + kSkip, kBlockSize - kSkip),
                0);
    } else {
      EXPECT_EQ(memcmp(expected_buf.data(), buffer_buf.data(), kBlockSize), 0);
    }
  }
}

void CopyBytes(const zx::vmo& source, const zx::vmo& destination, uint64_t offset,
               uint64_t length) {
  std::vector<uint8_t> buffer(length, 0);
  EXPECT_EQ(source.read(buffer.data(), offset, length), ZX_OK);
  EXPECT_EQ(destination.write(buffer.data(), offset, length), ZX_OK);
}

// The collection of all behaviors which are used by the journaling subsystem, and which are
// registered with the underlying block device.
struct JournalBuffers {
  zx::vmo journal_vmo;
  zx::vmo writeback_vmo;
  zx::vmo info_vmo;
};

// Identifies if the buffer is the in-memory version of the buffer (accessed directly by the journal
// code) or the on-disk representation (used by the test to represent all operations which have been
// transacted to disk).
enum class BufferType {
  kDiskBuffer,
  kMemoryBuffer,
};

// A mock VMO reigstry, which acts as the holder for all VMOs used by the journaling codebase to
// interact with the underlying device.
//
// In addition to the storage::VmoidRegistry interface, provides some additional utilities for
// buffer generation and verification.
class MockVmoidRegistry : public storage::VmoidRegistry {
 public:
  // Sets the next Vmoid which will be allocated when "BlockAttachVmo" is invoked.
  void SetNextVmoid(vmoid_t vmoid) { next_vmoid_ = vmoid; }

  // Initializes a storage::VmoBuffer with |length| blocks, pre-allocated to deterministic data.
  storage::VmoBuffer InitializeBuffer(size_t num_blocks) {
    storage::VmoBuffer buffer;
    SetNextVmoid(kOtherVmoid);
    EXPECT_EQ(buffer.Initialize(this, num_blocks, kBlockSize, "test-buffer"), ZX_OK);
    for (size_t i = 0; i < num_blocks; i++) {
      memset(buffer.Data(i), static_cast<uint8_t>(i), kBlockSize);
    }
    return buffer;
  }

  // Verifies that "replaying the journal" would result in the provided set of
  // |expected_operations|, with the corresponding |expected_sequence_number|.
  void VerifyReplay(const std::vector<storage::UnbufferedOperation>& expected_operations,
                    uint64_t expected_sequence_number);

  // Access VMOs by registered VMO ID.
  //
  // Callers may request the "in-memory" version or the "disk-based" version, storing the results of
  // all transacted write operations.
  const zx::vmo& GetVmo(vmoid_t vmoid, BufferType buffer);

  // Initializes |disk_buffers_| by copying the in-memory copies.
  void CreateDiskVmos();

  // Access the "disk-based" version of each buffer.
  const zx::vmo& journal() const { return disk_buffers_.journal_vmo; }
  const zx::vmo& writeback() const { return disk_buffers_.writeback_vmo; }
  const zx::vmo& info() const { return disk_buffers_.info_vmo; }

  // storage::VmoidRegistry interface:

  zx_status_t BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out) final;

  zx_status_t BlockDetachVmo(storage::Vmoid vmoid) final {
    [[maybe_unused]] vmoid_t id = vmoid.TakeId();
    return ZX_OK;
  }

 private:
  // Using the disk-based journal and info buffers attached to the registry, parse their contents as
  // if executing a replay operation.
  //
  // This allows us to exercise the integration of the "journal writeback" and the on reboot
  // "journal replay".
  void Replay(std::vector<storage::BufferedOperation>* operations, uint64_t* sequence_number);

  JournalBuffers memory_buffers_;
  JournalBuffers disk_buffers_;
  vmoid_t next_vmoid_ = BLOCK_VMOID_INVALID;
};

void MockVmoidRegistry::VerifyReplay(
    const std::vector<storage::UnbufferedOperation>& expected_operations,
    uint64_t expected_sequence_number) {
  std::vector<storage::BufferedOperation> operations;
  uint64_t sequence_number = 0;
  ASSERT_NO_FATAL_FAILURE(Replay(&operations, &sequence_number));
  EXPECT_EQ(expected_sequence_number, sequence_number);
  ASSERT_EQ(expected_operations.size(), operations.size());

  for (size_t i = 0; i < expected_operations.size(); i++) {
    EXPECT_EQ(expected_operations[i].op.type, operations[i].op.type);
    EXPECT_EQ(expected_operations[i].op.length, operations[i].op.length);
    EXPECT_EQ(expected_operations[i].op.dev_offset, operations[i].op.dev_offset);
    CheckCircularBufferContents(journal(), kJournalLength, operations[i].op.vmo_offset,
                                *expected_operations[i].vmo, expected_operations[i].op.vmo_offset,
                                expected_operations[i].op.length, EscapedBlocks::kVerified);
  }
}

zx_status_t MockVmoidRegistry::BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out) {
  switch (next_vmoid_) {
    case kJournalVmoid:
      EXPECT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &memory_buffers_.journal_vmo), ZX_OK);
      break;
    case kWritebackVmoid:
      EXPECT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &memory_buffers_.writeback_vmo), ZX_OK);
      break;
    case kInfoVmoid:
      EXPECT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &memory_buffers_.info_vmo), ZX_OK);
      break;
  }
  *out = storage::Vmoid(next_vmoid_);
  return ZX_OK;
}

const zx::vmo& MockVmoidRegistry::GetVmo(vmoid_t vmoid, BufferType buffer) {
  switch (vmoid) {
    case kJournalVmoid:
      return buffer == BufferType::kDiskBuffer ? disk_buffers_.journal_vmo
                                               : memory_buffers_.journal_vmo;
    case kWritebackVmoid:
      return buffer == BufferType::kDiskBuffer ? disk_buffers_.writeback_vmo
                                               : memory_buffers_.writeback_vmo;
    case kInfoVmoid:
      return buffer == BufferType::kDiskBuffer ? disk_buffers_.info_vmo : memory_buffers_.info_vmo;
    default:
      ZX_ASSERT(false);
  }
}

void MockVmoidRegistry::CreateDiskVmos() {
  size_t size = 0;
  EXPECT_EQ(memory_buffers_.journal_vmo.get_size(&size), ZX_OK);
  EXPECT_EQ(zx::vmo::create(size, 0, &disk_buffers_.journal_vmo), ZX_OK);
  CopyBytes(memory_buffers_.journal_vmo, disk_buffers_.journal_vmo, 0, size);

  EXPECT_EQ(memory_buffers_.writeback_vmo.get_size(&size), ZX_OK);
  EXPECT_EQ(zx::vmo::create(size, 0, &disk_buffers_.writeback_vmo), ZX_OK);
  CopyBytes(memory_buffers_.writeback_vmo, disk_buffers_.writeback_vmo, 0, size);

  EXPECT_EQ(memory_buffers_.info_vmo.get_size(&size), ZX_OK);
  EXPECT_EQ(zx::vmo::create(size, 0, &disk_buffers_.info_vmo), ZX_OK);
  CopyBytes(memory_buffers_.info_vmo, disk_buffers_.info_vmo, 0, size);
}

void MockVmoidRegistry::Replay(std::vector<storage::BufferedOperation>* operations,
                               uint64_t* sequence_number) {
  zx::vmo info_vmo;
  ASSERT_EQ(disk_buffers_.info_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &info_vmo), ZX_OK);
  fzl::OwnedVmoMapper mapper;
  ASSERT_EQ(mapper.Map(std::move(info_vmo), kBlockSize), ZX_OK);
  auto info_buffer =
      std::make_unique<storage::VmoBuffer>(this, std::move(mapper), kInfoVmoid, 1, kBlockSize);
  JournalSuperblock superblock(std::move(info_buffer));

  // Create a clone of the journal, since escaped blocks may be modified. This allows the "clone" to
  // be modified while leaving the original journal untouched.
  zx::vmo journal_vmo;
  uint64_t length = kBlockSize * kJournalLength;
  ASSERT_EQ(disk_buffers_.journal_vmo.create_child(ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, 0,
                                                   length, &journal_vmo),
            ZX_OK);
  ASSERT_EQ(mapper.Map(std::move(journal_vmo), length), ZX_OK);
  storage::VmoBuffer journal_buffer(this, std::move(mapper), kJournalVmoid, kJournalLength,
                                    kBlockSize);

  uint64_t next_entry_start = 0;
  ASSERT_EQ(ParseJournalEntries(&superblock, &journal_buffer, operations, sequence_number,
                                &next_entry_start),
            ZX_OK);
}

// A transaction handler class, controlling all block device operations which are transmitted by the
// journaling code.
//
// In addition to the |TransactionHandler| interface, this class allows clients to supply a series
// of callbacks, controlling the exact sequence of operations which should be observed by the
// underlying device. These take the form of callbacks, which can allow test code to "pause and
// check state" in between each operation.
class MockTransactionHandler final : public fs::TransactionHandler {
 public:
  using TransactionCallback =
      fit::function<zx_status_t(const std::vector<storage::BufferedOperation>& requests)>;

  explicit MockTransactionHandler(MockVmoidRegistry* registry,
                                  TransactionCallback* callbacks = nullptr,
                                  size_t transactions_expected = 0)
      : registry_(registry), callbacks_(callbacks), transactions_expected_(transactions_expected) {}

  ~MockTransactionHandler() override { EXPECT_EQ(transactions_expected_, transactions_seen_); }

  // TransactionHandler interface:

  uint64_t BlockNumberToDevice(uint64_t block_num) const final { return block_num; }

  zx_status_t RunRequests(const std::vector<storage::BufferedOperation>& requests) final {
    EXPECT_LT(transactions_seen_, transactions_expected_);
    if (transactions_seen_ == transactions_expected_) {
      return ZX_ERR_BAD_STATE;
    }

    // Transfer all bytes from the in-memory representation of data to
    // the "on-disk" representation of data.
    for (const storage::BufferedOperation& request : requests) {
      if (request.op.type == storage::OperationType::kWrite) {
        CopyBytes(registry_->GetVmo(request.vmoid, BufferType::kMemoryBuffer),
                  registry_->GetVmo(request.vmoid, BufferType::kDiskBuffer),
                  request.op.vmo_offset * kBlockSize, request.op.length * kBlockSize);
      }
    }
    return callbacks_[transactions_seen_++](requests);
  }

  zx_status_t Flush() final {
    EXPECT_LT(transactions_seen_, transactions_expected_);
    if (transactions_seen_ == transactions_expected_) {
      return ZX_ERR_BAD_STATE;
    }
    return callbacks_[transactions_seen_++](GetFlushRequests());
  }

  static bool IsFlush(const std::vector<storage::BufferedOperation>& requests) {
    return &requests == &GetFlushRequests();
  }

 private:
  static const std::vector<storage::BufferedOperation>& GetFlushRequests() {
    static auto requests = new std::vector<storage::BufferedOperation>();
    return *requests;
  }

  MockVmoidRegistry* registry_ = nullptr;
  TransactionCallback* callbacks_ = nullptr;
  size_t transactions_expected_ = 0;
  size_t transactions_seen_ = 0;
};

zx_status_t FlushCallback(const std::vector<storage::BufferedOperation>& requests) {
  EXPECT_TRUE(MockTransactionHandler::IsFlush(requests));
  return ZX_OK;
}

// A test fixture which initializes structures that are necessary for journal initialization.
//
// This initialization is repeated between all tests, so it is deduplicated here. However, journal
// construction itself is still left to each individaul test, but the prerequisite structures can be
// "taken" from this fixture using the "take_*" methods below.
class JournalTest : public testing::Test {
 public:
  virtual size_t GetJournalLength() const { return kJournalLength; }

  void SetUp() override {
    registry_.SetNextVmoid(kJournalVmoid);
    ASSERT_EQ(storage::BlockingRingBuffer::Create(&registry_, GetJournalLength(), kBlockSize,
                                                  "journal-writeback-buffer", &journal_buffer_),
              ZX_OK);

    registry_.SetNextVmoid(kWritebackVmoid);
    ASSERT_EQ(storage::BlockingRingBuffer::Create(&registry_, kWritebackLength, kBlockSize,
                                                  "data-writeback-buffer", &data_buffer_),
              ZX_OK);

    auto info_block_buffer = std::make_unique<storage::VmoBuffer>();
    registry_.SetNextVmoid(kInfoVmoid);
    ASSERT_EQ(
        info_block_buffer->Initialize(&registry_, kJournalMetadataBlocks, kBlockSize, "info-block"),
        ZX_OK);
    info_block_ = JournalSuperblock(std::move(info_block_buffer));
    info_block_.Update(0, 0);

    ASSERT_NO_FATAL_FAILURE(registry_.CreateDiskVmos());
  }

  MockVmoidRegistry* registry() { return &registry_; }

  // The following methods take the object out of the fixture. They are typically used for journal
  // initialization.
  JournalSuperblock take_info() { return std::move(info_block_); }
  std::unique_ptr<storage::BlockingRingBuffer> take_journal_buffer() {
    return std::move(journal_buffer_);
  }
  std::unique_ptr<storage::BlockingRingBuffer> take_data_buffer() {
    return std::move(data_buffer_);
  }

 private:
  MockVmoidRegistry registry_;
  JournalSuperblock info_block_;
  std::unique_ptr<storage::BlockingRingBuffer> journal_buffer_;
  std::unique_ptr<storage::BlockingRingBuffer> data_buffer_;
};

// Verifies that the info block marks |start| as the beginning of the journal (relative to the start
// of entries) with a sequence_number of |sequence_number|.
void CheckInfoBlock(const zx::vmo& info, uint64_t start, uint64_t sequence_number) {
  std::array<char, kBlockSize> buf = {};
  EXPECT_EQ(info.read(buf.data(), 0, kBlockSize), ZX_OK);
  const JournalInfo& journal_info = *reinterpret_cast<const JournalInfo*>(buf.data());
  EXPECT_EQ(kJournalMagic, journal_info.magic);
  EXPECT_EQ(start, journal_info.start_block);
  EXPECT_EQ(sequence_number, journal_info.timestamp);
}

// Convenience function which verifies the fields of a write request.
void CheckWriteRequest(const storage::BufferedOperation& request, vmoid_t vmoid,
                       uint64_t vmo_offset, uint64_t dev_offset, uint64_t length) {
  EXPECT_EQ(vmoid, request.vmoid);
  EXPECT_EQ(storage::OperationType::kWrite, request.op.type);
  EXPECT_EQ(vmo_offset, request.op.vmo_offset);
  EXPECT_EQ(dev_offset, request.op.dev_offset);
  EXPECT_EQ(length, request.op.length);
}

// A convenience verification class which holds:
// - References to the info block, journal, and data writeback.
// - Offsets within those structures.
//
// Verifying something as simple as "is this data in the right buffer" is non-trivial, given that
// the operation may wrap around one of many buffers, at a difficult-to-predict offset.
//
// Tests typically use this class to validate both:
// - Incoming requests to the "block device" are consistent, and
// - Data from the original operation actually exists in the source buffer where it should.
class JournalRequestVerifier {
 public:
  JournalRequestVerifier(const zx::vmo& info_block, const zx::vmo& journal,
                         const zx::vmo& data_writeback, uint64_t journal_start_block)
      : info_block_(&info_block),
        journal_(&journal),
        data_writeback_(&data_writeback),
        journal_start_block_(journal_start_block) {}

  void SetJournalOffset(uint64_t offset) {
    ASSERT_LT(offset, kJournalLength);
    journal_offset_ = offset;
  }

  void ExtendJournalOffset(uint64_t operation_length) {
    journal_offset_ = (journal_offset_ + operation_length) % kJournalLength;
  }

  // Returns the on-disk journal offset, relative to |EntryStart()|.
  uint64_t JournalOffset() const { return journal_offset_; }

  void SetDataOffset(uint64_t offset) {
    ASSERT_LT(offset, kWritebackLength);
    data_offset_ = offset;
  }

  void ExtendDataOffset(uint64_t operation_length) {
    data_offset_ = (data_offset_ + operation_length) % kWritebackLength;
  }

  uint64_t DataOffset() const { return data_offset_; }

  // Verifies that |operation| matches |requests|, and exists within the data writeback buffer at
  // |DataOffset()|.
  void VerifyDataWrite(const storage::UnbufferedOperation& operation,
                       const std::vector<storage::BufferedOperation>& requests) const;

  // Verifies that |operation| matches |requests|, exists within the journal buffer at
  // |JournalOffset()|, and targets the on-device journal.
  void VerifyJournalWrite(const storage::UnbufferedOperation& operation,
                          const std::vector<storage::BufferedOperation>& requests) const;

  // Verifies that |operation| matches |requests|, exists within the journal buffer at
  // |JournalOffset() + kJournalEntryHeaderBlocks|, and targets the final on-disk location (not the
  // journal).
  void VerifyMetadataWrite(const storage::UnbufferedOperation& operation,
                           const std::vector<storage::BufferedOperation>& requests) const;

  // Verifies that the info block is targeted by |requests|, with |sequence_number|, and a start
  // block at |JournalOffset()|.
  void VerifyInfoBlockWrite(uint64_t sequence_number,
                            const std::vector<storage::BufferedOperation>& requests) const;

 private:
  void VerifyJournalRequest(uint64_t entry_length,
                            const std::vector<storage::BufferedOperation>& requests) const;
  uint64_t EntryStart() const { return journal_start_block_ + kJournalMetadataBlocks; }

  // VMO of the journal info block.
  const zx::vmo* info_block_;
  // VMO of the journal itself.
  const zx::vmo* journal_;
  // VMO for data writeback.
  const zx::vmo* data_writeback_;
  // Starting block of the journal.
  uint64_t journal_start_block_ = 0;
  // Offset within the journal at which requests will be verified.
  uint64_t journal_offset_ = 0;
  // Offset within the data buffer at which requests will be verified.
  uint64_t data_offset_ = 0;
};

void JournalRequestVerifier::VerifyDataWrite(
    const storage::UnbufferedOperation& operation,
    const std::vector<storage::BufferedOperation>& requests) const {
  EXPECT_GE(requests.size(), 1ul) << "Not enough operations";
  EXPECT_LE(requests.size(), 2ul) << "Too many operations";

  uint64_t total_length = operation.op.length;
  uint64_t pre_wrap_length = std::min(kWritebackLength - DataOffset(), total_length);
  uint64_t post_wrap_length = total_length - pre_wrap_length;

  ASSERT_NO_FATAL_FAILURE(CheckWriteRequest(requests[0], kWritebackVmoid,
                                            /* vmo_offset= */ DataOffset(),
                                            /* dev_offset= */ operation.op.dev_offset,
                                            /* length= */ pre_wrap_length));
  if (post_wrap_length > 0) {
    EXPECT_EQ(requests.size(), 2ul);
    ASSERT_NO_FATAL_FAILURE(
        CheckWriteRequest(requests[1], kWritebackVmoid,
                          /* vmo_offset= */ 0,
                          /* dev_offset= */ operation.op.dev_offset + pre_wrap_length,
                          /* length= */ post_wrap_length));
  }

  // Verify that the writeback buffer is full of the data we used earlier.
  ASSERT_NO_FATAL_FAILURE(CheckCircularBufferContents(*data_writeback_, kWritebackLength,
                                                      /* data_writeback_offset= */ DataOffset(),
                                                      /* buffer= */ *operation.vmo,
                                                      /* buffer_offset= */ operation.op.vmo_offset,
                                                      /* length= */ pre_wrap_length,
                                                      EscapedBlocks::kIgnored));
  if (post_wrap_length > 0) {
    EXPECT_EQ(requests.size(), 2ul);
    ASSERT_NO_FATAL_FAILURE(
        CheckCircularBufferContents(*data_writeback_, kWritebackLength,
                                    /* data_writeback_offset= */ 0,
                                    /* buffer= */ *operation.vmo,
                                    /* buffer_offset= */ operation.op.vmo_offset + pre_wrap_length,
                                    /* length= */ post_wrap_length, EscapedBlocks::kIgnored));
  }
}

void JournalRequestVerifier::VerifyJournalRequest(
    uint64_t entry_length, const std::vector<storage::BufferedOperation>& requests) const {
  // Verify the operation is from the metadata buffer, targeting the journal.
  EXPECT_GE(requests.size(), 1ul) << "Not enough operations";

  uint64_t journal_offset = JournalOffset();

  // Validate that all operations target the expected location within the on-disk journal.
  uint64_t blocks_written = 0;
  for (const storage::BufferedOperation& request : requests) {
    // Requests may be split to wrap around the in-memory or on-disk buffer.
    const uint64_t journal_dev_capacity = kJournalLength - journal_offset;
    const uint64_t journal_vmo_capacity = kJournalLength - request.op.vmo_offset;
    EXPECT_LE(request.op.length, journal_dev_capacity);
    EXPECT_LE(request.op.length, journal_vmo_capacity);

    EXPECT_EQ(kJournalVmoid, request.vmoid);
    EXPECT_EQ(storage::OperationType::kWrite, request.op.type);
    EXPECT_EQ(EntryStart() + journal_offset, request.op.dev_offset);

    blocks_written += request.op.length;
    journal_offset = (journal_offset + request.op.length) % kJournalLength;
  }
  EXPECT_EQ(entry_length, blocks_written);
}

void JournalRequestVerifier::VerifyJournalWrite(
    const storage::UnbufferedOperation& operation,
    const std::vector<storage::BufferedOperation>& requests) const {
  uint64_t entry_length = operation.op.length + kEntryMetadataBlocks;

  ASSERT_NO_FATAL_FAILURE(VerifyJournalRequest(entry_length, requests));

  // Validate that all operations exist within the journal buffer.
  uint64_t buffer_offset = operation.op.vmo_offset;
  for (size_t i = 0; i < requests.size(); ++i) {
    uint64_t vmo_offset = requests[i].op.vmo_offset;
    uint64_t length = requests[i].op.length;
    if (i == 0) {
      // Skip over header block.
      vmo_offset++;
      length--;
    }
    if (i == requests.size() - 1) {
      // Drop commit block.
      length--;
    }

    ASSERT_NO_FATAL_FAILURE(CheckCircularBufferContents(*journal_, kJournalLength,
                                                        /* journal_offset= */ vmo_offset,
                                                        /* buffer= */ *operation.vmo,
                                                        /* buffer_offset= */ buffer_offset,
                                                        /* length= */ length,
                                                        EscapedBlocks::kVerified));

    buffer_offset += length;
  }
}

void JournalRequestVerifier::VerifyMetadataWrite(
    const storage::UnbufferedOperation& operation,
    const std::vector<storage::BufferedOperation>& requests) const {
  // Verify the operation is from the metadata buffer, targeting the final location on disk.
  EXPECT_GE(requests.size(), 1ul) << "Not enough operations";

  uint64_t blocks_written = 0;
  for (const storage::BufferedOperation& request : requests) {
    // We only care about wraparound from the in-memory buffer here; any wraparound from the on-disk
    // journal is not relevant to the metadata writeback.
    const uint64_t journal_vmo_capacity = kJournalLength - request.op.vmo_offset;
    EXPECT_LE(request.op.length, journal_vmo_capacity);

    EXPECT_EQ(kJournalVmoid, request.vmoid);
    EXPECT_EQ(storage::OperationType::kWrite, request.op.type);
    EXPECT_EQ(operation.op.dev_offset + blocks_written, request.op.dev_offset);

    const uint64_t buffer_offset = operation.op.vmo_offset + blocks_written;
    ASSERT_NO_FATAL_FAILURE(CheckCircularBufferContents(*journal_, kJournalLength,
                                                        /* journal_offset= */ request.op.vmo_offset,
                                                        /* buffer= */ *operation.vmo,
                                                        /* buffer_offset= */ buffer_offset,
                                                        /* length= */ request.op.length,
                                                        EscapedBlocks::kIgnored));

    blocks_written += request.op.length;
  }
  EXPECT_EQ(operation.op.length, blocks_written);
}

void JournalRequestVerifier::VerifyInfoBlockWrite(
    uint64_t sequence_number, const std::vector<storage::BufferedOperation>& requests) const {
  // Verify that the operation is the info block, with a new start block.
  EXPECT_EQ(requests.size(), 1ul);
  ASSERT_NO_FATAL_FAILURE(CheckWriteRequest(requests[0],
                                            /* vmoid= */ kInfoVmoid,
                                            /* vmo_offset= */ 0,
                                            /* dev_offset= */ journal_start_block_,
                                            /* length= */ 1));
  ASSERT_NO_FATAL_FAILURE(CheckInfoBlock(*info_block_, JournalOffset(), sequence_number));
}

// Tests the constructor of the journal doesn't bother updating the info block on a zero-filled
// journal.
TEST_F(JournalTest, JournalConstructor) {
  MockTransactionHandler handler(registry());
  Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
  CheckInfoBlock(registry()->info(), /* start= */ 0, /* sequence_number= */ 0);
  uint64_t sequence_number = 0;
  registry()->VerifyReplay({}, sequence_number);
}

// Tests that calling |journal.Sync| will wait for the journal to complete, while
// generating no additional work (without concurrent metadata writes).
TEST_F(JournalTest, NoWorkSyncCompletesBeforeJournalDestruction) {
  MockTransactionHandler handler(registry());
  Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);

  sync_completion_t sync_completion;
  bool sync_completed = false;
  auto promise = journal.Sync().and_then([&] {
    sync_completed = true;
    sync_completion_signal(&sync_completion);
    return fpromise::ok();
  });

  ASSERT_FALSE(sync_completed);
  journal.schedule_task(std::move(promise));
  ASSERT_EQ(sync_completion_wait(&sync_completion, zx::duration::infinite().get()), ZX_OK);
  ASSERT_TRUE(sync_completed);
}

// Tests that Sync operations are flushed if the journal is destroyed.
TEST_F(JournalTest, NoWorkSyncCompletesOnDestruction) {
  bool sync_completed = false;

  {
    MockTransactionHandler handler(registry());
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    auto promise = journal.Sync().and_then([&] {
      sync_completed = true;
      return fpromise::ok();
    });

    ASSERT_FALSE(sync_completed);
    journal.schedule_task(std::move(promise));
  }
  ASSERT_TRUE(sync_completed);
}

// Tests that writing data to the journal is observable from the "block device".
TEST_F(JournalTest, WriteDataObserveTransaction) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(1);
  const std::vector<storage::UnbufferedOperation> operations = {
      {.vmo = zx::unowned_vmo(buffer.vmo().get()),
       .op =
           {
               .type = storage::OperationType::kWrite,
               .vmo_offset = 0,
               .dev_offset = 20,
               .length = 1,
           }},
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 30,
                  .length = 1,
              },
      },
  };

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyDataWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));

  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]},
                                         .data_promise = journal.WriteData({operations[0]}),
                                         .commit_callback =
                                             [&] {
                                               CheckInfoBlock(registry()->info(), /* start= */ 0,
                                                              /* sequence_number= */ 0);
                                             }}),
              ZX_OK);
  }
}

TEST_F(JournalTest, WriteNoDataSucceeds) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(1);
  const storage::UnbufferedOperation operation = {
      .vmo = zx::unowned_vmo(buffer.vmo().get()),
      .op =
          {
              .type = storage::OperationType::kWrite,
              .vmo_offset = 0,
              .dev_offset = 20,
              .length = 1,
          },
  };
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operation, requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operation, requests);
        verifier.ExtendJournalOffset(operation.op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  bool committed = false;
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operation},
                                         .data_promise = journal.WriteData({}),
                                         .commit_callback = [&] { committed = true; }}),
              ZX_OK);
  }
  EXPECT_TRUE(committed);
}

// Tests that writing metadata to the journal is observable from the "block device".
//
// Operation 1: [ H, 1, C, _, _, _, _, _, _, _ ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, WriteMetadataObserveTransactions) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(1);

  const storage::UnbufferedOperation operation = {
      .vmo = zx::unowned_vmo(metadata.vmo().get()),
      .op =
          {
              .type = storage::OperationType::kWrite,
              .vmo_offset = 0,
              .dev_offset = 20,
              .length = 1,
          },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);

  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operation, requests);

        // Verify that if we were to reboot now the operation would be replayed.
        uint64_t sequence_number = 1;
        registry()->VerifyReplay({operation}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operation, requests);
        verifier.ExtendJournalOffset(operation.op.length + kEntryMetadataBlocks);
        uint64_t sequence_number = 1;
        registry()->VerifyReplay({operation}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};

  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operation}}), ZX_OK);
  }
}

// Tests that multiple metadata operations can be written to the journal.
//
// Operation 1: [ H, 1, C, _, _, _, _, _, _, _ ]
// Operation 2: [ _, _, _, H, 1, C, _, _, _, _ ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, WriteMultipleMetadataOperationsObserveTransactions) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(3);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        registry()->VerifyReplay(operations, 2);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, 2);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Tests that TrimData() is observable from the "block device".
TEST_F(JournalTest, TrimDataObserveTransaction) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(1);
  const storage::UnbufferedOperation metadata_operation = {
      .vmo = zx::unowned_vmo(metadata.vmo().get()),
      .op =
          {
              .type = storage::OperationType::kWrite,
              .vmo_offset = 0,
              .dev_offset = 20,
              .length = 1,
          },
  };
  const storage::BufferedOperation trim = {
      .op =
          {
              .type = storage::OperationType::kTrim,
              .vmo_offset = 0,
              .dev_offset = 20,
              .length = 5,
          },
  };
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  bool trim_received = false;
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(metadata_operation, requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        if (requests.size() != 1) {
          ADD_FAILURE() << "Unexpected count";
          return ZX_ERR_OUT_OF_RANGE;
        }
        EXPECT_EQ(storage::OperationType::kTrim, requests[0].op.type);
        EXPECT_EQ(requests[0].op.dev_offset, 20ul);
        EXPECT_EQ(requests[0].op.length, 5ul);
        trim_received = true;
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(metadata_operation, requests);
        verifier.ExtendJournalOffset(metadata_operation.op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));

  bool commit_callback_received = false;
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {metadata_operation},
                                         .trim = {trim},
                                         .commit_callback =
                                             [&] {
                                               EXPECT_TRUE(trim_received);
                                               CheckInfoBlock(registry()->info(), /* start= */ 0,
                                                              /* sequence_number= */ 0);
                                               commit_callback_received = true;
                                             }}),
              ZX_OK);
  }
  EXPECT_TRUE(commit_callback_received);
}

// Tests that the info block is not updated if it doesn't need to be updated.
//
// Operation 1: [ H, 1, 2, 3, 4, 5, C, _, _, _ ]
// Operation 2: [ _, _, _, _, _, _, _, H, 1, C ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, WriteExactlyFullJournalDoesNotUpdateInfoBlock) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 5,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  ASSERT_EQ(kJournalLength,
            2 * kEntryMetadataBlocks + operations[0].op.length + operations[1].op.length)
      << "Operations should just fill the journal (no early info writeback)";

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        uint64_t sequence_number = 2;
        registry()->VerifyReplay(operations, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Tests that the info block is updated after the journal is completely full.
//
// This acts as a regression test against a bug where "the journal was exactly full" appeared the
// same as "the journal is exactly empty" when making the decision to write back the info block.
//
// Operation 0: [ H, 1, 2, 3, 4, 5, 6, 7, 8, C ]
// Operation 1: [ H, 1, C, _, _, _, _, _, _, _ ]
//            : Info block update promted by operation 1.
TEST_F(JournalTest, WriteExactlyFullJournalDoesNotUpdateInfoBlockUntilNewOperationArrives) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 8,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  ASSERT_EQ(kJournalLength, kEntryMetadataBlocks + operations[0].op.length)
      << "Operations should just fill the journal (no early info writeback)";

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  uint64_t sequence_number = 0;
  MockTransactionHandler::TransactionCallback callbacks[] = {
      // Operation 0 written.
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        sequence_number++;
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        registry()->VerifyReplay({operations[0]}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      // Operation 1 written. This prompts the info block to be updated.
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        sequence_number++;
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        registry()->VerifyReplay({operations[1]}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      // Info block written on journal termination.
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Tests that the info block is updated if a metadata write would invalidate the entry pointed to by
// "start block".
//
// Operation 1: [ H, 1, 2, 3, 4, 5, 6, C, _, _ ]
//            : Info block update prompted by op 2.
// Operation 2: [ C, _, _, _, _, _, _, _, H, 1 ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, WriteToOverfilledJournalUpdatesInfoBlock) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 6,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  ASSERT_EQ(kJournalLength + 1,
            2 * kEntryMetadataBlocks + operations[0].op.length + operations[1].op.length)
      << "Operations should just barely overfill the journal to cause info writeback";

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);

        // Before we update the info block, check that a power failure would result in only the
        // second metadata operation being replayed.
        //
        // The first operation has already completed and peristed thanks to the earlier info block
        // update.
        uint64_t sequence_number = 2;
        registry()->VerifyReplay({operations[1]}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);

        // After we update the info block, check that a power failure would result in no operations
        // being replayed - this equivalent to the "clean shutdown" case, where there should be no
        // work to do on reboot.
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Tests that metadata updates still operate successfully if the commit block wraps around the
// journal.
//
// Operation 1: [ H, 1, 2, 3, 4, 5, 6, C, _, _ ]
//            : Info block written by explicit sync
// Operation 2: [ C, _, _, _, _, _, _, _, H, 1 ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, JournalWritesCausingCommitBlockWraparound) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 6,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);

        // Before we update the info block, check that a power failure would result in only the
        // second metadata operation being replayed.
        //
        // The first operation has already completed and peristed thanks to the earlier info block
        // update.
        uint64_t sequence_number = 2;
        registry()->VerifyReplay({operations[1]}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);

        // After we update the info block, check that a power failure would result in no operations
        // being replayed - this equivalent to the "clean shutdown" case, where there should be no
        // work to do on reboot.
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Tests that metadata updates still operate successfully if the commit block and entry wrap around
// the journal.
//
// Operation 1: [ H, 1, 2, 3, 4, 5, 6, 7, C, _ ]
//            : Info block written by explicit sync
// Operation 2: [ 1, C, _, _, _, _, _, _, _, H ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, JournalWritesCausingCommitAndEntryWraparound) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 7,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);

        // Before we update the info block, check that a power failure would result in only the
        // second metadata operation being replayed.
        //
        // The first operation has already completed and peristed thanks to the earlier info block
        // update.
        uint64_t sequence_number = 2;
        registry()->VerifyReplay({operations[1]}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);

        // After we update the info block, check that a power failure would result in no operations
        // being replayed - this equivalent to the "clean shutdown" case, where there should be no
        // work to do on reboot.
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    journal.schedule_task(journal.Sync());
    // This write will block until the prevoius operation completes.
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Writes operations where the in-memory and on-disk representation are not aligned.
// - In-memory buffer ahead of on-disk buffer, and
// - On-disk buffer ahead of in-memory buffer.
//
// Operation 0: [ _, _, _, H, 1, C, _, _, _, _ ] (In-memory)
// Operation 0: [ H, 1, C, _, _, _, _, _, _, _ ] (On-disk)
// Operation 1: [ H, 1, C, _, _, _, _, _, _, _ ] (In-memory)
// Operation 1: [ _, _, _, H, 1, C, _, _, _, _ ] (On-disk)
TEST_F(JournalTest, MetadataOnDiskOrderNotMatchingInMemoryOrder) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 4567,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyJournalWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyJournalWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyMetadataWrite(operations[0], requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyMetadataWrite(operations[1], requests);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  std::unique_ptr<storage::BlockingRingBuffer> journal_buffer = take_journal_buffer();
  internal::JournalWriter writer(&handler, take_info(), kJournalStartBlock,
                                 journal_buffer->capacity());

  // Reserve operations[1] in memory before operations[0].
  //
  // This means that in-memory, operations[1] wraps around the internal buffer.
  storage::BlockingRingBufferReservation reservation0, reservation1;
  uint64_t block_count1 = operations[1].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count1, &reservation1), ZX_OK);
  uint64_t block_count0 = operations[0].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count0, &reservation0), ZX_OK);

  // Actually write operations[0] before operations[1].
  std::vector<storage::BufferedOperation> buffered_operations0;
  ASSERT_TRUE(reservation0
                  .CopyRequests(cpp20::span(&operations[0], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations0)
                  .is_ok());
  auto result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation0), std::move(buffered_operations0)), {});
  ASSERT_TRUE(result.is_ok());

  std::vector<storage::BufferedOperation> buffered_operations1;
  ASSERT_TRUE(reservation1
                  .CopyRequests(cpp20::span(&operations[1], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations1)
                  .is_ok());
  result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation1), std::move(buffered_operations1)), {});
  ASSERT_TRUE(result.is_ok());

  ASSERT_TRUE(writer.Flush().is_ok());
}

// Writes operations with:
// - In-memory wraparound, but no on-disk wraparound, and
// - On-disk wraparound, but no in-memory wraparound.
//
// Operation 0: [ H, 1, 2, 3, 4, 5, 6, 7, C, _ ]
//            : Info block written by wraparound
// Operation 1: [ _, _, H, 1, C, _, _, _, _, _ ] (In-memory)
// Operation 1: [ 1, C, _, _, _, _, _, _, _, H ] (On-disk)
// Operation 2: [ 1, C, _, _, _, _, _, _, _, H ] (In-memory)
// Operation 2: [ _, _, H, 1, C, _, _, _, _, _ ] (On-disk)
TEST_F(JournalTest, MetadataOnDiskOrderNotMatchingInMemoryOrderWraparound) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 7,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 4567,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // Operation 1: [ _, _, H, 1, C, _, _, _, _, _ ] (In-memory)
        // Operation 1: [ 1, C, _, _, _, _, _, _, _, H ] (On-disk)
        //
        // This operation writes "H", then "1, C".
        EXPECT_EQ(requests.size(), 2ul);
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // Operation 2: [ 1, C, _, _, _, _, _, _, _, H ] (In-memory)
        // Operation 2: [ _, _, H, 1, C, _, _, _, _, _ ] (On-disk)
        //
        // This operation writes "H", then "1, C".
        EXPECT_EQ(requests.size(), 2ul);
        verifier.VerifyJournalWrite(operations[2], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyMetadataWrite(operations[2], requests);
        verifier.ExtendJournalOffset(operations[2].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  std::unique_ptr<storage::BlockingRingBuffer> journal_buffer = take_journal_buffer();
  internal::JournalWriter writer(&handler, take_info(), kJournalStartBlock,
                                 journal_buffer->capacity());

  // Issue the first operation, so the next operation will wrap around.
  storage::BlockingRingBufferReservation reservation;
  std::vector<storage::BufferedOperation> buffered_operations;
  uint64_t block_count = operations[0].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count, &reservation), ZX_OK);
  ASSERT_TRUE(reservation
                  .CopyRequests(cpp20::span(&operations[0], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations)
                  .is_ok());
  auto result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation), std::move(buffered_operations)), {});
  ASSERT_TRUE(result.is_ok());
  ASSERT_TRUE(writer.Flush().is_ok());

  // Reserve operations[2] in memory before operations[1].
  //
  // This means that in-memory, operations[2] wraps around the internal buffer.
  storage::BlockingRingBufferReservation reservation1, reservation2;
  uint64_t block_count2 = operations[2].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count2, &reservation2), ZX_OK);
  uint64_t block_count1 = operations[1].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count1, &reservation1), ZX_OK);

  // Actually write operations[1] before operations[2].
  //
  // This means that on-disk, operations[1] wraps around the journal.
  std::vector<storage::BufferedOperation> buffered_operations1;
  ASSERT_TRUE(reservation1
                  .CopyRequests(cpp20::span(&operations[1], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations1)
                  .is_ok());
  result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation1), std::move(buffered_operations1)), {});
  ASSERT_TRUE(result.is_ok());
  ASSERT_TRUE(writer.Flush().is_ok());

  std::vector<storage::BufferedOperation> buffered_operations2;
  ASSERT_TRUE(reservation2
                  .CopyRequests(cpp20::span(&operations[2], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations2)
                  .is_ok());
  result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation2), std::move(buffered_operations2)), {});
  ASSERT_TRUE(result.is_ok());
  ASSERT_TRUE(writer.Flush().is_ok());
}

// Tests that the in-memory writeback buffer for metadata and the on-disk buffer for metadata can
// both wraparound at different offsets.
//
// Operation 0: [ H, 1, 2, 3, 4, C, _, _, _, _ ]
// Operation _: [ _, _, _, _, _, _, X, X, X, _ ] (In-memory, reserved then released)
//            : Info block written by wraparound
// Operation 1: [ 1, 2, 3, 4, C, _, _, _, _, H ] (In-memory)
// Operation 1: [ 4, C, _, _, _, _, H, 1, 2, 3 ] (On-disk)
TEST_F(JournalTest, MetadataOnDiskAndInMemoryWraparoundAtDifferentOffsets) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 4,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 1234,
                  .length = 4,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // "H", then "1, 2, 3", then "4, C".
        EXPECT_EQ(requests.size(), 3ul);
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // "1, 2, 3, 4" are contiguous in the in-memory buffer.
        EXPECT_EQ(requests.size(), 1ul);
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  std::unique_ptr<storage::BlockingRingBuffer> journal_buffer = take_journal_buffer();
  internal::JournalWriter writer(&handler, take_info(), kJournalStartBlock,
                                 journal_buffer->capacity());

  // Issue the first operation, so the next operation will wrap around.
  storage::BlockingRingBufferReservation reservation;
  std::vector<storage::BufferedOperation> buffered_operations;
  uint64_t block_count = operations[0].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count, &reservation), ZX_OK);
  ASSERT_TRUE(reservation
                  .CopyRequests(cpp20::span(&operations[0], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations)
                  .is_ok());
  auto result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation), std::move(buffered_operations)), {});
  ASSERT_TRUE(result.is_ok());
  ASSERT_TRUE(writer.Flush().is_ok());

  storage::BlockingRingBufferReservation reservation_unused;
  ASSERT_EQ(journal_buffer->Reserve(3, &reservation_unused), ZX_OK);
  block_count = operations[1].op.length + kEntryMetadataBlocks;
  ASSERT_EQ(journal_buffer->Reserve(block_count, &reservation), ZX_OK);

  ASSERT_TRUE(reservation
                  .CopyRequests(cpp20::span(&operations[1], 1), kJournalEntryHeaderBlocks,
                                &buffered_operations)
                  .is_ok());
  result = writer.WriteMetadata(
      internal::JournalWorkItem(std::move(reservation), std::move(buffered_operations)), {});
  ASSERT_TRUE(result.is_ok());
  ASSERT_TRUE(writer.Flush().is_ok());
}

// Tests that writing "block N" to metadata before "block N" to data will revoke the block before
// data is written to the underlying device.
TEST_F(JournalTest, WriteSameBlockMetadataThenDataRevokesBlock) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);
  storage::VmoBuffer buffer = registry()->InitializeBuffer(5);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 10,
                  .length = 3,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 10,
                  .length = 3,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 3,
              },
      }};

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // This info block is written before a data operation to intentionally avoid replaying the
        // metadata operation on reboot.
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyDataWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[2], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[2], requests);
        verifier.ExtendJournalOffset(operations[2].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    sync_completion_t sync;
    EXPECT_EQ(
        journal.CommitTransaction({.metadata_operations = {operations[0]},
                                   .commit_callback = [&]() { sync_completion_signal(&sync); }}),
        ZX_OK);
    journal.schedule_task(journal.Sync());
    sync_completion_wait(&sync, ZX_TIME_INFINITE);
    EXPECT_EQ(journal.CommitTransaction({
                  .metadata_operations = {operations[2]},
                  .data_promise = journal.WriteData({operations[1]}),
              }),
              ZX_OK);
  }
}

// Tests that writing "block N" to metadata before "block M" to data will not revoke the block
// before data is written to the underlying device (For N != M).
TEST_F(JournalTest, WriteDifferentBlockMetadataThenDataDoesNotRevoke) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);
  storage::VmoBuffer buffer = registry()->InitializeBuffer(5);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 10,
                  .length = 3,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 3,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 30,
                  .length = 3,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  uint64_t sequence_number = 0;
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // Since the metadata and data regions do not overlap, we're fine letting the metadata
        // operation replay: it won't overwrite our data operation.
        registry()->VerifyReplay({operations[0]}, ++sequence_number);
        verifier.VerifyDataWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[2], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[2], requests);
        verifier.ExtendJournalOffset(operations[2].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        ++sequence_number;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({
                  .metadata_operations = {operations[2]},
                  .data_promise = journal.WriteData({operations[1]}),
              }),
              ZX_OK);
  }
}

// Tests that metadata updates still operate successfully if an entire entry wraps around the
// journal.
//
// Operation 1: [ H, 1, 2, 3, 4, 5, 6, 7, 8, C ]
//            : Info block written by explicit sync
// Operation 2: [ H, 1, C, _, _, _, _, _, _, _ ]
//            : Info block update prompted by termination.
TEST_F(JournalTest, JournalWritesCausingEntireEntryWraparound) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(kJournalLength);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 8,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);

        // Before we update the info block, check that a power failure would result in only the
        // second metadata operation being replayed.
        //
        // The first operation has already completed and peristed thanks to the earlier info block
        // update.
        uint64_t sequence_number = 2;
        registry()->VerifyReplay({operations[1]}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);

        // After we update the info block, check that a power failure would result in no operations
        // being replayed - this equivalent to the "clean shutdown" case, where there should be no
        // work to do on reboot.
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}), ZX_OK);
  }
}

// Tests that data writes are not ordered at the time "WriteData" is invoked.
TEST_F(JournalTest, DataOperationsAreNotOrderedGlobally) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(5);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 2,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 1,
                  .dev_offset = 200,
                  .length = 3,
              },
      },
  };

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.SetDataOffset(operations[0].op.length);
        verifier.VerifyDataWrite(operations[1], requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.SetDataOffset(0);
        verifier.VerifyDataWrite(operations[0], requests);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));

  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);

    // Although we "WriteData" in a particular order, we can "and-then" data to force an arbitrary
    // order that we want. This is visible in the transaction callbacks, where we notice
    // "operations[1]" before "operations[0]".
    auto first_promise = journal.WriteData({operations[0]});
    auto second_promise = journal.WriteData({operations[1]});

    journal.schedule_task(second_promise.and_then(std::move(first_promise)));
  }
}

// Tests a pretty common operation from a client point-of-view: order data operations around
// completion of a metadata update.
TEST_F(JournalTest, DataOperationsCanBeOrderedAroundMetadata) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(5);

  // We're using the same source buffer, but use:
  // - operations[0] as data
  // - operations[1] as metadata
  // - operations[2] as data
  // - operations[3] as metadata
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 1,
                  .dev_offset = 200,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 2000,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 3000,
                  .length = 1,
              },
      },
  };

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      // Operation[0]: Data.
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyDataWrite(operations[0], requests);
        verifier.ExtendDataOffset(operations[0].op.length);
        return ZX_OK;
      },
      // Operation[2]: Data.
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyDataWrite(operations[2], requests);
        verifier.ExtendDataOffset(operations[2].op.length);
        return ZX_OK;
      },
      FlushCallback,
      // Operation[1]: Metadata (journal).
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      // Operation[3]: Metadata (journal).
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[3], requests);
        verifier.ExtendJournalOffset(operations[3].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[3], requests);
        return ZX_OK;
      },
      FlushCallback,
      // Final operation: Updating the info block on journal teardown.
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 2;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));

  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    EXPECT_EQ(journal.CommitTransaction({
                  .metadata_operations = {operations[1]},
                  .data_promise = journal.WriteData({operations[0]}),
              }),
              ZX_OK);
    EXPECT_EQ(journal.CommitTransaction({
                  .metadata_operations = {operations[3]},
                  .data_promise = journal.WriteData({operations[2]}),
              }),
              ZX_OK);
  }
}

// Tests that many data operations, which overfill the writeback buffer, will cause subsequent
// requests to block.
TEST_F(JournalTest, WritingDataToFullBufferBlocksCaller) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(kWritebackLength);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 9,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 2,
              },
      },

  };

  ASSERT_EQ(kWritebackLength + 1, operations[0].op.length + operations[1].op.length)
      << "Operations should slightly overflow the data buffer";

  // Was operations[0] completed (received by transaction handler)?
  std::atomic<bool> op0_completed = false;
  // Was operations[1] successfully written to the buffer (WriteData promise created)?
  std::atomic<bool> op1_written = false;

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_FALSE(op0_completed);
        EXPECT_FALSE(op1_written);
        verifier.VerifyDataWrite(operations[0], requests);
        verifier.ExtendDataOffset(operations[0].op.length);
        op0_completed = true;
        return ZX_OK;
      },
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_TRUE(op0_completed);
        EXPECT_TRUE(op1_written);
        verifier.VerifyDataWrite(operations[1], requests);
        verifier.ExtendDataOffset(operations[1].op.length);
        return ZX_OK;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));

  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);

    auto promise0 = journal.WriteData({operations[0]});
    journal.schedule_task(std::move(promise0));

    // Start a background thread attempting to write operation[1]. It should block until
    // operations[0] has completed.
    std::thread worker([&]() {
      auto promise1 = journal.WriteData({operations[1]});
      if (!op0_completed) {
        fprintf(stderr, "Expected operation 0 to complete before operation 1\n");
        return;
      }
      op1_written = true;
      journal.schedule_task(std::move(promise1));
    });
    worker.join();
  }
  EXPECT_TRUE(op0_completed);
  EXPECT_TRUE(op1_written);
}

// Tests that sync after invoking |WriteData| waits for that data to be flushed to disk.
TEST_F(JournalTest, SyncAfterWritingDataWaitsForData) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(1);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 30,
                  .length = 1,
              },
      }};

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);

  std::atomic<bool> data_written = false;
  std::atomic<bool> sync_called = false;
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // While writing the data, we expect the sync callback to be waiting.
        EXPECT_FALSE(data_written);
        EXPECT_FALSE(sync_called);
        verifier.VerifyDataWrite(operations[0], requests);
        data_written = true;
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[1], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[1], requests);
        verifier.ExtendJournalOffset(operations[1].op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));

  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    EXPECT_EQ(journal.CommitTransaction({
                  .metadata_operations = {operations[1]},
                  .data_promise = journal.WriteData({operations[0]}),
              }),
              ZX_OK);
    journal.schedule_task(journal.Sync().and_then([&]() {
      // If sync has completed, we expect the data to have been written successfully.
      EXPECT_TRUE(data_written);
      EXPECT_FALSE(sync_called);
      sync_called = true;
      return fpromise::ok();
    }));
  }
  EXPECT_TRUE(data_written);
  EXPECT_TRUE(sync_called);
}

// Tests that sync after invoking |WriteMetadata| waits for that data to be flushed to disk.
TEST_F(JournalTest, SyncAfterWritingMetadataWaitsForMetadata) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(1);
  const storage::UnbufferedOperation operation = {
      .vmo = zx::unowned_vmo(buffer.vmo().get()),
      .op =
          {
              .type = storage::OperationType::kWrite,
              .vmo_offset = 0,
              .dev_offset = 20,
              .length = 1,
          },
  };

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);

  std::atomic<bool> metadata_written = false;
  std::atomic<bool> sync_called = false;
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operation, requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operation, requests);
        verifier.ExtendJournalOffset(operation.op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        EXPECT_FALSE(metadata_written);
        EXPECT_FALSE(sync_called);
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        metadata_written = true;
        return ZX_OK;
      }};
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);

    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operation}}), ZX_OK);
    journal.schedule_task(journal.Sync().and_then([&]() {
      // If sync has completed, we expect the metadata to have been written successfully.
      EXPECT_TRUE(metadata_written);
      EXPECT_FALSE(sync_called);
      sync_called = true;
      return fpromise::ok();
    }));
  }
  EXPECT_TRUE(metadata_written);
  EXPECT_TRUE(sync_called);
}

// Tests that operations which won't fit in data writeback will fail.
TEST_F(JournalTest, DataOperationTooLargeToFitInWritebackFails) {
  const uint64_t kBufferLength = kWritebackLength + 1;
  storage::VmoBuffer buffer = registry()->InitializeBuffer(kBufferLength);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = kBufferLength,
              },
      },
  };

  zx_status_t data_status = ZX_OK;
  MockTransactionHandler handler(registry());
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    auto promise = journal.WriteData({operations[0]}).or_else([&](zx_status_t& status) {
      data_status = status;
    });
    journal.schedule_task(std::move(promise));
  }

  EXPECT_EQ(data_status, ZX_ERR_NO_SPACE);
}

// Tests that operations which won't fit in metadata writeback will fail.
TEST_F(JournalTest, MetadataOperationTooLargeToFitInJournalFails) {
  const uint64_t kBufferLength = kJournalLength + 1;
  storage::VmoBuffer buffer = registry()->InitializeBuffer(kBufferLength);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = kBufferLength,
              },
      },
  };

  MockTransactionHandler handler(registry());
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_ERR_NO_SPACE);
  }
}

// Tests that when data operations fail, subsequent operations also fail to avoid leaving the device
// in an inconsistent state.
TEST_F(JournalTest, DataWriteFailureFailsSubsequentRequests) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(5);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 200,
                  .length = 1,
              },
      },
  };

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyDataWrite(operations[0], requests);
        verifier.ExtendDataOffset(operations[0].op.length);
        // Validate the request, but cause it to fail.
        return ZX_ERR_IO;
      },
  };
  std::atomic<bool> first_operation_failed = false;
  std::atomic<bool> second_operation_failed = false;

  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
    auto promise = journal.WriteData({operations[0]})
                       .then([&](fpromise::result<void, zx_status_t>& result) {
                         EXPECT_EQ(result.error(), ZX_ERR_IO)
                             << "operations[0] should fail with ZX_ERR_IO";
                         first_operation_failed = true;
                         return journal.WriteData({operations[1]});
                       })
                       .or_else([&](zx_status_t& status) {
                         EXPECT_EQ(status, ZX_ERR_IO_REFUSED);
                         second_operation_failed = true;
                         return fpromise::error(status);
                       });
    journal.schedule_task(std::move(promise));
  }

  EXPECT_TRUE(first_operation_failed);
  EXPECT_TRUE(second_operation_failed);
}

// Tests that when data operations fail, sync can still complete with a failed result.
TEST_F(JournalTest, DataWriteFailureStillLetsSyncComplete) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(5);
  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(buffer.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
  };

  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), 0);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyDataWrite(operations[0], requests);
        verifier.ExtendDataOffset(operations[0].op.length);
        // Validate the request, but cause it to fail.
        return ZX_ERR_IO;
      },
  };

  std::atomic<bool> sync_done = false;
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);

    auto data_promise = journal.WriteData({operations[0]});
    auto sync_promise = journal.Sync().then(
        [&](fpromise::result<void, zx_status_t>& result) -> fpromise::result<void, zx_status_t> {
          EXPECT_EQ(result.error(), ZX_ERR_IO_REFUSED);
          sync_done = true;
          return fpromise::ok();
        });
    journal.schedule_task(std::move(data_promise));
    journal.schedule_task(std::move(sync_promise));
  }

  EXPECT_TRUE(sync_done);
}

// Tests that when metadata operations fail, subsequent operations also fail to avoid leaving the
// device in an inconsistent state.
//
// Tests a failure which occurs when writing metadata to journal itself.
TEST_F(JournalTest, JournalWriteFailureFailsSubsequentRequests) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(3);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_ERR_IO;
      },
  };
  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    sync_completion_t sync_completion;
    journal.schedule_task(
        journal.Sync().inspect([&](const fpromise::result<void, zx_status_t>& result) {
          EXPECT_TRUE(result.is_error());
          if (result.is_error()) {
            EXPECT_EQ(result.error(), ZX_ERR_IO_REFUSED);
          }
          sync_completion_signal(&sync_completion);
        }));
    sync_completion_wait(&sync_completion, ZX_TIME_INFINITE);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}),
              ZX_ERR_IO_REFUSED);
  }
}

// Tests that when metadata operations fail, subsequent operations also fail to avoid leaving the
// device in an inconsistent state.
//
// Tests a failure which occurs when writing metadata to the final on-disk location (non-journal).
TEST_F(JournalTest, MetadataWriteFailureFailsSubsequentRequests) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(3);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 2,
                  .dev_offset = 1234,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        return ZX_ERR_IO;
      },
  };

  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]}}), ZX_OK);
    sync_completion_t sync;
    journal.schedule_task(journal.Sync().then(
        [&](const fpromise::result<void, zx_status_t>& result) { sync_completion_signal(&sync); }));
    sync_completion_wait(&sync, ZX_TIME_INFINITE);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}),
              ZX_ERR_IO_REFUSED);
  }
}

// Tests that when info block operations fail, subsequent operations also fail to avoid leaving the
// device in an inconsistent state.
//
// - Write Metadata (OK, but causes a delayed info block writeback)
// - Sync (cause info block writeback to happen, where it fails)
// - Write Metadata (fails, because info block writeback failed earlier)
TEST_F(JournalTest, InfoBlockWriteFailureFailsSubsequentRequests) {
  storage::VmoBuffer metadata = registry()->InitializeBuffer(3);

  const std::vector<storage::UnbufferedOperation> operations = {
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 20,
                  .length = 1,
              },
      },
      {
          .vmo = zx::unowned_vmo(metadata.vmo().get()),
          .op =
              {
                  .type = storage::OperationType::kWrite,
                  .vmo_offset = 0,
                  .dev_offset = 200,
                  .length = 1,
              },
      },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);
  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyJournalWrite(operations[0], requests);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operations[0], requests);
        verifier.ExtendJournalOffset(operations[0].op.length + kEntryMetadataBlocks);
        // At this point, the metadata operation will succeed.
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        // This will fail the sync, but not the write request.
        return ZX_ERR_IO;
      }};

  std::atomic<bool> write_ok = false;
  std::atomic<bool> sync_failed = false;

  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[0]},
                                         .commit_callback = [&] { write_ok = true; }}),
              ZX_OK);
    journal.schedule_task(journal.Sync().then([&](fpromise::result<void, zx_status_t>& result) {
      // Failure triggered by the info block writeback.
      EXPECT_EQ(result.error(), ZX_ERR_IO);
      sync_failed = true;
      EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operations[1]}}),
                ZX_ERR_IO_REFUSED);
    }));
  }

  EXPECT_TRUE(write_ok);
  EXPECT_TRUE(sync_failed);
}

// Tests that payload blocks which could be parsed as journal metadata are escaped.
//
// If the following metadata is written:
//  Operation:
//                   [1, 2, 3]
//  Journal:
//      [ _, _, _, H, 1, 2, 3, C, _, _ ]
//
// and continued operations occur, such that the header is overwritten, and the info block is
// updated:
//
//           New Operation
//           |
//      [ _, H, x, C, 1, 2, 3, C, _, _ ]
//
// Normally, the data would be invalid by the checksum, and ignored:
//
//      [ _, H, x, C, _, _, _, _, _, _ ]
//
// Resulting in replaying one operaton.
//
// However, if "[1, 2, 3]" actually sets block "1" to a valid header block, and block "3" to a valid
// commit block, the journal would look like the following:
//
//      [ _, H, x, C, H, 2, C, _, _, _ ]
//
// This would result in TWO operations being replayed, where the second could contain arbitrary
// data.
//
// To avoid this case, the journal converts payload blocks with "header entry magic" to a form that
// drops them on replay.
TEST_F(JournalTest, PayloadBlocksWithJournalMagicAreEscaped) {
  // Create an operation which will become escaped when written by the journal.
  storage::VmoBuffer metadata = registry()->InitializeBuffer(1);
  *reinterpret_cast<uint64_t*>(metadata.Data(0)) = kJournalEntryMagic;
  const storage::UnbufferedOperation operation = {
      .vmo = zx::unowned_vmo(metadata.vmo().get()),
      .op =
          {
              .type = storage::OperationType::kWrite,
              .vmo_offset = 0,
              .dev_offset = 20,
              .length = 1,
          },
  };

  constexpr uint64_t kJournalStartBlock = 55;
  JournalRequestVerifier verifier(registry()->info(), registry()->journal(),
                                  registry()->writeback(), kJournalStartBlock);

  MockTransactionHandler::TransactionCallback callbacks[] = {
      [&](const std::vector<storage::BufferedOperation>& requests) {
        // Verify the operation is first issued to the on-disk journal.
        verifier.VerifyJournalWrite(operation, requests);

        // Verify that the payload is escaped in the journal.
        std::array<char, kBlockSize> buffer = {};
        uint64_t offset = (verifier.JournalOffset() + kJournalEntryHeaderBlocks) * kBlockSize;
        uint64_t length = kBlockSize;
        EXPECT_EQ(registry()->journal().read(buffer.data(), offset, length), ZX_OK);
        EXPECT_NE(memcmp(metadata.Data(0), buffer.data(), kBlockSize), 0)
            << "metadata should have been escaped (modified)";

        // Verify that if we were to reboot now the operation would be replayed.
        uint64_t sequence_number = 1;
        registry()->VerifyReplay({operation}, sequence_number);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        verifier.VerifyMetadataWrite(operation, requests);

        // Verify that the payload is NOT escaped when writing to the final location.
        std::array<char, kBlockSize> buffer = {};
        uint64_t offset = (verifier.JournalOffset() + kJournalEntryHeaderBlocks) * kBlockSize;
        uint64_t length = kBlockSize;
        EXPECT_EQ(registry()->journal().read(buffer.data(), offset, length), ZX_OK);
        EXPECT_THAT(cpp20::span(static_cast<const uint8_t*>(metadata.Data(0)), kBlockSize),
                    ElementsAreArray(buffer.data(), kBlockSize))
            << "Metadata should only be escaped in the journal";

        verifier.ExtendJournalOffset(operation.op.length + kEntryMetadataBlocks);
        return ZX_OK;
      },
      FlushCallback,
      [&](const std::vector<storage::BufferedOperation>& requests) {
        uint64_t sequence_number = 1;
        verifier.VerifyInfoBlockWrite(sequence_number, requests);
        registry()->VerifyReplay({}, sequence_number);
        return ZX_OK;
      }};

  MockTransactionHandler handler(registry(), callbacks, std::size(callbacks));
  {
    Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(),
                    kJournalStartBlock);
    EXPECT_EQ(journal.CommitTransaction({.metadata_operations = {operation}}), ZX_OK);
  }
}

TEST_F(JournalTest, WriteMetadataWithBadBlockCountFails) {
  MockTransactionHandler handler(registry(), {}, 0);
  Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
  std::vector<storage::UnbufferedOperation> operations = {
      storage::UnbufferedOperation{.op = {.type = storage::OperationType::kWrite, .length = 10}},
      storage::UnbufferedOperation{.op = {.type = storage::OperationType::kWrite,
                                          .length = std::numeric_limits<uint64_t>::max() - 10}}};
  EXPECT_EQ(journal.CommitTransaction({.metadata_operations = std::move(operations)}),
            ZX_ERR_OUT_OF_RANGE);
}

class JournalTestWithLargeBuffer : public JournalTest {
 public:
  size_t GetJournalLength() const override { return 1024; }
};

class StubTransactionHandler final : public fs::TransactionHandler {
 public:
  uint64_t BlockNumberToDevice(uint64_t block_num) const final { return block_num; }
  zx_status_t RunRequests(const std::vector<storage::BufferedOperation>& requests) final {
    return ZX_OK;
  }
  zx_status_t Flush() final { return ZX_OK; }
};

TEST_F(JournalTestWithLargeBuffer, ExactlyMaxBlockDescriptorsSucceeds) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(1);
  const std::vector<storage::UnbufferedOperation> operations(
      kMaxBlockDescriptors, {
                                .vmo = zx::unowned_vmo(buffer.vmo().get()),
                                .op =
                                    {
                                        .type = storage::OperationType::kWrite,
                                        .vmo_offset = 0,
                                        .dev_offset = 20,
                                        .length = 1,
                                    },
                            });
  StubTransactionHandler handler;
  Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
  EXPECT_EQ(journal.CommitTransaction({.metadata_operations = operations}), ZX_OK);
}

TEST_F(JournalTestWithLargeBuffer, MoreThanMaxBlockDescriptorsFails) {
  storage::VmoBuffer buffer = registry()->InitializeBuffer(1);
  const std::vector<storage::UnbufferedOperation> operations(
      kMaxBlockDescriptors + 1, {
                                    .vmo = zx::unowned_vmo(buffer.vmo().get()),
                                    .op =
                                        {
                                            .type = storage::OperationType::kWrite,
                                            .vmo_offset = 0,
                                            .dev_offset = 20,
                                            .length = 1,
                                        },
                                });
  StubTransactionHandler handler;
  Journal journal(&handler, take_info(), take_journal_buffer(), take_data_buffer(), 0);
  EXPECT_EQ(journal.CommitTransaction({.metadata_operations = operations}), ZX_ERR_NO_SPACE);
}

class TestTransactionHandler : public DeviceTransactionHandler {
 public:
  TestTransactionHandler(block_client::BlockDevice& device) : device_(device) {}

  block_client::BlockDevice* GetDevice() override { return &device_; }
  uint64_t BlockNumberToDevice(uint64_t block_num) const override {
    return block_num * kJournalBlockSize / kBlockSize;
  }

 private:
  block_client::BlockDevice& device_;
};

std::unique_ptr<Journal> CreateJournal(TestTransactionHandler& handler) {
  // Create the buffers we need for the journal.
  std::unique_ptr<storage::BlockingRingBuffer> journal_buffer, data_buffer;
  ZX_ASSERT(storage::BlockingRingBuffer::Create(handler.GetDevice(), kJournalLength, kBlockSize,
                                                "journal-writeback-buffer",
                                                &journal_buffer) == ZX_OK);
  ZX_ASSERT(storage::BlockingRingBuffer::Create(handler.GetDevice(), kWritebackLength, kBlockSize,
                                                "data-writeback-buffer", &data_buffer) == ZX_OK);
  auto info_block_buffer = std::make_unique<storage::VmoBuffer>();
  ZX_ASSERT(info_block_buffer->Initialize(handler.GetDevice(), kJournalMetadataBlocks, kBlockSize,
                                          "info-block") == ZX_OK);
  JournalSuperblock info_block(std::move(info_block_buffer));
  info_block.Update(0, 0);

  // Write the super-block to the device.
  block_fifo_request_t requests[] = {{
                                         .command = {.opcode = BLOCK_OPCODE_WRITE, .flags = 0},
                                         .vmoid = info_block.buffer().vmoid(),
                                         .length = kJournalMetadataBlocks,
                                     },
                                     {
                                         .command = {.opcode = BLOCK_OPCODE_FLUSH, .flags = 0},
                                     }};
  EXPECT_EQ(handler.GetDevice()->FifoTransaction(requests, 2), ZX_OK);

  return std::make_unique<Journal>(&handler, std::move(info_block), std::move(journal_buffer),
                                   std::move(data_buffer),
                                   /*journal_start_block=*/0);
}

TEST(JournalCallbackTest, CommitCallbackTriggeredAtCorrectTime) {
  constexpr int kBlockCount = 100;
  block_client::FakeBlockDevice device(kBlockCount, kBlockSize);
  TestTransactionHandler handler(device);

  // Keep track of all the requests so that we can replay them later and potentially in a different
  // order.
  std::vector<block_fifo_request_t> requests;
  storage::VmoBuffer replay_buffer;
  int replay_blocks = 0;
  ASSERT_EQ(replay_buffer.Initialize(&device, 100, kBlockSize, "test-buffer"), ZX_OK);
  device.set_hook([&](const block_fifo_request_t& request, const zx::vmo* vmo) {
    switch (request.command.opcode) {
      case BLOCK_OPCODE_WRITE: {
        vmo->read(replay_buffer.Data(replay_blocks), request.vmo_offset * kBlockSize,
                  request.length * kBlockSize);
        block_fifo_request_t replay_request = request;
        replay_request.vmoid = replay_buffer.vmoid();
        replay_request.vmo_offset = replay_blocks;
        replay_blocks += request.length;
        requests.push_back(replay_request);
        break;
      }
      case BLOCK_OPCODE_FLUSH:
        requests.push_back(request);
        break;
    }
    return ZX_OK;
  });

  storage::VmoBuffer test_buffer;
  ASSERT_EQ(test_buffer.Initialize(&device, 5, kBlockSize, "test-buffer"), ZX_OK);
  // Set up the test buffer so the first byte in the first three blocks is 1, 2 & 3 respectively
  *static_cast<char*>(test_buffer.Data(0)) = 1;
  *static_cast<char*>(test_buffer.Data(1)) = 2;
  *static_cast<char*>(test_buffer.Data(2)) = 3;

  // Now run through a simple transaction involving a metadata write, followed by a write issued in
  // the commit callback that should only be visible if the metadata write is committed.
  {
    std::unique_ptr<Journal> journal = CreateJournal(handler);

    EXPECT_EQ(journal->CommitTransaction(
                  {.metadata_operations = {{
                                               .vmo = zx::unowned_vmo(test_buffer.vmo().get()),
                                               .op =
                                                   {
                                                       .type = storage::OperationType::kWrite,
                                                       .vmo_offset = 0,
                                                       .dev_offset = 20,
                                                       .length = 1,
                                                   },
                                           },
                                           {
                                               .vmo = zx::unowned_vmo(test_buffer.vmo().get()),
                                               .op =
                                                   {
                                                       .type = storage::OperationType::kWrite,
                                                       .vmo_offset = 1,
                                                       .dev_offset = 21,
                                                       .length = 1,
                                                   },
                                           }},
                   .commit_callback =
                       [&]() {
                         block_fifo_request_t request = {
                             .command = {.opcode = BLOCK_OPCODE_WRITE, .flags = 0},
                             .vmoid = test_buffer.vmoid(),
                             .length = 1,
                             .vmo_offset = 2,
                             .dev_offset = 22,
                         };
                         EXPECT_EQ(device.FifoTransaction(&request, 1), ZX_OK);
                       }}),
              ZX_OK);
  }

  // The commit callback is only supposed to be called at the point where writes that follow are
  // guaranteed to be visible after the transaction.  Now, depending on which writes persist, here
  // are the legal combinations:
  //
  // Block 20 | Block 21 | Block 22 |
  //    0     |    0     |    0     |
  //    1     |    2     |    0     |
  //    1     |    2     |    3     |
  //
  // All other combinations are illegal.

  device.set_hook({});

  for (int passes = 0;;) {
    // Repeatedly replay the writes but stop at different points.
    for (size_t stop_at = 1; stop_at < requests.size(); ++stop_at) {
      device.Wipe();
      EXPECT_EQ(device.FifoTransaction(requests.data(), stop_at), ZX_OK);

      // Replay the journal and check we see one of the legal combinations mentioned above.
      auto result = ReplayJournal(&handler, &device, 0, kJournalLength, kJournalBlockSize);
      EXPECT_TRUE(result.is_ok());

      block_fifo_request_t request = {
          .command = {.opcode = BLOCK_OPCODE_READ, .flags = 0},
          .vmoid = test_buffer.vmoid(),
          .length = 3,
          .dev_offset = 20,
      };
      EXPECT_EQ(device.FifoTransaction(&request, 1), ZX_OK);
      const int data = *static_cast<char*>(test_buffer.Data(0)) << 16 |
                       *static_cast<char*>(test_buffer.Data(1)) << 8 |
                       *static_cast<char*>(test_buffer.Data(2));
      std::cout << "Found " << std::setfill('0') << std::setw(6) << std::hex << data << std::endl;
      switch (data) {
        case 0x000000:
        case 0x010200:
        case 0x010203:
          break;
        default:
          ADD_FAILURE() << "passes=" << passes << ", stop_at=" << stop_at
                        << ", data=" << std::setfill('0') << std::setw(6) << std::hex << data;
      }
    }

    if (++passes == 2)
      break;

    // Now reverse the order of the writes between the flush calls, and we should see the same
    // results.
    for (auto iter = requests.begin(), sequence_start = iter;; ++iter) {
      if (iter == requests.end() || (iter->command.opcode) == BLOCK_OPCODE_FLUSH) {
        std::reverse(sequence_start, iter);
        if (iter == requests.end())
          break;
        sequence_start = iter + 1;
      }
    }
  }
}

TEST(JournalCallbackTest, CompleteCallbackTriggeredAtCorrectTime) {
  constexpr int kBlockCount = 100;
  block_client::FakeBlockDevice device(kBlockCount, kBlockSize);
  TestTransactionHandler handler(device);
  storage::VmoBuffer test_buffer;
  ASSERT_EQ(test_buffer.Initialize(&device, 5, kBlockSize, "test-buffer"), ZX_OK);
  // Set up the test buffer so the first byte in the first two blocks is 1 & 2 respectively
  *static_cast<char*>(test_buffer.Data(0)) = 1;
  *static_cast<char*>(test_buffer.Data(1)) = 2;
  bool commit_callback_received = false, complete_callback_received = false;
  {
    std::unique_ptr<Journal> journal = CreateJournal(handler);

    // At the point at which we receive the callback, we should be able to read the metadata changes
    // from their final locations.
    EXPECT_EQ(journal->CommitTransaction(
                  {.metadata_operations = {{
                                               .vmo = zx::unowned_vmo(test_buffer.vmo().get()),
                                               .op =
                                                   {
                                                       .type = storage::OperationType::kWrite,
                                                       .vmo_offset = 0,
                                                       .dev_offset = 20,
                                                       .length = 1,
                                                   },
                                           },
                                           {
                                               .vmo = zx::unowned_vmo(test_buffer.vmo().get()),
                                               .op =
                                                   {
                                                       .type = storage::OperationType::kWrite,
                                                       .vmo_offset = 1,
                                                       .dev_offset = 21,
                                                       .length = 1,
                                                   },
                                           }},
                   .commit_callback = [&] { commit_callback_received = true; },
                   .complete_callback =
                       [&] {
                         // This isn't actually important, but happens to be true and will likely
                         // always be true.
                         EXPECT_TRUE(commit_callback_received);

                         block_fifo_request_t request = {
                             .command = {.opcode = BLOCK_OPCODE_READ, .flags = 0},
                             .vmoid = test_buffer.vmoid(),
                             .length = 2,
                             .vmo_offset = 2,
                             .dev_offset = 20,
                         };
                         EXPECT_EQ(device.FifoTransaction(&request, 1), ZX_OK);
                         EXPECT_EQ(*static_cast<char*>(test_buffer.Data(2)), 1);
                         EXPECT_EQ(*static_cast<char*>(test_buffer.Data(3)), 2);
                         complete_callback_received = true;
                       }}),
              ZX_OK);
  }
  EXPECT_TRUE(complete_callback_received);
}

TEST(JournalSimpleTest, EmptyOperationWithDataOrTrimReturnsError) {
  constexpr int kBlockCount = 100;
  block_client::FakeBlockDevice device(kBlockCount, kBlockSize);
  TestTransactionHandler handler(device);
  std::unique_ptr<Journal> journal = CreateJournal(handler);
  EXPECT_EQ(journal->CommitTransaction(
                {.data_promise = fpromise::make_promise(
                     []() -> fpromise::result<void, zx_status_t> { return fpromise::ok(); })}),
            ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(journal->CommitTransaction({.trim = {{}}}), ZX_ERR_INVALID_ARGS);
}

zx_status_t MakeJournalHelper(uint8_t* dest_buffer, uint64_t blocks, uint64_t block_size) {
  fs::WriteBlocksFn write_blocks_fn = [dest_buffer, blocks, block_size](
                                          cpp20::span<const uint8_t> buffer, uint64_t block_offset,
                                          uint64_t block_count) {
    EXPECT_GE(buffer.size(), block_count * block_size);

    uint64_t max_offset =
        safemath::CheckMul(safemath::CheckAdd(block_offset, block_count).ValueOrDie(),
                           kJournalBlockSize)
            .ValueOrDie();
    uint64_t device_max_offset = safemath::CheckMul(blocks, block_size).ValueOrDie();

    if (device_max_offset < max_offset) {
      return ZX_ERR_IO_OVERRUN;
    }
    std::memcpy(&dest_buffer[block_offset * block_size], buffer.data(),
                block_count * kJournalBlockSize);

    return ZX_OK;
  };

  return fs::MakeJournal(blocks, write_blocks_fn);
}

TEST(MakeJournal, ValidArgs) {
  constexpr uint64_t kBlockCount = 10;
  uint8_t blocks[kBlockCount * fs::kJournalBlockSize];

  ASSERT_EQ(MakeJournalHelper(blocks, kBlockCount, fs::kJournalBlockSize), ZX_OK);
  auto info = reinterpret_cast<JournalInfo*>(blocks);
  ASSERT_EQ(kJournalMagic, info->magic);
  ASSERT_EQ(info->reserved, 0ul);
  ASSERT_EQ(info->start_block, 0ul);
  ASSERT_EQ(info->timestamp, 0ul);

  auto csum = info->checksum;
  info->checksum = 0;
  ASSERT_EQ(csum, crc32(0, blocks, sizeof(JournalInfo)));
  for (uint64_t i = sizeof(JournalInfo); i < sizeof(blocks); i++) {
    ASSERT_EQ(0, blocks[i]);
  }
}

TEST(MakeJournal, SmallBuffer) {
  constexpr uint64_t kBlockCount = 1;
  uint8_t blocks[kBlockCount * (fs::kJournalBlockSize - 1)];

  ASSERT_EQ(MakeJournalHelper(blocks, kBlockCount, fs::kJournalBlockSize - 1), ZX_ERR_IO_OVERRUN);
}

// TODO(fxbug.dev/34548): Test abandoning promises. This may require additional barrier support.

}  // namespace
}  // namespace fs
