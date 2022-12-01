// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_UTILS_SHADER_WARMUP_H_
#define SRC_UI_SCENIC_LIB_UTILS_SHADER_WARMUP_H_

#include <vector>

#include "src/ui/lib/escher/forward_declarations.h"

#include <vulkan/vulkan.hpp>

namespace utils {

// Return the full list of supported vk::Formats for client images.  These are used in sysmem
// negotiations for both Gfx and Flatland.
const std::vector<vk::Format>& SupportedClientImageFormats();

// Subset of the formats from SupportedClientImageFormats(), containing only the YUV formats.
const std::vector<vk::Format>& SupportedClientYuvImageFormats();

// Generate a list of immutable samplers for combinations of YUV formats and color spaces that are
// supported by Flatland and GFX.  These can be used for shader warm-up, and are also stashed in
// Escher's sampler cache.
std::vector<escher::SamplerPtr> ImmutableSamplersForShaderWarmup(escher::EscherWeakPtr escher,
                                                                 vk::Filter filter);

}  // namespace utils

#endif  // SRC_UI_SCENIC_LIB_UTILS_SHADER_WARMUP_H_
