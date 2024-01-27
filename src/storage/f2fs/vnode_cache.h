// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_F2FS_VNODE_CACHE_H_
#define SRC_STORAGE_F2FS_VNODE_CACHE_H_

namespace f2fs {

class VnodeCache {
 public:
  DISALLOW_COPY_ASSIGN_AND_MOVE(VnodeCache);
  VnodeCache();
  ~VnodeCache();

  // It checks if there is vnode for |ino| in vnode_table_, and
  // it returns ZX_OK with valid |out| if it find it.
  // Otherwise, it returns ZX_ERR_NOT_FOUND.
  // When a caller tries to look up a vnode that is being recyclyed,
  // it will be blocked until it gets inactive (deactivated) and
  // valid ref_count.
  zx_status_t Lookup(const ino_t& ino, fbl::RefPtr<VnodeF2fs>* out);
  zx_status_t LookupUnsafe(const ino_t& ino, fbl::RefPtr<VnodeF2fs>* out)
      __TA_REQUIRES(table_lock_);

  // It tries to evict |vnode| from vnode_table_.
  // It returns ZX_ERR_NOT_FOUND if it cannot find |vnode| in the table.
  // A caller should ensure that |vnode| does not exist in dirty_list_.
  zx_status_t Evict(VnodeF2fs* vnode);
  zx_status_t EvictUnsafe(VnodeF2fs* vnode) __TA_REQUIRES(table_lock_);

  // It tries to add |vnode| to vnode_table_.
  // It returns ZX_ERR_ALREADY_EXISTS if it is already in the table.
  zx_status_t Add(VnodeF2fs* vnode);

  // It tries to add |vnode| to vnode_list_.
  // It returns ZX_ERR_ALREADY_EXISTS if |vnode| is already in the list.
  zx_status_t AddDirty(VnodeF2fs* vnode);
  // It tries to remove |vnode| from dirty_list_.
  zx_status_t RemoveDirty(VnodeF2fs* vnode) __TA_EXCLUDES(list_lock_);
  zx_status_t RemoveDirtyUnsafe(VnodeF2fs* vnode) __TA_REQUIRES(list_lock_);
  void Downgrade(VnodeF2fs* raw_vnode);

  // It erases every element in vnode_table_. A caller should ensure that
  // dirty_list_ is empty.
  void Reset();

  // It traverses dirty_lists and executes cb for the dirty vnodes with
  // which cb_if returns ZX_OK.
  zx_status_t ForDirtyVnodesIf(VnodeCallback cb, VnodeCallback cb_if = nullptr);

  // It traverses vnode_tables and execute cb with every vnode.
  zx_status_t ForAllVnodes(VnodeCallback callback);

  bool IsDirtyListEmpty() __TA_EXCLUDES(list_lock_) {
    bool ret = false;
    fs::SharedLock lock(list_lock_);
    ret = dirty_list_.is_empty();
    ZX_ASSERT(ret == (ndirty_ == 0));
    return ret;
  }

 private:
  // All vnode raw pointers including dirty vnodes are kept in vnode_table_, and invalid vnodes
  // (nlink_ = 0) are evicted in VnodeF2fs::Recycle() when they have no connection anymore.
  // TODO(https://fxbug.dev/119885): Eviction policy needs to consider memory pressure.
  using VnodeTableTraits = fbl::DefaultKeyedObjectTraits<ino_t, VnodeF2fs>;
  using VnodeTable = fbl::WAVLTree<ino_t, VnodeF2fs*, VnodeTableTraits>;

  // |dirty_list_| intentionally keeps the reference of dirty vnodes until all the regarding
  // dirty pages are flushed. Once a dirty vnode is inserted in dirty_list_, it never happen that
  // their ref_count reaches zero. Checkpoint traverses dirty_list_ and evicts clean vnodes. Then
  // linked vnodes continue to be kept in vnode_tables_ in the form of raw pointers wheares unlinked
  // vnodes are deleted in Vnode::Recycle() in which LockType::kFileOp is acquired.
  using DirtyVnodeList = fbl::DoublyLinkedList<fbl::RefPtr<VnodeF2fs>>;

  std::mutex table_lock_{};
  fs::SharedMutex list_lock_{};
  VnodeTable vnode_table_ __TA_GUARDED(table_lock_){};
  DirtyVnodeList dirty_list_ __TA_GUARDED(list_lock_){};
  uint64_t ndirty_dir_ __TA_GUARDED(list_lock_){0};
  uint64_t ndirty_ __TA_GUARDED(list_lock_){0};
};

}  // namespace f2fs

#endif  // SRC_STORAGE_F2FS_VNODE_CACHE_H_
