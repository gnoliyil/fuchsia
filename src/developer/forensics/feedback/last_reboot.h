// Copyright 2021 The Fuchsia Authors.All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_LAST_REBOOT_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_LAST_REBOOT_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include <string>

#include "src/developer/forensics/feedback/reboot_log/reboot_log.h"
#include "src/developer/forensics/last_reboot/last_reboot_info_provider.h"
#include "src/developer/forensics/last_reboot/reporter.h"
#include "src/developer/forensics/utils/cobalt/logger.h"
#include "src/developer/forensics/utils/redact/redactor.h"

namespace forensics::feedback {

// Implements functionality last_reboot.cm previously implemented.
class LastReboot {
 public:
  struct Options {
    bool is_first_instance;
    RebootLog reboot_log;
    zx::duration oom_crash_reporting_delay;
  };

  LastReboot(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
             cobalt::Logger* cobalt, RedactorBase* redactor,
             fuchsia::feedback::CrashReporter* crash_reporter, Options options);

  fuchsia::feedback::LastRebootInfoProvider* LastRebootInfoProvider();

 private:
  last_reboot::Reporter reporter_;
  last_reboot::LastRebootInfoProvider last_reboot_info_provider_;
};

}  // namespace forensics::feedback

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_LAST_REBOOT_H_
