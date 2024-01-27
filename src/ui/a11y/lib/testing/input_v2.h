// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_TESTING_INPUT_V2_H_
#define SRC_UI_A11Y_LIB_TESTING_INPUT_V2_H_

#include <fuchsia/math/cpp/fidl.h>
#include <fuchsia/ui/pointer/augment/cpp/fidl.h>

#include <vector>

#include <glm/glm.hpp>

namespace accessibility_test::input_v2 {

using PointerId = decltype(fuchsia::ui::pointer::TouchInteractionId::pointer_id);

struct PointerParams {
  PointerParams(PointerId pointer_id, fuchsia::ui::pointer::EventPhase phase,
                const glm::vec2& coordinate);

  PointerId pointer_id;
  fuchsia::ui::pointer::EventPhase phase;
  glm::vec2 coordinate;
};

// The following could actually all be std::arrays and templates, but that may be overkill.

template <typename T>
std::vector<T> operator+(const std::vector<T>& a, const std::vector<T>& b) {
  std::vector<T> cat;
  cat.reserve(a.size() + b.size());
  cat.insert(cat.end(), a.begin(), a.end());
  cat.insert(cat.end(), b.begin(), b.end());
  return cat;
}

template <typename T>
std::vector<T> operator*(size_t n, const std::vector<T>& v) {
  std::vector<T> mul;
  mul.reserve(n * v.size());
  for (size_t i = 0; i < n; ++i) {
    mul.insert(mul.end(), v.begin(), v.end());
  }
  return mul;
}

template <typename T = PointerParams>  // Need to default because type inference can't reach into
                                       // nested arg type.
std::vector<T> Zip(const std::vector<std::vector<T>>& vv) {
  std::vector<T> acc;
  size_t size = 0;
  for (const auto& v : vv) {
    size += v.size();
  }

  acc.reserve(size);
  for (size_t i = 0; acc.size() < size; ++i) {
    for (const auto& v : vv) {
      if (i < v.size()) {
        acc.push_back(v[i]);
      }
    }
  }

  return acc;
}

constexpr size_t kDefaultMoves = 10;

// Some tests require us to verify local coordinates (e.g. in a GestureContext). To avoid
// verbosity, rather than taking local coordinates as additional arguments to the test
// methods below, we will simply apply a trivial scaling transform to ndc coordinates
// to obtain local coordinates.
constexpr float kTestNDCToLocalMultiplier = 50;

std::vector<PointerParams> DownEvents(PointerId pointer_id, const glm::vec2& coordinate);
std::vector<PointerParams> UpEvents(PointerId pointer_id, const glm::vec2& coordinate);
std::vector<PointerParams> TapEvents(PointerId pointer_id, const glm::vec2& coordinate);

// Pointer move events between two endpoints, (start, end]. The start point is exclusive and the end
// point is inclusive, as move events signify where a pointer has moved to rather than where it has
// moved from.
std::vector<PointerParams> MoveEvents(PointerId pointer_id, const glm::vec2& start,
                                      const glm::vec2& end, size_t moves = kDefaultMoves);

std::vector<PointerParams> DragEvents(PointerId pointer_id, const glm::vec2& start,
                                      const glm::vec2& end, size_t moves = kDefaultMoves);

fuchsia::ui::pointer::augment::TouchEventWithLocalHit ToTouchEvent(const PointerParams& params,
                                                                   uint64_t event_time,
                                                                   zx_koid_t koid = 0);

::fuchsia::math::PointF ToLocalCoordinates(const glm::vec2& ndc);

}  // namespace accessibility_test::input_v2

#endif  // SRC_UI_A11Y_LIB_TESTING_INPUT_V2_H_
