// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fidl/fuchsia.fs.startup/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/limits.h>
#include <lib/fdio/vfs.h>
#include <lib/zx/channel.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <new>

#include <fbl/unique_fd.h>
#include <fbl/vector.h>

#include "src/lib/storage/fs_management/cpp/component.h"
#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/storage/fs_management/cpp/launch.h"
#include "src/lib/storage/fs_management/cpp/options.h"
#include "src/lib/storage/fs_management/cpp/path.h"

namespace fs_management {
namespace {

zx_status_t FsckNativeFs(const char* device_path, const FsckOptions& options, LaunchCallback cb,
                         const char* binary) {
  auto block_device = component::Connect<fuchsia_hardware_block::Block>(device_path);
  if (block_device.is_error())
    return block_device.status_value();
  std::vector<std::pair<uint32_t, zx::handle>> handles;
  handles.emplace_back(FS_HANDLE_BLOCK_DEVICE_ID, block_device->TakeChannel());
  return cb(options.as_argv(binary), std::move(handles));
}

zx_status_t FsckFat(const char* device_path, const FsckOptions& options, LaunchCallback cb) {
  auto block_device = component::Connect<fuchsia_hardware_block::Block>(device_path);
  if (block_device.is_error())
    return block_device.status_value();
  std::vector<std::pair<uint32_t, zx::handle>> handles;
  handles.emplace_back(FS_HANDLE_BLOCK_DEVICE_ID, block_device->TakeChannel());
  std::vector args = {GetBinaryPath("block_adapter"), GetBinaryPath("fsck-msdosfs")};
  options.append_argv_fat32(args);
  return cb(std::move(args), std::move(handles));
}

zx::result<> FsckComponentFs(fidl::UnownedClientEnd<fuchsia_io::Directory> exposed_dir,
                             const std::string& device_path, const FsckOptions& options) {
  auto device = component::Connect<fuchsia_hardware_block::Block>(device_path.c_str());
  if (device.is_error())
    return device.take_error();

  auto startup_client_end = component::ConnectAt<fuchsia_fs_startup::Startup>(exposed_dir);
  if (startup_client_end.is_error())
    return startup_client_end.take_error();
  fidl::WireSyncClient startup_client{std::move(*startup_client_end)};

  auto res = startup_client->Check(std::move(*device), options.as_check_options());
  if (!res.ok())
    return zx::error(res.status());
  if (res->is_error())
    return zx::error(res->error_value());

  return zx::ok();
}

}  // namespace

__EXPORT
zx_status_t Fsck(std::string_view device_path, DiskFormat df, const FsckOptions& options,
                 LaunchCallback cb) {
  std::string device_path_str(device_path);
  if (options.component_child_name) {
    std::string_view url =
        options.component_url.empty() ? DiskFormatComponentUrl(df) : options.component_url;
    // If we don't know the url, fall back on the old launching method.
    if (!url.empty()) {
      // Otherwise, launch the component way.
      auto exposed_dir_or =
          ConnectFsComponent(url, *options.component_child_name, options.component_collection_name);
      if (exposed_dir_or.is_error()) {
        return exposed_dir_or.status_value();
      }
      return FsckComponentFs(*exposed_dir_or, device_path_str, options).status_value();
    }
  }

  switch (df) {
    case kDiskFormatFactoryfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("factoryfs").c_str());
    case kDiskFormatMinfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("minfs").c_str());
    case kDiskFormatFxfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("fxfs").c_str());
    case kDiskFormatFat:
      return FsckFat(device_path_str.c_str(), options, cb);
    case kDiskFormatBlobfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("blobfs").c_str());
    case kDiskFormatF2fs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("f2fs").c_str());
    default:
      auto* format = CustomDiskFormat::Get(df);
      if (format == nullptr)
        return ZX_ERR_NOT_SUPPORTED;
      return FsckNativeFs(device_path_str.c_str(), options, cb, format->binary_path().c_str());
  }
}

}  // namespace fs_management
