// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/service/startup.h"

#include <lib/fidl-async/cpp/bind.h>
#include <lib/syslog/cpp/macros.h>

#include "src/lib/storage/block_client/cpp/remote_block_device.h"
#include "src/storage/blobfs/fsck.h"
#include "src/storage/blobfs/mkfs.h"
#include "src/storage/blobfs/mount.h"

namespace blobfs {
namespace {

MountOptions ParseMountOptions(fuchsia_fs_startup::wire::StartOptions start_options) {
  MountOptions options;

  options.verbose = start_options.verbose;
  options.sandbox_decompression = start_options.sandbox_decompression;

  if (start_options.read_only) {
    options.writability = Writability::ReadOnlyFilesystem;
  }
  if (start_options.write_compression_level >= 0) {
    options.compression_settings.compression_level = start_options.write_compression_level;
  }

  switch (start_options.write_compression_algorithm) {
    case fuchsia_fs_startup::wire::CompressionAlgorithm::kZstdChunked:
      options.compression_settings.compression_algorithm = CompressionAlgorithm::kChunked;
      break;
    case fuchsia_fs_startup::wire::CompressionAlgorithm::kUncompressed:
      options.compression_settings.compression_algorithm = CompressionAlgorithm::kUncompressed;
      break;
    default:
      ZX_PANIC("Unknown compression algorithm: %d",
               static_cast<uint32_t>(start_options.write_compression_algorithm));
  }

  switch (start_options.cache_eviction_policy_override) {
    case fuchsia_fs_startup::wire::EvictionPolicyOverride::kNone:
      options.pager_backed_cache_policy = std::nullopt;
      break;
    case fuchsia_fs_startup::wire::EvictionPolicyOverride::kNeverEvict:
      options.pager_backed_cache_policy = CachePolicy::NeverEvict;
      break;
    case fuchsia_fs_startup::wire::EvictionPolicyOverride::kEvictImmediately:
      options.pager_backed_cache_policy = CachePolicy::EvictImmediately;
      break;
    default:
      ZX_PANIC("Unknown cache eviction policy override: %d",
               static_cast<uint32_t>(start_options.cache_eviction_policy_override));
  }

  return options;
}

FilesystemOptions ParseFormatOptions(fuchsia_fs_startup::wire::FormatOptions format_options) {
  FilesystemOptions options;

  if (format_options.num_inodes > 0) {
    options.num_inodes = format_options.num_inodes;
  }
  if (format_options.deprecated_padded_blobfs_format) {
    options.blob_layout_format = BlobLayoutFormat::kDeprecatedPaddedMerkleTreeAtStart;
  }

  return options;
}

MountOptions MergeComponentConfigIntoMountOptions(const ComponentOptions& config,
                                                  MountOptions options) {
  options.paging_threads = std::max(1, config.pager_threads);
  return options;
}

}  // namespace

StartupService::StartupService(async_dispatcher_t* dispatcher, const ComponentOptions& config,
                               ConfigureCallback cb)
    : fs::Service([dispatcher, this](fidl::ServerEnd<fuchsia_fs_startup::Startup> server_end) {
        return fidl::BindSingleInFlightOnly(dispatcher, std::move(server_end), this);
      }),
      component_config_(config),
      configure_(std::move(cb)) {}

void StartupService::Start(StartRequestView request, StartCompleter::Sync& completer) {
  // Use a closure to ensure that any sessions created are destroyed before we respond to the
  // request.
  //
  // TODO(https://fxbug.dev/97783): Consider removing this when multiple sessions are permitted.
  zx::result<> result = [&]() -> zx::result<> {
    std::unique_ptr<block_client::RemoteBlockDevice> device;

// TODO(fxbug.dev/121465): Use  Volume instead of Block.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    zx_status_t status =
        block_client::RemoteBlockDevice::Create(std::move(request->device), &device);
#ifdef __clang__
#pragma clang diagnostic pop
#elif __GNUC__
#pragma GCC diagnostic pop
#endif
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Could not initialize block device";
      return zx::error(status);
    }
    return configure_(std::move(device),
                      MergeComponentConfigIntoMountOptions(component_config_,
                                                           ParseMountOptions(request->options)));
  }();
  completer.Reply(result);
}

void StartupService::Format(FormatRequestView request, FormatCompleter::Sync& completer) {
  // Use a closure to ensure that any sessions created are destroyed before we respond to the
  // request.
  //
  // TODO(https://fxbug.dev/97783): Consider removing this when multiple sessions are permitted.
  zx::result<> result = [&]() -> zx::result<> {
    std::unique_ptr<block_client::RemoteBlockDevice> device;

// TODO(fxbug.dev/121465): Use  Volume instead of Block.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    zx_status_t status =
        block_client::RemoteBlockDevice::Create(std::move(request->device), &device);
#ifdef __clang__
#pragma clang diagnostic pop
#elif __GNUC__
#pragma GCC diagnostic pop
#endif
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Could not initialize block device: " << zx_status_get_string(status);
      return zx::error(status);
    }
    status = FormatFilesystem(device.get(), ParseFormatOptions(request->options));
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to format blobfs: " << zx_status_get_string(status);
      return zx::error(status);
    }
    return zx::ok();
  }();
  completer.Reply(result);
}

void StartupService::Check(CheckRequestView request, CheckCompleter::Sync& completer) {
  // Use a closure to ensure that any sessions created are destroyed before we respond to the
  // request.
  //
  // TODO(https://fxbug.dev/97783): Consider removing this when multiple sessions are permitted.
  zx::result<> result = [&]() -> zx::result<> {
    std::unique_ptr<block_client::RemoteBlockDevice> device;

// TODO(fxbug.dev/121465): Use  Volume instead of Block.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    zx_status_t status =
        block_client::RemoteBlockDevice::Create(std::move(request->device), &device);
#ifdef __clang__
#pragma clang diagnostic pop
#elif __GNUC__
#pragma GCC diagnostic pop
#endif
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Could not initialize block device";
      return zx::error(status);
    }
    // Blobfs supports none of the check options.
    MountOptions options;
    status = Fsck(std::move(device), options);
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Consistency check failed for blobfs";
      return zx::error(status);
    }
    return zx::ok();
  }();
  completer.Reply(result);
}

}  // namespace blobfs
