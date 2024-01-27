// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/forensics/exceptions/handler/crash_reporter.h"

#include <lib/async/cpp/task.h>
#include <lib/fidl/cpp/string.h>
#include <lib/fpromise/bridge.h>
#include <lib/fpromise/promise.h>
#include <lib/fpromise/result.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/exception.h>
#include <zircon/syscalls/object.h>

#include <optional>

#include "src/developer/forensics/exceptions/constants.h"
#include "src/developer/forensics/exceptions/handler/component_lookup.h"
#include "src/developer/forensics/exceptions/handler/minidump.h"
#include "src/developer/forensics/exceptions/handler/report_builder.h"
#include "src/developer/forensics/utils/fidl_oneshot.h"
#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/fxl/strings/join_strings.h"

namespace forensics {
namespace exceptions {
namespace handler {
namespace {

using fuchsia::feedback::CrashReport;

// Either resets the exception immediately if the process only has one thread or with a 5s delay
// otherwise.
void ResetException(async_dispatcher_t* dispatcher, zx::exception exception,
                    const zx::process& process) {
  if (!exception.is_valid()) {
    return;
  }

  if (!process.is_valid()) {
    FX_LOGS(ERROR) << "Process for exception is invalid";
    exception.reset();
    return;
  }

  size_t num_threads{0};
  if (const zx_status_t status = zx_object_get_info(process.get(), ZX_INFO_PROCESS_THREADS, nullptr,
                                                    0u, nullptr, &num_threads);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to get thread info from process " << process.get();
    exception.reset();
    return;
  }

  if (num_threads > 1) {
    // If the process has multiple threads, delay resetting |exception| for 5 seconds. If one of the
    // other threads is in an exception, releasing |exception| immediately may result in the process
    // being terminated by the kernel before the minidump for the other thread is generated.
    async::PostDelayedTask(
        dispatcher, [exception = std::move(exception)]() mutable { exception.reset(); },
        zx::sec(5));
  } else {
    exception.reset();
  }
}

// Returns true if the process is terminated.
bool IsProcessTerminated(zx::process& crashed_process) {
  zx_info_process_t process_info;
  if (const zx_status_t status = crashed_process.get_info(ZX_INFO_PROCESS, &process_info,
                                                          sizeof(process_info), nullptr, nullptr);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to get info for process " << crashed_process.get();
    return false;
  }

  return process_info.flags & ZX_INFO_PROCESS_FLAG_EXITED;
}

fpromise::promise<> Delay(async_dispatcher_t* dispatcher, const zx::duration duration) {
  fpromise::bridge<> bridge;

  if (const zx_status_t status = async::PostDelayedTask(
          dispatcher,
          [completer = std::move(bridge.completer)]() mutable { completer.complete_ok(); },
          zx::sec(5));
      status != ZX_OK) {
    // When |bridge.completer| gets destroyed, the completer is considered abandoned and the
    // consumer will receive an error result.
    FX_PLOGS(ERROR, status) << "Failed to delay connecting to the crash reporter, connecting now";
  }

  return bridge.consumer.promise_or(fpromise::error());
}

}  // namespace

CrashReporter::CrashReporter(async_dispatcher_t* dispatcher,
                             std::shared_ptr<sys::ServiceDirectory> services,
                             zx::duration component_lookup_timeout)
    : dispatcher_(dispatcher),
      executor_(dispatcher_),
      services_(services),
      component_lookup_timeout_(component_lookup_timeout) {}

void CrashReporter::Send(zx::exception exception, zx::process crashed_process,
                         zx::thread crashed_thread, SendCallback callback) {
  CrashReportBuilder builder;
  builder.SetProcess(crashed_process).SetThread(crashed_thread);

  if (exception.is_valid()) {
    std::optional<ExceptionReason> exception_reason{std::nullopt};
    std::optional<std::string> gwp_asan_exception_type;
    zx::vmo minidump = GenerateMinidump(exception, &exception_reason, &gwp_asan_exception_type);

    if (minidump.is_valid()) {
      builder.SetMinidump(std::move(minidump));
    } else {
      builder.SetProcessTerminated();
    }
    builder.SetExceptionReason(exception_reason);
    if (gwp_asan_exception_type.has_value()) {
      builder.SetGwpAsanExceptionType(gwp_asan_exception_type.value());
    }
  } else {
    builder.SetExceptionExpired();
  }

  if (IsProcessTerminated(crashed_process)) {
    builder.SetProcessTerminated();
  }
  ResetException(dispatcher_, std::move(exception), crashed_process);

  const auto thread_koid = fsl::GetKoid(crashed_thread.get());
  auto file_crash_report =
      GetComponentInfo(dispatcher_, services_, component_lookup_timeout_, thread_koid)
          .then([dispatcher = dispatcher_, services = services_, builder = std::move(builder),
                 callback =
                     std::move(callback)](::fpromise::result<ComponentInfo>& result) mutable {
            ComponentInfo component_info;
            if (result.is_ok()) {
              component_info = result.take_value();
            }

            builder.SetComponentInfo(component_info);

            fpromise::promise<> delay = (builder.ProcessName() != "feedback.cm")
                                            ? fpromise::make_ok_promise()
                                            : Delay(dispatcher, /*duration=*/zx::sec(5));

            return delay
                .then([dispatcher, services,
                       builder = std::move(builder)](const fpromise::result<>& result) mutable {
                  return OneShotCall<fuchsia::feedback::CrashReporter,
                                     &fuchsia::feedback::CrashReporter::FileReport>(
                      dispatcher, services, kFileReportTimeout, builder.Consume());
                })
                .then([component_info = std::move(component_info), callback = std::move(callback)](
                          const fpromise::result<fuchsia::feedback::CrashReporter_FileReport_Result,
                                                 forensics::Error>& result) {
                  ::fidl::StringPtr moniker = std::nullopt;
                  if (!component_info.moniker.empty()) {
                    moniker = component_info.moniker;
                  }
                  callback(std::move(moniker));

                  return ::fpromise::ok();
                });
          });

  executor_.schedule_task(std::move(file_crash_report));
}

}  // namespace handler
}  // namespace exceptions
}  // namespace forensics
