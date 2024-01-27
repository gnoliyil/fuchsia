// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "vulkan_context.h"

#include <assert.h>
#include <stddef.h>

#include <limits>
#include <memory>
#include <utility>

#include "src/graphics/tests/common/utils.h"

#include "vulkan/vulkan.hpp"

static inline uint32_t to_uint32(uint64_t val) {
  assert(val <= std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(val);
}

vk::DebugUtilsMessengerCreateInfoEXT VulkanContext::default_debug_info_s_(
    {} /* create flags */, vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
    vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
    DebugUtilsTestCallback);

VulkanContext::ContextWithUserData VulkanContext::default_debug_callback_user_data_s_;

VulkanContext::VulkanContext(const vk::InstanceCreateInfo &instance_info,
                             uint32_t physical_device_index,
                             const vk::DeviceCreateInfo &device_info,
                             const vk::DeviceQueueCreateInfo &queue_info,
                             const vk::QueueFlags &queue_flags,
                             const vk::DebugUtilsMessengerCreateInfoEXT &debug_info,
                             ContextWithUserData debug_callback_user_data,
                             vk::Optional<const vk::AllocationCallbacks> allocator,
                             bool validation_layers_enabled, bool validation_errors_ignored)
    : instance_info_(instance_info),
      physical_device_index_(physical_device_index),
      queue_family_index_(kInvalidQueueFamily),
      queue_info_(queue_info),
      device_info_(device_info),
      debug_callback_user_data_(std::move(debug_callback_user_data)),
      debug_info_(debug_info),
      queue_flags_(queue_flags),
      allocator_(allocator),
      validation_layers_enabled_(validation_layers_enabled),
      validation_errors_ignored_(validation_errors_ignored) {
  assert(debug_info_.pUserData == nullptr &&
         "Debug callback user data must be only set in |debug_callback_user_data|.");
}

VulkanContext::VulkanContext(uint32_t physical_device_index, const vk::QueueFlags &queue_flags,
                             vk::Optional<const vk::AllocationCallbacks> allocator)
    : physical_device_index_(physical_device_index),
      queue_family_index_(kInvalidQueueFamily),
      queue_info_(vk::DeviceQueueCreateFlags(), queue_family_index_, 1 /* queueCount */,
                  &queue_priority_),
      device_info_(vk::DeviceCreateFlags(), 1 /* queueCreateInfoCount */, &queue_info_),
      debug_info_(default_debug_info_s_),
      queue_flags_(queue_flags),
      allocator_(allocator) {}

bool VulkanContext::InitInstance() {
  if (instance_initialized_) {
    RTN_MSG(false, "Instance is already initialized.\n");
  }
  if (instance_info_.ppEnabledLayerNames && instance_info_.enabledLayerCount) {
    // Tack custom layers on to |layers_|.
    std::copy(instance_info_.ppEnabledLayerNames,
              instance_info_.ppEnabledLayerNames + instance_info_.enabledLayerCount,
              std::back_inserter(layers_));
  }

  if (instance_info_.ppEnabledExtensionNames && instance_info_.enabledExtensionCount) {
    // Tack custom extensions on to |extensions_|.
    std::copy(instance_info_.ppEnabledExtensionNames,
              instance_info_.ppEnabledExtensionNames + instance_info_.enabledExtensionCount,
              std::back_inserter(extensions_));
  }

  if (validation_layers_enabled_) {
    debug_callback_user_data_.context_ = this;
    debug_info_.pUserData = &debug_callback_user_data_;

    layers_.emplace_back("VK_LAYER_KHRONOS_validation");
    extensions_.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // Install a debug utils messenger info struct by default if validation layers are enabled.
    if (!DebugUtilsMessengerInstalled()) {
      if (debug_info_.pNext) {
        RTN_MSG(false, "Can't overwrite |debug_info_| structure chain.");
      }
      debug_info_.pNext = instance_info_.pNext;
      instance_info_.pNext = &debug_info_;
    }
  } else {
    if (DebugUtilsMessengerInstalled()) {
      printf(
          "VulkanContext::InitInstance: found debug utils messenger.  "
          "Expected |validation_layers_enabled| to be true.\n");
    }
  }

  instance_info_.ppEnabledLayerNames = layers_.data();
  instance_info_.enabledLayerCount = to_uint32(layers_.size());
  instance_info_.ppEnabledExtensionNames = extensions_.data();
  instance_info_.enabledExtensionCount = to_uint32(extensions_.size());

  vk::ResultValue<vk::UniqueInstance> rv_instance(vk::Result::eNotReady, vk::UniqueInstance{});
  if (allocator_) {
    // Verify valid use of callbacks.
    if (!(allocator_->pfnAllocation && allocator_->pfnReallocation && allocator_->pfnFree)) {
      RTN_MSG(false, "Required allocator function is missing.\n");
    }
    if (allocator_->pfnInternalAllocation && !allocator_->pfnInternalFree) {
      RTN_MSG(false, "pfnInternalAllocation defined without pfnInternalFree.\n");
    }
    if (allocator_->pfnInternalFree && !allocator_->pfnInternalAllocation) {
      RTN_MSG(false, "pfnInternalFree defined without pfnInternalAllocation.\n");
    }
    rv_instance = vk::createInstanceUnique(instance_info_, allocator_);
  } else {
    rv_instance = vk::createInstanceUnique(instance_info_);
  }

  if (vk::Result::eSuccess != rv_instance.result) {
    RTN_MSG(false, "VK Error - Create instance: %d (%s)\n", static_cast<int>(rv_instance.result),
            vk::to_string(rv_instance.result).data());
  }
  instance_ = std::move(rv_instance.value);

  if (validation_layers_enabled_) {
    loader_.init(instance_.get(), vkGetInstanceProcAddr);
    auto rv_messenger =
        instance_->createDebugUtilsMessengerEXTUnique(debug_info_, nullptr, loader_);
    if (rv_messenger.result != vk::Result::eSuccess) {
      RTN_MSG(false, "VK Error - CreateDebugUtilsMessenger: %d (%s)\n",
              static_cast<int>(rv_messenger.result), vk::to_string(rv_messenger.result).data());
    }
    messenger_ = std::move(rv_messenger.value);
  }
  instance_initialized_ = true;
  return true;
}

bool VulkanContext::InitQueueFamily() {
  if (!instance_initialized_) {
    RTN_MSG(false, "Instance must be initialized before queue family.\n");
  }
  if (queue_family_initialized_) {
    RTN_MSG(false, "Queue family is already initialized.\n");
  }
  auto [r_physical_devices, physical_devices] = instance_->enumeratePhysicalDevices();
  if (vk::Result::eSuccess != r_physical_devices || physical_devices.empty()) {
    RTN_MSG(false, "VK Error - No physical device found: %d (%s)\n",
            static_cast<int>(r_physical_devices), vk::to_string(r_physical_devices).data());
  }
  physical_device_ = physical_devices[physical_device_index_];

  const auto queue_families = physical_device_.getQueueFamilyProperties();
  queue_family_index_ = to_uint32(queue_families.size());
  for (size_t i = 0; i < queue_families.size(); ++i) {
    if (queue_families[i].queueFlags & queue_flags_) {
      queue_family_index_ = to_uint32(i);
      break;
    }
  }

  if (static_cast<size_t>(queue_family_index_) == queue_families.size()) {
    queue_family_index_ = kInvalidQueueFamily;
    RTN_MSG(false, "Couldn't find an appropriate queue family.\n");
  }
  queue_info_.queueFamilyIndex = queue_family_index_;
  queue_family_initialized_ = true;
  return true;
}

bool VulkanContext::InitDevice() {
  if (!queue_family_initialized_) {
    RTN_MSG(false, "Queue family must be initialized before device.\n");
  }
  if (device_initialized_) {
    RTN_MSG(false, "Device is already initialized.\n");
  }
  vk::ResultValue<vk::UniqueDevice> rv_device(vk::Result::eNotReady, vk::UniqueDevice{});
  if (allocator_) {
    rv_device = physical_device_.createDeviceUnique(device_info_, allocator_);
  } else {
    rv_device = physical_device_.createDeviceUnique(device_info_);
  }
  if (vk::Result::eSuccess != rv_device.result) {
    RTN_MSG(false, "VK Error - Create device: %d (%s)\n", static_cast<int>(rv_device.result),
            vk::to_string(rv_device.result).data());
  }
  device_ = std::move(rv_device.value);
  const auto &queue_create_info = device_info_.pQueueCreateInfos[0];
  if (queue_create_info.flags) {
    queue_ = device_->getQueue2(vk::DeviceQueueInfo2()
                                    .setFlags(queue_create_info.flags)
                                    .setQueueFamilyIndex(queue_family_index_));
  } else {
    queue_ = device_->getQueue(queue_family_index_, 0);
  }
  // Passing vkGetInstanceProcAddr means we can skip an unnecessary dlopen of the Vulkan loader,
  // and also prevents a crash observed running in the Linux Termina VM.
  loader_.init(instance_.get(), vkGetInstanceProcAddr, device_.get());
  device_initialized_ = true;
  return true;
}

bool VulkanContext::Init() {
  if (initialized_ || instance_initialized_ || device_initialized_ || queue_family_initialized_) {
    RTN_MSG(false, "Attempt to re-initialize VulkanContext.\n");
  }
  if (!InitInstance()) {
    RTN_MSG(false, "Unable to initialize instance.\n");
  }
  if (!InitQueueFamily()) {
    RTN_MSG(false, "Unable to initialize queue family.\n");
  }
  if (!InitDevice()) {
    RTN_MSG(false, "Unable to initialize device.\n");
  }
  initialized_ = true;
  return true;
}

bool VulkanContext::DebugUtilsMessengerInstalled() const {
  bool installed = false;
  VkBaseOutStructure *next = const_cast<VkBaseOutStructure *>(
      reinterpret_cast<const VkBaseOutStructure *>(instance_info_.pNext));
  while (next) {
    const VkBaseOutStructure *base = reinterpret_cast<const VkBaseOutStructure *>(next);
    if (base->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
      installed = true;
      break;
    }
    next = next->pNext;
  }
  return installed;
}

bool VulkanContext::set_instance_info(const vk::InstanceCreateInfo &v) {
  if (instance_initialized_) {
    RTN_MSG(false, "set_instance_info ignored.  Instance is already initialized.\n");
  }
  instance_info_ = v;
  return true;
}

bool VulkanContext::set_device_info(const vk::DeviceCreateInfo &v) {
  if (device_initialized_) {
    RTN_MSG(false, "set_device_info ignored.  Device is already initialized.\n");
  }
  device_info_ = v;
  return true;
}

bool VulkanContext::set_queue_info(const vk::DeviceQueueCreateInfo &v) {
  if (queue_family_initialized_) {
    RTN_MSG(false, "set_queue_info ignored.  Queue family is already initialized.\n");
  }
  queue_info_ = v;
  return true;
}

bool VulkanContext::set_queue_flags(const vk::QueueFlags &v) {
  if (queue_family_initialized_) {
    RTN_MSG(false, "set_queue_flag_bits ignored. Queue family is already initialized.\n");
  }
  queue_flags_ = v;
  return true;
}

void VulkanContext::set_debug_utils_messenger(
    const vk::DebugUtilsMessengerCreateInfoEXT &debug_info, const ContextWithUserData &user_data) {
  assert(debug_info.pUserData == nullptr &&
         "User data must only be set in |user_data| as it will be overwritten.");
  debug_info_ = debug_info;
  debug_callback_user_data_ = user_data;
}

const vk::UniqueInstance &VulkanContext::instance() const {
  if (!instance_initialized_) {
    assert(false && "Instance is not initialized.\n");
  }
  return instance_;
}

const vk::PhysicalDevice &VulkanContext::physical_device() const {
  if (!queue_family_initialized_) {
    assert(false && "Queue family is not initialized.\n");
  }
  return physical_device_;
}

int VulkanContext::queue_family_index() const {
  if (!queue_family_initialized_) {
    assert(false && "Queue family is not initialized.\n");
  }
  return queue_family_index_;
}

const vk::UniqueDevice &VulkanContext::device() const {
  if (!device_initialized_) {
    assert(false && "Device is not initialized.\n");
  }
  return device_;
}

const vk::Queue &VulkanContext::queue() const {
  if (!device_initialized_) {
    assert(false && "Device is not initialized.\n");
  }
  return queue_;
}

VulkanContext::Builder::Builder()
    : physical_device_index_(0),
      queue_priority_(0.0f),
      queue_info_(vk::DeviceQueueCreateFlags(), physical_device_index_, 1 /* queueCount */,
                  &queue_priority_),
      device_info_(vk::DeviceCreateFlags(), 1 /* queueCreateInfoCount */, &queue_info_),
      queue_flags_(kDefaultQueueFlags),
      allocator_(nullptr),
      debug_info_(VulkanContext::default_debug_info_s_) {}

vk::DeviceCreateInfo VulkanContext::Builder::DeviceInfo() const { return device_info_; }

vk::DeviceQueueCreateInfo VulkanContext::Builder::QueueInfo() const { return queue_info_; }

VulkanContext::Builder &VulkanContext::Builder::set_allocator(
    vk::Optional<const vk::AllocationCallbacks> v) {
  allocator_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_instance_info(const vk::InstanceCreateInfo &v) {
  instance_info_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_physical_device_index(const uint32_t v) {
  physical_device_index_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_device_info(const vk::DeviceCreateInfo &v) {
  device_info_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_queue_info(const vk::DeviceQueueCreateInfo &v) {
  queue_info_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_queue_flags(const vk::QueueFlags &v) {
  queue_flags_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_validation_layers_enabled(bool v) {
  validation_layers_enabled_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_validation_errors_ignored(bool v) {
  validation_errors_ignored_ = v;
  return *this;
}

VulkanContext::Builder &VulkanContext::Builder::set_debug_utils_messenger(
    const vk::DebugUtilsMessengerCreateInfoEXT &debug_info, const ContextWithUserData &user_data) {
  assert(debug_info.pUserData == nullptr &&
         "User data must only be set in |user_data| as it will be overwritten.");
  debug_info_ = debug_info;
  debug_callback_user_data_ = user_data;

  return *this;
}

std::unique_ptr<VulkanContext> VulkanContext::Builder::Unique() const {
  auto context = std::make_unique<VulkanContext>(
      instance_info_, physical_device_index_, device_info_, queue_info_, queue_flags_, debug_info_,
      debug_callback_user_data_, allocator_, validation_layers_enabled_,
      validation_errors_ignored_);
  if (!context->Init()) {
    RTN_MSG(nullptr, "Failed to initialize VulkanContext.\n");
  }
  return context;
}
