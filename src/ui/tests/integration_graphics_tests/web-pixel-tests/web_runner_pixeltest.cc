// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/buildinfo/cpp/fidl.h>
#include <fuchsia/component/cpp/fidl.h>
#include <fuchsia/fonts/cpp/fidl.h>
#include <fuchsia/input/injection/cpp/fidl.h>
#include <fuchsia/intl/cpp/fidl.h>
#include <fuchsia/kernel/cpp/fidl.h>
#include <fuchsia/memorypressure/cpp/fidl.h>
#include <fuchsia/metrics/cpp/fidl.h>
#include <fuchsia/net/interfaces/cpp/fidl.h>
#include <fuchsia/posix/socket/cpp/fidl.h>
#include <fuchsia/scheduler/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/tracing/provider/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/display/singleton/cpp/fidl.h>
#include <fuchsia/ui/input/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/test/input/cpp/fidl.h>
#include <fuchsia/vulkan/loader/cpp/fidl.h>
#include <fuchsia/web/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include <gtest/gtest.h>

#include "constants.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/ui/testing/util/portable_ui_test.h"
#include "src/ui/testing/util/screenshot_helper.h"

namespace integration_tests {

using component_testing::ChildRef;
using component_testing::Directory;
using component_testing::ParentRef;
using component_testing::Protocol;
using component_testing::Realm;
using component_testing::Route;

constexpr zx::duration kScreenshotTimeout = zx::sec(10);

enum class TapLocation { kTopLeft, kTopRight };

class WebRunnerPixelTest : public ui_testing::PortableUITest {
 public:
  void SetUp() override {
    ui_testing::PortableUITest::SetUp();

    screenshotter_ = realm_root()->component().Connect<fuchsia::ui::composition::Screenshot>();

    // Get display information.
    info_ = realm_root()->component().Connect<fuchsia::ui::display::singleton::Info>();
    bool has_completed = false;
    info_->GetMetrics([this, &has_completed](auto info) {
      display_width_ = info.extent_in_px().width;
      display_height_ = info.extent_in_px().height;
      has_completed = true;
    });

    RunLoopUntil([&has_completed] { return has_completed; });
  }

  bool TakeScreenshotUntil(
      ui_testing::Pixel color,
      fit::function<void(std::map<ui_testing::Pixel, uint32_t>)> histogram_predicate = nullptr,
      zx::duration timeout = kScreenshotTimeout) {
    return RunLoopWithTimeoutOrUntil(
        [this, &histogram_predicate, &color] {
          auto screenshot = TakeScreenshot();
          auto histogram = screenshot.Histogram();

          bool color_found = histogram[color] > 0;
          if (color_found && histogram_predicate != nullptr) {
            histogram_predicate(std::move(histogram));
          }
          return color_found;
        },
        timeout);
  }

  ui_testing::Screenshot TakeScreenshot() {
    FX_LOGS(INFO) << "Taking screenshot... ";

    fuchsia::ui::composition::ScreenshotTakeRequest request;
    request.set_format(fuchsia::ui::composition::ScreenshotFormat::BGRA_RAW);

    std::optional<fuchsia::ui::composition::ScreenshotTakeResponse> response;
    screenshotter_->Take(std::move(request), [this, &response](auto screenshot) {
      response = std::move(screenshot);
      QuitLoop();
    });

    FX_LOGS(INFO) << "Screenshot captured.";

    EXPECT_FALSE(RunLoopWithTimeout(kScreenshotTimeout)) << "Timed out waiting for screenshot.";

    return ui_testing::Screenshot(response->vmo(), display_width_, display_height_,
                                  0 /*display_rotation*/);
  }

  bool use_scene_manager() override { return true; }
  bool use_flatland() override { return true; }
  std::string GetTestUIStackUrl() override { return "#meta/test-ui-stack.cm"; }

  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;

 private:
  void ExtendRealm() override {
    // Add child components.
    for (auto [child, url] : GetTestComponents()) {
      realm_builder().AddChild(child, url, {.startup_mode = component_testing::StartupMode::EAGER});
    }

    // Add routes between components.
    for (auto route : GetWebEngineRoutes(ChildRef{kWebClient})) {
      realm_builder().AddRoute(route);
    }

    // Route the html code to the chromium client.
    realm_builder().InitMutableConfigToEmpty(kWebClient);
    realm_builder().SetConfigValue(kWebClient, "html", HtmlForTestCase());
  }

  virtual std::string HtmlForTestCase() = 0;

  std::vector<std::pair<std::string, std::string>> GetTestComponents() {
    return {
        std::make_pair(kBuildInfoProvider, kBuildInfoProviderUrl),
        std::make_pair(kFontsProvider, kFontsProviderUrl),
        std::make_pair(kIntl, kIntlUrl),
        std::make_pair(kMemoryPressureProvider, kMemoryPressureProviderUrl),
        std::make_pair(kMockCobalt, kMockCobaltUrl),
        std::make_pair(kNetstack, kNetstackUrl),
        std::make_pair(kWebClient, kWebClientUrl),
        std::make_pair(kTextManager, kTextManagerUrl),
        std::make_pair(kWebContextProvider, kWebContextProviderUrl),
        std::make_pair(kHttpServer, kHttpServerUrl),
    };
  }

  static std::vector<Route> GetWebEngineRoutes(ChildRef target) {
    return {{.capabilities = {Protocol{fuchsia::ui::composition::Screenshot::Name_},
                              Protocol{fuchsia::ui::display::singleton::Info::Name_}},
             .source = kTestUIStackRef,
             .targets = {ParentRef{}}},
            {.capabilities = {Protocol{fuchsia::fonts::Provider::Name_}},
             .source = ChildRef{kFontsProvider},
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::ui::input::ImeService::Name_}},
             .source = ChildRef{kTextManager},
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::memorypressure::Provider::Name_}},
             .source = ChildRef{kMemoryPressureProvider},
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::net::interfaces::State::Name_}},
             .source = ChildRef{kNetstack},
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::accessibility::semantics::SemanticsManager::Name_},
                              Protocol{fuchsia::ui::scenic::Scenic::Name_}},
             .source = kTestUIStackRef,
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::ui::composition::Flatland::Name_},
                              Protocol{fuchsia::ui::composition::Allocator::Name_}},
             .source = kTestUIStackRef,
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::web::ContextProvider::Name_}},
             .source = ChildRef{kWebContextProvider},
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::sys::Environment::Name_},
                              Protocol{fuchsia::logger::LogSink::Name_},
                              Protocol{fuchsia::media::ProfileProvider::Name_},
                              Protocol{fuchsia::media::AudioDeviceEnumerator::Name_}},
             .source = ParentRef(),
             .targets = {target, ChildRef{kWebContextProvider}, ChildRef{kHttpServer}}},
            {.capabilities = {Protocol{fuchsia::tracing::provider::Registry::Name_}},
             .source = ParentRef(),
             .targets = {ChildRef{kFontsProvider}}},
            {.capabilities = {Protocol{fuchsia::metrics::MetricEventLoggerFactory::Name_}},
             .source = ChildRef{kMockCobalt},
             .targets = {ChildRef{kMemoryPressureProvider}}},
            {.capabilities = {Protocol{fuchsia::sysmem::Allocator::Name_},
                              Protocol{fuchsia::vulkan::loader::Loader::Name_}},
             .source = ParentRef(),
             .targets = {ChildRef{kMemoryPressureProvider}, target}},
            {.capabilities = {Protocol{fuchsia::kernel::RootJobForInspect::Name_},
                              Protocol{fuchsia::kernel::Stats::Name_},
                              Protocol{fuchsia::scheduler::ProfileProvider::Name_},
                              Protocol{fuchsia::tracing::provider::Registry::Name_}},
             .source = ParentRef(),
             .targets = {ChildRef{kMemoryPressureProvider}}},
            {.capabilities = {Protocol{fuchsia::posix::socket::Provider::Name_}},
             .source = ChildRef{kNetstack},
             .targets = {target, ChildRef{kHttpServer}}},
            {.capabilities = {Protocol{fuchsia::buildinfo::Provider::Name_}},
             .source = ChildRef{kBuildInfoProvider},
             .targets = {target, ChildRef{kWebContextProvider}}},
            {.capabilities = {Protocol{fuchsia::intl::PropertyProvider::Name_}},
             .source = ChildRef{kIntl},
             .targets = {target}},
            {.capabilities = {Protocol{fuchsia::ui::app::ViewProvider::Name_}},
             .source = ChildRef{kWebClient},
             .targets = {ParentRef()}},
            {.capabilities =
                 {
                     Protocol{fuchsia::tracing::provider::Registry::Name_},
                     Protocol{fuchsia::logger::LogSink::Name_},
                     Directory{.name = "config-data",
                               .rights = fuchsia::io::R_STAR_DIR,
                               .path = "/config/data"},
                 },
             .source = ParentRef(),
             .targets = {ChildRef{kFontsProvider}}}};
  }

  static constexpr auto kWebClient = "chromium_pixel_client";
  static constexpr auto kWebClientUrl = "#meta/chromium_pixel_client.cm";

  static constexpr auto kFontsProvider = "fonts_provider";
  static constexpr auto kFontsProviderUrl = "#meta/fonts.cm";

  static constexpr auto kTextManager = "text_manager";
  static constexpr auto kTextManagerUrl = "#meta/text_manager.cm";

  static constexpr auto kIntl = "intl";
  static constexpr auto kIntlUrl = "#meta/intl_property_manager.cm";

  static constexpr auto kMemoryPressureProvider = "memory_pressure_provider";
  static constexpr auto kMemoryPressureProviderUrl = "#meta/memory_monitor.cm";

  static constexpr auto kNetstack = "netstack";
  static constexpr auto kNetstackUrl = "#meta/netstack.cm";

  static constexpr auto kWebContextProvider = "web_context_provider";
  static constexpr auto kWebContextProviderUrl =
      "fuchsia-pkg://fuchsia.com/web_engine#meta/context_provider.cm";

  static constexpr auto kBuildInfoProvider = "build_info_provider";
  static constexpr auto kBuildInfoProviderUrl = "#meta/fake_build_info.cm";

  static constexpr auto kMockCobalt = "cobalt";
  static constexpr auto kMockCobaltUrl = "#meta/mock_cobalt.cm";

  static constexpr auto kHttpServer = "http_server";
  static constexpr auto kHttpServerUrl = "#meta/http_server.cm";

  fuchsia::ui::composition::ScreenshotPtr screenshotter_;
  fuchsia::ui::display::singleton::InfoPtr info_;
};

// Displays a non interactive HTML page with a solid red background.
class StaticHtmlPixelTests : public WebRunnerPixelTest {
 private:
  std::string HtmlForTestCase() override {
    return fxl::StringPrintf("http://localhost:%d/%s", kPort, kStaticHtml);
  }
};

TEST_F(StaticHtmlPixelTests, ValidPixelTest) {
  LaunchClient();
  const auto num_pixels = display_width_ * display_height_;

  // TODO(fxb/116631): Find a better replacement for screenshot loops to verify that content has
  // been rendered on the display. Take screenshot until we see the web page's background color.
  ASSERT_TRUE(TakeScreenshotUntil(ui_testing::Screenshot::kRed,
                                  [num_pixels](std::map<ui_testing::Pixel, uint32_t> histogram) {
                                    EXPECT_EQ(histogram[ui_testing::Screenshot::kRed], num_pixels);
                                  }));
}

// Displays a HTML web page with a solid magenta color. The color of the web page changes to blue
// on a tap event.
class DynamicHtmlPixelTests : public WebRunnerPixelTest {
 public:
  void SetUp() override {
    WebRunnerPixelTest::SetUp();
    RegisterTouchScreen();
  }

  void InjectInput(TapLocation tap_location) {
    auto touch = std::make_unique<fuchsia::ui::input::TouchscreenReport>();
    switch (tap_location) {
      case TapLocation::kTopLeft:
        InjectTapWithRetry(/* x = */ display_width_ / 4, /* y = */ display_height_ / 4);
        break;
      case TapLocation::kTopRight:
        InjectTapWithRetry(/* x = */ 3 * display_width_ / 4, /* y = */ display_height_ / 4);
        break;
      default:
        FX_NOTREACHED();
    }
  }

 private:
  std::string HtmlForTestCase() override {
    return fxl::StringPrintf("http://localhost:%d/%s", kPort, kDynamicHtml);
  }
};

TEST_F(DynamicHtmlPixelTests, ValidPixelTest) {
  LaunchClient();
  const auto num_pixels = display_width_ * display_height_;

  // The web page should have a magenta background color.
  {
    ASSERT_TRUE(TakeScreenshotUntil(ui_testing::Screenshot::kMagenta,
                                    [num_pixels](std::map<ui_testing::Pixel, uint32_t> histogram) {
                                      EXPECT_EQ(histogram[ui_testing::Screenshot::kMagenta],
                                                num_pixels);
                                    }));
  }

  InjectInput(TapLocation::kTopLeft);

  // The background color of the web page should change to blue after receiving a tap event.
  {
    ASSERT_TRUE(TakeScreenshotUntil(ui_testing::Screenshot::kBlue,
                                    [num_pixels](std::map<ui_testing::Pixel, uint32_t> histogram) {
                                      EXPECT_EQ(histogram[ui_testing::Screenshot::kBlue],
                                                num_pixels);
                                    }));
  }
}

// This test renders a video in the browser and takes a screenshot to verify the pixels. The video
// displays a scene as shown below:-
//  __________________________________
// |                |                |
// |     Yellow     |        Red     |
// |                |                |
// |________________|________________|
// |                |                |
// |                |                |
// |      Blue      |     Green      |
// |________________|________________|
class VideoHtmlPixelTests : public WebRunnerPixelTest {
 private:
  std::string HtmlForTestCase() override {
    return fxl::StringPrintf("http://localhost:%d/%s", kPort, kVideoHtml);
  }
};

TEST_F(VideoHtmlPixelTests, ValidPixelTest) {
  // BGRA values,
  const ui_testing::Pixel kYellow = {16, 255, 255, 255};
  const ui_testing::Pixel kRed = {26, 17, 255, 255};
  const ui_testing::Pixel kBluev1 = {255, 10, 13, 255};
  const ui_testing::Pixel kBluev2 = {255, 11, 13, 255};
  const ui_testing::Pixel kGreenv1 = {17, 255, 45, 255};
  const ui_testing::Pixel kGreenv2 = {18, 255, 45, 255};
  const ui_testing::Pixel kMagenta = {255, 255, 255, 255};

  LaunchClient();

  // The web page should render the scene as shown above.
  // TODO(fxb/116631): Find a better replacement for screenshot loops to verify that content has
  // been rendered on the display.
  ASSERT_TRUE(TakeScreenshotUntil(kYellow, [&](std::map<ui_testing::Pixel, uint32_t> histogram) {
    // Video's background color should not be visible.
    EXPECT_EQ(histogram[kMagenta], 0u);

    // Note that we do not see pure colors in the video but a shade of the colors shown in the
    // diagram. Since it is hard to assert on the exact number of pixels for each shade of the
    // color, the test asserts on the most prominent shade which was visible in the video. We also
    // cannot go for the exact number of pixels because there is linear filtering applied by Scenic
    // when magnifying that blends colors, so we check these pixels exist.
    EXPECT_GT(histogram[kYellow], 100000u);
    EXPECT_GT(histogram[kRed], 100000u);
    EXPECT_GT(histogram[kBluev1] + histogram[kBluev2], 100000u);
    EXPECT_GT(histogram[kGreenv1] + histogram[kGreenv2], 100000u);
  }));
}

}  // namespace integration_tests
