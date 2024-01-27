// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_F2FS_WRITEBACK_H_
#define SRC_STORAGE_F2FS_WRITEBACK_H_

#ifdef __Fuchsia__
#include "src/lib/storage/vfs/cpp/journal/background_executor.h"
#else  // __Fuchsia__
#include <lib/fpromise/promise.h>
#include <lib/fpromise/single_threaded_executor.h>

#include <storage/buffer/array_buffer.h>
#endif  // __Fuchsia__

namespace f2fs {

// F2fs flushes dirty pages when the number of dirty data pages exceeds a half of
// |kMaxDirtyDataPages|.
constexpr int kMaxDirtyDataPages = 51200;

// This class is final because there might be background threads running when its destructor runs
// and that would be unsafe if this class had overridden virtual methods that might get called from
// those background threads.
class Writer final {
 public:
  Writer(Bcache *bc, size_t capacity);
  Writer() = delete;
  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;
  Writer(const Writer &&) = delete;
  Writer &operator=(const Writer &&) = delete;
  ~Writer();

  void ScheduleTask(fpromise::promise<> task);
  void ScheduleWriteback(fpromise::promise<> task);
  // It schedules SubmitPages().
  // If |completion| is set, it notifies the caller of the |pages| write completion.
  void ScheduleSubmitPages(sync_completion_t *completion = nullptr, PageList pages = {});

  // It merges Pages to be written.
  zx::result<> EnqueuePages() __TA_EXCLUDES(mutex_);

 private:
  // It takes write operations from |writer_buffer_| and passes them to RunReqeusts()
  // asynchronously. When the operations are complete, it wakes waiters on the completion of
  // the Page writes.
  fpromise::promise<> SubmitPages(sync_completion_t *completion);

  std::mutex mutex_;
  PageList pages_ __TA_GUARDED(mutex_);
  std::unique_ptr<StorageBuffer> write_buffer_;
  fs::TransactionHandler *transaction_handler_ = nullptr;
#ifdef __Fuchsia__
  fpromise::sequencer sequencer_;
  fs::BackgroundExecutor executor_;
  fs::BackgroundExecutor writeback_executor_;
#endif  // __Fuchsia__
};

class Reader {
 public:
  Reader(Bcache *bc, size_t capacity);
  Reader() = delete;
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  Reader(const Reader &&) = delete;
  Reader &operator=(const Reader &&) = delete;

  // It makes read operations from |writer_buffer_| and passes them to RunReqeusts()
  // synchronously.
  zx::result<std::vector<LockedPage>> SubmitPages(std::vector<LockedPage> pages,
                                                  std::vector<block_t> addrs);

 private:
  fs::TransactionHandler *transaction_handler_ = nullptr;
  std::unique_ptr<StorageBuffer> buffer_;
};

}  // namespace f2fs

#endif  // SRC_STORAGE_F2FS_WRITEBACK_H_
