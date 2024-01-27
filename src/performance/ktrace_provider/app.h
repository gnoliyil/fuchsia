// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_PERFORMANCE_KTRACE_PROVIDER_APP_H_
#define SRC_PERFORMANCE_KTRACE_PROVIDER_APP_H_

#include <lib/sys/cpp/component_context.h>
#include <lib/trace-provider/provider.h>
#include <lib/trace/observer.h>

#include <fbl/unique_fd.h>

#include "src/lib/fxl/command_line.h"
#include "src/performance/ktrace_provider/log_importer.h"

namespace ktrace_provider {

std::vector<trace::KnownCategory> GetKnownCategories();

class App {
 public:
  explicit App(const fxl::CommandLine& command_line);
  ~App();

 private:
  void UpdateState();

  void StartKTrace(uint32_t group_mask, trace_buffering_mode_t buffering_mode,
                   bool retain_current_data);
  void StopKTrace();

  std::unique_ptr<sys::ComponentContext> component_context_;
  trace::TraceObserver trace_observer_;
  LogImporter log_importer_;
  uint32_t current_group_mask_ = 0u;
  // This context keeps the trace context alive until we've written our trace
  // records, which doesn't happen until after tracing has stopped.
  trace_prolonged_context_t* context_ = nullptr;

  App(const App&) = delete;
  App(App&&) = delete;
  App& operator=(const App&) = delete;
  App& operator=(App&&) = delete;
};

}  // namespace ktrace_provider

#endif  // SRC_PERFORMANCE_KTRACE_PROVIDER_APP_H_
