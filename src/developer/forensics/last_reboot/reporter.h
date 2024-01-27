// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_LAST_REBOOT_REPORTER_H_
#define SRC_DEVELOPER_FORENSICS_LAST_REBOOT_REPORTER_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/fpromise/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include <memory>

#include "src/developer/forensics/feedback/reboot_log/reboot_log.h"
#include "src/developer/forensics/utils/cobalt/logger.h"
#include "src/developer/forensics/utils/redact/redactor.h"
#include "src/lib/fxl/functional/cancelable_callback.h"

namespace forensics {
namespace last_reboot {

// Logs the reboot reason with Cobalt and if the reboot was non-graceful, files a crash report.
class Reporter {
 public:
  // fuchsia.feedback.CrashReporter is expected to be in |services|.
  Reporter(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
           cobalt::Logger* cobalt, RedactorBase* redactor,
           fuchsia::feedback::CrashReporter* crash_reporter);

  void ReportOn(const feedback::RebootLog& reboot_log, zx::duration crash_reporting_delay);

 private:
  ::fpromise::promise<void> FileCrashReport(const feedback::RebootLog& reboot_log,
                                            zx::duration delay);

  async_dispatcher_t* dispatcher_;
  async::Executor executor_;

  cobalt::Logger* cobalt_;
  RedactorBase* redactor_;
  fuchsia::feedback::CrashReporter* crash_reporter_;

  // We wrap the delayed task we post on the async loop to delay the crash reporting in a
  // CancelableClosure so we can cancel it if we are done another way.
  fxl::CancelableClosure delayed_crash_reporting_;
};

}  // namespace last_reboot
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_LAST_REBOOT_REPORTER_H_
