// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/default.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "lib/ui/scenic/cpp/commands.h"
#include "src/ui/scenic/lib/gfx/gfx_system.h"
#include "src/ui/scenic/lib/gfx/tests/gfx_test.h"
#include "src/ui/scenic/lib/gfx/tests/mocks/util.h"
#include "src/ui/scenic/lib/utils/helpers.h"

namespace scenic_impl {
namespace gfx {
namespace test {

TEST_F(GfxSystemTest, CreateAndDestroySession) {
  EXPECT_EQ(0U, scenic()->num_sessions());

  fuchsia::ui::scenic::SessionPtr session;

  EXPECT_EQ(0U, scenic()->num_sessions());

  scenic()->CreateSession(session.NewRequest(), nullptr);

  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
  session = nullptr;
  RunLoopUntilIdle();
  EXPECT_EQ(0U, scenic()->num_sessions());
}

TEST_F(GfxSystemTest, ScheduleUpdateInOrder) {
  // Create a session.
  fuchsia::ui::scenic::SessionPtr session;
  EXPECT_EQ(0U, scenic()->num_sessions());
  scenic()->CreateSession(session.NewRequest(), nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
  // Present on the session with presentation_time = 1.
  session->Present(1, CreateEventArray(1), CreateEventArray(1), [](auto) {});
  // Briefly pump the message loop. Expect that the session is not destroyed.
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
  // Present with the same presentation time.
  session->Present(1, CreateEventArray(1), CreateEventArray(1), [](auto) {});
  // Briefly pump the message loop. Expect that the session is not destroyed.
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
}

TEST_F(GfxSystemTest, SchedulePresent2UpdateInOrder) {
  // Create a session.
  fuchsia::ui::scenic::SessionPtr session;
  EXPECT_EQ(0U, scenic()->num_sessions());
  scenic()->CreateSession(session.NewRequest(), nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
  // Present on the session with presentation_time = 1.
  session->Present2(utils::CreatePresent2Args(1, CreateEventArray(1), CreateEventArray(1), 0),
                    [](auto) {});
  // Briefly flush the message loop. Expect that the session is not destroyed.
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
  // Present with the same presentation time.
  session->Present2(utils::CreatePresent2Args(1, CreateEventArray(1), CreateEventArray(1), 0),
                    [](auto) {});
  // Briefly flush the message loop. Expect that the session is not destroyed.
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
}

TEST_F(GfxSystemTest, SchedulePresent2UpdateWithMissingFields) {
  // Create a session.
  fuchsia::ui::scenic::SessionPtr session;
  EXPECT_EQ(0U, scenic()->num_sessions());
  scenic()->CreateSession(session.NewRequest(), nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());
  // Present on the session with presentation_time = 1.
  session->Present2({}, [](auto) {});
  // Briefly flush the message loop. Expect that the session is destroyed.
  RunLoopUntilIdle();
  EXPECT_EQ(0U, scenic()->num_sessions());
}

TEST_F(GfxSystemTest, RequestPresentationTimes) {
  fuchsia::ui::scenic::SessionPtr session;
  EXPECT_EQ(0U, scenic()->num_sessions());
  scenic()->CreateSession(session.NewRequest(), nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());

  // Call RequestPresentationTimes() and expect the maximum amount of presents in flight since we
  // never called Present2().
  session->RequestPresentationTimes(
      0, [](fuchsia::scenic::scheduling::FuturePresentationTimes future_times) {
        EXPECT_EQ(future_times.remaining_presents_in_flight_allowed,
                  scheduling::FrameScheduler::kMaxPresentsInFlight);
      });

  EXPECT_TRUE(RunLoopUntilIdle());
}

TEST_F(GfxSystemTest, TooManyPresent2sInFlight_ShouldKillSession) {
  fuchsia::ui::scenic::SessionPtr session;
  EXPECT_EQ(0U, scenic()->num_sessions());
  scenic()->CreateSession(session.NewRequest(), nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());

  // Max out our budget of Present2s.
  for (int i = 0; i < 5; i++) {
    session->Present2(utils::CreatePresent2Args(0, {}, {}, 0), [](auto) {});
  }
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_EQ(1U, scenic()->num_sessions());

  // Execute one more Present2, which should kill the session.
  session->Present2(utils::CreatePresent2Args(0, {}, {}, 0), [](auto) {});
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_EQ(0U, scenic()->num_sessions());
}

// Ensure Present2's immediate callback is functionally equivalent to RequestPresentationTimes'
// callback.
TEST_F(GfxSystemTest, RequestPresentationTimesResponse_ShouldMatchPresent2CallbackResponse) {
  fuchsia::ui::scenic::SessionPtr session;
  EXPECT_EQ(0U, scenic()->num_sessions());
  scenic()->CreateSession(session.NewRequest(), nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(1U, scenic()->num_sessions());

  fuchsia::scenic::scheduling::FuturePresentationTimes present2_response = {};
  fuchsia::scenic::scheduling::FuturePresentationTimes rpt_response = {};

  session->Present2(
      utils::CreatePresent2Args(0, {}, {}, 0),
      [&present2_response](fuchsia::scenic::scheduling::FuturePresentationTimes future_times) {
        present2_response = std::move(future_times);
      });
  EXPECT_TRUE(RunLoopUntilIdle());

  session->RequestPresentationTimes(
      0, [&rpt_response](fuchsia::scenic::scheduling::FuturePresentationTimes future_times) {
        rpt_response = std::move(future_times);
      });
  EXPECT_TRUE(RunLoopUntilIdle());

  EXPECT_EQ(rpt_response.remaining_presents_in_flight_allowed,
            present2_response.remaining_presents_in_flight_allowed);
  EXPECT_EQ(rpt_response.future_presentations.size(),
            present2_response.future_presentations.size());

  for (size_t i = 0; i < rpt_response.future_presentations.size(); ++i) {
    auto rpt_elem = std::move(rpt_response.future_presentations[i]);
    auto present2_elem = std::move(present2_response.future_presentations[i]);

    EXPECT_EQ(rpt_elem.latch_point(), present2_elem.latch_point());
    EXPECT_EQ(rpt_elem.presentation_time(), present2_elem.presentation_time());
  }
}

// Check that the gfx::ViewTree is kept in sync on Session destruction.
TEST_F(GfxSystemTest, CreateAndDestroySessionWithView) {
  fuchsia::ui::scenic::SessionPtr session;
  scenic()->CreateSession(session.NewRequest(), nullptr);

  auto [control_ref, view_ref] = scenic::ViewRefPair::New();
  const zx_koid_t view_ref_koid = utils::ExtractKoid(view_ref);
  {  // Create a minimal scene.
    constexpr uint32_t kScene = 1, kViewHolder = 2, kView = 3;
    auto [v, vh] = scenic::ViewTokenPair::New();
    std::vector<fuchsia::ui::scenic::Command> commands;
    commands.push_back(scenic::NewCommand(scenic::NewCreateSceneCmd(kScene)));
    commands.push_back(scenic::NewCommand(
        scenic::NewCreateViewHolderCmd(kViewHolder, std::move(vh), "view_holder")));
    commands.push_back(scenic::NewCommand(scenic::NewAddChildCmd(kScene, kViewHolder)));
    commands.push_back(scenic::NewCommand(scenic::NewCreateViewCmd(
        kView, std::move(v), std::move(control_ref), std::move(view_ref), "view")));
    session->Enqueue(std::move(commands));

    session->Present2(utils::CreatePresent2Args(0, {}, {}, 0), [](auto) {});
    session.events().OnFramePresented = [this](auto) { QuitLoop(); };
    RunLoopFor(zx::sec(1));
  }

  // View is now part of the ViewTree.
  EXPECT_EQ(1U, scenic()->num_sessions());
  const auto& view_tree = engine()->scene_graph()->view_tree();
  EXPECT_TRUE(view_tree.IsTracked(view_ref_koid));

  {  // Send an invalid command which should cause session destruction.
    constexpr uint32_t kUnknownResourceId = 12456;
    std::vector<fuchsia::ui::scenic::Command> commands;
    commands.push_back(scenic::NewCommand(scenic::NewReleaseResourceCmd(kUnknownResourceId)));
    session->Enqueue(std::move(commands));

    session->Present2(utils::CreatePresent2Args(0, {}, {}, 0), [](auto) {});
    bool error = false;
    session.set_error_handler([this, &error](auto) {
      error = true;
      QuitLoop();
    });
    RunLoopFor(zx::sec(1));
    EXPECT_TRUE(error);
  }

  // View should no longer be part of the ViewTree.
  EXPECT_EQ(0U, scenic()->num_sessions());
  EXPECT_FALSE(view_tree.IsTracked(view_ref_koid));
}

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl
