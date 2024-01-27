// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_EXAMPLES_VKPROTO_COMMON_COMMAND_POOL_H_
#define SRC_GRAPHICS_EXAMPLES_VKPROTO_COMMON_COMMAND_POOL_H_

#include "src/graphics/examples/vkproto/common/device.h"

#include <vulkan/vulkan.hpp>

namespace vkp {

class CommandPool {
 public:
  CommandPool(
      std::shared_ptr<vk::Device> device, uint32_t queue_family_index,
      vk::CommandPoolCreateFlags flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer);

  bool Init();
  const vk::CommandPool &get() const { return command_pool_.get(); }

 private:
  VKP_DISALLOW_COPY_AND_ASSIGN(CommandPool);

  bool initialized_;
  std::shared_ptr<vk::Device> device_;
  uint32_t queue_family_index_{};
  vk::CommandPoolCreateFlags flags_{};

  vk::UniqueCommandPool command_pool_;
};

}  // namespace vkp

#endif  // SRC_GRAPHICS_EXAMPLES_VKPROTO_COMMON_COMMAND_POOL_H_
