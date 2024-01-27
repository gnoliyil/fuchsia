// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/system_log_recorder/writer.h"

#include <fcntl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <string>

#include "src/lib/files/directory.h"
#include "src/lib/files/path.h"

namespace forensics {
namespace feedback_data {
namespace system_log_recorder {

SystemLogWriter::SystemLogWriter(const std::string& logs_dir, size_t max_num_files,
                                 LogMessageStore* store)
    : logs_dir_(logs_dir), max_num_files_(max_num_files), file_queue_(), store_(store) {
  FX_CHECK(max_num_files_ > 0);
  if (!files::CreateDirectory(logs_dir)) {
    FX_LOGS(WARNING) << "Failed to create logs directory, will re-try on the next block, no logs "
                        "persisted until then";
    return;
  }

  std::vector<std::string> current_log_files;
  files::ReadDirContents(logs_dir_, &current_log_files);

  // Remove the current directory from the files.
  current_log_files.erase(std::remove(current_log_files.begin(), current_log_files.end(), "."),
                          current_log_files.end());

  // Get the numbers the previous writer assigned to the files – there should only be previous
  // files in case of a component restart.
  for (const auto& fname : current_log_files) {
    file_queue_.push_back(std::strtoull(fname.c_str(), nullptr, /*base=*/10));
  }

  // Sort the files such that the oldest files will be deleted first.
  std::sort(file_queue_.begin(), file_queue_.end());

  StartNewFile();
}

void SystemLogWriter::StartNewFile() {
  if (!files::IsDirectory(logs_dir_)) {
    file_queue_.clear();

    if (files::CreateDirectory(logs_dir_)) {
      FX_LOGS(INFO)
          << "Re-created logs directory. Disk was most likely full at some earlier point in time";
    } else {
      FX_LOGS_FIRST_N(WARNING, 10)
          << "Still cannot re-create logs directory. Disk still most likely full";
    }
  }

  const size_t next_file_num = (file_queue_.empty()) ? 0u : file_queue_.back() + 1;
  if (file_queue_.size() >= max_num_files_) {
    TRACE_DURATION("feedback:io", "SystemLogWriter::RemoveFile");
    remove(Path(file_queue_.front()).c_str());
    file_queue_.pop_front();
  }

  file_queue_.push_back(next_file_num);

  TRACE_DURATION("feedback:io", "SystemLogWriter::OpenFile");
  current_file_descriptor_.reset(
      open(Path(next_file_num).c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR));
}

void SystemLogWriter::Write() {
  TRACE_DURATION("feedback:io", "SystemLogWriter::Write");
  bool end_of_block;
  const std::string str = store_->Consume(&end_of_block);

  // The file descriptor could be negative if the file failed to open.
  if (current_file_descriptor_.is_valid()) {
    // Overcommit, i.e. write everything we consumed before starting a new file for the next
    // block as we cannot have a block spanning multiple files.
    write(current_file_descriptor_.get(), str.c_str(), str.size());
  }

  if (end_of_block) {
    StartNewFile();
  }
}

void SystemLogWriter::Fsync() { fsync(current_file_descriptor_.get()); }

std::string SystemLogWriter::Path(const size_t file_num) const {
  return files::JoinPath(logs_dir_, std::to_string(file_num));
}

}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics
