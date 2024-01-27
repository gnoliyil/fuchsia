// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

Page::Page(FileCache *file_cache, pgoff_t index) : file_cache_(file_cache), index_(index) {}

VnodeF2fs &Page::GetVnode() const { return file_cache_->GetVnode(); }

VmoManager &Page::GetVmoManager() const { return file_cache_->GetVmoManager(); }

FileCache &Page::GetFileCache() const { return *file_cache_; }

Page::~Page() {
  ZX_DEBUG_ASSERT(IsWriteback() == false);
  ZX_DEBUG_ASSERT(InTreeContainer() == false);
  ZX_DEBUG_ASSERT(InListContainer() == false);
  ZX_DEBUG_ASSERT(IsDirty() == false);
  ZX_DEBUG_ASSERT(IsLocked() == false);
}

void Page::RecyclePage() {
  // Since active Pages are evicted only when having strong references,
  // it is safe to call InContainer().
  if (InTreeContainer()) {
    ZX_ASSERT(VmoOpUnlock() == ZX_OK);
    file_cache_->Downgrade(this);
  } else {
    delete this;
  }
}

F2fs *Page::fs() const { return file_cache_->fs(); }

bool Page::SetDirty() {
  SetUptodate();
  // No need to make dirty Pages for orphan files.
  if (!file_cache_->IsOrphan() &&
      !flags_[static_cast<uint8_t>(PageFlag::kPageDirty)].test_and_set(std::memory_order_acquire)) {
    VnodeF2fs &vnode = GetVnode();
    SuperblockInfo &superblock_info = fs()->GetSuperblockInfo();
    vnode.MarkInodeDirty(true);
    vnode.IncreaseDirtyPageCount();
    if (vnode.IsNode()) {
      superblock_info.IncreasePageCount(CountType::kDirtyNodes);
    } else if (vnode.IsDir()) {
      superblock_info.IncreasePageCount(CountType::kDirtyDents);
      superblock_info.IncreaseDirtyDir();
    } else if (vnode.IsMeta()) {
      superblock_info.IncreasePageCount(CountType::kDirtyMeta);
      superblock_info.SetDirty();
    } else {
      superblock_info.IncreasePageCount(CountType::kDirtyData);
    }
    return false;
  }
  return true;
}

bool Page::ClearDirtyForIo() {
  ZX_DEBUG_ASSERT(IsLocked());
  VnodeF2fs &vnode = GetVnode();
  SuperblockInfo &superblock_info = fs()->GetSuperblockInfo();
  if (IsDirty()) {
    ClearFlag(PageFlag::kPageDirty);
    vnode.DecreaseDirtyPageCount();
    if (vnode.IsNode()) {
      superblock_info.DecreasePageCount(CountType::kDirtyNodes);
    } else if (vnode.IsDir()) {
      superblock_info.DecreasePageCount(CountType::kDirtyDents);
      superblock_info.DecreaseDirtyDir();
    } else if (vnode.IsMeta()) {
      superblock_info.DecreasePageCount(CountType::kDirtyMeta);
    } else {
      superblock_info.DecreasePageCount(CountType::kDirtyData);
    }
    return true;
  }
  return false;
}

zx_status_t Page::GetPage() {
  ZX_DEBUG_ASSERT(IsLocked());
  auto committed_or = VmoOpLock();
  if (committed_or.is_ok()) {
    if (!committed_or.value()) {
      ZX_DEBUG_ASSERT(!IsDirty());
      ZX_DEBUG_ASSERT(!IsWriteback());
      ClearUptodate();
    }
  }
  ZX_ASSERT(committed_or.is_ok());
  return committed_or.status_value();
}

void Page::Invalidate() {
  ZX_DEBUG_ASSERT(IsLocked());
  ClearDirtyForIo();
  ClearColdData();
  ClearUptodate();
}

bool Page::SetUptodate() {
  ZX_DEBUG_ASSERT(IsLocked());
  return SetFlag(PageFlag::kPageUptodate);
}

void Page::ClearUptodate() {
  // block_addr_ is valid only when the uptodate flag is set.
  block_addr_ = kNullAddr;
  ClearFlag(PageFlag::kPageUptodate);
}

void Page::WaitOnWriteback() {
  if (IsWriteback()) {
    fs()->ScheduleWriter();
  }
  WaitOnFlag(PageFlag::kPageWriteback);
}

bool Page::SetWriteback() {
  bool ret = SetFlag(PageFlag::kPageWriteback);
  if (!ret) {
    fs()->GetSuperblockInfo().IncreasePageCount(CountType::kWriteback);
  }
  return ret;
}

void Page::ClearWriteback() {
  if (IsWriteback()) {
    fs()->GetSuperblockInfo().DecreasePageCount(CountType::kWriteback);
    ClearFlag(PageFlag::kPageWriteback);
    WakeupFlag(PageFlag::kPageWriteback);
  }
}

void Page::SetColdData() {
  ZX_DEBUG_ASSERT(IsLocked());
  ZX_DEBUG_ASSERT(!IsWriteback());
  SetFlag(PageFlag::kPageColdData);
}

zx::result<> Page::SetBlockAddr(block_t addr) {
  if (IsLocked() && IsUptodate()) {
    block_addr_ = addr;
    return zx::ok();
  }
  return zx::error(ZX_ERR_UNAVAILABLE);
}

bool Page::ClearColdData() {
  if (IsColdData()) {
    ClearFlag(PageFlag::kPageColdData);
    return true;
  }
  return false;
}

bool LockedPage::SetDirty(bool add_to_list) {
  bool ret = page_->SetDirty();
  if (!ret && add_to_list) {
    ZX_ASSERT(page_->fs()->GetDirtyDataPageList().AddDirty(*this).is_ok());
  }
  return ret;
}

zx_status_t Page::VmoOpUnlock(bool evict) {
  ZX_DEBUG_ASSERT(InTreeContainer());
  // |evict| can be true only when the Page is clean or subject to invalidation.
  if (((!IsDirty() && !file_cache_->IsOrphan()) || evict) && IsVmoLocked()) {
    WaitOnWriteback();
    ClearFlag(PageFlag::kPageVmoLocked);
    return GetVmoManager().UnlockVmo(index_, evict);
  }
  return ZX_OK;
}

zx::result<bool> Page::VmoOpLock() {
  ZX_DEBUG_ASSERT(InTreeContainer());
  ZX_DEBUG_ASSERT(IsLocked());
  if (!SetFlag(PageFlag::kPageVmoLocked)) {
    return GetVmoManager().CreateAndLockVmo(index_);
  }
  return zx::ok(true);
}

FileCache::FileCache(VnodeF2fs *vnode, VmoManager *vmo_manager)
    : vnode_(vnode), vmo_manager_(vmo_manager) {}

FileCache::~FileCache() {
  Reset();
  {
    std::lock_guard tree_lock(tree_lock_);
    ZX_DEBUG_ASSERT(page_tree_.is_empty());
  }
}

void FileCache::Downgrade(Page *raw_page) {
  // We can downgrade multiple Pages simultaneously.
  fs::SharedLock tree_lock(tree_lock_);
  // Resurrect |this|.
  raw_page->ResurrectRef();
  fbl::RefPtr<Page> page = fbl::ImportFromRawPtr(raw_page);
  // Leak it to keep alive in FileCache.
  [[maybe_unused]] auto leak = fbl::ExportToRawPtr(&page);
  raw_page->ClearActive();
  recycle_cvar_.notify_all();
}

F2fs *FileCache::fs() const { return GetVnode().fs(); }

zx_status_t FileCache::AddPageUnsafe(const fbl::RefPtr<Page> &page) {
  if (page->InTreeContainer()) {
    return ZX_ERR_ALREADY_EXISTS;
  }
  page_tree_.insert(page.get());
  return ZX_OK;
}

zx::result<std::vector<LockedPage>> FileCache::GetPages(const pgoff_t start, const pgoff_t end) {
  std::lock_guard tree_lock(tree_lock_);
  std::vector<LockedPage> locked_pages(end - start);
  auto exist_pages = GetLockedPagesUnsafe(start, end);
  uint32_t exist_pages_index = 0, count = 0;
  for (pgoff_t index = start; index < end; ++index, ++count) {
    if (exist_pages_index < exist_pages.size() &&
        exist_pages[exist_pages_index]->GetKey() == index) {
      locked_pages[count] = std::move(exist_pages[exist_pages_index]);
      ++exist_pages_index;
    } else {
      locked_pages[count] = GetNewPage(index);
    }

    if (auto ret = locked_pages[count]->GetPage(); ret != ZX_OK) {
      return zx::error(ret);
    }
  }

  return zx::ok(std::move(locked_pages));
}

zx::result<std::vector<LockedPage>> FileCache::FindPages(const pgoff_t start, const pgoff_t end) {
  std::lock_guard tree_lock(tree_lock_);
  auto pages = GetLockedPagesUnsafe(start, end);
  for (auto &page : pages) {
    if (auto ret = page->GetPage(); ret != ZX_OK) {
      return zx::error(ret);
    }
  }

  return zx::ok(std::move(pages));
}

zx::result<std::vector<LockedPage>> FileCache::GetPages(const std::vector<pgoff_t> &page_offsets) {
  std::lock_guard tree_lock(tree_lock_);
  if (page_offsets.empty()) {
    return zx::ok(std::vector<LockedPage>(0));
  }

  auto locked_pages = GetLockedPagesUnsafe(page_offsets);
  uint32_t count = 0;
  for (pgoff_t index : page_offsets) {
    if (index != kInvalidPageOffset) {
      if (!locked_pages[count]) {
        locked_pages[count] = GetNewPage(index);
      }

      if (zx_status_t ret = locked_pages[count]->GetPage(); ret != ZX_OK) {
        return zx::error(ret);
      }
    }
    ++count;
  }

  return zx::ok(std::move(locked_pages));
}

LockedPage FileCache::GetNewPage(const pgoff_t index) {
  fbl::RefPtr<Page> page;
  if (GetVnode().IsNode()) {
    page = fbl::MakeRefCounted<NodePage>(this, index);
  } else {
    page = fbl::MakeRefCounted<Page>(this, index);
  }
  ZX_ASSERT(AddPageUnsafe(page) == ZX_OK);
  auto locked_page = LockedPage(std::move(page));
  locked_page->SetActive();
  return locked_page;
}

zx_status_t FileCache::GetPage(const pgoff_t index, LockedPage *out) {
  LockedPage locked_page;
  std::lock_guard tree_lock(tree_lock_);
  auto locked_page_or = GetPageUnsafe(index);
  if (locked_page_or.is_error()) {
    locked_page = GetNewPage(index);
  } else {
    locked_page = std::move(*locked_page_or);
  }
  if (auto ret = locked_page->GetPage(); ret != ZX_OK) {
    return ret;
  }
  *out = std::move(locked_page);
  return ZX_OK;
}

zx_status_t FileCache::FindPage(const pgoff_t index, fbl::RefPtr<Page> *out) {
  std::lock_guard tree_lock(tree_lock_);
  auto locked_page_or = GetPageUnsafe(index);
  if (locked_page_or.is_error()) {
    return locked_page_or.error_value();
  }
  if (auto ret = (*locked_page_or)->GetPage(); ret != ZX_OK) {
    return ret;
  }
  *out = (*locked_page_or).release();
  return ZX_OK;
}

zx::result<LockedPage> FileCache::GetLockedPageFromRawUnsafe(Page *raw_page) {
  auto page = fbl::MakeRefPtrUpgradeFromRaw(raw_page, tree_lock_);
  if (page == nullptr) {
    // Wait for it to be resurrected when it is being recycled.
    recycle_cvar_.wait(tree_lock_);
    return zx::error(ZX_ERR_SHOULD_WAIT);
  }
  // Try to make LockedPage from |page|.
  // If |page| has been already locked, it waits for it to be unlock and returns ZX_ERR_SHOULD_WAIT.
  auto locked_page_or = GetLockedPage(std::move(page));
  if (locked_page_or.is_error()) {
    return zx::error(ZX_ERR_SHOULD_WAIT);
  }
  // Here, Page::ref_count should not be less than one.
  return zx::ok(std::move(*locked_page_or));
}

zx::result<LockedPage> FileCache::GetPageUnsafe(const pgoff_t index) {
  while (true) {
    auto raw_ptr = page_tree_.find(index).CopyPointer();
    if (raw_ptr != nullptr) {
      if (raw_ptr->IsActive()) {
        auto locked_page_or = GetLockedPageFromRawUnsafe(raw_ptr);
        if (locked_page_or.is_error()) {
          continue;
        }
        return zx::ok(std::move(*locked_page_or));
      }
      auto page = fbl::ImportFromRawPtr(raw_ptr);
      LockedPage locked_page(std::move(page));
      locked_page->SetActive();
      ZX_DEBUG_ASSERT(locked_page->IsLastReference());
      return zx::ok(std::move(locked_page));
    }
    break;
  }
  return zx::error(ZX_ERR_NOT_FOUND);
}

zx::result<LockedPage> FileCache::GetLockedPage(fbl::RefPtr<Page> page) {
  if (page->TryLock()) {
    tree_lock_.unlock();
    {
      // If |page| is already locked, wait for it to be unlocked.
      // Ensure that the references to |page| drop before |tree_lock_|.
      // If |page| is the last reference, it enters Page::RecyclePage() and
      // possibly acquires |tree_lock_|.
      LockedPage locked_page(std::move(page));
    }
    // It is not allowed to acquire |tree_lock_| with locked Pages.
    tree_lock_.lock();
    return zx::error(ZX_ERR_SHOULD_WAIT);
  }
  LockedPage locked_page(std::move(page), false);
  return zx::ok(std::move(locked_page));
}

zx_status_t FileCache::EvictUnsafe(Page *page) {
  if (!page->InTreeContainer()) {
    return ZX_ERR_NOT_FOUND;
  }
  // Before eviction, check if it requires VMO_OP_UNLOCK
  // since Page::RecyclePage() tries VMO_OP_UNLOCK only when |page| keeps in FileCache.
  ZX_ASSERT(page->VmoOpUnlock(true) == ZX_OK);
  page_tree_.erase(*page);
  return ZX_OK;
}

std::vector<LockedPage> FileCache::GetLockedPagesUnsafe(pgoff_t start, pgoff_t end) {
  std::vector<LockedPage> pages;
  auto current = page_tree_.lower_bound(start);
  while (current != page_tree_.end() && current->GetKey() < end) {
    if (!current->IsActive()) {
      LockedPage locked_page(fbl::ImportFromRawPtr(current.CopyPointer()));
      locked_page->SetActive();
      pages.push_back(std::move(locked_page));
    } else {
      auto prev_key = current->GetKey();
      auto locked_page_or = GetLockedPageFromRawUnsafe(current.CopyPointer());
      if (locked_page_or.is_error()) {
        current = page_tree_.lower_bound(prev_key);
        continue;
      }
      pages.push_back(std::move(*locked_page_or));
    }
    ++current;
  }
  return pages;
}

std::vector<LockedPage> FileCache::GetLockedPagesUnsafe(const std::vector<pgoff_t> &page_offsets) {
  std::vector<LockedPage> pages(page_offsets.size());
  if (page_tree_.is_empty()) {
    return pages;
  }

  uint32_t index = 0;
  while (index < page_offsets.size()) {
    if (page_offsets[index] == kInvalidPageOffset) {
      ++index;
      continue;
    }
    auto current = page_tree_.find(page_offsets[index]);
    if (current == page_tree_.end()) {
      ++index;
      continue;
    }
    if (!current->IsActive()) {
      // No reference to |current|. It is safe to make a reference.
      LockedPage locked_page(fbl::ImportFromRawPtr(current.CopyPointer()));
      locked_page->SetActive();
      pages[index] = std::move(locked_page);
    } else {
      auto locked_page_or = GetLockedPageFromRawUnsafe(current.CopyPointer());
      if (locked_page_or.is_error()) {
        continue;
      }
      pages[index] = std::move(*locked_page_or);
    }
    ++index;
  }
  return pages;
}

std::vector<LockedPage> FileCache::CleanupPagesUnsafe(pgoff_t start, pgoff_t end) {
  std::vector<LockedPage> pages = GetLockedPagesUnsafe(start, end);
  for (auto &page : pages) {
    EvictUnsafe(page.get());
  }
  return pages;
}

std::vector<LockedPage> FileCache::InvalidatePages(pgoff_t start, pgoff_t end) {
  std::vector<LockedPage> pages;
  {
    std::lock_guard tree_lock(tree_lock_);
    pages = CleanupPagesUnsafe(start, end);
    // zero the range on the underlying vmo as |pages| have been evicted safely.
    vmo_manager_->ZeroRange(start, end);
  }
  for (auto &page : pages) {
    if (page->IsDirty()) {
      ZX_ASSERT(fs()->GetDirtyDataPageList().RemoveDirty(page).is_ok());
    }
    page->Invalidate();
  }
  return pages;
}

void FileCache::ClearDirtyPages(pgoff_t start, pgoff_t end) {
  std::vector<LockedPage> pages;
  {
    std::lock_guard tree_lock(tree_lock_);
    pages = GetLockedPagesUnsafe(start, end);
  }
  // Clear the dirty flag of all Pages.
  for (auto &page : pages) {
    page->ClearDirtyForIo();
  }
}

void FileCache::Reset() {
  std::vector<LockedPage> pages;
  {
    std::lock_guard tree_lock(tree_lock_);
    pages = CleanupPagesUnsafe();
  }
  for (auto &page : pages) {
    if (page->IsDirty()) {
      page->Invalidate();
    }
  }
  vmo_manager_->Reset();
}

std::vector<bool> FileCache::GetReadaheadPagesInfo(pgoff_t index, size_t max_scan) {
  std::lock_guard tree_lock(tree_lock_);
  std::vector<bool> read_blocks;
  size_t num_read_blocks = kDefaultReadaheadSize;
  read_blocks.reserve(num_read_blocks);

  if (index > 0) {
    auto current_key = index - 1;
    auto current = page_tree_.find(current_key);
    uint64_t count = 0;
    while (current_key >= 0 && current != page_tree_.end() && current_key == current->GetKey() &&
           current->IsUptodate() && count < max_scan) {
      --current;
      --current_key;
      ++count;
    }

    if (count != max_scan && count != index) {
      // It fails to detect seq. stream. Set the number of read blocks to 64KiB by default.
      num_read_blocks = kDefaultReadaheadSize / 2;
    }
  }

  auto current = page_tree_.find(index);
  for (size_t i = 0; i < num_read_blocks; ++i) {
    read_blocks.push_back(true);
    if (current != page_tree_.end() && current->GetKey() == index + i) {
      if (current->IsDirty() || current->IsWriteback()) {
        // no read IOs for dirty pages which are uptodate and pinned.
        read_blocks[i] = false;
        // the first page should be subject to read io as it triggers page fault.
        ZX_ASSERT(i);
      }
      ++current;
    }
  }
  return read_blocks;
}

std::vector<LockedPage> FileCache::GetLockedDirtyPagesUnsafe(const WritebackOperation &operation) {
  std::vector<LockedPage> pages;
  pgoff_t nwritten = 0;

  auto current = page_tree_.lower_bound(operation.start);
  // Get Pages from |operation.start| to |operation.end|.
  while (nwritten <= operation.to_write && current != page_tree_.end() &&
         current->GetKey() < operation.end) {
    auto raw_page = current.CopyPointer();
    if (raw_page->IsActive()) {
      // Do not touch any active Pages except for those in F2fs::dirty_data_page_list_.
      // When getting active Pages, any Page in FileCache must not be recycled here. If so,
      // deadlock can occurs since Page::RecycleNode tries to acquire tree_lock_.
      if (raw_page->IsDirty() && raw_page->InListContainer() &&
          vnode_->GetPageType() == PageType::kData && !vnode_->IsDir()) {
        auto prev_key = raw_page->GetKey();
        auto locked_page_or = GetLockedPageFromRawUnsafe(raw_page);
        if (locked_page_or.is_error()) {
          current = page_tree_.lower_bound(prev_key);
          continue;
        }
        if (!operation.if_page || operation.if_page((*locked_page_or).CopyRefPtr()) == ZX_OK) {
          ZX_ASSERT(vnode_->fs()->GetDirtyDataPageList().RemoveDirty(*locked_page_or).is_ok());
          pages.push_back(std::move(*locked_page_or));
          ++nwritten;
        } else {
          // F2fs::dirty_data_page_list_ must keep the ref of |*locked_page_or|.
          ZX_ASSERT(raw_page->InListContainer());
        }
      }
      ++current;
    } else {
      ++current;
      // For inactive Pages, try to evict clean Pages if operation.bReleasePages is set or if their
      // vnodes are inactive(closed).
      ZX_ASSERT(!raw_page->IsLocked());
      LockedPage page(fbl::ImportFromRawPtr(raw_page));

      if (page->IsDirty()) {
        ZX_DEBUG_ASSERT(page->IsLastReference());
        auto page_ref = page.CopyRefPtr();
        if (!operation.if_page || operation.if_page(page_ref) == ZX_OK) {
          page->SetActive();
          ZX_DEBUG_ASSERT(page->IsUptodate());
          ZX_DEBUG_ASSERT(page->IsVmoLocked());
          pages.push_back(std::move(page));
          page.reset();
          ++nwritten;
        }
      } else if (operation.bReleasePages || !vnode_->IsActive()) {
        // There is no other reference. It is safe to release it.
        EvictUnsafe(page.get());
        page.reset();
      }
      if (page) {
        auto page_ref = page.release();
        // It prevents |page| from entering RecyclePage() and
        // keeps |page| alive in FileCache.
        [[maybe_unused]] auto leak = fbl::ExportToRawPtr(&page_ref);
      }
    }
  }
  return pages;
}

// TODO: Consider using a global lock as below
// if (!IsDir())
//   mutex_lock(&superblock_info->writepages);
// Writeback()
// if (!IsDir())
//   mutex_unlock(&superblock_info->writepages);
// fs()->RemoveDirtyDirInode(this);
pgoff_t FileCache::Writeback(WritebackOperation &operation) {
  pgoff_t nwritten = 0;
  // FileCache::Writeback is not supposed to handle memory reclaim at this moment.
  if (operation.bReclaim) {
    return nwritten;
  }
  std::vector<LockedPage> pages;
  {
    std::lock_guard tree_lock(tree_lock_);
    pages = GetLockedDirtyPagesUnsafe(operation);
  }

  PageList pages_to_disk;
  for (auto &page : pages) {
    ZX_DEBUG_ASSERT(page->IsUptodate());
    zx::result<block_t> addr_or;
    if (vnode_->IsMeta()) {
      addr_or = fs()->GetSegmentManager().GetBlockAddrForDirtyMetaPage(page, operation.bReclaim);
    } else if (vnode_->IsNode()) {
      if (operation.node_page_cb) {
        // If it is last dnode page, set |is_last_dnode| flag to process additional operation.
        bool is_last_dnode = page.get() == pages.back().get();
        operation.node_page_cb(page.CopyRefPtr(), is_last_dnode);
      }
      addr_or = fs()->GetNodeManager().GetBlockAddrForDirtyNodePage(page, operation.bReclaim);
    } else {
      addr_or = vnode_->GetBlockAddrForDirtyDataPage(page, operation.bReclaim);
    }
    if (addr_or.is_error()) {
      if (page->IsUptodate() && addr_or.status_value() != ZX_ERR_NOT_FOUND) {
        // In case of failure, we just redirty it.
        page.SetDirty();
        FX_LOGS(WARNING) << "[f2fs] Allocating a block address failed." << addr_or.status_value();
      }
      page->ClearWriteback();
    } else {
      ZX_ASSERT(*addr_or != kNullAddr && *addr_or != kNewAddr);
      pages_to_disk.push_back(page.release());
      ++nwritten;
    }
  }

  sync_completion_t completion;
  fs()->ScheduleWriter(operation.bSync ? &completion : nullptr, std::move(pages_to_disk));
  if (operation.bSync) {
    // Release remaining Pages in |pages| for waiter.
    pages.clear();
    pages.shrink_to_fit();
    sync_completion_wait(&completion, ZX_TIME_INFINITE);
  }
  return nwritten;
}

}  // namespace f2fs
