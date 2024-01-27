// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <string.h>

#include <utility>

#include <fbl/string_buffer.h>
#include <fbl/string_printf.h>
#include <trace-reader/records.h>

namespace trace {
namespace {
const char* EventScopeToString(EventScope scope) {
  switch (scope) {
    case EventScope::kGlobal:
      return "global";
    case EventScope::kProcess:
      return "process";
    case EventScope::kThread:
      return "thread";
  }
  return "???";
}

const char* ThreadStateToString(ThreadState state) {
  switch (state) {
    case ThreadState::kNew:
      return "new";
    case ThreadState::kRunning:
      return "running";
    case ThreadState::kSuspended:
      return "suspended";
    case ThreadState::kBlocked:
      return "blocked";
    case ThreadState::kDying:
      return "dying";
    case ThreadState::kDead:
      return "dead";
  }
  return "???";
}

const char* ObjectTypeToString(zx_obj_type_t type) {
  switch (type) {
    case ZX_OBJ_TYPE_PROCESS:
      return "process";
    case ZX_OBJ_TYPE_THREAD:
      return "thread";
    case ZX_OBJ_TYPE_VMO:
      return "vmo";
    case ZX_OBJ_TYPE_CHANNEL:
      return "channel";
    case ZX_OBJ_TYPE_EVENT:
      return "event";
    case ZX_OBJ_TYPE_PORT:
      return "port";
    case ZX_OBJ_TYPE_INTERRUPT:
      return "interrupt";
    case ZX_OBJ_TYPE_PCI_DEVICE:
      return "pci-device";
    case ZX_OBJ_TYPE_LOG:
      return "log";
    case ZX_OBJ_TYPE_SOCKET:
      return "socket";
    case ZX_OBJ_TYPE_RESOURCE:
      return "resource";
    case ZX_OBJ_TYPE_EVENTPAIR:
      return "event-pair";
    case ZX_OBJ_TYPE_JOB:
      return "job";
    case ZX_OBJ_TYPE_VMAR:
      return "vmar";
    case ZX_OBJ_TYPE_FIFO:
      return "fifo";
    case ZX_OBJ_TYPE_GUEST:
      return "guest";
    case ZX_OBJ_TYPE_VCPU:
      return "vcpu";
    case ZX_OBJ_TYPE_TIMER:
      return "timer";
    case ZX_OBJ_TYPE_IOMMU:
      return "iommu";
    case ZX_OBJ_TYPE_BTI:
      return "bti";
    case ZX_OBJ_TYPE_PROFILE:
      return "profile";
    case ZX_OBJ_TYPE_PMT:
      return "pmt";
    case ZX_OBJ_TYPE_SUSPEND_TOKEN:
      return "suspend-token";
    case ZX_OBJ_TYPE_PAGER:
      return "pager";
    case ZX_OBJ_TYPE_EXCEPTION:
      return "exception";
    default:
      return "???";
  }
}

template <size_t max_preview_start, size_t max_preview_end>
fbl::String PreviewBlobData(const void* blob, size_t blob_size) {
  static_assert((max_preview_start + max_preview_end) > 0);

  auto blob_data = (const unsigned char*)blob;
  fbl::StringBuffer<3 * (max_preview_start + max_preview_end) + 128> result;

  size_t num_leading_bytes;
  size_t num_trailing_bytes;
  if (blob_size <= (max_preview_start + max_preview_end)) {
    num_leading_bytes = blob_size;
    num_trailing_bytes = 0;
  } else {
    num_leading_bytes = max_preview_start;
    num_trailing_bytes = max_preview_end;
  }

  result.Append('<');
  for (size_t i = 0; i < num_leading_bytes; i++) {
    if (i > 0)
      result.Append(' ');
    result.AppendPrintf("%02x", blob_data[i]);
  }
  if (num_trailing_bytes)
    result.Append(" ...");
  for (size_t i = blob_size - num_trailing_bytes; i < blob_size; i++) {
    result.Append(' ');
    result.AppendPrintf("%02x", blob_data[i]);
  }
  result.Append('>');

  return result.ToString();
}

fbl::String FormatArgumentList(const std::vector<trace::Argument>& args) {
  fbl::StringBuffer<1024> result;

  result.Append('{');
  for (size_t i = 0; i < args.size(); i++) {
    if (i != 0)
      result.Append(", ");
    result.Append(args[i].ToString());
  }
  result.Append('}');

  return result.ToString();
}
}  // namespace

fbl::String ProcessThread::ToString() const {
  return fbl::StringPrintf("%" PRIu64 "/%" PRIu64, process_koid_, thread_koid_);
}

fbl::String ArgumentValue::ToString() const {
  switch (type()) {
    case ArgumentType::kNull:
      return "null";
    case ArgumentType::kBool:
      return fbl::StringPrintf("bool(%s)", cpp17::get<Bool>(value_).value ? "true" : "false");
    case ArgumentType::kInt32:
      return fbl::StringPrintf("int32(%" PRId32 ")", cpp17::get<int32_t>(value_));
    case ArgumentType::kUint32:
      return fbl::StringPrintf("uint32(%" PRIu32 ")", cpp17::get<uint32_t>(value_));
    case ArgumentType::kInt64:
      return fbl::StringPrintf("int64(%" PRId64 ")", cpp17::get<int64_t>(value_));
    case ArgumentType::kUint64:
      return fbl::StringPrintf("uint64(%" PRIu64 ")", cpp17::get<uint64_t>(value_));
    case ArgumentType::kDouble:
      return fbl::StringPrintf("double(%f)", cpp17::get<double>(value_));
    case ArgumentType::kString:
      return fbl::StringPrintf("string(\"%s\")", cpp17::get<fbl::String>(value_).c_str());
    case ArgumentType::kPointer:
      return fbl::StringPrintf("pointer(%p)",
                               reinterpret_cast<void*>(cpp17::get<Pointer>(value_).value));
    case ArgumentType::kKoid:
      return fbl::StringPrintf("koid(%" PRIu64 ")", cpp17::get<Koid>(value_).value);
  }
  ZX_ASSERT(false);
}

fbl::String Argument::ToString() const {
  return fbl::StringPrintf("%s: %s", name_.c_str(), value_.ToString().c_str());
}

void TraceInfoContent::Destroy() {
  switch (type_) {
    case TraceInfoType::kMagicNumber:
      magic_number_info_.~MagicNumberInfo();
      break;
  }
}

void TraceInfoContent::MoveFrom(TraceInfoContent&& other) {
  type_ = other.type_;
  switch (type_) {
    case TraceInfoType::kMagicNumber:
      new (&magic_number_info_) MagicNumberInfo(std::move(other.magic_number_info_));
      break;
  }
}

fbl::String TraceInfoContent::ToString() const {
  switch (type_) {
    case TraceInfoType::kMagicNumber:
      return fbl::StringPrintf("MagicNumberInfo(magic_value: 0x%" PRIx32 ")",
                               magic_number_info_.magic_value);
  }
  ZX_ASSERT(false);
}

void MetadataContent::Destroy() {
  switch (type_) {
    case MetadataType::kProviderInfo:
      provider_info_.~ProviderInfo();
      break;
    case MetadataType::kProviderSection:
      provider_section_.~ProviderSection();
      break;
    case MetadataType::kProviderEvent:
      provider_event_.~ProviderEvent();
      break;
    case MetadataType::kTraceInfo:
      trace_info_.~TraceInfo();
      break;
  }
}

void MetadataContent::MoveFrom(MetadataContent&& other) {
  type_ = other.type_;
  switch (type_) {
    case MetadataType::kProviderInfo:
      new (&provider_info_) ProviderInfo(std::move(other.provider_info_));
      break;
    case MetadataType::kProviderSection:
      new (&provider_section_) ProviderSection(std::move(other.provider_section_));
      break;
    case MetadataType::kProviderEvent:
      new (&provider_event_) ProviderEvent(std::move(other.provider_event_));
      break;
    case MetadataType::kTraceInfo:
      new (&trace_info_) TraceInfo(std::move(other.trace_info_));
      break;
  }
}

fbl::String MetadataContent::ToString() const {
  switch (type_) {
    case MetadataType::kProviderInfo:
      return fbl::StringPrintf("ProviderInfo(id: %" PRId32 ", name: \"%s\")", provider_info_.id,
                               provider_info_.name.c_str());
    case MetadataType::kProviderSection:
      return fbl::StringPrintf("ProviderSection(id: %" PRId32 ")", provider_section_.id);
    case MetadataType::kProviderEvent: {
      fbl::String name;
      ProviderEventType type = provider_event_.event;
      switch (type) {
        case ProviderEventType::kBufferOverflow:
          name = "buffer overflow";
          break;
        default:
          name = fbl::StringPrintf("unknown(%u)", static_cast<unsigned>(type));
          break;
      }
      return fbl::StringPrintf("ProviderEvent(id: %" PRId32 ", %s)", provider_event_.id,
                               name.c_str());
    }
    case MetadataType::kTraceInfo: {
      return fbl::StringPrintf("TraceInfo(content: %s)", trace_info_.content.ToString().c_str());
    }
  }
  ZX_ASSERT(false);
}

void EventData::Destroy() {
  switch (type_) {
    case EventType::kInstant:
      instant_.~Instant();
      break;
    case EventType::kCounter:
      counter_.~Counter();
      break;
    case EventType::kDurationBegin:
      duration_begin_.~DurationBegin();
      break;
    case EventType::kDurationEnd:
      duration_end_.~DurationEnd();
      break;
    case EventType::kDurationComplete:
      duration_complete_.~DurationComplete();
      break;
    case EventType::kAsyncBegin:
      async_begin_.~AsyncBegin();
      break;
    case EventType::kAsyncInstant:
      async_instant_.~AsyncInstant();
      break;
    case EventType::kAsyncEnd:
      async_end_.~AsyncEnd();
      break;
    case EventType::kFlowBegin:
      flow_begin_.~FlowBegin();
      break;
    case EventType::kFlowStep:
      flow_step_.~FlowStep();
      break;
    case EventType::kFlowEnd:
      flow_end_.~FlowEnd();
      break;
  }
}

void EventData::MoveFrom(EventData&& other) {
  type_ = other.type_;
  switch (type_) {
    case EventType::kInstant:
      new (&instant_) Instant(std::move(other.instant_));
      break;
    case EventType::kCounter:
      new (&counter_) Counter(std::move(other.counter_));
      break;
    case EventType::kDurationBegin:
      new (&duration_begin_) DurationBegin(std::move(other.duration_begin_));
      break;
    case EventType::kDurationEnd:
      new (&duration_end_) DurationEnd(std::move(other.duration_end_));
      break;
    case EventType::kDurationComplete:
      new (&duration_complete_) DurationComplete(std::move(other.duration_complete_));
      break;
    case EventType::kAsyncBegin:
      new (&async_begin_) AsyncBegin(std::move(other.async_begin_));
      break;
    case EventType::kAsyncInstant:
      new (&async_instant_) AsyncInstant(std::move(other.async_instant_));
      break;
    case EventType::kAsyncEnd:
      new (&async_end_) AsyncEnd(std::move(other.async_end_));
      break;
    case EventType::kFlowBegin:
      new (&flow_begin_) FlowBegin(std::move(other.flow_begin_));
      break;
    case EventType::kFlowStep:
      new (&flow_step_) FlowStep(std::move(other.flow_step_));
      break;
    case EventType::kFlowEnd:
      new (&flow_end_) FlowEnd(std::move(other.flow_end_));
      break;
  }
}

fbl::String EventData::ToString() const {
  switch (type_) {
    case EventType::kInstant:
      return fbl::StringPrintf("Instant(scope: %s)", EventScopeToString(instant_.scope));
    case EventType::kCounter:
      return fbl::StringPrintf("Counter(id: %" PRIu64 ")", counter_.id);
    case EventType::kDurationBegin:
      return "DurationBegin";
    case EventType::kDurationEnd:
      return "DurationEnd";
    case EventType::kDurationComplete:
      return fbl::StringPrintf("DurationComplete(end_ts: %" PRIu64 ")",
                               duration_complete_.end_time);
    case EventType::kAsyncBegin:
      return fbl::StringPrintf("AsyncBegin(id: %" PRIu64 ")", async_begin_.id);
    case EventType::kAsyncInstant:
      return fbl::StringPrintf("AsyncInstant(id: %" PRIu64 ")", async_instant_.id);
    case EventType::kAsyncEnd:
      return fbl::StringPrintf("AsyncEnd(id: %" PRIu64 ")", async_end_.id);
    case EventType::kFlowBegin:
      return fbl::StringPrintf("FlowBegin(id: %" PRIu64 ")", flow_begin_.id);
    case EventType::kFlowStep:
      return fbl::StringPrintf("FlowStep(id: %" PRIu64 ")", flow_step_.id);
    case EventType::kFlowEnd:
      return fbl::StringPrintf("FlowEnd(id: %" PRIu64 ")", flow_end_.id);
  }
  ZX_ASSERT(false);
}

void LargeRecordData::Destroy() {
  switch (type_) {
    case LargeRecordType::kBlob:
      blob_.~Blob();
      break;
  }
}

void LargeRecordData::MoveFrom(trace::LargeRecordData&& other) {
  switch (type_) {
    case LargeRecordType::kBlob:
      new (&blob_) Blob(std::move(other.blob_));
      break;
  }
}

fbl::String LargeRecordData::ToString() const {
  switch (type_) {
    case LargeRecordType::kBlob:
      if (cpp17::holds_alternative<BlobEvent>(blob_)) {
        const auto& data = cpp17::get<BlobEvent>(blob_);
        return fbl::StringPrintf(
            "Blob(format: blob_event, category: \"%s\", name: \"%s\", "
            "ts: %" PRIu64
            ", pt: %s, %s, "
            "size: %" PRIu64 ", preview: %s)",
            data.category.c_str(), data.name.c_str(), data.timestamp,
            data.process_thread.ToString().c_str(), FormatArgumentList(data.arguments).c_str(),
            data.blob_size, PreviewBlobData<8, 8>(data.blob, data.blob_size).c_str());
      } else if (cpp17::holds_alternative<BlobAttachment>(blob_)) {
        const auto& data = cpp17::get<BlobAttachment>(blob_);
        return fbl::StringPrintf(
            "Blob(format: blob_attachment, category: \"%s\", name: \"%s\", "
            "size: %" PRIu64 ", preview: %s)",
            data.category.c_str(), data.name.c_str(), data.blob_size,
            PreviewBlobData<8, 8>(data.blob, data.blob_size).c_str());
      }
      break;
  }
  ZX_ASSERT(false);
}

void Record::Destroy() {
  switch (type_) {
    case RecordType::kMetadata:
      metadata_.~Metadata();
      break;
    case RecordType::kInitialization:
      initialization_.~Initialization();
      break;
    case RecordType::kString:
      string_.~String();
      break;
    case RecordType::kThread:
      thread_.~Thread();
      break;
    case RecordType::kEvent:
      event_.~Event();
      break;
    case RecordType::kBlob:
      blob_.~Blob();
      break;
    case RecordType::kKernelObject:
      kernel_object_.~KernelObject();
      break;
    case RecordType::kScheduler:
      scheduler_event_.~SchedulerEvent();
      break;
    case RecordType::kLog:
      log_.~Log();
      break;
    case RecordType::kLargeRecord:
      large_.~Large();
      break;
  }
}

void Record::MoveFrom(Record&& other) {
  type_ = other.type_;
  switch (type_) {
    case RecordType::kMetadata:
      new (&metadata_) Metadata(std::move(other.metadata_));
      break;
    case RecordType::kInitialization:
      new (&initialization_) Initialization(std::move(other.initialization_));
      break;
    case RecordType::kString:
      new (&string_) String(std::move(other.string_));
      break;
    case RecordType::kThread:
      new (&thread_) Thread(std::move(other.thread_));
      break;
    case RecordType::kEvent:
      new (&event_) Event(std::move(other.event_));
      break;
    case RecordType::kBlob:
      new (&blob_) Blob(std::move(other.blob_));
      break;
    case RecordType::kKernelObject:
      new (&kernel_object_) KernelObject(std::move(other.kernel_object_));
      break;
    case RecordType::kScheduler:
      new (&scheduler_event_) SchedulerEvent(std::move(other.scheduler_event_));
      break;
    case RecordType::kLog:
      new (&log_) Log(std::move(other.log_));
      break;
    case RecordType::kLargeRecord:
      new (&large_) Large(std::move(other.large_));
      break;
  }
}

fbl::String Record::ToString() const {
  switch (type_) {
    case RecordType::kMetadata:
      return fbl::StringPrintf("Metadata(content: %s)", metadata_.content.ToString().c_str());
    case RecordType::kInitialization:
      return fbl::StringPrintf("Initialization(ticks_per_second: %" PRIu64 ")",
                               initialization_.ticks_per_second);
    case RecordType::kString:
      return fbl::StringPrintf("String(index: %" PRIu32 ", \"%s\")", string_.index,
                               string_.string.c_str());
    case RecordType::kThread:
      return fbl::StringPrintf("Thread(index: %" PRIu32 ", %s)", thread_.index,
                               thread_.process_thread.ToString().c_str());
    case RecordType::kEvent:
      return fbl::StringPrintf(
          "Event(ts: %" PRIu64 ", pt: %s, category: \"%s\", name: \"%s\", %s, %s)",
          event_.timestamp, event_.process_thread.ToString().c_str(), event_.category.c_str(),
          event_.name.c_str(), event_.data.ToString().c_str(),
          FormatArgumentList(event_.arguments).c_str());
      break;
    case RecordType::kBlob:
      return fbl::StringPrintf("Blob(name: %s, size: %zu, preview: %s)", blob_.name.c_str(),
                               blob_.blob_size,
                               PreviewBlobData<8, 8>(blob_.blob, blob_.blob_size).c_str());
    case RecordType::kKernelObject:
      return fbl::StringPrintf("KernelObject(koid: %" PRIu64 ", type: %s, name: \"%s\", %s)",
                               kernel_object_.koid, ObjectTypeToString(kernel_object_.object_type),
                               kernel_object_.name.c_str(),
                               FormatArgumentList(kernel_object_.arguments).c_str());
      break;
    case RecordType::kScheduler:
      if (scheduler_event_.type() == SchedulerEventType::kLegacyContextSwitch) {
        auto& context_switch = scheduler_event_.legacy_context_switch();
        return fbl::StringPrintf("ContextSwitch(ts: %" PRIu64 ", cpu: %" PRIu32
                                 ", os: %s, opt: %s, ipt: %s"
                                 ", oprio: %" PRIu32 ", iprio: %" PRIu32 ")",
                                 context_switch.timestamp, context_switch.cpu_number,
                                 ThreadStateToString(context_switch.outgoing_thread_state),
                                 context_switch.outgoing_thread.ToString().c_str(),
                                 context_switch.incoming_thread.ToString().c_str(),
                                 context_switch.outgoing_thread_priority,
                                 context_switch.incoming_thread_priority);
      }
      if (scheduler_event_.type() == SchedulerEventType::kContextSwitch) {
        auto& context_switch = scheduler_event_.context_switch();
        return fbl::StringPrintf("ContextSwitch(ts: %" PRIu64 ", cpu: %" PRIu32
                                 ", os: %s, ot: %" PRIu64 ", it: %" PRIu64 ", %s)",
                                 context_switch.timestamp, context_switch.cpu_number,
                                 ThreadStateToString(context_switch.outgoing_thread_state),
                                 context_switch.outgoing_tid, context_switch.incoming_tid,
                                 FormatArgumentList(context_switch.arguments).c_str());
      }
      if (scheduler_event_.type() == SchedulerEventType::kThreadWakeup) {
        auto& thread_wakeup = scheduler_event_.thread_wakeup();
        return fbl::StringPrintf(
            "ThreadWakeup(ts: %" PRIu64 ", cpu: %" PRIu32 ", it: %" PRIu64 ", %s)",
            thread_wakeup.timestamp, thread_wakeup.cpu_number, thread_wakeup.incoming_tid,
            FormatArgumentList(thread_wakeup.arguments).c_str());
      }
      return fbl::StringPrintf("UnknownSchedulerEvent(type: %d)",
                               static_cast<int>(scheduler_event_.type()));
    case RecordType::kLog:
      return fbl::StringPrintf("Log(ts: %" PRIu64 ", pt: %s, \"%s\")", log_.timestamp,
                               log_.process_thread.ToString().c_str(), log_.message.c_str());
    case RecordType::kLargeRecord:
      return fbl::StringPrintf("LargeRecord(%s)", large_.ToString().c_str());
  }
  ZX_ASSERT(false);
}

std::optional<fbl::String> Record::GetName() const {
  switch (type_) {
    // Do not have a namefield
    case RecordType::kMetadata:
    case RecordType::kInitialization:
    case RecordType::kString:
    case RecordType::kThread:
    case RecordType::kScheduler:
    case RecordType::kLog:
    case RecordType::kLargeRecord:
      return std::nullopt;
    case RecordType::kEvent:
      return {event_.name};
    case RecordType::kBlob:
      return {blob_.name};
    case RecordType::kKernelObject:
      return {kernel_object_.name};
  }
}

}  // namespace trace
