// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fake_buffer_collection.h"

#include <fidl/fuchsia.sysmem/cpp/wire.h>
#include <fuchsia/sysmem/c/banjo.h>
#include <lib/image-format/image_format.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/vmo.h>
#include <stdlib.h>
#include <unistd.h>
#include <zircon/errors.h>
#include <zircon/pixelformat.h>

#include <fbl/algorithm.h>
#include <safemath/safe_conversions.h>

#include "src/devices/lib/sysmem/sysmem.h"

namespace camera {

constexpr auto kTag = "FakeBufferCollection";

// TODO(fxbug.dev/41499): Track creation & destruction of buffer collections for programmatic
//      checks of leaks.

const uint32_t kIspLineAlignment = 128;  // Required alignment of ISP buffers

static void GetFakeBufferSettings(buffer_collection_info_2_t& buffer_collection, size_t vmo_size) {
  buffer_collection.settings.buffer_settings.size_bytes =
      safemath::checked_cast<uint32_t>(vmo_size);
  buffer_collection.settings.buffer_settings.is_physically_contiguous = true;
  buffer_collection.settings.buffer_settings.is_secure = false;
  // constraints, coherency_domain and heap are unused.
  buffer_collection.settings.has_image_format_constraints = false;
}

zx_status_t GetImageFormat(image_format_2_t& image_format, uint32_t pixel_format_type,
                           uint32_t width, uint32_t height) {
  image_format = image_format_2_t{
      .pixel_format =
          {
              .type = pixel_format_type,
              .has_format_modifier = false,
              .format_modifier =
                  {
                      .value = static_cast<uint64_t>(fuchsia_sysmem::kFormatModifierNone),
                  },
          },
      .coded_width = width,
      .coded_height = height,
      .display_width = width,
      .display_height = height,
      .layers = 1,
      .color_space =
          {
              .type = static_cast<color_space_type_t>(fuchsia_sysmem::ColorSpaceType::kSrgb),
          },
      .has_pixel_aspect_ratio = false,
      .pixel_aspect_ratio_width = 1,
      .pixel_aspect_ratio_height = 1,
  };

  // Round coded_width up to a multiple of 128 to meet the ISP alignment restrictions.
  // TODO(jsasinowski) Determine if this should be handled in the buffer negotiation elsewhere.
  // For now, plan to move the alignment to a parameter (?)
  image_format.bytes_per_row = fbl::round_up(
      image_format.coded_width *
          ImageFormatStrideBytesPerWidthPixel(sysmem::banjo_to_fidl(image_format.pixel_format)),
      kIspLineAlignment);

  return ZX_OK;
}

zx_status_t CreateContiguousBufferCollectionInfo(buffer_collection_info_2_t& buffer_collection,
                                                 const image_format_2_t& image_format,
                                                 zx_handle_t bti_handle, uint32_t num_buffers) {
  // Initialize all the vmo handles to invalid.
  for (auto& buffer : buffer_collection.buffers) {
    buffer.vmo = ZX_HANDLE_INVALID;
  }

  if (bti_handle == ZX_HANDLE_INVALID || num_buffers >= std::size(buffer_collection.buffers)) {
    return ZX_ERR_INVALID_ARGS;
  }
  size_t vmo_size = ImageFormatImageSize(sysmem::banjo_to_fidl(image_format));

  buffer_collection.buffer_count = num_buffers;
  GetFakeBufferSettings(buffer_collection, vmo_size);
  zx_status_t status;
  for (uint32_t i = 0; i < buffer_collection.buffer_count; ++i) {
    buffer_collection.buffers[i].vmo_usable_start = 0;
    status = zx_vmo_create_contiguous(bti_handle, vmo_size, 0, &buffer_collection.buffers[i].vmo);
    if (status != ZX_OK) {
      FX_LOGST(ERROR, kTag) << "Failed to allocate Buffer Collection";
      return status;
    }
  }

  return ZX_OK;
}

zx_status_t DestroyContiguousBufferCollection(buffer_collection_info_2_t& buffer_collection) {
  auto result = ZX_OK;

  // Release all the vmo handles.
  for (auto& vmo_buffer : buffer_collection.buffers) {
    auto status = zx_handle_close(vmo_buffer.vmo);
    if (status != ZX_OK) {
      FX_LOGST(ERROR, kTag) << "Error destroying a vmo.";
      result = status;
    }
    vmo_buffer.vmo = ZX_HANDLE_INVALID;
  }

  return result;
}

}  // namespace camera
