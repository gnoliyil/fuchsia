// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_PERFORMANCE_TRACE_MANAGER_TRACE_MANAGER_H_
#define SRC_PERFORMANCE_TRACE_MANAGER_TRACE_MANAGER_H_

#include <fuchsia/tracing/controller/cpp/fidl.h>
#include <fuchsia/tracing/cpp/fidl.h>
#include <fuchsia/tracing/provider/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_ptr_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/socket.h>

#include <list>
#include <queue>

#include "src/performance/trace_manager/config.h"
#include "src/performance/trace_manager/trace_provider_bundle.h"
#include "src/performance/trace_manager/trace_session.h"

namespace tracing {

namespace controller = fuchsia::tracing::controller;
namespace provider = fuchsia::tracing::provider;

// forward decl, here to break mutual header dependency
class TraceManagerApp;

class TraceManager : public controller::Controller, public provider::Registry {
 public:
  TraceManager(TraceManagerApp* app, Config config, async_dispatcher_t* dispatcher);
  ~TraceManager() override;

  // For testing.
  const TraceSession* session() const { return session_.get(); }

  void OnEmptyControllerSet();

 private:
  // |Controller| implementation.
  void InitializeTracing(controller::TraceConfig config, zx::socket output) override;
  void TerminateTracing(controller::TerminateOptions options, TerminateTracingCallback cb) override;
  void StartTracing(controller::StartOptions options, StartTracingCallback cb) override;
  void StopTracing(controller::StopOptions options, StopTracingCallback cb) override;
  void GetProviders(GetProvidersCallback cb) override;
  void GetKnownCategories(GetKnownCategoriesCallback callback) override;
  void WatchAlert(WatchAlertCallback cb) override;

  // |TraceRegistry| implementation.
  void RegisterProviderWorker(fidl::InterfaceHandle<provider::Provider> provider, uint64_t pid,
                              fidl::StringPtr name);
  void RegisterProvider(fidl::InterfaceHandle<provider::Provider> provider, uint64_t pid,
                        std::string name) override;
  void RegisterProviderSynchronously(fidl::InterfaceHandle<provider::Provider> provider,
                                     uint64_t pid, std::string name,
                                     RegisterProviderSynchronouslyCallback callback) override;

  void SendSessionStateEvent(controller::SessionState state);
  controller::SessionState TranslateSessionState(TraceSession::State state);

  void OnAlert(const std::string& alert_name);

  TraceManagerApp* const app_;

  const Config config_;

  uint32_t next_provider_id_ = 1u;
  fxl::RefPtr<TraceSession> session_;
  std::list<TraceProviderBundle> providers_;
  std::queue<std::string> alerts_;
  std::queue<WatchAlertCallback> watch_alert_callbacks_;
  async::Executor executor_;

  TraceManager(const TraceManager&) = delete;
  TraceManager(TraceManager&&) = delete;
  TraceManager& operator=(const TraceManager&) = delete;
  TraceManager& operator=(TraceManager&&) = delete;
};

}  // namespace tracing

#endif  // SRC_PERFORMANCE_TRACE_MANAGER_TRACE_MANAGER_H_
