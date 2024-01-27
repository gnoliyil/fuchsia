// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_IMAGE_CONVERSION_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_IMAGE_CONVERSION_H_

#include <fuchsia/images/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <png.h>

#include <vector>

namespace forensics {
namespace feedback_data {

// Encodes |raw_image| in PNG.
//
// The only |pixel_format| supported today is BGRA_8.
bool RawToPng(const std::vector<uint8_t>& raw, size_t height, size_t width, size_t stride,
              fuchsia::images::PixelFormat pixel_format, fuchsia::mem::Buffer* png_image);

}  // namespace feedback_data
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_IMAGE_CONVERSION_H_
