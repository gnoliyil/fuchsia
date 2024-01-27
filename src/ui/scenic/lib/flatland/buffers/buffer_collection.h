// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_BUFFERS_BUFFER_COLLECTION_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_BUFFERS_BUFFER_COLLECTION_H_

#include <fuchsia/images/cpp/fidl.h>
#include <lib/fit/result.h>
#include <lib/syslog/cpp/macros.h>

#include <memory>

namespace flatland {

using BufferCollectionHandle = fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken>;

// |BufferCollectionInfo| stores the information regarding a BufferCollection.
// Instantiated via calls to |New| below.
class BufferCollectionInfo {
 public:
  // Creates a new |BufferCollectionInfo| instance. The return value is null if the buffer was
  // not created successfully. This function sets the server-side sysmem image constraints.
  //
  // TODO(fxbug.dev/48210): Make this an asynchronous call. This function is currently thread safe
  // as Allocator_Sync pointers are thread safe, but if this becomes async it may become unsafe.
  static fit::result<fit::failed, BufferCollectionInfo> New(
      fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
      BufferCollectionHandle buffer_collection_token);

  // Creates a non-initialized instance of this class. Fully initialized instances must
  // be created via a call to |New|.
  BufferCollectionInfo() = default;
  ~BufferCollectionInfo();
  BufferCollectionInfo& operator=(BufferCollectionInfo&& other) noexcept;
  BufferCollectionInfo(BufferCollectionInfo&& other) noexcept;
  BufferCollectionInfo(const BufferCollectionInfo& other) = delete;
  BufferCollectionInfo& operator=(const BufferCollectionInfo& other) = delete;

  // This BufferCollectionInfo may not be allocated due to the fact that it may not necessarily
  // have all constraints set from every client with a token. This function will return false if
  // that is the case and true if the buffer collection has actually been allocated. Additionally,
  // if this function returns true, the client will be able to access the sysmem information of
  // the collection via a call to GetSysmemInfo(). Once this function returns true, it won't ever
  // return |false| again.
  //
  // This function is thread-safe because |buffer_collection_ptr_|, which is a
  // SynchronousInterfacePtr, is thread-safe. This function will return false if the buffers are not
  // able to be constructed, for example if there are incompatible constraints that are set on the
  // server and client.
  bool BuffersAreAllocated();

  // Info describing |buffer_collection_ptr|. Do not call this until after verifying the allocation
  // status of the buffer collection with BuffersAreAllocated().
  const fuchsia::sysmem::BufferCollectionInfo_2& GetSysmemInfo() const {
    // DCHECK if the struct is uninitialized.
    FX_DCHECK(buffer_collection_info_.buffer_count >= 1);
    return buffer_collection_info_;
  }

 private:
  BufferCollectionInfo(fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection_ptr)
      : buffer_collection_ptr_(std::move(buffer_collection_ptr)) {}

  fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection_ptr_;
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info_;
};

}  // namespace flatland

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_BUFFERS_BUFFER_COLLECTION_H_
