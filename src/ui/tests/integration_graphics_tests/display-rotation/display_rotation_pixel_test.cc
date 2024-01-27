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
#include "src/ui/testing/ui_test_realm/ui_test_realm.h"
#include "src/ui/testing/util/flatland_test_view.h"
#include "src/ui/testing/util/gfx_test_view.h"

namespace integration_tests {
namespace {

constexpr auto kViewProvider = "view-provider";

std::vector<ui_testing::UITestRealm::Config> UIConfigurationsToTest(
    std::vector<int> display_rotations) {
  std::vector<ui_testing::UITestRealm::Config> configs;

  // GFX x scene manager
  {
    ui_testing::UITestRealm::Config config;
    config.use_scene_owner = true;
    config.device_pixel_ratio = ui_testing::kDefaultDevicePixelRatio;
    config.ui_to_client_services = {fuchsia::ui::scenic::Scenic::Name_};
    for (const auto rotation : display_rotations) {
      config.display_rotation = rotation;
      configs.push_back(config);
    }
  }

  // Flatland x scene manager
  {
    ui_testing::UITestRealm::Config config;
    config.use_flatland = true;
    config.use_scene_owner = true;
    config.accessibility_owner = ui_testing::UITestRealm::AccessibilityOwnerType::FAKE;
    config.device_pixel_ratio = ui_testing::kDefaultDevicePixelRatio;
    config.ui_to_client_services = {fuchsia::ui::composition::Flatland::Name_,
                                    fuchsia::ui::composition::Allocator::Name_};
    for (const auto rotation : display_rotations) {
      config.display_rotation = rotation;
      configs.push_back(config);
    }
  }
  return configs;
}

}  // namespace

using component_testing::ChildRef;
using component_testing::ParentRef;
using component_testing::Protocol;
using component_testing::Realm;
using component_testing::Route;

// This test verifies that Scene Manager propagates
// 'config/data/display_rotation' correctly.
class DisplayRotationPixelTestBase : public gtest::RealLoopFixture {
 protected:
  explicit DisplayRotationPixelTestBase(ui_testing::UITestRealm::Config config)
      : config_(std::move(config)) {}

  // |testing::Test|
  void SetUp() override {
    ui_test_manager_.emplace(config_);

    // Build realm.
    FX_LOGS(INFO) << "Building realm";
    realm_ = ui_test_manager_->AddSubrealm();

    test_view_access_ = std::make_shared<ui_testing::TestViewAccess>();

    component_testing::LocalComponentFactory test_view;
    // Add a test view provider. Make either a gfx test view or flatland test view depending
    // on the config parameters.
    if (config_.use_flatland) {
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

    for (const auto& protocol : config_.ui_to_client_services) {
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

    display_width_ = width;
    display_height_ = height;
    FX_LOGS(INFO) << "Got display_width = " << display_width_
                  << " and display_height = " << display_height_;
  }

  void TearDown() override {
    bool complete = false;
    ui_test_manager_->TeardownRealm(
        [&](fit::result<fuchsia::component::Error> result) { complete = true; });
    RunLoopUntil([&]() { return complete; });
  }

  ui_testing::Screenshot TakeScreenshot() { return ui_test_manager_->TakeScreenshot(); }

  // Validates that the content present in |screenshot| matches the content of
  // |ui_testing::TestView::ContentType::COORDINATE_GRID|.
  static void AssertScreenshot(const ui_testing::Screenshot& screenshot, uint64_t width,
                               uint64_t height) {
    // Check pixel content at all four corners.
    EXPECT_EQ(screenshot.GetPixelAt(0, 0), ui_testing::Screenshot::kBlack);  // Top left
    EXPECT_EQ(screenshot.GetPixelAt(0, screenshot.height() - 1),
              ui_testing::Screenshot::kBlue);  // Bottom left
    EXPECT_EQ(screenshot.GetPixelAt(screenshot.width() - 1, 0),
              ui_testing::Screenshot::kRed);  // Top right
    EXPECT_EQ(screenshot.GetPixelAt(screenshot.width() - 1, screenshot.height() - 1),
              ui_testing::Screenshot::kMagenta);  // Bottom right

    // Check pixel content at center of each rectangle.
    EXPECT_EQ(screenshot.GetPixelAt(screenshot.width() / 4, screenshot.height() / 4),
              ui_testing::Screenshot::kBlack);  // Top left
    EXPECT_EQ(screenshot.GetPixelAt(screenshot.width() / 4, (3 * screenshot.height()) / 4),
              ui_testing::Screenshot::kBlue);  // Bottom left
    EXPECT_EQ(screenshot.GetPixelAt((3 * screenshot.width()) / 4, screenshot.height() / 4),
              ui_testing::Screenshot::kRed);  // Top right
    EXPECT_EQ(screenshot.GetPixelAt((3 * screenshot.width()) / 4, (3 * screenshot.height()) / 4),
              ui_testing::Screenshot::kMagenta);  // Bottom right
    EXPECT_EQ(screenshot.GetPixelAt(screenshot.width() / 2, screenshot.height() / 2),
              ui_testing::Screenshot::kGreen);  // Center

    // Width and height of the rectangle in the center is |width|/4 and
    // |height|/4.
    const auto expected_green_pixels = (height / 4) * (width / 4);

    // The number of pixels inside each quadrant would be |width|/2 * |height|/2.
    // Since the central rectangle would cover equal areas in each quadrant, we subtract
    // |expected_green_pixels|/4 from each quadrants to get the pixel for the quadrant's color.
    const auto expected_black_pixels = (height / 2) * (width / 2) - (expected_green_pixels / 4);

    const auto expected_blue_pixels = expected_black_pixels,
               expected_red_pixels = expected_black_pixels,
               expected_magenta_pixels = expected_black_pixels;

    auto histogram = screenshot.Histogram();

    EXPECT_EQ(histogram[ui_testing::Screenshot::kBlack], expected_black_pixels);
    EXPECT_EQ(histogram[ui_testing::Screenshot::kBlue], expected_blue_pixels);
    EXPECT_EQ(histogram[ui_testing::Screenshot::kRed], expected_red_pixels);
    EXPECT_EQ(histogram[ui_testing::Screenshot::kMagenta], expected_magenta_pixels);
    EXPECT_EQ(histogram[ui_testing::Screenshot::kGreen], expected_green_pixels);
  }

  float ClientViewScaleFactor() { return ui_test_manager_->ClientViewScaleFactor(); }

  uint64_t display_height_ = 0.f;
  uint64_t display_width_ = 0.f;
  std::shared_ptr<ui_testing::TestViewAccess> test_view_access_;

 private:
  ui_testing::UITestRealm::Config config_;
  std::optional<ui_testing::UITestManager> ui_test_manager_;
  std::unique_ptr<sys::ServiceDirectory> realm_exposed_services_;
  std::optional<Realm> realm_;
};

class LandscapeModeTest : public DisplayRotationPixelTestBase,
                          public ::testing::WithParamInterface<ui_testing::UITestRealm::Config> {
 public:
  // The display is said to be in landscape mode when it is oriented horizontally i.e rotated by 0
  // or 180 degrees.
  static std::vector<int> GetDisplayRotation() { return {0, 180}; }

 protected:
  LandscapeModeTest() : DisplayRotationPixelTestBase(GetParam()) {}
};

INSTANTIATE_TEST_SUITE_P(
    DisplayRotationPixelTestWithParams, LandscapeModeTest,
    ::testing::ValuesIn(UIConfigurationsToTest(LandscapeModeTest::GetDisplayRotation())));

// This test leverage the coordinate test view to ensure that display rotation is working
// properly.
// _____________DISPLAY_______________
// |                |                |
// |     BLACK      |        RED     |
// |           _____|_____           |
// |___________|  GREEN  |___________|
// |           |_________|           |
// |                |                |
// |      BLUE      |     MAGENTA    |
// |________________|________________|
//
// The display is in landscape mode. By landscape we mean that the user sees the drawn content
// as shown above (display being rotated horizontally). The screenshot taken shows how the content
// is seen by the user.
TEST_P(LandscapeModeTest, ValidContentTest) {
  auto data = TakeScreenshot();
  auto scale_factor = ClientViewScaleFactor();

  // The width and height of the screenshot should be the same as that of the display for landscape
  // orientation.
  ASSERT_EQ(data.width(), display_width_);
  ASSERT_EQ(data.height(), display_height_);

  EXPECT_EQ(test_view_access_->view()->width(),
            static_cast<uint64_t>(static_cast<float>(data.width()) / scale_factor));
  EXPECT_EQ(test_view_access_->view()->height(),
            static_cast<uint64_t>(static_cast<float>(data.height()) / scale_factor));

  // The content of the screenshot should be independent of the display's orientation.
  AssertScreenshot(data, display_width_, display_height_);
}

class PortraitModeTest : public DisplayRotationPixelTestBase,
                         public ::testing::WithParamInterface<ui_testing::UITestRealm::Config> {
 public:
  // The display is said to be in portrait mode when it is oriented vertically i.e rotated by 90 or
  // 270 degrees.
  static std::vector<int> GetDisplayRotation() { return {90, 270}; }

 protected:
  PortraitModeTest() : DisplayRotationPixelTestBase(GetParam()) {}
};

INSTANTIATE_TEST_SUITE_P(
    DisplayRotationPixelTestWithParams, PortraitModeTest,
    ::testing::ValuesIn(UIConfigurationsToTest(PortraitModeTest::GetDisplayRotation())));

// This test leverage the coordinate test view to ensure that display rotation is working
// properly.
//  _____________________
// |          |          |
// |          |          |
// |          |          |D
// |  BLACK   |   RED    |I
// |        __|__        |S
// |       |     |       |P
// |-------|GREEN|--------L
// |       |     |       |A
// |       |__ __|       |Y
// |          |          |
// |  BLUE    |  MAGENTA |
// |          |          |
// |          |          |
//  _____________________
//
// The display is in portrait mode. By portrait we mean that the user sees the drawn content
// as shown above (display being rotated vertically). The screenshot taken shows how the content
// is seen by the user.
TEST_P(PortraitModeTest, ValidContentTest) {
  auto data = TakeScreenshot();
  auto scale_factor = ClientViewScaleFactor();

  // The width and height are flipped because the display is in portrait mode.
  ASSERT_EQ(data.width(), display_height_);
  ASSERT_EQ(data.height(), display_width_);

  EXPECT_EQ(test_view_access_->view()->width(),
            static_cast<uint64_t>(static_cast<float>(data.width()) / scale_factor));
  EXPECT_EQ(test_view_access_->view()->height(),
            static_cast<uint64_t>(static_cast<float>(data.height()) / scale_factor));

  // The content of the screenshot should be independent of the display's orientation.
  AssertScreenshot(data, display_height_, display_width_);
}

}  // namespace integration_tests
