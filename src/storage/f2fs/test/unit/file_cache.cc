// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <numeric>

#include <gtest/gtest.h>
#include <safemath/checked_math.h>

#include "src/storage/f2fs/f2fs.h"
#include "unit_lib.h"

namespace f2fs {
namespace {

using FileCacheTest = F2fsFakeDevTestFixture;

TEST_F(FileCacheTest, WaitOnLock) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  {
    LockedPage page;

    vn->GrabCachePage(0, &page);
    ASSERT_EQ(page->TryLock(), true);

    fbl::RefPtr<Page> unlocked_page = page.release();
    ASSERT_EQ(unlocked_page->TryLock(), false);

    std::thread thread([&]() { unlocked_page->Unlock(); });
    // Wait for |thread| to unlock |page|.
    page = LockedPage(unlocked_page);
    thread.join();
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, WaitOnWriteback) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  {
    LockedPage page;

    vn->GrabCachePage(0, &page);
    page->SetWriteback();
    std::thread thread([&]() {
      page->ClearWriteback();
      page->Lock();
      ASSERT_EQ(page->IsWriteback(), true);
      page->ClearWriteback();
    });

    // Wait for |thread| to run.
    page->WaitOnWriteback();
    page->SetWriteback();
    ASSERT_EQ(page->IsWriteback(), true);
    page->Unlock();
    // Wait for |thread| to clear kPageWriteback.
    page->WaitOnWriteback();
    ASSERT_EQ(page->IsWriteback(), false);
    thread.join();
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, Map) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  Page *raw_ptr;
  {
    LockedPage page;

    vn->GrabCachePage(0, &page);
    // Set kPageUptodate to keep |page| in FileCache.
    page->SetUptodate();
    // Since FileCache hold the last reference to |page|, it is safe to use |raw_ptr| here.
    raw_ptr = page.get();
    // If kDirtyPage is set, FileCache keeps the mapping of |page| since writeback will use it soon.
    // Otherwise, |page| is unmapped when there is no reference except for FileCache.
  }

  // Even after LockedPage is destructed, the mapping is maintained
  // since VmoManager keeps the mapping of VmoNode as long as its vnode is active.
  ASSERT_EQ(raw_ptr->IsLocked(), false);

  {
    LockedPage page;
    vn->GrabCachePage(0, &page);
    ASSERT_EQ(page->IsLocked(), true);
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, EvictActivePages) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));
  char buf[kPageSize];

  // Make two dirty Pages.
  FileTester::AppendToFile(vn.get(), buf, kPageSize);
  FileTester::AppendToFile(vn.get(), buf, kPageSize);

  // Configure op to flush dirty Pages in a sync manner.
  WritebackOperation op = {.bSync = true, .bReleasePages = false};

  constexpr uint64_t kPageNum = 2;
  fbl::RefPtr<Page> unlock_page;
  Page *raw_pages[kPageNum];
  for (auto i = 0ULL; i < kPageNum; ++i) {
    LockedPage page;
    vn->GrabCachePage(i, &page);
    ASSERT_TRUE(page->IsDirty());
    ASSERT_FALSE(page->IsWriteback());
    raw_pages[i] = page.get();
    if (!i) {
      unlock_page = page.release();
    }
  }

  // Flush every dirty Page regardless of its active flag.
  ASSERT_EQ(vn->Writeback(op), kPageNum);

  for (auto i = 0ULL; i < kPageNum; ++i) {
    LockedPage page;
    vn->GrabCachePage(i, &page);
    ASSERT_FALSE(page->IsDirty());
  }

  // Every Page becomes inactive.
  unlock_page.reset();

  fbl::RefPtr<Page> inactive_pages[kPageNum];
  for (auto i = 0ULL; i < kPageNum; ++i) {
    ASSERT_FALSE(raw_pages[i]->IsActive());
    ASSERT_TRUE(raw_pages[i]->InTreeContainer());
    // Get the ref of Pages to avoid deletion when they are evicted from FileCache.
    inactive_pages[i] = fbl::RefPtr<Page>(raw_pages[i]);
  }

  // Evict every inactive Page.
  op.bReleasePages = true;
  op.bSync = false;
  ASSERT_EQ(vn->Writeback(op), 0ULL);

  for (auto i = 0ULL; i < kPageNum; ++i) {
    ASSERT_FALSE(raw_pages[i]->IsActive());
    // Every Pages were evicted.
    ASSERT_FALSE(raw_pages[i]->InTreeContainer());
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, WritebackOperation) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));
  char buf[kPageSize];
  pgoff_t key;
  WritebackOperation op = {.start = 0,
                           .end = 2,
                           .to_write = 2,
                           .bSync = true,
                           .bReleasePages = false,
                           .if_page = [&key](const fbl::RefPtr<Page> &page) {
                             if (page->GetKey() <= key) {
                               return ZX_OK;
                             }
                             return ZX_ERR_NEXT;
                           }};

  // |vn| should not have any dirty Pages.
  ASSERT_EQ(vn->GetDirtyPageCount(), 0U);
  FileTester::AppendToFile(vn.get(), buf, kPageSize);
  FileTester::AppendToFile(vn.get(), buf, kPageSize);
  // Flush the Page of 1st block.
  {
    LockedPage page;
    vn->GrabCachePage(0, &page);
    ASSERT_EQ(vn->GetDirtyPageCount(), 2U);
    key = page->GetKey();
    auto unlocked_page = page.release();
    // Request writeback for dirty Pages. |unlocked_page| should be written out.
    key = 0;
    ASSERT_EQ(vn->Writeback(op), 1UL);
    // Writeback() should be able to flush |unlocked_page|.
    ASSERT_EQ(vn->GetDirtyPageCount(), 1U);
    ASSERT_EQ(fs_->GetSuperblockInfo().GetPageCount(CountType::kWriteback), 0);
    ASSERT_EQ(fs_->GetSuperblockInfo().GetPageCount(CountType::kDirtyData), 1);
    ASSERT_EQ(unlocked_page->IsWriteback(), false);
    ASSERT_EQ(unlocked_page->IsDirty(), false);
  }
  // Set sync. writeback.
  op.bSync = true;
  // Request writeback for dirty Pages, but there is no Page meeting op.if_page.
  ASSERT_EQ(vn->Writeback(op), 0UL);
  // Every writeback Page has been already flushed since op.bSync is set.
  ASSERT_EQ(fs_->GetSuperblockInfo().GetPageCount(CountType::kWriteback), 0);
  ASSERT_EQ(fs_->GetSuperblockInfo().GetPageCount(CountType::kDirtyData), 1);

  key = 1;
  // Set async. writeback.
  op.bSync = false;
  // Now, 2nd Page meets op.if_page.
  ASSERT_EQ(vn->Writeback(op), 1UL);
  ASSERT_EQ(vn->GetDirtyPageCount(), 0U);
  ASSERT_EQ(fs_->GetSuperblockInfo().GetPageCount(CountType::kDirtyData), 0);
  // Set sync. writeback.
  op.bSync = true;
  // No dirty Pages to be written.
  // All writeback Pages should be clean.
  ASSERT_EQ(vn->Writeback(op), 0UL);
  ASSERT_EQ(fs_->GetSuperblockInfo().GetPageCount(CountType::kWriteback), 0);

  // It should not release any clean Pages since op.bReleasePages is false.
  ASSERT_EQ(vn->Writeback(op), 0UL);
  // Pages at 1st and 2nd blocks should be uptodate
  {
    LockedPage page;
    vn->GrabCachePage(0, &page);
    ASSERT_EQ(page->IsUptodate(), true);
  }
  {
    LockedPage page;
    vn->GrabCachePage(1, &page);
    ASSERT_EQ(page->IsUptodate(), true);
  }

  // Release clean Pages
  op.bReleasePages = true;
  // It should release and evict clean Pages from FileCache.
  ASSERT_EQ(vn->Writeback(op), 0UL);
  // There is no uptodate Page.
  {
    LockedPage page;
    vn->GrabCachePage(0, &page);
    ASSERT_EQ(page->IsUptodate(), false);
  }
  {
    LockedPage page;
    vn->GrabCachePage(1, &page);
    ASSERT_EQ(page->IsUptodate(), false);
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, Recycle) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));
  char buf[kPageSize];

  FileTester::AppendToFile(vn.get(), buf, kPageSize);

  Page *raw_page;
  {
    LockedPage locked_page;
    vn->GrabCachePage(0, &locked_page);
    ASSERT_EQ(locked_page->IsDirty(), true);
    raw_page = locked_page.get();
  }

  fbl::RefPtr<Page> page = fbl::ImportFromRawPtr(raw_page);
  // ref_count should be set to one in Page::fbl_recycle().
  ASSERT_EQ(page->IsLastReference(), true);
  // Leak it to keep alive in FileCache.
  raw_page = fbl::ExportToRawPtr(&page);
  FileCache &cache = raw_page->GetFileCache();

  raw_page->Lock();
  // Test FileCache::GetPage() and FileCache::Downgrade() with multiple threads
  std::thread thread1([&]() {
    int i = 1000;
    while (--i) {
      LockedPage page;
      vn->GrabCachePage(0, &page);
      ASSERT_EQ(page->IsDirty(), true);
    }
  });

  std::thread thread2([&]() {
    int i = 1000;
    while (--i) {
      LockedPage page;
      vn->GrabCachePage(0, &page);
      ASSERT_EQ(page->IsDirty(), true);
    }
  });
  // Start threads.
  raw_page->Unlock();
  thread1.join();
  thread2.join();

  cache.InvalidatePages();

  // Test FileCache::Downgrade() and FileCache::Reset() with multiple threads.
  std::thread thread_get_page([&]() {
    bool bStop = false;
    std::thread thread_reset([&]() {
      while (!bStop) {
        cache.Reset();
      }
    });

    int i = 1000;
    while (--i) {
      LockedPage page;
      vn->GrabCachePage(0, &page);
      ASSERT_EQ(page->IsUptodate(), false);
    }
    bStop = true;
    thread_reset.join();
  });

  thread_get_page.join();

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, GetPages) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));
  constexpr uint32_t kTestNum = 10;
  constexpr uint32_t kTestEndNum = kTestNum * 2;
  char buf[kPageSize * kTestNum];

  FileTester::AppendToFile(vn.get(), buf, static_cast<size_t>(kPageSize) * kTestNum);

  std::vector<LockedPage> locked_pages;
  std::vector<pgoff_t> pg_offsets(kTestEndNum);
  std::iota(pg_offsets.begin(), pg_offsets.end(), 0);
  {
    auto pages_or = vn->GrabCachePages(pg_offsets);
    ASSERT_TRUE(pages_or.is_ok());
    for (size_t i = 0; i < kTestNum; ++i) {
      ASSERT_EQ(pages_or.value()[i]->IsDirty(), true);
    }
    for (size_t i = kTestNum; i < kTestEndNum; ++i) {
      ASSERT_EQ(pages_or.value()[i]->IsDirty(), false);
    }
    locked_pages = std::move(pages_or.value());
  }

  auto task = [&]() {
    int i = 1000;
    while (--i) {
      auto pages_or = vn->GrabCachePages(pg_offsets);
      ASSERT_TRUE(pages_or.is_ok());
      for (size_t i = 0; i < kTestNum; ++i) {
        ASSERT_EQ(pages_or.value()[i]->IsDirty(), true);
      }
      for (size_t i = kTestNum; i < kTestEndNum; ++i) {
        ASSERT_EQ(pages_or.value()[i]->IsDirty(), false);
      }
    }
  };
  // Test FileCache::GetPages() with multiple threads
  std::thread thread1(task);
  std::thread thread2(task);
  // Start threads.
  for (auto &locked_page : locked_pages) {
    locked_page.reset();
  }
  thread1.join();
  thread2.join();

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, Basic) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  uint8_t buf[kPageSize];
  const uint16_t nblocks = 256;

  // All pages should not be uptodated.
  for (uint16_t i = 0; i < nblocks; ++i) {
    LockedPage page;
    vn->GrabCachePage(i, &page);
    // A newly created page should have kPageUptodate/kPageDirty/kPageWriteback flags clear.
    ASSERT_EQ(page->IsUptodate(), false);
    ASSERT_EQ(page->IsDirty(), false);
    ASSERT_EQ(page->IsWriteback(), false);
    ASSERT_EQ(page->IsLocked(), true);
  }

  // Append |nblocks| * |kPageSize|.
  // Each block is filled with its block offset.
  for (uint16_t i = 0; i < nblocks; ++i) {
    memset(buf, i, kPageSize);
    FileTester::AppendToFile(vn.get(), buf, kPageSize);
  }

  // All pages should be uptodated and dirty.
  for (uint16_t i = 0; i < nblocks; ++i) {
    LockedPage page;
    memset(buf, i, kPageSize);
    vn->GrabCachePage(i, &page);
    ASSERT_EQ(page->IsUptodate(), true);
    ASSERT_EQ(page->IsDirty(), true);
    ASSERT_EQ(memcmp(buf, page->GetAddress(), kPageSize), 0);
  }

  // Write out some dirty pages
  WritebackOperation op = {.end = nblocks / 2, .bSync = true};
  vn->Writeback(op);

  // Check if each page has a correct dirty flag.
  for (size_t i = 0; i < nblocks; ++i) {
    LockedPage page;
    vn->GrabCachePage(i, &page);
    ASSERT_EQ(page->IsUptodate(), true);
    if (i < nblocks / 2) {
      ASSERT_EQ(page->IsDirty(), false);
    } else {
      ASSERT_EQ(page->IsDirty(), true);
    }
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, Truncate) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  uint8_t buf[kPageSize];
  const uint16_t nblocks = 256;

  // Append |nblocks| * |kPageSize|.
  // Each block is filled with its block offset.
  for (uint16_t i = 0; i < nblocks; ++i) {
    memset(buf, i, kPageSize);
    FileTester::AppendToFile(vn.get(), buf, kPageSize);
  }

  // All pages should be uptodated and dirty.
  for (uint16_t i = 0; i < nblocks; ++i) {
    LockedPage page;
    vn->GrabCachePage(i, &page);
    ASSERT_EQ(page->IsUptodate(), true);
    ASSERT_EQ(page->IsDirty(), true);
  }

  // Truncate the size of |vn| to the half.
  pgoff_t start = static_cast<pgoff_t>(nblocks) / 2 * kPageSize;
  vn->TruncateBlocks(start);

  // Check if each page has correct flags.
  for (size_t i = 0; i < nblocks; ++i) {
    LockedPage page;
    vn->GrabCachePage(i, &page);
    auto data_blkaddr = vn->FindDataBlkAddr(i);
    ASSERT_TRUE(data_blkaddr.is_ok());
    if (i >= start / kPageSize) {
      ASSERT_EQ(page->IsDirty(), false);
      ASSERT_EQ(page->IsUptodate(), false);
      ASSERT_EQ(data_blkaddr.value(), kNullAddr);
    } else {
      ASSERT_EQ(page->IsDirty(), true);
      ASSERT_EQ(page->IsUptodate(), true);
      ASSERT_EQ(data_blkaddr.value(), kNewAddr);
    }
  }

  --start;
  // Punch a hole at start
  vn->TruncateHole(start, start + 1);

  {
    LockedPage page;
    vn->GrabCachePage(start, &page);
    auto data_blkaddr = vn->FindDataBlkAddr(start);
    ASSERT_TRUE(data_blkaddr.is_ok());
    // |page| for the hole should be invalidated.
    ASSERT_EQ(page->IsDirty(), false);
    ASSERT_EQ(page->IsUptodate(), false);
    ASSERT_EQ(data_blkaddr.value(), kNullAddr);
  }

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, LockedPageBasic) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  {
    LockedPage page;
    ASSERT_EQ(page, nullptr);
    ASSERT_EQ(vn->GrabCachePage(0, &page), ZX_OK);
  }

  fbl::RefPtr<Page> page;
  ASSERT_EQ(vn->FindPage(0, &page), ZX_OK);
  {
    LockedPage locked_page(page);
    ASSERT_TRUE(page->IsLocked());
  }
  ASSERT_FALSE(page->IsLocked());

  vn->Close();
  vn = nullptr;
}

TEST_F(FileCacheTest, LockedPageRelease) {
  fbl::RefPtr<fs::Vnode> test_file;
  root_dir_->Create("test", S_IFREG, &test_file);
  fbl::RefPtr<f2fs::File> vn = fbl::RefPtr<f2fs::File>::Downcast(std::move(test_file));

  {
    LockedPage page;
    ASSERT_EQ(vn->GrabCachePage(0, &page), ZX_OK);
  }

  fbl::RefPtr<Page> page;
  ASSERT_EQ(vn->FindPage(0, &page), ZX_OK);

  LockedPage locked_page(page);
  ASSERT_TRUE(page->IsLocked());
  fbl::RefPtr<Page> released_page = locked_page.release();
  ASSERT_FALSE(released_page->IsLocked());

  ASSERT_EQ(page, released_page);

  vn->Close();
  vn = nullptr;
}

}  // namespace
}  // namespace f2fs
