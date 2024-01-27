// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/factory/factoryfs/factoryfs.h"

#include <getopt.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/result.h>
#include <zircon/process.h>
#include <zircon/processargs.h>

#include <iostream>

#include <storage/buffer/owned_vmoid.h>

#include "src/lib/storage/block_client/cpp/reader.h"
#include "src/lib/storage/vfs/cpp/trace.h"
#include "src/lib/storage/vfs/cpp/vnode.h"
#include "src/storage/factory/factoryfs/directory.h"
#include "src/storage/factory/factoryfs/file.h"
#include "src/storage/factory/factoryfs/format.h"
#include "src/storage/factory/factoryfs/superblock.h"

namespace factoryfs {

static zx_status_t IsValidDirectoryEntry(const DirectoryEntry& entry, const Superblock& info) {
  if (entry.name_len == 0 || entry.name_len > kFactoryfsMaxNameSize) {
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  uint32_t max_data_off = info.data_blocks + info.directory_ent_blocks + kFactoryfsSuperblockBlocks;
  if (entry.data_off >= max_data_off) {
    return ZX_ERR_IO_DATA_INTEGRITY;
  }
  return ZX_OK;
}

static void DumpDirectoryEntry(const DirectoryEntry* entry) {
  if (fs::trace_debug_enabled()) {
    std::string_view entry_name(entry->name, entry->name_len);
    std::cerr << "Directory entry: data_len=" << entry->data_len << ", data_off=" << entry->data_off
              << ", name=" << entry_name << std::endl;
  }
}

uint32_t FsToDeviceBlocks(uint32_t fs_block, uint32_t disk_block_size) {
  return fs_block * (kFactoryfsBlockSize / disk_block_size);
}

zx_status_t Factoryfs::OpenRootNode(fbl::RefPtr<fs::Vnode>* out) {
  auto root = fbl::MakeRefCounted<Directory>(*this, std::string_view());
  auto validated_options = root->ValidateOptions(fs::VnodeConnectionOptions());
  if (validated_options.is_error()) {
    return validated_options.status_value();
  }
  zx_status_t status = root->Open(validated_options.value(), nullptr);
  if (status != ZX_OK) {
    return status;
  }

  *out = std::move(root);
  return ZX_OK;
}

zx::result<fs::FilesystemInfo> Factoryfs::GetFilesystemInfo() {
  fs::FilesystemInfo info;

  info.block_size = kFactoryfsBlockSize;
  info.max_filename_size = kFactoryfsMaxNameSize;
  info.fs_type = fuchsia_fs::VfsType::kFactoryfs;
  info.total_bytes = static_cast<uint64_t>(superblock_.data_blocks) * kFactoryfsBlockSize;
  info.used_bytes = static_cast<uint64_t>(superblock_.data_blocks) * kFactoryfsBlockSize;
  info.total_nodes = superblock_.directory_entries;
  info.used_nodes = superblock_.directory_entries;
  info.SetFsId(fs_id_);
  info.name = "factoryfs";

  return zx::ok(info);
}

Factoryfs::Factoryfs(std::unique_ptr<BlockDevice> device, const Superblock* superblock,
                     fs::FuchsiaVfs* vfs)
    : block_device_(std::move(device)), superblock_(*superblock), vfs_(vfs) {
  zx::event::create(0, &fs_id_);
}

std::unique_ptr<BlockDevice> Factoryfs::Reset() {
  if (!block_device_) {
    return nullptr;
  }
  // TODO(fxbug.dev/105904) Shutdown all internal connections to factoryfs,
  // by iterating over open_vnodes
  return std::move(block_device_);
}

zx::result<std::unique_ptr<Factoryfs>> Factoryfs::Create(async_dispatcher_t* dispatcher,
                                                         std::unique_ptr<BlockDevice> device,
                                                         MountOptions* options,
                                                         fs::FuchsiaVfs* vfs) {
  TRACE_DURATION("factoryfs", "Factoryfs::Create");
  Superblock superblock;
  if (zx_status_t status = block_client::Reader(*device).Read(0, kFactoryfsBlockSize, &superblock);
      status != ZX_OK) {
    FX_LOGS(ERROR) << "could not read info block: " << zx_status_get_string(status);
    return zx::error(status);
  }

  fuchsia_hardware_block::wire::BlockInfo block_info;
  if (zx_status_t status = device->BlockGetInfo(&block_info); status != ZX_OK) {
    FX_LOGS(ERROR) << "cannot acquire block info: " << zx_status_get_string(status);
    return zx::error(status);
  }
  // Factoryfs only supports read, not write. However, both generic fsck as well as generic mount
  // open the device in read-write mode. Hence we cannot return an error here. Simply log this
  // inconsistency for now.
  if (block_info.flags & fuchsia_hardware_block::wire::Flag::kReadonly) {
    FX_LOGS(ERROR) << "Factory partition should only be mounting as read-only.";
    // return ZX_ERR_IO;
  }
  if (kFactoryfsBlockSize % block_info.block_size != 0) {
    FX_LOGS(ERROR) << "Factoryfs block size (" << kFactoryfsBlockSize
                   << ") not divisible by device block size (" << block_info.block_size << ")";
    return zx::error(ZX_ERR_IO);
  }

  // Perform superblock validations.
  if (zx_status_t status = CheckSuperblock(&superblock); status != ZX_OK) {
    FX_LOGS(ERROR) << "Check Superblock failure";
    return zx::error(status);
  }

  auto fs = std::unique_ptr<Factoryfs>(new Factoryfs(std::move(device), &superblock, vfs));
  fs->block_info_ = block_info;

  return zx::ok(std::move(fs));
}

Factoryfs::~Factoryfs() { Reset(); }

zx_status_t Factoryfs::InitDirectoryVmo() {
  if (directory_vmo_.is_valid()) {
    return ZX_OK;
  }

  const size_t vmo_size = fbl::round_up(GetDirectorySize(), kFactoryfsBlockSize);
  if (zx_status_t status = zx::vmo::create(vmo_size, 0, &directory_vmo_); status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to initialize directory vmo; error: " << zx_status_get_string(status);
    return status;
  }

  zx_object_set_property(directory_vmo_.get(), ZX_PROP_NAME, "factoryfs-directory",
                         strlen("factoryfs-directory"));
  storage::OwnedVmoid vmoid;
  if (zx_status_t status =
          Device().BlockAttachVmo(directory_vmo_, &vmoid.GetReference(block_device_.get()));
      status != ZX_OK) {
    directory_vmo_.reset();
    return status;
  }

  Superblock info = Info();
  uint32_t dev_block_size = GetDeviceBlockInfo().block_size;
  uint32_t dev_blocks = info.directory_ent_blocks * (kFactoryfsBlockSize / dev_block_size);

  block_fifo_request_t request = {
      .opcode = BLOCK_OP_READ,
      .vmoid = vmoid.get(),
      .length = dev_blocks,
      .vmo_offset = 0,
      .dev_offset = info.directory_ent_start_block *
                    static_cast<uint64_t>(kFactoryfsBlockSize / dev_block_size),
  };

  return Device().FifoTransaction(&request, 1);
}

// Parses all entries in the container directory from offset 0.
// |parse_data| is guarenteed to be 4 byte aligned.
zx_status_t Factoryfs::ParseEntries(Callback callback, void* parse_data) {
  size_t avail = GetDirectorySize();

  // To enforce 4 byte alignment for all the directory entries,
  // we need to enforce the starting address of parse_data is
  // also 4 byte aligned.
  uintptr_t buffer = reinterpret_cast<uintptr_t>(parse_data);
  if (buffer & 3) {
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  // Note about alignment: ptr is enforced to be always 4-byte aligned,
  // and DirectoryEntry itself is always 4 byte aligned i.e DirentSize will be a multiple of 4.
  // Hence it is safe to use reinterpret cast.
  while (avail > sizeof(DirectoryEntry)) {
    DirectoryEntry* entry = reinterpret_cast<DirectoryEntry*>(buffer);
    if (entry->name_len == 0) {
      break;
    }
    size_t size = DirentSize(entry->name_len);
    if (size > avail) {
      FX_LOGS(ERROR) << "invalid directory entry: size > avail!";
      DumpDirectoryEntry(entry);
      return ZX_ERR_IO;
    }
    if (zx_status_t status = IsValidDirectoryEntry(*entry, Info()); status != ZX_OK) {
      FX_LOGS(ERROR) << "invalid directory entry!";
      DumpDirectoryEntry(entry);
      return status;
    }
    if (callback(entry) == ZX_OK) {
      return ZX_OK;
    }
    buffer += size;
    avail -= size;
  }
  return ZX_ERR_NOT_FOUND;
}

zx::result<std::unique_ptr<DirectoryEntryManager>> Factoryfs::LookupInternal(
    const std::string_view path) {
  if (path.empty()) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  uint32_t dirent_blocks = Info().directory_ent_blocks;

  // We need to make sure "block" is 4 byte aligned to access directory entries.
  // and initialized to zero.
  std::vector<uint32_t> block(dirent_blocks * kFactoryfsBlockSize / 4, 0);

  uint32_t len = dirent_blocks * kFactoryfsBlockSize;

  zx_status_t status = ZX_OK;
  if (zx_status_t status = InitDirectoryVmo(); status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to initialize VMO error: " << zx_status_get_string(status);
    return zx::error(status);
  }

  if (zx_status_t status = directory_vmo_.read(block.data(), 0, len); status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to read VMO error: " << zx_status_get_string(status);
    return zx::error(status);
  }

  std::unique_ptr<DirectoryEntryManager> out_entry;
  status = ParseEntries(
      [&](const DirectoryEntry* entry) {
        std::string_view entry_name(entry->name, entry->name_len);
        // Perform a partial match.
        if (entry_name.compare(0, path.size(), path) == 0 &&
            (entry_name.size() == path.size() || entry_name[path.size()] == '/')) {
          return DirectoryEntryManager::Create(entry, &out_entry);
        }
        return ZX_ERR_NOT_FOUND;
      },
      block.data());

  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Directory::LookupInternal failed with error: "
                   << zx_status_get_string(status);
    return zx::error(status);
  }

  ZX_ASSERT(out_entry);
  return zx::ok(std::move(out_entry));
}

zx::result<fbl::RefPtr<fs::Vnode>> Factoryfs::Lookup(const std::string_view path) {
  auto iter = open_vnodes_cache_.find(path);
  if (iter != open_vnodes_cache_.end()) {
    return zx::ok(fbl::RefPtr(iter->second));
  }

  std::unique_ptr<DirectoryEntryManager> dir_entry;
  if (auto dir_entry_or = LookupInternal(path); dir_entry_or.is_error()) {
    return dir_entry_or.take_error();
  } else {
    dir_entry = std::move(dir_entry_or).value();
  }

  // If we got a partial match, then we need to create a directory node rather than
  // a file node.
  if (path.size() < dir_entry->GetName().size()) {
    return zx::ok(fbl::MakeRefCounted<Directory>(*this, path));
  }
  return zx::ok(fbl::MakeRefCounted<File>(*this, std::move(dir_entry)));
}

void Factoryfs::DidOpen(std::string_view path, fs::Vnode& vnode) {
  ZX_ASSERT(open_vnodes_cache_.emplace(path, &vnode).second);
}

void Factoryfs::DidClose(const std::string_view path) {
  auto iter = open_vnodes_cache_.find(path);
  ZX_ASSERT(iter != open_vnodes_cache_.end());
  open_vnodes_cache_.erase(iter);
}

}  // namespace factoryfs
