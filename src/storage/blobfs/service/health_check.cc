// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/service/health_check.h"

#include <fidl/fuchsia.update.verify/cpp/wire.h>
#include <lib/async/dispatcher.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>

#include <fbl/ref_ptr.h>

#include "src/lib/storage/vfs/cpp/service.h"
#include "src/storage/blobfs/blob.h"
#include "src/storage/blobfs/cache_node.h"
#include "zircon/errors.h"

namespace fuv = fuchsia_update_verify;

namespace blobfs {

HealthCheckService::HealthCheckService(async_dispatcher_t* dispatcher, Blobfs& blobfs)
    : fs::Service(
          [dispatcher, this](fidl::ServerEnd<fuchsia_update_verify::BlobfsVerifier> server_end) {
            fidl::BindServer(dispatcher, std::move(server_end), this);
            return ZX_OK;
          }),
      blobfs_(blobfs) {}

void HealthCheckService::Verify(VerifyRequestView request, VerifyCompleter::Sync& completer) {
  constexpr size_t kMaxBytesToVerify{static_cast<size_t>(1024) * 1024};
  size_t bytes_verified = 0;
  zx_status_t status = blobfs_.GetCache().ForAllOpenNodes([&](fbl::RefPtr<CacheNode> node) {
    auto blob = fbl::RefPtr<Blob>::Downcast(std::move(node));
    if (blob->DeletionQueued()) {
      // Skip blobs that are scheduled for deletion.
      return ZX_OK;
    }
    if (blob->FileSize() == 0) {
      // Skip the null blob, or blobs which aren't in the readable state.
      return ZX_OK;
    }
    // If we run multithreaded, the blob cound transition to deleted between the above
    // DeletionQueued() check and this Verify() call. That should be OK as it only means we check a
    // blob that we didn't need to. If we need 100% correctness, we'll need to add a
    // Blob::VerifyIfNotDeleted() function that can atomically check and verify.
    if (zx_status_t status = blob->Verify(); status != ZX_OK) {
      FX_LOGS(ERROR) << "Detected corrupted blob " << blob->digest();
      return ZX_ERR_IO_DATA_INTEGRITY;
    }
    bytes_verified += blob->FileSize();
    if (bytes_verified >= kMaxBytesToVerify) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  });
  if (status == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(fuv::wire::VerifyError::kInternal);
  }
}

}  // namespace blobfs
