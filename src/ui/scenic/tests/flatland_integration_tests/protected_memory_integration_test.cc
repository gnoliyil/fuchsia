// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/ui/composition/cpp/fidl.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_creation_tokens.h>
#include <lib/ui/scenic/cpp/view_identity.h>

#include <iostream>

#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/scenic/lib/allocation/buffer_collection_import_export_tokens.h"
#include "src/ui/scenic/lib/utils/helpers.h"
#include "src/ui/scenic/tests/utils/scenic_realm_builder.h"
#include "src/ui/scenic/tests/utils/utils.h"
#include "src/ui/testing/util/screenshot_helper.h"

namespace integration_tests {

using fuchsia::ui::composition::ChildViewWatcher;
using fuchsia::ui::composition::ContentId;
using fuchsia::ui::composition::FlatlandPtr;
using fuchsia::ui::composition::ParentViewportWatcher;
using fuchsia::ui::composition::TransformId;
using ui_testing::Screenshot;

class ProtectedMemoryIntegrationTest : public gtest::RealLoopFixture {
 public:
  ProtectedMemoryIntegrationTest()
      : realm_(ScenicRealmBuilder()
                   .AddRealmProtocol(fuchsia::ui::composition::Flatland::Name_)
                   .AddRealmProtocol(fuchsia::ui::composition::FlatlandDisplay::Name_)
                   .AddRealmProtocol(fuchsia::ui::composition::Allocator::Name_)
                   .Build()) {
    auto context = sys::ComponentContext::Create();
    context->svc()->Connect(sysmem_allocator_.NewRequest());

    flatland_display_ = realm_.component().Connect<fuchsia::ui::composition::FlatlandDisplay>();
    flatland_display_.set_error_handler([](zx_status_t status) {
      FAIL() << "Lost connection to Scenic: " << zx_status_get_string(status);
    });

    flatland_allocator_ = realm_.component().ConnectSync<fuchsia::ui::composition::Allocator>();

    root_flatland_ = realm_.component().Connect<fuchsia::ui::composition::Flatland>();
    root_flatland_.set_error_handler([](zx_status_t status) {
      FAIL() << "Lost connection to Scenic: " << zx_status_get_string(status);
    });

    // Attach |root_flatland_| as the only Flatland under |flatland_display_|.
    auto [child_token, parent_token] = scenic::ViewCreationTokenPair::New();
    fidl::InterfacePtr<ChildViewWatcher> child_view_watcher;
    flatland_display_->SetContent(std::move(parent_token), child_view_watcher.NewRequest());
    fidl::InterfacePtr<ParentViewportWatcher> parent_viewport_watcher;
    root_flatland_->CreateView2(std::move(child_token), scenic::NewViewIdentityOnCreation(), {},
                                parent_viewport_watcher.NewRequest());

    // Get the display's width and height. Since there is no Present in FlatlandDisplay, receiving
    // this callback ensures that all |flatland_display_| calls are processed.
    std::optional<fuchsia::ui::composition::LayoutInfo> info;
    parent_viewport_watcher->GetLayout([&info](auto result) { info = std::move(result); });
    RunLoopUntil([&info] { return info.has_value(); });
    display_width_ = info->logical_size().width;
    display_height_ = info->logical_size().height;

    screenshotter_ = realm_.component().ConnectSync<fuchsia::ui::composition::Screenshot>();
  }

 protected:
  void BlockingPresent(FlatlandPtr& flatland) {
    bool presented = false;
    flatland.events().OnFramePresented = [&presented](auto) { presented = true; };
    flatland->Present({});
    RunLoopUntil([&presented] { return presented; });
    flatland.events().OnFramePresented = nullptr;
  }

  void SetConstraintsAndAllocateBuffer(fuchsia::sysmem::BufferCollectionTokenSyncPtr token,
                                       bool use_protected_memory) {
    fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection;
    auto status =
        sysmem_allocator_->BindSharedCollection(std::move(token), buffer_collection.NewRequest());
    ASSERT_EQ(status, ZX_OK);
    fuchsia::sysmem::BufferCollectionConstraints constraints;
    if (use_protected_memory) {
      constraints.has_buffer_memory_constraints = true;
      constraints.buffer_memory_constraints.secure_required = true;
      constraints.buffer_memory_constraints.inaccessible_domain_supported = true;
      constraints.buffer_memory_constraints.cpu_domain_supported = false;
      constraints.buffer_memory_constraints.ram_domain_supported = false;
    }
    constraints.usage.none = fuchsia::sysmem::noneUsage;
    constraints.min_buffer_count = 1;
    constraints.image_format_constraints_count = 1;
    auto& image_constraints = constraints.image_format_constraints[0];
    image_constraints.pixel_format.type = fuchsia::sysmem::PixelFormatType::BGRA32;
    image_constraints.color_spaces_count = 1;
    image_constraints.color_space[0] =
        fuchsia::sysmem::ColorSpace{.type = fuchsia::sysmem::ColorSpaceType::SRGB};
    image_constraints.required_min_coded_width = display_width_;
    image_constraints.required_min_coded_height = display_height_;
    image_constraints.required_max_coded_width = display_width_;
    image_constraints.required_max_coded_height = display_height_;
    status = buffer_collection->SetConstraints(true, constraints);
    ASSERT_EQ(status, ZX_OK);
    zx_status_t allocation_status = ZX_OK;
    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info{};
    status =
        buffer_collection->WaitForBuffersAllocated(&allocation_status, &buffer_collection_info);
    ASSERT_EQ(ZX_OK, status);
    ASSERT_EQ(ZX_OK, allocation_status);
    EXPECT_EQ(constraints.min_buffer_count, buffer_collection_info.buffer_count);
    ASSERT_EQ(ZX_OK, buffer_collection->Close());
  }

  const TransformId kRootTransform{.value = 1};
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;

  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator_;
  fuchsia::ui::composition::AllocatorSyncPtr flatland_allocator_;
  FlatlandPtr root_flatland_;
  fuchsia::ui::composition::ScreenshotSyncPtr screenshotter_;

 private:
  component_testing::RealmRoot realm_;
  fuchsia::ui::composition::FlatlandDisplayPtr flatland_display_;
};

TEST_F(ProtectedMemoryIntegrationTest, RendersProtectedImage) {
  auto [local_token, scenic_token] = utils::CreateSysmemTokens(sysmem_allocator_.get());

  // Send one token to Flatland Allocator.
  allocation::BufferCollectionImportExportTokens bc_tokens =
      allocation::BufferCollectionImportExportTokens::New();
  fuchsia::ui::composition::RegisterBufferCollectionArgs rbc_args = {};
  rbc_args.set_export_token(std::move(bc_tokens.export_token));
  rbc_args.set_buffer_collection_token(std::move(scenic_token));
  fuchsia::ui::composition::Allocator_RegisterBufferCollection_Result result;
  flatland_allocator_->RegisterBufferCollection(std::move(rbc_args), &result);
  ASSERT_FALSE(result.is_err());

  // Use the local token to allocate a protected buffer.
  SetConstraintsAndAllocateBuffer(std::move(local_token), /*use_protected_memory=*/true);

  // Create the image in the Flatland instance.
  fuchsia::ui::composition::ImageProperties image_properties = {};
  image_properties.set_size({display_width_, display_height_});
  const ContentId kImageContentId{.value = 1};
  root_flatland_->CreateImage(kImageContentId, std::move(bc_tokens.import_token),
                              /*buffer_collection_index=*/0, std::move(image_properties));
  BlockingPresent(root_flatland_);

  // Present the created Image.
  root_flatland_->CreateTransform(kRootTransform);
  root_flatland_->SetRootTransform(kRootTransform);
  root_flatland_->SetContent(kRootTransform, kImageContentId);
  BlockingPresent(root_flatland_);

  // Verify that render happened without any errors.
}

TEST_F(ProtectedMemoryIntegrationTest, ScreenshotReplacesProtectedImage) {
  auto [local_token, scenic_token] = utils::CreateSysmemTokens(sysmem_allocator_.get());

  // Send one token to Flatland Allocator.
  allocation::BufferCollectionImportExportTokens bc_tokens =
      allocation::BufferCollectionImportExportTokens::New();
  fuchsia::ui::composition::RegisterBufferCollectionArgs rbc_args = {};
  rbc_args.set_export_token(std::move(bc_tokens.export_token));
  rbc_args.set_buffer_collection_token(std::move(scenic_token));
  fuchsia::ui::composition::Allocator_RegisterBufferCollection_Result result;
  flatland_allocator_->RegisterBufferCollection(std::move(rbc_args), &result);
  ASSERT_FALSE(result.is_err());

  // Use the local token to allocate a protected buffer.
  SetConstraintsAndAllocateBuffer(std::move(local_token), /*use_protected_memory=*/true);

  // Create the image in the Flatland instance.
  fuchsia::ui::composition::ImageProperties image_properties = {};
  image_properties.set_size({display_width_, display_height_});
  const ContentId kImageContentId{.value = 1};
  root_flatland_->CreateImage(kImageContentId, std::move(bc_tokens.import_token),
                              /*buffer_collection_index=*/0, std::move(image_properties));
  BlockingPresent(root_flatland_);

  // Present the created Image.
  root_flatland_->CreateTransform(kRootTransform);
  root_flatland_->SetRootTransform(kRootTransform);
  root_flatland_->SetContent(kRootTransform, kImageContentId);
  BlockingPresent(root_flatland_);

  // Verify that screenshot works and replaced the content with black.
  auto screenshot = TakeScreenshot(screenshotter_, display_width_, display_height_);
  EXPECT_EQ(screenshot.Histogram()[ui_testing::Screenshot::kBlack],
            screenshot.width() * screenshot.height());
}

}  // namespace integration_tests
