// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_STORAGE_WIPER_H_
#define SRC_STORAGE_FSHOST_STORAGE_WIPER_H_

#include <lib/zx/result.h>

#include <optional>
#include <string_view>

#include <fbl/unique_fd.h>

#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/storage/fshost/fshost_config.h"

namespace fshost::storage_wiper {

// Reprovision the given block device with a new FVM and blob/data partition. Only the blob
// partition will be formatted. Returns a handle to the newly formatted blob partition's data root.
//
// *WARNING*: This function will cause irreversible data loss. Use with caution.
zx::result<fs_management::StartedSingleVolumeFilesystem> WipeStorage(
    fbl::unique_fd fvm_block_device, const fshost_config::Config& config);

}  // namespace fshost::storage_wiper

#endif  // SRC_STORAGE_FSHOST_STORAGE_WIPER_H_
