// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/swapchain/swapchain_factory.h"

namespace scenic_impl {
namespace gfx {

std::unique_ptr<DisplaySwapchain> SwapchainFactory::CreateDisplaySwapchain(
    uint64_t swapchain_image_count, display::Display* display, Sysmem* sysmem,
    display::DisplayManager* display_manager, escher::Escher* escher) {
  FX_DCHECK(!display->is_claimed());
  return std::make_unique<DisplaySwapchain>(sysmem, display_manager->default_display_controller(),
                                            display_manager->default_display_controller_listener(),
                                            swapchain_image_count, display, escher);
}

}  // namespace gfx
}  // namespace scenic_impl
