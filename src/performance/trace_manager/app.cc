// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/performance/trace_manager/app.h"

#include <lib/syslog/cpp/macros.h>

#include <utility>

namespace tracing {

TraceManagerApp::TraceManagerApp(std::unique_ptr<sys::ComponentContext> context, Config config,
                                 async_dispatcher_t* dispatcher)
    : context_(std::move(context)), trace_manager_(this, std::move(config), dispatcher) {
  [[maybe_unused]] zx_status_t status;

  status = context_->outgoing()->AddPublicService(
      provider_registry_bindings_.GetHandler(&trace_manager_));
  FX_DCHECK(status == ZX_OK);

  status = context_->outgoing()->AddPublicService(controller_bindings_.GetHandler(&trace_manager_));
  FX_DCHECK(status == ZX_OK);
  controller_bindings_.set_empty_set_handler([this]() { trace_manager_.OnEmptyControllerSet(); });

  FX_VLOGS(2) << "TraceManager services registered";
}

TraceManagerApp::~TraceManagerApp() = default;

}  // namespace tracing
