// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_TESTS_MOCK_FLATLAND_PRESENTER_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_TESTS_MOCK_FLATLAND_PRESENTER_H_

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/ui/scenic/lib/flatland/flatland.h"
#include "src/ui/scenic/lib/flatland/flatland_presenter.h"
#include "src/ui/scenic/lib/flatland/uber_struct_system.h"

namespace flatland {

class MockFlatlandPresenter : public FlatlandPresenter {
 public:
  MOCK_METHOD(void, ScheduleUpdateForSession,
              (zx::time requested_presentation_time, scheduling::SchedulingIdPair id_pair,
               bool squashable, std::vector<zx::event> release_fences));
  MOCK_METHOD(std::vector<scheduling::FuturePresentationInfo>, GetFuturePresentationInfos, ());
  MOCK_METHOD(void, RemoveSession,
              (scheduling::SessionId session_id, std::optional<zx::event> release_fence));
};

}  // namespace flatland

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_TESTS_MOCK_FLATLAND_PRESENTER_H_
