// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "provider_impl.h"

#include <fidl/fuchsia.tracing.provider/cpp/fidl.h>
#include <fidl/fuchsia.tracing/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/zx/process.h>
#include <stdio.h>
#include <zircon/assert.h>
#include <zircon/status.h>

#include <utility>

#include "export.h"
#include "lib/trace-engine/handler.h"
#include "lib/trace-engine/types.h"
#include "lib/trace-provider/provider.h"
#include "session.h"
#include "utils.h"

namespace {
trace_buffering_mode_t FidlBufferingModeToTraceEngineBufferingMode(
    fuchsia_tracing::BufferingMode buffering_mode) {
  switch (buffering_mode) {
    case fuchsia_tracing::BufferingMode::kOneshot:
      return TRACE_BUFFERING_MODE_ONESHOT;
    case fuchsia_tracing::BufferingMode::kCircular:
      return TRACE_BUFFERING_MODE_CIRCULAR;
    case fuchsia_tracing::BufferingMode::kStreaming:
      return TRACE_BUFFERING_MODE_STREAMING;
  }
}

trace_start_mode_t FidlBufferingDispositionToTraceEngineStartMode(
    fuchsia_tracing::BufferDisposition buffer_disposition) {
  switch (buffer_disposition) {
    case fuchsia_tracing::BufferDisposition::kClearEntire:
      return TRACE_START_CLEAR_ENTIRE_BUFFER;
    case fuchsia_tracing::BufferDisposition::kClearNondurable:
      return TRACE_START_CLEAR_NONDURABLE_BUFFER;
    case fuchsia_tracing::BufferDisposition::kRetain:
      return TRACE_START_RETAIN_BUFFER;
  }
}

std::vector<std::string> CloneCategories(const fuchsia_tracing_provider::ProviderConfig& config) {
  std::vector<std::string> categories;
  categories.reserve(config.categories().size());
  for (const auto& category : config.categories()) {
    categories.emplace_back(category.data(), category.size());
  }
  return categories;
}
}  // namespace

namespace trace {
namespace internal {

constexpr bool kVerboseTraceErrors = false;

TraceProviderImpl::TraceProviderImpl(std::string name, async_dispatcher_t* dispatcher,
                                     fidl::ServerEnd<fuchsia_tracing_provider::Provider> server_end)
    : name_(std::move(name)), dispatcher_(dispatcher), executor_(dispatcher) {
  fidl::BindServer(
      dispatcher_, std::move(server_end), this,
      [](TraceProviderImpl* impl, fidl::UnbindInfo info,
         fidl::ServerEnd<fuchsia_tracing_provider::Provider> server_end) { OnClose(); });
}

void TraceProviderImpl::Initialize(InitializeRequest& request,
                                   InitializeCompleter::Sync& completer) {
  fuchsia_tracing_provider::ProviderConfig& config = request.config();
  Session::InitializeEngine(
      dispatcher_, FidlBufferingModeToTraceEngineBufferingMode(config.buffering_mode()),
      std::move(config.buffer()), std::move(config.fifo()), CloneCategories(config));
  provider_config_ = {
      .buffering_mode = FidlBufferingModeToTraceEngineBufferingMode(config.buffering_mode()),
      .categories = CloneCategories(config),
  };
}

void TraceProviderImpl::Start(StartRequest& request, StartCompleter::Sync& completer) {
  const fuchsia_tracing_provider::StartOptions& options = request.options();
  // TODO(fxbug.dev/22973): Add support for additional categories.
  Session::StartEngine(
      FidlBufferingDispositionToTraceEngineStartMode(options.buffer_disposition()));
}

void TraceProviderImpl::Stop(StopCompleter::Sync& completer) { Session::StopEngine(); }

void TraceProviderImpl::Terminate(TerminateCompleter::Sync& completer) { OnClose(); }

void TraceProviderImpl::GetKnownCategories(GetKnownCategoriesCompleter::Sync& completer) {
  if (get_known_categories_callback_ != nullptr) {
    auto promise = get_known_categories_callback_().then(
        [completer = completer.ToAsync(),
         name = name_](fpromise::result<std::vector<trace::KnownCategory>>& result) mutable {
          if (result.is_error()) {
            fprintf(stderr, "TraceProvider: error getting known categories for %s\n", name.c_str());
            completer.Reply({});
            return;
          }
          auto known_categories = result.take_value();
          std::vector<fuchsia_tracing::KnownCategory> known_categories_fidl;
          known_categories_fidl.reserve(known_categories.size());
          for (const auto& known_category : known_categories) {
            known_categories_fidl.emplace_back(known_category.name, known_category.description);
          }
          completer.Reply(known_categories_fidl);
        });
    executor_.schedule_task(std::move(promise));
    return;
  }

  // TODO(fxb/117525): Return the trace categories that were registered with the category string
  // literal.
  completer.Reply({});
}

void TraceProviderImpl::SetGetKnownCategoriesCallback(GetKnownCategoriesCallback callback) {
  get_known_categories_callback_ = std::move(callback);
}

void TraceProviderImpl::OnClose() { Session::TerminateEngine(); }

const ProviderConfig& TraceProviderImpl::GetProviderConfig() const { return provider_config_; }

}  // namespace internal

ProviderConfig TraceProvider::GetProviderConfig() const {
  ZX_DEBUG_ASSERT(provider_);
  const auto* provider_impl = reinterpret_cast<internal::TraceProviderImpl*>(provider_);
  return provider_impl->GetProviderConfig();
}

void TraceProvider::SetGetKnownCategoriesCallback(GetKnownCategoriesCallback callback) {
  ZX_DEBUG_ASSERT(provider_);
  auto* provider_impl = reinterpret_cast<internal::TraceProviderImpl*>(provider_);
  provider_impl->SetGetKnownCategoriesCallback(std::move(callback));
}
}  // namespace trace

EXPORT trace_provider_t* trace_provider_create_with_name(zx_handle_t to_service_h,
                                                         async_dispatcher_t* dispatcher,
                                                         const char* name) {
  const fidl::ClientEnd<fuchsia_tracing_provider::Registry> to_service{zx::channel{to_service_h}};

  ZX_DEBUG_ASSERT(to_service.is_valid());
  ZX_DEBUG_ASSERT(dispatcher);

  // Create the channel to which we will bind the trace provider.
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_tracing_provider::Provider>();
  if (endpoints.is_error()) {
    fprintf(stderr, "TraceProvider: channel create failed: status=%d(%s)\n",
            endpoints.status_value(), endpoints.status_string());
    return nullptr;
  }

  // Register the trace provider.
  const fidl::Status result =
      fidl::WireCall(to_service)
          ->RegisterProvider(std::move(endpoints->client), trace::internal::GetPid(),
                             fidl::StringView::FromExternal(name));
  if (!result.ok()) {
    if (trace::internal::kVerboseTraceErrors) {
      fprintf(stderr, "TraceProvider: registry failed: result=%s\n",
              result.FormatDescription().c_str());
    }
    return nullptr;
  }
  // Note: |to_service| can be closed now. Let it close as a consequence
  // of going out of scope.

  return new trace::internal::TraceProviderImpl(name, dispatcher, std::move(endpoints->server));
}

EXPORT trace_provider_t* trace_provider_create(zx_handle_t to_service,
                                               async_dispatcher_t* dispatcher) {
  auto self = zx::process::self();
  char name[ZX_MAX_NAME_LEN];
  auto status = self->get_property(ZX_PROP_NAME, name, sizeof(name));
  if (status != ZX_OK) {
    fprintf(stderr, "TraceProvider: error getting process name: status=%d(%s)\n", status,
            zx_status_get_string(status));
    name[0] = '\0';
  }
  return trace_provider_create_with_name(to_service, dispatcher, name);
}

EXPORT trace_provider_t* trace_provider_create_synchronously(zx_handle_t to_service_h,
                                                             async_dispatcher_t* dispatcher,
                                                             const char* name,
                                                             bool* out_already_started) {
  const fidl::ClientEnd<fuchsia_tracing_provider::Registry> to_service{zx::channel{to_service_h}};

  ZX_DEBUG_ASSERT(to_service.is_valid());
  ZX_DEBUG_ASSERT(dispatcher);

  // Create the channel to which we will bind the trace provider.
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_tracing_provider::Provider>();
  if (endpoints.is_error()) {
    fprintf(stderr, "TraceProvider: channel create failed: status=%d(%s)\n",
            endpoints.status_value(), endpoints.status_string());
    return nullptr;
  }

  // Register the trace provider.
  const fidl::WireResult result =
      fidl::WireCall(to_service)
          ->RegisterProviderSynchronously(std::move(endpoints->client), trace::internal::GetPid(),
                                          fidl::StringView::FromExternal(name));
  if (!result.ok()) {
    fprintf(stderr, "TraceProvider: RegisterProviderSynchronously failed: result=%s\n",
            result.FormatDescription().c_str());
    return nullptr;
  }
  const fidl::WireResponse response = result.value();
  if (const zx_status_t status = response.s; status != ZX_OK) {
    fprintf(stderr, "TraceProvider: registry failed: status=%d(%s)\n", status,
            zx_status_get_string(status));
    return nullptr;
  }
  // Note: |to_service| can be closed now. Let it close as a consequence
  // of going out of scope.

  if (out_already_started) {
    *out_already_started = response.started;
  }
  return new trace::internal::TraceProviderImpl(name, dispatcher, std::move(endpoints->server));
}

EXPORT void trace_provider_destroy(trace_provider_t* provider) {
  ZX_DEBUG_ASSERT(provider);

  // The provider's dispatcher may be running on a different thread. This happens when, e.g., the
  // dispatcher is running in a background thread and we are called in the foreground thread.
  // async::WaitBase, which we use, requires all calls be made on the dispatcher thread. Thus we
  // can't delete |provider| here. Instead we schedule it to be deleted on the dispatcher's thread.
  //
  // There are two cases to be handled:
  // 1) The dispatcher's thread is our thread.
  // 2) The dispatcher's thread is a different thread.
  // In both cases there's an additional wrinkle:
  // a) The task we post is run.
  // b) The task we post is not run.
  // In cases (1a,2a) we're ok: The provider is deleted. The provider isn't destroyed immediately
  // but that's ok, it will be shortly.
  // In cases (1b,2b) we're also ok. The only time this happens is if the loop is shutdown before
  // our task is run. This is ok because when this happens our WaitBase method cannot be running.
  //
  // While one might want to check whether we're running in a different thread from the dispatcher
  // with dispatcher == async_get_default_dispatcher(), we don't do this as we don't assume the
  // default dispatcher has been set.

  auto raw_provider_impl = static_cast<trace::internal::TraceProviderImpl*>(provider);
  std::unique_ptr<trace::internal::TraceProviderImpl> provider_impl(raw_provider_impl);
  async::PostTask(raw_provider_impl->dispatcher(), [provider_impl = std::move(provider_impl)]() {
    // The provider will be deleted when the closure is deleted.
  });
}
