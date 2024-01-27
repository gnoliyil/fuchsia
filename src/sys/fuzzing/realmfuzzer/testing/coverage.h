// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_FUZZING_REALMFUZZER_TESTING_COVERAGE_H_
#define SRC_SYS_FUZZING_REALMFUZZER_TESTING_COVERAGE_H_

#include <fuchsia/debugdata/cpp/fidl.h>
#include <fuchsia/fuzzer/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/vmo.h>

#include <string>

#include "src/lib/fxl/macros.h"
#include "src/sys/fuzzing/common/async-deque.h"
#include "src/sys/fuzzing/common/async-types.h"
#include "src/sys/fuzzing/common/options.h"

namespace fuzzing {

using fuchsia::fuzzer::CoverageDataCollectorV2;
using fuchsia::fuzzer::CoverageDataProviderV2;
using fuchsia::fuzzer::CoverageDataV2;

// This class represents a simplified fuzz coverage component. Unlike the real version (located at
// src/sys/test_manager/fuzz_coverage), this version for testing accepts only a single collector
// connection and a single provider connection, and does not use event streams.
class FakeCoverage final : public CoverageDataCollectorV2, CoverageDataProviderV2 {
 public:
  explicit FakeCoverage(ExecutorPtr executor);
  ~FakeCoverage() = default;

  OptionsPtr options() const { return options_; }

  fidl::InterfaceRequestHandler<CoverageDataCollectorV2> GetCollectorHandler();
  fidl::InterfaceRequestHandler<CoverageDataProviderV2> GetProviderHandler();

  // CoverageDataCollector FIDL methods.
  void Initialize(zx::eventpair eventpair, zx::process process,
                  InitializeCallback callback) override;
  void AddInline8bitCounters(zx::vmo inline_8bit_counters,
                             AddInline8bitCountersCallback callback) override;

  // CoverageDataProvider FIDL method.
  void SetOptions(Options options) override;
  void WatchCoverageData(WatchCoverageDataCallback callback) override;

  // Additional methods that allow direct access to the underlying `AsyncDeque` for more flexible
  // testing.
  void Send(CoverageDataV2 coverage_data);
  Result<CoverageDataV2> TryReceive();
  Promise<CoverageDataV2> Receive();

 private:
  fidl::Binding<CoverageDataCollectorV2> collector_;
  fidl::Binding<CoverageDataProviderV2> provider_;
  ExecutorPtr executor_;
  OptionsPtr options_;
  AsyncSender<CoverageDataV2> sender_;
  AsyncReceiver<CoverageDataV2> receiver_;
  bool first_ = false;
  zx_koid_t target_id_ = ZX_KOID_INVALID;
  Scope scope_;

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(FakeCoverage);
};

}  // namespace fuzzing

#endif  // SRC_SYS_FUZZING_REALMFUZZER_TESTING_COVERAGE_H_
