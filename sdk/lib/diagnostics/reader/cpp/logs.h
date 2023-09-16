// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_DIAGNOSTICS_READER_CPP_LOGS_H_
#define LIB_DIAGNOSTICS_READER_CPP_LOGS_H_

#include <fuchsia/diagnostics/cpp/fidl.h>
#include <lib/fpromise/bridge.h>
#include <lib/fpromise/promise.h>
#include <lib/fpromise/scope.h>
#include <lib/stdcompat/optional.h>

#include <cstdint>
#include <optional>
#include <queue>

#include <rapidjson/document.h>

namespace diagnostics::reader {

// Container for inspect data returned by a component.
//
// This class provides methods for parsing common fields from diagnostics output.
class LogsData {
 public:
  struct LogsMetadata final {
    std::string component_url;
    uint64_t timestamp;
    fuchsia::diagnostics::Severity severity;
    std::vector<std::string> tags;
    std::optional<uint64_t> pid;
    std::optional<uint64_t> tid;
    std::optional<std::string> file;
    std::optional<uint64_t> line;
    // TODO(b/300181458): process errors.
  };

  // Create a new LogsData wrapper from a JSON document.
  explicit LogsData(rapidjson::Document document);

  // Movable but not copyable.
  LogsData(const LogsData&) = delete;
  LogsData(LogsData&&) = default;
  LogsData& operator=(const LogsData&) = delete;
  LogsData& operator=(LogsData&&) = default;

  // Return the moniker of the component that created this data.
  const std::string& moniker() const { return moniker_; }

  // Return the version of the component that created this data.
  uint64_t version() const { return version_; }

  // Return the metadata of the component that created this data.
  const LogsData::LogsMetadata& metadata() const { return metadata_; }

  // Return the message of the log.
  const std::string& message() const { return message_; }

 private:
  // Moniker of the component that generated the payload.
  std::string moniker_;

  // The metadata for the diagnostics payload.
  LogsMetadata metadata_;

  // The message of this log.
  std::string message_;

  // Schema version.
  uint64_t version_;
};

class LogsSubscription {
 public:
  using Promise = fpromise::promise<std::optional<LogsData>, std::string>;

  explicit LogsSubscription(fuchsia::diagnostics::BatchIteratorPtr iterator);

  // Not movable nor copyable.
  LogsSubscription(const LogsSubscription&) = delete;
  LogsSubscription(LogsSubscription&&) = delete;
  LogsSubscription& operator=(const LogsSubscription&) = delete;
  LogsSubscription& operator=(LogsSubscription&&) = delete;

  /// Returns a promise that will resolve when the iterator receives the next
  /// log. When the stream has completed and no more data will come, returns `std::nullopt`.
  LogsSubscription::Promise Next();

  /// Whether or not the subscription is Done. When this returns `true`,  `Next` is guaranteed to
  /// return `std::nullopt`.
  bool Done();

 private:
  LogsSubscription::Promise ReadBatch();
  std::optional<LogsData> LoadJson(rapidjson::Document document);

  // Iterator connection.
  fuchsia::diagnostics::BatchIteratorPtr iterator_;
  // Pending data to return before calling BatchIterator/GetNext again.
  std::queue<LogsData> pending_;
  // The scope to tie async task lifetimes to this object.
  fpromise::scope scope_;
  // Whether or not this subscription has completed and will return more data.
  bool done_;
};

}  // namespace diagnostics::reader

#endif  // LIB_DIAGNOSTICS_READER_CPP_LOGS_H_
