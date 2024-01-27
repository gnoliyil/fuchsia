// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/fuzzing/common/controller.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/sanitizer.h>
#include <zircon/status.h>

#include <iostream>

#include "src/sys/fuzzing/common/async-socket.h"
#include "src/sys/fuzzing/common/async-types.h"
#include "src/sys/fuzzing/common/corpus-reader-client.h"
#include "src/sys/fuzzing/common/options.h"

namespace fuzzing {

using ::fuchsia::fuzzer::DONE_MARKER;

ControllerImpl::ControllerImpl(RunnerPtr runner)
    : binding_(this), executor_(runner->executor()), runner_(runner) {}

void ControllerImpl::Bind(fidl::InterfaceRequest<Controller> request) {
  FX_DCHECK(runner_);
  binding_.Bind(std::move(request));
}

ZxPromise<> ControllerImpl::ResetArtifact() {
  return fpromise::make_promise([this]() -> ZxResult<> {
           artifact_ = Artifact();
           return fpromise::ok();
         })
      .wrap_with(scope_);
}

void ControllerImpl::Finish() {
  std::cout << std::endl << DONE_MARKER << std::endl;
  std::cerr << std::endl << DONE_MARKER << std::endl;
  FX_LOGS(INFO) << DONE_MARKER;
}

///////////////////////////////////////////////////////////////
// FIDL methods.

void ControllerImpl::Configure(Options options, ConfigureCallback callback) {
  auto task =
      fpromise::make_promise([this, overrides = std::move(options)]() mutable -> ZxResult<> {
        auto options = runner_->options();
        SetOptions(options.get(), overrides);
        return fpromise::ok();
      })
          .and_then(runner_->Configure())
          .then([callback = std::move(callback)](const ZxResult<>& result) {
            callback(result.is_ok() ? ZX_OK : result.error());
          })
          .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::GetOptions(GetOptionsCallback callback) {
  const auto& options = runner_->options();
  callback(CopyOptions(*options));
}

void ControllerImpl::AddToCorpus(CorpusType corpus_type, FidlInput fidl_input,
                                 AddToCorpusCallback callback) {
  auto task = AsyncSocketRead(executor_, std::move(fidl_input))
                  .and_then([this, corpus_type](Input& received) -> ZxResult<> {
                    return AsZxResult(runner_->AddToCorpus(corpus_type, std::move(received)));
                  })
                  .then([callback = std::move(callback)](const ZxResult<>& result) {
                    callback(result.is_ok() ? ZX_OK : result.error());
                  })
                  .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::ReadCorpus(CorpusType corpus_type, fidl::InterfaceHandle<CorpusReader> reader,
                                ReadCorpusCallback callback) {
  auto client = std::make_unique<CorpusReaderClient>(executor_);
  client->Bind(std::move(reader));

  // The client must be moved into the promise to prevent it going out of scope.
  auto task = fpromise::make_promise([runner = runner_, corpus_type, client = std::move(client),
                                      send = ZxFuture<>()](Context& context) mutable -> ZxResult<> {
                if (!send) {
                  send = client->Send(runner->GetCorpus(corpus_type));
                }
                if (!send(context)) {
                  return fpromise::pending();
                }
                return send.take_result();
              }).and_then([callback = std::move(callback)]() { callback(); });
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::WriteDictionary(FidlInput dictionary, WriteDictionaryCallback callback) {
  auto task = AsyncSocketRead(executor_, std::move(dictionary))
                  .and_then([this](Input& received) -> ZxResult<> {
                    return AsZxResult(runner_->ParseDictionary(std::move(received)));
                  })
                  .then([callback = std::move(callback)](const ZxResult<>& result) {
                    callback(result.is_ok() ? ZX_OK : result.error());
                  })
                  .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::ReadDictionary(ReadDictionaryCallback callback) {
  callback(AsyncSocketWrite(executor_, runner_->GetDictionaryAsInput()));
}

void ControllerImpl::GetStatus(GetStatusCallback callback) { callback(runner_->CollectStatus()); }

void ControllerImpl::AddMonitor(fidl::InterfaceHandle<Monitor> monitor,
                                AddMonitorCallback callback) {
  FX_DCHECK(runner_);
  runner_->AddMonitor(std::move(monitor));
  callback();
}

void ControllerImpl::Fuzz(FuzzCallback callback) {
  auto task = ResetArtifact()
                  .and_then(runner_->Fuzz())
                  .and_then([this](Artifact& artifact) {
                    artifact_ = artifact.Duplicate();
                    return fpromise::ok(AsyncSocketWrite(executor_, std::move(artifact)));
                  })
                  .then([this, callback = std::move(callback)](ZxResult<FidlArtifact>& result) {
                    callback(std::move(result));
                    Finish();
                  })
                  .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::TryOne(FidlInput fidl_input, TryOneCallback callback) {
  auto task = ResetArtifact()
                  .and_then(AsyncSocketRead(executor_, std::move(fidl_input)))
                  .and_then([this](Input& received) {
                    artifact_ = Artifact(FuzzResult::NO_ERRORS, received.Duplicate());
                    return runner_->TryOne(std::move(received));
                  })
                  .then([this, callback = std::move(callback)](ZxResult<FuzzResult>& result) {
                    if (result.is_ok()) {
                      auto input = artifact_.take_input();
                      artifact_ = Artifact(result.value(), std::move(input));
                    }
                    callback(std::move(result));
                    Finish();
                  })
                  .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::Minimize(FidlInput fidl_input, MinimizeCallback callback) {
  auto task =
      ResetArtifact()
          .and_then(AsyncSocketRead(executor_, std::move(fidl_input)))
          .and_then([this](Input& received) { return runner_->Minimize(std::move(received)); })
          .and_then([this](Input& input) {
            artifact_ = Artifact(FuzzResult::NO_ERRORS, input.Duplicate());
            return fpromise::ok(AsyncSocketWrite(executor_, std::move(input)));
          })
          .then([this, callback = std::move(callback)](ZxResult<FidlInput>& result) {
            callback(std::move(result));
            Finish();
          })
          .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::Cleanse(FidlInput fidl_input, CleanseCallback callback) {
  auto task =
      ResetArtifact()
          .and_then(AsyncSocketRead(executor_, std::move(fidl_input)))
          .and_then([this](Input& received) { return runner_->Cleanse(std::move(received)); })
          .and_then([this](Input& input) {
            artifact_ = Artifact(FuzzResult::NO_ERRORS, input.Duplicate());
            return fpromise::ok(AsyncSocketWrite(executor_, std::move(input)));
          })
          .then([this, callback = std::move(callback)](ZxResult<FidlInput>& result) {
            callback(std::move(result));
            Finish();
          })
          .wrap_with(scope_);
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::Merge(MergeCallback callback) {
  auto task = ResetArtifact()
                  .and_then(runner_->Merge())
                  .then([this, callback = std::move(callback)](ZxResult<>& result) {
                    callback(result.is_ok() ? ZX_OK : result.error());
                    Finish();
                  });
  executor_->schedule_task(std::move(task));
}

void ControllerImpl::GetResults(GetResultsCallback callback) {
  const auto& input = artifact_.input();
  callback(artifact_.fuzz_result(), AsyncSocketWrite(executor_, input.Duplicate()));
}

void ControllerImpl::Stop() {
  if (runner_) {
    executor_->schedule_task(runner_->Stop());
  }
}

}  // namespace fuzzing
