// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/performance/lib/trace_converters/chromium_exporter.h"

#include <inttypes.h>
#include <lib/stdcompat/variant.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace-engine/types.h>

#include <utility>

#include <third_party/modp_b64/modp_b64.h>
#include <trace-reader/reader.h>

#include "rapidjson/writer.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/fxl/strings/utf_codecs.h"
#include "src/performance/lib/perfmon/writer.h"

namespace tracing {
namespace {

constexpr char kProcessArgKey[] = "process";
constexpr zx_koid_t kNoProcess = 0u;
constexpr uint32_t kUnicodeReplacementCharacter = 0xFFFD;

bool IsEventTypeSupported(trace::EventType type) {
  switch (type) {
    case trace::EventType::kInstant:
    case trace::EventType::kCounter:
    case trace::EventType::kDurationBegin:
    case trace::EventType::kDurationEnd:
    case trace::EventType::kDurationComplete:
    case trace::EventType::kAsyncBegin:
    case trace::EventType::kAsyncInstant:
    case trace::EventType::kAsyncEnd:
    case trace::EventType::kFlowBegin:
    case trace::EventType::kFlowStep:
    case trace::EventType::kFlowEnd:
      return true;
    default:
      break;
  }

  return false;
}

const trace::ArgumentValue* GetArgumentValue(const std::vector<trace::Argument>& arguments,
                                             const char* name) {
  for (const auto& arg : arguments) {
    if (arg.name() == name)
      return &arg.value();
  }
  return nullptr;
}

// The JSON specification requires that the JSON is valid unicode. This function
// replaces any invalid unicode sequences with the replacement character, so
// that the output will be valid UTF-8, even if a trace provider gives us
// invalid UTF-8 in a string.
std::string CleanString(fbl::String str) {
  std::string result;
  const char* data = str.data();
  const size_t len = str.length();
  size_t char_index = 0;

  while (char_index < len) {
    uint32_t code_point;
    if (!fxl::ReadUnicodeCharacter(data, len, &char_index, &code_point)) {
      static bool logged_once = false;
      if (!logged_once) {
        FX_LOGS(WARNING) << "Invalid unicode present in trace";
        logged_once = true;
      }
      code_point = kUnicodeReplacementCharacter;
    }
    fxl::WriteUnicodeCharacter(code_point, &result);
    char_index++;
  }

  return result;
}

}  // namespace

ChromiumExporter::ChromiumExporter(std::unique_ptr<std::ostream> stream_out)
    : stream_out_(std::move(stream_out)), wrapper_(*stream_out_), writer_(wrapper_) {
  Start();
}

ChromiumExporter::ChromiumExporter(std::ostream& out) : wrapper_(out), writer_(wrapper_) {
  Start();
}

ChromiumExporter::~ChromiumExporter() { Stop(); }

void ChromiumExporter::Start() {
  writer_.StartObject();
  writer_.Key("displayTimeUnit");
  writer_.String("ns");
  writer_.Key("traceEvents");
  writer_.StartArray();
}

void ChromiumExporter::Stop() {
  writer_.EndArray();
  writer_.Key("systemTraceEvents");
  writer_.StartObject();
  writer_.Key("type");
  writer_.String("fuchsia");
  writer_.Key("events");
  writer_.StartArray();

  for (const auto& pair : processes_) {
    const zx_koid_t process_koid = pair.first;
    const fbl::String& name = pair.second;

    writer_.StartObject();
    writer_.Key("ph");
    writer_.String("p");
    writer_.Key("pid");
    writer_.Uint64(process_koid);
    writer_.Key("name");
    writer_.String(CleanString(name));

    if (process_koid == kNoProcess) {
      writer_.Key("sort_index");
      writer_.Int64(-1);
    }
    writer_.EndObject();
  }

  for (const auto& process_threads : threads_) {
    const zx_koid_t process_koid = process_threads.first;
    for (const auto& thread : process_threads.second) {
      const zx_koid_t thread_koid = thread.first;
      const fbl::String& name = thread.second;
      writer_.StartObject();
      writer_.Key("ph");
      writer_.String("t");
      writer_.Key("pid");
      writer_.Uint64(process_koid);
      writer_.Key("tid");
      writer_.Uint64(thread_koid);
      writer_.Key("name");
      writer_.String(CleanString(name));
      writer_.EndObject();
    }
  }

  for (const auto& record : scheduler_event_records_) {
    ExportSchedulerEvent(record);
  }

  writer_.EndArray();
  writer_.EndObject();  // Finishes systemTraceEvents

  if (last_branch_records_.size() > 0) {
    writer_.Key("lastBranch");
    writer_.StartObject();
    writer_.Key("records");
    writer_.StartArray();
    for (const auto record : last_branch_records_) {
      ExportLastBranchBlob(*record);
    }
    writer_.EndArray();
    writer_.EndObject();
  }

  writer_.EndObject();  // Finishes StartObject() begun in Start()
}

void ChromiumExporter::ExportRecord(const trace::Record& record) {
  switch (record.type()) {
    case trace::RecordType::kMetadata:
      ExportMetadata(record.GetMetadata());
      break;
    case trace::RecordType::kInitialization:
      // Compute scale factor for ticks to microseconds.
      // Microseconds is the unit for the "ts" field.
      tick_scale_ = 1'000'000.0 / record.GetInitialization().ticks_per_second;
      break;
    case trace::RecordType::kEvent:
      ExportEvent(record.GetEvent());
      break;
    case trace::RecordType::kKernelObject:
      ExportKernelObject(record.GetKernelObject());
      break;
    case trace::RecordType::kBlob: {
      const auto& blob = record.GetBlob();
      if (blob.type == TRACE_BLOB_TYPE_LAST_BRANCH) {
        auto lbr = reinterpret_cast<const perfmon::LastBranchRecordBlob*>(blob.blob);
        last_branch_records_.push_back(lbr);
      } else {
        // Drop the record.
        FX_LOGS(INFO) << "Dropping blob record: "
                      << "name " << blob.name.c_str() << " of size " << blob.blob_size;
      }
      break;
    }
    case trace::RecordType::kLog:
      ExportLog(record.GetLog());
      break;
    case trace::RecordType::kScheduler:
      // We can't emit these into the regular stream, save them for later.
      scheduler_event_records_.emplace_back(record.GetSchedulerEvent());
      break;
    case trace::RecordType::kString:
    case trace::RecordType::kThread:
      // We can ignore these, trace::TraceReader consumes them and maintains
      // tables for future lookup.
      break;
    case trace::RecordType::kLargeRecord:
      switch (record.GetLargeRecord().type()) {
        case trace::LargeRecordType::kBlob:
          ExportBlob(record.GetLargeRecord().GetBlob());
          break;
        default:
          break;
      }
      break;
  }
}

void ChromiumExporter::ExportEvent(const trace::Record::Event& event) {
  if (!IsEventTypeSupported(event.type()))
    return;

  writer_.StartObject();

  writer_.Key("cat");
  writer_.String(CleanString(event.category));
  writer_.Key("name");
  writer_.String(CleanString(event.name));
  writer_.Key("ts");
  writer_.Double(event.timestamp * tick_scale_);
  writer_.Key("pid");
  writer_.Uint64(event.process_thread.process_koid());
  writer_.Key("tid");
  writer_.Uint64(event.process_thread.thread_koid());

  switch (event.type()) {
    case trace::EventType::kInstant:
      writer_.Key("ph");
      writer_.String("i");
      writer_.Key("s");
      switch (event.data.GetInstant().scope) {
        case trace::EventScope::kGlobal:
          writer_.String("g");
          break;
        case trace::EventScope::kProcess:
          writer_.String("p");
          break;
        case trace::EventScope::kThread:
        default:
          writer_.String("t");
          break;
      }
      break;
    case trace::EventType::kCounter:
      writer_.Key("ph");
      writer_.String("C");
      if (event.data.GetCounter().id) {
        writer_.Key("id");
        writer_.String(fxl::StringPrintf("0x%" PRIx64, event.data.GetCounter().id).c_str());
      }
      break;
    case trace::EventType::kDurationBegin:
      writer_.Key("ph");
      writer_.String("B");
      break;
    case trace::EventType::kDurationEnd:
      writer_.Key("ph");
      writer_.String("E");
      break;
    case trace::EventType::kDurationComplete:
      writer_.Key("ph");
      writer_.String("X");
      writer_.Key("dur");
      writer_.Double((event.data.GetDurationComplete().end_time - event.timestamp) * tick_scale_);
      break;
    case trace::EventType::kAsyncBegin:
      writer_.Key("ph");
      writer_.String("b");
      writer_.Key("id");
      writer_.Uint64(event.data.GetAsyncBegin().id);
      break;
    case trace::EventType::kAsyncInstant:
      writer_.Key("ph");
      writer_.String("n");
      writer_.Key("id");
      writer_.Uint64(event.data.GetAsyncInstant().id);
      break;
    case trace::EventType::kAsyncEnd:
      writer_.Key("ph");
      writer_.String("e");
      writer_.Key("id");
      writer_.Uint64(event.data.GetAsyncEnd().id);
      break;
    case trace::EventType::kFlowBegin:
      writer_.Key("ph");
      writer_.String("s");
      writer_.Key("id");
      writer_.String(std::to_string(event.data.GetFlowBegin().id));
      break;
    case trace::EventType::kFlowStep:
      writer_.Key("ph");
      writer_.String("t");
      writer_.Key("id");
      writer_.String(std::to_string(event.data.GetFlowStep().id));
      break;
    case trace::EventType::kFlowEnd:
      writer_.Key("ph");
      writer_.String("f");
      writer_.Key("bp");
      writer_.String("e");
      writer_.Key("id");
      writer_.String(std::to_string(event.data.GetFlowEnd().id));
      break;
    default:
      break;
  }

  if (event.arguments.size() > 0) {
    writer_.Key("args");
    writer_.StartObject();
    WriteArgs(event.arguments);
    writer_.EndObject();
  }

  writer_.EndObject();
}

void ChromiumExporter::ExportKernelObject(const trace::Record::KernelObject& kernel_object) {
  // The same kernel objects may appear repeatedly within the trace as
  // they are logged by multiple trace providers.  Stash the best quality
  // information to be output at the end of the trace.  In particular, note
  // that the ktrace provider may truncate names, so we try to pick the
  // longest one to preserve.
  switch (kernel_object.object_type) {
    case ZX_OBJ_TYPE_PROCESS: {
      auto it = processes_.find(kernel_object.koid);
      if (it == processes_.end()) {
        processes_.emplace(kernel_object.koid, kernel_object.name);
      } else if (kernel_object.name.size() > it->second.size()) {
        it->second = kernel_object.name;
      }
      break;
    }
    case ZX_OBJ_TYPE_THREAD: {
      const trace::ArgumentValue* process_arg =
          GetArgumentValue(kernel_object.arguments, kProcessArgKey);
      if (!process_arg || process_arg->type() != trace::ArgumentType::kKoid)
        break;
      const zx_koid_t process_koid = process_arg->GetKoid();
      auto process_it = threads_.find(process_koid);
      if (process_it == threads_.end()) {
        process_it = threads_
                         .emplace(std::piecewise_construct, std::forward_as_tuple(process_koid),
                                  std::forward_as_tuple())
                         .first;
      }
      auto& threads = process_it->second;
      auto thread_it = threads.find(kernel_object.koid);
      if (thread_it == threads.end()) {
        threads.emplace(kernel_object.koid, kernel_object.name);
      } else if (kernel_object.name.size() > thread_it->second.size()) {
        thread_it->second = kernel_object.name;
      }
      break;
    }
  }
}

void ChromiumExporter::ExportLastBranchBlob(const perfmon::LastBranchRecordBlob& lbr) {
  writer_.StartObject();
  writer_.Key("cpu");
  writer_.Uint(lbr.cpu);
  writer_.Key("aspace");
  writer_.Uint(lbr.aspace);
  writer_.Key("event_time");
  writer_.Uint(lbr.event_time);
  writer_.Key("branches");
  writer_.StartArray();
  for (unsigned i = 0; i < lbr.num_branches; ++i) {
    writer_.StartObject();
    writer_.Key("from");
    writer_.Uint64(lbr.branches[i].from);
    writer_.Key("to");
    writer_.Uint64(lbr.branches[i].to);
    writer_.Key("info");
    writer_.Uint64(lbr.branches[i].info);
    writer_.EndObject();
  }
  writer_.EndArray();
  writer_.EndObject();
}

void ChromiumExporter::ExportLog(const trace::Record::Log& log) {
  writer_.StartObject();
  writer_.Key("name");
  writer_.String("log");
  writer_.Key("ph");
  writer_.String("i");
  writer_.Key("ts");
  writer_.Double(log.timestamp * tick_scale_);
  writer_.Key("pid");
  writer_.Uint64(log.process_thread.process_koid());
  writer_.Key("tid");
  writer_.Uint64(log.process_thread.thread_koid());
  writer_.Key("s");
  writer_.String("g");
  writer_.Key("args");
  writer_.StartObject();
  writer_.Key("message");
  writer_.String(CleanString(log.message));
  writer_.EndObject();
  writer_.EndObject();
}

void ChromiumExporter::ExportMetadata(const trace::Record::Metadata& metadata) {
  switch (metadata.type()) {
    case trace::MetadataType::kProviderInfo:
    case trace::MetadataType::kProviderSection:
    case trace::MetadataType::kTraceInfo:
      // These are handled elsewhere.
      break;
    case trace::MetadataType::kProviderEvent: {
      const auto& event = metadata.content.GetProviderEvent();
      const auto& id = event.id;
      if (event.event == trace::ProviderEventType::kBufferOverflow) {
        // TODO(dje): Need to get provider name.
        FX_LOGS(WARNING) << "#" << id << " buffer overflowed,"
                         << " records were likely dropped";
      }
      break;
    }
  }
}

void ChromiumExporter::ExportSchedulerEvent(const trace::Record::SchedulerEvent& scheduler_event) {
  switch (scheduler_event.type()) {
    case trace::SchedulerEventType::kLegacyContextSwitch: {
      auto& context_switch = scheduler_event.legacy_context_switch();
      writer_.StartObject();
      writer_.Key("ph");
      writer_.String("k");
      writer_.Key("ts");
      writer_.Double(static_cast<double>(context_switch.timestamp) * tick_scale_);
      writer_.Key("cpu");
      writer_.Uint(context_switch.cpu_number);
      writer_.Key("out");
      writer_.StartObject();
      writer_.Key("pid");
      writer_.Uint64(context_switch.outgoing_thread.process_koid());
      writer_.Key("tid");
      writer_.Uint64(context_switch.outgoing_thread.thread_koid());
      writer_.Key("state");
      writer_.Uint(static_cast<uint32_t>(context_switch.outgoing_thread_state));
      writer_.Key("prio");
      writer_.Uint(static_cast<uint32_t>(context_switch.outgoing_thread_priority));
      writer_.EndObject();
      writer_.Key("in");
      writer_.StartObject();
      writer_.Key("pid");
      writer_.Uint64(context_switch.incoming_thread.process_koid());
      writer_.Key("tid");
      writer_.Uint64(context_switch.incoming_thread.thread_koid());
      writer_.Key("prio");
      writer_.Uint(static_cast<uint32_t>(context_switch.incoming_thread_priority));
      writer_.EndObject();
      writer_.EndObject();
      break;
    }
    case trace::SchedulerEventType::kContextSwitch: {
      auto& context_switch = scheduler_event.context_switch();
      writer_.StartObject();
      writer_.Key("ph");
      writer_.String("k");
      writer_.Key("ts");
      writer_.Double(static_cast<double>(context_switch.timestamp) * tick_scale_);
      writer_.Key("cpu");
      writer_.Uint(context_switch.cpu_number);
      writer_.Key("out");
      writer_.StartObject();
      writer_.Key("tid");
      writer_.Uint64(context_switch.outgoing_tid);
      writer_.Key("state");
      writer_.Uint(static_cast<uint32_t>(context_switch.outgoing_thread_state));

      if (const trace::Argument* outgoing_weight =
              trace::Argument::Find("outgoing_weight", context_switch.arguments);
          outgoing_weight != nullptr) {
        const trace::ArgumentValue& value = outgoing_weight->value();
        switch (value.type()) {
          case trace::ArgumentType::kInt32:
            writer_.Key("prio");
            writer_.Int(value.GetInt32());
            break;
          case trace::ArgumentType::kUint32:
            writer_.Key("prio");
            writer_.Uint(value.GetUint32());
            break;
          case trace::ArgumentType::kInt64:
            writer_.Key("prio");
            writer_.Int64(value.GetInt64());
            break;
          case trace::ArgumentType::kUint64:
            writer_.Key("prio");
            writer_.Uint64(value.GetUint64());
            break;
          default:
            break;
        }
      }

      writer_.EndObject();
      writer_.Key("in");
      writer_.StartObject();
      writer_.Key("tid");
      writer_.Uint64(context_switch.incoming_tid);

      if (const trace::Argument* incoming_weight =
              trace::Argument::Find("incoming_weight", context_switch.arguments);
          incoming_weight != nullptr) {
        const trace::ArgumentValue& value = incoming_weight->value();
        switch (value.type()) {
          case trace::ArgumentType::kInt32:
            writer_.Key("prio");
            writer_.Int(value.GetInt32());
            break;
          case trace::ArgumentType::kUint32:
            writer_.Key("prio");
            writer_.Uint(value.GetUint32());
            break;
          case trace::ArgumentType::kInt64:
            writer_.Key("prio");
            writer_.Int64(value.GetInt64());
            break;
          case trace::ArgumentType::kUint64:
            writer_.Key("prio");
            writer_.Uint64(value.GetUint64());
            break;
          default:
            break;
        }
      }

      writer_.EndObject();
      writer_.EndObject();
      break;
    }
    case trace::SchedulerEventType::kThreadWakeup: {
      auto& thread_wakeup = scheduler_event.thread_wakeup();
      writer_.StartObject();
      writer_.Key("ph");
      writer_.String("w");
      writer_.Key("ts");
      writer_.Double(static_cast<double>(thread_wakeup.timestamp) * tick_scale_);
      writer_.Key("cpu");
      writer_.Uint(thread_wakeup.cpu_number);
      writer_.Key("tid");
      writer_.Uint64(thread_wakeup.incoming_tid);

      if (const trace::Argument* weight = trace::Argument::Find("weight", thread_wakeup.arguments);
          weight != nullptr) {
        const trace::ArgumentValue& value = weight->value();
        switch (value.type()) {
          case trace::ArgumentType::kInt32:
            writer_.Key("prio");
            writer_.Int(value.GetInt32());
            break;
          case trace::ArgumentType::kUint32:
            writer_.Key("prio");
            writer_.Uint(value.GetUint32());
            break;
          case trace::ArgumentType::kInt64:
            writer_.Key("prio");
            writer_.Int64(value.GetInt64());
            break;
          case trace::ArgumentType::kUint64:
            writer_.Key("prio");
            writer_.Uint64(value.GetUint64());
            break;
          default:
            break;
        }
      }

      writer_.EndObject();
      break;
    }
  }
}

void ChromiumExporter::ExportBlob(const trace::LargeRecordData::Blob& data) {
  if (cpp17::holds_alternative<trace::LargeRecordData::BlobEvent>(data)) {
    const auto& blob = cpp17::get<trace::LargeRecordData::BlobEvent>(data);

    if (blob.category == "fidl:blob") {
      ExportFidlBlob(blob);
      return;
    }

    // Drop blob event record.
    FX_LOGS(INFO) << "Dropping large blob event record: "
                  << "name " << blob.name.c_str() << " of size " << blob.blob_size;
  } else if (cpp17::holds_alternative<trace::LargeRecordData::BlobAttachment>(data)) {
    const auto& blob = cpp17::get<trace::LargeRecordData::BlobAttachment>(data);

    // Drop blob attachment record.
    FX_LOGS(INFO) << "Dropping large blob attachment record: "
                  << "name " << blob.name.c_str() << " of size " << blob.blob_size;
  }
}

void ChromiumExporter::ExportFidlBlob(const trace::LargeRecordData::BlobEvent& blob) {
  writer_.StartObject();
  writer_.Key("ph");
  writer_.String("O");
  writer_.Key("id");
  writer_.String("");
  writer_.Key("cat");
  writer_.String(CleanString(blob.category));
  writer_.Key("name");
  writer_.String(CleanString(blob.name));
  writer_.Key("ts");
  writer_.Double(blob.timestamp * tick_scale_);
  writer_.Key("pid");
  writer_.Uint64(blob.process_thread.process_koid());
  writer_.Key("tid");
  writer_.Uint64(blob.process_thread.thread_koid());
  writer_.Key("blob");
  auto blob_str = std::string(static_cast<const char*>(blob.blob), blob.blob_size);
  auto blob_str_base64 = modp_b64_encode(blob_str);
  writer_.String(blob_str_base64);
  writer_.EndObject();
}

void ChromiumExporter::WriteArgs(const std::vector<trace::Argument>& arguments) {
  for (const auto& arg : arguments) {
    switch (arg.value().type()) {
      case trace::ArgumentType::kBool:
        writer_.Key(CleanString(arg.name()));
        writer_.Bool(arg.value().GetBool());
        break;
      case trace::ArgumentType::kInt32:
        writer_.Key(CleanString(arg.name()));
        writer_.Int(arg.value().GetInt32());
        break;
      case trace::ArgumentType::kUint32:
        writer_.Key(CleanString(arg.name()));
        writer_.Uint(arg.value().GetUint32());
        break;
      case trace::ArgumentType::kInt64:
        writer_.Key(CleanString(arg.name()));
        writer_.Int64(arg.value().GetInt64());
        break;
      case trace::ArgumentType::kUint64:
        writer_.Key(CleanString(arg.name()));
        writer_.Uint64(arg.value().GetUint64());
        break;
      case trace::ArgumentType::kDouble:
        writer_.Key(CleanString(arg.name()));
        writer_.Double(arg.value().GetDouble());
        break;
      case trace::ArgumentType::kString:
        writer_.Key(CleanString(arg.name()));
        writer_.String(CleanString(arg.value().GetString()));
        break;
      case trace::ArgumentType::kPointer:
        writer_.Key(CleanString(arg.name()));
        writer_.String(fxl::StringPrintf("0x%" PRIx64, arg.value().GetPointer()).c_str());
        break;
      case trace::ArgumentType::kKoid:
        writer_.Key(CleanString(arg.name()));
        writer_.String(fxl::StringPrintf("#%" PRIu64, arg.value().GetKoid()).c_str());
        break;
      default:
        break;
    }
  }
}

}  // namespace tracing
