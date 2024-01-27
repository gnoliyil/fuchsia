// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_EXAMPLES_VKPROTO_COMMON_FRAMEBUFFERS_H_
#define SRC_GRAPHICS_EXAMPLES_VKPROTO_COMMON_FRAMEBUFFERS_H_

#include <vector>

#include "src/graphics/examples/vkproto/common/device.h"

#include <vulkan/vulkan.hpp>

namespace vkp {

class Framebuffers {
 public:
  Framebuffers(std::shared_ptr<vk::Device> device, const vk::Extent2D &extent,
               vk::RenderPass render_pass, std::vector<vk::ImageView> image_views);
  bool Init();
  const std::vector<vk::UniqueFramebuffer> &framebuffers() const;

 private:
  VKP_DISALLOW_COPY_AND_ASSIGN(Framebuffers);

  bool initialized_;
  std::shared_ptr<vk::Device> device_;
  vk::Extent2D extent_;
  std::vector<vk::ImageView> image_views_;
  vk::RenderPass render_pass_;
  std::vector<vk::UniqueFramebuffer> framebuffers_;
};

}  // namespace vkp

#endif  // SRC_GRAPHICS_EXAMPLES_VKPROTO_COMMON_FRAMEBUFFERS_H_
