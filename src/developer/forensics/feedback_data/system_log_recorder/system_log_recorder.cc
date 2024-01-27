// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/system_log_recorder/system_log_recorder.h"

#include <lib/syslog/cpp/macros.h>

#include "src/lib/files/path.h"

namespace forensics {
namespace feedback_data {
namespace system_log_recorder {

// No rate limiting in the first minute of recording to allow us to catch up on all the log
// messages prior to listening.
constexpr zx::duration kNoRateLimitDuration = zx::sec(60);

SystemLogRecorder::SystemLogRecorder(async_dispatcher_t* archive_dispatcher,
                                     async_dispatcher_t* write_dispatcher,
                                     std::shared_ptr<sys::ServiceDirectory> services,
                                     WriteParameters write_parameters,
                                     std::unique_ptr<RedactorBase> redactor,
                                     std::unique_ptr<Encoder> encoder)
    : archive_dispatcher_(archive_dispatcher),
      write_dispatcher_(write_dispatcher),
      write_period_(write_parameters.period),
      logs_dir_(write_parameters.logs_dir),
      store_(write_parameters.total_log_size / write_parameters.max_num_files,
             write_parameters.max_write_size, std::move(redactor), std::move(encoder)),
      log_source_(archive_dispatcher, services, &store_),
      writer_(logs_dir_, write_parameters.max_num_files, &store_) {}

void SystemLogRecorder::Start() {
  log_source_.Start();
  periodic_write_task_.Post(write_dispatcher_);

  async::PostDelayedTask(
      archive_dispatcher_, [this] { store_.TurnOnRateLimiting(); }, kNoRateLimitDuration);
}

void SystemLogRecorder::Flush(const std::optional<std::string> message) {
  FX_LOGS(INFO) << "Received signal to flush cached logs to disk";
  if (message.has_value()) {
    store_.AppendToEnd(message.value());
  }

  // Ensure data is written to disk ASAP when the system log recorder (and maybe the system) is
  // expected to stop soon.
  writer_.Write();
  writer_.Fsync();
}

void SystemLogRecorder::StopAndDeleteLogs() {
  // Stop collecting logs.
  log_source_.Stop();
  periodic_write_task_.Cancel();

  // Consume the data currently in the store to flush it.
  bool end_of_block;
  store_.Consume(&end_of_block);

  // Delete the persisted logs.
  files::DeletePath(logs_dir_, /*recursive=*/true);

  FX_LOGS(INFO) << "Stopped log recording and flushed persisted logs";
}

void SystemLogRecorder::PeriodicWriteTask() {
  writer_.Write();
  periodic_write_task_.PostDelayed(write_dispatcher_, write_period_);
}

}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics
