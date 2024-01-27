// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/fs_management/cpp/fvm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/limits.h>
#include <lib/fdio/vfs.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/stdcompat/string_view.h>
#include <string.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <fbl/string_printf.h>
#include <fbl/unique_fd.h>

#include "src/lib/storage/block_client/cpp/remote_block_device.h"
#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/storage/fs_management/cpp/fvm_internal.h"
#include "src/storage/fvm/fvm.h"

namespace fs_management {

namespace {

constexpr std::string_view kBlockDevPath = "/dev/class/block/";
constexpr std::string_view kBlockDevRelativePath = "class/block/";

constexpr zx_duration_t kOpenPartitionTimeout = ZX_SEC(30);

// Overwrites the FVM and waits for it to disappear from devfs.
//
// devfs_root_fd: (OPTIONAL) A connection to devfs. If supplied, |path| is relative to this root.
// parent_fd: An fd to the parent of the FVM device.
// path: The path to the FVM device. Relative to |devfs_root_fd| if supplied.
zx_status_t DestroyFvmAndWait(int devfs_root_fd, fbl::unique_fd parent_fd, fbl::unique_fd driver_fd,
                              std::string_view path) {
  auto volume_info_or = fs_management::FvmQuery(driver_fd.get());
  if (volume_info_or.is_error()) {
    return ZX_ERR_WRONG_TYPE;
  }

  struct FvmDestroyer {
    int devfs_root_fd;
    uint64_t slice_size;
    std::string_view path;
    bool destroyed;
  } destroyer;
  destroyer.devfs_root_fd = devfs_root_fd;
  destroyer.slice_size = volume_info_or->slice_size;
  destroyer.path = path;
  destroyer.destroyed = false;

  auto cb = [](int dirfd, int event, const char* fn, void* cookie) {
    auto destroyer = static_cast<FvmDestroyer*>(cookie);
    if (event == WATCH_EVENT_WAITING) {
      zx_status_t status = ZX_ERR_INTERNAL;
      if (destroyer->devfs_root_fd != -1) {
        status =
            FvmOverwriteWithDevfs(destroyer->devfs_root_fd, destroyer->path, destroyer->slice_size);
      } else {
        status = FvmOverwrite(destroyer->path, destroyer->slice_size);
      }
      destroyer->destroyed = true;
      return status;
    }
    if ((event == WATCH_EVENT_REMOVE_FILE) && !strcmp(fn, "fvm")) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };
  if (zx_status_t status =
          fdio_watch_directory(parent_fd.get(), cb, zx::time::infinite().get(), &destroyer);
      status != ZX_ERR_STOP) {
    return status;
  }
  return ZX_OK;
}

// Helper function to overwrite FVM given the slice_size
zx_status_t FvmOverwriteImpl(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device,
                             size_t slice_size) {
  const fidl::WireResult result = fidl::WireCall(device)->GetInfo();
  if (!result.ok()) {
    return result.status();
  }
  const fit::result response = result.value();
  if (response.is_error()) {
    return response.error_value();
  }
  const fuchsia_hardware_block::wire::BlockInfo& block_info = response.value()->info;

  size_t disk_size = block_info.block_count * block_info.block_size;
  fvm::Header header = fvm::Header::FromDiskSize(fvm::kMaxUsablePartitions, disk_size, slice_size);

  // Overwrite all the metadata from the beginning of the device to the start of the data.
  // TODO(jfsulliv) Use MetadataBuffer::BytesNeeded() when that's ready.
  size_t metadata_size = header.GetDataStartOffset();
  std::unique_ptr<uint8_t[]> buf(new uint8_t[metadata_size]);
  memset(buf.get(), 0, metadata_size);

  if (block_client::SingleWriteBytes(device, buf.get(), metadata_size, 0) != ZX_OK) {
    fprintf(stderr, "FvmOverwriteImpl: Failed to write metadata\n");
    return ZX_ERR_IO;
  }

  {
    // TODO(https://fxbug.dev/112484): this relies on multiplexing.
    const fidl::WireResult result =
        fidl::WireCall(fidl::UnownedClientEnd<fuchsia_device::Controller>(device.channel()))
            ->Rebind({});
    if (!result.ok()) {
      return result.status();
    }
    const fit::result response = result.value();
    if (response.is_error()) {
      return response.error_value();
    }
    return ZX_OK;
  }
}

zx_status_t FvmAllocatePartitionImpl(int fvm_fd, const alloc_req_t& request) {
  fdio_cpp::UnownedFdioCaller caller(fvm_fd);

  fuchsia_hardware_block_partition::wire::Guid type_guid;
  memcpy(type_guid.value.data(), request.type, BLOCK_GUID_LEN);
  fuchsia_hardware_block_partition::wire::Guid instance_guid;
  memcpy(instance_guid.value.data(), request.guid, BLOCK_GUID_LEN);

  fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager> client(
      caller.borrow_channel());
  auto response = fidl::WireCall(client)->AllocatePartition(
      request.slice_count, type_guid, instance_guid, request.name, request.flags);
  if (response.status() != ZX_OK) {
    return response.status();
  }
  if (response.value().status != ZX_OK) {
    return response.value().status;
  }
  return ZX_OK;
}

// Takes ownership of |dir|.
zx::result<fbl::unique_fd> OpenPartitionImpl(DIR* dir, std::string_view out_path_base,
                                             const PartitionMatcher& matcher, zx_duration_t timeout,
                                             std::string* out_path) {
  auto cleanup = fit::defer([&]() { closedir(dir); });

  struct AllocHelperInfo {
    const PartitionMatcher& matcher;
    std::string_view out_path_base;
    std::string* out_path;
    fbl::unique_fd out_partition;
  } info = {
      .matcher = matcher,
      .out_path_base = out_path_base,
      .out_path = out_path,
  };

  auto cb = [](int dirfd, int event, const char* fn, void* cookie) {
    if (event != WATCH_EVENT_ADD_FILE || std::string_view{fn} == ".") {
      return ZX_OK;
    }
    auto& info = *static_cast<AllocHelperInfo*>(cookie);
    fdio_cpp::UnownedFdioCaller caller(dirfd);
    zx::result channel = component::ConnectAt<fuchsia_device::Controller>(caller.directory(), fn);
    if (channel.is_error()) {
      return channel.status_value();
    }
    if (PartitionMatches(channel.value(), info.matcher)) {
      if (zx_status_t status = fdio_fd_create(channel.value().TakeChannel().release(),
                                              info.out_partition.reset_and_get_address());
          status != ZX_OK) {
        return status;
      }
      if (info.out_path != nullptr) {
        *info.out_path = std::string(info.out_path_base).append(fn);
      }
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };

  zx_time_t deadline = zx_deadline_after(timeout);
  if (zx_status_t status = fdio_watch_directory(dirfd(dir), cb, deadline, &info);
      status != ZX_ERR_STOP) {
    return zx::error(status);
  }
  return zx::ok(std::move(info.out_partition));
}

zx_status_t DestroyPartitionImpl(
    fidl::UnownedClientEnd<fuchsia_hardware_block_volume::Volume> volume) {
  const fidl::WireResult result = fidl::WireCall(volume)->Destroy();
  if (!result.ok()) {
    return result.status();
  }
  return result.value().status;
}

}  // namespace

__EXPORT
bool PartitionMatches(fidl::UnownedClientEnd<fuchsia_device::Controller> channel,
                      const PartitionMatcher& matcher) {
  ZX_ASSERT(!matcher.type_guids.empty() || !matcher.instance_guids.empty() ||
            !matcher.detected_formats.empty() || !matcher.labels.empty() ||
            !matcher.parent_device.empty());

  zx::result partition_endpoints =
      fidl::CreateEndpoints<fuchsia_hardware_block_partition::Partition>();
  if (partition_endpoints.is_error()) {
    return false;
  }
  auto& [partition_client_end, partition_server_end] = partition_endpoints.value();
  if (fidl::OneWayStatus response =
          fidl::WireCall(channel)->ConnectToDeviceFidl(partition_server_end.TakeChannel());
      !response.ok()) {
    return false;
  }

  if (!matcher.type_guids.empty()) {
    const fidl::WireResult result = fidl::WireCall(partition_client_end)->GetTypeGuid();
    if (!result.ok()) {
      return false;
    }
    const fidl::WireResponse response = result.value();
    if (response.status != ZX_OK) {
      return false;
    }
    if (!std::any_of(matcher.type_guids.cbegin(), matcher.type_guids.cend(),
                     [type_guid = response.guid->value](const uuid::Uuid& match_guid) {
                       return std::equal(type_guid.cbegin(), type_guid.cend(), match_guid.cbegin());
                     })) {
      return false;
    }
  }
  if (!matcher.instance_guids.empty()) {
    const fidl::WireResult result = fidl::WireCall(partition_client_end)->GetInstanceGuid();
    if (!result.ok()) {
      return false;
    }
    const fidl::WireResponse response = result.value();
    if (response.status != ZX_OK) {
      return false;
    }
    if (!std::any_of(matcher.instance_guids.cbegin(), matcher.instance_guids.cend(),
                     [instance_guid = response.guid->value](const uuid::Uuid& match_guid) {
                       return std::equal(instance_guid.cbegin(), instance_guid.cend(),
                                         match_guid.cbegin());
                     })) {
      return false;
    }
  }
  if (!matcher.labels.empty()) {
    const fidl::WireResult result = fidl::WireCall(partition_client_end)->GetName();
    if (!result.ok()) {
      return false;
    }
    const fidl::WireResponse response = result.value();
    if (response.status != ZX_OK) {
      return false;
    }

    if (!std::any_of(matcher.labels.cbegin(), matcher.labels.cend(),
                     [part_label = response.name.get()](std::string_view match_label) {
                       return part_label == match_label;
                     })) {
      return false;
    }
  }

  std::string topological_path;
  if (!matcher.parent_device.empty() || !matcher.ignore_prefix.empty() ||
      !matcher.ignore_if_path_contains.empty()) {
    const auto resp = fidl::WireCall(channel)->GetTopologicalPath();
    if (!resp.ok() || resp->is_error()) {
      return false;
    }

    topological_path = std::string(resp->value()->path.data(), resp->value()->path.size());
  }
  const std::string_view path(topological_path);
  if (!matcher.parent_device.empty() && !cpp20::starts_with(path, matcher.parent_device)) {
    return false;
  }
  if (!matcher.ignore_prefix.empty() && cpp20::starts_with(path, matcher.ignore_prefix)) {
    return false;
  }
  if (!matcher.ignore_if_path_contains.empty() &&
      path.find(matcher.ignore_if_path_contains) != std::string::npos) {
    return false;
  }
  if (!matcher.detected_formats.empty()) {
    // TODO(https://fxbug.dev/122007): avoid this cast
    const DiskFormat part_format =
        DetectDiskFormat(fidl::UnownedClientEnd<fuchsia_hardware_block::Block>(
            partition_client_end.borrow().channel()));
    if (!std::any_of(
            matcher.detected_formats.cbegin(), matcher.detected_formats.cend(),
            [part_format](const DiskFormat match_format) { return part_format == match_format; })) {
      return false;
    }
  }
  return true;
}

__EXPORT
zx_status_t FvmInitPreallocated(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device,
                                uint64_t initial_volume_size, uint64_t max_volume_size,
                                size_t slice_size) {
  if (slice_size % fvm::kBlockSize != 0) {
    // Alignment
    return ZX_ERR_INVALID_ARGS;
  }
  if ((slice_size * fvm::kMaxVSlices) / fvm::kMaxVSlices != slice_size) {
    // Overflow
    return ZX_ERR_INVALID_ARGS;
  }
  if (initial_volume_size > max_volume_size || initial_volume_size == 0 || max_volume_size == 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  fvm::Header header = fvm::Header::FromGrowableDiskSize(
      fvm::kMaxUsablePartitions, initial_volume_size, max_volume_size, slice_size);
  if (header.pslice_count == 0) {
    return ZX_ERR_NO_SPACE;
  }

  // This buffer needs to hold both copies of the metadata.
  // TODO(fxbug.dev/60709): Eliminate layout assumptions.
  size_t metadata_allocated_bytes = header.GetMetadataAllocatedBytes();
  size_t dual_metadata_bytes = metadata_allocated_bytes * 2;
  std::unique_ptr<uint8_t[]> mvmo(new uint8_t[dual_metadata_bytes]);
  // Clear entire primary copy of metadata
  memset(mvmo.get(), 0, metadata_allocated_bytes);

  // Save the header to our primary metadata.
  memcpy(mvmo.get(), &header, sizeof(fvm::Header));
  size_t metadata_used_bytes = header.GetMetadataUsedBytes();
  fvm::UpdateHash(mvmo.get(), metadata_used_bytes);

  // Copy the new primary metadata to the backup copy.
  void* backup = mvmo.get() + header.GetSuperblockOffset(fvm::SuperblockType::kSecondary);
  memcpy(backup, mvmo.get(), metadata_allocated_bytes);

  // Validate our new state.
  if (!fvm::PickValidHeader(mvmo.get(), backup, metadata_used_bytes)) {
    return ZX_ERR_BAD_STATE;
  }

  // Write to primary copy.
  auto status = block_client::SingleWriteBytes(device, mvmo.get(), metadata_allocated_bytes, 0);
  if (status != ZX_OK) {
    return status;
  }
  // Write to secondary copy, to overwrite any previous FVM metadata copy that
  // could be here.
  return block_client::SingleWriteBytes(device, mvmo.get(), metadata_allocated_bytes,
                                        metadata_allocated_bytes);
}

__EXPORT
zx_status_t FvmInitWithSize(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device,
                            uint64_t volume_size, size_t slice_size) {
  return FvmInitPreallocated(device, volume_size, volume_size, slice_size);
}

__EXPORT
zx_status_t FvmInit(fidl::UnownedClientEnd<fuchsia_hardware_block::Block> device,
                    size_t slice_size) {
  // The metadata layout of the FVM is dependent on the
  // size of the FVM's underlying partition.
  const fidl::WireResult result = fidl::WireCall(device)->GetInfo();
  if (!result.ok()) {
    return result.status();
  }
  const fit::result response = result.value();
  if (response.is_error()) {
    return response.error_value();
  }
  const fuchsia_hardware_block::wire::BlockInfo& block_info = response.value()->info;
  if (slice_size == 0 || slice_size % block_info.block_size) {
    return ZX_ERR_BAD_STATE;
  }

  return FvmInitWithSize(device, block_info.block_count * block_info.block_size, slice_size);
}

__EXPORT
zx_status_t FvmOverwrite(std::string_view path, size_t slice_size) {
  zx::result device = component::Connect<fuchsia_hardware_block::Block>(path);
  if (device.is_error()) {
    return device.status_value();
  }
  return FvmOverwriteImpl(device.value(), slice_size);
}

__EXPORT
zx_status_t FvmOverwriteWithDevfs(int devfs_root_fd, std::string_view relative_path,
                                  size_t slice_size) {
  fdio_cpp::UnownedFdioCaller caller(devfs_root_fd);
  zx::result device =
      component::ConnectAt<fuchsia_hardware_block::Block>(caller.directory(), relative_path);
  if (device.is_error()) {
    return device.status_value();
  }
  return FvmOverwriteImpl(device.value(), slice_size);
}

// Helper function to destroy FVM
__EXPORT
zx_status_t FvmDestroy(std::string_view path) {
  fbl::String driver_path = fbl::StringPrintf("%s/fvm", path.data());

  fbl::unique_fd parent_fd(open(path.data(), O_RDONLY | O_DIRECTORY));
  if (!parent_fd) {
    return ZX_ERR_NOT_FOUND;
  }
  fbl::unique_fd fvm_fd(open(driver_path.c_str(), O_RDONLY));
  if (!fvm_fd) {
    return ZX_ERR_NOT_FOUND;
  }
  return DestroyFvmAndWait(-1, std::move(parent_fd), std::move(fvm_fd), path);
}

__EXPORT
zx_status_t FvmDestroyWithDevfs(int devfs_root_fd, std::string_view relative_path) {
  fbl::String driver_path = fbl::StringPrintf("%s/fvm", relative_path.data());

  fbl::unique_fd parent_fd(openat(devfs_root_fd, relative_path.data(), O_RDONLY | O_DIRECTORY));
  if (!parent_fd) {
    return ZX_ERR_NOT_FOUND;
  }
  fbl::unique_fd fvm_fd(openat(devfs_root_fd, driver_path.c_str(), O_RDONLY));
  if (!fvm_fd) {
    return ZX_ERR_NOT_FOUND;
  }
  return DestroyFvmAndWait(devfs_root_fd, std::move(parent_fd), std::move(fvm_fd), relative_path);
}

// Helper function to allocate, find, and open VPartition.
__EXPORT
zx::result<fbl::unique_fd> FvmAllocatePartition(int fvm_fd, const alloc_req_t& request) {
  if (zx_status_t status = FvmAllocatePartitionImpl(fvm_fd, request); status != ZX_OK) {
    return zx::error(status);
  }
  const PartitionMatcher matcher{
      .type_guids = {uuid::Uuid(request.type)},
      .instance_guids = {uuid::Uuid(request.guid)},
  };
  return OpenPartition(matcher, kOpenPartitionTimeout, nullptr);
}

__EXPORT
zx::result<fbl::unique_fd> FvmAllocatePartitionWithDevfs(int devfs_root_fd, int fvm_fd,
                                                         const alloc_req_t& request) {
  int alloc_status = FvmAllocatePartitionImpl(fvm_fd, request);
  if (alloc_status != 0) {
    return zx::error(alloc_status);
  }
  PartitionMatcher matcher{
      .type_guids = {uuid::Uuid(request.type)},
      .instance_guids = {uuid::Uuid(request.guid)},
  };
  return OpenPartitionWithDevfs(devfs_root_fd, matcher, kOpenPartitionTimeout, nullptr);
}

__EXPORT
zx::result<fuchsia_hardware_block_volume::wire::VolumeManagerInfo> FvmQuery(int fvm_fd) {
  fdio_cpp::UnownedFdioCaller caller(fvm_fd);

  const fidl::WireResult result =
      fidl::WireCall(caller.borrow_as<fuchsia_hardware_block_volume::VolumeManager>())->GetInfo();
  if (!result.ok()) {
    return zx::error(result.status());
  }
  const fidl::WireResponse response = result.value();
  if (zx_status_t status = response.status; status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(*response.info);
}

__EXPORT
zx::result<fbl::unique_fd> OpenPartition(const PartitionMatcher& matcher, zx_duration_t timeout,
                                         std::string* out_path) {
  DIR* dir = opendir(kBlockDevPath.data());
  if (dir == nullptr) {
    return zx::error(ZX_ERR_IO);
  }

  return OpenPartitionImpl(dir, kBlockDevPath, matcher, timeout, out_path);
}

__EXPORT
zx::result<fbl::unique_fd> OpenPartitionWithDevfs(int devfs_root_fd,
                                                  const PartitionMatcher& matcher,
                                                  zx_duration_t timeout,
                                                  std::string* out_path_relative) {
  fbl::unique_fd block_dev_fd(openat(devfs_root_fd, kBlockDevRelativePath.data(), O_RDONLY));
  if (!block_dev_fd) {
    return zx::error(ZX_ERR_IO);
  }
  DIR* dir = fdopendir(block_dev_fd.get());
  if (dir == nullptr) {
    return zx::error(ZX_ERR_IO);
  }

  return OpenPartitionImpl(dir, kBlockDevRelativePath, matcher, timeout, out_path_relative);
}

__EXPORT
zx_status_t DestroyPartition(const PartitionMatcher& matcher) {
  zx::result fd = OpenPartition(matcher, 0, nullptr);
  if (fd.is_error()) {
    return fd.status_value();
  }
  fdio_cpp::FdioCaller caller(std::move(fd.value()));
  return DestroyPartitionImpl(caller.borrow_as<fuchsia_hardware_block_volume::Volume>());
}

__EXPORT
zx_status_t DestroyPartitionWithDevfs(int devfs_root_fd, const PartitionMatcher& matcher) {
  zx::result fd = OpenPartitionWithDevfs(devfs_root_fd, matcher, 0, nullptr);
  if (fd.is_error()) {
    return fd.status_value();
  }
  fdio_cpp::FdioCaller caller(std::move(fd.value()));
  return DestroyPartitionImpl(caller.borrow_as<fuchsia_hardware_block_volume::Volume>());
}

__EXPORT
zx_status_t FvmActivate(int fvm_fd, fuchsia_hardware_block_partition::wire::Guid deactivate,
                        fuchsia_hardware_block_partition::wire::Guid activate) {
  fdio_cpp::UnownedFdioCaller caller(fvm_fd);
  fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager> client(
      caller.borrow_channel());
  auto response = fidl::WireCall(client)->Activate(deactivate, activate);
  if (response.status() != ZX_OK) {
    return response.status();
  }
  if (response.value().status != ZX_OK) {
    return response.value().status;
  }
  return ZX_OK;
}

}  // namespace fs_management
