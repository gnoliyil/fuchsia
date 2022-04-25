// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

NodeManager::NodeManager(F2fs *fs) : fs_(fs), superblock_info_(&fs_->GetSuperblockInfo()) {}

NodeManager::NodeManager(SuperblockInfo *sbi) : superblock_info_(sbi) {}

void NodeManager::SetNatCacheDirty(NatEntry &ne) {
  ZX_ASSERT(clean_nat_list_.erase(ne) != nullptr);
  dirty_nat_list_.push_back(&ne);
}

void NodeManager::ClearNatCacheDirty(NatEntry &ne) {
  ZX_ASSERT(dirty_nat_list_.erase(ne) != nullptr);
  clean_nat_list_.push_back(&ne);
}

void NodeManager::NodeInfoFromRawNat(NodeInfo &ni, RawNatEntry &raw_ne) {
  ni.ino = LeToCpu(raw_ne.ino);
  ni.blk_addr = LeToCpu(raw_ne.block_addr);
  ni.version = raw_ne.version;
}

bool NodeManager::IncValidNodeCount(VnodeF2fs *vnode, uint32_t count) {
  block_t valid_block_count;
  uint32_t ValidNodeCount;

  std::lock_guard stat_lock(GetSuperblockInfo().GetStatLock());

  valid_block_count = GetSuperblockInfo().GetTotalValidBlockCount() + static_cast<block_t>(count);
  GetSuperblockInfo().SetAllocValidBlockCount(GetSuperblockInfo().GetAllocValidBlockCount() +
                                              static_cast<block_t>(count));
  ValidNodeCount = GetSuperblockInfo().GetTotalValidNodeCount() + count;

  if (valid_block_count > GetSuperblockInfo().GetUserBlockCount()) {
    return false;
  }

  if (ValidNodeCount > GetSuperblockInfo().GetTotalNodeCount()) {
    return false;
  }

  if (vnode)
    vnode->IncBlocks(count);
  GetSuperblockInfo().SetTotalValidNodeCount(ValidNodeCount);
  GetSuperblockInfo().SetTotalValidBlockCount(valid_block_count);

  return true;
}

zx_status_t NodeManager::NextFreeNid(nid_t *nid) {
  if (free_nid_count_ <= 0)
    return ZX_ERR_OUT_OF_RANGE;

  std::lock_guard free_nid_lock(free_nid_list_lock_);
  FreeNid *fnid = containerof(free_nid_list_.next, FreeNid, list);
  *nid = fnid->nid;
  return ZX_OK;
}

void NodeManager::GetNatBitmap(void *out) {
  memcpy(out, nat_bitmap_.get(), nat_bitmap_size_);
  memcpy(nat_prev_bitmap_.get(), nat_bitmap_.get(), nat_bitmap_size_);
}

pgoff_t NodeManager::CurrentNatAddr(nid_t start) {
  pgoff_t block_off;
  pgoff_t block_addr;
  pgoff_t seg_off;

  block_off = NatBlockOffset(start);
  seg_off = block_off >> GetSuperblockInfo().GetLogBlocksPerSeg();

  block_addr = static_cast<pgoff_t>(
      nat_blkaddr_ + (seg_off << GetSuperblockInfo().GetLogBlocksPerSeg() << 1) +
      (block_off & ((1 << GetSuperblockInfo().GetLogBlocksPerSeg()) - 1)));

  if (TestValidBitmap(block_off, nat_bitmap_.get()))
    block_addr += GetSuperblockInfo().GetBlocksPerSeg();

  return block_addr;
}

bool NodeManager::IsUpdatedNatPage(nid_t start) {
  pgoff_t block_off;

  block_off = NatBlockOffset(start);

  return (TestValidBitmap(block_off, nat_bitmap_.get()) ^
          TestValidBitmap(block_off, nat_prev_bitmap_.get()));
}

pgoff_t NodeManager::NextNatAddr(pgoff_t block_addr) {
  block_addr -= nat_blkaddr_;
  if ((block_addr >> GetSuperblockInfo().GetLogBlocksPerSeg()) % 2)
    block_addr -= GetSuperblockInfo().GetBlocksPerSeg();
  else
    block_addr += GetSuperblockInfo().GetBlocksPerSeg();

  return block_addr + nat_blkaddr_;
}

void NodeManager::SetToNextNat(nid_t start_nid) {
  pgoff_t block_off = NatBlockOffset(start_nid);

  if (TestValidBitmap(block_off, nat_bitmap_.get()))
    ClearValidBitmap(block_off, nat_bitmap_.get());
  else
    SetValidBitmap(block_off, nat_bitmap_.get());
}

// Coldness identification:
//  - Mark cold files in InodeInfo
//  - Mark cold node blocks in their node footer
//  - Mark cold data pages in page cache
bool NodeManager::IsColdFile(VnodeF2fs &vnode) { return (vnode.IsAdviseSet(FAdvise::kCold) != 0); }

#if 0  // When gc impl, use the cold hint.
int NodeManager::IsColdData(Page *page) {
  // return PageChecked(page);
  return 0;
}

void NodeManager::SetColdData(Page &page) {
  // SetPageChecked(page);
}

void NodeManager::ClearColdData(Page *page) {
  // ClearPageChecked(page);
}
#endif

void NodeManager::DecValidNodeCount(VnodeF2fs *vnode, uint32_t count) {
  std::lock_guard stat_lock(GetSuperblockInfo().GetStatLock());

  ZX_ASSERT(!(GetSuperblockInfo().GetTotalValidBlockCount() < count));
  ZX_ASSERT(!(GetSuperblockInfo().GetTotalValidNodeCount() < count));

  vnode->DecBlocks(count);
  GetSuperblockInfo().SetTotalValidNodeCount(GetSuperblockInfo().GetTotalValidNodeCount() - count);
  GetSuperblockInfo().SetTotalValidBlockCount(GetSuperblockInfo().GetTotalValidBlockCount() -
                                              count);
}

void NodeManager::GetCurrentNatPage(nid_t nid, fbl::RefPtr<Page> *out) {
  pgoff_t index = CurrentNatAddr(nid);
  fs_->GetMetaPage(index, out);
}

void NodeManager::GetNextNatPage(nid_t nid, fbl::RefPtr<Page> *out) {
  fbl::RefPtr<Page> src_page;
  fbl::RefPtr<Page> dst_page;

  pgoff_t src_off = CurrentNatAddr(nid);
  pgoff_t dst_off = NextNatAddr(src_off);

  // get current nat block page with lock
  fs_->GetMetaPage(src_off, &src_page);

  // Dirty src_page means that it is already the new target NAT page
#if 0  // porting needed
  // if (PageDirty(src_page))
#endif
  if (IsUpdatedNatPage(nid)) {
    *out = std::move(src_page);
    return;
  }

  fs_->GrabMetaPage(dst_off, &dst_page);

  memcpy(dst_page->GetAddress(), src_page->GetAddress(), kPageSize);
  dst_page->SetDirty();
  Page::PutPage(std::move(src_page), true);

  SetToNextNat(nid);

  *out = std::move(dst_page);
}

// Readahead NAT pages
void NodeManager::RaNatPages(nid_t nid) {
  for (int i = 0; i < kFreeNidPages; ++i, nid += kNatEntryPerBlock) {
    if (nid >= max_nid_) {
      nid = 0;
    }
    fbl::RefPtr<Page> page;
    pgoff_t index = CurrentNatAddr(nid);
    if (zx_status_t ret = fs_->GetMetaPage(index, &page); ret != ZX_OK) {
      continue;
    }
#if 0  // porting needed
    // page_cache_release(page);
#endif
    Page::PutPage(std::move(page), true);
  }
}

NatEntry *NodeManager::LookupNatCache(nid_t n) {
  if (auto nat_entry = nat_cache_.find(n); nat_entry != nat_cache_.end()) {
    return &(*nat_entry);
  }
  return nullptr;
}

uint32_t NodeManager::GangLookupNatCache(uint32_t nr, NatEntry **out) {
  uint32_t ret = 0;
  for (auto &entry : nat_cache_) {
    out[ret] = &entry;
    if (++ret == nr)
      break;
  }
  return ret;
}

void NodeManager::DelFromNatCache(NatEntry &entry) {
  ZX_ASSERT_MSG(clean_nat_list_.erase(entry) != nullptr, "Cannot find NAT in list(nid = %u)",
                entry.GetNid());
  auto deleted = nat_cache_.erase(entry);
  ZX_ASSERT_MSG(deleted != nullptr, "Cannot find NAT in cache(nid = %u)", entry.GetNid());
  --nat_entries_count_;
}

bool NodeManager::IsCheckpointedNode(nid_t nid) {
  fs::SharedLock nat_lock(nat_tree_lock_);
  NatEntry *ne = LookupNatCache(nid);
  if (ne && !ne->IsCheckpointed()) {
    return false;
  }
  return true;
}

NatEntry *NodeManager::GrabNatEntry(nid_t nid) {
  auto new_entry = std::make_unique<NatEntry>();

  if (!new_entry)
    return nullptr;

  auto entry = new_entry.get();
  entry->SetNid(nid);

  clean_nat_list_.push_back(entry);
  nat_cache_.insert(std::move(new_entry));
  ++nat_entries_count_;
  return entry;
}

void NodeManager::CacheNatEntry(nid_t nid, RawNatEntry &raw_entry) {
  while (true) {
    std::lock_guard lock(nat_tree_lock_);
    NatEntry *entry = LookupNatCache(nid);
    if (!entry) {
      if (entry = GrabNatEntry(nid); !entry) {
        continue;
      }
    }
    entry->SetBlockAddress(LeToCpu(raw_entry.block_addr));
    entry->SetIno(LeToCpu(raw_entry.ino));
    entry->SetVersion(raw_entry.version);
    entry->SetCheckpointed();
    break;
  }
}

void NodeManager::SetNodeAddr(NodeInfo &ni, block_t new_blkaddr) {
  while (true) {
    std::lock_guard nat_lock(nat_tree_lock_);
    NatEntry *entry = LookupNatCache(ni.nid);
    if (!entry) {
      entry = GrabNatEntry(ni.nid);
      if (!entry) {
        continue;
      }
      entry->SetNodeInfo(ni);
      entry->SetCheckpointed();
      ZX_ASSERT(ni.blk_addr != kNewAddr);
    } else if (new_blkaddr == kNewAddr) {
      // when nid is reallocated,
      // previous nat entry can be remained in nat cache.
      // So, reinitialize it with new information.
      entry->SetNodeInfo(ni);
      ZX_ASSERT(ni.blk_addr == kNullAddr);
    }

    if (new_blkaddr == kNewAddr) {
      entry->ClearCheckpointed();
    }

    // sanity check
    ZX_ASSERT(!(entry->GetBlockAddress() != ni.blk_addr));
    ZX_ASSERT(!(entry->GetBlockAddress() == kNullAddr && new_blkaddr == kNullAddr));
    ZX_ASSERT(!(entry->GetBlockAddress() == kNewAddr && new_blkaddr == kNewAddr));
    ZX_ASSERT(!(entry->GetBlockAddress() != kNewAddr && entry->GetBlockAddress() != kNullAddr &&
                new_blkaddr == kNewAddr));

    // increament version no as node is removed
    if (entry->GetBlockAddress() != kNewAddr && new_blkaddr == kNullAddr) {
      uint8_t version = entry->GetVersion();
      entry->SetVersion(IncNodeVersion(version));
    }

    // change address
    entry->SetBlockAddress(new_blkaddr);
    SetNatCacheDirty(*entry);
    break;
  }
}

int NodeManager::TryToFreeNats(int nr_shrink) {
  if (nat_entries_count_ < 2 * kNmWoutThreshold)
    return 0;

  std::lock_guard nat_lock(nat_tree_lock_);
  while (nr_shrink && !clean_nat_list_.is_empty()) {
    NatEntry *cache_entry = &clean_nat_list_.front();
    DelFromNatCache(*cache_entry);
    --nr_shrink;
  }
  return nr_shrink;
}

// This function returns always success
void NodeManager::GetNodeInfo(nid_t nid, NodeInfo &out) {
  CursegInfo *curseg = fs_->GetSegmentManager().CURSEG_I(CursegType::kCursegHotData);
  SummaryBlock *sum = curseg->sum_blk;
  nid_t start_nid = StartNid(nid);
  NatBlock *nat_blk;
  fbl::RefPtr<Page> page;
  RawNatEntry ne;
  int i;

  out.nid = nid;

  {
    // Check nat cache
    fs::SharedLock nat_lock(nat_tree_lock_);
    NatEntry *entry = LookupNatCache(nid);
    if (entry) {
      out.ino = entry->GetIno();
      out.blk_addr = entry->GetBlockAddress();
      out.version = entry->GetVersion();
      return;
    }
  }

  {
    // Check current segment summary
    std::lock_guard curseg_lock(curseg->curseg_mutex);
    i = LookupJournalInCursum(sum, JournalType::kNatJournal, nid, 0);
    if (i >= 0) {
      ne = NatInJournal(sum, i);
      NodeInfoFromRawNat(out, ne);
    }
  }
  if (i < 0) {
    // Fill NodeInfo from nat page
    GetCurrentNatPage(start_nid, &page);
    nat_blk = page->GetAddress<NatBlock>();
    ne = nat_blk->entries[nid - start_nid];

    NodeInfoFromRawNat(out, ne);
    Page::PutPage(std::move(page), true);
  }
  CacheNatEntry(nid, ne);
}

// The maximum depth is four.
// Offset[0] will have raw inode offset.
zx::status<int32_t> NodeManager::GetNodePath(VnodeF2fs &vnode, pgoff_t block, int32_t (&offset)[4],
                                             uint32_t (&noffset)[4]) {
  const pgoff_t direct_index = kAddrsPerInode - (vnode.GetExtraISize() / sizeof(uint32_t));
  const pgoff_t direct_blks = kAddrsPerBlock;
  const pgoff_t dptrs_per_blk = kNidsPerBlock;
  const pgoff_t indirect_blks = kAddrsPerBlock * kNidsPerBlock;
  const pgoff_t dindirect_blks = indirect_blks * kNidsPerBlock;
  int32_t n = 0;
  int32_t level = 0;

  noffset[0] = 0;
  do {
    if (block < direct_index) {
      offset[n++] = static_cast<int>(block);
      level = 0;
      break;
    }
    block -= direct_index;
    if (block < direct_blks) {
      offset[n++] = kNodeDir1Block;
      noffset[n] = 1;
      offset[n++] = static_cast<int>(block);
      level = 1;
      break;
    }
    block -= direct_blks;
    if (block < direct_blks) {
      offset[n++] = kNodeDir2Block;
      noffset[n] = 2;
      offset[n++] = static_cast<int>(block);
      level = 1;
      break;
    }
    block -= direct_blks;
    if (block < indirect_blks) {
      offset[n++] = kNodeInd1Block;
      noffset[n] = 3;
      offset[n++] = static_cast<int>(block / direct_blks);
      noffset[n] = 4 + offset[n - 1];
      offset[n++] = block % direct_blks;
      level = 2;
      break;
    }
    block -= indirect_blks;
    if (block < indirect_blks) {
      offset[n++] = kNodeInd2Block;
      noffset[n] = 4 + dptrs_per_blk;
      offset[n++] = static_cast<int>(block / direct_blks);
      noffset[n] = 5 + dptrs_per_blk + offset[n - 1];
      offset[n++] = block % direct_blks;
      level = 2;
      break;
    }
    block -= indirect_blks;
    if (block < dindirect_blks) {
      offset[n++] = kNodeDIndBlock;
      noffset[n] = 5 + (dptrs_per_blk * 2);
      offset[n++] = static_cast<int>(block / indirect_blks);
      noffset[n] = 6 + (dptrs_per_blk * 2) + offset[n - 1] * (dptrs_per_blk + 1);
      offset[n++] = (block / direct_blks) % dptrs_per_blk;
      noffset[n] = 7 + (dptrs_per_blk * 2) + offset[n - 2] * (dptrs_per_blk + 1) + offset[n - 1];
      offset[n++] = block % direct_blks;
      level = 3;
      break;
    } else {
      return zx::error(ZX_ERR_NOT_FOUND);
    }
  } while (false);
  return zx::ok(level);
}

zx_status_t NodeManager::FindLockedDnodePage(VnodeF2fs &vnode, pgoff_t index,
                                             LockedPage<NodePage> *out) {
  int32_t offset[4];
  uint32_t noffset[4];

  auto node_path = GetNodePath(vnode, index, offset, noffset);
  if (node_path.is_error()) {
    return node_path.error_value();
  }

  auto level = *node_path;

  nid_t nid = vnode.Ino();

  for (auto i = 0; i <= level; ++i) {
    // TODO: node_page type should be LockedPage when GetNodePage returns LockedPage instead of
    // RefPtr.
    fbl::RefPtr<NodePage> node_page;
    if (zx_status_t err = GetNodePage(nid, &node_page); err != ZX_OK) {
      return err;
    }

    if (i < level) {
      nid = node_page->GetNid(offset[i], IsInode(*node_page));
      Page::PutPage(std::move(node_page), true);
    } else {
      node_page->Unlock();
      *out = LockedPage<NodePage>(node_page);
    }
  }
  return ZX_OK;
}

zx_status_t NodeManager::GetLockedDnodePage(VnodeF2fs &vnode, pgoff_t index,
                                            LockedPage<NodePage> *out) {
  int32_t offset[4];
  uint32_t noffset[4];

  auto node_path = GetNodePath(vnode, index, offset, noffset);
  if (node_path.is_error()) {
    return node_path.error_value();
  }

  auto level = *node_path;

  nid_t nid = vnode.Ino();
  LockedPage<NodePage> parent;
  for (auto i = 0; i <= level; ++i) {
    LockedPage<NodePage> node_page;
    if (!nid && i > 0) {
      // alloc new node
      if (!AllocNid(nid)) {
        return ZX_ERR_NO_SPACE;
      }
      // TODO: page_locked will be removed when GetNodePage returns LockedPage instead of RefPtr.
      fbl::RefPtr<NodePage> page_locked;
      if (zx_status_t err = NewNodePage(vnode, nid, noffset[i], &page_locked); err != ZX_OK) {
        AllocNidFailed(nid);
        return err;
      }
      page_locked->Unlock();
      node_page = LockedPage<NodePage>(page_locked);

      parent->SetNid(offset[i - 1], nid, IsInode(*parent));
      AllocNidDone(nid);
    } else {
      // TODO: page_locked will be removed when GetNodePage returns LockedPage instead of RefPtr.
      fbl::RefPtr<NodePage> page_locked;
      if (zx_status_t err = GetNodePage(nid, &page_locked); err != ZX_OK) {
        return err;
      }
      page_locked->Unlock();
      node_page = LockedPage<NodePage>(page_locked);
    }

    if (i < level) {
      nid = node_page->GetNid(offset[i], IsInode(*node_page));
      parent = std::move(node_page);
    } else {
      *out = std::move(node_page);
    }
  }
  return ZX_OK;
}

zx::status<uint32_t> NodeManager::GetOfsInDnode(VnodeF2fs &vnode, pgoff_t index) {
  int32_t offset[4];
  uint32_t noffset[4];

  auto node_path = GetNodePath(vnode, index, offset, noffset);
  if (node_path.is_error()) {
    return zx::error(node_path.error_value());
  }

  return zx::ok(offset[*node_path]);
}

void NodeManager::TruncateNode(VnodeF2fs &vnode, nid_t nid, NodePage &node_page) {
  NodeInfo ni;
  GetNodeInfo(nid, ni);
  ZX_ASSERT(ni.blk_addr != kNullAddr);

  if (ni.blk_addr != kNullAddr) {
    fs_->GetSegmentManager().InvalidateBlocks(ni.blk_addr);
  }

  // Deallocate node address
  DecValidNodeCount(&vnode, 1);
  SetNodeAddr(ni, kNullAddr);

  if (nid == vnode.Ino()) {
    fs_->RemoveOrphanInode(nid);
    fs_->DecValidInodeCount();
  } else {
    vnode.MarkInodeDirty();
  }

  node_page.Invalidate();
  GetSuperblockInfo().SetDirty();
}

zx::status<uint32_t> NodeManager::TruncateDnode(VnodeF2fs &vnode, nid_t nid) {
  fbl::RefPtr<NodePage> page;

  if (nid == 0) {
    return zx::ok(1);
  }

  // get direct node
  if (auto err = fs_->GetNodeManager().GetNodePage(nid, &page); err != ZX_OK) {
    // It is already invalid.
    if (err == ZX_ERR_NOT_FOUND) {
      return zx::ok(1);
    }
    return zx::error(err);
  }

  vnode.TruncateDataBlocks(*page);
  TruncateNode(vnode, nid, *page);
  Page::PutPage(std::move(page), true);
  return zx::ok(1);
}

zx::status<uint32_t> NodeManager::TruncateNodes(VnodeF2fs &vnode, nid_t start_nid, uint32_t nofs,
                                                int32_t ofs, int32_t depth) {
  if (start_nid == 0) {
    return zx::ok(kNidsPerBlock + 1);
  }

  fbl::RefPtr<NodePage> page;
  if (auto ret = fs_->GetNodeManager().GetNodePage(start_nid, &page); ret != ZX_OK) {
    return zx::error(ret);
  }

  uint32_t child_nofs = 0, freed = 0;
  nid_t child_nid;
  Node *rn = page->GetAddress<Node>();
  if (depth < 3) {
    for (auto i = ofs; i < kNidsPerBlock; ++i, ++freed) {
      child_nid = LeToCpu(rn->in.nid[i]);
      if (child_nid == 0) {
        continue;
      }
      if (auto ret = TruncateDnode(vnode, child_nid); ret.is_error()) {
        Page::PutPage(std::move(page), true);
        return ret;
      }
      page->SetNid(i, 0, false);
    }
  } else {
    child_nofs = nofs + ofs * (kNidsPerBlock + 1) + 1;
    for (auto i = ofs; i < kNidsPerBlock; ++i) {
      child_nid = LeToCpu(rn->in.nid[i]);
      if (child_nid == 0) {
        child_nofs += kNidsPerBlock + 1;
        continue;
      }
      auto freed_or = TruncateNodes(vnode, child_nid, child_nofs, 0, depth - 1);
      if (freed_or.is_error()) {
        if (freed_or.error_value() != ZX_ERR_NOT_FOUND) {
          Page::PutPage(std::move(page), true);
          return freed_or;
        }
      } else if (*freed_or == (kNidsPerBlock + 1)) {
        page->SetNid(i, 0, false);
        child_nofs += *freed_or;
      }
    }
    freed = child_nofs;
  }

  if (!ofs) {
    TruncateNode(vnode, start_nid, *page);
    ++freed;
  }
  Page::PutPage(std::move(page), true);
  return zx::ok(freed);
}

zx_status_t NodeManager::TruncatePartialNodes(VnodeF2fs &vnode, const Inode &ri,
                                              const int32_t (&offset)[4], int32_t depth) {
  fbl::RefPtr<NodePage> pages[2];
  nid_t nid[3];
  auto idx = depth - 2;
  auto free_pages = fit::defer([&]() {
    for (auto i = idx; i >= 0; --i) {
      if (pages[i]) {
        Page::PutPage(std::move(pages[i]), true);
      }
    }
  });

  if (nid[0] = LeToCpu(ri.i_nid[offset[0] - kNodeDir1Block]); !nid[0]) {
    return ZX_OK;
  }

  // get indirect nodes in the path
  for (auto i = 0; i < idx + 1; ++i) {
    pages[i] = nullptr;
    if (auto ret = fs_->GetNodeManager().GetNodePage(nid[i], &pages[i]); ret != ZX_OK) {
      // idx will be used in free_pages
      idx = i - 1;
      return ret;
    }
    nid[i + 1] = pages[i]->GetNid(offset[i + 1], false);
  }

  // free direct nodes linked to a partial indirect node
  for (auto i = offset[idx + 1]; i < kNidsPerBlock; ++i) {
    nid_t child_nid = pages[idx]->GetNid(i, false);
    if (!child_nid)
      continue;
    if (auto ret = TruncateDnode(vnode, child_nid); ret.is_error()) {
      return ret.error_value();
    }
    pages[idx]->SetNid(i, 0, false);
  }

  if (offset[idx + 1] == 0) {
    TruncateNode(vnode, nid[idx], *pages[idx]);
  }
  Page::PutPage(std::move(pages[idx]), true);
  // idx will be used in free_pages
  --idx;
  return ZX_OK;
}

// All the block addresses of data and nodes should be nullified.
zx_status_t NodeManager::TruncateInodeBlocks(VnodeF2fs &vnode, pgoff_t from) {
  uint32_t nofs, noffset[4];
  int32_t offset[4];
  auto node_path = GetNodePath(vnode, from, offset, noffset);
  if (node_path.is_error())
    return node_path.error_value();

  fbl::RefPtr<NodePage> ipage;
  if (auto ret = GetNodePage(vnode.Ino(), &ipage); ret != ZX_OK) {
    return ret;
  }
  ipage->Unlock();

  auto level = *node_path;
  Node *rn = ipage->GetAddress<Node>();
  switch (level) {
    case 0:
    case 1:
      nofs = noffset[1];
      break;
    case 2:
      nofs = noffset[1];
      if (!offset[level - 1]) {
        break;
      }
      if (auto ret = TruncatePartialNodes(vnode, rn->i, offset, level);
          ret != ZX_OK && ret != ZX_ERR_NOT_FOUND) {
        Page::PutPage(std::move(ipage), false);
        return ret;
      }
      ++offset[level - 2];
      offset[level - 1] = 0;
      nofs += 1 + kNidsPerBlock;
      break;
    case 3:
      nofs = 5 + 2 * kNidsPerBlock;
      if (!offset[level - 1]) {
        break;
      }
      if (auto ret = TruncatePartialNodes(vnode, rn->i, offset, level);
          ret != ZX_OK && ret != ZX_ERR_NOT_FOUND) {
        Page::PutPage(std::move(ipage), false);
        return ret;
      }
      ++offset[level - 2];
      offset[level - 1] = 0;
      break;
    default:
      ZX_ASSERT(0);
  }

  bool run = true;
  while (run) {
    zx::status<uint32_t> freed_or;
    nid_t nid = LeToCpu(rn->i.i_nid[offset[0] - kNodeDir1Block]);
    switch (offset[0]) {
      case kNodeDir1Block:
      case kNodeDir2Block:
        freed_or = TruncateDnode(vnode, nid);
        break;

      case kNodeInd1Block:
      case kNodeInd2Block:
        freed_or = TruncateNodes(vnode, nid, nofs, offset[1], 2);
        break;

      case kNodeDIndBlock:
        freed_or = TruncateNodes(vnode, nid, nofs, offset[1], 3);
        run = false;
        break;

      default:
        ZX_ASSERT(0);
    }
    if (freed_or.is_error()) {
      if (freed_or.error_value() != ZX_ERR_NOT_FOUND) {
        Page::PutPage(std::move(ipage), false);
        return freed_or.error_value();
      }
      // Do not count invalid nodes.
      freed_or = zx::ok(0);
    }
    if (offset[1] == 0 && rn->i.i_nid[offset[0] - kNodeDir1Block]) {
      ipage->Lock();
      ipage->WaitOnWriteback();
      rn->i.i_nid[offset[0] - kNodeDir1Block] = 0;
      ipage->SetDirty();
      ipage->Unlock();
    }
    offset[1] = 0;
    ++offset[0];
    nofs += *freed_or;
  }
  Page::PutPage(std::move(ipage), false);
  return ZX_OK;
}

zx_status_t NodeManager::RemoveInodePage(VnodeF2fs *vnode) {
  fbl::RefPtr<NodePage> ipage;
  nid_t ino = vnode->Ino();
  zx_status_t err = 0;

  err = GetNodePage(ino, &ipage);
  if (err) {
    return err;
  }

  if (nid_t nid = vnode->GetXattrNid(); nid > 0) {
    fbl::RefPtr<NodePage> node_page;
    if (auto err = GetNodePage(nid, &node_page); err != ZX_OK) {
      return err;
    }

    vnode->ClearXattrNid();
    TruncateNode(*vnode, nid, *node_page);
    Page::PutPage(std::move(node_page), true);
  }
  if (vnode->GetBlocks() == 1) {
    TruncateNode(*vnode, ino, *ipage);
    Page::PutPage(std::move(ipage), true);
  } else if (vnode->GetBlocks() == 0) {
    NodeInfo ni;
    GetNodeInfo(vnode->Ino(), ni);
    ZX_ASSERT(ni.blk_addr == kNullAddr);
    Page::PutPage(std::move(ipage), true);
  } else {
    ZX_ASSERT(0);
  }
  return ZX_OK;
}

zx_status_t NodeManager::NewInodePage(Dir *parent, VnodeF2fs *child) {
  fbl::RefPtr<NodePage> page;

  // allocate inode page for new inode
  if (zx_status_t ret = NewNodePage(*child, child->Ino(), 0, &page); ret != ZX_OK) {
    return ret;
  }
  parent->InitDentInode(child, page.get());

  Page::PutPage(std::move(page), true);
  page.reset();
  return ZX_OK;
}

zx_status_t NodeManager::NewNodePage(VnodeF2fs &vnode, nid_t nid, uint32_t ofs,
                                     fbl::RefPtr<NodePage> *out) {
  NodeInfo old_ni, new_ni;

  if (vnode.TestFlag(InodeInfoFlag::kNoAlloc)) {
    return ZX_ERR_ACCESS_DENIED;
  }

  if (zx_status_t ret = fs_->GetNodeVnode().GrabCachePage(nid, out); ret != ZX_OK) {
    return ZX_ERR_NO_MEMORY;
  }

  GetNodeInfo(nid, old_ni);

  (*out)->SetUptodate();
  (*out)->FillNodeFooter(nid, vnode.Ino(), ofs, true);

  // Reinitialize old_ni with new node page
  ZX_ASSERT(old_ni.blk_addr == kNullAddr);
  new_ni = old_ni;
  new_ni.ino = vnode.Ino();

  if (!IncValidNodeCount(&vnode, 1)) {
    (*out)->ClearUptodate();
    Page::PutPage(std::move(*out), true);
#ifdef __Fuchsia__
    fs_->GetInspectTree().OnOutOfSpace();
#endif
    return ZX_ERR_NO_SPACE;
  }
  SetNodeAddr(new_ni, kNewAddr);

  vnode.MarkInodeDirty();

  (*out)->SetDirty();
  (*out)->SetColdNode(vnode);
  if (ofs == 0)
    fs_->IncValidInodeCount();

  return ZX_OK;
}

zx_status_t NodeManager::ReadNodePage(fbl::RefPtr<Page> page, nid_t nid, int type) {
  NodeInfo ni;

  GetNodeInfo(nid, ni);

  if (ni.blk_addr == kNullAddr)
    return ZX_ERR_NOT_FOUND;

  return fs_->MakeOperation(storage::OperationType::kRead, std::move(page), ni.blk_addr,
                            PageType::kNode);
}

// TODO: This method should return LockedPage instead of RefPtr when Page Lock is fully managed by
// wrapper class.
zx_status_t NodeManager::GetNodePage(nid_t nid, fbl::RefPtr<NodePage> *out) {
  if (zx_status_t ret = fs_->GetNodeVnode().GrabCachePage(nid, out); ret != ZX_OK) {
    return ret;
  }

  if (zx_status_t ret = ReadNodePage(*out, nid, kReadSync); ret != ZX_OK) {
    Page::PutPage(std::move(*out), true);
    return ret;
  }

  ZX_ASSERT(nid == (*out)->NidOfNode());
#if 0  // porting needed
  // mark_page_accessed(page);
#endif
  return ZX_OK;
}

#if 0  // porting needed
// TODO: Readahead a node page
void NodeManager::RaNodePage(nid_t nid) {
  // TODO: IMPL Read ahead
}
// Return a locked page for the desired node page.
// And, readahead kMaxRaNode number of node pages.
Page *NodeManager::GetNodePageRa(Page *parent, int start) {
  // TODO: IMPL Read ahead
  return nullptr;
}
#endif

pgoff_t NodeManager::SyncNodePages(WritebackOperation &operation) {
  if (superblock_info_->GetPageCount(CountType::kDirtyNodes) == 0 && !operation.bReleasePages) {
    return 0;
  }
  if (zx_status_t status = fs_->GetVCache().ForDirtyVnodesIf(
          [this](fbl::RefPtr<VnodeF2fs> &vnode) {
            if (!vnode->ShouldFlush()) {
              ZX_ASSERT(fs_->GetVCache().RemoveDirty(vnode.get()) == ZX_OK);
              return ZX_ERR_NEXT;
            }
            ZX_ASSERT(vnode->WriteInode(false) == ZX_OK);
            ZX_ASSERT(fs_->GetVCache().RemoveDirty(vnode.get()) == ZX_OK);
            ZX_ASSERT(vnode->ClearDirty() == true);
            return ZX_OK;
          },
          [](fbl::RefPtr<VnodeF2fs> &vnode) {
            if (vnode->GetDirtyPageCount()) {
              return ZX_ERR_NEXT;
            }
            return ZX_OK;
          });
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to flush dirty vnodes ";
    return 0;
  }
  // TODO: Consider ordered writeback
  return fs_->GetNodeVnode().Writeback(operation);

#if 0  // porting needed
  // SuperblockInfo &superblock_info = GetSuperblockInfo();
  // //address_space *mapping = superblock_info.node_inode->i_mapping;
  // pgoff_t index, end;
  // // TODO: IMPL
  // //pagevec pvec;
  // int step = ino ? 2 : 0;
  // int nwritten = 0, wrote = 0;

  // // TODO: IMPL
  // //pagevec_init(&pvec, 0);

  // next_step:
  // 	index = 0;
  // 	end = LONG_MAX;

  // 	while (index <= end) {
  // 		int nr_pages;
  //  TODO: IMPL
  // nr_pages = pagevec_lookup_tag(&pvec, mapping, &index,
  // 		PAGECACHE_TAG_DIRTY,
  // 		min(end - index, (pgoff_t)PAGEVEC_SIZE-1) + 1);
  // if (nr_pages == 0)
  // 	break;

  // 		for (int i = 0; i < nr_pages; ++i) {
  // 			page *page = pvec.pages[i];

  // 			/*
  // 			 * flushing sequence with step:
  // 			 * 0. indirect nodes
  // 			 * 1. dentry dnodes
  // 			 * 2. file dnodes
  // 			 */
  // 			if (step == 0 && IS_DNODE(page))
  // 				continue;
  // 			if (step == 1 && (!IS_DNODE(page) ||
  // 						IsColdNode(page)))
  // 				continue;
  // 			if (step == 2 && (!IS_DNODE(page) ||
  // 						!IsColdNode(page)))
  // 				continue;

  // 			/*
  // 			 * If an fsync mode,
  // 			 * we should not skip writing node pages.
  // 			 */
  // 			if (ino && InoOfNode(page) == ino)
  // 				lock_page(page);
  // 			else if (!trylock_page(page))
  // 				continue;

  // 			if (unlikely(page->mapping != mapping)) {
  // continue_unlock:
  // 				unlock_page(page);
  // 				continue;
  // 			}
  // 			if (ino && InoOfNode(page) != ino)
  // 				goto continue_unlock;

  // 			if (!PageDirty(page)) {
  // 				/* someone wrote it for us */
  // 				goto continue_unlock;
  // 			}

  // 			if (!ClearPageDirtyForIo(page))
  // 				goto continue_unlock;

  // 			/* called by fsync() */
  // 			if (ino && IS_DNODE(page)) {
  // 				int mark = !IsCheckpointedNode(superblock_info, ino);
  // 				SetFsyncMark(page, 1);
  // 				if (IsInode(page))
  // 					SetDentryMark(page, mark);
  // 				++nwritten;
  // 			} else {
  // 				SetFyncMark(page, 0);
  // 				SetDentryMark(page, 0);
  // 			}
  // 			mapping->a_ops->writepage(page, wbc);
  // 			++wrote;

  // 			if (--wbc->nr_to_write == 0)
  // 				break;
  // 		}
  // 		pagevec_release(&pvec);
  // 		cond_resched();

  // 		if (wbc->nr_to_write == 0) {
  // 			step = 2;
  // 			break;
  // 		}
  // 	}

  // 	if (step < 2) {
  // 		++step;
  // 		goto next_step;
  // 	}

  // 	if (wrote)
  // 		f2fs_submit_bio(superblock_info, NODE, wbc->sync_mode == WB_SYNC_ALL);

  //	return nwritten;
#endif
}

zx_status_t NodeManager::F2fsWriteNodePage(fbl::RefPtr<NodePage> page, bool is_reclaim) {
#if 0  // porting needed
  // 	if (wbc->for_reclaim) {
  // 		superblock_info.DecreasePageCount(CountType::kDirtyNodes);
  // 		++wbc->pages_skipped;
  //		// set_page_dirty(page);
  //		FlushDirtyNodePage(fs_, page);
  // 		return kAopWritepageActivate;
  // 	}
#endif
  page->WaitOnWriteback();
  if (page->ClearDirtyForIo(true)) {
    page->SetWriteback();
    fs::SharedLock rlock(GetSuperblockInfo().GetFsLock(LockType::kNodeOp));
    // get old block addr of this node page
    nid_t nid = page->NidOfNode();
    ZX_ASSERT(page->GetIndex() == nid);

    NodeInfo ni;
    GetNodeInfo(nid, ni);
    // This page is already truncated
    if (ni.blk_addr == kNullAddr) {
      return ZX_ERR_NOT_FOUND;
    }

    block_t new_addr;
    // insert node offset
    fs_->GetSegmentManager().WriteNodePage(std::move(page), nid, ni.blk_addr, &new_addr);
    SetNodeAddr(ni, new_addr);
  }
  return ZX_OK;
}

#if 0  // porting needed
int NodeManager::F2fsWriteNodePages(struct address_space *mapping, WritebackControl *wbc) {
  // struct SuperblockInfo *superblock_info = F2FS_SB(mapping->host->i_sb);
  // struct block_device *bdev = superblock_info->sb->s_bdev;
  // long nr_to_write = wbc->nr_to_write;

  // if (wbc->for_kupdate)
  // 	return 0;

  // if (superblock_info->GetPageCount(CountType::kDirtyNodes) == 0)
  // 	return 0;

  // if (try_to_free_nats(superblock_info, kNatEntryPerBlock)) {
  // 	write_checkpoint(superblock_info, false, false);
  // 	return 0;
  // }

  // /* if mounting is failed, skip writing node pages */
  // wbc->nr_to_write = bio_get_nr_vecs(bdev);
  // sync_node_pages(superblock_info, 0, wbc);
  // wbc->nr_to_write = nr_to_write -
  // 	(bio_get_nr_vecs(bdev) - wbc->nr_to_write);
  // return 0;
  return 0;
}
#endif

FreeNid *NodeManager::LookupFreeNidList(nid_t n) {
  list_node_t *this_list;
  FreeNid *i = nullptr;
  list_for_every(&free_nid_list_, this_list) {
    i = containerof(this_list, FreeNid, list);
    if (i->nid == n)
      break;
    i = nullptr;
  }
  return i;
}

void NodeManager::DelFromFreeNidList(FreeNid *i) {
  list_delete(&i->list);
  delete i;
}

int NodeManager::AddFreeNid(nid_t nid) {
  FreeNid *i;

  if (free_nid_count_ > 2 * kMaxFreeNids)
    return 0;
  do {
    i = new FreeNid;
    sleep(0);
  } while (!i);
  i->nid = nid;
  i->state = static_cast<int>(NidState::kNidNew);

  std::lock_guard free_nid_lock(free_nid_list_lock_);
  if (LookupFreeNidList(nid)) {
    delete i;
    return 0;
  }
  list_add_tail(&free_nid_list_, &i->list);
  ++free_nid_count_;
  return 1;
}

void NodeManager::RemoveFreeNid(nid_t nid) {
  FreeNid *i;
  std::lock_guard free_nid_lock(free_nid_list_lock_);
  i = LookupFreeNidList(nid);
  if (i && i->state == static_cast<int>(NidState::kNidNew)) {
    DelFromFreeNidList(i);
    --free_nid_count_;
  }
}

int NodeManager::ScanNatPage(Page &nat_page, nid_t start_nid) {
  NatBlock *nat_blk = nat_page.GetAddress<NatBlock>();
  block_t blk_addr;
  int fcnt = 0;

  // 0 nid should not be used
  if (start_nid == 0)
    ++start_nid;

  for (uint32_t i = start_nid % kNatEntryPerBlock; i < kNatEntryPerBlock; ++i, ++start_nid) {
    blk_addr = LeToCpu(nat_blk->entries[i].block_addr);
    ZX_ASSERT(blk_addr != kNewAddr);
    if (blk_addr == kNullAddr)
      fcnt += AddFreeNid(start_nid);
  }
  return fcnt;
}

void NodeManager::BuildFreeNids() {
  FreeNid *fnid, *next_fnid;
  CursegInfo *curseg = fs_->GetSegmentManager().CURSEG_I(CursegType::kCursegHotData);
  SummaryBlock *sum = curseg->sum_blk;
  nid_t nid = 0;
  bool is_cycled = false;
  uint64_t fcnt = 0;

  nid = next_scan_nid_;
  init_scan_nid_ = nid;

  RaNatPages(nid);

  while (true) {
    fbl::RefPtr<Page> page;
    GetCurrentNatPage(nid, &page);

    fcnt += ScanNatPage(*page, nid);
    Page::PutPage(std::move(page), 1);

    nid += (kNatEntryPerBlock - (nid % kNatEntryPerBlock));

    if (nid >= max_nid_) {
      nid = 0;
      is_cycled = true;
    }
    if (fcnt > kMaxFreeNids)
      break;
    if (is_cycled && init_scan_nid_ <= nid)
      break;
  }

  next_scan_nid_ = nid;

  {
    // find free nids from current sum_pages
    std::lock_guard curseg_lock(curseg->curseg_mutex);
    for (int i = 0; i < NatsInCursum(sum); ++i) {
      block_t addr = LeToCpu(NatInJournal(sum, i).block_addr);
      nid = LeToCpu(NidInJournal(sum, i));
      if (addr == kNullAddr) {
        AddFreeNid(nid);
      } else {
        RemoveFreeNid(nid);
      }
    }
  }

  // remove the free nids from current allocated nids
  list_for_every_entry_safe (&free_nid_list_, fnid, next_fnid, FreeNid, list) {
    fs::SharedLock nat_lock(nat_tree_lock_);
    NatEntry *entry = LookupNatCache(fnid->nid);
    if (entry && entry->GetBlockAddress() != kNullAddr) {
      RemoveFreeNid(fnid->nid);
    }
  }
}

// If this function returns success, caller can obtain a new nid
// from second parameter of this function.
// The returned nid could be used ino as well as nid when inode is created.
bool NodeManager::AllocNid(nid_t &out) {
  FreeNid *i = nullptr;
  list_node_t *this_list;
  do {
    {
      std::lock_guard lock(build_lock_);
      if (!free_nid_count_) {
        // scan NAT in order to build free nid list
        BuildFreeNids();
        if (!free_nid_count_) {
#ifdef __Fuchsia__
          fs_->GetInspectTree().OnOutOfSpace();
#endif
          return false;
        }
      }
    }
    // We check fcnt again since previous check is racy as
    // we didn't hold free_nid_list_lock. So other thread
    // could consume all of free nids.
  } while (!free_nid_count_);

  std::lock_guard lock(free_nid_list_lock_);
  ZX_ASSERT(!list_is_empty(&free_nid_list_));

  list_for_every(&free_nid_list_, this_list) {
    i = containerof(this_list, FreeNid, list);
    if (i->state == static_cast<int>(NidState::kNidNew))
      break;
  }

  ZX_ASSERT(i->state == static_cast<int>(NidState::kNidNew));
  out = i->nid;
  i->state = static_cast<int>(NidState::kNidAlloc);
  --free_nid_count_;
  return true;
}

// alloc_nid() should be called prior to this function.
void NodeManager::AllocNidDone(nid_t nid) {
  FreeNid *i;

  std::lock_guard free_nid_lock(free_nid_list_lock_);
  i = LookupFreeNidList(nid);
  if (i) {
    ZX_ASSERT(i->state == static_cast<int>(NidState::kNidAlloc));
    DelFromFreeNidList(i);
  }
}

// alloc_nid() should be called prior to this function.
void NodeManager::AllocNidFailed(nid_t nid) {
  AllocNidDone(nid);
  AddFreeNid(nid);
}

void NodeManager::RecoverNodePage(fbl::RefPtr<NodePage> page, Summary &sum, NodeInfo &ni,
                                  block_t new_blkaddr) {
  fs_->GetSegmentManager().RewriteNodePage(page, &sum, ni.blk_addr, new_blkaddr);
  SetNodeAddr(ni, new_blkaddr);
  page->Invalidate();
  // TODO: Remove it when impl. recovery.
  ZX_ASSERT(0);
}

zx_status_t NodeManager::RecoverInodePage(NodePage &page) {
  Node *src, *dst;
  nid_t ino = page.InoOfNode();
  NodeInfo old_ni, new_ni;
  fbl::RefPtr<NodePage> ipage;

  if (zx_status_t ret = fs_->GetNodeVnode().GrabCachePage(ino, &ipage); ret != ZX_OK) {
    return ret;
  }

  // Should not use this inode  from free nid list
  RemoveFreeNid(ino);

  GetNodeInfo(ino, old_ni);

  ipage->SetUptodate();
  ipage->FillNodeFooter(ino, ino, 0, true);

  src = page.GetAddress<Node>();
  dst = ipage->GetAddress<Node>();

  memcpy(dst, src, reinterpret_cast<uint64_t>(&src->i.i_ext) - reinterpret_cast<uint64_t>(&src->i));
  dst->i.i_size = 0;
  dst->i.i_blocks = 1;
  dst->i.i_links = 1;
  dst->i.i_xattr_nid = 0;

  new_ni = old_ni;
  new_ni.ino = ino;

  SetNodeAddr(new_ni, kNewAddr);
  fs_->IncValidInodeCount();

  Page::PutPage(std::move(ipage), true);
  return ZX_OK;
}

zx_status_t NodeManager::RestoreNodeSummary(uint32_t segno, SummaryBlock &sum) {
  int last_offset = GetSuperblockInfo().GetBlocksPerSeg();
  block_t addr = fs_->GetSegmentManager().StartBlock(segno);
  Summary *sum_entry = &sum.entries[0];

  for (int i = 0; i < last_offset; ++i, ++sum_entry, ++addr) {
    fbl::RefPtr<Page> page;
    if (zx_status_t ret = fs_->GetMetaPage(addr, &page); ret != ZX_OK) {
      return ret;
    }

    Node *rn = page->GetAddress<Node>();
    sum_entry->nid = rn->footer.nid;
    sum_entry->version = 0;
    sum_entry->ofs_in_node = 0;

    page->Invalidate();
    Page::PutPage(std::move(page), true);
  }
  return ZX_OK;
}

bool NodeManager::FlushNatsInJournal() {
  CursegInfo *curseg = fs_->GetSegmentManager().CURSEG_I(CursegType::kCursegHotData);
  SummaryBlock *sum = curseg->sum_blk;
  int i;

  std::lock_guard curseg_lock(curseg->curseg_mutex);

  {
    fs::SharedLock nat_lock(nat_tree_lock_);
    size_t dirty_nat_cnt = dirty_nat_list_.size_slow();
    if ((NatsInCursum(sum) + dirty_nat_cnt) <= kNatJournalEntries) {
      return false;
    }
  }

  for (i = 0; i < NatsInCursum(sum); ++i) {
    NatEntry *cache_entry = nullptr;
    RawNatEntry raw_entry = NatInJournal(sum, i);
    nid_t nid = LeToCpu(NidInJournal(sum, i));

    while (!cache_entry) {
      std::lock_guard nat_lock(nat_tree_lock_);
      cache_entry = LookupNatCache(nid);
      if (cache_entry) {
        SetNatCacheDirty(*cache_entry);
      } else {
        cache_entry = GrabNatEntry(nid);
        if (!cache_entry) {
          continue;
        }
        cache_entry->SetBlockAddress(LeToCpu(raw_entry.block_addr));
        cache_entry->SetIno(LeToCpu(raw_entry.ino));
        cache_entry->SetVersion(raw_entry.version);
        SetNatCacheDirty(*cache_entry);
      }
    }
  }
  UpdateNatsInCursum(sum, -i);
  return true;
}

// This function is called during the checkpointing process.
void NodeManager::FlushNatEntries() {
  CursegInfo *curseg = fs_->GetSegmentManager().CURSEG_I(CursegType::kCursegHotData);
  SummaryBlock *sum = curseg->sum_blk;
  fbl::RefPtr<Page> page;
  NatBlock *nat_blk = nullptr;
  nid_t start_nid = 0, end_nid = 0;
  bool flushed;

  flushed = FlushNatsInJournal();

#if 0  // porting needed
  //	if (!flushed)
#endif
  std::lock_guard curseg_lock(curseg->curseg_mutex);

  // 1) flush dirty nat caches
  {
    std::lock_guard nat_lock(nat_tree_lock_);
    for (auto iter = dirty_nat_list_.begin(); iter != dirty_nat_list_.end();) {
      nid_t nid;
      RawNatEntry raw_ne;
      int offset = -1;
      __UNUSED block_t old_blkaddr, new_blkaddr;

      // During each iteration, |iter| can be removed from |dirty_nat_list_|.
      // Therefore, make a copy of |iter| and move to the next element before futher operations.
      NatEntry *cache_entry = iter.CopyPointer();
      ++iter;

      nid = cache_entry->GetNid();

      if (cache_entry->GetBlockAddress() == kNewAddr)
        continue;

      if (!flushed) {
        // if there is room for nat enries in curseg->sumpage
        offset = LookupJournalInCursum(sum, JournalType::kNatJournal, nid, 1);
      }

      if (offset >= 0) {  // flush to journal
        raw_ne = NatInJournal(sum, offset);
        old_blkaddr = LeToCpu(raw_ne.block_addr);
      } else {  // flush to NAT block
        if (!page || (start_nid > nid || nid > end_nid)) {
          if (page) {
            page->SetDirty();
            Page::PutPage(std::move(page), true);
          }
          start_nid = StartNid(nid);
          end_nid = start_nid + kNatEntryPerBlock - 1;

          // get nat block with dirty flag, increased reference
          // count, mapped and lock
          GetNextNatPage(start_nid, &page);
          nat_blk = page->GetAddress<NatBlock>();
        }

        ZX_ASSERT(nat_blk);
        raw_ne = nat_blk->entries[nid - start_nid];
        old_blkaddr = LeToCpu(raw_ne.block_addr);
      }

      new_blkaddr = cache_entry->GetBlockAddress();

      raw_ne.ino = CpuToLe(cache_entry->GetIno());
      raw_ne.block_addr = CpuToLe(new_blkaddr);
      raw_ne.version = cache_entry->GetVersion();

      if (offset < 0) {
        nat_blk->entries[nid - start_nid] = raw_ne;
      } else {
        SetNatInJournal(sum, offset, raw_ne);
        SetNidInJournal(sum, offset, CpuToLe(nid));
      }

      if (cache_entry->GetBlockAddress() == kNullAddr) {
        DelFromNatCache(*cache_entry);
        // We can reuse this freed nid at this point
        AddFreeNid(nid);
      } else {
        ClearNatCacheDirty(*cache_entry);
        cache_entry->SetCheckpointed();
      }
    }
  }

  // Write out last modified NAT block
  if (page != nullptr) {
    page->SetDirty();
    Page::PutPage(std::move(page), true);
  }

  // 2) shrink nat caches if necessary
  TryToFreeNats(nat_entries_count_ - kNmWoutThreshold);
}

zx_status_t NodeManager::InitNodeManager() {
  const Superblock &sb_raw = GetSuperblockInfo().GetRawSuperblock();
  uint8_t *version_bitmap;
  uint32_t nat_segs, nat_blocks;

  nat_blkaddr_ = LeToCpu(sb_raw.nat_blkaddr);
  // segment_count_nat includes pair segment so divide to 2
  nat_segs = LeToCpu(sb_raw.segment_count_nat) >> 1;
  nat_blocks = nat_segs << LeToCpu(sb_raw.log_blocks_per_seg);
  max_nid_ = kNatEntryPerBlock * nat_blocks;
  free_nid_count_ = 0;
  nat_entries_count_ = 0;

  list_initialize(&free_nid_list_);

  nat_bitmap_size_ = GetSuperblockInfo().BitmapSize(MetaBitmap::kNatBitmap);
  init_scan_nid_ = LeToCpu(GetSuperblockInfo().GetCheckpoint().next_free_nid);
  next_scan_nid_ = LeToCpu(GetSuperblockInfo().GetCheckpoint().next_free_nid);

  nat_bitmap_ = std::make_unique<uint8_t[]>(nat_bitmap_size_);
  memset(nat_bitmap_.get(), 0, nat_bitmap_size_);
  nat_prev_bitmap_ = std::make_unique<uint8_t[]>(nat_bitmap_size_);
  memset(nat_prev_bitmap_.get(), 0, nat_bitmap_size_);

  version_bitmap = static_cast<uint8_t *>(GetSuperblockInfo().BitmapPtr(MetaBitmap::kNatBitmap));
  if (!version_bitmap)
    return ZX_ERR_INVALID_ARGS;

  // copy version bitmap
  memcpy(nat_bitmap_.get(), version_bitmap, nat_bitmap_size_);
  memcpy(nat_prev_bitmap_.get(), nat_bitmap_.get(), nat_bitmap_size_);
  return ZX_OK;
}

zx_status_t NodeManager::BuildNodeManager() {
  if (zx_status_t err = InitNodeManager(); err != ZX_OK) {
    return err;
  }

  BuildFreeNids();
  return ZX_OK;
}

void NodeManager::DestroyNodeManager() {
  FreeNid *i, *next_i;
  NatEntry *natvec[kNatvecSize];
  uint32_t found;

  {
    // destroy free nid list
    std::lock_guard free_nid_lock(free_nid_list_lock_);
    list_for_every_entry_safe (&free_nid_list_, i, next_i, FreeNid, list) {
      ZX_ASSERT(i->state != static_cast<int>(NidState::kNidAlloc));
      DelFromFreeNidList(i);
      --free_nid_count_;
    }
  }
  ZX_ASSERT(!free_nid_count_);

  {
    // destroy nat cache
    std::lock_guard nat_lock(nat_tree_lock_);
    while ((found = GangLookupNatCache(kNatvecSize, natvec))) {
      for (uint32_t idx = 0; idx < found; ++idx) {
        NatEntry *e = natvec[idx];
        DelFromNatCache(*e);
      }
    }
    ZX_ASSERT(!nat_entries_count_);
    ZX_ASSERT(clean_nat_list_.is_empty());
    ZX_ASSERT(dirty_nat_list_.is_empty());
    ZX_ASSERT(nat_cache_.is_empty());
  }

  nat_bitmap_.reset();
  nat_prev_bitmap_.reset();
}

SuperblockInfo &NodeManager::GetSuperblockInfo() { return *superblock_info_; }

}  // namespace f2fs
