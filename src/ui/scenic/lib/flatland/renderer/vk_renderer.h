// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_VK_RENDERER_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_VK_RENDERER_H_

#include <fuchsia/images/cpp/fidl.h>

#include <iterator>
#include <set>
#include <unordered_map>

#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/ui/lib/escher/flatland/rectangle_compositor.h"
#include "src/ui/scenic/lib/allocation/buffer_collection_importer.h"
#include "src/ui/scenic/lib/allocation/id.h"
#include "src/ui/scenic/lib/flatland/renderer/renderer.h"

namespace flatland {

using allocation::BufferCollectionUsage;
using allocation::GlobalBufferCollectionId;
using allocation::GlobalImageId;
using allocation::ImageMetadata;

// Implementation of the Flatland Renderer interface that relies on Escher and
// by extension the Vulkan API.
class VkRenderer final : public Renderer {
 public:
  explicit VkRenderer(escher::EscherWeakPtr escher);
  ~VkRenderer() override;

  // |BufferCollectionImporter|
  // Only called from the main thread.
  bool ImportBufferCollection(GlobalBufferCollectionId collection_id,
                              fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
                              fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
                              BufferCollectionUsage usage,
                              std::optional<fuchsia::math::SizeU> size) override;

  // |BufferCollectionImporter|
  // Only called from the main thread.
  void ReleaseBufferCollection(GlobalBufferCollectionId collection_id,
                               BufferCollectionUsage usage) override;

  // |BufferCollectionImporter|
  // Called from main thread or Flatland threads.
  bool ImportBufferImage(const ImageMetadata& metadata, BufferCollectionUsage usage) override;

  // |BufferCollectionImporter|
  // Called from main thread or Flatland threads.
  void ReleaseBufferImage(GlobalImageId image_id) override;

  // |Renderer|.
  // Only called from the main thread.
  void Render(const ImageMetadata& render_target, const std::vector<ImageRect>& rectangles,
              const std::vector<ImageMetadata>& images,
              const std::vector<zx::event>& release_fences = {},
              bool apply_color_conversion = false) override;

  // |Renderer|.
  // Only called from the main thread.
  void SetColorConversionValues(const std::array<float, 9>& coefficients,
                                const std::array<float, 3>& preoffsets,
                                const std::array<float, 3>& postoffsets) override;

  // |Renderer|.
  // Only called from the main thread.
  zx_pixel_format_t ChoosePreferredPixelFormat(
      const std::vector<zx_pixel_format_t>& available_formats) const override;

  // |Renderer|.
  // Only called from the main thread.
  bool SupportsRenderInProtected() const override;

  // |Renderer|.
  // Only called from the main thread.
  bool RequiresRenderInProtected(
      const std::vector<allocation::ImageMetadata>& images) const override;

  // Wait for all gpu operations to complete.
  // Only called from the main thread.
  bool WaitIdle();

 private:
  // Wrapper struct to contain the sysmem collection handle, the vulkan
  // buffer collection.
  struct CollectionData {
    fuchsia::sysmem::BufferCollectionSyncPtr collection;
    vk::BufferCollectionFUCHSIA vk_collection;
  };

  // The function ExtractImage() creates an escher Image from a sysmem collection vmo.
  escher::ImagePtr ExtractImage(const ImageMetadata& metadata,
                                vk::BufferCollectionFUCHSIA collection, vk::ImageUsageFlags usage,
                                bool readback = false);

  // ExtractTexture() is a wrapper function to ExtractImage().
  escher::TexturePtr ExtractTexture(const ImageMetadata& metadata,
                                    vk::BufferCollectionFUCHSIA collection);

  // Copies |source_image| into |dest_image|.
  void BlitRenderTarget(escher::CommandBuffer* command_buffer, escher::ImagePtr source_image,
                        vk::ImageLayout* source_image_layout, escher::ImagePtr dest_image,
                        const ImageMetadata& metadata);

  std::unordered_map<GlobalBufferCollectionId, CollectionData>* UsageToCollection(
      BufferCollectionUsage usage) FXL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Escher is how we access Vulkan.
  escher::EscherWeakPtr escher_;

  // Vulkan rendering component.
  escher::RectangleCompositor compositor_;

  // This mutex protects access to class members that are accessed on main thread and the Flatland
  // threads.
  mutable std::mutex lock_;

  std::unordered_map<GlobalBufferCollectionId, CollectionData> texture_collections_
      FXL_GUARDED_BY(lock_);
  std::unordered_map<GlobalBufferCollectionId, CollectionData> render_target_collections_
      FXL_GUARDED_BY(lock_);
  std::unordered_map<GlobalBufferCollectionId, CollectionData> readback_collections_
      FXL_GUARDED_BY(lock_);
  std::unordered_map<GlobalImageId, escher::TexturePtr> texture_map_ FXL_GUARDED_BY(lock_);
  std::unordered_map<GlobalImageId, escher::ImagePtr> render_target_map_ FXL_GUARDED_BY(lock_);
  std::unordered_map<GlobalImageId, escher::TexturePtr> depth_target_map_ FXL_GUARDED_BY(lock_);
  std::unordered_map<GlobalImageId, escher::ImagePtr> readback_image_map_ FXL_GUARDED_BY(lock_);
  std::set<GlobalImageId> pending_textures_ FXL_GUARDED_BY(lock_);
  std::set<GlobalImageId> pending_render_targets_ FXL_GUARDED_BY(lock_);

  uint32_t frame_number_ = 0;

  const async_dispatcher_t* const main_dispatcher_;
};

}  // namespace flatland

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_VK_RENDERER_H_
