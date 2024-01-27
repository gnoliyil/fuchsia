// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_MINFS_VNODE_H_
#define SRC_STORAGE_MINFS_VNODE_H_

#include <inttypes.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#ifdef __Fuchsia__
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fzl/resizeable-vmo-mapper.h>
#include <lib/zx/vmo.h>

#include <fbl/auto_lock.h>

#include "src/lib/storage/vfs/cpp/watcher.h"
#include "src/storage/minfs/vnode_allocation.h"
#endif

#include <lib/zircon-internal/fnv1hash.h>

#include <fbl/algorithm.h>
#include <fbl/macros.h>
#include <fbl/ref_ptr.h>

#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/lib/storage/vfs/cpp/vnode.h"
#include "src/storage/minfs/format.h"
#include "src/storage/minfs/lazy_buffer.h"
#include "src/storage/minfs/minfs.h"
#include "src/storage/minfs/transaction_limits.h"
#include "src/storage/minfs/vnode_mapper.h"
#include "src/storage/minfs/writeback.h"

namespace minfs {

// Used by fsck
class Minfs;

// An abstract Vnode class contains the following:
//
// - A VMO, holding the in-memory representation of data stored persistently.
// - An inode, holding the root of this node's metadata.
//
// This class is capable of writing, reading, and truncating the node's data
// in a linear block-address space.
class VnodeMinfs : public fs::Vnode,
                   public fbl::SinglyLinkedListable<VnodeMinfs*>,
                   public fbl::Recyclable<VnodeMinfs> {
 public:
  explicit VnodeMinfs(Minfs* fs);
  ~VnodeMinfs() override;

  // Allocates a new Vnode and initializes the in-memory inode structure given the type, where
  // type is one of:
  // - kMinfsTypeFile
  // - kMinfsTypeDir
  //
  // Sets create / modify times of the new node.
  // Does not allocate an inode number for the Vnode.
  static void Allocate(Minfs* fs, uint32_t type, fbl::RefPtr<VnodeMinfs>* out);

  // Allocates a Vnode, loading |ino| from storage.
  //
  // Doesn't update create / modify times of the node.
  static void Recreate(Minfs* fs, ino_t ino, fbl::RefPtr<VnodeMinfs>* out);

  bool IsUnlinked() const { return inode_.link_count == 0; }

  const Inode* GetInode() const { return &inode_; }
  Inode* GetMutableInode() { return &inode_; }
  ino_t GetIno() const { return ino_; }

  ino_t GetKey() const { return ino_; }
  // Should only be called once for the VnodeMinfs lifecycle.
  void SetIno(ino_t ino);

  void SetNextInode(ino_t ino) { inode_.next_inode = ino; }
  void SetLastInode(ino_t ino) { inode_.last_inode = ino; }

  void AddLink();

  void MarkPurged() { inode_.magic = kMinfsMagicPurged; }

  static size_t GetHash(ino_t key) { return fnv1a_tiny(key, kMinfsHashBits); }

  // fs::Vnode interface (invoked publicly).

  // Required for memory management, see the class comment above Vnode for more.
  void fbl_recycle() { RecycleNode(); }

  // Queries the underlying vnode to ask if it may be unlinked.
  //
  // If the response is not ZX_OK, operations to unlink (or rename on top of) this
  // vnode will fail.
  virtual zx::result<> CanUnlink() const = 0;

  // Removes from disk an unlinked and closed vnode. Asserts that inode IsUnlinked().
  zx::result<> RemoveUnlinked();

  // Issues a write on all dirty bytes within a vnode.
  virtual zx::result<> FlushCachedWrites() = 0;

  // Discards all the dirty bytes within a vnode.
  // This also drops any inode or block reservation a vnode might have.
  virtual void DropCachedWrites() = 0;

  // Returns the current block count of the vnode.
  virtual blk_t GetBlockCount() const = 0;

  // Returns the total size of the vnode.
  virtual uint64_t GetSize() const = 0;

  // Returns if the node is a directory.
  virtual bool IsDirectory() const = 0;

  // Sets the new size of the vnode.
  // Should update the in-memory representation of the Vnode, but not necessarily
  // write it out to persistent storage.
  //
  // TODO(unknown): Upgrade internal size to 64-bit integer.
  virtual void SetSize(uint32_t new_size) = 0;

  // Accesses a block in the vnode at |vmo_offset| relative to the start of the file,
  // which was previously at the device offset |dev_offset|.
  //
  // If the block was not previously allocated, |dev_offset| is zero.
  // |*out_dev_offset| must contain the new value of the device offset to use when writing
  // to this part of the Vnode. By default, it is set to |dev_offset|.
  //
  // |*out_dev_offset| may be passed to |IssueWriteback| as |dev_offset|.
  virtual void AcquireWritableBlock(Transaction* transaction, blk_t vmo_offset, blk_t dev_offset,
                                    blk_t* out_dev_offset) = 0;

  // Deletes the block at |vmo_offset| within the file, corresponding to on-disk block
  // |dev_offset| (zero if unallocated). |indirect| specifies whether the block is a direct or
  // indirect block.
  virtual void DeleteBlock(PendingWork* transaction, blk_t vmo_offset, blk_t dev_offset,
                           bool indirect) = 0;

#ifdef __Fuchsia__
  // Instructs the Vnode to write out |count| blocks of the vnode, starting at local
  // offset |vmo_offset|, corresponding to on-disk offset |dev_offset|.
  virtual void IssueWriteback(Transaction* transaction, blk_t vmo_offset, blk_t dev_offset,
                              blk_t count) = 0;

  // Queries the node, returning |true| if the node has an in-flight operation on |vmo_offset|
  // that has not yet been enqueued to the writeback pipeline.
  virtual bool HasPendingAllocation(blk_t vmo_offset) = 0;

  // Instructs the node to cancel all pending writeback operations that have not yet been
  // enqueued to the writeback pipeline.
  //
  // This method is used exclusively when deleting nodes.
  virtual void CancelPendingWriteback() = 0;

  // Returns a copy of unowned vmo.
  zx::unowned_vmo vmo() const { return zx::unowned_vmo(vmo_.get()); }
#endif

  // Returns true if dirty pages can be cached.
  virtual bool DirtyCacheEnabled() const = 0;

  // Returns true if the vnode needs to be flushed.
  virtual bool IsDirty() const = 0;

  Minfs* Vfs() const { return fs_; }

  // Local implementations of read, write, and truncate functions which
  // may operate on either files or directories.
  zx::result<> ReadInternal(PendingWork* transaction, void* data, size_t len, size_t off,
                            size_t* actual);
  zx::result<> ReadExactInternal(PendingWork* transaction, void* data, size_t len, size_t off);
  zx::result<> WriteInternal(Transaction* transaction, const uint8_t* data, size_t len, size_t off,
                             size_t* actual);
  zx::result<> WriteExactInternal(Transaction* transaction, const void* data, size_t len,
                                  size_t off);
  zx::result<> TruncateInternal(Transaction* transaction, size_t len);

  // Update the vnode's inode and write it to disk.
  void InodeSync(PendingWork* transaction, uint32_t flags);

  // Decrements the inode link count to a vnode.
  // Writes the inode back to |transaction|.
  //
  // If the link count becomes zero, the node either:
  // 1) Calls |Purge()| (if no open fds exist), or
  // 2) Adds itself to the "unlinked list", to be purged later.
  [[nodiscard]] zx::result<> RemoveInodeLink(Transaction* transaction);

  // Allocates an indirect block.
  void AllocateIndirect(PendingWork* transaction, blk_t* block);

  // Initializes (if necessary) and returns the indirect file.
  [[nodiscard]] zx::result<LazyBuffer*> GetIndirectFile();

  // Deletes all blocks (relative to a file) from "start" (inclusive) to the end
  // of the file. Does not update mtime/atime.
  // This can be extended to return indices of deleted bnos, or to delete a specific number of
  // bnos
  zx::result<> BlocksShrink(PendingWork* transaction, blk_t start);

 protected:
  // fs::Vnode protected interface.
  void RecycleNode() final;

 private:
  // fs::Vnode private interface.
  zx_status_t CloseNode() final;
  zx_status_t GetAttributes(fs::VnodeAttributes* a) final;
  zx_status_t SetAttributes(fs::VnodeAttributesUpdate a) final;
#ifdef __Fuchsia__
  zx::result<std::string> GetDevicePath() const final;
#endif

  // Get the disk block 'bno' corresponding to the 'n' block
  //
  // May or may not allocate |bno|; certain Vnodes (like File) delay allocation
  // until writeback, and will return a sentinel value of zero.
  zx::result<blk_t> BlockGetWritable(Transaction* transaction, blk_t n);

  // Get the disk block 'bno' corresponding to relative block address |n| within the file.
  // Does not allocate any blocks, direct or indirect, to acquire this block.
  zx::result<blk_t> BlockGetReadable(blk_t n);

  // Deletes this Vnode from disk, freeing the inode and blocks.
  //
  // Must only be called on Vnodes which
  // - Have no open fds
  // - Are fully unlinked (link count == 0)
  [[nodiscard]] zx::result<> Purge(Transaction* transaction);

#ifdef __Fuchsia__
  zx_status_t GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights rights,
                                     fs::VnodeRepresentation* info) final;

  void Sync(SyncCallback closure) final;

  // Initializes vmo that contains file's data by reading data from the disk.
  // Since we cannot yet register the filesystem as a paging service (and
  // cleanly fault on pages when they are actually needed), we currently read an
  // entire file to a VMO when a file's data block are accessed.
  zx::result<> InitVmo();

  // Use the watcher container to implement a directory watcher
  void Notify(std::string_view name, fuchsia_io::wire::WatchEvent event) final;
  zx_status_t WatchDir(fs::FuchsiaVfs* vfs, fuchsia_io::wire::WatchMask mask, uint32_t options,
                       fidl::ServerEnd<fuchsia_io::DirectoryWatcher> watcher) final;
#endif

  Minfs* const fs_;

#ifdef __Fuchsia__
  zx::vmo vmo_{};
  uint64_t vmo_size_ = 0;
  storage::Vmoid vmoid_;
  fs::WatcherContainer watcher_{};
#endif

  // vnode_mapper.cc explains what this is and the code there is responsible for manipulating it.
  // It is created on-demand.
  std::unique_ptr<LazyBuffer> indirect_file_;

  ino_t ino_{};

  // DataBlockAssigner may modify this field asynchronously, so a valid Transaction object must
  // be held before accessing it.
  Inode inode_{};
};

}  // namespace minfs

#endif  // SRC_STORAGE_MINFS_VNODE_H_
