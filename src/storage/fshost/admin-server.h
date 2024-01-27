// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_ADMIN_SERVER_H_
#define SRC_STORAGE_FSHOST_ADMIN_SERVER_H_

#include <fidl/fuchsia.fshost/cpp/wire.h>
#include <lib/async-loop/default.h>

#include <thread>

#include "src/lib/storage/vfs/cpp/service.h"
#include "src/storage/fshost/fs-manager.h"

namespace fshost {

class FsManager;

class AdminServer final : public fidl::WireServer<fuchsia_fshost::Admin> {
 public:
  AdminServer(FsManager* fs_manager, const fshost_config::Config& config,
              BlockWatcher& block_watcher)
      : fs_manager_(fs_manager), config_(config), block_watcher_(block_watcher) {}

  // Creates a new fs::Service backed by a new AdminServer, to be inserted into
  // a pseudo fs.
  static fbl::RefPtr<fs::Service> Create(FsManager* fs_manager, const fshost_config::Config& config,
                                         async_dispatcher* dispatcher, BlockWatcher& block_watcher);

  void Mount(MountRequestView request, MountCompleter::Sync& completer) override;

  void Unmount(UnmountRequestView request, UnmountCompleter::Sync& completer) override;

  void GetDevicePath(GetDevicePathRequestView request,
                     GetDevicePathCompleter::Sync& completer) override;

  void WriteDataFile(WriteDataFileRequestView request,
                     WriteDataFileCompleter::Sync& completer) override;

  void WipeStorage(WipeStorageRequestView request, WipeStorageCompleter::Sync& completer) override;

  void ShredDataVolume(ShredDataVolumeCompleter::Sync& completer) override;

 private:
  zx::result<> WriteDataFileInner(WriteDataFileRequestView request);

  FsManager* fs_manager_;
  const fshost_config::Config& config_;
  BlockWatcher& block_watcher_;
};

}  // namespace fshost

#endif  // SRC_STORAGE_FSHOST_ADMIN_SERVER_H_
