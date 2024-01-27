// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

VnodeCache::VnodeCache() = default;

VnodeCache::~VnodeCache() {
  Reset();
  {
    std::lock_guard list_lock(list_lock_);
    std::lock_guard table_lock(table_lock_);
    ZX_ASSERT(dirty_list_.is_empty());
    ZX_ASSERT(vnode_table_.is_empty());
    ZX_ASSERT(ndirty_ == 0);
    ZX_ASSERT(ndirty_dir_ == 0);
  }
}

void VnodeCache::Reset() {
  {
    std::lock_guard list_lock(list_lock_);
    ZX_ASSERT(dirty_list_.is_empty());
  }

  ForAllVnodes([this](fbl::RefPtr<VnodeF2fs>& vnode) { return Evict(vnode.get()); });
}

zx_status_t VnodeCache::ForAllVnodes(VnodeCallback callback) {
  fbl::RefPtr<VnodeF2fs> prev_vnode;

  while (true) {
    fbl::RefPtr<VnodeF2fs> vnode;
    // Scope the lock to prevent letting fbl::RefPtr<VnodeF2fs> destructors from running while
    // it is held.
    {
      std::lock_guard lock(table_lock_);
      if (vnode_table_.is_empty()) {
        return ZX_OK;
      }

      VnodeF2fs* raw_vnode = nullptr;
      if (prev_vnode == nullptr) {
        // Acquire the first node from the front of the cache...
        raw_vnode = &vnode_table_.front();
      } else {
        // ... Acquire all subsequent nodes by iterating from the lower bound of the current node.
        auto current = vnode_table_.lower_bound(prev_vnode->GetKey());
        if (current == vnode_table_.end()) {
          return ZX_OK;
        } else if (current.CopyPointer() != prev_vnode.get()) {
          raw_vnode = current.CopyPointer();
        } else {
          auto next = ++current;
          if (next == vnode_table_.end()) {
            return ZX_OK;
          }
          raw_vnode = next.CopyPointer();
        }
      }

      if (raw_vnode->IsActive()) {
        vnode = fbl::MakeRefPtrUpgradeFromRaw(raw_vnode, table_lock_);
        if (vnode == nullptr) {
          // When it is being recycled, we should wait for deactivation or eviction.
          raw_vnode->WaitForDeactive(table_lock_);
          continue;
        }
      } else {
        // When it is inactive, it is safe to make Refptr.
        vnode = fbl::ImportFromRawPtr(raw_vnode);
        vnode->Activate();
      }
    }
    zx_status_t status = callback(vnode);
    prev_vnode = std::move(vnode);
    if (status == ZX_ERR_STOP) {
      break;
    }
    if (status != ZX_ERR_NEXT && status != ZX_OK) {
      return status;
    }
  }
  return ZX_OK;
}

zx_status_t VnodeCache::ForDirtyVnodesIf(VnodeCallback cb, VnodeCallback cb_if) {
  std::vector<fbl::RefPtr<VnodeF2fs>> dirty_vnodes;
  {
    std::lock_guard lock(list_lock_);
    for (auto iter = dirty_list_.begin(); iter != dirty_list_.end(); ++iter) {
      fbl::RefPtr<VnodeF2fs> vnode = iter.CopyPointer();
      if (cb_if == nullptr || cb_if(vnode) == ZX_OK) {
        dirty_vnodes.push_back(std::move(vnode));
      }
    }
  }

  if (dirty_vnodes.empty()) {
    return ZX_OK;
  }

  for (auto& vnode : dirty_vnodes) {
    if (zx_status_t status = cb(vnode); status == ZX_ERR_STOP) {
      break;
    } else if (status != ZX_ERR_NEXT && status != ZX_OK) {
      return status;
    }
  }

  return ZX_OK;
}

void VnodeCache::Downgrade(VnodeF2fs* raw_vnode) {
  std::lock_guard lock(table_lock_);
  // We resurrect it, so it can be used without strong references in the inactive state
  raw_vnode->ResurrectRef();
  fbl::RefPtr<VnodeF2fs> vnode = fbl::ImportFromRawPtr(raw_vnode);

  // If it has been evicted already, it should be freed.
  if (!(*raw_vnode).fbl::WAVLTreeContainable<VnodeF2fs*>::InContainer()) {
    ZX_ASSERT(!(*raw_vnode).fbl::DoublyLinkedListable<fbl::RefPtr<VnodeF2fs>>::InContainer());
    delete fbl::ExportToRawPtr(&vnode);
    return;
  }

  // TODO(https://fxbug.dev/119885): Need to adjust the size of vnode_table_ according to memory
  // pressure

  // It is leaked to keep alive in vnode_table
  [[maybe_unused]] auto leak = fbl::ExportToRawPtr(&vnode);
  raw_vnode->Deactivate();
}

zx_status_t VnodeCache::Lookup(const ino_t& ino, fbl::RefPtr<VnodeF2fs>* out) {
  fbl::RefPtr<VnodeF2fs> vnode;
  {
    std::lock_guard lock(table_lock_);
    if (zx_status_t status = LookupUnsafe(ino, &vnode); status != ZX_OK) {
      return status;
    }
  }
  *out = std::move(vnode);
  return ZX_OK;
}

zx_status_t VnodeCache::LookupUnsafe(const ino_t& ino, fbl::RefPtr<VnodeF2fs>* out) {
  while (true) {
    auto raw_ptr = vnode_table_.find(ino).CopyPointer();
    if (raw_ptr != nullptr) {
      // When the vnode is active, we should check if it is being recycled.
      if (raw_ptr->IsActive()) {
        *out = fbl::MakeRefPtrUpgradeFromRaw(raw_ptr, table_lock_);
        if (*out == nullptr) {
          // When it is being recycled, we should wait for it to be deactivate.
          raw_ptr->WaitForDeactive(table_lock_);
          continue;
        }
        return ZX_OK;
      }
      // When it is inactive, it is safe to make Refptr.
      *out = fbl::ImportFromRawPtr(raw_ptr);
      (*out)->Activate();
      return ZX_OK;
    }
    break;
  }
  return ZX_ERR_NOT_FOUND;
}

zx_status_t VnodeCache::Evict(VnodeF2fs* vnode) {
  ZX_ASSERT(!(*vnode).fbl::DoublyLinkedListable<fbl::RefPtr<VnodeF2fs>>::InContainer());
  std::lock_guard lock(table_lock_);
  return EvictUnsafe(vnode);
}

zx_status_t VnodeCache::EvictUnsafe(VnodeF2fs* vnode) {
  if (!(*vnode).fbl::WAVLTreeContainable<VnodeF2fs*>::InContainer()) {
    FX_LOGS(INFO) << "EvictUnsafe: " << vnode->GetNameView() << "(" << vnode->GetKey()
                  << ") cannot be found in vnode table";
    return ZX_ERR_NOT_FOUND;
  }
  ZX_ASSERT_MSG(vnode_table_.erase(*vnode) != nullptr, "Cannot find vnode (%u)", vnode->GetKey());
  return ZX_OK;
}

zx_status_t VnodeCache::Add(VnodeF2fs* vnode) {
  {
    std::lock_guard lock(table_lock_);
    if ((*vnode).fbl::WAVLTreeContainable<VnodeF2fs*>::InContainer()) {
      return ZX_ERR_ALREADY_EXISTS;
    }
    vnode_table_.insert(vnode);
  }
  return ZX_OK;
}

zx_status_t VnodeCache::AddDirty(VnodeF2fs* vnode, bool to_back) {
  std::lock_guard lock(list_lock_);
  ZX_ASSERT(vnode != nullptr);
  fbl::RefPtr<VnodeF2fs> vnode_refptr;
  if ((*vnode).fbl::DoublyLinkedListable<fbl::RefPtr<VnodeF2fs>>::InContainer()) {
    if (!to_back) {
      return ZX_ERR_ALREADY_EXISTS;
    } else if (dirty_list_.back().GetKey() == vnode->GetKey()) {
      return ZX_OK;
    }
    auto removed = RemoveDirtyUnsafe(vnode);
    ZX_ASSERT(removed.is_ok());
    vnode_refptr = std::move(*removed);
  }
  if (!vnode_refptr) {
    vnode_refptr = fbl::MakeRefPtrUpgradeFromRaw(vnode, list_lock_);
  }
  // It should not be nullptr because the element holds its ref_count.
  ZX_DEBUG_ASSERT(vnode_refptr);
  dirty_list_.push_back(std::move(vnode_refptr));
  if (vnode->IsDir()) {
    ++ndirty_dir_;
  }
  ++ndirty_;
  return ZX_OK;
}

zx::result<fbl::RefPtr<VnodeF2fs>> VnodeCache::RemoveDirty(VnodeF2fs* vnode) {
  std::lock_guard lock(list_lock_);
  return RemoveDirtyUnsafe(vnode);
}

zx::result<fbl::RefPtr<VnodeF2fs>> VnodeCache::RemoveDirtyUnsafe(VnodeF2fs* vnode) {
  ZX_ASSERT(vnode != nullptr);
  if (!(*vnode).fbl::DoublyLinkedListable<fbl::RefPtr<VnodeF2fs>>::InContainer()) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }
  auto vnode_refptr = dirty_list_.erase(*vnode);
  if (vnode_refptr->IsDir()) {
    --ndirty_dir_;
  }
  --ndirty_;
  return zx::ok(std::move(vnode_refptr));
}

}  // namespace f2fs
