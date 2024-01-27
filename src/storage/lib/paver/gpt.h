// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_STORAGE_LIB_PAVER_GPT_H_
#define SRC_STORAGE_LIB_PAVER_GPT_H_

#include <lib/component/incoming/cpp/clone.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>

#include <string_view>

#include <gpt/gpt.h>

#include "src/lib/uuid/uuid.h"
#include "src/storage/lib/paver/device-partitioner.h"

namespace paver {

using gpt::GptDevice;

// Useful for when a GPT table is available (e.g. x86 devices). Provides common
// utility functions.
class GptDevicePartitioner {
 public:
  using FilterCallback = fit::function<bool(const gpt_partition_t&)>;

  struct InitializeGptResult {
    std::unique_ptr<GptDevicePartitioner> gpt;
    bool initialize_partition_tables;
  };

  // TODO(https://fxbug.dev/127870): Replace this function with one that uses channels.
  //
  // Find and initialize a GPT based device.
  //
  // If block_device is provided, then search is skipped, and block_device is used
  // directly. If it is not provided, we search for a device with a valid GPT,
  // with an entry for an FVM. If multiple devices with valid GPT containing
  // FVM entries are found, an error is returned.
  static zx::result<InitializeGptResult> InitializeGptWithFd(
      fbl::unique_fd devfs_root, fidl::UnownedClientEnd<fuchsia_io::Directory> svc_root,
      const fbl::unique_fd& block_device);

  // Returns block info for a specified block device.
  const fuchsia_hardware_block::wire::BlockInfo& GetBlockInfo() const { return block_info_; }

  GptDevice* GetGpt() const { return gpt_.get(); }

  struct FindFirstFitResult {
    size_t start;
    size_t length;
  };

  // Find the first spot that has at least |bytes_requested| of space.
  //
  // Returns the |start_out| block and |length_out| blocks, indicating
  // how much space was found, on success. This may be larger than
  // the number of bytes requested.
  zx::result<FindFirstFitResult> FindFirstFit(size_t bytes_requested) const;

  // Creates a partition, adds an entry to the GPT, and returns a file descriptor to it.
  // Assumes that the partition does not already exist.
  zx::result<std::unique_ptr<PartitionClient>> AddPartition(const char* name,
                                                            const uuid::Uuid& type,
                                                            size_t minimum_size_bytes,
                                                            size_t optional_reserve_bytes) const;

  struct FindPartitionResult {
    std::unique_ptr<BlockDevicePartitionClient> partition;
    gpt_partition_t* gpt_partition;
  };

  // Returns a file descriptor to a partition which can be paved,
  // if one exists.
  zx::result<FindPartitionResult> FindPartition(FilterCallback filter) const;

  // Wipes a specified partition from the GPT, and overwrites first 8KiB with
  // nonsense.
  zx::result<> WipeFvm() const;

  // Removes all partitions from GPT.
  zx::result<> WipePartitionTables() const;

  // Wipes all partitions meeting given criteria.
  zx::result<> WipePartitions(FilterCallback filter) const;

  const fbl::unique_fd& devfs_root() { return devfs_root_; }

  fidl::UnownedClientEnd<fuchsia_io::Directory> svc_root() { return svc_root_; }

  // FIDL clients for a block device that could contain a GPT.
  struct GptClients {
    std::string topological_path;
    fidl::ClientEnd<fuchsia_hardware_block::Block> block;
    fidl::ClientEnd<fuchsia_device::Controller> controller;
  };

  // Find all block devices which could contain a GPT.
  static zx::result<std::vector<GptClients>> FindGptDevices(const fbl::unique_fd& devfs_root);

 private:
  using GptFds = std::vector<std::pair<std::string, fbl::unique_fd>>;
  // Find all block devices which could contain a GPT.
  // TODO(fxbug.dev/127870): Replace usages of this with FindGptDevices.
  static bool FindGptFds(const fbl::unique_fd& devfs_root, GptFds* out);

  // Initializes GPT for a device which was explicitly provided. If |gpt_device| doesn't have a
  // valid GPT, it will initialize it with a valid one.
  static zx::result<std::unique_ptr<GptDevicePartitioner>> InitializeProvidedGptDevice(
      fbl::unique_fd devfs_root, fidl::UnownedClientEnd<fuchsia_io::Directory> svc_root,
      fbl::unique_fd gpt_device);

  GptDevicePartitioner(fbl::unique_fd devfs_root,
                       fidl::UnownedClientEnd<fuchsia_io::Directory> svc_root,
                       std::unique_ptr<GptDevice> gpt,
                       fuchsia_hardware_block::wire::BlockInfo block_info)
      : devfs_root_(std::move(devfs_root)),
        svc_root_(component::MaybeClone(svc_root)),
        gpt_(std::move(gpt)),
        block_info_(block_info) {}

  zx::result<uuid::Uuid> CreateGptPartition(const char* name, const uuid::Uuid& type,
                                            uint64_t offset, uint64_t blocks) const;

  const fbl::unique_fd devfs_root_;
  fidl::ClientEnd<fuchsia_io::Directory> svc_root_;
  mutable std::unique_ptr<GptDevice> gpt_;
  fuchsia_hardware_block::wire::BlockInfo block_info_;
};

zx::result<uuid::Uuid> GptPartitionType(Partition type,
                                        PartitionScheme scheme = PartitionScheme::kLegacy);

// TODO(69527): Remove this and migrate usages to |utf16_to_utf8|
inline void utf16_to_cstring(char* dst, const uint8_t* src, size_t charcount) {
  while (charcount > 0) {
    *dst++ = *src;
    src += 2;
    charcount -= 2;
  }
}

inline bool FilterByType(const gpt_partition_t& part, const uuid::Uuid& type) {
  return type == uuid::Uuid(part.type);
}

bool FilterByName(const gpt_partition_t& part, std::string_view name);

bool FilterByTypeAndName(const gpt_partition_t& part, const uuid::Uuid& type,
                         std::string_view name);

inline bool IsFvmPartition(const gpt_partition_t& part) {
  return FilterByType(part, GUID_FVM_VALUE) ||
         FilterByTypeAndName(part, GPT_FVM_TYPE_GUID, GPT_FVM_NAME);
}

// Returns true if the spec partition is Zircon A/B/R.
inline bool IsZirconPartitionSpec(const PartitionSpec& spec) {
  return spec.partition == Partition::kZirconA || spec.partition == Partition::kZirconB ||
         spec.partition == Partition::kZirconR;
}

}  // namespace paver

#endif  // SRC_STORAGE_LIB_PAVER_GPT_H_
