// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_PRODUCER_NODE_H_
#define SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_PRODUCER_NODE_H_

#include <lib/zx/time.h>
#include <zircon/types.h>

#include <memory>
#include <variant>

#include "src/media/audio/services/common/delay_watcher_client.h"
#include "src/media/audio/services/common/delay_watcher_server.h"
#include "src/media/audio/services/mixer/common/basic_types.h"
#include "src/media/audio/services/mixer/fidl/node.h"
#include "src/media/audio/services/mixer/fidl/ptr_decls.h"
#include "src/media/audio/services/mixer/fidl/stream_sink_server.h"
#include "src/media/audio/services/mixer/mix/producer_stage.h"
#include "src/media/audio/services/mixer/mix/ring_buffer.h"
#include "src/media/audio/services/mixer/mix/start_stop_control.h"

namespace media_audio {

// This is an ordinary node that wraps a ProducerStage.
class ProducerNode : public Node {
 public:
  using DataSource = std::variant<std::shared_ptr<StreamSinkServer>, std::shared_ptr<RingBuffer>>;

  struct Args {
    // Name of this node.
    std::string_view name;

    // Whether this node participates in an input pipeline or an output pipeline.
    PipelineDirection pipeline_direction;

    // Format of data produced by this node.
    Format format;

    // Reference clock of this nodes's destination streams.
    std::shared_ptr<Clock> reference_clock;

    // Ticks of media time per nanoseconds of reference time.
    TimelineRate media_ticks_per_ns;

    // Object from which to produce data.
    DataSource data_source;

    // For input pipelines, the upstream delay at this producer.
    std::shared_ptr<DelayWatcherClient> delay_watcher;

    // For output pipelines, BindLeadTimeWatcher creates DelayWatcherServers on this thread.
    std::shared_ptr<const FidlThread> thread_for_lead_time_servers;

    // On creation, the node is initially assigned to this detached thread.
    GraphDetachedThreadPtr detached_thread;

    // For queuing tasks on mixer threads.
    std::shared_ptr<GlobalTaskQueue> global_task_queue;
  };

  static std::shared_ptr<ProducerNode> Create(Args args);

  // Starts this producer. The command is forwarded to the underlying ProducerStage.
  //
  // Returns false if there is already a pending Start or Stop command. Otherwise, returns true.
  bool Start(ProducerStage::StartCommand cmd) const;

  // Stops this producer. The command is forwarded to the underlying ProducerStage.
  //
  // Returns false if there is already a pending Start or Stop command. Otherwise, returns true.
  bool Stop(ProducerStage::StopCommand cmd) const;

  // Cancels pending Start or Stop command.
  void CancelStartOrStop() const;

  // Binds a new lead time watcher.
  // REQUIRED: `pipeline_direction() == kOutput`
  void BindLeadTimeWatcher(fidl::ServerEnd<fuchsia_audio::DelayWatcher> server_end);

  // Implements `Node`.
  std::optional<std::pair<ThreadId, fit::closure>> SetMaxDelays(Delays delays) final;
  zx::duration PresentationDelayForSourceEdge(const Node* source) const final;

 private:
  using PendingStartStopCommand = ProducerStage::PendingStartStopCommand;

  ProducerNode(std::string_view name, std::shared_ptr<Clock> reference_clock,
               PipelineDirection pipeline_direction, PipelineStagePtr pipeline_stage,
               std::shared_ptr<PendingStartStopCommand> pending_start_stop_command,
               std::shared_ptr<DelayWatcherClient> delay_watcher,
               std::shared_ptr<DelayWatcherServerGroup> delay_reporter,
               std::shared_ptr<GlobalTaskQueue> global_task_queue);

  NodePtr CreateNewChildSource() final {
    UNREACHABLE << "CreateNewChildSource should not be called on ordinary nodes";
  }
  NodePtr CreateNewChildDest() final {
    UNREACHABLE << "CreateNewChildDest should not be called on ordinary nodes";
  }
  void PrepareToDeleteSelf() final;
  bool CanAcceptSourceFormat(const Format& format) const final;
  std::optional<size_t> MaxSources() const final;
  bool AllowsDest() const final;
  void SetUpstreamInputDelay(std::optional<zx::duration> delay);

  const std::shared_ptr<PendingStartStopCommand> pending_start_stop_command_;
  const std::shared_ptr<GlobalTaskQueue> global_task_queue_;
  const std::shared_ptr<DelayWatcherServerGroup> delay_reporter_;

  // Logically const, but non-const so we can discard this in `PrepareToDeleteSelf` to remove a
  // circular reference.
  std::shared_ptr<DelayWatcherClient> delay_watcher_;

  zx::duration upstream_input_delay_;
};

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_PRODUCER_NODE_H_
