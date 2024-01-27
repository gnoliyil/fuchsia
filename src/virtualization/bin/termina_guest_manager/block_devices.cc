// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/termina_guest_manager/block_devices.h"

#include <dirent.h>
#include <fcntl.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/namespace.h>
#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <zircon/hw/gpt.h>

#include <filesystem>

#include <fbl/unique_fd.h>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/storage/block_client/cpp/remote_block_device.h"

namespace {

namespace fio = fuchsia_io;

constexpr size_t kNumRetries = 5;
constexpr auto kRetryDelay = zx::msec(100);

constexpr const char kBlockPath[] = "/dev/class/block";
constexpr auto kGuidSize = fuchsia::hardware::block::partition::GUID_LENGTH;
constexpr std::array<uint8_t, kGuidSize> kFvmGuid = GUID_FVM_VALUE;
constexpr std::array<uint8_t, kGuidSize> kGptFvmGuid = GPT_FVM_TYPE_GUID;

using VolumeHandle = fidl::InterfaceHandle<fuchsia::hardware::block::volume::Volume>;
using ManagerHandle = fidl::InterfaceHandle<fuchsia::hardware::block::volume::VolumeManager>;

// Information about a disk image.
struct DiskImage {
  const char* path;  // Path to the file containing the image
  bool read_only;
  bool create_file;
};

#if defined(USE_VOLATILE_BLOCK)
constexpr bool kForceVolatileWrites = true;
#else
constexpr bool kForceVolatileWrites = false;
#endif

constexpr DiskImage kBlockFileStatefulImage = DiskImage{
    // NOTE: This assumes the /data directory is using Fxfs
    .path = "/data/fxfs_virtualization_guest_image",
    .read_only = false,
    .create_file = true,
};
constexpr DiskImage kFileStatefulImage = DiskImage{
    .path = "/data/fxfs_virtualization_guest_image",
    .read_only = false,
    .create_file = true,
};

constexpr DiskImage kExtrasImage = DiskImage{
    .path = "/pkg/data/termina_extras.img",
    .read_only = true,
    .create_file = false,
};

// Finds the guest FVM partition, and the FVM GPT partition.
zx::result<std::tuple<VolumeHandle, ManagerHandle>> FindPartitions(DIR* dir) {
  VolumeHandle volume;
  ManagerHandle manager;

  fdio_cpp::UnownedFdioCaller caller(dirfd(dir));
  for (dirent* entry; (entry = readdir(dir)) != nullptr;) {
    fuchsia::hardware::block::partition::PartitionSyncPtr partition;
    zx_status_t status = fdio_service_connect_at(caller.borrow_channel(), entry->d_name,
                                                 partition.NewRequest().TakeChannel().release());
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to connect to '" << entry->d_name
                     << "': " << zx_status_get_string(status);
      return zx::error(status);
    }

    zx_status_t guid_status;
    std::unique_ptr<fuchsia::hardware::block::partition::Guid> guid;
    status = partition->GetTypeGuid(&guid_status, &guid);
    if (status != ZX_OK || guid_status != ZX_OK || !guid) {
      continue;
    }
    if (std::equal(kGuestPartitionGuid.begin(), kGuestPartitionGuid.end(), guid->value.begin())) {
      // If we find the guest FVM partition, then we can break out of the loop.
      // We only need to find the FVM GPT partition if there is no guest FVM
      // partition, in order to create the guest FVM partition.
      volume.set_channel(partition.Unbind().TakeChannel());
      break;
    }
    if (std::equal(kFvmGuid.begin(), kFvmGuid.end(), guid->value.begin()) ||
        std::equal(kGptFvmGuid.begin(), kGptFvmGuid.end(), guid->value.begin())) {
      fuchsia::device::ControllerSyncPtr controller;
      controller.Bind(partition.Unbind().TakeChannel());
      fuchsia::device::Controller_GetTopologicalPath_Result topo_result;
      status = controller->GetTopologicalPath(&topo_result);
      if (status != ZX_OK || topo_result.is_err()) {
        FX_LOGS(ERROR) << "Failed to get topological path for '" << entry->d_name << "'";
        return zx::error(ZX_ERR_IO);
      }

      auto fvm_path = topo_result.response().path + "/fvm";
      status = fdio_service_connect(fvm_path.data(), manager.NewRequest().TakeChannel().release());
      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to connect to '" << fvm_path
                       << "': " << zx_status_get_string(status);
        return zx::error(status);
      }
    }
  }

  return zx::ok(std::make_tuple(std::move(volume), std::move(manager)));
}

// Waits for the guest partition to be allocated.
//
// TODO(fxbug.dev/90469): Use a directory watcher instead of scanning for
// new partitions.
zx::result<VolumeHandle> WaitForPartition(DIR* dir) {
  for (size_t retry = 0; retry != kNumRetries; retry++) {
    auto partitions = FindPartitions(dir);
    if (partitions.is_error()) {
      return partitions.take_error();
    }
    auto& [volume, manager] = *partitions;
    if (volume) {
      return zx::ok(std::move(volume));
    }
    zx::nanosleep(zx::deadline_after(kRetryDelay));
  }
  FX_LOGS(ERROR) << "Failed to create guest partition";
  return zx::error(ZX_ERR_IO);
}

// Locates the FVM partition for a guest block device. If a partition does not
// exist, allocate one.
zx::result<VolumeHandle> FindOrAllocatePartition(std::string_view path, size_t partition_size,
                                                 size_t min_size) {
  auto dir = opendir(path.data());
  if (dir == nullptr) {
    FX_LOGS(ERROR) << "Failed to open directory '" << path << "'";
    return zx::error(ZX_ERR_IO);
  }
  auto defer = fit::defer([dir] { closedir(dir); });

  auto partitions = FindPartitions(dir);
  if (partitions.is_error()) {
    return partitions.take_error();
  }
  auto& [volume, manager] = *partitions;

  if (!volume) {
    if (!manager) {
      FX_LOGS(ERROR) << "Failed to find FVM";
      return zx::error(ZX_ERR_NOT_FOUND);
    }
    auto sync = manager.BindSync();
    zx_status_t info_status = ZX_OK;
    // Get the partition slice size.
    std::unique_ptr<fuchsia::hardware::block::volume::VolumeManagerInfo> info;
    zx_status_t status = sync->GetInfo(&info_status, &info);
    if (status != ZX_OK || info_status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to get volume info: " << zx_status_get_string(status) << " and "
                     << zx_status_get_string(info_status);
      return zx::error(ZX_ERR_IO);
    }

    // Round up to the next full slice.
    size_t slices = (partition_size + info->slice_size - 1) / info->slice_size;

    // Avoid allocating more than 90% of the disk space. This is a somewhat arbitrary limit prevent
    // fully consuming  all remaining disk space for the VM.
    size_t available_slices = info->slice_count - info->assigned_slice_count;
    size_t usable_slices = available_slices * 9 / 10;
    size_t usable_bytes = usable_slices * info->slice_size;
    if (usable_bytes < min_size) {
      FX_LOGS(ERROR) << "Only " << usable_bytes << "b usable (limited to 90%% of available space);"
                     << " unable to allocate disk";
      return zx::error(ZX_ERR_NO_SPACE);
    }

    if (slices > usable_slices) {
      size_t requested_bytes = slices * info->slice_size;
      FX_LOGS(WARNING) << "Requested disk of " << requested_bytes << "b but only " << usable_bytes
                       << "b usable (90%% of avilable space); clamping";
      FX_LOGS(WARNING) << "\tslice_size " << info->slice_size;
      FX_LOGS(WARNING) << "\tslice_count " << info->slice_count;
      FX_LOGS(WARNING) << "\tassigned_slice_count " << info->assigned_slice_count;
      FX_LOGS(WARNING) << "\tmaxiumum_slice_count " << info->maximum_slice_count;
      FX_LOGS(WARNING) << "\tmax_virtual_slice " << info->max_virtual_slice;
      slices = usable_slices;
    }

    zx_status_t part_status = ZX_OK;
    status = sync->AllocatePartition(slices, {.value = kGuestPartitionGuid}, {},
                                     kGuestPartitionName, 0, &part_status);
    if (status != ZX_OK || part_status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to allocate partition: " << zx_status_get_string(status) << " and "
                     << zx_status_get_string(part_status);
      return zx::error(ZX_ERR_IO);
    }
    return WaitForPartition(dir);
  }

  return zx::ok(std::move(volume));
}

// Opens the given disk image.
zx::result<fuchsia::io::FileHandle> GetPartition(const DiskImage& image) {
  TRACE_DURATION("termina_guest_manager", "GetPartition");
  fuchsia::io::OpenFlags flags = fuchsia::io::OpenFlags::RIGHT_READABLE;
  if (!image.read_only) {
    flags |= fuchsia::io::OpenFlags::RIGHT_WRITABLE;
  }
  if (image.create_file) {
    flags |= fuchsia::io::OpenFlags::CREATE;
  }
  fuchsia::io::FileHandle file;
  zx_status_t status = fdio_open(image.path, static_cast<uint32_t>(flags),
                                 file.NewRequest().TakeChannel().release());
  if (status) {
    return zx::error(status);
  }
  return zx::ok(std::move(file));
}

// Opens the given disk image.
zx::result<fidl::InterfaceHandle<fuchsia::hardware::block::Block>> GetFxfsPartition(
    const DiskImage& image, const size_t image_size_bytes) {
  TRACE_DURATION("linux_runner", "GetFxfsPartition");

  // First, use regular file operations to make a huge file at image.path
  // NOTE: image.path is assumed to be a path on an Fxfs filesystem
  fbl::unique_fd fd(open(image.path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR));
  if (!fd) {
    FX_LOGS(ERROR) << "open(image.path) failed with errno: " << strerror(errno);
    return zx::error(ZX_ERR_IO);
  }

  // Make sure the file is the requested size (image_size_bytes).
  // NOTE: This is usually a huge size (e.g. 40 gigabytes).
  off_t existingFilesize = lseek(fd.get(), 0, SEEK_END);
  if (existingFilesize == static_cast<off_t>(-1) ||
      static_cast<size_t>(existingFilesize) < image_size_bytes) {
    if (ftruncate(fd.get(), image_size_bytes) == -1) {
      FX_LOGS(ERROR) << "ftruncate(image.path) failed with errno: " << strerror(errno);
      return zx::error(ZX_ERR_IO);
    }
  }
  if (close(fd.release()) == -1) {
    FX_LOGS(ERROR) << "close(image.path) failed with errno: " << strerror(errno);
    return zx::error(ZX_ERR_IO);
  }

  /// Now we can try to reopen the file, but in block device mode

  /// First we have to open the parent directory...
  auto dir_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (dir_endpoints.status_value() != ZX_OK) {
    FX_PLOGS(ERROR, dir_endpoints.status_value())
        << "CreateEndpoints() for Fxfs parent directory failed";
    return zx::error(dir_endpoints.status_value());
  }
  auto [dir_client, dir_server] = *std::move(dir_endpoints);
  std::filesystem::path image_path(image.path);
  uint32_t dir_flags = static_cast<uint32_t>(
      fio::OpenFlags::kRightReadable | fio::OpenFlags::kRightWritable | fio::OpenFlags::kDirectory);
  zx_status_t dir_open_status =
      fdio_open(image_path.parent_path().c_str(), dir_flags, dir_server.TakeChannel().release());
  if (dir_open_status != ZX_OK) {
    FX_PLOGS(ERROR, dir_open_status) << "fdio_open(Fxfs image.path.parent) failed";
    return zx::error(dir_open_status);
  }

  // We want to open the "file" at image.path, but as a block device (i.e. fuchsia.hardware.block).
  fio::OpenFlags flags = fio::OpenFlags::kRightReadable;
  if (!image.read_only) {
    flags |= fio::OpenFlags::kRightWritable;
  }
  auto device_endpoints = fidl::CreateEndpoints<fuchsia_io::Node>();
  if (device_endpoints.status_value() != ZX_OK) {
    FX_PLOGS(ERROR, device_endpoints.status_value())
        << "CreateEndpoints() for Fxfs block device file failed";
    return zx::error(device_endpoints.status_value());
  }
  auto [device_client, device_server] = *std::move(device_endpoints);
  flags |= fio::OpenFlags::kBlockDevice;
  // TODO(fxbug.dev/103241): Consider using io2 for the Open() call.
  auto device_open_result =
      fidl::WireCall(dir_client)
          ->Open(flags, {}, fidl::StringView::FromExternal(image_path.filename().c_str()),
                 std::move(device_server));
  if (!device_open_result.ok()) {
    FX_PLOGS(ERROR, device_open_result.status())
        << "WireCall->Open(image.path) as Fxfs block device failed";
    return zx::error(device_open_result.status());
  }

  return zx::ok(fuchsia::hardware::block::BlockHandle(device_client.TakeChannel()));
}

}  // namespace

fit::result<std::string, std::vector<fuchsia::virtualization::BlockSpec>> GetBlockDevices(
    const termina_config::Config& structured_config, size_t min_size) {
  TRACE_DURATION("termina_guest_manager", "Guest::GetBlockDevices");

  std::vector<fuchsia::virtualization::BlockSpec> devices;

  const uint64_t stateful_image_size_bytes = structured_config.stateful_partition_size();

  // Get/create the stateful partition.
  fuchsia::virtualization::BlockSpec stateful_spec;
  stateful_spec.id = "stateful";
  FX_LOGS(INFO) << "Adding stateful partition type: "
                << structured_config.stateful_partition_type();
  if (structured_config.stateful_partition_type() == "block-file") {
    // Use a file opened with OpenFlags.BLOCK_DEVICE.
    auto handle = GetFxfsPartition(kBlockFileStatefulImage, stateful_image_size_bytes);
    if (handle.is_error()) {
      return fit::error(
          fxl::StringPrintf("Failed to open or create stateful Fxfs file / block device: %s",
                            zx_status_get_string(handle.error_value())));
    }
    stateful_spec.mode = fuchsia::virtualization::BlockMode::READ_WRITE;
    stateful_spec.format.set_block(std::move(handle.value()));
  } else if (structured_config.stateful_partition_type() == "fvm") {
    // FVM
    auto handle = FindOrAllocatePartition(kBlockPath, stateful_image_size_bytes, min_size);
    if (handle.is_error()) {
      return fit::error(fxl::StringPrintf("Failed to find or allocate a partition: %s",
                                          zx_status_get_string(handle.error_value())));
    }
    stateful_spec.mode = fuchsia::virtualization::BlockMode::READ_WRITE;
    stateful_spec.format.set_block(
        fidl::InterfaceHandle<fuchsia::hardware::block::Block>(handle.value().TakeChannel()));
  } else if (structured_config.stateful_partition_type() == "file") {
    // Simple files.
    auto handle = GetPartition(kFileStatefulImage);
    if (handle.is_error()) {
      return fit::error(fxl::StringPrintf("Failed to open or create stateful file: %s",
                                          zx_status_get_string(handle.error_value())));
    }

    auto ptr = handle->BindSync();
    fuchsia::io::File_Resize_Result resize_result;
    zx_status_t status = ptr->Resize(stateful_image_size_bytes, &resize_result);
    if (status != ZX_OK || resize_result.is_err()) {
      return fit::error(fxl::StringPrintf("Failed resize stateful file: %s/%s",
                                          zx_status_get_string(status),
                                          zx_status_get_string(resize_result.err())));
    }

    stateful_spec.mode = fuchsia::virtualization::BlockMode::READ_WRITE;
    stateful_spec.format.set_file(ptr.Unbind());
  }
  if (kForceVolatileWrites) {
    stateful_spec.mode = fuchsia::virtualization::BlockMode::VOLATILE_WRITE;
  }
  devices.push_back(std::move(stateful_spec));

  // Add the extras partition if it exists.
  auto extras = GetPartition(kExtrasImage);
  if (extras.is_ok()) {
    devices.push_back({
        .id = "extras",
        .mode = fuchsia::virtualization::BlockMode::VOLATILE_WRITE,
        .format = fuchsia::virtualization::BlockFormat::WithFile(std::move(extras.value())),
    });
  }

  return fit::success(std::move(devices));
}

void DropDevNamespace() {
  // Drop access to /dev, in order to prevent any further access.
  fdio_ns_t* ns;
  zx_status_t status = fdio_ns_get_installed(&ns);
  FX_CHECK(status == ZX_OK) << "Failed to get installed namespace";
  if (fdio_ns_is_bound(ns, "/dev")) {
    status = fdio_ns_unbind(ns, "/dev");
    FX_CHECK(status == ZX_OK) << "Failed to unbind '/dev' from the installed namespace";
  }
}

zx::result<> WipeStatefulPartition(size_t bytes_to_zero, uint8_t value,
                                   VolumeAction volume_action) {
  auto dir = opendir(kBlockPath);
  if (dir == nullptr) {
    FX_LOGS(ERROR) << "Failed to open directory '" << kBlockPath << "'";
    return zx::error(ZX_ERR_IO);
  }
  auto defer = fit::defer([dir] { closedir(dir); });

  auto partitions = FindPartitions(dir);
  if (partitions.is_error()) {
    FX_LOGS(ERROR) << "Failed to find partition";
    return zx::error(ZX_ERR_NOT_FOUND);
  }
  auto& [volume, manager] = *partitions;
  if (!volume) {
    FX_LOGS(ERROR) << "Failed to find volume";
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  // For devices that support TRIM, there is a more efficient path we could take. Since we expect
  // to move the stateful partition to fxfs before too long we keep this logic simple and don't
  // attempt to optimize for devices that support TRIM.
  constexpr size_t kWipeBufferSize = 65536;  // 64 KiB write buffer
  uint8_t bytes[kWipeBufferSize];
  memset(&bytes, value, kWipeBufferSize);
  for (size_t offset = 0; offset < bytes_to_zero; offset += kWipeBufferSize) {
    zx_status_t status = block_client::SingleWriteBytes(
        fidl::UnownedClientEnd<fuchsia_hardware_block::Block>(volume.channel().borrow()), bytes,
        std::min(bytes_to_zero - offset, kWipeBufferSize), offset);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to write bytes";
      return zx::error(ZX_ERR_IO);
    }
  }

  // Now deallocate the partition. This will allow us to recreate the FVM partition the next time
  // we start the VM.
  //
  // Note we still need to zero bytes because FVM can reallocate the same slices the next time we
  // create the volume.
  if (volume_action == VolumeAction::REMOVE) {
    auto volume_sync = volume.BindSync();
    zx_status_t fidl_status;
    zx_status_t status = volume_sync->Destroy(&fidl_status);
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Transport Error Destroying FVM partition";
      return zx::error(status);
    }
    if (fidl_status != ZX_OK) {
      FX_PLOGS(ERROR, fidl_status) << "FIDL Error Destroying FVM partition";
      return zx::error(fidl_status);
    }
  }
  return zx::ok();
}
