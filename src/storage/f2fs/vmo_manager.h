// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_F2FS_VMO_MANAGER_H_
#define SRC_STORAGE_F2FS_VMO_MANAGER_H_

#include <fbl/intrusive_wavl_tree.h>

#include "src/storage/f2fs/f2fs_layout.h"
#include "src/storage/f2fs/f2fs_types.h"

namespace f2fs {

enum class VmoMode {
  kDiscardable = 0,
  kPaged,
};

class VmoMapping : public fbl::WAVLTreeContainable<std::unique_ptr<VmoMapping>> {
 public:
  VmoMapping() = delete;
  explicit VmoMapping(zx::vmo &vmo, pgoff_t index, size_t size, bool zero);
  virtual ~VmoMapping();

  virtual zx::result<bool> Lock(pgoff_t offset) = 0;
  virtual zx_status_t Unlock(pgoff_t offset) = 0;
  virtual zx_status_t Zero(pgoff_t start, pgoff_t len) = 0;

  zx::result<zx_vaddr_t> GetAddress(pgoff_t offset) const;
  pgoff_t GetKey() const { return index(); }
  uint64_t GetActivePages() const { return active_pages(); }

 protected:
  zx::vmo &vmo() {
    if (owned_vmo_.is_valid()) {
      return owned_vmo_;
    }
    return vmo_;
  }
  zx_vaddr_t address() const { return address_; }
  size_t get_size() const { return size_in_blocks_; }
  zx_vaddr_t page_to_address(pgoff_t page_index) const;
  pgoff_t address_to_page(zx_vaddr_t address) const;
  pgoff_t index() const { return index_; }
  uint64_t increase_active_pages() { return ++active_pages_; }
  uint64_t decrease_active_pages() { return --active_pages_; }
  uint64_t active_pages() const { return active_pages_.load(std::memory_order_acquire); }

 private:
  // A va mapping to |this|.
  zx_vaddr_t address_ = 0;
  // The number of active pages to |vmo_|.
  std::atomic<uint64_t> active_pages_ = 0;
  const size_t size_in_blocks_ = 0;
  const pgoff_t index_;
  zx::vmo owned_vmo_;
  zx::vmo &vmo_;
};

class VmoPaged : public VmoMapping {
 public:
  VmoPaged() = delete;
  explicit VmoPaged(zx::vmo &vmo, pgoff_t index, size_t size, bool zero);
  ~VmoPaged();

  zx::result<bool> Lock(pgoff_t offset) final;
  zx_status_t Unlock(pgoff_t offset) final;
  zx_status_t Zero(pgoff_t start, pgoff_t len) final;
};

// It manages the lifecycle of a discardable Vmo that Pages use in each vnode.
class VmoDiscardable : public VmoMapping {
 public:
  VmoDiscardable() = delete;
  VmoDiscardable(const VmoDiscardable &) = delete;
  VmoDiscardable &operator=(const VmoDiscardable &) = delete;
  VmoDiscardable(const VmoDiscardable &&) = delete;
  VmoDiscardable &operator=(const VmoDiscardable &&) = delete;
  explicit VmoDiscardable(zx::vmo &vmo, pgoff_t index, size_t size, bool zero);

  ~VmoDiscardable();

  // It ensures that |vmo_| keeps VMO_OP_LOCK as long as any Pages refer to it
  // by calling Page::GetPage(). When a valid page gets a new access to a |this|,
  // it increases VmoMapping::active_pages_, and then decreases VmoMapping::active_pages_
  // in Unlock() as a active page is truncated. When VmoMapping::active_pages_ reaches
  // zero, Unlock() does VMO_OP_UNLOCK for page reclaim.
  // When VmoMapping::active_pages is zero with new page access, it tries VMO_OP_TRY_LOCK
  // to check which pages kernel has reclaimed.
  zx::result<bool> Lock(pgoff_t offset) final;
  // It unlocks |vmo_| when there is no Page using it.
  zx_status_t Unlock(pgoff_t offset) final;
  zx_status_t Zero(pgoff_t start, pgoff_t len) final;

 private:
  // It tracks which Page has been decommitted by kernel during |vmo_| unlocked.
  // When a bit is 0, a caller (i.e., Page::GetPage()) clears the kUptodate flag of the
  // corresponding Page and fill the Page with data read from disk.
  std::vector<bool> page_bitmap_;
};

// It provides vmo service to Filecache of a vnode. To cover the full range of a vnode and save va
// mapping resource, it divides the range of a vnode into a fixed size of vmo nodes and keeps them
// in |vmo_tree_|. A vmo node represents a range between VmoMapping::index_ and VmoMapping::index_ +
// VmoMapping::size_in_blocks within the range of a vnode. A vmo node can be configured as a
// discardable vmo or a part of a paged vmo. The size of a vmo node is set to
// VmoManager::node_size_in_blocks_, and the va mapping of a vmo node keeps as long as the vmo node
// is kept in |vmo_tree_|.
class VmoManager {
 public:
  VmoManager() = delete;
  VmoManager(VmoMode mode, size_t size, zx::vmo vmo = {})
      : mode_(mode), node_size_in_blocks_(size), vmo_(std::move(vmo)) {
    if (vmo_.is_valid()) {
      size_t size_in_bytes;
      ZX_ASSERT(vmo_.get_size(&size_in_bytes) == ZX_OK);
      size_t new_size_in_bytes =
          fbl::round_up(size_in_bytes, kBlockSize * kDefaultBlocksPerSegment);
      size_in_blocks_ = new_size_in_bytes / kBlockSize;
      if (new_size_in_bytes != size_in_bytes) {
        ZX_ASSERT(vmo_.set_size(new_size_in_bytes) == ZX_OK);
      }
    }
  }
  VmoManager(const VmoManager &) = delete;
  VmoManager &operator=(const VmoManager &) = delete;
  VmoManager(const VmoManager &&) = delete;
  VmoManager &operator=(const VmoManager &&) = delete;
  ~VmoManager() { Reset(true); }

  zx::result<bool> CreateAndLockVmo(pgoff_t index) __TA_EXCLUDES(mutex_);
  zx_status_t UnlockVmo(pgoff_t index, bool evict) __TA_EXCLUDES(mutex_);
  zx::result<zx_vaddr_t> GetAddress(pgoff_t index) __TA_EXCLUDES(mutex_);
  zx_status_t ZeroRange(pgoff_t start, pgoff_t end) __TA_EXCLUDES(mutex_);
  void Reset(bool shutdown = false) __TA_EXCLUDES(mutex_);

 private:
  pgoff_t GetOffsetInVmoNode(pgoff_t page_index) const;
  pgoff_t GetVmoNodeKey(pgoff_t page_index) const;

  using VmoTreeTraits = fbl::DefaultKeyedObjectTraits<pgoff_t, VmoMapping>;
  using VmoTree = fbl::WAVLTree<pgoff_t, std::unique_ptr<VmoMapping>, VmoTreeTraits>;
  zx::result<VmoMapping *> FindVmoNodeUnsafe(pgoff_t index) __TA_REQUIRES_SHARED(mutex_);
  zx::result<VmoMapping *> GetVmoNodeUnsafe(pgoff_t index, bool zero) __TA_REQUIRES(mutex_);

  fs::SharedMutex mutex_;
  VmoTree vmo_tree_ __TA_GUARDED(mutex_);
  const VmoMode mode_;

  // the maximum file size (4TiB) in blocks
  size_t size_in_blocks_ = 0;
  const size_t node_size_in_blocks_ = 0;
  // a copy of paged vmo
  zx::vmo vmo_ __TA_GUARDED(mutex_);
};

}  // namespace f2fs

#endif  // SRC_STORAGE_F2FS_VMO_MANAGER_H_
