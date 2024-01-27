// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/performance/perfetto-bridge/consumer_adapter.h"

#include <fcntl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fit/function.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <unistd.h>
#include <zircon/status.h>

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <vector>

#include <perfetto/ext/tracing/core/trace_packet.h>
#include <perfetto/ext/tracing/core/tracing_service.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "lib/fpromise/bridge.h"
#include "lib/trace-engine/context.h"
#include "lib/trace-engine/instrumentation.h"
#include "lib/trace-provider/provider.h"
#include "src/lib/fxl/strings/string_printf.h"

#include <protos/perfetto/common/track_event_descriptor.gen.h>
#include <protos/perfetto/config/track_event/track_event_config.gen.h>
#include <third_party/perfetto/protos/perfetto/common/data_source_descriptor.gen.h>
#include <third_party/perfetto/protos/perfetto/common/trace_stats.gen.h>
#include <third_party/perfetto/protos/perfetto/common/tracing_service_state.gen.h>
#include <third_party/perfetto/protos/perfetto/config/chrome/chrome_config.gen.h>
#include <third_party/perfetto/protos/perfetto/config/data_source_config.gen.h>
#include <third_party/perfetto/protos/perfetto/config/trace_config.gen.h>
#include <third_party/perfetto/protos/perfetto/config/track_event/track_event_config.gen.h>

namespace {
// The size of the consumer buffer.
constexpr size_t kConsumerBufferSizeKb = 20ul * 1024ul;  // 20MB.

// The delay between buffer utilization checks.
constexpr int kConsumerStatsPollIntervalMs = 500;

// Interval for recreating interned string data, in milliseconds.
// Used for stream recovery in the event of data loss.
constexpr uint32_t kIncrementalStateClearMs = 4000;

constexpr char kBlobName[] = "perfetto-bridge";
constexpr char kChromiumTraceEvent[] = "org.chromium.trace_event";

class EmptyConsumer : public perfetto::Consumer {
 private:
  void OnConnect() override {}
  void OnDisconnect() override {}
  void OnTracingDisabled(const std::string& error) override {}
  void OnTraceData(std::vector<perfetto::TracePacket> packets, bool has_more) override {}
  void OnDetach(bool success) override {}
  void OnAttach(bool success, const perfetto::TraceConfig&) override {}
  void OnTraceStats(bool success, const perfetto::TraceStats&) override {}
  void OnObservableEvents(const perfetto::ObservableEvents&) override {}
};

void LogTraceStats(const perfetto::TraceStats& stats) {
  const auto& buffer_stats = stats.buffer_stats().front();
  FX_LOGS(INFO) << fxl::StringPrintf(
      "Trace stats: "
      "producers_connected: %u , "
      "data_sources_registered: %u , "
      "tracing_sessions: %u",
      stats.producers_connected(), stats.data_sources_registered(), stats.tracing_sessions());
  FX_LOGS(INFO) << fxl::StringPrintf(
      "Consumer buffer stats :"
      "consumer bytes_written: %lu, "
      "consumer bytes_read: %lu, "
      "consumer bytes_overwritten (lost): %lu, ",
      buffer_stats.bytes_written(), buffer_stats.bytes_read(), buffer_stats.bytes_overwritten());
  if (buffer_stats.bytes_overwritten() > 0) {
    // If too much data was lost, then the consumer buffer should be enlarged
    // and/or the drain interval shortened.
    FX_LOGS(WARNING) << "Perfetto consumer buffer overrun detected.";
  }
}

// TODO(fxbug.dev/115525): Remove this once the migration to track_event_config is complete.
std::string GetChromeTraceConfigString(
    const perfetto::protos::gen::TrackEventConfig& track_event_config) {
  rapidjson::Document chrome_trace_config(rapidjson::kObjectType);
  auto& allocator = chrome_trace_config.GetAllocator();

  rapidjson::Value included_categories(rapidjson::kArrayType);
  for (const auto& enabled_category : track_event_config.enabled_categories()) {
    included_categories.PushBack(rapidjson::StringRef(enabled_category), allocator);
  }
  chrome_trace_config.AddMember("included_categories", included_categories, allocator);

  rapidjson::Value excluded_categories(rapidjson::kArrayType);
  for (const auto& disabled_category : track_event_config.disabled_categories()) {
    excluded_categories.PushBack(rapidjson::StringRef(disabled_category), allocator);
  }
  chrome_trace_config.AddMember("excluded_categories", excluded_categories, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer writer(buffer);
  chrome_trace_config.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

perfetto::protos::gen::TrackEventConfig GetTrackEventConfig(
    const trace::ProviderConfig& provider_config) {
  perfetto::protos::gen::TrackEventConfig track_event_config;
  if (!provider_config.categories.empty()) {
    // Disable all categories that aren't added to `enabled_categories`
    track_event_config.add_disabled_categories("*");
  }
  for (const auto& enabled_category : provider_config.categories) {
    track_event_config.add_enabled_categories(enabled_category);
  }
  return track_event_config;
}

class FuchsiaTracingImpl : public FuchsiaTracing {
 public:
  explicit FuchsiaTracingImpl(trace::TraceProvider* trace_provider)
      : trace_provider_(trace_provider) {
    FX_DCHECK(trace_provider_);
  }
  ~FuchsiaTracingImpl() override {
    FX_DCHECK(!prolonged_context_);
    FX_DCHECK(!blob_write_context_);
  }

  void StartObserving(fit::function<void(trace_state_t)> observe_cb) override {
    trace_observer_.Start(
        async_get_default_dispatcher(),
        [observe_cb = std::move(observe_cb)]() mutable { observe_cb(trace_state()); });
  }

  void AcquireProlongedContext() override {
    FX_DCHECK(!prolonged_context_);

    prolonged_context_ = trace_acquire_prolonged_context();
    FX_DCHECK(prolonged_context_);
  }

  void ReleaseProlongedContext() override {
    FX_DCHECK(prolonged_context_);
    FX_DCHECK(!blob_write_context_);

    trace_release_prolonged_context(prolonged_context_);
    prolonged_context_ = nullptr;
  }

  void AcquireWriteContext() override {
    FX_DCHECK(!blob_write_context_);

    blob_write_context_ = trace_acquire_context();
    if (blob_write_context_) {
      trace_context_register_string_literal(blob_write_context_, kBlobName, &blob_name_ref_);
    }
  }

  bool HasWriteContext() override { return blob_write_context_ != nullptr; }

  void WriteBlob(const char* data, size_t size) override {
    FX_DCHECK(blob_write_context_);
    trace_context_write_blob_record(blob_write_context_, TRACE_BLOB_TYPE_PERFETTO, &blob_name_ref_,
                                    data, size);
  }

  void ReleaseWriteContext() override {
    if (blob_write_context_) {
      trace_release_context(blob_write_context_);
      blob_write_context_ = nullptr;
    }
  }

  trace::ProviderConfig GetProviderConfig() override {
    return trace_provider_->GetProviderConfig();
  }

  void SetGetKnownCategoriesCallback(trace::GetKnownCategoriesCallback callback) override {
    trace_provider_->SetGetKnownCategoriesCallback(std::move(callback));
  }

 private:
  trace_prolonged_context_t* prolonged_context_ = nullptr;
  trace_context_t* blob_write_context_ = nullptr;
  trace_string_ref_t blob_name_ref_;
  trace::TraceProvider* trace_provider_;

  // Used for handling Fuchsia trace system events.
  trace::TraceObserver trace_observer_;
};

}  // namespace

ConsumerAdapter::ConsumerAdapter(ConnectConsumerCallback connect_callback,
                                 std::unique_ptr<FuchsiaTracing> fuchsia_tracing,
                                 perfetto::base::TaskRunner* perfetto_task_runner)
    : perfetto_task_runner_(perfetto_task_runner),
      connect_callback_(std::move(connect_callback)),
      fuchsia_tracing_(std::move(fuchsia_tracing)) {
  FX_DCHECK(connect_callback_);
  FX_DCHECK(fuchsia_tracing_);
  FX_DCHECK(perfetto_task_runner_);

  fuchsia_tracing_->SetGetKnownCategoriesCallback([this] { return GetKnownCategories(); });
  fuchsia_tracing_->StartObserving(fit::bind_member(this, &ConsumerAdapter::OnTraceStateUpdate));
}

ConsumerAdapter::ConsumerAdapter(perfetto::TracingService* tracing_service,
                                 trace::TraceProvider* trace_provider,
                                 perfetto::base::TaskRunner* perfetto_task_runner)
    : ConsumerAdapter(
          [tracing_service](perfetto::Consumer* consumer) {
            return tracing_service->ConnectConsumer(consumer, 0);
          },
          std::make_unique<FuchsiaTracingImpl>(trace_provider), perfetto_task_runner) {}

ConsumerAdapter::~ConsumerAdapter() {
  perfetto_task_runner_->PostTask(
      [endpoint = this->consumer_endpoint_.release()]() { delete endpoint; });
}

ConsumerAdapter::State ConsumerAdapter::GetState() {
  std::lock_guard lock(state_mutex_);
  return state_;
}

void ConsumerAdapter::ChangeState(State new_state) {
  std::lock_guard lock(state_mutex_);
  State old_state = state_;

  bool valid_transition = false;
  switch (new_state) {
    case State::INACTIVE:
      valid_transition = old_state == State::SHUTDOWN_STATS;
      break;
    case State::ACTIVE:
      valid_transition =
          old_state == State::INACTIVE || old_state == State::STATS || old_state == State::READING;
      break;
    case State::STATS:
      valid_transition = old_state == State::ACTIVE;
      break;
    case State::READING:
      valid_transition = old_state == State::STATS;
      break;
    case State::SHUTDOWN_FLUSH:
      valid_transition = old_state == State::ACTIVE ||
                         old_state == State::READING_PENDING_SHUTDOWN || old_state == State::STATS;
      break;
    case State::READING_PENDING_SHUTDOWN:
      valid_transition = old_state == State::READING;
      break;
    case State::SHUTDOWN_DISABLED:
      valid_transition = old_state == State::SHUTDOWN_FLUSH || old_state == State::ACTIVE;
      break;
    case State::SHUTDOWN_READING:
      valid_transition = old_state == State::SHUTDOWN_DISABLED;
      break;
    case State::SHUTDOWN_STATS:
      valid_transition = old_state == State::SHUTDOWN_READING;
      break;
  }

  FX_CHECK(valid_transition) << "Unknown state transition: " << static_cast<int>(old_state) << "->"
                             << static_cast<int>(new_state);
  state_ = new_state;
}

void ConsumerAdapter::OnTraceData(std::vector<perfetto::TracePacket> packets, bool has_more) {
  FX_DCHECK(GetState() == State::READING || GetState() == State::SHUTDOWN_READING ||
            GetState() == State::READING_PENDING_SHUTDOWN);
  FX_DCHECK(perfetto_task_runner_->RunsTasksOnCurrentThread());

  if (fuchsia_tracing_->HasWriteContext()) {
    // Proto messages must be written as atomic blobs to prevent truncation mid-message
    // if the output buffer is filled.
    std::string proto_str;
    for (auto& cur_packet : packets) {
      auto [preamble_data, preamble_size] = cur_packet.GetProtoPreamble();
      proto_str.assign(preamble_data, preamble_size);

      for (auto& cur_slice : cur_packet.slices()) {
        proto_str.append(reinterpret_cast<const char*>(cur_slice.start), cur_slice.size);
      }

      if (proto_str.size() > TRACE_MAX_BLOB_SIZE) {
        FX_LOGS(WARNING) << "Dropping excessively long Perfetto message (size=" << proto_str.size()
                         << " bytes)";
      } else {
        fuchsia_tracing_->WriteBlob(proto_str.data(), proto_str.size());
      }
      proto_str.clear();
    }
  }

  if (!has_more) {
    OnPerfettoReadBuffersComplete();
  }
}

void ConsumerAdapter::OnTraceStateUpdate(trace_state_t new_state) {
  switch (new_state) {
    case TRACE_STARTED:
      perfetto_task_runner_->PostTask([this]() { OnStartTracing(); });
      return;
    case TRACE_STOPPING:
      perfetto_task_runner_->PostTask([this]() {
        if (GetState() == State::READING) {
          ChangeState(State::READING_PENDING_SHUTDOWN);
        } else {
          CallPerfettoFlush();
        }
      });
      return;
    case TRACE_STOPPED:
      break;
  }
}

void ConsumerAdapter::OnStartTracing() {
  FX_DCHECK(perfetto_task_runner_->RunsTasksOnCurrentThread());

  perfetto::TraceConfig trace_config;
  trace_config.mutable_incremental_state_config()->set_clear_period_ms(kIncrementalStateClearMs);

  perfetto::TraceConfig::BufferConfig* buffer_config = trace_config.add_buffers();
  buffer_config->set_size_kb(kConsumerBufferSizeKb);

  // RING_BUFFER is the only FillPolicy suitable for streaming, because DISCARD will enter a
  // bad state in the event of consumer buffer saturation (e.g. if there is a burst of data).
  buffer_config->set_fill_policy(perfetto::TraceConfig::BufferConfig::RING_BUFFER);

  perfetto::protos::gen::DataSourceConfig* data_source_config =
      trace_config.add_data_sources()->mutable_config();
  // The data source name is necessary and hardcoded for now, but it should
  // be sourced from FXT somehow.
  data_source_config->set_name("org.chromium.trace_event");

  const auto track_event_config = GetTrackEventConfig(fuchsia_tracing_->GetProviderConfig());
  data_source_config->set_track_event_config_raw(track_event_config.SerializeAsString());

  // TODO(fxbug.dev/115525): Remove this once the migration to track_event_config is complete.
  data_source_config->mutable_chrome_config()->set_trace_config(
      GetChromeTraceConfigString(track_event_config));

  FX_CHECK(!consumer_endpoint_);
  consumer_endpoint_ = connect_callback_(this);
  FX_DCHECK(consumer_endpoint_);
  consumer_endpoint_->EnableTracing(trace_config);

  // Explicitly manage the lifetime of the Fuchsia tracing session.
  fuchsia_tracing_->AcquireProlongedContext();

  ChangeState(State::ACTIVE);
  SchedulePerfettoGetStats();
}

void ConsumerAdapter::CallPerfettoDisableTracing() {
  FX_DCHECK(perfetto_task_runner_->RunsTasksOnCurrentThread());

  ChangeState(State::SHUTDOWN_DISABLED);
  consumer_endpoint_->DisableTracing();
}

void ConsumerAdapter::SchedulePerfettoGetStats() {
  FX_DCHECK(GetState() == State::ACTIVE);

  perfetto_task_runner_->PostDelayedTask(
      [this]() {
        if (GetState() == State::ACTIVE) {
          CallPerfettoGetTraceStats(false /* on_shutdown */);
        }
      },
      kConsumerStatsPollIntervalMs);
}

void ConsumerAdapter::CallPerfettoReadBuffers(bool on_shutdown) {
  FX_DCHECK(!fuchsia_tracing_->HasWriteContext());
  ChangeState(on_shutdown ? State::SHUTDOWN_READING : State::READING);

  fuchsia_tracing_->AcquireWriteContext();
  if (fuchsia_tracing_->HasWriteContext()) {
    consumer_endpoint_->ReadBuffers();
  } else {
    // The Fuchsia tracing context is gone, so there is nowhere to write
    // the data to.
    OnPerfettoReadBuffersComplete();
  }
}

void ConsumerAdapter::OnPerfettoReadBuffersComplete() {
  fuchsia_tracing_->ReleaseWriteContext();

  if (GetState() == State::READING) {
    ChangeState(State::ACTIVE);
    SchedulePerfettoGetStats();
  } else if (GetState() == State::SHUTDOWN_READING) {
    CallPerfettoGetTraceStats(true);
  } else if (GetState() == State::READING_PENDING_SHUTDOWN) {
    CallPerfettoFlush();
  }
}

void ConsumerAdapter::CallPerfettoFlush() {
  ChangeState(State::SHUTDOWN_FLUSH);
  consumer_endpoint_->Flush(0, [this](bool success) {
    if (!success) {
      FX_LOGS(WARNING) << "Flush failed.";
    }
    CallPerfettoDisableTracing();
  });
}

void ConsumerAdapter::CallPerfettoGetTraceStats(bool on_shutdown) {
  ChangeState(on_shutdown ? State::SHUTDOWN_STATS : State::STATS);
  consumer_endpoint_->GetTraceStats();
}

fpromise::promise<std::vector<trace::KnownCategory>> ConsumerAdapter::GetKnownCategories() {
  fpromise::bridge<std::vector<trace::KnownCategory>> bridge;
  auto completer = std::make_shared<fpromise::completer<std::vector<trace::KnownCategory>>>(
      std::move(bridge.completer));
  auto empty_consumer = std::make_shared<EmptyConsumer>();
  const auto consumer_endpoint = connect_callback_(empty_consumer.get());
  auto on_service_state = [completer, empty_consumer](
                              bool success, const perfetto::TracingServiceState& service_state) {
    if (!success) {
      completer->complete_error();
    }
    std::unordered_set<trace::KnownCategory, trace::KnownCategoryHash> categories;
    for (const auto& data_source : service_state.data_sources()) {
      if (data_source.ds_descriptor().name() != kChromiumTraceEvent) {
        continue;
      }
      auto raw_descriptor = data_source.ds_descriptor().track_event_descriptor_raw();
      perfetto::protos::gen::TrackEventDescriptor track_event_descriptor;
      track_event_descriptor.ParseFromArray(raw_descriptor.data(), raw_descriptor.size());
      for (const auto& available_category : track_event_descriptor.available_categories()) {
        categories.insert(
            {.name = available_category.name(), .description = available_category.description()});
      }
    }
    completer->complete_ok(std::vector<trace::KnownCategory>{categories.begin(), categories.end()});
  };
  consumer_endpoint->QueryServiceState(std::move(on_service_state));

  return bridge.consumer.promise();
}

void ConsumerAdapter::OnTracingDisabled(const std::string& error) {
  FX_DCHECK(perfetto_task_runner_->RunsTasksOnCurrentThread());

  if (!error.empty()) {
    FX_LOGS(WARNING) << "OnTracingDisabled() reported an error: " << error;
  }

  CallPerfettoReadBuffers(true /* shutdown */);
}

void ConsumerAdapter::OnTraceStats(bool success, const perfetto::TraceStats& stats) {
  FX_DCHECK(perfetto_task_runner_->RunsTasksOnCurrentThread());

  if (GetState() == State::STATS) {
    if (success) {
      const auto& buffer_stats = stats.buffer_stats().front();
      const size_t buffer_used = buffer_stats.bytes_written() -
                                 (buffer_stats.bytes_read() + buffer_stats.bytes_overwritten());
      const float utilization =
          static_cast<float>(buffer_used) / static_cast<float>(buffer_stats.buffer_size());

      if (utilization >= kConsumerUtilizationReadThreshold) {
        CallPerfettoReadBuffers(false /* shutdown */);
        return;
      }
    }
    ChangeState(State::ACTIVE);
    SchedulePerfettoGetStats();
  } else if (GetState() == State::SHUTDOWN_STATS) {
    ChangeState(State::INACTIVE);

    if (success) {
      LogTraceStats(stats);
    }

    ShutdownTracing();
  }
}

void ConsumerAdapter::ShutdownTracing() {
  consumer_endpoint_.reset();
  fuchsia_tracing_->ReleaseWriteContext();
  fuchsia_tracing_->ReleaseProlongedContext();
}

// Ignored Perfetto Consumer events.
void ConsumerAdapter::OnConnect() {}
void ConsumerAdapter::OnDisconnect() {}
void ConsumerAdapter::OnDetach(bool success) {}
void ConsumerAdapter::OnAttach(bool success, const perfetto::TraceConfig&) {}
void ConsumerAdapter::OnObservableEvents(const perfetto::ObservableEvents&) {}
