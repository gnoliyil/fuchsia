// Copyright 2022 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_PATH_H_
#define ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_PATH_H_

#include <lib/devicetree/devicetree.h>
#include <lib/fit/result.h>

#include <optional>
#include <string_view>
#include <utility>

namespace devicetree {

namespace internal {

// Compares to ranges described by [start_1, end_1) and [start_2, end_2).
// Returns an iterator to the first mismatching element in the ranges.
// Given a result <ret_1, ret_2>:
//     ret_1 == end_1 && ret_2 == end_2 implies ranges are equal.
//     ret_1 == end_1 && ret_2 != end_2 implies [start_1, end_1) is a subrange of [start_2, end_2).
//     ret_1 != end_1 && ret_2 == end_2 implies [start_2, end_2) is a subrange of [start_1, end_1).
//     ret_1 != end_1 && ret_2 != end_2 implies that both ranges share a common prefix
//                                      [start_1, ret_1) and [start_2, ret_2]. Where ret_1 and
//                                      ret_2 point to the first mismatching element.
template <typename Iter, typename Iter2, typename Pred>
std::pair<Iter, Iter2> CompareRanges(Iter start_1, Iter end_1, Iter2 start_2, Iter2 end_2,
                                     Pred compare) {
  while (start_1 != end_1 && start_2 != end_2) {
    if (!compare(*start_1, *start_2)) {
      break;
    }
    ++start_1;
    ++start_2;
  }
  return {start_1, start_2};
}

// Compares to ranges described by [start_1, end_1) and [start_2, end_2).
// Returns an iterator to the first mismatching element in the ranges.
// Given a result <ret_1, ret_2>:
//     ret_1 == end_1 && ret_2 == end_2 implies ranges are equal.
//     ret_1 == end_1 && ret_2 != end_2 implies [start_1, end_1) is a subrange of [start_2, end_2).
//     ret_1 != end_1 && ret_2 == end_2 implies [start_2, end_2) is a subrange of [start_1, end_1).
//     ret_1 != end_1 && ret_2 != end_2 implies that both ranges share a common prefix
//                                      [start_1, ret_1) and [start_2, ret_2]. Where ret_1 and
//                                      ret_2 point to the first mismatching element.
template <typename Iter, typename Iter2>
auto CompareRangesOfNodes(Iter start_1, Iter end_1, Iter2 start_2, Iter2 end_2) {
  return CompareRanges(start_1, end_1, start_2, end_2,
                       [](const auto& a, const auto& b) { return a == b; });
}

}  // namespace internal

// Represents the result of comparing |path_a| to |path_b|.
// Method meant to be read |path_a| |MethodName| of |path_b|.
// E.g.
//   path_a: '/root'
//   path_b: '/root/a'
//
//   path_a IsNode path_b: false
//   path_a IsAncestor path_b: true
//   path_a IsDescendant path_b: false
enum class CompareResult {
  // |path_a| references the same node as |path_b|.
  kIsMatch,

  // |path_a| references a node that is an ancestor of |path_b|.
  kIsAncestor,

  // |path_a| references a node that is a descendant of |path_b|.
  kIsDescendant,

  // the paths references nodes that are not in each other path's. (!IsAncestor() &&
  // !IsDescendant() && !IsNode()).
  // E.g.
  //   path_a: '/A/B/C'
  //   path_b : '/A/D'
  // This is a mismatch, since the paths to the nodes diverge on '/A', with '/B' and '/D' being the
  // mismatching elements.
  kIsMismatch,

};

// Compares |path_a| with |path_b|. |path_b| is a resolved path where the absolute
// stem and relative stem have been computed.
CompareResult ComparePath(const NodePath& path_a, const ResolvedPath& path_b);

// Compares |path_a| with |path_b|. |absolute_path_b| must be an absolute path.
CompareResult ComparePath(const NodePath& path_a, std::string_view absolute_path_b);

}  // namespace devicetree

#endif  // ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_PATH_H_
