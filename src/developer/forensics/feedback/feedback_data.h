// Copyright 2021 The Fuchsia Authors.All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_FEEDBACK_DATA_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_FEEDBACK_DATA_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/process/lifecycle/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/defer.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include <memory>

#include "src/developer/forensics/feedback/annotations/annotation_manager.h"
#include "src/developer/forensics/feedback/attachment_providers.h"
#include "src/developer/forensics/feedback/config.h"
#include "src/developer/forensics/feedback/constants.h"
#include "src/developer/forensics/feedback_data/data_provider.h"
#include "src/developer/forensics/feedback_data/data_provider_controller.h"
#include "src/developer/forensics/feedback_data/inspect_data_budget.h"
#include "src/developer/forensics/utils/cobalt/logger.h"
#include "src/developer/forensics/utils/inspect_node_manager.h"
#include "src/lib/timekeeper/clock.h"

namespace forensics::feedback {

class FeedbackData {
 public:
  struct Options {
    SnapshotConfig config;
    bool is_first_instance;
    bool limit_inspect_data;
    bool spawn_system_log_recorder;
    std::optional<zx::duration> delete_previous_boot_logs_time;
  };

  FeedbackData(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
               timekeeper::Clock* clock, inspect::Node* inspect_root, cobalt::Logger* cobalt,
               RedactorBase* redactor, feedback::AnnotationManager* annotation_manager,
               Options options);

  feedback_data::DataProvider* DataProvider();
  feedback_data::DataProviderController* DataProviderController();

  void ShutdownImminent(::fit::deferred_callback stop_respond);

 private:
  void SpawnSystemLogRecorder();

  async_dispatcher_t* dispatcher_;
  std::shared_ptr<sys::ServiceDirectory> services_;
  timekeeper::Clock* clock_;
  cobalt::Logger* cobalt_;

  InspectNodeManager inspect_node_manager_;
  feedback_data::InspectDataBudget inspect_data_budget_;
  AttachmentProviders attachment_providers_;
  feedback_data::DataProvider data_provider_;
  feedback_data::DataProviderController data_provider_controller_;

  fuchsia::process::lifecycle::LifecyclePtr system_log_recorder_lifecycle_;
};

}  // namespace forensics::feedback

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_FEEDBACK_DATA_H_
