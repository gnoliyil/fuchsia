// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/drivers/test_utils/fake_buffer_collection.h"

#include <fidl/fuchsia.sysmem/cpp/wire.h>
#include <fuchsia/sysmem/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/fake-bti/bti.h>
#include <lib/zx/bti.h>
#include <lib/zx/vmo.h>

#include <gtest/gtest.h>

namespace camera {
namespace {

constexpr uint32_t kWidth = 1080;
constexpr uint32_t kHeight = 764;
constexpr uint32_t kNumberOfBuffers = 8;

TEST(CreateContiguousBufferCollectionInfo2, CreatesCollection2) {
  zx::bti bti;
  ASSERT_EQ(fake_bti_create(bti.reset_and_get_address()), ZX_OK);

  buffer_collection_info_2_t buffer_collection;
  image_format_2_t image_format;

  EXPECT_EQ(GetImageFormat(image_format, fidl::ToUnderlying(fuchsia_sysmem::PixelFormatType::kNv12),
                           kWidth, kHeight),
            ZX_OK);
  ASSERT_EQ(CreateContiguousBufferCollectionInfo(buffer_collection, image_format, bti.get(),
                                                 kNumberOfBuffers),
            ZX_OK);

  // Check it made the buffer collection like we want:
  EXPECT_EQ(buffer_collection.buffer_count, kNumberOfBuffers);
  EXPECT_EQ(image_format.coded_width, kWidth);
  EXPECT_EQ(image_format.coded_height, kHeight);
  for (uint32_t i = 0; i < std::size(buffer_collection.buffers); ++i) {
    if (i < kNumberOfBuffers) {
      EXPECT_FALSE(buffer_collection.buffers[i].vmo == ZX_HANDLE_INVALID);
    } else {
      EXPECT_TRUE(buffer_collection.buffers[i].vmo == ZX_HANDLE_INVALID);
    }
  }

  // Clean up
  EXPECT_EQ(DestroyContiguousBufferCollection(buffer_collection), ZX_OK);
}

TEST(CreateContiguousBufferCollectionInfo2, FailsOnBadHandle) {
  zx_handle_t bti_handle = ZX_HANDLE_INVALID;
  buffer_collection_info_2_t buffer_collection;
  image_format_2_t image_format;

  EXPECT_EQ(GetImageFormat(image_format, fidl::ToUnderlying(fuchsia_sysmem::PixelFormatType::kNv12),
                           kWidth, kHeight),
            ZX_OK);
  EXPECT_EQ(camera::CreateContiguousBufferCollectionInfo(buffer_collection, image_format,
                                                         bti_handle, kNumberOfBuffers),
            ZX_ERR_INVALID_ARGS);

  // Cleanup
  EXPECT_EQ(DestroyContiguousBufferCollection(buffer_collection), ZX_OK);
}
}  // namespace
}  // namespace camera
