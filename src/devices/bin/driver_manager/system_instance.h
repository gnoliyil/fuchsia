// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_SYSTEM_INSTANCE_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_SYSTEM_INSTANCE_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/namespace.h>
#include <lib/stdcompat/span.h>

#include "src/devices/bin/driver_manager/coordinator.h"
#include "src/lib/storage/vfs/cpp/managed_vfs.h"
#include "src/lib/storage/vfs/cpp/pseudo_dir.h"

// Host's a vfs which forwards a subset of requests to a channel.
class DirectoryFilter {
 public:
  DirectoryFilter(async_dispatcher_t* dispatcher)
      : root_dir_(fbl::MakeRefCounted<fs::PseudoDir>()), vfs_(dispatcher) {}
  ~DirectoryFilter();

  zx_status_t Initialize(zx::channel forwarding_dir, cpp20::span<const char*> allow_filter);

  zx_status_t Serve(fidl::ServerEnd<fuchsia_io::Directory> request) {
    return vfs_.ServeDirectory(root_dir_, std::move(request));
  }

 private:
  zx::channel forwarding_dir_;
  fbl::RefPtr<fs::PseudoDir> root_dir_;
  fs::ManagedVfs vfs_;
};

class SystemInstance : public FsProvider {
 public:
  // Implementation required to implement FsProvider
  fidl::ClientEnd<fuchsia_io::Directory> CloneFs(const char* path) override;

  zx_status_t CreateDriverHostJob(const zx::job& root_job, zx::job* driver_host_job_out);

 private:
  zx_status_t InitializeDriverHostSvcDir();

  // Hosts vfs which filters driver host svc requests to /svc provided by svchost.
  // Lazily initialized.
  async::Loop loop_{&kAsyncLoopConfigNeverAttachToThread};
  std::optional<DirectoryFilter> driver_host_svc_;
};

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_SYSTEM_INSTANCE_H_
