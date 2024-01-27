// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/image_conversion.h"

#include <fuchsia/images/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <png.h>

#include <src/lib/fostr/fidl/fuchsia/images/formatting.h>

#include "src/lib/fsl/vmo/sized_vmo.h"
#include "src/lib/fsl/vmo/vector.h"

namespace forensics {
namespace feedback_data {
namespace {

bool RawToPng(const png_structp png_ptr, const png_infop info_ptr, const std::vector<uint8_t>& raw,
              const size_t height, const size_t width, const size_t stride,
              const fuchsia::images::PixelFormat pixel_format, fuchsia::mem::Buffer* png_image) {
  // This is libpng obscure syntax for setting up the error handler.
  if (setjmp(png_jmpbuf(png_ptr))) {
    FX_LOGS(ERROR) << "Something went wrong in libpng";
    return false;
  }

  if (pixel_format != fuchsia::images::PixelFormat::BGRA_8) {
    FX_LOGS(ERROR) << "Expected raw image in BGRA_8, got " << pixel_format;
    return false;
  }
  const int bit_depth = 8;

  // Set the headers: output is 8-bit depth, RGBA format.
  png_set_IHDR(png_ptr, info_ptr, (uint32_t)width, (uint32_t)height, bit_depth, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  // Give libpng a pointer to each pixel at the beginning of each row.
  std::vector<const uint8_t*> rows(height);
  for (size_t y = 0; y < height; ++y) {
    rows[y] = raw.data() + y * stride;
  }
  // Data is const type this far, but not further.
  png_set_rows(png_ptr, info_ptr, const_cast<uint8_t**>(rows.data()));

  // Tell libpng how to process each row? libpng is so obscure.
  std::vector<uint8_t> pixels;
  png_set_write_fn(
      png_ptr, &pixels,
      [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto p = reinterpret_cast<std::vector<uint8_t>*>(png_get_io_ptr(png_ptr));
        p->insert(p->end(), data, data + length);
      },
      NULL);

  // This is actually the blocking call. At the end, the info and image will be written to |pixels|.
  // Note the swizzle flag, which instructs the library to read from BGRA data.
  png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_BGR, NULL);

  fsl::SizedVmo sized_vmo;
  if (!fsl::VmoFromVector(pixels, &sized_vmo)) {
    FX_LOGS(ERROR) << "Cannot convert PNG pixels to VMO";
    return false;
  }
  *png_image = std::move(sized_vmo).ToTransport();
  return true;
}

}  // namespace

bool RawToPng(const std::vector<uint8_t>& raw, const size_t height, const size_t width,
              const size_t stride, const fuchsia::images::PixelFormat pixel_format,
              fuchsia::mem::Buffer* png_image) {
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr) {
    return false;
  }
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr, NULL);
    return false;
  }

  bool success = RawToPng(png_ptr, info_ptr, raw, height, width, stride, pixel_format, png_image);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  return success;
}

}  // namespace feedback_data
}  // namespace forensics
