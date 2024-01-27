// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_LOG_SOURCE_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_LOG_SOURCE_H_

#include <fuchsia/diagnostics/cpp/fidl.h>
#include <fuchsia/logger/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fpromise/result.h>
#include <lib/sys/cpp/service_directory.h>

#include <memory>
#include <string>

#include "src/lib/backoff/backoff.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace forensics::feedback_data {

// Receives logs emitted by the system.
class LogSink {
 public:
  // A message is a FIDL LogMessage or an error string.
  using MessageOr = ::fpromise::result<fuchsia::logger::LogMessage, std::string>;

  // Adds |message| to the sink.
  //
  // Returns false if the write fails though callers are not expected to take action on failure.
  virtual bool Add(MessageOr message) = 0;

  // Notifies the sink the log source was interrupted and the messages it received in the past may
  // be sent again.
  virtual void NotifyInterruption() = 0;

  // Returns true if the sink is safe to use after an interruption has occurred.
  virtual bool SafeAfterInterruption() const = 0;
};

// Receives log messages from the system's logging service and dispatches them to a sink.
class LogSource {
 public:
  LogSource(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
            LogSink* sink, std::unique_ptr<backoff::Backoff> backoff = nullptr);

  // Starts log collection.
  //
  // Note: a check-fail will occur if |sink_| cannot safely receive messages after being interrupted
  // and the log stream has stopped due to a disconnection or call to Stop.
  void Start();

  // Stops log collection and notfies |sink_| collection was interrupted.
  void Stop();

 private:
  void OnDisconnect();
  void GetNext();

  async_dispatcher_t* dispatcher_;
  std::shared_ptr<sys::ServiceDirectory> services_;

  LogSink* sink_;
  fuchsia::diagnostics::ArchiveAccessorPtr archive_accessor_;
  fuchsia::diagnostics::BatchIteratorPtr batch_iterator_;

  bool is_stopped_{false};
  std::unique_ptr<backoff::Backoff> backoff_;
  fxl::WeakPtrFactory<LogSource> ptr_factory_{this};
};

}  // namespace forensics::feedback_data

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_LOG_SOURCE_H_
