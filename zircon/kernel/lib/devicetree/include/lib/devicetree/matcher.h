// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_MATCHER_H_
#define ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_MATCHER_H_

#include "devicetree.h"
#include "internal/matcher.h"

namespace devicetree {

// The Matcher API uses the following terms:
//
//  * |walk| : Refers to |Devicetree::Walk| operation, which consists on visiting all nodes on the
//  tree.
//
//  * |scan| : Refers to inspecting nodes in the devicetree during a |walk|, to retrieve interesting
//  information for all registered matchers.
//
//  * |matcher| : Entity collecting information through 1 or more |scans|. Each |scan| may collect a
//  piece of the information.
//
//  * |registered matcher| : Matcher who has not yet reached completion, and will continue to be
//  part of the scan preocess.
//
// A |Matcher| type object must follow the following compile time contract:
// struct Matcher {
//   // Maximum number of full tree scans needed to collect all required information.
//   //
//   // A |Matcher| must reach |kDone| state before the number of scans exceeds
//   // |kMaxScans|.
//   static constexpr size_t kMaxScans = 1;
//
//   // During a tree scan, |Matcher::OnNode| is called for each node in the tree,
//   // as long as the matcher has not reached |kDone| state or |kIgnoreSubtree| on
//   // parent node or |kNeedsAliases|.
//   ScanState OnNode(NodePath& path, PropertyDecoder& decoder);
//
//   // When multiple tree scans are performed, |Matcher::OnWalk| is called
//   // at the end of each walk, meaning all nodes of the tree have at least all
//   // nodes that this matcher has showed interest on have been visited.
//   ScanState OnWalk();
//
//   // Called whenever an error happens.
//   void OnError(std::string_view error);
// };
//
//
// Usage example:
// // Assume |dt| is a |devicetree::Devicetree| object.
//
// struct FooMatcher {
//   constexpr size_t kMaxScans = 1;
//   ScanState OnNode(const NodePath& path, const PropertyDecoder& decoder) {
//     if (path.back() == "foo") {
//        foo_count++;
//        return ScanState::kActive;
//     }
//   }
//
//   ScanState OnWalk() {
//     return ScanState::kDone;
//   }
//
//   void OnError(std::string_view err) {
//     std::cout << " Foo Matcher had an error: " << err << std::endl;
//   }
//
//   int foo_count = 0;
//  }
//
//  ...
//
//  FooMatcher foo_matcher;
//  if (!devicetree::Match(dt, foo_matcher)) {
//    return;
//  }
//  std::cout << " Nodes names foo: " << foo_matcher.foo_count << std::endl;
//

enum class ScanState {
  // Matcher has finished collecting information, no more scans are needed.
  kDone,

  // Matcher cannot do further progress in the current path.
  kDoneWithSubtree,

  // Matcher needs nodes in the current path, so it wishes to visit offspring.
  kActive,

  // Matcher cannot make further progress until the aliases node is resolved.
  kNeedsPathResolution,
};

template <typename... Matchers>
constexpr bool Match(const devicetree::Devicetree& devicetree, Matchers&&... matchers) {
  static_assert((internal::CheckInterface<std::decay_t<Matchers>>() && ...));
  using internal::ForEachMatcher;
  using internal::MatcherVisit;
  constexpr size_t kMaxScanForMatchers = std::max({internal::GetMaxScans<Matchers>()...});
  std::array<MatcherVisit<>, sizeof...(Matchers)> visit_state;

  // Call |OnNode| on all matchers that are not done or avoiding the subtree.
  auto visit_and_prune = [&visit_state, &matchers...](const NodePath& path,
                                                      const PropertyDecoder& decoder) {
    auto on_each_matcher = [&visit_state, &path, &decoder](auto& matcher, size_t index) {
      auto& matcher_state = visit_state[index];
      if (matcher_state.state() == ScanState::kActive ||
          matcher_state.state() == ScanState::kNeedsPathResolution) {
        matcher_state.set_state(matcher.OnNode(path, decoder));
        if (matcher_state.state() == ScanState::kDoneWithSubtree) {
          matcher_state.Prune(path);
        }
      }
    };
    ForEachMatcher(on_each_matcher, matchers...);
    // Return whether we still need to visit any node in the underlying subtree.
    return std::any_of(visit_state.begin(), visit_state.end(),
                       [](auto& visit_state) { return visit_state.state() == ScanState::kActive; });
  };

  // Unprune any pruned Node, as a post order visitor.
  auto unprune = [&visit_state, &matchers...](const NodePath& path,
                                              const PropertyDecoder& decoder) {
    ForEachMatcher(
        [&visit_state, &path](auto& matcher, size_t index) { visit_state[index].Unprune(path); },
        matchers...);
  };

  // Call OnWalk on ever matcher
  auto on_walk = [](auto& visit_state, auto&... matchers) {
    ForEachMatcher(
        [&visit_state](auto& matcher, size_t index) {
          if (visit_state[index].state() != ScanState::kDone) {
            visit_state[index].set_state(matcher.OnWalk());
          }
        },
        matchers...);
  };

  // Verify that matchers fulfill their scan contract, that is every matcher visit state must be
  // |kDone| after finishing the current devicetree scan. Return value:
  enum class ScanResult {
    kMatchersDone,
    kMatchersPending,
    kMatchersWithError,
  };

  auto all_matchers_done = [&visit_state](size_t current_scan, auto&... matchers) {
    int error_count = 0;
    int finished_count = 0;
    ForEachMatcher(
        [&error_count, &finished_count, &visit_state, current_scan](auto& matcher, size_t index) {
          using MatcherType = std::decay_t<decltype(matcher)>;
          if (visit_state[index].state() != ScanState::kDone) {
            if (current_scan >= (MatcherType::kMaxScans - 1)) {
              matcher.OnError("Matcher failed to reach completion the requested scan number.");
              error_count++;
            }
            return;
          }
          finished_count++;
        },
        matchers...);

    if (finished_count == sizeof...(matchers)) {
      return ScanResult::kMatchersDone;
    }

    if (error_count == 0) {
      return ScanResult::kMatchersPending;
    }
    return ScanResult::kMatchersWithError;
  };

  for (size_t i = 0; i < kMaxScanForMatchers; ++i) {
    devicetree.Walk(visit_and_prune, unprune);
    on_walk(visit_state, matchers...);

    // If result == 1 then no errors found, but not all matchers are done.
    if (auto res = all_matchers_done(i, matchers...); res != ScanResult::kMatchersPending) {
      return res == ScanResult::kMatchersDone;
    }
  }

  return true;
}

}  // namespace devicetree

#endif  // ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_MATCHER_H_
