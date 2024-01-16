// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_CAPTURE_IMAGE_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_CAPTURE_IMAGE_H_

#include <lib/inspect/cpp/inspect.h>

#include <cstdint>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "src/graphics/display/drivers/coordinator/client-id.h"
#include "src/graphics/display/drivers/coordinator/id-map.h"
#include "src/graphics/display/lib/api-types-cpp/driver-capture-image-id.h"
#include "src/graphics/display/lib/api-types-cpp/image-id.h"

namespace display {

class Controller;

class CaptureImage : public fbl::RefCounted<CaptureImage>,
                     public IdMappable<fbl::RefPtr<CaptureImage>, ImageId> {
 public:
  CaptureImage(Controller* controller, DriverCaptureImageId driver_capture_image_id,
               inspect::Node* parent_node, ClientId client_id);
  ~CaptureImage();

  DriverCaptureImageId driver_capture_image_id() const { return driver_capture_image_id_; }

  // The client that owns the image.
  ClientId client_id() const { return client_id_; }

 private:
  void InitializeInspect(inspect::Node* parent_node);

  DriverCaptureImageId driver_capture_image_id_;
  ClientId client_id_;

  Controller* const controller_;

  inspect::Node node_;
  inspect::ValueList properties_;
};

}  // namespace display

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_CAPTURE_IMAGE_H_
