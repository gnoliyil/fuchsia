// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_SCHEDULING_DEFAULT_FRAME_SCHEDULER_H_
#define SRC_UI_SCENIC_LIB_SCHEDULING_DEFAULT_FRAME_SCHEDULER_H_

#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/trace/event.h>
#include <lib/zx/time.h>

#include <queue>

#include "lib/inspect/cpp/inspect.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/ui/scenic/lib/scheduling/frame_predictor.h"
#include "src/ui/scenic/lib/scheduling/frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/frame_stats.h"
#include "src/ui/scenic/lib/scheduling/vsync_timing.h"

namespace scheduling {

using UpdateSessions = fit::function<SessionUpdater::UpdateResults(
    const std::unordered_map<SessionId, PresentId>& sessions_to_update, uint64_t trace_id,
    std::vector<zx::event> fences_from_previous_presents)>;
using OnCpuWorkDone = fit::function<void()>;
using OnFramePresented = fit::function<void(
    const std::unordered_map<SessionId, std::map<PresentId, /*latched_time*/ zx::time>>&,
    PresentTimestamps)>;
using RenderScheduledFrame =
    fit::function<void(uint64_t, zx::time, FrameRenderer::FramePresentedCallback)>;

// TODOs can be found in the frame scheduler epic: fxbug.dev/24406. Any new bugs filed concerning
// the frame scheduler should be added to it as well.

// DefaultFrameScheduler is the source of a number of events:
// - UpdateSessions: Fired when CPU should be done apply any pending updates to the scene graph and
//   prepare for rendering.
// - OnCpuWorkDone: Fired when CPU work has completed.
// - OnFramePresented: Fired when the renderer has signaled that the previous frame was presented
//   to the display.
// - RenderScheduledFrame: Fired when non-CPU-based rendering should begin.
class DefaultFrameScheduler final : public FrameScheduler {
 public:
  explicit DefaultFrameScheduler(std::unique_ptr<FramePredictor> predictor,
                                 inspect::Node inspect_node = inspect::Node(),
                                 metrics::Metrics* metrics_logger = nullptr);
  ~DefaultFrameScheduler();

  // Set the renderer and session updaters to be used. Can only be called once.
  // |session_updaters| will be called in this order for every event.
  void Initialize(std::shared_ptr<const VsyncTiming> vsync_timing, UpdateSessions update_sessions,
                  OnCpuWorkDone on_cpu_work_done, ::scheduling::OnFramePresented on_frame_presented,
                  RenderScheduledFrame render_scheduled_frame);

  // |FrameScheduler|
  void SetRenderContinuously(bool render_continuously) override;

  // |FrameScheduler|
  PresentId RegisterPresent(SessionId session_id, std::vector<zx::event> release_fences,
                            PresentId present_id = kInvalidPresentId) override;

  // |FrameScheduler|
  void ScheduleUpdateForSession(zx::time requested_presentation_time, SchedulingIdPair id_pair,
                                bool squashable) override;

  // |FrameScheduler|
  std::vector<FuturePresentationInfo> GetFuturePresentationInfos(
      zx::duration requested_prediction_span) override;

  // |FrameScheduler|
  void RemoveSession(SessionId session_id) override;

  constexpr static zx::duration kMinPredictedFrameDuration = zx::msec(0);
  constexpr static zx::duration kInitialRenderDuration = zx::msec(5);
  constexpr static zx::duration kInitialUpdateDuration = zx::msec(1);

  // Log some information that can be used to decide if Scenic is making progress.
  void LogPeriodicDebugInfo();

 private:
  void HandleFramePresented(uint64_t frame_number, zx::time render_start_time,
                            zx::time target_presentation_time,
                            const FrameRenderer::Timestamps& timestamps);

  // Requests a new frame to be drawn, which schedules the next wake up time for rendering. If we've
  // already scheduled a wake up time, it checks if it needs rescheduling and deals with it
  // appropriately.
  void RequestFrame(zx::time requested_presentation_time);

  // Check if there are pending updates, and if there are then find the lowest next requested
  // presentation time among all sessions and use it to request another frame.
  void HandleNextFrameRequest();

  // Update the global scene and then draw it... maybe.  There are multiple reasons why this might
  // not happen.  For example, the swapchain might apply back-pressure if we can't hit our target
  // frame rate. Or, the frame before this one has yet to finish rendering. Etc.
  void MaybeRenderFrame(async_dispatcher_t*, async::TaskBase*, zx_status_t);

  // Computes the target presentation time for the requested presentation time, and a wake-up time
  // that is early enough to start rendering in order to hit the target presentation time. These
  // times are guaranteed to be in the future.
  std::pair<zx::time, zx::time> ComputePresentationAndWakeupTimesForTargetTime(
      zx::time requested_presentation_time) const;

  // Executes updates that are scheduled up to and including a given presentation time.
  bool ApplyUpdates(zx::time target_presentation_time, zx::time latched_time,
                    uint64_t frame_number);

  // Return true if there are any scheduled session updates that have not yet been applied.
  bool HaveUpdatableSessions() const { return !pending_present_requests_.empty(); }

  // Signal all SessionUpdaters that frames up to |frame_number| have been presented.
  void SignalPresentedUpTo(uint64_t frame_number, zx::time presentation_time,
                           zx::duration presentation_interval);

  // Get map of latch times for each present up to |id_pair.present_id| for |id_pair.session_id|.
  std::map<PresentId, zx::time> ExtractLatchTimestampsUpTo(SchedulingIdPair id_pair);

  // Set all unset latched times for each registered present of |session_id|, up to and including
  // |present_id|.
  void SetLatchedTimeForPresentsUpTo(SchedulingIdPair id_pair, zx::time latched_time);

  // Extracts all presents that should be updated this frame and returns them as a map of SessionIds
  // to the last PresentId that should be updated for that session.
  std::unordered_map<SessionId, PresentId> CollectUpdatesForThisFrame(
      zx::time target_presentation_time);

  // Prepares all per-present data for later OnFrameRendered and OnFramePresented events.
  // Returns all the fences from previous presents for each session in |updates|.
  std::vector<zx::event> PrepareUpdates(const std::unordered_map<SessionId, PresentId>& updates,
                                        zx::time latched_time, uint64_t frame_number);

  // Removes all references to each session passed in.
  void RemoveFailedSessions(const std::unordered_set<SessionId>& sessions_with_failed_updates);

  struct PresentRequest {
    zx::time requested_presentation_time;
    trace_flow_id_t flow_id;
    // Determines if this Present can be combined with following Presents, or must be displayed for
    // at least one frame.
    bool squashable;
  };
  // Map of all pending Present calls ordered by SessionId and then PresentId.
  std::map<SchedulingIdPair, PresentRequest> pending_present_requests_;

  std::unordered_set<SessionId> sessions_with_unsquashable_updates_pending_presentation_;

  struct FrameUpdate {
    uint64_t frame_number;
    std::unordered_map<SessionId, PresentId> updated_sessions;
    zx::time latched_time;
  };
  // Queue of session updates mapped to frame numbers. Used in OnFramePresented.
  std::queue<FrameUpdate> latched_updates_;

  // All currently tracked presents and their associated latched_times.
  std::map<SchedulingIdPair, /*latched_time*/ std::optional<zx::time>> presents_;

  // Per-present release fences. To be released as each subsequent present for each session is
  // rendered.
  std::map<SchedulingIdPair, std::vector<zx::event>> release_fences_;

  // References.
  async_dispatcher_t* const dispatcher_;
  std::shared_ptr<const VsyncTiming> vsync_timing_ = nullptr;
  UpdateSessions update_sessions_;
  OnCpuWorkDone on_cpu_work_done_;
  OnFramePresented on_frame_presented_;
  RenderScheduledFrame render_scheduled_frame_;

  // State.
  // Frame number is 1-based so that |last_presented_frame_number_| can remain unsigned.
  uint64_t frame_number_ = 1;
  uint64_t last_presented_frame_number_ = 0;
  bool last_frame_is_presented_ = false;
  std::deque<zx::time> outstanding_latch_points_;
  bool render_continuously_ = false;
  zx::time wakeup_time_;
  zx::time next_target_presentation_time_;
  const std::unique_ptr<FramePredictor> frame_predictor_;

  // The async task that wakes up to start rendering.
  async::TaskMethod<DefaultFrameScheduler, &DefaultFrameScheduler::MaybeRenderFrame>
      frame_render_task_{this};

  uint64_t wakeups_without_render_ = 0;

  inspect::Node inspect_node_;
  inspect::UintProperty inspect_frame_number_;
  inspect::UintProperty inspect_wakeups_without_render_;
  inspect::UintProperty inspect_last_successful_update_start_time_;
  inspect::UintProperty inspect_last_successful_render_start_time_;

  // These mirror the inspect properties above, which are write-only.  They are used only for
  // logging, not for the frame-scheduling algorithm.
  zx::time last_successful_update_start_time_ = zx::time(0);
  zx::time last_successful_render_start_time_ = zx::time(0);

  FrameStats stats_;

  fxl::WeakPtrFactory<DefaultFrameScheduler> weak_factory_;  // must be last

  FXL_DISALLOW_COPY_AND_ASSIGN(DefaultFrameScheduler);
};

}  // namespace scheduling

#endif  // SRC_UI_SCENIC_LIB_SCHEDULING_DEFAULT_FRAME_SCHEDULER_H_
