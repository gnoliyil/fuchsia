// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/last_reboot.h"

namespace forensics::feedback {

LastReboot::LastReboot(async_dispatcher_t* dispatcher,
                       std::shared_ptr<sys::ServiceDirectory> services, cobalt::Logger* cobalt,
                       RedactorBase* redactor, fuchsia::feedback::CrashReporter* crash_reporter,
                       const Options options)
    : reboot_watcher_(services, options.graceful_reboot_reason_write_path, cobalt),
      reporter_(dispatcher, services, cobalt, redactor, crash_reporter),
      last_reboot_info_provider_(options.reboot_log) {
  reboot_watcher_.Connect();
  if (options.is_first_instance) {
    const zx::duration delay = (options.reboot_log.RebootReason() == RebootReason::kOOM)
                                   ? options.oom_crash_reporting_delay
                                   : zx::sec(0);
    reporter_.ReportOn(options.reboot_log, delay);
  }
}

fuchsia::feedback::LastRebootInfoProvider* LastReboot::LastRebootInfoProvider() {
  return &last_reboot_info_provider_;
}

}  // namespace forensics::feedback
