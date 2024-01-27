// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/cpp/task.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include <memory>

#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/testing/ui_test_manager/ui_test_manager.h"
#include "src/ui/testing/util/flatland_test_view.h"
#include "src/ui/testing/util/gfx_test_view.h"

namespace integration_tests {

namespace {

using component_testing::ChildRef;
using component_testing::LocalComponent;
using component_testing::LocalComponentHandles;
using component_testing::ParentRef;
using component_testing::Protocol;
using component_testing::Realm;
using component_testing::Route;

constexpr auto kViewProvider = "view-provider";

std::vector<ui_testing::UITestRealm::Config> UIConfigurationsToTest() {
  std::vector<ui_testing::UITestRealm::Config> configs;

  // GFX x scene manager
  {
    ui_testing::UITestRealm::Config config;
    config.scene_owner = ui_testing::UITestRealm::SceneOwnerType::SCENE_MANAGER;
    config.ui_to_client_services = {fuchsia::ui::scenic::Scenic::Name_};
    configs.push_back(std::move(config));
  }

  // Flatland x scene manager
  {
    ui_testing::UITestRealm::Config config;
    config.use_flatland = true;
    config.scene_owner = ui_testing::UITestRealm::SceneOwnerType::SCENE_MANAGER;
    config.ui_to_client_services = {fuchsia::ui::composition::Flatland::Name_,
                                    fuchsia::ui::composition::Allocator::Name_};
    configs.push_back(std::move(config));
  }
  return configs;
}

}  // namespace

// This test verifies that the scene owner correctly connects the scene graph to
// the display so that pixels render, and enforces the expected presentation
// semantics.
class PresentationTest : public gtest::RealLoopFixture,
                         public ::testing::WithParamInterface<ui_testing::UITestRealm::Config> {
 protected:
  // |testing::Test|
  void SetUp() override {
    auto config = GetParam();
    ui_test_manager_.emplace(config);

    // Build realm.
    FX_LOGS(INFO) << "Building realm";
    realm_ = ui_test_manager_->AddSubrealm();

    test_view_access_ = std::make_shared<ui_testing::TestViewAccess>();

    component_testing::LocalComponentFactory test_view;
    // Add a test view provider. Make either a gfx test view or flatland test view depending
    // on the config parameters.
    if (config.use_flatland) {
      test_view = [d = dispatcher(), a = test_view_access_]() {
        return std::make_unique<ui_testing::FlatlandTestView>(
            d, /* content = */ ui_testing::TestView::ContentType::COORDINATE_GRID, a);
      };
    } else {
      test_view = [d = dispatcher(), a = test_view_access_]() {
        return std::make_unique<ui_testing::GfxTestView>(
            d, /* content = */ ui_testing::TestView::ContentType::COORDINATE_GRID, a);
      };
    }

    realm_->AddLocalChild(kViewProvider, std::move(test_view));
    realm_->AddRoute(Route{.capabilities = {Protocol{fuchsia::ui::app::ViewProvider::Name_}},
                           .source = ChildRef{kViewProvider},
                           .targets = {ParentRef()}});

    for (const auto& protocol : config.ui_to_client_services) {
      realm_->AddRoute(Route{.capabilities = {Protocol{protocol}},
                             .source = ParentRef(),
                             .targets = {ChildRef{kViewProvider}}});
    }

    ui_test_manager_->BuildRealm();
    realm_exposed_services_ = ui_test_manager_->CloneExposedServicesDirectory();

    // Attach view, and wait for it to render.
    ui_test_manager_->InitializeScene();
    RunLoopUntil([this]() { return ui_test_manager_->ClientViewIsRendering(); });
  }

  void TearDown() override {
    bool complete = false;
    ui_test_manager_->TeardownRealm(
        [&](fit::result<fuchsia::component::Error> result) { complete = true; });
    RunLoopUntil([&]() { return complete; });
  }

  ui_testing::Screenshot TakeScreenshot() { return ui_test_manager_->TakeScreenshot(); }

  std::optional<ui_testing::UITestManager> ui_test_manager_;
  std::unique_ptr<sys::ServiceDirectory> realm_exposed_services_;
  std::shared_ptr<ui_testing::TestViewAccess> test_view_access_;
  std::optional<Realm> realm_;
};

INSTANTIATE_TEST_SUITE_P(PresentationTestWithParams, PresentationTest,
                         ::testing::ValuesIn(UIConfigurationsToTest()));

TEST_P(PresentationTest, RenderCoordinateGridPattern) {
  auto data = TakeScreenshot();

  EXPECT_EQ(data.GetPixelAt(data.width() / 4, data.height() / 4), ui_testing::Screenshot::kBlack);
  EXPECT_EQ(data.GetPixelAt(data.width() / 4, 3 * data.height() / 4),
            ui_testing::Screenshot::kBlue);
  EXPECT_EQ(data.GetPixelAt(3 * data.width() / 4, data.height() / 4), ui_testing::Screenshot::kRed);
  EXPECT_EQ(data.GetPixelAt(3 * data.width() / 4, 3 * data.height() / 4),
            ui_testing::Screenshot::kMagenta);
  EXPECT_EQ(data.GetPixelAt(data.width() / 2, data.height() / 2), ui_testing::Screenshot::kGreen);
}

}  // namespace integration_tests
