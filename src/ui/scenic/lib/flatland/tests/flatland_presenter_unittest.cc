// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-testing/test_loop.h>
#include <lib/async/default.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/thread_checker.h>
#include <lib/syslog/cpp/macros.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/scenic/lib/flatland/flatland_presenter_impl.h"
#include "src/ui/scenic/lib/scheduling/tests/mocks/frame_scheduler_mocks.h"
#include "src/ui/scenic/lib/utils/helpers.h"

using flatland::FlatlandPresenterImpl;

namespace flatland::test {

namespace {

// This harness uses a real loop instead of a test loop since the multithreading test requires the
// tasks posted by the FlatlandPresenterImpl to run without blocking the worker threads.
class FlatlandPresenterTest : public gtest::RealLoopFixture {
 public:
  std::shared_ptr<FlatlandPresenterImpl> CreateFlatlandPresenterImpl(
      scheduling::FrameScheduler& scheduler) {
    return std::make_shared<FlatlandPresenterImpl>(dispatcher(), scheduler);
  }
};

}  // namespace

TEST_F(FlatlandPresenterTest, RegisterPresentForwardsToFrameScheduler) {
  scheduling::test::MockFrameScheduler frame_scheduler;

  // Capture the relevant arguments of the RegisterPresent() call.
  scheduling::SessionId last_session_id = scheduling::kInvalidSessionId;
  scheduling::PresentId last_present_id = scheduling::kInvalidPresentId;

  frame_scheduler.set_register_present_callback(
      [&last_session_id, &last_present_id](scheduling::SessionId session_id,
                                           std::vector<zx::event> release_fences,
                                           scheduling::PresentId present_id) {
        last_session_id = session_id;
        last_present_id = present_id;
      });

  auto presenter = CreateFlatlandPresenterImpl(frame_scheduler);

  const scheduling::SessionId kSessionId = 2;
  const scheduling::PresentId present_id = scheduling::GetNextPresentId();
  presenter->ScheduleUpdateForSession(zx::time(0), {kSessionId, present_id}, /*unsquashable=*/false,
                                      /*release_fences=*/{});
  RunLoopUntilIdle();

  EXPECT_EQ(last_session_id, kSessionId);
  EXPECT_EQ(last_present_id, present_id);
}

TEST_F(FlatlandPresenterTest, ScheduleUpdateForSessionForwardsToFrameScheduler) {
  scheduling::test::MockFrameScheduler frame_scheduler;

  // Capture the relevant arguments of the ScheduleUpdateForSession() call.
  zx::time last_presentation_time = zx::time(0);
  auto last_id_pair = scheduling::SchedulingIdPair({
      .session_id = scheduling::kInvalidSessionId,
      .present_id = scheduling::kInvalidPresentId,
  });
  bool last_squashable = false;

  frame_scheduler.set_schedule_update_for_session_callback(
      [&last_presentation_time, &last_id_pair, &last_squashable](
          zx::time presentation_time, scheduling::SchedulingIdPair id_pair, bool squashable) {
        last_presentation_time = presentation_time;
        last_id_pair = id_pair;
        last_squashable = squashable;
      });

  auto presenter = CreateFlatlandPresenterImpl(frame_scheduler);

  const auto kIdPair = scheduling::SchedulingIdPair({
      .session_id = 1,
      .present_id = 2,
  });
  const zx::time kPresentationTime = zx::time(123);
  const bool kUnsquashable = false;

  presenter->ScheduleUpdateForSession(kPresentationTime, kIdPair, kUnsquashable,
                                      /*release_fences=*/{});
  RunLoopUntilIdle();

  EXPECT_EQ(last_presentation_time, kPresentationTime);
  EXPECT_EQ(last_id_pair, kIdPair);
  EXPECT_EQ(last_squashable, !kUnsquashable);
}

TEST_F(FlatlandPresenterTest, RemoveSessionForwardsToFrameScheduler) {
  scheduling::test::MockFrameScheduler frame_scheduler;

  // FlatlandPresenter should first remove the session_id and *then* schedule a new frame for it.
  // Capture both ordering and arguments to confirm.
  std::vector<std::pair<std::string, scheduling::SessionId>> results;
  frame_scheduler.set_remove_session_callback([&results](scheduling::SessionId session_id) {
    results.emplace_back("RemoveSession", session_id);
  });
  frame_scheduler.set_schedule_update_for_session_callback(
      [&results](auto time, auto id_pair, auto squashable) {
        results.emplace_back("Schedule", id_pair.session_id);
      });

  auto presenter = CreateFlatlandPresenterImpl(frame_scheduler);

  const scheduling::SessionId kSessionId = 1;
  presenter->RemoveSession(kSessionId);

  RunLoopUntilIdle();

  // Since this function runs on the main thread, no RunLoopUntilIdle() is necessary.
  EXPECT_THAT(results, testing::ElementsAre(std::make_pair("RemoveSession", kSessionId),
                                            std::make_pair("Schedule", kSessionId)));
}

TEST_F(FlatlandPresenterTest, GetFuturePresentationInfosForwardsToFrameScheduler) {
  scheduling::test::MockFrameScheduler frame_scheduler;

  // Capture the relevant arguments of the GetFuturePresentationInfos() call.
  zx::duration last_requested_prediction_span;
  const zx::time kLatchPoint = zx::time(15122);
  const zx::time kPresentationTime = zx::time(15410);
  frame_scheduler.set_get_future_presentation_infos_callback(
      [&last_requested_prediction_span, kLatchPoint,
       kPresentationTime](zx::duration requested_prediction_span) {
        last_requested_prediction_span = requested_prediction_span;
        std::vector<scheduling::FuturePresentationInfo> presentation_infos(1);
        presentation_infos[0].latch_point = kLatchPoint;
        presentation_infos[0].presentation_time = kPresentationTime;
        return presentation_infos;
      });

  auto presenter = CreateFlatlandPresenterImpl(frame_scheduler);

  std::vector<scheduling::FuturePresentationInfo> presentation_infos =
      presenter->GetFuturePresentationInfos();
  RunLoopUntilIdle();

  // The requested prediction span should be reasonable - greater than 1 frame's worth of data.
  EXPECT_GT(last_requested_prediction_span, zx::msec(17));
  EXPECT_EQ(presentation_infos.size(), 1u);
  EXPECT_EQ(presentation_infos[0].latch_point, kLatchPoint);
  EXPECT_EQ(presentation_infos[0].presentation_time, kPresentationTime);
}

// Helper function for TakeReleaseFences test below.  Encapsulates two calls which always happen
// together in the test: UpdateSessions() and TakeReleaseFences().
static std::vector<zx::event> TakeReleaseFences(
    const std::shared_ptr<FlatlandPresenterImpl>& presenter,
    const std::unordered_map<scheduling::SessionId, scheduling::PresentId>& sessions_to_update) {
  presenter->AccumulateReleaseFences(sessions_to_update);
  return presenter->TakeReleaseFences();
}

TEST_F(FlatlandPresenterTest, TakeReleaseFences) {
  // The frame scheduler isn't actually used for this test, although it *is* required for the
  // presenter to properly stash the release fences (not inherently, just an implementation detail).
  scheduling::test::MockFrameScheduler frame_scheduler;
  auto presenter = CreateFlatlandPresenterImpl(frame_scheduler);

  const scheduling::SessionId kSessionIdA = 3;
  const scheduling::SessionId kSessionIdB = 7;

  // Create release fences
  std::vector<zx::event> release_fences_A1 = utils::CreateEventArray(2);
  std::vector<zx_koid_t> release_fence_koids_A1 = utils::ExtractKoids(release_fences_A1);
  std::vector<zx::event> release_fences_A2 = utils::CreateEventArray(2);
  std::vector<zx_koid_t> release_fence_koids_A2 = utils::ExtractKoids(release_fences_A2);
  std::vector<zx::event> release_fences_B1 = utils::CreateEventArray(2);
  std::vector<zx_koid_t> release_fence_koids_B1 = utils::ExtractKoids(release_fences_B1);
  std::vector<zx::event> release_fences_B2 = utils::CreateEventArray(2);
  std::vector<zx_koid_t> release_fence_koids_B2 = utils::ExtractKoids(release_fences_B2);
  std::vector<zx::event> release_fences_B3 = utils::CreateEventArray(2);
  std::vector<zx_koid_t> release_fence_koids_B3 = utils::ExtractKoids(release_fences_B3);

  const auto present_id_A1 = scheduling::GetNextPresentId();
  presenter->ScheduleUpdateForSession(zx::time(0), {kSessionIdA, present_id_A1},
                                      /*unsquashable=*/true, std::move(release_fences_A1));
  const auto present_id_A2 = scheduling::GetNextPresentId();
  presenter->ScheduleUpdateForSession(zx::time(0), {kSessionIdA, present_id_A2},
                                      /*unsquashable=*/true, std::move(release_fences_A2));
  const auto present_id_B1 = scheduling::GetNextPresentId();
  presenter->ScheduleUpdateForSession(zx::time(0), {kSessionIdB, present_id_B1},
                                      /*unsquashable=*/true, std::move(release_fences_B1));
  const auto present_id_B2 = scheduling::GetNextPresentId();
  presenter->ScheduleUpdateForSession(zx::time(0), {kSessionIdB, present_id_B2},
                                      /*unsquashable=*/true, std::move(release_fences_B2));

  // There will be no fences yet, because ScheduleUpdateForSession() stashes the fences in a task
  // dispatched to the main thread, which hasn't run yet.
  auto fences_empty = TakeReleaseFences(presenter, {
                                                       {kSessionIdA, present_id_A2},
                                                       {kSessionIdB, present_id_B1},
                                                   });
  EXPECT_TRUE(fences_empty.empty());

  // Try to take the same fences.  We should see the fences for A1/A2/B1, but not B2.  Note that we
  // don't explicitly mention A1, but we get the fences for it too, because A2 has a higher present
  // ID for the same session ID.
  RunLoopUntilIdle();
  auto fences_A1A2B1 = TakeReleaseFences(presenter, {
                                                        {kSessionIdA, present_id_A2},
                                                        {kSessionIdB, present_id_B1},
                                                    });
  EXPECT_EQ(fences_A1A2B1.size(), release_fence_koids_A1.size() + release_fence_koids_A2.size() +
                                      release_fence_koids_B1.size());
  auto fences_A1A2B1_koids = utils::ExtractKoids(fences_A1A2B1);
  for (auto koid : release_fence_koids_A1) {
    EXPECT_THAT(fences_A1A2B1_koids, testing::Contains(koid));
  }
  for (auto koid : release_fence_koids_A2) {
    EXPECT_THAT(fences_A1A2B1_koids, testing::Contains(koid));
  }
  for (auto koid : release_fence_koids_B1) {
    EXPECT_THAT(fences_A1A2B1_koids, testing::Contains(koid));
  }

  // Register one more present.
  const auto present_id_B3 = scheduling::GetNextPresentId();
  presenter->ScheduleUpdateForSession(zx::time(0), {kSessionIdB, present_id_B3},
                                      /*unsquashable=*/true, std::move(release_fences_B3));
  RunLoopUntilIdle();
  auto fences_B2B3 = TakeReleaseFences(presenter, {
                                                      {kSessionIdB, present_id_B3},
                                                  });
  EXPECT_EQ(fences_B2B3.size(), release_fence_koids_B2.size() + release_fence_koids_B3.size());
  auto fences_B2B3_koids = utils::ExtractKoids(fences_B2B3);
  for (auto koid : release_fence_koids_B2) {
    EXPECT_THAT(fences_B2B3_koids, testing::Contains(koid));
  }
  for (auto koid : release_fence_koids_B3) {
    EXPECT_THAT(fences_B2B3_koids, testing::Contains(koid));
  }
}

TEST_F(FlatlandPresenterTest, MultithreadedAccess) {
  scheduling::test::MockFrameScheduler frame_scheduler;

  // The FrameScheduler will be accessed in a thread-safe way, so the test instead collects the
  // registered presents and scheduled updates and ensures each function was called the correct
  // number of times with the correct set of ID pairs.
  std::set<scheduling::SchedulingIdPair> registered_presents;
  std::set<scheduling::SchedulingIdPair> scheduled_updates;

  // Also use a generic function call counter to test mutual exclusion between function calls.
  size_t function_count = 0;

  frame_scheduler.set_register_present_callback(
      [&registered_presents, &function_count](scheduling::SessionId session_id,
                                              std::vector<zx::event> release_fences,
                                              scheduling::PresentId present_id) {
        registered_presents.insert({session_id, present_id});
        ++function_count;
      });

  frame_scheduler.set_schedule_update_for_session_callback(
      [&scheduled_updates, &function_count](zx::time presentation_time,
                                            scheduling::SchedulingIdPair id_pair, bool squashable) {
        scheduled_updates.insert(id_pair);

        ++function_count;
      });

  frame_scheduler.set_get_future_presentation_infos_callback(
      [&function_count](zx::duration requested_prediction_span)
          -> std::vector<scheduling::FuturePresentationInfo> {
        ++function_count;
        std::vector<scheduling::FuturePresentationInfo> infos;
        return infos;
      });

  auto presenter = CreateFlatlandPresenterImpl(frame_scheduler);

  // Start 10 "sessions", each of which registers 100 presents and schedules 100 updates.
  static constexpr uint64_t kNumSessions = 10;
  static constexpr uint64_t kNumPresents = 100;

  std::vector<std::thread> threads;

  std::mutex mutex;
  std::unordered_set<scheduling::PresentId> present_ids;

  const auto now = std::chrono::steady_clock::now();
  const auto then = now + std::chrono::milliseconds(50);

  std::atomic<uint64_t> sessions_posted_all_tasks = 0;
  std::atomic<uint64_t> loop_quits = 0;

  for (uint64_t session_id = 1; session_id <= kNumSessions; ++session_id) {
    std::thread thread([then, session_id, &mutex, &present_ids, &presenter, &loop_quits,
                        &sessions_posted_all_tasks]() {
      // Because each of the threads do a fixed amount of work, they may trigger in succession
      // without overlap. In order to bombard the system with concurrent requests, stall thread
      // execution until a specific time.
      std::this_thread::sleep_until(then);
      std::vector<scheduling::PresentId> presents;
      uint64_t presentation_info_count = 0;

      // Create a thread checker so that we can verify that the GetFuturePresentationInfos()
      // response runs on the correct thread.
      fit::thread_checker checker;
      EXPECT_TRUE(checker.is_thread_valid());

      // Set loop and dispatcher which is needed for GetFuturePresentationInfos().
      async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

      for (uint64_t i = 0; i < kNumPresents; ++i) {
        // ScheduleUpdateForSession() is one of the two functions being tested.
        auto present_id = scheduling::GetNextPresentId();
        presenter->ScheduleUpdateForSession(zx::time(0), {session_id, present_id},
                                            /*unsquashable=*/true, /*release_fences=*/{});
        presents.push_back(present_id);

        // Yield with some randomness so the threads get jumbled up a bit.
        if (std::rand() % 4 == 0) {
          std::this_thread::yield();
        }

        presentation_info_count++;

        EXPECT_TRUE(checker.is_thread_valid());

        // When all kNumPresents tasks are posted back, this thread can safely return.
        if (presentation_info_count == kNumPresents) {
          loop_quits++;
          loop.Quit();
        }
      }

      // Acquire the test mutex and insert all IDs for later evaluation.
      {
        std::scoped_lock lock(mutex);
        for (const auto& present_id : presents) {
          present_ids.insert(present_id);
        }
      }

      sessions_posted_all_tasks++;
      // This thread should run until it receives all replies back from the frame scheduler.
      loop.Run();
    });

    threads.push_back(std::move(thread));
  }

  // Make calls directly to the FrameScheduler to mimic GFX, which runs on the "main" looper, which
  // in this test is just this thread.
  static constexpr scheduling::SessionId kGfxSessionId = kNumSessions + 1;
  static constexpr uint64_t kNumGfxPresents = 500;

  std::vector<scheduling::PresentId> gfx_presents;

  std::this_thread::sleep_until(then);

  for (uint64_t i = 0; i < kNumGfxPresents; ++i) {
    // RegisterPresent() is one of the three functions being tested.
    auto present_id = scheduling::GetNextPresentId();
    frame_scheduler.RegisterPresent(kGfxSessionId, /*release_fences=*/{}, present_id);
    gfx_presents.push_back(present_id);

    // ScheduleUpdateForSession() is the second function being tested.
    frame_scheduler.ScheduleUpdateForSession(zx::time(0), {kGfxSessionId, present_id},
                                             /*squashable=*/true);
  }

  {
    std::scoped_lock lock(mutex);
    for (const auto& present_id : gfx_presents) {
      present_ids.insert(present_id);
    }
  }

  // We need to be careful to account for the race where this line can be reached before all t
  // sessions have posted their GetFuturePresentationTimes() messages, leading the test to deadlock.
  //
  // First ensure all t threads have posted all their tasks on the main dispatcher.
  RunLoopUntil([&sessions_posted_all_tasks] { return sessions_posted_all_tasks == kNumSessions; });

  // Then, ensure the main dispatcher can reply to all the tasks, and posts its replies.
  RunLoopUntilIdle();

  // Finally, join all threads that can now process the replies.
  for (auto& t : threads) {
    t.join();
  }

  // Flush all the tasks posted by the presenter.
  RunLoopUntilIdle();

  // Verify that all the PresentIds are unique and that the sets from both mock functions have the
  // same number of ID pairs.
  static constexpr uint64_t kTotalNumPresents = (kNumSessions * kNumPresents) + kNumGfxPresents;

  EXPECT_EQ(present_ids.size(), kTotalNumPresents);
  EXPECT_EQ(registered_presents.size(), kTotalNumPresents);
  EXPECT_EQ(scheduled_updates.size(), kTotalNumPresents);

  // Verify that the correct total number of function calls were made.
  EXPECT_EQ(function_count, kTotalNumPresents * 2ul);

  // Verify that the sets from both mock functions are identical.
  EXPECT_THAT(registered_presents, ::testing::ElementsAreArray(scheduled_updates));

  // Verify that every session received the total number of presentation_infos.
  EXPECT_EQ(loop_quits, kNumSessions);
}

}  // namespace flatland::test
