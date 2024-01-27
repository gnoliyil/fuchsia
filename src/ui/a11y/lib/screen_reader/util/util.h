// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_UTIL_UTIL_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_UTIL_UTIL_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>

#include <cstdint>
#include <set>

#include "src/ui/a11y/lib/semantics/semantics_source.h"

namespace a11y {

bool NodeIsDescribable(const fuchsia::accessibility::semantics::Node* node);

// Converts floating point to a string and strips trailing zeros.
std::string FormatFloat(float input);

// Returns true if the given node's parent 'contains all the same information'.
// That is, returns true iff `parent_node`
// - is not null,
// - has a label, and
// - has the same label as `node`, and
// - is describable (in particular, it's not a container), and
// - has no other children, and
// - has a set of actions that is a superset of `node`'s.
bool SameInformationAsParent(const fuchsia::accessibility::semantics::Node* node,
                             const fuchsia::accessibility::semantics::Node* parent_node);

// Returns a list of all container IDs that are ancestors of the given node,
// sorted 'deepest-last'. Will not include the node itself.
std::vector<const fuchsia::accessibility::semantics::Node*> GetContainerNodes(
    zx_koid_t koid, uint32_t node_id, SemanticsSource* semantics_source);

// Returns true if the node represents a slider.
bool NodeIsSlider(const fuchsia::accessibility::semantics::Node* node);

// Get the string representation of a slider's value. Some sliders use the
// range_value field to store a float value, while others use the value field
// to store a string representation. We prefer range_value, but if it's not
// present, we fall back to value.
std::string GetSliderValue(const fuchsia::accessibility::semantics::Node& node);

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_UTIL_UTIL_H_
