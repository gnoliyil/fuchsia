// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/minfs/mount.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/inspect/service/cpp/service.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace-provider/provider.h>

#include <safemath/safe_math.h>

#include "fidl/fuchsia.hardware.block/cpp/wire_types.h"
#include "src/storage/minfs/component_runner.h"
#include "src/storage/minfs/minfs_private.h"
#include "src/storage/minfs/runner.h"

namespace minfs {

zx::result<CreateBcacheResult> CreateBcache(std::unique_ptr<block_client::BlockDevice> device) {
  fuchsia_hardware_block::wire::BlockInfo info;
  zx_status_t status = device->BlockGetInfo(&info);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not access device info: " << status;
    return zx::error(status);
  }

  uint64_t device_size;
  if (!safemath::CheckMul(info.block_size, info.block_count).AssignIfValid(&device_size)) {
    FX_LOGS(ERROR) << "Device slize overflow";
    return zx::error(ZX_ERR_OUT_OF_RANGE);
  }
  if (device_size == 0) {
    FX_LOGS(ERROR) << "Invalid device size";
    return zx::error(ZX_ERR_NO_SPACE);
  }

  uint32_t block_count;
  if (!safemath::CheckDiv(device_size, kMinfsBlockSize)
           .Cast<uint32_t>()
           .AssignIfValid(&block_count)) {
    FX_LOGS(ERROR) << "Block count overflow";
    return zx::error(ZX_ERR_OUT_OF_RANGE);
  }

  auto bcache_or = minfs::Bcache::Create(std::move(device), block_count);
  if (bcache_or.is_error()) {
    return bcache_or.take_error();
  }

  CreateBcacheResult result{
      .bcache = std::move(bcache_or.value()),
      .is_read_only = static_cast<bool>(info.flags & fuchsia_hardware_block::wire::Flag::kReadonly),
  };
  return zx::ok(std::move(result));
}

zx::result<> Mount(std::unique_ptr<minfs::Bcache> bcache, const MountOptions& options,
                   fidl::ServerEnd<fuchsia_io::Directory> root) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());

  auto runner = Runner::Create(loop.dispatcher(), std::move(bcache), options);
  if (runner.is_error()) {
    return runner.take_error();
  }
  runner->SetUnmountCallback([&loop]() { loop.Quit(); });
  zx::result status = runner->ServeRoot(std::move(root));
  if (status.is_error()) {
    return status.take_error();
  }

  if (options.verbose) {
    FX_LOGS(INFO) << "Mounted successfully";
  }

  // |ZX_ERR_CANCELED| is returned when the loop is cancelled via |loop.Quit()|.
  ZX_ASSERT(loop.Run() == ZX_ERR_CANCELED);
  return zx::ok();
}

zx::result<> StartComponent(fidl::ServerEnd<fuchsia_io::Directory> root,
                            fidl::ServerEnd<fuchsia_process_lifecycle::Lifecycle> lifecycle) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());

  std::unique_ptr<ComponentRunner> runner(new ComponentRunner(loop.dispatcher()));
  runner->SetUnmountCallback([&loop]() { loop.Quit(); });
  auto status = runner->ServeRoot(std::move(root), std::move(lifecycle));
  if (status.is_error()) {
    return status;
  }

  // |ZX_ERR_CANCELED| is returned when the loop is cancelled via |loop.Quit()|.
  ZX_ASSERT(loop.Run() == ZX_ERR_CANCELED);
  return zx::ok();
}

}  // namespace minfs
