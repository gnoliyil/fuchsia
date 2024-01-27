// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/testing/ram_disk.h"

#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/fdio/directory.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/job.h>
#include <lib/zx/time.h>
#include <zircon/syscalls.h>

namespace storage {

zx::result<> WaitForRamctl(zx::duration time) {
  if (zx::result channel =
          device_watcher::RecursiveWaitForFile("/dev/sys/platform/00:00:2d/ramctl", time);
      channel.is_error()) {
    FX_PLOGS(ERROR, channel.error_value()) << "Failed to wait for for ramctl";
    return channel.take_error();
  }
  return zx::ok();
}

zx::result<RamDisk> RamDisk::Create(int block_size, uint64_t block_count,
                                    const RamDisk::Options& options) {
  auto status = WaitForRamctl();
  if (status.is_error()) {
    return status.take_error();
  }
  ramdisk_client_t* client;
  if (options.type_guid) {
    status = zx::make_result(ramdisk_create_with_guid(
        block_size, block_count, options.type_guid->data(), options.type_guid->size(), &client));
  } else {
    status = zx::make_result(ramdisk_create(block_size, block_count, &client));
  }
  if (status.is_error()) {
    FX_LOGS(ERROR) << "Could not create ramdisk for test: " << status.status_string();
    return status.take_error();
  }
  return zx::ok(RamDisk(client));
}

zx::result<RamDisk> RamDisk::CreateWithVmo(zx::vmo vmo, uint64_t block_size) {
  auto status = WaitForRamctl();
  if (status.is_error()) {
    return status.take_error();
  }
  ramdisk_client_t* client;
  status = zx::make_result(ramdisk_create_from_vmo_with_params(vmo.release(), block_size,
                                                               /*type_guid*/ nullptr,
                                                               /*guid_len*/ 0, &client));
  if (status.is_error()) {
    FX_LOGS(ERROR) << "Could not create ramdisk for test: " << status.status_string();
    return status.take_error();
  }
  return zx::ok(RamDisk(client));
}

zx::result<fidl::ClientEnd<fuchsia_hardware_block::Block>> RamDisk::channel() const {
  return component::Connect<fuchsia_hardware_block::Block>(path());
}

}  // namespace storage
