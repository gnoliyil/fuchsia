// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_PERFORMANCE_LIB_TRACE_CONVERTERS_CHROMIUM_EXPORTER_H_
#define SRC_PERFORMANCE_LIB_TRACE_CONVERTERS_CHROMIUM_EXPORTER_H_

#include <memory>
#include <ostream>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <trace-reader/reader.h>

#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/writer.h"
#include "src/performance/lib/perfmon/writer.h"

namespace tracing {

class ChromiumExporter {
 public:
  explicit ChromiumExporter(std::unique_ptr<std::ostream> stream_out);
  explicit ChromiumExporter(std::ostream& out);
  ~ChromiumExporter();

  void ExportRecord(const trace::Record& record);

 private:
  void Start();
  void Stop();
  void ExportEvent(const trace::Record::Event& event);
  void ExportLastBranchBlob(const perfmon::LastBranchRecordBlob& lbr);
  void ExportKernelObject(const trace::Record::KernelObject& kernel_object);
  void ExportLog(const trace::Record::Log& log);
  void ExportMetadata(const trace::Record::Metadata& metadata);
  void ExportSchedulerEvent(const trace::Record::SchedulerEvent& scheduler_event);
  void ExportBlob(const trace::LargeRecordData::Blob& blob);
  void ExportFidlBlob(const trace::LargeRecordData::BlobEvent& blob);

  // Writes argument data. Assumes it is already within an
  // "args" key object.
  void WriteArgs(const std::vector<trace::Argument>& arguments);

  std::unique_ptr<std::ostream> stream_out_;
  rapidjson::OStreamWrapper wrapper_;
  rapidjson::Writer<rapidjson::OStreamWrapper> writer_;

  // Scale factor to get to microseconds.
  // By default ticks are in nanoseconds.
  double tick_scale_ = 0.001;

  std::unordered_map<zx_koid_t, fbl::String> processes_;
  // Virtual threads mean the same thread id can appear in different processes.
  // Organize threads by process to cope with this.
  std::unordered_map<zx_koid_t /* process id */,
                     std::unordered_map<zx_koid_t /* thread id */, fbl::String /* thread name */>>
      threads_;

  // The chromium/catapult trace file format doesn't support scheduler event
  // records, so we can't emit them inline. Save them for later emission to
  // the systemTraceEvents section.
  std::vector<trace::Record::SchedulerEvent> scheduler_event_records_;

  // The chromium/catapult trace file format doesn't support random blobs,
  // so we can't emit them inline. Save them for later emission.
  // LastBranch records will go to the lastBranch section.
  std::vector<const perfmon::LastBranchRecordBlob*> last_branch_records_;
};

}  // namespace tracing

#endif  // SRC_PERFORMANCE_LIB_TRACE_CONVERTERS_CHROMIUM_EXPORTER_H_
