// Copyright 2021 The Fuchsia Authors.All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/feedback_data.h"

#include <lib/async/cpp/task.h>
#include <lib/fdio/spawn.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>
#include <zircon/processargs.h>
#include <zircon/types.h>

#include <memory>

#include "src/developer/forensics/feedback/constants.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/feedback_data/data_provider_controller.h"
#include "src/lib/files/path.h"

namespace forensics::feedback {

FeedbackData::FeedbackData(async_dispatcher_t* dispatcher,
                           std::shared_ptr<sys::ServiceDirectory> services,
                           timekeeper::Clock* clock, inspect::Node* inspect_root,
                           cobalt::Logger* cobalt, RedactorBase* redactor,
                           feedback::AnnotationManager* annotation_manager, Options options)
    : dispatcher_(dispatcher),
      services_(services),
      clock_(clock),
      cobalt_(cobalt),
      inspect_node_manager_(inspect_root),
      inspect_data_budget_(options.limit_inspect_data, &inspect_node_manager_, cobalt_),
      attachment_providers_(dispatcher, services, options.delete_previous_boot_logs_time, clock,
                            redactor, &inspect_data_budget_, options.config.attachment_allowlist),
      data_provider_(dispatcher_, services_, clock_, redactor, options.is_first_instance,
                     options.config.annotation_allowlist, options.config.attachment_allowlist,
                     cobalt_, annotation_manager, attachment_providers_.GetAttachmentManager(),
                     &inspect_data_budget_),
      data_provider_controller_() {
  if (options.spawn_system_log_recorder) {
    SpawnSystemLogRecorder();
  }
}

feedback_data::DataProvider* FeedbackData::DataProvider() { return &data_provider_; }

feedback_data::DataProviderController* FeedbackData::DataProviderController() {
  return &data_provider_controller_;
}

void FeedbackData::ShutdownImminent(::fit::deferred_callback stop_respond) {
  system_log_recorder_lifecycle_.set_error_handler(
      [stop_respond = std::move(stop_respond)](const zx_status_t status) mutable {
        if (status != ZX_OK) {
          FX_PLOGS(WARNING, status) << "Lost connection to system log recorder";
        }

        // |stop_respond| must explicitly be called otherwise it won't run until the error
        // handler is destroyed (which doesn't happen).
        stop_respond.call();
      });
  system_log_recorder_lifecycle_->Stop();
}

void FeedbackData::SpawnSystemLogRecorder() {
  zx::channel controller_client, controller_server;
  if (const auto status = zx::channel::create(0, &controller_client, &controller_server);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status)
        << "Failed to create system log recorder controller channel, logs will not be persisted";
    return;
  }

  zx::channel lifecycle_client, lifecycle_server;
  if (const auto status = zx::channel::create(0, &lifecycle_client, &lifecycle_server);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status)
        << "Failed to create system log recorder lifecycle channel, logs will not be persisted";
    return;
  }

  const std::array<const char*, 2> argv = {
      "system_log_recorder" /* process name */,
      nullptr,
  };
  const std::array actions = {
      fdio_spawn_action_t{
          .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
          .h =
              {
                  .id = PA_HND(PA_USER0, 0),
                  .handle = controller_server.release(),
              },
      },
      fdio_spawn_action_t{
          .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
          .h =
              {
                  .id = PA_HND(PA_USER1, 0),
                  .handle = lifecycle_server.release(),
              },
      },
  };

  zx_handle_t process;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH] = {};
  if (const zx_status_t status = fdio_spawn_etc(
          ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, "/pkg/bin/system_log_recorder", argv.data(),
          /*environ=*/nullptr, actions.size(), actions.data(), &process, err_msg);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to spawn system log recorder, logs will not be persisted: "
                            << err_msg;
    return;
  }

  data_provider_controller_.BindSystemLogRecorderController(std::move(controller_client),
                                                            dispatcher_);
  system_log_recorder_lifecycle_.Bind(std::move(lifecycle_client), dispatcher_);
}

}  // namespace forensics::feedback
