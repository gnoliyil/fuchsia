// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_DEVICE_WATCHER_CPP_DEVICE_WATCHER_DEPRECATED_H_
#define LIB_DEVICE_WATCHER_CPP_DEVICE_WATCHER_DEPRECATED_H_

#include <fbl/unique_fd.h>

#include "device-watcher.h"

// TODO(fxbug.dev/89042): Remove this API once clients have migrated.
namespace device_watcher {

zx_status_t WaitForFile(const fbl::unique_fd& dir, const char* file, fbl::unique_fd* out);

zx_status_t RecursiveWaitForFile(const fbl::unique_fd& dir, const char* path, fbl::unique_fd* out);

zx_status_t RecursiveWaitForFile(const char* path, fbl::unique_fd* out);

zx_status_t RecursiveWaitForFileReadOnly(const fbl::unique_fd& dir, const char* path,
                                         fbl::unique_fd* out);

zx_status_t RecursiveWaitForFileReadOnly(const char* path, fbl::unique_fd* out);

using FileCallback = fit::function<zx_status_t(std::string_view, zx::channel)>;
zx_status_t IterateDirectory(const fbl::unique_fd& fd, FileCallback callback);

}  // namespace device_watcher

#endif  // LIB_DEVICE_WATCHER_CPP_DEVICE_WATCHER_DEPRECATED_H_
