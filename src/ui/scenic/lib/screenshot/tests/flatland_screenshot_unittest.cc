// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/screenshot/flatland_screenshot.h"

#include <fuchsia/ui/composition/cpp/fidl.h>
#include <lib/sys/cpp/testing/component_context_provider.h>

#include <utility>

#include <gtest/gtest.h>

#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/scenic/lib/allocation/allocator.h"
#include "src/ui/scenic/lib/flatland/renderer/null_renderer.h"
#include "src/ui/scenic/lib/screen_capture/screen_capture_buffer_collection_importer.h"

using allocation::BufferCollectionImporter;
using screen_capture::ScreenCaptureBufferCollectionImporter;

namespace screenshot {
namespace test {

constexpr auto kDisplayWidth = 100u;
constexpr auto kDisplayHeight = 200u;

class FlatlandScreenshotTest : public gtest::RealLoopFixture,
                               public ::testing::WithParamInterface<int> {
 public:
  FlatlandScreenshotTest() = default;
  void SetUp() override {
    renderer_ = std::make_shared<flatland::NullRenderer>();
    importer_ = std::make_shared<ScreenCaptureBufferCollectionImporter>(
        utils::CreateSysmemAllocatorSyncPtr("ScreenshotTest"), renderer_);

    std::vector<std::shared_ptr<BufferCollectionImporter>> screenshot_importers;
    screenshot_importers.push_back(importer_);

    screen_capturer_ = std::make_unique<screen_capture::ScreenCapture>(
        screen_capture_ptr_.NewRequest(), screenshot_importers, renderer_,
        /*get_renderables=*/[](auto...) {
          return std::make_pair<std::vector<flatland::ImageRect>,
                                std::vector<allocation::ImageMetadata>>({}, {});
        });

    // Create flatland allocator.
    {
      std::vector<std::shared_ptr<BufferCollectionImporter>> extra_importers;
      std::vector<std::shared_ptr<BufferCollectionImporter>> screenshot_importers;
      screenshot_importers.push_back(importer_);
      flatland_allocator_ = std::make_shared<allocation::Allocator>(
          context_provider_.context(), extra_importers, screenshot_importers,
          utils::CreateSysmemAllocatorSyncPtr("-allocator"));
    }

    // We have what we need to make the flatland screenshot client.

    fuchsia::math::SizeU display_size = {.width = kDisplayWidth, .height = kDisplayHeight};

    flatland_screenshotter_ = std::make_unique<screenshot::FlatlandScreenshot>(
        std::move(screen_capturer_), flatland_allocator_, display_size, GetParam(), [](auto...) {});
    RunLoopUntilIdle();
  }

  void TearDown() override {}

  std::unique_ptr<screenshot::FlatlandScreenshot> flatland_screenshotter_;

 private:
  sys::testing::ComponentContextProvider context_provider_;

  std::shared_ptr<flatland::NullRenderer> renderer_;
  std::shared_ptr<ScreenCaptureBufferCollectionImporter> importer_;

  std::shared_ptr<allocation::Allocator> flatland_allocator_;

  fuchsia::ui::composition::ScreenCapturePtr screen_capture_ptr_;
  std::unique_ptr<screen_capture::ScreenCapture> screen_capturer_;
};

INSTANTIATE_TEST_SUITE_P(ParameterizedFlatlandScreenshotTest, FlatlandScreenshotTest,
                         testing::Values(0, 90, 180, 270));

TEST_P(FlatlandScreenshotTest, SimpleTest) {
  fuchsia::ui::composition::ScreenshotTakeRequest request;
  request.set_format(fuchsia::ui::composition::ScreenshotFormat::BGRA_RAW);

  fuchsia::ui::composition::ScreenshotTakeResponse take_response;
  bool done = false;

  flatland_screenshotter_->Take(
      std::move(request),
      [&take_response, &done](fuchsia::ui::composition::ScreenshotTakeResponse response) {
        take_response = std::move(response);
        done = true;
      });

  RunLoopUntil([&done] { return done; });

  EXPECT_TRUE(take_response.has_vmo());
  EXPECT_TRUE(take_response.has_size());

  // Width and height are flipped when the display is rotated by 90 or 270 degrees.
  if (GetParam() == 90 || GetParam() == 270) {
    EXPECT_EQ(take_response.size().width, kDisplayHeight);
    EXPECT_EQ(take_response.size().height, kDisplayWidth);

  } else {
    EXPECT_EQ(take_response.size().width, kDisplayWidth);
    EXPECT_EQ(take_response.size().height, kDisplayHeight);
  }
  EXPECT_NE(take_response.vmo(), 0u);
}

}  // namespace test
}  // namespace screenshot
