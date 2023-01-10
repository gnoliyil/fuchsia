// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/allocation/allocator.h"

#include <lib/async/cpp/wait.h>
#include <lib/async/default.h>
#include <lib/fit/function.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

#include <memory>

#include "src/lib/fsl/handles/object_info.h"
#include "src/ui/scenic/lib/allocation/buffer_collection_importer.h"

using allocation::BufferCollectionUsage;
using fuchsia::ui::composition::BufferCollectionExportToken;
using fuchsia::ui::composition::RegisterBufferCollectionError;
using fuchsia::ui::composition::RegisterBufferCollectionUsages;

namespace allocation {

namespace {

RegisterBufferCollectionUsages UsageToUsages(
    fuchsia::ui::composition::RegisterBufferCollectionUsage usage) {
  switch (usage) {
    case fuchsia::ui::composition::RegisterBufferCollectionUsage::DEFAULT:
      return fuchsia::ui::composition::RegisterBufferCollectionUsages::DEFAULT;
    case fuchsia::ui::composition::RegisterBufferCollectionUsage::SCREENSHOT:
      return fuchsia::ui::composition::RegisterBufferCollectionUsages::SCREENSHOT;
  }
}

}  // namespace

Allocator::Allocator(sys::ComponentContext* app_context,
                     const std::vector<std::shared_ptr<BufferCollectionImporter>>&
                         default_buffer_collection_importers,
                     const std::vector<std::shared_ptr<BufferCollectionImporter>>&
                         screenshot_buffer_collection_importers,
                     fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator)
    : dispatcher_(async_get_default_dispatcher()),
      default_buffer_collection_importers_(default_buffer_collection_importers),
      screenshot_buffer_collection_importers_(screenshot_buffer_collection_importers),
      sysmem_allocator_(std::move(sysmem_allocator)),
      weak_factory_(this) {
  FX_DCHECK(app_context);
  app_context->outgoing()->AddPublicService(bindings_.GetHandler(this));
}

Allocator::~Allocator() {
  FX_DCHECK(dispatcher_ == async_get_default_dispatcher());

  // Allocator outlives |*_buffer_collection_importers_| instances, because we hold shared_ptrs. It
  // is safe to release all remaining buffer collections because there should be no more usage.
  while (!buffer_collections_.empty()) {
    ReleaseBufferCollection(buffer_collections_.begin()->first);
  }
}

void Allocator::RegisterBufferCollection(
    fuchsia::ui::composition::RegisterBufferCollectionArgs args,
    RegisterBufferCollectionCallback callback) {
  TRACE_DURATION("gfx", "allocation::Allocator::RegisterBufferCollection");
  FX_DCHECK(dispatcher_ == async_get_default_dispatcher());

  // It's okay if there's no specified RegisterBufferCollectionUsage. In that case, assume it is
  // DEFAULT.
  if (!args.has_buffer_collection_token() || !args.has_export_token()) {
    FX_LOGS(ERROR) << "RegisterBufferCollection called with missing arguments";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  auto export_token = std::move(*args.mutable_export_token());
  auto buffer_collection_token = std::move(*args.mutable_buffer_collection_token());

  // If no usages are set we default to DEFAULT. Otherwise the newer "usages" value takes precedence
  // over the deprecated "usage" variant.
  RegisterBufferCollectionUsages usages = RegisterBufferCollectionUsages::DEFAULT;
  if (args.has_usages()) {
    usages = args.usages();
  } else if (args.has_usage()) {
    usages = UsageToUsages(args.usage());
  }

  if (!buffer_collection_token.is_valid()) {
    FX_LOGS(ERROR) << "RegisterBufferCollection called with invalid buffer collection token";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  if (!export_token.value.is_valid()) {
    FX_LOGS(ERROR) << "RegisterBufferCollection called with invalid export token";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  // Check if there is a valid peer.
  if (fsl::GetRelatedKoid(export_token.value.get()) == ZX_KOID_INVALID) {
    FX_LOGS(ERROR) << "RegisterBufferCollection called with no valid import tokens";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  if (usages.has_unknown_bits()) {
    FX_LOGS(ERROR) << "Arguments contain unknown BufferCollectionUsage type";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  // Grab object koid to be used as unique_id.
  const GlobalBufferCollectionId koid = fsl::GetKoid(export_token.value.get());
  FX_DCHECK(koid != ZX_KOID_INVALID);

  // Check if this export token has already been used.
  if (buffer_collections_.find(koid) != buffer_collections_.end()) {
    FX_LOGS(ERROR) << "RegisterBufferCollection called with pre-registered export token";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  // Create a token for each of the buffer collection importers and stick all of the tokens into
  // a std::vector.
  fuchsia::sysmem::BufferCollectionTokenSyncPtr sync_token = buffer_collection_token.BindSync();
  std::vector<fuchsia::sysmem::BufferCollectionTokenSyncPtr> tokens;

  const auto importers = GetImporters(usages);

  for (uint32_t i = 0; i < importers.size(); i++) {
    fuchsia::sysmem::BufferCollectionTokenSyncPtr extra_token;
    zx_status_t status =
        sync_token->Duplicate(std::numeric_limits<uint32_t>::max(), extra_token.NewRequest());
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "RegisterBufferCollection called with a buffer collection token where "
                        "Duplicate() failed";
      callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
      return;
    }
    tokens.push_back(std::move(extra_token));
  }
  // Sync to ensure that Duplicate() calls are received on the sysmem server side.
  fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection;
  zx_status_t status = sysmem_allocator_->BindSharedCollection(std::move(sync_token),
                                                               buffer_collection.NewRequest());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "RegisterBufferCollection called with a buffer collection token where "
                      "BindSharedCollection() failed";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }
  status = buffer_collection->Sync();
  if (status != ZX_OK) {
    FX_LOGS(ERROR)
        << "RegisterBufferCollection called with a buffer collection token where Sync() failed";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }
  status = buffer_collection->Close();
  if (status != ZX_OK) {
    FX_LOGS(ERROR)
        << "RegisterBufferCollection called with a buffer collection token where Close() failed";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  // Loop over each of the importers and provide each of them with a token from the map we
  // created above. We declare the iterator |i| outside the loop to aid in cleanup if registering
  // fails.
  uint32_t i = 0;
  for (i = 0; i < importers.size(); i++) {
    auto& [importer, usage] = importers.at(i);
    auto result = importer.ImportBufferCollection(koid, sysmem_allocator_.get(),
                                                  std::move(tokens[i]), usage, std::nullopt);
    // Exit the loop early if a importer fails to import the buffer collection.
    if (!result) {
      break;
    }
  }

  // If the iterator |i| isn't equal to the number of importers than we know that one of the
  // importers has failed.
  if (i < importers.size()) {
    // We have to clean up the buffer collection from the importers where importation was
    // successful.
    for (uint32_t j = 0; j < i; j++) {
      auto& [importer, usage] = importers.at(j);
      importer.ReleaseBufferCollection(koid, usage);
    }
    FX_LOGS(ERROR) << "Failed to import the buffer collection to the BufferCollectionimporter.";
    callback(fpromise::error(RegisterBufferCollectionError::BAD_OPERATION));
    return;
  }

  buffer_collections_[koid] = usages;

  // Use a self-referencing async::WaitOnce to deregister buffer collections when all
  // BufferCollectionImportTokens are used, i.e. peers of eventpair are closed. Note that the
  // ownership of |export_token| is also passed, so that GetRelatedKoid() calls return valid koid.
  auto wait =
      std::make_shared<async::WaitOnce>(export_token.value.release(), ZX_EVENTPAIR_PEER_CLOSED);
  status = wait->Begin(async_get_default_dispatcher(),
                       [copy_ref = wait, weak_ptr = weak_factory_.GetWeakPtr(), koid](
                           async_dispatcher_t*, async::WaitOnce*, zx_status_t status,
                           const zx_packet_signal_t* /*signal*/) mutable {
                         FX_DCHECK(status == ZX_OK || status == ZX_ERR_CANCELED);
                         if (!weak_ptr)
                           return;
                         // Because Flatland::CreateImage() holds an import token, this
                         // is guaranteed to be called after all images are created, so
                         // it is safe to release buffer collection.
                         weak_ptr->ReleaseBufferCollection(koid);
                       });
  FX_DCHECK(status == ZX_OK);

  callback(fpromise::ok());
}

std::vector<std::pair<BufferCollectionImporter&, BufferCollectionUsage>> Allocator::GetImporters(
    const RegisterBufferCollectionUsages usages) const {
  std::vector<std::pair<BufferCollectionImporter&, BufferCollectionUsage>> importers;
  if (usages & RegisterBufferCollectionUsages::DEFAULT) {
    for (const auto& importer : default_buffer_collection_importers_) {
      importers.emplace_back(*importer, BufferCollectionUsage::kClientImage);
    }
  }
  if (usages & RegisterBufferCollectionUsages::SCREENSHOT) {
    for (const auto& importer : screenshot_buffer_collection_importers_) {
      importers.emplace_back(*importer, BufferCollectionUsage::kRenderTarget);
    }
  }

  return importers;
}

void Allocator::ReleaseBufferCollection(GlobalBufferCollectionId collection_id) {
  TRACE_DURATION("gfx", "allocation::Allocator::ReleaseBufferCollection");
  FX_DCHECK(dispatcher_ == async_get_default_dispatcher());

  const auto usages = buffer_collections_.at(collection_id);
  buffer_collections_.erase(collection_id);

  for (auto& [importer, usage] : GetImporters(usages)) {
    importer.ReleaseBufferCollection(collection_id, usage);
  }
}

}  // namespace allocation
