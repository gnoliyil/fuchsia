// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/flatland/rectangle_compositor.h"

#include <gtest/gtest.h>

#include "src/ui/lib/escher/defaults/default_shader_program_factory.h"
#include "src/ui/lib/escher/flatland/flatland_static_config.h"
#include "src/ui/lib/escher/renderer/batch_gpu_uploader.h"
#include "src/ui/lib/escher/renderer/frame.h"
#include "src/ui/lib/escher/renderer/render_funcs.h"
#include "src/ui/lib/escher/resources/resource.h"
#include "src/ui/lib/escher/resources/resource_manager.h"
#include "src/ui/lib/escher/test/common/gtest_escher.h"
#include "src/ui/lib/escher/test/common/readback_test.h"
#include "src/ui/lib/escher/types/color.h"
#include "src/ui/lib/escher/types/color_histogram.h"
#include "src/ui/lib/escher/vk/texture.h"

namespace escher {

// Default 1x1 texture for Renderables with no texture.
TexturePtr CreateWhiteTexture(EscherWeakPtr escher, BatchGpuUploader* gpu_uploader) {
  FX_DCHECK(escher);
  uint8_t channels[4];
  channels[0] = channels[1] = channels[2] = channels[3] = 255;
  auto image = escher->NewRgbaImage(gpu_uploader, 1, 1, channels);
  return escher->NewTexture(std::move(image), vk::Filter::eNearest);
}

// 2x2 texture with white, red, green and blue pixels.
TexturePtr CreateFourColorTexture(EscherWeakPtr escher, BatchGpuUploader* gpu_uploader) {
  FX_DCHECK(escher);
  uint8_t channels[16] = {/*white*/ 255, 255, 255, 255, /*red*/ 255, 0, 0,   255,
                          /*green*/ 0,   255, 0,   255, /*blue*/ 0,  0, 255, 255};
  auto image = escher->NewRgbaImage(gpu_uploader, 2, 2, channels);
  return escher->NewTexture(std::move(image), vk::Filter::eNearest);
}

TexturePtr CreateDepthBuffer(Escher* escher, const ImagePtr& output_image) {
  TexturePtr depth_buffer;
  RenderFuncs::ObtainDepthTexture(
      escher, output_image->use_protected_memory(), output_image->info(),
      escher->device()->caps().GetMatchingDepthStencilFormat().value, depth_buffer);
  return depth_buffer;
}

// Extends ReadbackTest to allow for quick testing of RectangleCompositor.
class RectangleCompositorTest : public ReadbackTest {
 protected:
  // |ReadbackTest|
  void SetUp() override {
    ReadbackTest::SetUp();
    escher()->shader_program_factory()->filesystem()->InitializeWithRealFiles(kFlatlandShaderPaths);
    ren_ = std::make_unique<RectangleCompositor>(escher()->GetWeakPtr());
    frame_setup();
    auto gpu_uploader =
        std::make_shared<escher::BatchGpuUploader>(escher(), frame_data_.frame->frame_number());
    auto cmd_buf = frame_data_.frame->cmds();
    auto upload_semaphore = escher::Semaphore::New(escher()->vk_device());
    gpu_uploader->AddSignalSemaphore(upload_semaphore);

    default_texture_ = CreateWhiteTexture(escher(), gpu_uploader.get());
    cmd_buf->AddWaitSemaphore(std::move(upload_semaphore),
                              vk::PipelineStageFlagBits::eVertexInput |
                                  vk::PipelineStageFlagBits::eFragmentShader |
                                  vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                  vk::PipelineStageFlagBits::eTransfer);

    gpu_uploader->Submit();
    frame_data_.frame->EndFrame(SemaphorePtr(), []() {});
  }

  // |ReadbackTest|
  void TearDown() override {
    frame_data_.frame->EndFrame(SemaphorePtr(), []() {});
    EXPECT_VK_SUCCESS(escher()->vk_device().waitIdle());
    ASSERT_TRUE(escher()->Cleanup());
    ren_.reset();
    ReadbackTest::TearDown();
  }

  // Sets up the environment.
  void frame_setup() { frame_data_ = NewFrame(vk::ImageLayout::eColorAttachmentOptimal); }

  escher::RectangleCompositor* renderer() const { return ren_.get(); }

 public:
  std::unique_ptr<escher::RectangleCompositor> ren_;

  // Frame environment variables.
  ReadbackTest::FrameData frame_data_;

  // Default texture;
  TexturePtr default_texture_;

  // Common colors used between tests.
  static constexpr ColorBgra kWhite = ColorBgra(255, 255, 255, 255);
  static constexpr ColorBgra kRed = ColorBgra(255, 0, 0, 255);
  static constexpr ColorBgra kGreen = ColorBgra(0, 255, 0, 255);
  static constexpr ColorBgra kBlue = ColorBgra(0, 0, 255, 255);
  static constexpr ColorBgra kBlack = ColorBgra(0, 0, 0, 0);
};

// Render a single renderable using the RectangleCompositor. It should
// render as a single white rectangle.
VK_TEST_F(RectangleCompositorTest, SingleRenderableTest) {
  frame_setup();
  EXPECT_TRUE(ren_);

  Rectangle2D rectangle(vec2(150, 200), vec2(100, 300));
  RectangleCompositor::ColorData color_data(vec4(1), /*is_opaque*/ true);

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, {rectangle}, {default_texture_}, {color_data},
                  frame_data_.color_attachment, depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);
  constexpr ColorBgra kWhite(255, 255, 255, 255);
  constexpr ColorBgra kBlack(0, 0, 0, 0);
  EXPECT_EQ(2U, histogram.size());
  EXPECT_EQ(histogram[kWhite], 30000U);  // 100x300.
  EXPECT_EQ(histogram[kBlack], (512U * 512U - 30000U));
}

// Render a single full-screen renderable with a texture that has 4 colors.
VK_TEST_F(RectangleCompositorTest, SimpleTextureTest) {
  frame_setup();

  auto gpu_uploader =
      std::make_shared<escher::BatchGpuUploader>(escher(), frame_data_.frame->frame_number());
  EXPECT_TRUE(gpu_uploader);
  EXPECT_TRUE(ren_);

  auto texture = CreateFourColorTexture(escher(), gpu_uploader.get());
  gpu_uploader->Submit();

  Rectangle2D rectangle(vec2(0, 0), vec2(512, 512));
  RectangleCompositor::ColorData color_data(vec4(1), /*is_opaque*/ true);

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, {rectangle}, {texture}, {color_data}, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  constexpr uint32_t num_pixels = 512 * 512 / 4;
  EXPECT_EQ(4U, histogram.size());
  EXPECT_EQ(histogram[kWhite], num_pixels);
  EXPECT_EQ(histogram[kRed], num_pixels);
  EXPECT_EQ(histogram[kGreen], num_pixels);
  EXPECT_EQ(histogram[kBlue], num_pixels);
}

// Render with color conversion applied.
VK_TEST_F(RectangleCompositorTest, ColorConversionTest) {
  frame_setup();

  auto gpu_uploader =
      std::make_shared<escher::BatchGpuUploader>(escher(), frame_data_.frame->frame_number());
  EXPECT_TRUE(gpu_uploader);
  EXPECT_TRUE(ren_);

  auto texture = CreateWhiteTexture(escher(), gpu_uploader.get());
  gpu_uploader->Submit();

  // Set the color conversion parameters.
  const glm::mat4 conversion_matrix(.288299, 0.052709, -0.257912, 0.00000, 0.711701, 0.947291,
                                    0.257912, 0.00000, 0.000000, -0.000000, 1.000000, 0.00000,
                                    0.000000, 0.000000, 0.00000, 1.00000);
  const glm::vec4 offsets(0);
  ren_->SetColorConversionParams({conversion_matrix, offsets, offsets});

  Rectangle2D rectangle(vec2(0, 0), vec2(512, 512));
  RectangleCompositor::ColorData color_data(vec4(1), /*is_opaque*/ true);

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, {rectangle}, {texture}, {color_data}, frame_data_.color_attachment,
                  depth_texture, /*apply_color_conversion*/ true);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  // Calculate expected color.
  auto expected_color_float = conversion_matrix * glm::vec4(1, 1, 1, 1);
  ColorBgra expected_color = {static_cast<uint8_t>(std::max(expected_color_float.x * 255, 0.f)),
                              static_cast<uint8_t>(std::max(expected_color_float.y * 255, 0.f)),
                              static_cast<uint8_t>(std::max(expected_color_float.z * 255, 0.f)),
                              static_cast<uint8_t>(std::max(expected_color_float.w * 255, 0.f))};

  constexpr uint32_t num_pixels = 512 * 512;
  EXPECT_EQ(1U, histogram.size());
  EXPECT_EQ(histogram[expected_color], num_pixels);

  // Reset compositor.
  ren_->SetColorConversionParams({glm::mat4(1), glm::vec4(0), glm::vec4(0)});
}

// Render a single full-screen renderable with a texture that has 4 colors but
// with uv coordinates that make it so only one pixel is covering the renderable
// at a time, and test each one.
VK_TEST_F(RectangleCompositorTest, SimpleTextureNonStandardUVsTest) {
  frame_setup();

  auto gpu_uploader =
      std::make_shared<escher::BatchGpuUploader>(escher(), frame_data_.frame->frame_number());
  EXPECT_TRUE(gpu_uploader);
  EXPECT_TRUE(ren_);

  auto texture = CreateFourColorTexture(escher(), gpu_uploader.get());
  gpu_uploader->Submit();

  constexpr uint32_t num_pixels = 512 * 512;

  const uint32_t kNumColors = 4;
  ColorBgra colors[] = {kWhite, kRed, kGreen, kBlue};

  std::array<vec2, 4> uvs[] = {
      /*white*/ {vec2(0, 0), vec2(0.5, 0), vec2(0.5, 0.5), vec2(0, 0.5)},
      /*red*/ {vec2(0.5, 0), vec2(1.0, 0), vec2(1.0, 0.5), vec2(0.5, 0.5)},
      /*green*/ {vec2(0., 0.5), vec2(0.5, 0.5), vec2(0.5, 1.0), vec2(0, 1.0)},
      /*blue*/ {vec2(0.5, 0.5), vec2(1.0, 0.5), vec2(1.0, 1.0), vec2(0.5, 1.0)}};

  RectangleCompositor::ColorData color_data(vec4(1), /*is_opaque*/ true);
  for (uint32_t i = 0; i < kNumColors; i++) {
    auto cmd_buf = frame_data_.frame->cmds();
    auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);

    Rectangle2D rectangle(vec2(0, 0), vec2(512, 512), uvs[i]);

    ren_->DrawBatch(cmd_buf, {rectangle}, {texture}, {color_data}, frame_data_.color_attachment,
                    depth_texture);

    auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                             frame_data_.color_attachment->swapchain_layout(),
                                             vk::ImageLayout::eColorAttachmentOptimal);

    const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

    EXPECT_EQ(1U, histogram.size());
    EXPECT_EQ(histogram[colors[i]], num_pixels);
  }
}

// Render a single full-screen renderable that is rotated by 90 degrees
// and shifted so that it is half off the screen to the right. This should
// make it so that only 2 out of the 4 texture colors display, and those 2
// colors should be the proper colors post-rotation.
// Prerotation:
// | W R |
// | G B |
///
// Post rotation:
// | G W |
// | B R |
//
// When this post-rotation renderable is shifted to the right hand of the screen,
// only the green and blue colors should show.
VK_TEST_F(RectangleCompositorTest, RotatedTextureTest) {
  frame_setup();

  auto gpu_uploader =
      std::make_shared<escher::BatchGpuUploader>(escher(), frame_data_.frame->frame_number());
  EXPECT_TRUE(gpu_uploader);
  EXPECT_TRUE(ren_);

  auto texture = CreateFourColorTexture(escher(), gpu_uploader.get());
  gpu_uploader->Submit();

  Rectangle2D rectangle(vec2(kFramebufferWidth / 2, 0), vec2(kFramebufferWidth, kFramebufferHeight),
                        {vec2(0, 1), vec2(0, 0), vec2(1, 0), vec2(1, 1)});
  RectangleCompositor::ColorData color_data(vec4(1), /*is_opaque*/ true);

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, {rectangle}, {texture}, {color_data}, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  constexpr uint32_t num_pixels_per_color = kFramebufferWidth * kFramebufferHeight / 4;

  // The three colors that should show are black (background), green and blue.
  EXPECT_EQ(3U, histogram.size());
  EXPECT_EQ(histogram[kWhite], 0U);
  EXPECT_EQ(histogram[kRed], 0U);
  EXPECT_EQ(histogram[kGreen], num_pixels_per_color);
  EXPECT_EQ(histogram[kBlue], num_pixels_per_color);
}

// Render 4 rectangles side by side, each one taking up
// 1/4 of the entire frame. There should be no black pixels
// and each rectangle should have the same exact number of
// pixels covered.
VK_TEST_F(RectangleCompositorTest, MultiRenderableTest) {
  frame_setup();
  EXPECT_TRUE(ren_);

  std::vector<Rectangle2D> rectangles;
  std::vector<RectangleCompositor::ColorData> color_datas;
  std::vector<const TexturePtr> textures;
  vec4 colors[4] = {vec4{1, 0, 0, 1}, vec4(0, 1, 0, 1), vec4(0, 0, 1, 1), vec4(1, 1, 1, 1)};
  for (uint32_t i = 0; i < 4; i++) {
    Rectangle2D rectangle(vec2(128 * i, 0), vec2(128, 512));
    RectangleCompositor::ColorData color_data(colors[i], /*is_opaque*/ true);

    rectangles.emplace_back(rectangle);
    color_datas.emplace_back(color_data);
    textures.emplace_back(default_texture_);
  }

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, rectangles, textures, color_datas, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  size_t pixels_per_color = 128 * 512;

  EXPECT_EQ(4U, histogram.size());
  EXPECT_EQ(histogram[kRed], pixels_per_color);
  EXPECT_EQ(histogram[kGreen], pixels_per_color);
  EXPECT_EQ(histogram[kBlue], pixels_per_color);

  EXPECT_EQ(histogram[kWhite], pixels_per_color);
  EXPECT_EQ(histogram[kBlack], 0U);
}

// This test makes sure that depth is taken into account when
// rendering rectangles. Rectangle depth is implicit, with later
// rectangles being higher up than earlier rectangles. So this test
// renders two renderables, directly on top of eachother, red then
// green. Since the green one is inserted second, it should cover the
// red one, which should not have any pixels rendered.
VK_TEST_F(RectangleCompositorTest, OverlapTest) {
  frame_setup();
  EXPECT_TRUE(ren_);

  std::vector<Rectangle2D> rectangles;
  std::vector<RectangleCompositor::ColorData> color_datas;
  std::vector<const TexturePtr> textures;
  vec4 colors[2] = {vec4{1, 0, 0, 1}, vec4(0, 1, 0, 1)};
  for (uint32_t i = 0; i < 2; i++) {
    Rectangle2D rectangle(vec2(200, 200), vec2(100, 100));

    RectangleCompositor::ColorData color_data(colors[i], /*is_opaque*/ true);

    rectangles.emplace_back(rectangle);
    color_datas.emplace_back(color_data);
    textures.emplace_back(default_texture_);
  }

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, rectangles, textures, color_datas, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  size_t pixels_per_color = 100 * 100;

  EXPECT_EQ(2U, histogram.size());
  EXPECT_EQ(histogram[kRed], 0U);
  EXPECT_EQ(histogram[kGreen], pixels_per_color);
  EXPECT_EQ(histogram[kBlack], 512U * 512U - pixels_per_color);
}

// This test makes sure that alpha-blending transparency works.
// It renders a blue rectangle with 0.6 alpha on top of an
// opaque red rectangle.
// It does this test *twice*, once with is_opaque turned off
// and one with it on. Transparency should only be applied when
// the flag is on, even if the RectangleRenderable color has an
// alpha that is < 1.0.
// TODO (43394): Add testing for multiple interleaved opaque and
// transparent rectangles.
VK_TEST_F(RectangleCompositorTest, TransparencyTest) {
  frame_setup();
  EXPECT_TRUE(ren_);

  std::vector<Rectangle2D> rectangles;
  std::vector<RectangleCompositor::ColorData> color_datas;
  std::vector<const TexturePtr> textures;

  vec4 colors[2] = {vec4{1, 0, 0, 1}, vec4(0, 0, 1, 0.6)};
  for (uint32_t i = 0; i < 2; i++) {
    Rectangle2D rectangle(vec2(200, 200), vec2(100, 100));
    RectangleCompositor::ColorData color_data(colors[i], /*is_opaque*/ false);

    rectangles.emplace_back(rectangle);
    color_datas.emplace_back(color_data);
    textures.emplace_back(default_texture_);
  }

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, rectangles, textures, color_datas, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  // On Fuchsia the above transparency operation results in kBlend,
  // but on LinuxHost it results in kBlend2. We check equality against
  // both so that the test is robust regardless of platform.
  constexpr ColorBgra kBlend(102, 0, 153, 255);
  constexpr ColorBgra kBlend2(102, 0, 152, 255);
  size_t pixels_per_color = 100 * 100;
  EXPECT_EQ(2U, histogram.size());
  EXPECT_EQ(histogram[kRed], 0U);
  EXPECT_EQ(histogram[kBlue], 0U);
  EXPECT_TRUE(histogram[kBlend] == pixels_per_color || histogram[kBlend2] == pixels_per_color);
  EXPECT_EQ(histogram[kBlack], 512U * 512U - pixels_per_color);
}

// Turn the transparency flag off and try rendering with transparency again. Now
// even though the color has transparency, it should still render as opaque.
VK_TEST_F(RectangleCompositorTest, TransparencyFlagOffTest) {
  frame_setup();
  EXPECT_TRUE(ren_);

  std::vector<Rectangle2D> rectangles;
  std::vector<RectangleCompositor::ColorData> color_datas;
  std::vector<const TexturePtr> textures;

  vec4 colors[2] = {vec4{1, 0, 0, 1}, vec4(0, 0, 1, 0.6)};
  for (uint32_t i = 0; i < 2; i++) {
    Rectangle2D rectangle(vec2(200, 200), vec2(100, 100));
    RectangleCompositor::ColorData color_data(colors[i], /*is_opaque*/ true);

    rectangles.emplace_back(rectangle);
    color_datas.emplace_back(color_data);
    textures.emplace_back(default_texture_);
  }

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, rectangles, textures, color_datas, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram2(bytes.data(), kFramebufferWidth * kFramebufferHeight);
  constexpr ColorBgra kBlue2(0, 0, 153, 153);  // Premultiplied alpha.
  constexpr ColorBgra kBlend(102, 0, 153, 255);
  constexpr ColorBgra kBlend2(102, 0, 152, 255);

  size_t pixels_per_color = 100 * 100;
  EXPECT_EQ(2U, histogram2.size());
  EXPECT_EQ(histogram2[kRed], 0U);
  EXPECT_EQ(histogram2[kBlue2], pixels_per_color);
  EXPECT_TRUE(histogram2[kBlend] == pixels_per_color || histogram2[kBlend2] == 0);
  EXPECT_EQ(histogram2[kBlack], 512U * 512U - pixels_per_color);
}

// Render 100 renderables.
VK_TEST_F(RectangleCompositorTest, StressTest) {
  frame_setup();
  EXPECT_TRUE(ren_);

  std::vector<Rectangle2D> rectangles;
  std::vector<RectangleCompositor::ColorData> color_datas;
  std::vector<const TexturePtr> textures;
  uint32_t max_renderables = 100;
  for (uint32_t i = 0; i < max_renderables; i++) {
    Rectangle2D rectangle(vec2(i, 0), vec2(1, 1));
    RectangleCompositor::ColorData color_data(vec4(1, 0, 0, 1), /*is_opaque*/ true);

    rectangles.emplace_back(rectangle);
    color_datas.emplace_back(color_data);
    textures.emplace_back(default_texture_);
  }

  auto cmd_buf = frame_data_.frame->cmds();
  auto depth_texture = CreateDepthBuffer(escher().get(), frame_data_.color_attachment);
  ren_->DrawBatch(cmd_buf, rectangles, textures, color_datas, frame_data_.color_attachment,
                  depth_texture);

  auto bytes = ReadbackFromColorAttachment(frame_data_.frame,
                                           frame_data_.color_attachment->swapchain_layout(),
                                           vk::ImageLayout::eColorAttachmentOptimal);

  const ColorHistogram<ColorBgra> histogram(bytes.data(), kFramebufferWidth * kFramebufferHeight);

  EXPECT_EQ(2U, histogram.size());
  EXPECT_EQ(histogram[kRed], 100U);
  EXPECT_EQ(histogram[kBlack], 512U * 512U - 100U);
}

// Test to make sure that the image create info returned by the RectangleCompositor is
// AFBC compliant.
VK_TEST_F(RectangleCompositorTest, SetsAFBCCompatibleConstraints) {
  frame_setup();
  auto image_usage =
      RectangleCompositor::kRenderTargetUsageFlags | vk::ImageUsageFlagBits::eTransferSrc;
  auto vk_format = vk::Format::eB8G8R8A8Srgb;
  auto constraints = RectangleCompositor::GetDefaultImageConstraints(vk_format, image_usage);

  EXPECT_EQ(constraints.format, vk_format);
  EXPECT_EQ(constraints.mipLevels, 1U);
  EXPECT_EQ(constraints.arrayLayers, 1U);

  // https://developer.arm.com/documentation/101897/0300/Buffers-and-textures/AFBC-textures-for-Vulkan
  // In order to maintain ARM Framebuffer Compression (AFBC):
  // - VkSampleCountFlagBits must be VK_SAMPLE_COUNT_1_BIT.
  EXPECT_EQ(constraints.samples, vk::SampleCountFlagBits::e1);
  // - VkImageType must be VK_IMAGE_TYPE_2D.
  EXPECT_EQ(constraints.imageType, vk::ImageType::e2D);
  // - VkImageTiling must be VK_IMAGE_TILING_OPTIMAL.
  EXPECT_EQ(constraints.tiling, vk::ImageTiling::eOptimal);

  // VkImageUsageFlags must not contain:
  // - VK_IMAGE_USAGE_STORAGE_BIT
  EXPECT_FALSE(constraints.usage & vk::ImageUsageFlagBits::eStorage);
  // - VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
  EXPECT_FALSE(constraints.usage & vk::ImageUsageFlagBits::eTransientAttachment);
}

}  // namespace escher
