// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/tests/session_test.h"

#include <lib/syslog/cpp/macros.h>

#include "src/ui/scenic/lib/scheduling/constant_frame_predictor.h"
#include "src/ui/scenic/lib/scheduling/default_frame_scheduler.h"

namespace scenic_impl::gfx::test {

void SessionTest::SetUp() {
  ErrorReportingTest::SetUp();

  frame_scheduler_ = std::make_shared<scheduling::DefaultFrameScheduler>(
      std::make_unique<scheduling::ConstantFramePredictor>(/* static_vsync_offset */ zx::msec(5)));

  image_pipe_updater_ = std::make_shared<ImagePipeUpdater>(*frame_scheduler_);
  frame_scheduler_->Initialize(
      std::make_shared<scheduling::VsyncTiming>(),
      /*update_sessions*/
      [this](auto& sessions_to_update, auto trace_id, auto fences_from_previous_presents) {
        return image_pipe_updater_->UpdateSessions(sessions_to_update, trace_id);
      },
      /*on_cpu_work_done*/
      [this] { image_pipe_updater_->OnCpuWorkDone(); },
      /*on_frame_presented*/
      [this](auto latched_times, auto present_times) {
        image_pipe_updater_->OnFramePresented(latched_times, present_times);
      },
      /*render_scheduled_frame*/ [](auto...) {});

  session_context_ = CreateSessionContext();
  session_ = CreateSession();
}

void SessionTest::TearDown() {
  session_.reset();
  frame_scheduler_.reset();

  ErrorReportingTest::TearDown();
}

SessionContext SessionTest::CreateSessionContext() {
  FX_DCHECK(frame_scheduler_);

  SessionContext session_context{.vk_device = vk::Device(),
                                 .escher = nullptr,
                                 .escher_resource_recycler = nullptr,
                                 .escher_image_factory = nullptr,
                                 .scene_graph = nullptr,
                                 .view_linker = nullptr};
  return session_context;
}

CommandContext SessionTest::CreateCommandContext() {
  return CommandContext{
      .view_tree_updater = &view_tree_updater_,
      .image_pipe_updater = image_pipe_updater_,
  };
}

std::unique_ptr<Session> SessionTest::CreateSession() {
  static uint32_t next_id = 1;
  return std::make_unique<Session>(next_id++, session_context_, shared_event_reporter(),
                                   shared_error_reporter());
}

bool SessionTest::Apply(::fuchsia::ui::gfx::Command command) {
  auto command_context = CreateCommandContext();
  auto retval = session_->ApplyCommand(&command_context, std::move(command));
  return retval;
}

}  // namespace scenic_impl::gfx::test
