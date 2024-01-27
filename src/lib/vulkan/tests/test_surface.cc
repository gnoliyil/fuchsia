// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/backtrace-request/backtrace-request.h>
#include <lib/zx/channel.h>

#include <cstdio>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

class TestSurface {
 public:
  TestSurface(bool use_framebuffer = false) : use_framebuffer_(use_framebuffer) {
    const char* layer_name = use_framebuffer ? "VK_LAYER_FUCHSIA_imagepipe_swapchain_fb"
                                             : "VK_LAYER_FUCHSIA_imagepipe_swapchain";
    std::vector<const char*> instance_layers{layer_name};
    std::vector<const char*> instance_ext{VK_KHR_SURFACE_EXTENSION_NAME,
                                          VK_FUCHSIA_IMAGEPIPE_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .pApplicationInfo = nullptr,
        .enabledLayerCount = static_cast<uint32_t>(instance_layers.size()),
        .ppEnabledLayerNames = instance_layers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(instance_ext.size()),
        .ppEnabledExtensionNames = instance_ext.data(),
    };

    VkResult ret = vkCreateInstance(&inst_info, nullptr, &vk_instance_);
    if (ret != VK_SUCCESS) {
      fprintf(stderr, "ERROR: vkCreateInstance() returned: %d\n", ret);
      return;
    }

    init_ = true;
  }

  ~TestSurface() {
    if (vk_instance_) {
      vkDestroyInstance(vk_instance_, nullptr);
    }
  }

  void CreateSurface(bool use_dynamic_symbol) {
    ASSERT_TRUE(init_);

    PFN_vkCreateImagePipeSurfaceFUCHSIA f_vkCreateImagePipeSurfaceFUCHSIA =
        use_dynamic_symbol
            ? reinterpret_cast<PFN_vkCreateImagePipeSurfaceFUCHSIA>(
                  vkGetInstanceProcAddr(vk_instance_, "vkCreateImagePipeSurfaceFUCHSIA"))
            : vkCreateImagePipeSurfaceFUCHSIA;
    ASSERT_TRUE(f_vkCreateImagePipeSurfaceFUCHSIA);

    zx::channel endpoint0, endpoint1;
    if (!use_framebuffer_) {
      EXPECT_EQ(ZX_OK, zx::channel::create(0, &endpoint0, &endpoint1));
    }

    VkImagePipeSurfaceCreateInfoFUCHSIA create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGEPIPE_SURFACE_CREATE_INFO_FUCHSIA,
        .pNext = nullptr,
        .imagePipeHandle = use_framebuffer_ ? ZX_HANDLE_INVALID : endpoint0.release(),
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result =
        f_vkCreateImagePipeSurfaceFUCHSIA(vk_instance_, &create_info, nullptr, &surface);
    EXPECT_EQ(VK_SUCCESS, result);
    if (VK_SUCCESS == result) {
      vkDestroySurfaceKHR(vk_instance_, surface, nullptr);
    }
  }

  void GetPresentModes() {
    ASSERT_TRUE(init_);

    zx::channel endpoint0, endpoint1;
    if (!use_framebuffer_) {
      EXPECT_EQ(ZX_OK, zx::channel::create(0, &endpoint0, &endpoint1));
    }

    VkImagePipeSurfaceCreateInfoFUCHSIA create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGEPIPE_SURFACE_CREATE_INFO_FUCHSIA,
        .pNext = nullptr,
        .imagePipeHandle = use_framebuffer_ ? ZX_HANDLE_INVALID : endpoint0.release(),
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ASSERT_EQ(VK_SUCCESS,
              vkCreateImagePipeSurfaceFUCHSIA(vk_instance_, &create_info, nullptr, &surface));

    uint32_t count;
    ASSERT_EQ(VK_SUCCESS, vkEnumeratePhysicalDevices(vk_instance_, &count, nullptr));
    ASSERT_GE(count, 1u);

    std::vector<VkPhysicalDevice> devices(count);
    EXPECT_EQ(VK_SUCCESS, vkEnumeratePhysicalDevices(vk_instance_, &count, devices.data()));

    EXPECT_EQ(VK_SUCCESS,
              vkGetPhysicalDeviceSurfacePresentModesKHR(devices[0], surface, &count, nullptr));

    std::vector<VkPresentModeKHR> present_modes(count);
    ASSERT_GE(count, 1u);

    EXPECT_EQ(VK_SUCCESS, vkGetPhysicalDeviceSurfacePresentModesKHR(devices[0], surface, &count,
                                                                    present_modes.data()));

    bool found_fifo = false;
    for (auto mode : present_modes) {
      if (mode == VK_PRESENT_MODE_FIFO_KHR) {
        found_fifo = true;
        break;
      }
    }
    EXPECT_TRUE(found_fifo);

    vkDestroySurfaceKHR(vk_instance_, surface, nullptr);
  }

  bool use_framebuffer_ = false;
  VkInstance vk_instance_ = VK_NULL_HANDLE;
  bool init_ = false;
};

TEST(Surface, CreateImagePipeSurface) {
  TestSurface(false /* use_framebuffer */).CreateSurface(false /* use_dynamic_symbol */);
}

TEST(Surface, CreateImagePipeSurfaceDynamicSymbol) { TestSurface(false).CreateSurface(true); }

// Flaking: see https://fxbug.dev/65248
TEST(Surface, DISABLED_CreateFramebufferSurface) { TestSurface(true).CreateSurface(false); }

TEST(Surface, CreateFramebufferSurfaceDynamicSymbol) {
  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  loop.StartThread();
  // Perform a backtrace if the test takes too long to try to diagnose fxbug.dev/109002
  async::PostDelayedTask(
      loop.dispatcher(), []() { backtrace_request_all_threads(); }, zx::sec(20));

  TestSurface(true).CreateSurface(true);
  // Cancels the backtrace request if it hasn't yet executed.
  loop.Shutdown();
}

TEST(Surface, GetPresentModes) { TestSurface(true).GetPresentModes(); }
