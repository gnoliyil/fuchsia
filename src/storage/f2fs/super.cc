// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <safemath/checked_math.h>

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

void F2fs::PutSuper() {
#if 0  // porting needed
  // DestroyStats(superblock_info_.get());
  // StopGcThread(superblock_info_.get());
#endif

  WriteCheckpoint(false, true);
  if (superblock_info_->TestCpFlags(CpFlag::kCpErrorFlag)) {
    // In the checkpoint error case, flush the dirty vnode list.
    GetVCache().ForDirtyVnodesIf([&](fbl::RefPtr<VnodeF2fs> &vnode) {
      ZX_ASSERT(vnode->ClearDirty());
      return ZX_OK;
    });
  }
  SetTearDown();
  writer_.reset();
  reader_.reset();
  ResetPsuedoVnodes();
  GetVCache().Reset();
  GetDirEntryCache().Reset();

  node_manager_->DestroyNodeManager();
  segment_manager_->DestroySegmentManager();

  node_manager_.reset();
  segment_manager_.reset();
  gc_manager_.reset();
  raw_sb_.reset();
  superblock_info_.reset();
}

void F2fs::ScheduleWriteback(size_t num_pages) {
  // Schedule a Writer task for kernel to reclaim memory pages until the current memory pressure
  // becomes normal. If memory pressure events are not available, the task runs until the number of
  // dirty Pages is less than kMaxDirtyDataPages / 4. |writeback_flag_| ensures that neither
  // checkpoint nor gc runs during this writeback. If there is not enough space, stop writeback as
  // flushing N of dirty Pages can produce N of additional dirty node Pages in the worst case.
  if (HasNotEnoughMemory() && writeback_flag_.try_acquire()) {
    auto promise = fpromise::make_promise([this]() mutable {
      while (!segment_manager_->HasNotEnoughFreeSecs() && CanReclaim() && !StopWriteback()) {
        GetVCache().ForDirtyVnodesIf(
            [&](fbl::RefPtr<VnodeF2fs> &vnode) {
              WritebackOperation op = {.bReclaim = true};
              vnode->Writeback(op);
              return ZX_OK;
            },
            [](fbl::RefPtr<VnodeF2fs> &vnode) {
              if (!vnode->IsDir() && vnode->GetDirtyPageList().Size()) {
                return ZX_OK;
              }
              return ZX_ERR_NEXT;
            });
      }
      // Wake waiters of WaitForWriteback().
      writeback_flag_.release();
      return fpromise::ok();
    });
    writer_->ScheduleWriteback(std::move(promise));
  }
}

void F2fs::SyncFs(bool bShutdown) {
  // TODO:: Consider !superblock_info_.IsDirty()
  if (bShutdown) {
    FX_LOGS(INFO) << "prepare for shutdown";
    // Stop writeback before umount.
    FlagAcquireGuard flag(&stop_reclaim_flag_);
    ZX_DEBUG_ASSERT(flag.IsAcquired());
    ZX_ASSERT(WaitForWriteback().is_ok());
    // Stop listening to memorypressure.
    memory_pressure_watcher_.reset();

    // Flush every dirty Pages.
    while (superblock_info_->GetPageCount(CountType::kDirtyData)) {
      // If necessary, do gc.
      if (segment_manager_->HasNotEnoughFreeSecs()) {
        if (auto ret = gc_manager_->F2fsGc(); ret.is_error()) {
          // If CpFlag::kCpErrorFlag is set, it cannot be synchronized to disk. So we will drop all
          // dirty pages.
          if (superblock_info_->TestCpFlags(CpFlag::kCpErrorFlag)) {
            break;
          }
          // F2fsGc() returns ZX_ERR_UNAVAILABLE when there is no available victim section,
          // otherwise BUG
          ZX_DEBUG_ASSERT(ret.error_value() == ZX_ERR_UNAVAILABLE);
        }
      }
      WritebackOperation op = {.to_write = kDefaultBlocksPerSegment};
      op.if_vnode = [](fbl::RefPtr<VnodeF2fs> &vnode) {
        if (!vnode->IsDir()) {
          return ZX_OK;
        }
        return ZX_ERR_NEXT;
      };
      FlushDirtyDataPages(op);
    }
  } else {
    WriteCheckpoint(false, false);
  }
}

#if 0  // porting needed
// int F2fs::F2fsStatfs(dentry *dentry /*, kstatfs *buf*/) {
  // super_block *sb = dentry->d_sb;
  // SuperblockInfo *superblock_info = F2FS_SB(sb);
  // u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
  // block_t total_count, user_block_count, start_count, ovp_count;

  // total_count = LeToCpu(superblock_info->raw_super->block_count);
  // user_block_count = superblock_info->GetUserBlockCount();
  // start_count = LeToCpu(superblock_info->raw_super->segment0_blkaddr);
  // ovp_count = GetSmInfo(superblock_info).ovp_segments << superblock_info->GetLogBlocksPerSeg();
  // buf->f_type = kF2fsSuperMagic;
  // buf->f_bsize = superblock_info->GetBlocksize();

  // buf->f_blocks = total_count - start_count;
  // buf->f_bfree = buf->f_blocks - ValidUserBlocks(superblock_info) - ovp_count;
  // buf->f_bavail = user_block_count - ValidUserBlocks(superblock_info);

  // buf->f_files = ValidInodeCount(superblock_info);
  // buf->f_ffree = superblock_info->GetTotalNodeCount() - ValidNodeCount(superblock_info);

  // buf->f_namelen = kMaxNameLen;
  // buf->f_fsid.val[0] = (u32)id;
  // buf->f_fsid.val[1] = (u32)(id >> 32);

  // return 0;
// }

// VnodeF2fs *F2fs::F2fsNfsGetInode(uint64_t ino, uint32_t generation) {
//   fbl::RefPtr<VnodeF2fs> vnode_refptr;
//   VnodeF2fs *vnode = nullptr;
//   int err;

//   if (ino < superblock_info_->GetRootIno())
//     return (VnodeF2fs *)ErrPtr(-ESTALE);

//   /*
//    * f2fs_iget isn't quite right if the inode is currently unallocated!
//    * However f2fs_iget currently does appropriate checks to handle stale
//    * inodes so everything is OK.
//    */
//   err = VnodeF2fs::Vget(this, ino, &vnode_refptr);
//   if (err)
//     return (VnodeF2fs *)ErrPtr(err);
//   vnode = vnode_refptr.get();
//   if (generation && vnode->i_generation != generation) {
//     /* we didn't find the right inode.. */
//     return (VnodeF2fs *)ErrPtr(-ESTALE);
//   }
//   return vnode;
// }

// struct fid {};

// dentry *F2fs::F2fsFhToDentry(fid *fid, int fh_len, int fh_type) {
//   return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
//             f2fs_nfs_get_inode);
// }

// dentry *F2fs::F2fsFhToParent(fid *fid, int fh_len, int fh_type) {
//   return generic_fh_to_parent(sb, fid, fh_len, fh_type,
//             f2fs_nfs_get_inode);
// }
#endif

void F2fs::ParseOptions() {
  for (const auto &i : MountOptions::Iter()) {
    auto value_or = mount_options_.GetValue(i);
    ZX_DEBUG_ASSERT(value_or.is_ok());
    switch (i) {
      case MountOption::kActiveLogs:
        superblock_info_->SetActiveLogs(*value_or);
        break;
      case MountOption::kDiscard:
      case MountOption::kBgGcOff:
      case MountOption::kNoHeap:
      case MountOption::kDisableExtIdentify:
      case MountOption::kNoUserXAttr:
      case MountOption::kNoAcl:
      case MountOption::kDisableRollForward:
      case MountOption::kInlineXattr:
      case MountOption::kInlineData:
      case MountOption::kInlineDentry:
      case MountOption::kForceLfs:
      case MountOption::kReadOnly:
        if (*value_or)
          superblock_info_->SetOpt(i);
        break;
      default:
        break;
    };
  }
}

zx_status_t F2fs::SanityCheckRawSuper() {
  if (kF2fsSuperMagic != LeToCpu(raw_sb_->magic)) {
    return ZX_ERR_INVALID_ARGS;
  }

  block_t blocksize =
      safemath::CheckLsh<block_t>(1, LeToCpu(raw_sb_->log_blocksize)).ValueOrDefault(kUint32Max);
  if (blocksize != kPageSize)
    return ZX_ERR_INVALID_ARGS;
  // 512/1024/2048/4096 sector sizes are supported.
  if (LeToCpu(raw_sb_->log_sectorsize) > kMaxLogSectorSize ||
      LeToCpu(raw_sb_->log_sectorsize) < kMinLogSectorSize)
    return ZX_ERR_INVALID_ARGS;
  if ((LeToCpu(raw_sb_->log_sectors_per_block) + LeToCpu(raw_sb_->log_sectorsize)) !=
      kMaxLogSectorSize)
    return ZX_ERR_INVALID_ARGS;
  return ZX_OK;
}

zx_status_t F2fs::SanityCheckCkpt() {
  unsigned int total, fsmeta;

  total = LeToCpu(raw_sb_->segment_count);
  fsmeta = LeToCpu(raw_sb_->segment_count_ckpt);
  fsmeta += LeToCpu(raw_sb_->segment_count_sit);
  fsmeta += LeToCpu(raw_sb_->segment_count_nat);
  fsmeta += LeToCpu(superblock_info_->GetCheckpoint().rsvd_segment_count);
  fsmeta += LeToCpu(raw_sb_->segment_count_ssa);

  if (fsmeta >= total) {
    return ZX_ERR_BAD_STATE;
  }

  uint32_t sit_ver_bitmap_bytesize =
      ((LeToCpu(raw_sb_->segment_count_sit) / 2) << LeToCpu(raw_sb_->log_blocks_per_seg)) / 8;
  uint32_t nat_ver_bitmap_bytesize =
      ((LeToCpu(raw_sb_->segment_count_nat) / 2) << LeToCpu(raw_sb_->log_blocks_per_seg)) / 8;
  block_t nat_blocks = (LeToCpu(raw_sb_->segment_count_nat) >> 1)
                       << LeToCpu(raw_sb_->log_blocks_per_seg);

  if (superblock_info_->GetCheckpoint().sit_ver_bitmap_bytesize != sit_ver_bitmap_bytesize ||
      superblock_info_->GetCheckpoint().nat_ver_bitmap_bytesize != nat_ver_bitmap_bytesize ||
      superblock_info_->GetCheckpoint().next_free_nid >= kNatEntryPerBlock * nat_blocks) {
    return ZX_ERR_BAD_STATE;
  }

  return ZX_OK;
}

void F2fs::InitSuperblockInfo() {
  superblock_info_->SetLogSectorsPerBlock(LeToCpu(RawSb().log_sectors_per_block));
  superblock_info_->SetLogBlocksize(LeToCpu(RawSb().log_blocksize));
  superblock_info_->SetBlocksize(1 << superblock_info_->GetLogBlocksize());
  superblock_info_->SetLogBlocksPerSeg(LeToCpu(RawSb().log_blocks_per_seg));
  superblock_info_->SetBlocksPerSeg(1 << superblock_info_->GetLogBlocksPerSeg());
  superblock_info_->SetSegsPerSec(LeToCpu(RawSb().segs_per_sec));
  superblock_info_->SetSecsPerZone(LeToCpu(RawSb().secs_per_zone));
  superblock_info_->SetTotalSections(LeToCpu(RawSb().section_count));
  superblock_info_->SetTotalNodeCount((LeToCpu(RawSb().segment_count_nat) / 2) *
                                      superblock_info_->GetBlocksPerSeg() * kNatEntryPerBlock);
  superblock_info_->SetRootIno(LeToCpu(RawSb().root_ino));
  superblock_info_->SetNodeIno(LeToCpu(RawSb().node_ino));
  superblock_info_->SetMetaIno(LeToCpu(RawSb().meta_ino));

  std::vector<std::string> extension_list;
  for (int index = 0; index < safemath::checked_cast<int>(RawSb().extension_count); ++index) {
    ZX_ASSERT(index < kMaxExtension);
    ZX_ASSERT(RawSb().extension_list[index][7] == '\0');
    extension_list.push_back(reinterpret_cast<char *>(RawSb().extension_list[index]));
  }
  ZX_ASSERT(RawSb().extension_count == extension_list.size());
  superblock_info_->SetExtensionList(std::move(extension_list));
}

void F2fs::Reset() {
  ResetPsuedoVnodes();
  if (node_manager_) {
    node_manager_->DestroyNodeManager();
    node_manager_.reset();
  }
  if (segment_manager_) {
    segment_manager_->DestroySegmentManager();
    segment_manager_.reset();
  }
  gc_manager_.reset();
  superblock_info_.reset();
}

zx_status_t F2fs::FillSuper() {
  zx_status_t err = ZX_OK;
  auto reset = fit::defer([&] { Reset(); });

  // allocate memory for f2fs-specific super block info
  superblock_info_ = std::make_unique<SuperblockInfo>();

  ParseOptions();

  // sanity checking of raw super
  if (err = SanityCheckRawSuper(); err != ZX_OK) {
    return err;
  }

  superblock_info_->SetRawSuperblock(raw_sb_.get());
  superblock_info_->ClearOnRecovery();
  InitSuperblockInfo();

  node_vnode_ = std::make_unique<VnodeF2fs>(this, GetSuperblockInfo().GetNodeIno(), 0);
  meta_vnode_ = std::make_unique<VnodeF2fs>(this, GetSuperblockInfo().GetMetaIno(), 0);

  reader_ = std::make_unique<Reader>(bc_.get(), kDefaultBlocksPerSegment);
  writer_ = std::make_unique<Writer>(
      bc_.get(),
      safemath::CheckMul<size_t>(superblock_info_->GetActiveLogs(), kDefaultBlocksPerSegment)
          .ValueOrDie());

  if (err = GetValidCheckpoint(); err != ZX_OK) {
    return err;
  }

  // sanity checking of checkpoint
  if (err = SanityCheckCkpt(); err != ZX_OK) {
    return err;
  }

  superblock_info_->SetTotalValidNodeCount(
      LeToCpu(superblock_info_->GetCheckpoint().valid_node_count));
  superblock_info_->SetTotalValidInodeCount(
      LeToCpu(superblock_info_->GetCheckpoint().valid_inode_count));
  superblock_info_->SetUserBlockCount(
      static_cast<block_t>(LeToCpu(superblock_info_->GetCheckpoint().user_block_count)));
  superblock_info_->SetTotalValidBlockCount(
      static_cast<block_t>(LeToCpu(superblock_info_->GetCheckpoint().valid_block_count)));
  superblock_info_->SetLastValidBlockCount(superblock_info_->GetTotalValidBlockCount());
  superblock_info_->SetAllocValidBlockCount(0);

  segment_manager_ = std::make_unique<SegmentManager>(this);
  node_manager_ = std::make_unique<NodeManager>(this);
  gc_manager_ = std::make_unique<GcManager>(this);
  if (err = segment_manager_->BuildSegmentManager(); err != ZX_OK) {
    return err;
  }

  if (err = node_manager_->BuildNodeManager(); err != ZX_OK) {
    return err;
  }

  // if there are nt orphan nodes free them
  if (err = PurgeOrphanInodes(); err != ZX_OK) {
    return err;
  }

  // read root inode and dentry
  if (err = VnodeF2fs::Vget(this, superblock_info_->GetRootIno(), &root_vnode_); err != ZX_OK) {
    err = ZX_ERR_NO_MEMORY;
    return err;
  }

  // root vnode is corrupted
  if (!root_vnode_->IsDir() || !root_vnode_->GetBlocks() || !root_vnode_->GetSize()) {
    return err;
  }

  if (!superblock_info_->TestOpt(MountOption::kDisableRollForward)) {
    RecoverFsyncData();
  }

  // TODO(https://fxbug.dev/119887): enable a thread for background gc
  // After POR, we can run background GC thread
  // err = StartGcThread(superblock_info);
  // if (err)
  //   goto fail;
  reset.cancel();
  return err;
}
}  // namespace f2fs
