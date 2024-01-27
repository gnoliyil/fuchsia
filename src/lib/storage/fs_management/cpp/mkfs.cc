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

#include <iterator>
#include <new>
#include <vector>

#include <fbl/algorithm.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>

#include "src/lib/storage/fs_management/cpp/component.h"
#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/lib/storage/fs_management/cpp/path.h"
#include "src/lib/storage/fs_management/cpp/volumes.h"

namespace fs_management {
namespace {

zx_status_t MkfsNativeFs(const char* binary, const char* device_path, LaunchCallback cb,
                         const MkfsOptions& options) {
  auto block_device = component::Connect<fuchsia_hardware_block::Block>(device_path);
  if (block_device.is_error())
    return block_device.status_value();
  std::vector<std::pair<uint32_t, zx::handle>> handles;
  handles.push_back({FS_HANDLE_BLOCK_DEVICE_ID, block_device->TakeChannel()});
  if (options.crypt_client)
    handles.push_back({PA_HND(PA_USER0, 2), options.crypt_client()});
  return cb(options.as_argv(binary), std::move(handles));
}

zx_status_t MkfsFat(const char* device_path, LaunchCallback cb, const MkfsOptions& options) {
  std::vector<std::string> argv = {GetBinaryPath("block_adapter"), GetBinaryPath("mkfs-msdosfs")};
  if (options.sectors_per_cluster != 0) {
    argv.push_back("-c");
    argv.push_back(std::to_string(options.sectors_per_cluster));
  }
  auto block_device = component::Connect<fuchsia_hardware_block::Block>(device_path);
  if (block_device.is_error())
    return block_device.status_value();
  std::vector<std::pair<uint32_t, zx::handle>> handles;
  handles.push_back({FS_HANDLE_BLOCK_DEVICE_ID, block_device->TakeChannel()});
  return cb(std::move(argv), std::move(handles));
}

zx::result<> MkfsComponentFs(fidl::UnownedClientEnd<fuchsia_io::Directory> exposed_dir,
                             const std::string& device_path, const MkfsOptions& options) {
  auto device = component::Connect<fuchsia_hardware_block::Block>(device_path.c_str());
  if (device.is_error())
    return device.take_error();

  auto startup_client_end = component::ConnectAt<fuchsia_fs_startup::Startup>(exposed_dir);
  if (startup_client_end.is_error())
    return startup_client_end.take_error();
  fidl::WireSyncClient startup_client{std::move(*startup_client_end)};

  fidl::Arena arena;
  auto res = startup_client->Format(std::move(*device), options.as_format_options(arena));
  if (!res.ok())
    return zx::error(res.status());
  if (res->is_error())
    return zx::error(res->error_value());

  return zx::ok();
}

}  // namespace

__EXPORT
zx_status_t Mkfs(const char* device_path, DiskFormat df, LaunchCallback cb,
                 const MkfsOptions& options) {
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
      return MkfsComponentFs(*exposed_dir_or, device_path, options).status_value();
    }
  }

  switch (df) {
    case kDiskFormatFactoryfs:
      return MkfsNativeFs(GetBinaryPath("factoryfs").c_str(), device_path, cb, options);
    case kDiskFormatMinfs:
      return MkfsNativeFs(GetBinaryPath("minfs").c_str(), device_path, cb, options);
    case kDiskFormatFxfs:
      return MkfsNativeFs(GetBinaryPath("fxfs").c_str(), device_path, cb, options);
    case kDiskFormatFat:
      return MkfsFat(device_path, cb, options);
    case kDiskFormatBlobfs:
      return MkfsNativeFs(GetBinaryPath("blobfs").c_str(), device_path, cb, options);
    case kDiskFormatF2fs:
      return MkfsNativeFs(GetBinaryPath("f2fs").c_str(), device_path, cb, options);
    default:
      auto* format = CustomDiskFormat::Get(df);
      if (format == nullptr)
        return ZX_ERR_NOT_SUPPORTED;
      return MkfsNativeFs(format->binary_path().c_str(), device_path, cb, options);
  }
}

}  // namespace fs_management
