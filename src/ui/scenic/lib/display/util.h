// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_DISPLAY_UTIL_H_
#define SRC_UI_SCENIC_LIB_DISPLAY_UTIL_H_

#include <fuchsia/hardware/display/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>

#include <cstdint>

#include "src/ui/scenic/lib/allocation/id.h"

namespace scenic_impl {
using DisplayEventId = uint64_t;

// Imports a sysmem buffer collection token to a display controller, and sets the constraints.
// A successful import will return true, otherwise it will return false.
bool ImportBufferCollection(allocation::GlobalBufferCollectionId identifier,
                            const fuchsia::hardware::display::ControllerSyncPtr& display_controller,
                            fuchsia::sysmem::BufferCollectionTokenSyncPtr token,
                            const fuchsia::hardware::display::ImageConfig& image_config);

// Imports a zx::event to the provided display controller. The return value is an ID to
// reference that event on other display controller functions that take an event as an
// argument. On failure, the return value will be fuchsia::hardware::display::INVALID_DISP_ID.
DisplayEventId ImportEvent(const fuchsia::hardware::display::ControllerSyncPtr& display_controller,
                           const zx::event& event);

// Returns true if the display controller backend is capable of image capture (i.e.
// screenshotting).
bool IsCaptureSupported(const fuchsia::hardware::display::ControllerSyncPtr& display_controller);

// Imports an image to be used for capture (i.e. screenshotting) of the display controller. This
// function returns the internal image id used by the display controller for the image. On failure,
// this function returns a value of 0 for the image id.
uint64_t ImportImageForCapture(
    const fuchsia::hardware::display::ControllerSyncPtr& display_controller,
    const fuchsia::hardware::display::ImageConfig& image_config,
    allocation::GlobalBufferCollectionId buffer_collection_id, uint32_t vmo_idx);

}  // namespace scenic_impl

#endif  // SRC_UI_SCENIC_LIB_DISPLAY_UTIL_H_
