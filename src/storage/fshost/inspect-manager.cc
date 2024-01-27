// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "inspect-manager.h"

#include <lib/inspect/service/cpp/service.h>
#include <lib/syslog/cpp/macros.h>
#include <sys/stat.h>

#include "src/lib/storage/vfs/cpp/service.h"

namespace fio = fuchsia_io;

namespace fshost {

zx_status_t OpenNode(fidl::UnownedClientEnd<fio::Directory> root, const std::string& path,
                     fuchsia_io::wire::OpenFlags flags, fidl::ClientEnd<fio::Node>* result) {
  auto dir = fidl::CreateEndpoints<fio::Node>();
  if (!dir.is_ok()) {
    return dir.status_value();
  }

  fidl::StringView path_view(fidl::StringView::FromExternal(path));
  zx_status_t status = fidl::WireCall(root)
                           ->Open(fs::VnodeConnectionOptions::ReadOnly().ToIoV1Flags() | flags, {},
                                  std::move(path_view), std::move(dir->server))
                           .status();
  if (status != ZX_OK) {
    return status;
  }
  *result = std::move(dir->client);
  return ZX_OK;
}

fbl::RefPtr<fs::PseudoDir> FshostInspectManager::Initialize(async_dispatcher* dispatcher) {
  auto diagnostics_dir = fbl::MakeRefCounted<fs::PseudoDir>();
  diagnostics_dir->AddEntry(
      fuchsia::inspect::Tree::Name_,
      fbl::MakeRefCounted<fs::Service>([connector = inspect::MakeTreeHandler(
                                            &inspector_, dispatcher)](zx::channel chan) mutable {
        connector(fidl::InterfaceRequest<fuchsia::inspect::Tree>(std::move(chan)));
        return ZX_OK;
      }));
  return diagnostics_dir;
}

void FshostInspectManager::ServeStats(std::string name,
                                      fidl::ClientEnd<fuchsia_io::Directory> root) {
  inspector_.GetRoot().CreateLazyNode(
      name + "_stats",
      [this, name = std::move(name), root = std::move(root)] {
        if (!root.is_valid()) {
          FX_LOGS_FIRST_N(ERROR, 10)
              << "Cannot serve stats for " << name << ": invalid root channel!";
          return fpromise::make_result_promise<inspect::Inspector>(fpromise::error());
        }
        fidl::ClientEnd<fio::Node> root_chan;
        zx_status_t status =
            OpenNode(root, ".", fuchsia_io::wire::OpenFlags::kDirectory, &root_chan);
        if (status != ZX_OK) {
          FX_LOGS_FIRST_N(ERROR, 10) << "Cannot serve stats for " << name
                                     << ": failed to open node: " << zx_status_get_string(status);
          return fpromise::make_result_promise<inspect::Inspector>(fpromise::error());
        }
        // Note: we are unsafely assuming that |root_chan| is a directory
        // i.e. speaks |fuchsia.io/Directory|.
        fidl::ClientEnd<fio::Directory> root_dir(root_chan.TakeChannel());
        inspect::Inspector insp;
        FillFileTreeSizes(std::move(root_dir), insp.GetRoot().CreateChild(name), &insp);
        FillStats(root, &insp);
        return fpromise::make_ok_promise(std::move(insp));
      },
      &inspector_);
}

void FshostInspectManager::FillStats(fidl::UnownedClientEnd<fio::Directory> dir_chan,
                                     inspect::Inspector* inspector) {
  fidl::UnownedClientEnd<fuchsia_io::Directory> dir(dir_chan.channel());
  auto result = fidl::WireCall(dir)->QueryFilesystem();
  inspect::Node stats = inspector->GetRoot().CreateChild("stats");
  if (result.status() == ZX_OK) {
    fidl::WireResponse<fuchsia_io::Directory::QueryFilesystem>* response = result.Unwrap();
    fuchsia_io::wire::FilesystemInfo* info = response->info.get();
    if (info != nullptr) {
      stats.CreateUint("fvm_free_bytes", info->free_shared_pool_bytes, inspector);
      stats.CreateUint("allocated_inodes", info->total_nodes, inspector);
      stats.CreateUint("used_inodes", info->used_nodes, inspector);
      // Total bytes is the size of the partition plus the size it could conceivably grow into.
      // TODO(fxbug.dev/84626): Remove this misleading metric.
      stats.CreateUint("total_bytes", info->total_bytes + info->free_shared_pool_bytes, inspector);
      stats.CreateUint("allocated_bytes", info->total_bytes, inspector);
      stats.CreateUint("used_bytes", info->used_bytes, inspector);
    } else {
      stats.CreateString("error", "Query failed", inspector);
    }
  } else {
    stats.CreateString("error", "Query failed", inspector);
  }
  inspector->emplace(std::move(stats));
}

void FshostInspectManager::FillFileTreeSizes(fidl::ClientEnd<fio::Directory> current_dir,
                                             inspect::Node node, inspect::Inspector* inspector) {
  struct PendingDirectory {
    std::unique_ptr<DirectoryEntriesIterator> entries_iterator;
    inspect::Node node;
    size_t total_size;
  };

  // Keeps track of entries in the stack, the entry at N+1 will always be a child of the entry at N
  // to be able to update the parent `total_size` and propagate the sizes up. We use the lazy
  // iterator to have a single child connection at a time per node.
  std::vector<PendingDirectory> work_stack;
  auto current = PendingDirectory{
      .entries_iterator = std::make_unique<DirectoryEntriesIterator>(std::move(current_dir)),
      .node = std::move(node),
      .total_size = 0,
  };
  work_stack.push_back(std::move(current));

  while (!work_stack.empty()) {
    auto& current = work_stack.back();

    // If we have finished with this node then pop it from the stack, save it in inspect and
    // continue.
    if (current.entries_iterator->finished()) {
      // Maintain this node alive in inspect by adding it to the inspector value list and delete the
      // stack item.
      current.node.CreateUint("size", current.total_size, inspector);
      inspector->emplace(std::move(current.node));
      size_t size = current.total_size;

      work_stack.pop_back();

      // The next node in the stack is the parent of this node. Increment its size by the total size
      // of this node. If the work stack is emtpy, then `current` is the root.
      if (!work_stack.empty()) {
        work_stack.back().total_size += size;
      }

      continue;
    }

    // Get the next entry.
    while (auto entry = current.entries_iterator->GetNext()) {
      // If the entry is a directory, push it to the stack and continue the stack loop.
      if (entry->is_dir) {
        work_stack.push_back(PendingDirectory{
            .entries_iterator = std::make_unique<DirectoryEntriesIterator>(
                fidl::ClientEnd<fuchsia_io::Directory>(entry->node.TakeChannel())),
            .node = current.node.CreateChild(entry->name),
            .total_size = 0,
        });
        break;
      } else {
        // If the entry is a file, record its size.
        inspect::Node child_node = current.node.CreateChild(entry->name);
        child_node.CreateUint("size", entry->size, inspector);
        inspector->emplace(std::move(child_node));
        current.total_size += entry->size;
      }
    }
  }
}

void FshostInspectManager::LogMigrationStatus(zx_status_t status) {
  if (!migration_status_node_) {
    migration_status_node_ = inspector_.GetRoot().CreateChild("migration_status");
  }
  switch (status) {
    case ZX_OK: {
      migration_status_ = migration_status_node_->CreateInt("success", 1);
      break;
    }
    case ZX_ERR_NO_SPACE: {
      migration_status_ = migration_status_node_->CreateInt("out_of_space", 1);
      break;
    }
    default: {
      FX_LOGS(ERROR) << "Migration failed with status: " << status;
      migration_status_ = migration_status_node_->CreateInt("other_error", 1);
      break;
    }
  }
}

void FshostInspectManager::LogCorruption(fs_management::DiskFormat format) {
  if (!corruption_node_.has_value()) {
    corruption_node_ = inspector_.GetRoot().CreateChild("corruption_events");
  }
  auto found_it = corruption_events_.find(format);
  if (found_it == corruption_events_.cend()) {
    found_it = corruption_events_.emplace_hint(
        found_it, format, corruption_node_->CreateUint(DiskFormatString(format), 0u));
  }
  found_it->second.Add(1u);
}

// Create a new lazy iterator.
DirectoryEntriesIterator::DirectoryEntriesIterator(fidl::ClientEnd<fio::Directory> directory)
    : directory_(std::move(directory)) {}

// Get the next entry. If there's no more entries left, this method will return std::nullopt
// forever.
std::optional<DirectoryEntry> DirectoryEntriesIterator::GetNext() {
  // Loop until we can return an entry or there are none left.
  while (true) {
    // If we have pending entries to return, take one and return it. If for some reason, we fail
    // to make a result out of the pending entry (it may not exist anymore) then keep trying until
    // we can return one.
    while (!pending_entries_.empty()) {
      auto entry_name = pending_entries_.front();
      pending_entries_.pop();
      if (auto result = MaybeMakeEntry(entry_name)) {
        return result;
      }
    }

    // When there are no pending entries and we have already finished, return.
    if (finished_) {
      return std::nullopt;
    }

    // Load the next set of dirents.
    RefreshPendingEntries();

    // If we didn't find any pending entries in this batch of dirents, then we have finished.
    if (pending_entries_.empty()) {
      finished_ = true;
      return std::nullopt;
    }
  }
}

std::optional<DirectoryEntry> DirectoryEntriesIterator::MaybeMakeEntry(
    const std::string& entry_name) {
  // Open child of the current node with the given entry name.
  fidl::ClientEnd<fio::Node> child_chan;
  zx_status_t status = OpenNode(directory_, entry_name, {}, &child_chan);
  if (status != ZX_OK) {
    return std::nullopt;
  }

  // Get child attributes to know whether the child is a directory or not.
  auto result = fidl::WireCall(child_chan)->GetAttr();
  if (result.status() != ZX_OK) {
    return std::nullopt;
  }
  fidl::WireResponse<fio::Node::GetAttr>* response = result.Unwrap();

  bool is_dir = response->attributes.mode & fio::wire::kModeTypeDirectory;
  return std::optional<DirectoryEntry>{{
      .name = entry_name,
      .node = std::move(child_chan),
      .size = (is_dir) ? 0 : response->attributes.content_size,
      .is_dir = is_dir,
  }};
}

// Reads the next set of dirents and loads them into `pending_entries_`.
void DirectoryEntriesIterator::RefreshPendingEntries() {
  auto result = fidl::WireCall(directory_)->ReadDirents(fio::wire::kMaxBuf);
  if (result.status() != ZX_OK) {
    return;
  }
  fidl::WireResponse<fio::Directory::ReadDirents>* response = result.Unwrap();
  if (response->dirents.count() == 0) {
    return;
  }

  size_t offset = 0;
  auto data_ptr = response->dirents.data();

  while (sizeof(vdirent_t) < response->dirents.count() - offset) {
    const vdirent_t* entry = reinterpret_cast<const vdirent_t*>(data_ptr + offset);
    std::string entry_name(entry->name, entry->size);
    offset += sizeof(vdirent_t) + entry->size;
    if (entry_name == "." || entry_name == "..") {
      continue;
    }
    pending_entries_.push(std::move(entry_name));
  }
}

}  // namespace fshost
