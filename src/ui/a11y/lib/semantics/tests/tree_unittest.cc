// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/executor.h>
#include <lib/fdio/fd.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/inspect/testing/cpp/inspect.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/zx/event.h>

#include <algorithm>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/lib/files/file.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/a11y/bin/a11y_manager/tests/util/util.h"
#include "src/ui/a11y/lib/semantics/semantic_tree.h"
#include "src/ui/a11y/lib/semantics/tests/semantic_tree_parser.h"
#include "src/ui/a11y/lib/util/util.h"

namespace accessibility_test {
namespace {

using ::a11y::SemanticTree;
using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::Role;
using ::inspect::Inspector;
using ::testing::HasSubstr;

// Valid tree paths.
const std::string kSemanticTreeSingleNodePath = "/pkg/data/semantic_tree_single_node.json";
const std::string kSemanticTreeOddNodesPath = "/pkg/data/semantic_tree_odd_nodes.json";
const std::string kSemanticTreeEvenNodesPath = "/pkg/data/semantic_tree_even_nodes.json";
// Invalid tree paths.
const std::string kSemanticTreeWithCyclePath = "/pkg/data/cyclic_semantic_tree.json";
const std::string kSemanticTreeWithMissingChildrenPath =
    "/pkg/data/semantic_tree_not_parseable.json";

constexpr char kInspectNodeName[] = "test_inspect_node";

class SemanticTreeTest : public gtest::RealLoopFixture {
 public:
  SemanticTreeTest() : executor_(dispatcher()) {}

 protected:
  void SetUp() override {
    RealLoopFixture::SetUp();

    inspector_ = std::make_unique<inspect::Inspector>();
    tree_ = std::make_unique<SemanticTree>(inspector_->GetRoot().CreateChild(kInspectNodeName));
    tree_->set_action_handler([this](uint32_t node_id,
                                     fuchsia::accessibility::semantics::Action action,
                                     fuchsia::accessibility::semantics::SemanticListener::
                                         OnAccessibilityActionRequestedCallback callback) {
      this->action_handler_called_ = true;
    });
    tree_->set_hit_testing_handler(
        [this](fuchsia::math::PointF local_point,
               fuchsia::accessibility::semantics::SemanticListener::HitTestCallback callback) {
          this->hit_testing_called_ = true;
        });
  }

  // Helper function to ensure that a promise completes.
  void RunPromiseToCompletion(fpromise::promise<> promise) {
    bool done = false;
    executor_.schedule_task(std::move(promise).and_then([&]() { done = true; }));
    RunLoopUntil([&] { return done; });
  }

  // Checks if the tree contains all nodes  in |ids|.
  void TreeContainsNodes(const std::vector<uint32_t>& ids) {
    for (const auto id : ids) {
      auto node = tree_->GetNode(id);
      EXPECT_TRUE(node);
      EXPECT_EQ(node->node_id(), id);
    }
  }

  SemanticTree::TreeUpdates BuildUpdatesFromFile(const std::string& file_path) {
    SemanticTree::TreeUpdates updates;
    std::vector<Node> nodes;
    EXPECT_TRUE(semantic_tree_parser_.ParseSemanticTree(file_path, &nodes));
    for (auto& node : nodes) {
      updates.emplace_back(std::move(node));
    }
    return updates;
  }

  SemanticTreeParser semantic_tree_parser_;

  // Whether the action handler was called.
  bool action_handler_called_ = false;

  // Whether the hit testing handler was called.
  bool hit_testing_called_ = false;

  // Required to verify inspect metrics.
  std::unique_ptr<inspect::Inspector> inspector_;

  // Our test subject.
  std::unique_ptr<SemanticTree> tree_;

  // Required to retrieve inspect metrics.
  async::Executor executor_;
};

TEST_F(SemanticTreeTest, GetNodesById) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeSingleNodePath);

  EXPECT_TRUE(tree_->Update(std::move(updates)));

  // Attempt to retrieve node with id not present in tree.
  auto invalid_node = tree_->GetNode(1u);
  auto root = tree_->GetNode(SemanticTree::kRootNodeId);

  EXPECT_EQ(invalid_node, nullptr);
  EXPECT_EQ(root->node_id(), SemanticTree::kRootNodeId);
}

TEST_F(SemanticTreeTest, ClearsTheTree) {
  SemanticTree::TreeUpdates updates;
  updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
  updates.emplace_back(CreateTestNode(1u, "node1"));
  updates.emplace_back(CreateTestNode(2u, "node2"));

  EXPECT_TRUE(tree_->Update(std::move(updates)));
  EXPECT_EQ(tree_->Size(), 3u);

  // Set event callback to verify that callback was called with the correct
  // event type.
  bool semantics_event_callback_called = false;
  tree_->set_semantics_event_callback(
      [&semantics_event_callback_called](a11y::SemanticsEventInfo event_info) {
        semantics_event_callback_called = true;
        EXPECT_EQ(event_info.event_type, a11y::SemanticsEventType::kSemanticTreeUpdated);
      });

  tree_->Clear();
  EXPECT_EQ(tree_->Size(), 0u);
  EXPECT_TRUE(semantics_event_callback_called);
}

TEST_F(SemanticTreeTest, SemanticsEventCallbackInvokedOnSuccessfulUpdate) {
  // Set event callback to verify that callback was called with the correct
  // event type.
  bool semantics_event_callback_called = false;
  tree_->set_semantics_event_callback(
      [&semantics_event_callback_called](a11y::SemanticsEventInfo event_info) {
        semantics_event_callback_called = true;
        EXPECT_EQ(event_info.event_type, a11y::SemanticsEventType::kSemanticTreeUpdated);
      });

  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));
  EXPECT_TRUE(semantics_event_callback_called);
}

TEST_F(SemanticTreeTest, ReceivesTreeInOneSingleUpdate) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_->Update(std::move(updates)));
  TreeContainsNodes(added_ids);
}

TEST_F(SemanticTreeTest, BuildsTreeFromTheLeaves) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  // Updates is in ascending order. Sort it in descending order to send the
  // updates from the leaves.
  std::sort(updates.begin(), updates.end(),
            [](const auto& a, const auto& b) { return a.node().node_id() > b.node().node_id(); });

  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_->Update(std::move(updates)));
  TreeContainsNodes(added_ids);
}

TEST_F(SemanticTreeTest, InvalidTreeWithoutParent) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  // Remove the root (first node).
  updates.erase(updates.begin());
  EXPECT_FALSE(tree_->Update(std::move(updates)));
}

TEST_F(SemanticTreeTest, InvalidTreeWithCycle) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeWithCyclePath);
  EXPECT_FALSE(tree_->Update(std::move(updates)));
  EXPECT_EQ(tree_->Size(), 0u);
}

TEST_F(SemanticTreeTest, DeletingNodesByUpdatingTheParent) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_->Update(std::move(updates)));
  {
    auto root = tree_->GetNode(SemanticTree::kRootNodeId);
    EXPECT_EQ(root->attributes().label(), "Node-0");
    EXPECT_EQ(root->child_ids().size(), 2u);
  }
  // Update the root to point to nobody else.
  auto new_root = CreateTestNode(SemanticTree::kRootNodeId, "node1");
  new_root.set_child_ids(std::vector<uint32_t>());  // Points to no children.
  new_root.mutable_attributes()->set_label("new node");
  EXPECT_TRUE(new_root.has_child_ids());
  SemanticTree::TreeUpdates new_updates;
  new_updates.emplace_back(std::move(new_root));
  EXPECT_TRUE(tree_->Update(std::move(new_updates)));
  {
    auto root = tree_->GetNode(0);
    EXPECT_TRUE(root->child_ids().empty());
    EXPECT_EQ(root->attributes().label(), "new node");
  }
  EXPECT_EQ(tree_->Size(), 1u);
  for (const auto id : added_ids) {
    auto node = tree_->GetNode(id);
    if (id == SemanticTree::kRootNodeId) {
      EXPECT_TRUE(node);
      EXPECT_EQ(node->node_id(), id);
    } else {
      EXPECT_FALSE(node);
    }
  }
}

TEST_F(SemanticTreeTest, ExplicitlyDeletingNodes) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  SemanticTree::TreeUpdates delete_updates;
  delete_updates.emplace_back(5);
  delete_updates.emplace_back(6);
  // Update the parent.
  auto updated_parent = CreateTestNode(2, "updated parent");
  *updated_parent.mutable_child_ids() = std::vector<uint32_t>();
  delete_updates.push_back(std::move(updated_parent));
  // Remove 5 and 6 from |added_ids|.
  auto it_5 = std::find(added_ids.begin(), added_ids.end(), 5);
  EXPECT_NE(it_5, added_ids.end());
  added_ids.erase(it_5);
  auto it_6 = std::find(added_ids.begin(), added_ids.end(), 6);
  EXPECT_NE(it_6, added_ids.end());
  added_ids.erase(it_6);
  EXPECT_TRUE(tree_->Update(std::move(delete_updates)));

  EXPECT_EQ(tree_->Size(), 5u);
  TreeContainsNodes(added_ids);
}

TEST_F(SemanticTreeTest, DeletingRootNodeClearsTheTree) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  SemanticTree::TreeUpdates delete_updates;
  delete_updates.emplace_back(SemanticTree::kRootNodeId);
  EXPECT_TRUE(tree_->Update(std::move(delete_updates)));

  EXPECT_EQ(tree_->Size(), 0u);
}

TEST_F(SemanticTreeTest, ReplaceNodeWithADeletion) {
  {
    SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
    EXPECT_TRUE(tree_->Update(std::move(updates)));
  }

  {
    SemanticTree::TreeUpdates updates;
    updates.emplace_back(2);

    // Create a test node that sets a new field (1), and does NOT set one of the
    // fields in the original node (2). We want to verify that the new node contains
    // field (1), but not field (2).
    fuchsia::accessibility::semantics::Node node;
    node.set_node_id(2u);
    node.set_child_ids({5, 6});
    node.set_container_id(0u);
    updates.emplace_back(std::move(node));

    EXPECT_TRUE(tree_->Update(std::move(updates)));
  }

  EXPECT_EQ(tree_->Size(), 7u);
  auto node = tree_->GetNode(2);
  EXPECT_TRUE(node);

  // The node should no longer have a label. If it still does, then we did not
  // properly delete the node before updating.
  EXPECT_FALSE(node->has_attributes());

  EXPECT_TRUE(node->has_container_id());
  EXPECT_EQ(node->container_id(), 0u);
  EXPECT_THAT(node->child_ids(), testing::ElementsAre(5, 6));
}

TEST_F(SemanticTreeTest, SemanticTreeWithMissingChildren) {
  SemanticTree::TreeUpdates updates;
  updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
  updates.emplace_back(CreateTestNode(1u, "node1"));
  updates.emplace_back(CreateTestNode(2u, "node2", {3}));
  EXPECT_FALSE(tree_->Update(std::move(updates)));
  EXPECT_EQ(tree_->Size(), 0u);
}

TEST_F(SemanticTreeTest, SemanticTreeCommitLeavesTreeEmpty) {
  SemanticTree::TreeUpdates updates;

  // Create and delete the same set of nodes.
  updates.emplace_back(CreateTestNode(1u, "node1", {2u}));
  updates.emplace_back(CreateTestNode(2u, "node2", {}));
  updates.emplace_back(1u);
  updates.emplace_back(2u);

  EXPECT_TRUE(tree_->Update(std::move(updates)));
}

TEST_F(SemanticTreeTest, SemanticTreeCommitLeavesTreeEmptyInvalidDeletion) {
  SemanticTree::TreeUpdates updates;

  // Attempt to delete a node that doesn't exist.
  updates.emplace_back(1u);

  EXPECT_FALSE(tree_->Update(std::move(updates)));
}

TEST_F(SemanticTreeTest, SemanticTreeCommitCreatesAndDeletesSameNodes) {
  {
    SemanticTree::TreeUpdates updates;
    updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
    updates.emplace_back(CreateTestNode(1u, "node1"));
    updates.emplace_back(CreateTestNode(2u, "node2"));

    EXPECT_TRUE(tree_->Update(std::move(updates)));
    EXPECT_EQ(tree_->Size(), 3u);
  }

  // Create and delete the same set of nodes.
  {
    SemanticTree::TreeUpdates updates;
    updates.emplace_back(CreateTestNode(3u, "node3"));
    updates.emplace_back(CreateTestNode(4u, "node4"));
    updates.emplace_back(3u);
    updates.emplace_back(4u);

    EXPECT_TRUE(tree_->Update(std::move(updates)));
  }

  EXPECT_EQ(tree_->Size(), 3u);
}

TEST_F(SemanticTreeTest, PartialUpdateCopiesNewInfo) {
  {
    SemanticTree::TreeUpdates updates;
    updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
    updates.emplace_back(CreateTestNode(1u, "node1"));
    updates.emplace_back(CreateTestNode(2u, "node2"));
    EXPECT_TRUE(tree_->Update(std::move(updates)));
  }

  EXPECT_EQ(tree_->Size(), 3u);
  SemanticTree::TreeUpdates updates;
  // Partial update of the root node with a new label.
  // Please note that there are three partial updates on the root node, and the
  // partial update must always be applied on top of the existing one.
  auto first_root_update = CreateTestNode(SemanticTree::kRootNodeId, "root", {1, 2, 10});
  first_root_update.set_role(fuchsia::accessibility::semantics::Role::UNKNOWN);
  first_root_update.mutable_states()->set_selected(true);
  updates.emplace_back(std::move(first_root_update));

  auto second_root_update = CreateTestNode(SemanticTree::kRootNodeId, "root");
  second_root_update.mutable_states()->set_selected(false);
  second_root_update.set_actions({fuchsia::accessibility::semantics::Action::DEFAULT,
                                  fuchsia::accessibility::semantics::Action::SHOW_ON_SCREEN});
  updates.emplace_back(std::move(second_root_update));

  auto third_root_update = CreateTestNode(SemanticTree::kRootNodeId, "updated label");
  fuchsia::ui::gfx::BoundingBox box;
  box.max.z = 10.f;
  third_root_update.set_location(box);
  third_root_update.set_transform(
      scenic::NewMatrix4Value({2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1}).value);
  third_root_update.set_container_id(2u);
  updates.emplace_back(std::move(third_root_update));

  updates.emplace_back(CreateTestNode(10, "node 10"));

  EXPECT_TRUE(tree_->Update(std::move(updates)));
  EXPECT_EQ(tree_->Size(), 4u);

  // Verify that the contents of the root node represent a merge of the three
  // updates.
  auto root = tree_->GetNode(SemanticTree::kRootNodeId);
  EXPECT_EQ(root->attributes().label(), "updated label");
  EXPECT_EQ(root->actions().size(), 2u);
  EXPECT_EQ(root->location().max.z, 10.f);
  EXPECT_EQ(root->transform().matrix[0], 2);
  EXPECT_EQ(root->container_id(), 2u);
  EXPECT_THAT(root->child_ids(), testing::ElementsAre(1, 2, 10));
  EXPECT_EQ(root->role(), fuchsia::accessibility::semantics::Role::UNKNOWN);
  EXPECT_FALSE(root->states().selected());
}

TEST_F(SemanticTreeTest, ReparentsNodes) {
  // A common use case of semantic trees is to reparent a node. Within an
  // update, reparenting would look like as a removal of a child node ID of one
  // node and the addition of that same child node ID to another node (new
  // parent).
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));
  SemanticTree::TreeUpdates reparenting_updates;
  reparenting_updates.push_back(
      CreateTestNode(SemanticTree::kRootNodeId, "root", {1}));  // 2 removed.
  reparenting_updates.push_back(
      CreateTestNode(1, "new parent", {3, 4, 2}));  // 2 will have 1 as new parent.
  EXPECT_TRUE(tree_->Update(std::move(reparenting_updates)));
  EXPECT_EQ(tree_->Size(), 7u);
  auto root = tree_->GetNode(SemanticTree::kRootNodeId);
  EXPECT_TRUE(root);
  EXPECT_THAT(root->child_ids(), testing::ElementsAre(1));
  auto new_parent = tree_->GetNode(1);
  EXPECT_TRUE(new_parent);
  EXPECT_THAT(new_parent->child_ids(), testing::ElementsAre(3, 4, 2));
}

TEST_F(SemanticTreeTest, GetParentNodeTest) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));
  auto parent = tree_->GetParentNode(1);
  auto missing_parent = tree_->GetParentNode(SemanticTree::kRootNodeId);
  EXPECT_TRUE(parent);
  EXPECT_FALSE(missing_parent);

  EXPECT_EQ(parent->node_id(), SemanticTree::kRootNodeId);
}

TEST_F(SemanticTreeTest, PerformAccessibilityActionRequested) {
  tree_->PerformAccessibilityAction(1, fuchsia::accessibility::semantics::Action::DEFAULT,
                                    [](auto...) {});
  EXPECT_TRUE(action_handler_called_);
}

TEST_F(SemanticTreeTest, PerformHitTestingRequested) {
  tree_->PerformHitTesting({1, 1}, [](auto...) {});
  EXPECT_TRUE(hit_testing_called_);
}

TEST_F(SemanticTreeTest, NextNodeExists) {
  // Tests the case where semantic tree is not balanced, and GetNextNode is called on a node which
  // is the leaf node, without any sibling. This will fail in case of a level order traversal.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeEvenNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetNextNode(
      7u, [](const fuchsia::accessibility::semantics::Node* node) { return true; });
  EXPECT_NE(next_node, nullptr);
  EXPECT_EQ(next_node->node_id(), 4u);
}

TEST_F(SemanticTreeTest, GetNextNodeFilterReturnsFalse) {
  // Test case where intermediate nodes which are not describable are skipped. This will fail in
  // case of level order traversal.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetNextNode(
      2u, [](const fuchsia::accessibility::semantics::Node* node) { return false; });
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, NoNextNode) {
  // Tests case where next node doesn't exist.This will fail in case of level order traversal.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeEvenNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetNextNode(
      6u, [](const fuchsia::accessibility::semantics::Node* node) { return true; });
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, GetNextNodeForNonexistentId) {
  // Tests case where input node doesn't exist.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetNextNode(
      10u, [](const fuchsia::accessibility::semantics::Node* node) { return true; });
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, PreviousNodeExists) {
  // Tests the case where semantic tree is not balanced, and GetPreviousNode is called on a non leaf
  // which should return a leaf node. This will fail in case of a level order traversal.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeEvenNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetPreviousNode(
      4u, [](const fuchsia::accessibility::semantics::Node* node) { return true; });
  EXPECT_NE(next_node, nullptr);
  EXPECT_EQ(next_node->node_id(), 7u);
}

TEST_F(SemanticTreeTest, GetPreviousNodeFilterReturnsFalse) {
  // Test case where intermediate nodes which are not describable are skipped.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  updates.clear();

  auto previous_node = tree_->GetPreviousNode(
      6u, [](const fuchsia::accessibility::semantics::Node* node) { return false; });
  EXPECT_EQ(previous_node, nullptr);
}

TEST_F(SemanticTreeTest, NoPreviousNode) {
  // Tests case where previous node doesn't exist.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetPreviousNode(
      0u, [](const fuchsia::accessibility::semantics::Node* node) { return true; });
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, GetPreviousNodeForNonexistentId) {
  // Tests case where input node doesn't exist.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  auto next_node = tree_->GetPreviousNode(
      10u, [](const fuchsia::accessibility::semantics::Node* node) { return true; });
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, NodeFilterWithParentGetsCorrectParent) {
  // Test case using a NodeFilterWithParent - confirms that it always receives
  // the right parent node.
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeEvenNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  const auto kNodeFilterCheckingParents =
      [](const fuchsia::accessibility::semantics::Node* node,
         const fuchsia::accessibility::semantics::Node* parent) {
        // Check to ensure that `parent` is actually the parent of `node`.
        if (parent) {
          auto children = parent->child_ids();
          EXPECT_TRUE(std::find(children.begin(), children.end(), node->node_id()) !=
                      children.end());
        } else {
          EXPECT_TRUE(node->node_id() == 1);
        }

        return false;
      };

  for (int i = 0; i < 8; i++) {
    tree_->GetNextNode(0u, kNodeFilterCheckingParents);
    tree_->GetPreviousNode(0u, kNodeFilterCheckingParents);
  }
}

TEST_F(SemanticTreeTest, InspectOutput) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_->Update(std::move(updates)));

  fpromise::result<inspect::Hierarchy> hierarchy;
  ASSERT_FALSE(hierarchy.is_ok());
  RunPromiseToCompletion(inspect::ReadFromInspector(*inspector_)
                             .then([&](fpromise::result<inspect::Hierarchy>& result) {
                               hierarchy = std::move(result);
                             }));
  ASSERT_TRUE(hierarchy.is_ok());

  using namespace inspect::testing;
  using testing::UnorderedElementsAre;

  auto node3 =
      AllOf(NodeMatches(AllOf(
                NameMatches("node_idx_0_id_3"),
                PropertyList(UnorderedElementsAre(UintIs("id", 3), StringIs("label", "Node-3"))))),
            ChildrenMatch(UnorderedElementsAre()));
  auto node4 =
      AllOf(NodeMatches(AllOf(
                NameMatches("node_idx_1_id_4"),
                PropertyList(UnorderedElementsAre(UintIs("id", 4), StringIs("label", "Node-4"))))),
            ChildrenMatch(UnorderedElementsAre()));
  auto node5 =
      AllOf(NodeMatches(AllOf(
                NameMatches("node_idx_0_id_5"),
                PropertyList(UnorderedElementsAre(UintIs("id", 5), StringIs("label", "Node-5"))))),
            ChildrenMatch(UnorderedElementsAre()));
  auto node6 =
      AllOf(NodeMatches(AllOf(
                NameMatches("node_idx_1_id_6"),
                PropertyList(UnorderedElementsAre(UintIs("id", 6), StringIs("label", "Node-6"))))),
            ChildrenMatch(UnorderedElementsAre()));
  auto node1 =
      AllOf(NodeMatches(AllOf(
                NameMatches("node_idx_0_id_1"),
                PropertyList(UnorderedElementsAre(UintIs("id", 1), StringIs("label", "Node-1"))))),
            ChildrenMatch(UnorderedElementsAre(node3, node4)));
  auto node2 =
      AllOf(NodeMatches(AllOf(
                NameMatches("node_idx_1_id_2"),
                PropertyList(UnorderedElementsAre(UintIs("id", 2), StringIs("label", "Node-2"))))),
            ChildrenMatch(UnorderedElementsAre(node5, node6)));

  auto root_node =
      AllOf(NodeMatches(AllOf(
                NameMatches("semantic_tree_root"),
                PropertyList(UnorderedElementsAre(UintIs("id", 0), StringIs("label", "Node-0"))))),
            ChildrenMatch(UnorderedElementsAre(node1, node2)));

  auto tree_inspect_hierarchy = hierarchy.value().GetByPath({kInspectNodeName});
  ASSERT_NE(tree_inspect_hierarchy, nullptr);

  EXPECT_THAT(
      *tree_inspect_hierarchy,
      AllOf(NodeMatches(PropertyList(UnorderedElementsAre(UintIs("tree_update_count", 7u)))),
            ChildrenMatch(UnorderedElementsAre(root_node))));
}

TEST_F(SemanticTreeTest, GetNodeToRootTransformWithV2TransformAndContainers) {
  std::vector<a11y::SemanticTree::TreeUpdate> node_updates;

  // Create test nodes.
  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 1.0, .y = 2.0, .z = 3.0},
                                                  .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
    auto node = CreateTestNode(0u, "test_label_0", {4u});
    node.set_container_id(0u);
    node.set_node_to_container_transform({10, 0, 0, 0, 0, 10, 0, 0, 0, 0, 10, 0, 50, 60, 70, 1});
    node.set_location(std::move(bounding_box));
    node_updates.emplace_back(std::move(node));
  }

  // This node's transform will be ignored since its child specifies another
  // node as its container.
  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 3.0, .y = 4.0, .z = 5.0},
                                                  .max = {.x = 6.0, .y = 7.0, .z = 8.0}};
    auto node = CreateTestNode(4u, "test_label_4", {1u});
    node.set_node_to_container_transform({7, 0, 0, 0, 0, 7, 0, 0, 0, 0, 7, 0, 10, 10, 10, 1});
    node.set_location(std::move(bounding_box));
    node_updates.emplace_back(std::move(node));
  }

  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 5.0, .y = 6.0, .z = 7.0},
                                                  .max = {.x = 8.0, .y = 9.0, .z = 10.0}};
    auto node = CreateTestNode(1u, "test_label_1", {2u});
    node.set_node_to_container_transform({2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 1, 1, 1, 1});
    node.set_location(std::move(bounding_box));
    node.set_container_id(0u);
    node_updates.emplace_back(std::move(node));
  }

  // This node's transform will be ignored since its child specifies another
  // node as its container.
  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 15.0, .y = 16.0, .z = 17.0},
                                                  .max = {.x = 18.0, .y = 19.0, .z = 20.0}};
    auto node = CreateTestNode(2u, "test_label_2", {3u});
    node.set_node_to_container_transform({20, 0, 0, 0, 0, 20, 0, 0, 0, 0, 20, 0, 5, 10, 15, 1});
    node.set_location(std::move(bounding_box));
    node_updates.emplace_back(std::move(node));
  }

  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 2.0, .y = 3.0, .z = 4.0},
                                                  .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
    auto node = CreateTestNode(3u, "test_label_3", {});
    node.set_node_to_container_transform({5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 5, 0, 10, 20, 30, 1});
    node.set_location(std::move(bounding_box));
    node.set_container_id(1u);
    node_updates.emplace_back(std::move(node));
  }

  ASSERT_TRUE(tree_->Update(std::move(node_updates)));
  RunLoopUntilIdle();

  const auto& node_to_root_transform = tree_->GetNodeToRootTransform(3u);
  EXPECT_TRUE(node_to_root_transform.has_value());
  auto node_to_root_translation = node_to_root_transform->translation_vector();
  EXPECT_EQ(node_to_root_translation[0], 370.0f);
  EXPECT_EQ(node_to_root_translation[1], 870.0f);
  EXPECT_EQ(node_to_root_translation[2], 1590.0f);

  auto node_to_root_scale = node_to_root_transform->scale_vector();
  EXPECT_EQ(node_to_root_scale[0], 100.0f);
  EXPECT_EQ(node_to_root_scale[1], 150.0f);
}

TEST_F(SemanticTreeTest, GetNodeToRootTransformWithV2TransformNoContainers) {
  std::vector<a11y::SemanticTree::TreeUpdate> node_updates;

  // Create test nodes.
  fuchsia::ui::gfx::BoundingBox root_bounding_box = {.min = {.x = 1.0, .y = 2.0, .z = 3.0},
                                                     .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
  auto root_node = CreateTestNode(0u, "test_label_0", {1u});
  root_node.set_transform({10, 0, 0, 0, 0, 10, 0, 0, 0, 0, 10, 0, 50, 60, 70, 1});
  root_node.set_location(std::move(root_bounding_box));
  node_updates.emplace_back(std::move(root_node));

  fuchsia::ui::gfx::BoundingBox parent_bounding_box = {.min = {.x = 1.0, .y = 2.0, .z = 3.0},
                                                       .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
  auto parent_node = CreateTestNode(1u, "test_label_1", {2u});
  parent_node.set_transform({2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 1, 1, 1, 1});
  parent_node.set_location(std::move(parent_bounding_box));
  node_updates.emplace_back(std::move(parent_node));

  fuchsia::ui::gfx::BoundingBox child_bounding_box = {.min = {.x = 2.0, .y = 3.0, .z = 4.0},
                                                      .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
  auto child_node = CreateTestNode(2u, "test_label_2", {});
  child_node.set_transform({5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 5, 0, 10, 20, 30, 1});
  child_node.set_location(std::move(child_bounding_box));
  node_updates.emplace_back(std::move(child_node));

  ASSERT_TRUE(tree_->Update(std::move(node_updates)));
  RunLoopUntilIdle();

  const auto& node_to_root_transform = tree_->GetNodeToRootTransform(2u);
  EXPECT_TRUE(node_to_root_transform.has_value());
  auto node_to_root_translation = node_to_root_transform->translation_vector();
  EXPECT_EQ(node_to_root_translation[0], 260.0f);
  EXPECT_EQ(node_to_root_translation[1], 670.0f);
  EXPECT_EQ(node_to_root_translation[2], 1280.0f);

  auto node_to_root_scale = node_to_root_transform->scale_vector();
  EXPECT_EQ(node_to_root_scale[0], 100.0f);
  EXPECT_EQ(node_to_root_scale[1], 150.0f);
}

TEST_F(SemanticTreeTest, GetNodeToRootTransformWithV2TransformSelfReferentContainer) {
  std::vector<a11y::SemanticTree::TreeUpdate> node_updates;

  // Create test nodes.
  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 1.0, .y = 2.0, .z = 3.0},
                                                  .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
    auto node = CreateTestNode(0u, "test_label_0", {1u});
    node.set_node_to_container_transform({10, 0, 0, 0, 0, 10, 0, 0, 0, 0, 10, 0, 50, 60, 70, 1});
    node.set_location(std::move(bounding_box));
    node_updates.emplace_back(std::move(node));
  }

  // This node's offset container is equal to its own node id, so the loop to
  // apply transforms should stop after this node.
  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 1.0, .y = 2.0, .z = 3.0},
                                                  .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
    auto node = CreateTestNode(1u, "test_label_1", {2u});
    node.set_container_id(1u);
    node.set_node_to_container_transform({7, 0, 0, 0, 0, 8, 0, 0, 0, 0, 9, 0, 10, 10, 10, 1});
    node.set_location(std::move(bounding_box));
    node_updates.emplace_back(std::move(node));
  }

  {
    fuchsia::ui::gfx::BoundingBox bounding_box = {.min = {.x = 1.0, .y = 2.0, .z = 3.0},
                                                  .max = {.x = 4.0, .y = 5.0, .z = 6.0}};
    auto node = CreateTestNode(2u, "test_label_2");
    node.set_container_id(1u);
    node.set_node_to_container_transform({1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 100, 100, 100, 1});
    node.set_location(std::move(bounding_box));
    node_updates.emplace_back(std::move(node));
  }

  ASSERT_TRUE(tree_->Update(std::move(node_updates)));
  RunLoopUntilIdle();

  const auto& node_to_root_transform = tree_->GetNodeToRootTransform(2u);
  const auto& node_to_root_translation = node_to_root_transform->translation_vector();
  EXPECT_EQ(node_to_root_translation[0], 717.0f);
  EXPECT_EQ(node_to_root_translation[1], 826.0f);
  EXPECT_EQ(node_to_root_translation[2], 937.0f);

  const auto& node_to_root_scale = node_to_root_transform->scale_vector();
  EXPECT_EQ(node_to_root_scale[0], 7.0f);
  EXPECT_EQ(node_to_root_scale[1], 16.0f);
}

}  // namespace
}  // namespace accessibility_test
