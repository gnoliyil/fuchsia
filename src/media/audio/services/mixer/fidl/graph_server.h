// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_GRAPH_SERVER_H_
#define SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_GRAPH_SERVER_H_

#include <fidl/fuchsia.audio.mixer/cpp/wire.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/profile.h>
#include <zircon/errors.h>

#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "src/media/audio/lib/clock/timer.h"
#include "src/media/audio/services/common/base_fidl_server.h"
#include "src/media/audio/services/mixer/common/basic_types.h"
#include "src/media/audio/services/mixer/fidl/clock_registry.h"
#include "src/media/audio/services/mixer/fidl/consumer_node.h"
#include "src/media/audio/services/mixer/fidl/gain_control_server.h"
#include "src/media/audio/services/mixer/fidl/graph_detached_thread.h"
#include "src/media/audio/services/mixer/fidl/graph_mix_thread.h"
#include "src/media/audio/services/mixer/fidl/node.h"
#include "src/media/audio/services/mixer/fidl/producer_node.h"
#include "src/media/audio/services/mixer/fidl/ptr_decls.h"

namespace media_audio {

class GraphServer
    : public BaseFidlServer<GraphServer, fidl::WireServer, fuchsia_audio_mixer::Graph>,
      public std::enable_shared_from_this<GraphServer> {
 public:
  struct Args {
    // Name of this graph.
    // For debugging only: may be empty or not unique.
    std::string name;

    // Factory to create clocks used by this graph.
    std::shared_ptr<ClockFactory> clock_factory;

    // Registry for all clocks used by this graph.
    std::shared_ptr<ClockRegistry> clock_registry;
  };

  // The returned server will live until the `server_end` channel is closed.
  static std::shared_ptr<GraphServer> Create(std::shared_ptr<const FidlThread> fidl_thread,
                                             fidl::ServerEnd<fuchsia_audio_mixer::Graph> server_end,
                                             Args args);

  // Implementation of fidl::WireServer<fuchsia_audio_mixer::Graph>.
  void CreateProducer(CreateProducerRequestView request,
                      CreateProducerCompleter::Sync& completer) final;
  void CreateConsumer(CreateConsumerRequestView request,
                      CreateConsumerCompleter::Sync& completer) final;
  void CreateMixer(CreateMixerRequestView request, CreateMixerCompleter::Sync& completer) final;
  void CreateSplitter(CreateSplitterRequestView request,
                      CreateSplitterCompleter::Sync& completer) final;
  void CreateCustom(CreateCustomRequestView request, CreateCustomCompleter::Sync& completer) final;
  void DeleteNode(DeleteNodeRequestView request, DeleteNodeCompleter::Sync& completer) final;
  void CreateEdge(CreateEdgeRequestView request, CreateEdgeCompleter::Sync& completer) final;
  void DeleteEdge(DeleteEdgeRequestView request, DeleteEdgeCompleter::Sync& completer) final;
  void CreateThread(CreateThreadRequestView request, CreateThreadCompleter::Sync& completer) final;
  void DeleteThread(DeleteThreadRequestView request, DeleteThreadCompleter::Sync& completer) final;
  void CreateGainControl(CreateGainControlRequestView request,
                         CreateGainControlCompleter::Sync& completer) final;
  void DeleteGainControl(DeleteGainControlRequestView request,
                         DeleteGainControlCompleter::Sync& completer) final;
  void CreateGraphControlledReferenceClock(
      CreateGraphControlledReferenceClockCompleter::Sync& completer) final;
  void Start(StartRequestView request, StartCompleter::Sync& completer) final;
  void Stop(StopRequestView request, StopCompleter::Sync& completer) final;
  void CancelStartOrStop(CancelStartOrStopRequestView request,
                         CancelStartOrStopCompleter::Sync& completer) final;
  void BindProducerLeadTimeWatcher(BindProducerLeadTimeWatcherRequestView request,
                                   BindProducerLeadTimeWatcherCompleter::Sync& completer) final;

  // Name of this graph.
  // For debugging only: may be empty or not unique.
  std::string_view name() const { return name_; }

 private:
  static inline constexpr std::string_view kClassName = "GraphServer";
  template <typename ServerT, template <typename T> typename FidlServerT, typename ProtocolT>
  friend class BaseFidlServer;

  // Note: args.server_end is consumed by BaseFidlServer.
  explicit GraphServer(Args args);

  void OnShutdown(fidl::UnbindInfo info) final;
  GainControlId NextGainControlId();
  NodeId NextNodeId();
  ThreadId NextThreadId();

  const std::string name_;
  const std::shared_ptr<GlobalTaskQueue> global_task_queue_ = std::make_shared<GlobalTaskQueue>();
  const std::shared_ptr<GraphDetachedThread> detached_thread_ =
      std::make_shared<GraphDetachedThread>(global_task_queue_);

  const std::shared_ptr<ClockFactory> clock_factory_;
  const std::shared_ptr<ClockRegistry> clock_registry_;

  // Gain controls mapping.
  std::unordered_map<GainControlId, std::shared_ptr<GainControlServer>> gain_controls_;
  GainControlId next_gain_control_id_ = 1;

  // Nodes mappings.
  std::unordered_map<NodeId, NodePtr> nodes_;  // contains all nodes
  std::unordered_map<NodeId, std::shared_ptr<ProducerNode>> producer_nodes_;
  std::unordered_map<NodeId, std::shared_ptr<ConsumerNode>> consumer_nodes_;
  std::unordered_map<NodeId, std::unordered_set<NodeId>> custom_node_children_ids_;
  NodeId next_node_id_ = 1;

  // Threads mapping.
  std::unordered_map<ThreadId, std::shared_ptr<GraphMixThread>> mix_threads_;
  ThreadId next_thread_id_ = 1;

  // List of pending one-shot waiters. Each waiter is responsible for removing themselves from this
  // list after they have run.
  std::list<async::WaitOnce> pending_one_shot_waiters_;

  // How many graph-controlled clocks have been created.
  int64_t num_graph_controlled_clocks_ = 0;

  const Node::GraphContext ctx_ = {
      .gain_controls = gain_controls_,
      .global_task_queue = *global_task_queue_,
      .detached_thread = detached_thread_,
  };
};

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_GRAPH_SERVER_H_
