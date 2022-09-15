// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_TESTING_FAKE_NODE_H_
#define SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_TESTING_FAKE_NODE_H_

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/media/audio/lib/format2/format.h"
#include "src/media/audio/services/common/logging.h"
#include "src/media/audio/services/mixer/common/basic_types.h"
#include "src/media/audio/services/mixer/fidl/node.h"
#include "src/media/audio/services/mixer/mix/testing/fake_pipeline_stage.h"

namespace media_audio {

class FakeNode;
class FakeGraph;

using FakeNodePtr = std::shared_ptr<FakeNode>;

// A fake node for use in tests.
// See FakeGraph for creation methods.
//
// Not safe for concurrent use.
class FakeNode : public Node, public std::enable_shared_from_this<FakeNode> {
 public:
  // Register a handler for `CreateNewChildSource`.
  // If a handler is not registered, a default handler is used.
  void SetOnCreateNewChildSource(std::function<NodePtr()> handler) {
    on_create_new_child_source_ = std::move(handler);
  }

  // Register a handler for `CreateNewChildDest`.
  // If a handler is not registered, a default handler is used.
  void SetOnCreateNewChildDest(std::function<NodePtr()> handler) {
    on_create_new_child_dest_ = std::move(handler);
  }

  // Register a handler for `DestroyChildSource`.
  // If a handler is not registered, a default handler is used.
  void SetOnDestroyChildSource(std::function<void(NodePtr)> handler) {
    on_destroy_child_source_ = std::move(handler);
  }

  // Register a handler for `DestroyChildDest`.
  // If a handler is not registered, a default handler is used.
  void SetOnDestroyChildDest(std::function<void(NodePtr)> handler) {
    on_destroy_child_dest_ = std::move(handler);
  }

  // Register a handler for `CanAcceptSourceFormat`.
  // The default handler always returns true.
  void SetOnCanAcceptSourceFormat(std::function<bool(const Format&)> handler) {
    on_can_accept_source_format_ = std::move(handler);
  }

  // Register a handler for `MaxSources`.
  // The default handler returns infinity.
  void SetOnMaxSources(std::function<std::optional<size_t>()> handler) {
    on_max_sources_ = std::move(handler);
  }

  // Register a handler for `AllowsDest`.
  // The default handler returns true.
  void SetOnAllowsDest(std::function<bool()> handler) { on_allows_dest_ = std::move(handler); }

  // Allow anyone to set the thread.
  using Node::set_pipeline_stage_thread;

  // Our PipelineStage is always this type.
  FakePipelineStagePtr fake_pipeline_stage() const {
    return std::static_pointer_cast<FakePipelineStage>(pipeline_stage());
  }

  // Implements `Node`.
  zx::duration GetSelfPresentationDelayForSource(const NodePtr& source) override {
    return zx::duration(0);
  }

 protected:
  // Implements Node.
  NodePtr CreateNewChildSource() final;
  NodePtr CreateNewChildDest() final;
  void DestroyChildSource(NodePtr child_source) final;
  void DestroyChildDest(NodePtr child_dest) final;
  bool CanAcceptSourceFormat(const Format& format) const final;
  std::optional<size_t> MaxSources() const final;
  bool AllowsDest() const final;

 private:
  // All FakeNodes belong to a FakeGraph. The constructor is private to ensure that it's impossible
  // to create a FakeNode which outlives its parent FakeGraph.
  friend class FakeGraph;
  FakeNode(FakeGraph& graph, NodeId id, bool is_meta, PipelineDirection pipeline_direction,
           FakeNodePtr parent, const Format* format);

  FakeGraph& graph_;
  std::function<NodePtr()> on_create_new_child_source_;
  std::function<NodePtr()> on_create_new_child_dest_;
  std::function<void(NodePtr)> on_destroy_child_source_;
  std::function<void(NodePtr)> on_destroy_child_dest_;
  std::function<bool(const Format&)> on_can_accept_source_format_;
  std::function<std::optional<size_t>()> on_max_sources_;
  std::function<bool()> on_allows_dest_;
};

// This class makes it easy to create graphs of FakeNodes during tests. For example, the following
// code:
//
// ```
//   auto graph = FakeGraph::Create({
//       .meta_nodes = {
//           {1, {
//               .source_children = {2, 3},
//               .dest_children = {4, 5},
//           }},
//       },
//       .edges = {
//           {0, 2},
//           {4, 6},
//           {5, 7},
//       },
//    });
// ```
//
// Creates a graph that looks like:
//
// ```
//     0
//     |
//   +-V-----+
//   | 2   3 |
//   |   1   |
//   | 4   5 |
//   +-|---|-+
//     V   V
//     6   7
// ```
//
// The destructor deletes all edges (to remove circular references) and drops all references to
// FakeNodes that were created by this FakeGraph.
//
// Not safe for concurrent use.
class FakeGraph {
 public:
  struct MetaNodeArgs {
    std::unordered_set<NodeId> source_children;
    std::unordered_set<NodeId> dest_children;
  };

  struct Edge {
    NodeId source;
    NodeId dest;
  };

  struct Args {
    // Meta nodes and their children.
    std::unordered_map<NodeId, MetaNodeArgs> meta_nodes;

    // Adjaceny list.
    // All nodes must be ordinary nodes (i.e. not a key of `meta_nodes`).
    std::vector<Edge> edges;

    // Unconnected ordinary nodes.
    // These must not be mentioned in `edges`.
    std::unordered_set<NodeId> unconnected_ordinary_nodes;

    // Format of data generated by each node's PipelineStage.
    // The `Format*` pointers do not need to live beyond the constructor.
    std::vector<std::pair<const Format*, std::vector<NodeId>>> formats;

    // Direction of each node.
    std::unordered_map<PipelineDirection, std::unordered_set<NodeId>> pipeline_directions;

    // The default direction if not specified above.
    PipelineDirection default_pipeline_direction;

    // Assignment of nodes to threads.
    // All nodes must be ordinary nodes (i.e. not a key of `meta_nodes`).
    std::unordered_map<ThreadPtr, std::vector<NodeId>> threads;

    // The default thread to use if not specified above.
    // May be nullptr.
    ThreadPtr default_thread;
  };

  explicit FakeGraph(Args args);
  ~FakeGraph();

  // Creates a meta node or return the node if the `id` already exists.
  // It is illegal to call CreateMetaNode and CreateOrdinaryNode with the same `id`.
  //
  // If `id` is unspecified, an `id` is selected automatically.
  FakeNodePtr CreateMetaNode(std::optional<NodeId> id);

  // Creates an ordinary node or return the node if `id` already exists.
  // It is illegal to call CreateMetaNode and CreateOrdinaryNode with the same `id`.
  //
  // If `id` is unspecified, an `id` is selected automatically.
  // If `parent` is specified and `id` already exists, the given `parent` must match the old parent.
  FakeNodePtr CreateOrdinaryNode(std::optional<NodeId> id, FakeNodePtr parent);

  // Returns the node with the given ID.
  // Must exist.
  FakeNodePtr node(NodeId id) const {
    auto it = nodes_.find(id);
    FX_CHECK(it != nodes_.end()) << "FakeGraph does have node " << id;
    return it->second;
  }

 private:
  NodeId NextId();
  PipelineDirection PipelineDirectionForNode(NodeId id) const;

  std::unordered_map<NodeId, FakeNodePtr> nodes_;
  std::unordered_map<NodeId, std::shared_ptr<Format>> formats_;
  std::unordered_map<PipelineDirection, std::unordered_set<NodeId>> pipeline_directions_;
  PipelineDirection default_pipeline_direction_;
  ThreadPtr default_thread_;
};

}  // namespace media_audio

#endif  // SRC_MEDIA_AUDIO_SERVICES_MIXER_FIDL_TESTING_FAKE_NODE_H_
