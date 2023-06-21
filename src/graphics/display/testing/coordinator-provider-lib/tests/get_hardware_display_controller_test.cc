// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fpromise/promise.h>
#include <lib/fpromise/single_threaded_executor.h>
#include <lib/sys/cpp/component_context.h>

#include <gtest/gtest.h>

#include "src/graphics/display/testing/coordinator-provider-lib/client-hlcpp.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace display {

namespace {

struct fake_context : fpromise::context {
  fpromise::executor* executor() const override { return nullptr; }
  fpromise::suspended_task suspend_task() override { return fpromise::suspended_task(); }
};

class GetHardwareDisplayCoordinatorWithoutProviderServiceTest : public gtest::RealLoopFixture {};

TEST_F(GetHardwareDisplayCoordinatorWithoutProviderServiceTest, FailedOnNoProviderService) {
  auto promise = GetCoordinatorHlcpp();
  fake_context context;
  EXPECT_TRUE(promise(context).is_error());
}

}  // namespace

}  // namespace display
