// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_FVM_INTERNAL_H_
#define SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_FVM_INTERNAL_H_

#include <fidl/fuchsia.device/cpp/wire.h>
#include <lib/zx/channel.h>

#include "src/lib/storage/fs_management/cpp/fvm.h"

namespace fs_management {

// Checks that |channel| is a partition which matches |matcher|.
zx::result<bool> PartitionMatches(fidl::UnownedClientEnd<fuchsia_device::Controller> channel,
                                  const PartitionMatcher& matcher);

}  // namespace fs_management

#endif  // SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_FVM_INTERNAL_H_
