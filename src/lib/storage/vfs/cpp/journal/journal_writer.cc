// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/journal/journal_writer.h"

#include <lib/fit/defer.h>
#include <lib/sync/completion.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include <cstdio>

#include "src/lib/storage/vfs/cpp/journal/entry_view.h"

namespace fs {
namespace {

using storage::OperationType;

}  // namespace

namespace internal {

JournalWriter::JournalWriter(TransactionHandler* transaction_handler,
                             JournalSuperblock journal_superblock, uint64_t journal_start_block,
                             uint64_t entries_length)
    : transaction_handler_(transaction_handler),
      journal_superblock_(std::move(journal_superblock)),
      journal_start_block_(journal_start_block),
      next_sequence_number_(journal_superblock_.sequence_number()),
      next_entry_start_block_(journal_superblock_.start()),
      entries_length_(entries_length) {}

JournalWriter::JournalWriter(TransactionHandler* transaction_handler)
    : transaction_handler_(transaction_handler) {}

fpromise::result<void, zx_status_t> JournalWriter::WriteData(JournalWorkItem work) {
  // If any of the data operations we're about to write overlap with in-flight metadata operations,
  // then we risk those metadata operations "overwriting" our data blocks on replay.
  //
  // Before writing data, identify that those metadata blocks should not be replayed.
  for (const auto& operation : work.operations) {
    range::Range<uint64_t> range(operation.op.dev_offset,
                                 operation.op.dev_offset + operation.op.length);
    // Determine if the given range partially overlaps any live operations.
    if (live_metadata_operations_.find(range) != live_metadata_operations_.end()) {
      // Currently, writing the info block is sufficient to "avoid metadata replay", but this is
      // only the case because the JournalWriter is synchronous, single-threaded, and non-caching.
      // If we enable asynchronous writeback, emitting revocation records may be a more desirable
      // option than "blocking until all prior operations complete, then blocking on writing the
      // info block".
      zx_status_t status = WriteInfoBlock();
      if (status != ZX_OK) {
        FX_LOGST(WARNING, "journal") << "Failed to write data: " << zx_status_get_string(status);
        return fpromise::error(status);
      }
    }
  }

  zx_status_t status = WriteOperations(work.operations);
  if (status != ZX_OK) {
    FX_LOGST(WARNING, "journal") << "Failed to write data: " << zx_status_get_string(status);
    return fpromise::error(status);
  }
  return fpromise::ok();
}

fpromise::result<void, zx_status_t> JournalWriter::WriteMetadata(
    JournalWorkItem work, std::optional<JournalWorkItem> trim_work) {
  const uint64_t block_count = work.reservation.length();
  FX_LOGST(DEBUG, "journal") << "WriteMetadata: Writing " << block_count
                             << " blocks (includes header, commit)";
  // Ensure the info block is caught up, so it doesn't point to the middle of an invalid entry.
  zx_status_t status = WriteInfoBlockIfIntersect(block_count);
  if (status != ZX_OK) {
    FX_LOGST(WARNING, "journal") << "WriteMetadata: Failed to write info block: "
                                 << zx_status_get_string(status);
    return fpromise::error(status);
  }

  // Monitor the in-flight metadata operations.
  for (const auto& operation : work.operations) {
    range::Range<uint64_t> range(operation.op.dev_offset,
                                 operation.op.dev_offset + operation.op.length);
    live_metadata_operations_.insert(std::move(range));
  }

  // Write metadata to the journal itself.
  status = WriteMetadataToJournal(&work);
  if (status != ZX_OK) {
    FX_LOGST(WARNING, "journal") << "WriteMetadata: Failed to write metadata to journal: "
                                 << zx_status_get_string(status);
    return fpromise::error(status);
  }
  // We rely on trim going first because the callbacks need to be after the trim operations have
  // been submitted (e.g. it's not safe to send new data operations until after trim operations have
  // been submitted).
  if (trim_work) {
    pending_work_items_.push_back(*std::move(trim_work));
  }
  pending_work_items_.push_back(std::move(work));
  return fpromise::ok();
}

zx_status_t JournalWriter::WriteOperationToJournal(const storage::BlockBufferView& view) {
  const uint64_t total_block_count = view.length();
  const uint64_t max_reservation_size = EntriesLength();
  uint64_t written_block_count = 0;
  std::vector<storage::BufferedOperation> journal_operations;
  storage::BufferedOperation operation;
  operation.vmoid = view.vmoid();
  operation.op.type = storage::OperationType::kWrite;

  // Both the reservation and the on-disk location may wraparound.
  while (written_block_count != total_block_count) {
    operation.op.vmo_offset = (view.start() + written_block_count) % max_reservation_size;
    operation.op.dev_offset = EntriesStartBlock() + next_entry_start_block_;

    // The maximum number of blocks that can be written to the journal, on-disk, before needing to
    // wrap around.
    const uint64_t journal_block_max = EntriesLength() - next_entry_start_block_;
    // The maximum number of blocks that can be written from the reservation, in-memory, before
    // needing to wrap around.
    const uint64_t reservation_block_max = max_reservation_size - operation.op.vmo_offset;
    operation.op.length = std::min(total_block_count - written_block_count,
                                   std::min(journal_block_max, reservation_block_max));
    journal_operations.push_back(operation);
    written_block_count += operation.op.length;
    next_entry_start_block_ = (next_entry_start_block_ + operation.op.length) % EntriesLength();
  }

  zx_status_t status = WriteOperations(journal_operations);
  if (status != ZX_OK) {
    FX_LOGST(WARNING, "journal") << "JournalWriter::WriteOperationToJournal: Failed to write: "
                                 << zx_status_get_string(status);
    return status;
  }
  return status;
}

fpromise::result<void, zx_status_t> JournalWriter::Sync() {
  if (!IsWritebackEnabled()) {
    return fpromise::error(ZX_ERR_IO_REFUSED);
  }

  if (next_sequence_number_ == journal_superblock_.sequence_number()) {
    FX_LOGST(DEBUG, "journal") << "Sync: Skipping write to info block (no sequence update)";
    return fpromise::ok();
  }

  zx_status_t status = WriteInfoBlock();
  if (status != ZX_OK) {
    return fpromise::error(status);
  }
  return fpromise::ok();
}

zx_status_t JournalWriter::WriteMetadataToJournal(JournalWorkItem* work) {
  FX_LOGST(DEBUG, "journal") << "WriteMetadataToJournal: Writing " << work->reservation.length()
                             << " blocks with sequence_number " << next_sequence_number_;

  // Set the header and commit blocks within the journal.
  JournalEntryView entry(work->reservation.buffer_view(), work->operations,
                         next_sequence_number_++);

  zx_status_t status = WriteOperationToJournal(work->reservation.buffer_view());
  // Although the payload may be encoded while written to the journal, it should be decoded
  // when written to the final on-disk location later.
  entry.DecodePayloadBlocks();
  return status;
}

zx_status_t JournalWriter::WriteInfoBlockIfIntersect(uint64_t block_count) {
  // We need to write the info block now if [journal tail, journal tail + block_count) intersects
  // with [journal head, journal tail).
  //
  // Logically, the journal is a circular buffer:
  //
  //   [ ____, ____, ____, ____, ____, ____ ]
  //
  // Within that buffer, the journal has some entries which will be replayed
  //
  //           Info Block        Next Entry Start Block
  //           |                 |
  //   [ ____, head, data, tail, ____, ____ ]
  //
  // In this diagram, it would be safe to write one, two, or three additional blocks: they would fit
  // within the journal. However, if four blocks are written, the journal would "eat its own head":
  //
  //           Info Block
  //           |
  //   [ blk3, blk4, data, tail, blk1, blk2 ]
  //           |
  //           Collision!
  //
  // If power failure occurred, replay would be unable to parse prior entries, since the start block
  // would point to an invalid entry. However, if we also wrote the info block repeatedly, the
  // journaling code would incur a significant write amplification cost.
  //
  // To compromise, we write the info block before any writes that would trigger this collision.
  const uint64_t head = journal_superblock_.start();
  const uint64_t tail = next_entry_start_block_;
  const uint64_t capacity = EntriesLength();

  // It's a little tricky to distinguish between an "empty" and "full" journal, so we observe
  // that case explicitly first, using the sequence number to make the distinction.
  //
  // We require an info block update if the journal is full, but not if it's empty.
  bool write_info =
      (head == tail) && (next_sequence_number_ != journal_superblock_.sequence_number());

  if (!write_info) {
    const uint64_t journal_used = (head <= tail) ? (tail - head) : ((capacity - head) + tail);
    const uint64_t journal_free = capacity - journal_used;
    if (journal_free < block_count) {
      FX_LOGST(DEBUG, "journal") << "WriteInfoBlockIfIntersect: Writing info block (can't write "
                                 << block_count << " blocks)";
      write_info = true;
    } else {
      FX_LOGST(DEBUG, "journal") << "WriteInfoBlockIfIntersect: Not writing info (have "
                                 << journal_free << ", need " << block_count << " blocks)";
    }
  }

  if (write_info) {
    zx_status_t status = WriteInfoBlock();
    if (status != ZX_OK) {
      FX_LOGST(WARNING, "journal") << "WriteInfoBlockIfIntersect: Failed to write info block";
      return status;
    }
  }
  return ZX_OK;
}

zx_status_t JournalWriter::WriteInfoBlock() {
  // Before writing the info block, we must make sure any previous metadata writes have been flushed
  // because the journal can't replay those writes after writing the info block.
  while (!pending_work_items_.empty() || pending_flush_) {
    if (auto result = Flush(); result.is_error()) {
      return result.take_error();
    }
  }
  ZX_DEBUG_ASSERT(next_sequence_number_ > journal_superblock_.sequence_number());
  FX_LOGST(DEBUG, "journal") << "WriteInfoBlock: Updating sequence_number from "
                             << journal_superblock_.sequence_number() << " to "
                             << next_sequence_number_;

  ZX_DEBUG_ASSERT(next_entry_start_block_ < EntriesLength());
  journal_superblock_.Update(next_entry_start_block_, next_sequence_number_);
  std::vector<storage::BufferedOperation> journal_operations;
  storage::BufferedOperation operation;
  operation.vmoid = journal_superblock_.buffer().vmoid();
  operation.op.type = storage::OperationType::kWrite;
  operation.op.vmo_offset = 0;
  operation.op.dev_offset = InfoStartBlock();
  operation.op.length = InfoLength();
  journal_operations.push_back(operation);
  zx_status_t status = WriteOperations(journal_operations);
  if (status != ZX_OK) {
    return status;
  }
  // Immediately after the info block is updated, no metadata operations should be replayed
  // on reboot.
  live_metadata_operations_.clear();
  return ZX_OK;
}

zx_status_t JournalWriter::WriteOperations(
    const std::vector<storage::BufferedOperation>& operations) {
  if (!IsWritebackEnabled()) {
    FX_LOGST(INFO, "journal")
        << "WriteOperations: Not issuing writeback because writeback is disabled";
    return ZX_ERR_IO_REFUSED;
  }

  zx_status_t status = transaction_handler_->RunRequests(operations);
  if (status != ZX_OK) {
    FX_LOGST(ERROR, "journal") << "WriteOperations: Failed to write requests " << operations << ": "
                               << zx_status_get_string(status) << ". Filesystem now read-only.";
    DisableWriteback();
    return status;
  }
  return ZX_OK;
}

fpromise::result<void, zx_status_t> JournalWriter::Flush() {
  // In case of success or failure, we want to clear pending_work_items_ because there might be
  // cleanup that needs to run buried in the destructors of the callbacks.
  auto clean_up = fit::defer([this] { pending_work_items_.clear(); });

  if (!IsWritebackEnabled()) {
    FX_LOGST(INFO, "journal")
        << "JournalWriter::Flush: Not issuing writeback because writeback is disabled";
    return fpromise::error(ZX_ERR_BAD_STATE);
  }
  if (zx_status_t status = transaction_handler_->Flush(); status != ZX_OK) {
    FX_LOGST(WARNING, "journal") << "JournalWriter::Flush: " << zx_status_get_string(status);
    DisableWriteback();
    return fpromise::error(status);
  }
  pending_flush_ = false;
  for (JournalWorkItem& w : pending_work_items_) {
    // Move, so that we release resources when this goes out of scope.
    JournalWorkItem work = std::move(w);
    if (work.commit_callback) {
      work.commit_callback();
    }
    zx_status_t status = WriteOperations(work.operations);
    if (status != ZX_OK) {
      FX_LOGST(WARNING, "journal")
          << "Flush: Failed to write metadata to final location: " << zx_status_get_string(status);
      // WriteOperations will mark things so that all subsequent writes will fail.
      return fpromise::error(status);
    }
    // Before we can write the info block, we need another flush to ensure the writes to final
    // locations will persist.
    pending_flush_ = true;
    // We've written the metadata so reads will work now and we can call complete_callback.
    if (work.complete_callback) {
      work.complete_callback();
    }
  }
  return fpromise::ok();
}

}  // namespace internal
}  // namespace fs
