// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_ELD_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_ELD_H_

#include <fbl/array.h>

#include "src/graphics/display/lib/edid/edid.h"

namespace display {

void ComputeEld(const edid::Edid& edid, fbl::Array<uint8_t>& out_eld);

}  // namespace display

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_COORDINATOR_ELD_H_
