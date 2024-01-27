// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_SCHEDULING_FRAME_SCHEDULER_H_
#define SRC_UI_SCENIC_LIB_SCHEDULING_FRAME_SCHEDULER_H_

#include <lib/fit/function.h>
#include <lib/zx/event.h>
#include <lib/zx/time.h>

#include <unordered_set>

#include "src/ui/scenic/lib/scheduling/id.h"

namespace scheduling {

struct PresentTimestamps {
  zx::time presented_time = zx::time(0);
  zx::duration vsync_interval = zx::duration(0);
};

struct FuturePresentationInfo {
  zx::time latch_point = zx::time(0);
  zx::time presentation_time = zx::time(0);
};

using SessionsWithFailedUpdates = std::unordered_set<SessionId>;

// The timestamp data that is expected to be delivered after rendering and presenting a frame.
// TODO(fxbug.dev/24669): If there are multiple render passes, |render_done_time| is the time
// furthest forward in time. Solving 24669 may involve expanding this struct to support multiple
// passes in data.
// TODO(fxbug.dev/70283): when there are multiple displays, there is no single "actual
// presentation time" that the FrameRenderer can return.
struct Timestamps {
  zx::time render_done_time;
  zx::time actual_presentation_time;
};
// Time value used to signal the time measurement was dropped.
static constexpr zx::time kTimeDropped = zx::time(ZX_TIME_INFINITE);

// Callback passed to the renderer when rendering a frame. It is the responsibility of the renderer
// to trigger the callback once all timestamp data is available.
using FramePresentedCallback = fit::function<void(const Timestamps&)>;

// The FrameScheduler is responsible for scheduling frames to be drawn in response to requests from
// clients.  When a frame is requested, the FrameScheduler will decide at which Vsync the frame
// should be displayed at. This time will be no earlier than the requested time, and will be as
// close as possible to the requested time, subject to various constraints.  For example, if the
// requested time is earlier than the time that rendering would finish, were it started immediately,
// then the frame will be scheduled for a later Vsync.
class FrameScheduler {
 public:
  virtual ~FrameScheduler() = default;

  // If |render_continuously|, we keep scheduling new frames immediately after each presented frame,
  // regardless of whether they're explicitly requested using RequestFrame().
  virtual void SetRenderContinuously(bool render_continuously) = 0;

  // Registers per-present information with the frame scheduler and returns an incrementing
  // PresentId unique to that session. When not equal to scheduling::kInvalidPresentId, the
  // |present_id| argument will be used in place of a new PresentId, allowing feed-forward
  // semantics for clients that need them.
  virtual PresentId RegisterPresent(SessionId session_id, std::vector<zx::event> release_fences,
                                    PresentId present_id = kInvalidPresentId) = 0;

  // Tell the FrameScheduler to schedule a frame. This is also used for updates triggered by
  // something other than a Session update i.e. an ImagePipe with a new Image to present.
  // |squashable| determines if the update is allowed to be combined with a following one in case
  // of delays.
  virtual void ScheduleUpdateForSession(zx::time presentation_time, SchedulingIdPair id_pair,
                                        bool squashable) = 0;

  // Gets the predicted latch points and presentation times for the frames at or before the next
  // |requested_prediction_span| time span. Uses the FramePredictor to do so.
  virtual std::vector<FuturePresentationInfo> GetFuturePresentationInfos(
      zx::duration requested_prediction_span) = 0;

  // Removes all references to |session_id|.
  virtual void RemoveSession(SessionId session_id) = 0;

  // Clients cannot call Present() anymore when |presents_in_flight_| reaches this value. Scenic
  // uses this to apply backpressure to clients.
  // TODO(fxbug.dev/44211): Move into implementation.
  static constexpr int64_t kMaxPresentsInFlight = 5;
};

}  // namespace scheduling

#endif  // SRC_UI_SCENIC_LIB_SCHEDULING_FRAME_SCHEDULER_H_
