// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_UBER_STRUCT_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_UBER_STRUCT_H_

#include <fuchsia/ui/composition/cpp/fidl.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "src/ui/scenic/lib/allocation/buffer_collection_importer.h"
#include "src/ui/scenic/lib/flatland/flatland_types.h"
#include "src/ui/scenic/lib/flatland/transform_graph.h"
#include "src/ui/scenic/lib/flatland/transform_handle.h"

#include <glm/glm.hpp>
#include <glm/mat3x3.hpp>

namespace flatland {

// The sample region to use for an image when texturing a rectangle.
using ImageSampleRegion = fuchsia::math::RectF;

// The clip region for a transform to bound its children.
using TransformClipRegion = fuchsia::math::Rect;

// TODO(fxbug.dev/45932): find the appropriate name for this struct.
//
// A collection of data local to a particular Flatland instance representing the most recent commit
// of that instance's presented state. Because the UberStruct represents a snapshot of the local
// state of a Flatland instance, it must be stateless. It should contain only data and no
// references to external resources.
struct UberStruct {
  using InstanceMap = std::unordered_map<TransformHandle::InstanceId, std::shared_ptr<UberStruct>>;

  // The local topology of this Flatland instance.
  TransformGraph::TopologyVector local_topology;

  // The local (i.e. relative to the parent) geometric transformation matrix of each
  // TransformHandle. Handles with no entry indicate an identity matrix.
  std::unordered_map<TransformHandle, glm::mat3> local_matrices;

  // The local (i.e. relative to the parent) opacity values of each TransformHandles. Handles
  // with no entry indicate an opacity value of 1.0.
  std::unordered_map<TransformHandle, float> local_opacity_values;

  // Map of the regions of images used to texture renderables. These are set per-image.
  std::unordered_map<TransformHandle, ImageSampleRegion> local_image_sample_regions;

  // Map of the regions of transforms that clip child content.
  std::unordered_map<TransformHandle, TransformClipRegion> local_clip_regions;

  // The images associated with each TransformHandle.
  std::unordered_map<TransformHandle, allocation::ImageMetadata> images;

  // Map of local hit regions.
  std::unordered_map<TransformHandle, std::vector<flatland::HitRegion>> local_hit_regions_map;

  // The ViewRef for the root (View) of this Flatland instance.
  // Can be nullptr when not attached to the scene, otherwise must be set.
  std::shared_ptr<const fuchsia::ui::views::ViewRef> view_ref = nullptr;

  // Set from SetDebugName(). Can be empty if the client does not calls SetDebugName().
  std::string debug_name;
};

}  // namespace flatland

namespace std {
ostream& operator<<(ostream& out, const flatland::UberStruct& us);
}  // namespace std

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_UBER_STRUCT_H_
