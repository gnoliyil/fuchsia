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
#include <vector>

#include <gtest/gtest.h>
#include <test/accessibility/cpp/fidl.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/testing/ui_test_manager/ui_test_manager.h"
#include "src/ui/testing/util/flatland_test_view.h"
#include "src/ui/testing/util/gfx_test_view.h"

namespace integration_tests {
namespace {

constexpr auto kViewProvider = "view-provider";
constexpr float kEpsilon = 0.005f;

std::vector<ui_testing::UITestRealm::Config> UIConfigurationsToTest(
    const std::vector<float>& pixel_densities) {
  std::vector<ui_testing::UITestRealm::Config> configs;
  std::vector<std::string> protocols_required = {fuchsia::ui::scenic::Scenic::Name_};

  // GFX x root presenter
  {
    ui_testing::UITestRealm::Config config;
    config.scene_owner = ui_testing::UITestRealm::SceneOwnerType::ROOT_PRESENTER;
    config.ui_to_client_services = protocols_required;
    for (auto dpr : pixel_densities) {
      config.device_pixel_ratio = dpr;
      configs.push_back(config);
    }
  }

  // GFX x scene manager
  {
    ui_testing::UITestRealm::Config config;
    config.scene_owner = ui_testing::UITestRealm::SceneOwnerType::SCENE_MANAGER;
    config.ui_to_client_services = protocols_required;
    for (auto dpr : pixel_densities) {
      config.device_pixel_ratio = dpr;
      configs.push_back(config);
    }
  }

  // Flatland x scene manager
  {
    ui_testing::UITestRealm::Config config;
    config.use_flatland = true;
    config.scene_owner = ui_testing::UITestRealm::SceneOwnerType::SCENE_MANAGER;
    config.ui_to_client_services = protocols_required;
    config.ui_to_client_services.push_back(fuchsia::ui::composition::Flatland::Name_);
    for (auto dpr : pixel_densities) {
      config.device_pixel_ratio = dpr;
      configs.push_back(config);
    }
  }
  return configs;
}

}  // namespace

using component_testing::ChildRef;
using component_testing::Directory;
using component_testing::ParentRef;
using component_testing::Protocol;
using component_testing::Realm;
using component_testing::Route;

// This test verifies that Root Presenter and Scene Manager propagate
// 'config/data/device_pixel_ratio' correctly.
class DisplayPixelRatioTest
    : public gtest::RealLoopFixture,
      public ::testing::WithParamInterface<ui_testing::UITestRealm::Config> {
 public:
  static std::vector<float> GetDevicePixelRatiosToTest() {
    std::vector<float> pixel_density;
    pixel_density.emplace_back(ui_testing::kDefaultDevicePixelRatio);
    pixel_density.emplace_back(ui_testing::kMediumResolutionDevicePixelRatio);
    pixel_density.emplace_back(ui_testing::kHighResolutionDevicePixelRatio);
    return pixel_density;
  }

 protected:
  // |testing::Test|
  void SetUp() override {
    auto config = GetParam();
    ui_test_manager_ = std::make_unique<ui_testing::UITestManager>(config);

    // Build realm.
    FX_LOGS(INFO) << "Building realm";
    realm_ = std::make_unique<Realm>(ui_test_manager_->AddSubrealm());

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

    // Get display's width and height.
    auto [width, height] = ui_test_manager_->GetDisplayDimensions();

    display_width_ = static_cast<double>(width);
    display_height_ = static_cast<double>(height);
    FX_LOGS(INFO) << "Got display_width = " << display_width_
                  << " and display_height = " << display_height_;
  }

  float ClientViewScaleFactor() { return ui_test_manager_->ClientViewScaleFactor(); }

  ui_testing::Screenshot TakeScreenshot() { return ui_test_manager_->TakeScreenshot(); }

  std::shared_ptr<ui_testing::TestViewAccess> test_view_access_;

  double display_width_ = 0;
  double display_height_ = 0;

 private:
  std::unique_ptr<ui_testing::UITestManager> ui_test_manager_;

  std::unique_ptr<sys::ServiceDirectory> realm_exposed_services_;
  std::unique_ptr<Realm> realm_;
};

INSTANTIATE_TEST_SUITE_P(DisplayPixelRatioTestWithParams, DisplayPixelRatioTest,
                         ::testing::ValuesIn(UIConfigurationsToTest(
                             DisplayPixelRatioTest::GetDevicePixelRatiosToTest())));

// This test leverage the coordinate test view to ensure that display pixel ratio is working
// properly.
// ___________________________________
// |                |                |
// |     BLACK      |        BLUE    |
// |           _____|_____           |
// |___________|  GREEN  |___________|
// |           |_________|           |
// |                |                |
// |      RED       |     MAGENTA    |
// |________________|________________|
TEST_P(DisplayPixelRatioTest, TestScale) {
  auto config = GetParam();
  const auto expected_dpr = config.device_pixel_ratio;

  // TODO(fxbug.dev/112999): Only run this check on GFX for now until we update ClientScaleFactor()
  // to work with Flatland.
  if (!config.use_flatland) {
    EXPECT_NEAR(ClientViewScaleFactor(), expected_dpr, kEpsilon);
  }

  EXPECT_NEAR(display_width_ / test_view_access_->view()->width(), expected_dpr, kEpsilon);
  EXPECT_NEAR(display_height_ / test_view_access_->view()->height(), expected_dpr, kEpsilon);

  // The drawn content should cover the screen's display.
  auto data = TakeScreenshot();

  // Check pixel content at all four corners.
  EXPECT_EQ(data.GetPixelAt(0, 0), ui_testing::Screenshot::kBlack);                 // Top left
  EXPECT_EQ(data.GetPixelAt(0, data.height() - 1), ui_testing::Screenshot::kBlue);  // Bottom left
  EXPECT_EQ(data.GetPixelAt(data.width() - 1, 0), ui_testing::Screenshot::kRed);    // Top right
  EXPECT_EQ(data.GetPixelAt(data.width() - 1, data.height() - 1),
            ui_testing::Screenshot::kMagenta);  // Bottom right

  // Check pixel content at center of each rectangle.
  EXPECT_EQ(data.GetPixelAt(data.width() / 4, data.height() / 4),
            ui_testing::Screenshot::kBlack);  // Top left
  EXPECT_EQ(data.GetPixelAt(data.width() / 4, (3 * data.height()) / 4),
            ui_testing::Screenshot::kBlue);  // Bottom left
  EXPECT_EQ(data.GetPixelAt((3 * data.width()) / 4, data.height() / 4),
            ui_testing::Screenshot::kRed);  // Top right
  EXPECT_EQ(data.GetPixelAt((3 * data.width()) / 4, (3 * data.height()) / 4),
            ui_testing::Screenshot::kMagenta);  // Bottom right
  EXPECT_EQ(data.GetPixelAt(data.width() / 2, data.height() / 2),
            ui_testing::Screenshot::kGreen);  // Center
}

TEST_P(DisplayPixelRatioTest, TestPixelColorDistribution) {
  auto data = TakeScreenshot();

  // Width and height of the rectangle in the center is |display_width_|/4 and |display_height_|/4.
  const auto expected_green_pixels = (display_height_ / 4) * (display_width_ / 4);

  // The number of pixels inside each quadrant would be |display_width_|/2 * |display_height_|/2.
  // Since the central rectangle would cover equal areas in each quadrant, we subtract
  // |expected_green_pixels|/4 from each quadrants to get the pixel for the quadrant's color.
  const auto expected_black_pixels =
      (display_height_ / 2) * (display_width_ / 2) - (expected_green_pixels / 4);

  const auto expected_blue_pixels = expected_black_pixels,
             expected_red_pixels = expected_black_pixels,
             expected_magenta_pixels = expected_black_pixels;

  auto histogram = data.Histogram();

  EXPECT_EQ(histogram[ui_testing::Screenshot::kBlack], expected_black_pixels);
  EXPECT_EQ(histogram[ui_testing::Screenshot::kBlue], expected_blue_pixels);
  EXPECT_EQ(histogram[ui_testing::Screenshot::kRed], expected_red_pixels);
  EXPECT_EQ(histogram[ui_testing::Screenshot::kMagenta], expected_magenta_pixels);
  EXPECT_EQ(histogram[ui_testing::Screenshot::kGreen], expected_green_pixels);
}

}  // namespace integration_tests
