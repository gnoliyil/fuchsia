// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>

#include <gtest/gtest.h>

#include "src/lib/files/file.h"
#include "src/proc/tests/chromiumos/syscalls/test_helper.h"

constexpr size_t MMAP_FILE_SIZE = 64;
constexpr intptr_t LIMIT_4GB = 0x80000000;
constexpr size_t PAGE_SIZE = 0x1000;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

TEST(MmapTest, Map32Test) {
  char* tmp = getenv("TEST_TMPDIR");
  std::string path = tmp == nullptr ? "/tmp/mmaptest" : std::string(tmp) + "/mmaptest";
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
  ASSERT_GE(fd, 0);
  for (unsigned char i = 0; i < MMAP_FILE_SIZE; i++) {
    ASSERT_EQ(write(fd, &i, sizeof(i)), 1);
  }
  close(fd);

  int fdm = open(path.c_str(), O_RDWR);
  ASSERT_GE(fdm, 0);

  void* mapped =
      mmap(nullptr, MMAP_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_32BIT, fdm, 0);
  intptr_t maploc = reinterpret_cast<intptr_t>(mapped);
  intptr_t limit = LIMIT_4GB - MMAP_FILE_SIZE;
  ASSERT_GT(maploc, 0);
  ASSERT_LE(maploc, limit);

  ASSERT_EQ(munmap(mapped, MMAP_FILE_SIZE), 0);
  close(fd);

  unlink(path.c_str());
}

TEST(MmapTest, MprotectMultipleMappings) {
  char* page1 = (char*)mmap(nullptr, PAGE_SIZE * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(page1, MAP_FAILED) << strerror(errno);
  char* page2 = (char*)mmap(page1 + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  ASSERT_NE(page2, MAP_FAILED) << strerror(errno);
  memset(page1, 'F', PAGE_SIZE * 2);
  // This gets the starnix mapping state out of sync with the real zircon mappings...
  ASSERT_EQ(mprotect(page1, PAGE_SIZE * 2, PROT_READ), 0) << strerror(errno);
  // ...so madvise clears a page that is not mapped.
  ASSERT_EQ(madvise(page2, PAGE_SIZE, MADV_DONTNEED), 0) << strerror(errno);
  ASSERT_EQ(*page1, 'F');
  ASSERT_EQ(*page2, 0);  // would fail
}

TEST(MmapTest, MprotectSecondPageStringRead) {
  char* addr = static_cast<char*>(
      mmap(nullptr, PAGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

  mprotect(addr + PAGE_SIZE, PAGE_SIZE, 0);
  strcpy(addr, "/dev/null");
  int fd = open(addr, O_RDONLY);
  EXPECT_NE(fd, -1);
  close(fd);
  munmap(addr, PAGE_SIZE * 2);
}

// This variable is accessed from within a signal handler and thus must be declared volatile.
volatile void* expected_fault_address;

// The initial layout for each test is:
//
// ---- 0x00000000
//  ~~
// ---- lowest_addr_                      - start of the playground area, offset 0
//  ~~
// ---- lowest_guard_region_page          - start of guard region (not a mapping)
// 256 pages
// ---- initial_grows_down_low            - start of MAP_GROWSDOWN mapping at the start of the test
// 2 pages (initially, expected to grow)
// ---- grows_down_high                   - end of MAP_GROWSDOWN mapping
// 16 pages
// ---- highest_addr_                     - end of the playground area, offset playground_size()
class MapGrowsdownTest : public testing::Test {
 protected:
  void SetUp() override {
    page_size_ = SAFE_SYSCALL(sysconf(_SC_PAGE_SIZE));
    playground_size_ = 8 * 1024 * page_size_;

    // Find a large portion of unused address space to use in tests.
    void* base_addr =
        mmap(nullptr, playground_size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(base_addr, MAP_FAILED) << "mmap failed: " << strerror(errno) << "(" << errno << ")";
    SAFE_SYSCALL(munmap(base_addr, playground_size_));
    lowest_addr_ = static_cast<std::byte*>(base_addr);
    highest_addr_ = lowest_addr_ + playground_size_;

    // Create a new mapping with MAP_GROWSDOWN a bit below the top of the playground.
    initial_grows_down_size_ = 2 * page_size();
    initial_grows_down_low_offset_ = playground_size() - 16 * page_size();

    void* grow_initial_low_address =
        MapRelative(initial_grows_down_low_offset_, initial_grows_down_size_,
                    PROT_READ | PROT_WRITE, MAP_GROWSDOWN);
    ASSERT_NE(grow_initial_low_address, MAP_FAILED)
        << "mmap failed: " << strerror(errno) << "(" << errno << ")";
    ASSERT_EQ(grow_initial_low_address, OffsetToAddress(initial_grows_down_low_offset_));
    grows_down_high_offset_ = initial_grows_down_low_offset_ + initial_grows_down_size_;
  }

  void TearDown() override { SAFE_SYSCALL(munmap(lowest_addr_, playground_size_)); }

  void* MapRelative(size_t offset, size_t len, int prot, int flags) {
    return mmap(OffsetToAddress(offset), len, prot, flags | MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
  }

  // Tests that a read at |offset| within the playground generates a fault.
  bool TestThatReadSegfaults(intptr_t offset) {
    return TestThatAccessSegfaults(offset, AccessType::Read);
  }

  // Tests that a write at |offset| within the playground generates a fault.
  bool TestThatWriteSegfaults(intptr_t offset) {
    return TestThatAccessSegfaults(offset, AccessType::Write);
  }

  std::byte* OffsetToAddress(intptr_t offset) { return lowest_addr_ + offset; }

  char ReadAtOffset(intptr_t offset) {
    return std::to_integer<char>(*static_cast<volatile std::byte*>(OffsetToAddress(offset)));
  }

  void WriteAtOffset(intptr_t offset) {
    *static_cast<volatile std::byte*>(OffsetToAddress(offset)) = std::byte{};
  }

  void PrintCurrentMappingsToStderr() {
    std::string maps;
    ASSERT_TRUE(files::ReadFileToString("/proc/self/maps", &maps));
    fprintf(stderr, "Playground area is [%p, %p)\n", lowest_addr_, highest_addr_);
    fprintf(stderr, "MAP_GROWSDOWN region initially mapped to [%p, %p)\n",
            OffsetToAddress(initial_grows_down_low_offset_),
            OffsetToAddress(grows_down_high_offset_));
    fprintf(stderr, "%s\n", maps.c_str());
  }

  size_t page_size() const { return page_size_; }
  size_t playground_size() const { return playground_size_; }

  size_t initial_grows_down_size() const { return initial_grows_down_size_; }
  intptr_t initial_grows_down_low_offset() const { return initial_grows_down_low_offset_; }
  intptr_t grows_down_high_offset() const { return grows_down_high_offset_; }

  std::byte* lowest_addr() const { return lowest_addr_; }
  std::byte* highest_addr() const { return highest_addr_; }

 private:
  enum AccessType { Read, Write };

  bool TestThatAccessSegfaults(intptr_t offset, AccessType type) {
    std::byte* test_address = OffsetToAddress(offset);
    ForkHelper helper;
    helper.RunInForkedProcess([test_address, type] {
      struct sigaction segv_act;
      segv_act.sa_sigaction = [](int signo, siginfo_t* info, void* ucontext) {
        if (signo == SIGSEGV && info->si_addr == expected_fault_address) {
          _exit(EXIT_SUCCESS);
        }
        _exit(EXIT_FAILURE);
      };
      segv_act.sa_flags = SA_SIGINFO;
      SAFE_SYSCALL(sigaction(SIGSEGV, &segv_act, nullptr));
      expected_fault_address = test_address;
      if (type == AccessType::Read) {
        *static_cast<volatile std::byte*>(test_address);
      } else {
        *static_cast<volatile std::byte*>(test_address) = std::byte{};
      }
      ADD_FAILURE() << "Expected to fault on access of " << test_address;
      exit(EXIT_FAILURE);
    });
    return helper.WaitForChildren();
  }

  size_t page_size_;
  size_t initial_grows_down_size_;
  intptr_t initial_grows_down_low_offset_;
  intptr_t grows_down_high_offset_;
  size_t playground_size_;
  std::byte* lowest_addr_;
  std::byte* highest_addr_;
};

TEST_F(MapGrowsdownTest, Grow) {
  size_t expected_guard_region_size = 256 * page_size();

  // Create a mapping 4 guard page regions below the first mapping to constrain growth.
  size_t gap_to_next_mapping = 4 * expected_guard_region_size;
  intptr_t constraint_offset = initial_grows_down_low_offset() - gap_to_next_mapping;
  void* constraint_mapping = MapRelative(constraint_offset, page_size(), PROT_NONE, 0);
  ASSERT_NE(constraint_mapping, MAP_FAILED)
      << "mmap failed: " << strerror(errno) << "(" << errno << ")";

  // Read from pages sequentially in the guard regions from just below the MAP_GROWSDOWN mapping up
  // to the edge of the second mapping.
  for (size_t i = 0; i < 4 * expected_guard_region_size / page_size(); ++i) {
    ASSERT_EQ(ReadAtOffset(initial_grows_down_low_offset() - i * page_size()), 0);
  }

  // We should have grown our MAP_GROWSDOWN mapping to touch constraint_mapping. Test by trying to
  // make a new mapping immediately above constraint_mapping with MAP_FIXED_NOREPLACE - this should
  // fail with EXXIST.
  intptr_t test_mapping_offset = constraint_offset + page_size();
  void* desired_test_mapping_address = OffsetToAddress(test_mapping_offset);
  void* rv = mmap(desired_test_mapping_address, page_size(), PROT_READ,
                  MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  EXPECT_EQ(rv, MAP_FAILED);
  EXPECT_EQ(errno, EEXIST);

  size_t expected_growsdown_final_size = initial_grows_down_size() + 4 * expected_guard_region_size;
  intptr_t final_grows_down_offset = grows_down_high_offset() - expected_growsdown_final_size;
  std::byte* final_grows_down_address = OffsetToAddress(final_grows_down_offset);
  SAFE_SYSCALL(munmap(final_grows_down_address, expected_growsdown_final_size));
  SAFE_SYSCALL(munmap(constraint_mapping, gap_to_next_mapping));
}

TEST_F(MapGrowsdownTest, TouchPageAbove) {
  // The page immediately above the MAP_GROWSDOWN region is unmapped so issuing a read should SEGV.
  ASSERT_TRUE(TestThatReadSegfaults(grows_down_high_offset()));
}

TEST_F(MapGrowsdownTest, TouchHighestGuardRegionPage) {
  intptr_t highest_guard_region_page_offset = initial_grows_down_low_offset() - page_size();
  intptr_t lowest_guard_region_page_offset = highest_guard_region_page_offset - 512 * page_size();

  // Try making a NOREPLACE mapping just below the guard region.
  intptr_t test_offset = lowest_guard_region_page_offset - page_size();
  std::byte* test_address = OffsetToAddress(test_offset);
  void* test_mapping = mmap(test_address, page_size(), PROT_READ,
                            MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(test_mapping, MAP_FAILED) << "mmap failed: " << strerror(errno) << "(" << errno << ")";
  ASSERT_EQ(test_mapping, test_address);
  SAFE_SYSCALL(munmap(test_mapping, page_size()));

  // Read from the highest guard region page. This should trigger growth of the MAP_GROWSDOWN
  // mapping by one page.
  EXPECT_EQ(ReadAtOffset(test_offset), '\0');

  // Now mapping the page we just touched should fail.
  void* rv = mmap(test_address, page_size(), PROT_READ,
                  MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_EQ(rv, MAP_FAILED);
  ASSERT_EQ(errno, EEXIST);

  // And we shouldn't be able to make a NOREPLACE mapping in the new guard region.
  test_mapping = mmap(test_address, page_size(), PROT_READ,
                      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_EQ(test_mapping, MAP_FAILED);
  ASSERT_EQ(errno, EEXIST);
}

TEST_F(MapGrowsdownTest, MapNoreplaceInGuardRegion) {
  // Make a MAP_GROWSDOWN mapping slightly below the top of the playground area.
  size_t initial_grows_down_size = 2 * page_size();
  intptr_t grow_low_offset = playground_size() - 16 * page_size();

  void* grow_initial_low_address =
      MapRelative(grow_low_offset, initial_grows_down_size, PROT_READ, MAP_GROWSDOWN);
  ASSERT_NE(grow_initial_low_address, MAP_FAILED)
      << "mmap failed: " << strerror(errno) << "(" << errno << ")";
  ASSERT_EQ(grow_initial_low_address, OffsetToAddress(grow_low_offset));

  // The page immediately below grow_low_address is the highest guard page. Try making a new mapping
  // in this region.
  intptr_t highest_guard_region_page_offset = grow_low_offset - page_size();
  std::byte* highest_guard_region_page_address = OffsetToAddress(highest_guard_region_page_offset);
  void* rv = mmap(highest_guard_region_page_address, page_size(), PROT_READ,
                  MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_EQ(rv, highest_guard_region_page_address);

  // Now that we've mapped something else into the guard region, touching the pages below the new
  // mapping will no longer trigger growth of our MAP_GROWSDOWN section.
  intptr_t test_offset = highest_guard_region_page_offset - page_size();
  ASSERT_TRUE(TestThatReadSegfaults(test_offset));

  // Unmap our mapping in the guard region.
  SAFE_SYSCALL(munmap(highest_guard_region_page_address, page_size()));

  // Now the region is growable again.
  ASSERT_EQ(ReadAtOffset(test_offset), '\0');

  // Since we've grown the region, we can no longer map into what used to be the top of the guard
  // region.
  rv = mmap(highest_guard_region_page_address, page_size(), PROT_READ,
            MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_EQ(rv, MAP_FAILED);
  ASSERT_EQ(errno, EEXIST);
}

TEST_F(MapGrowsdownTest, MprotectBeforeGrow) {
  // Reduce the production on the low page of the growsdown region to read-only
  SAFE_SYSCALL(mprotect(OffsetToAddress(initial_grows_down_low_offset()), page_size(),
                        PROT_READ | PROT_GROWSDOWN));

  // The high page of the initial region should still be writable.
  WriteAtOffset(grows_down_high_offset() - page_size());

  // Grow the region by touching a page in the guard region.
  intptr_t test_offset = initial_grows_down_low_offset() - page_size();
  ASSERT_EQ(ReadAtOffset(test_offset), 0);

  // The new page should have only PROT_READ protections as the mprotect on the bottom of the
  // growsdown region extends to new pages.
  ASSERT_TRUE(TestThatWriteSegfaults(test_offset));
}

TEST_F(MapGrowsdownTest, MprotectAfterGrow) {
  // Grow the region down by 2 pages by accessing a page in the guard region.
  intptr_t test_offset = initial_grows_down_low_offset() - 2 * page_size();
  ASSERT_EQ(ReadAtOffset(initial_grows_down_low_offset() - 2 * page_size()), 0);

  // Set protection on low page of the initial growsdown region to PROT_NONE | PROT_GROWSDOWN.
  SAFE_SYSCALL(mprotect(OffsetToAddress(initial_grows_down_low_offset()), page_size(),
                        PROT_NONE | PROT_GROWSDOWN));

  // This also changes the protection of pages below the mprotect() region, so we can no longer read
  // at |test_offset|.
  ASSERT_TRUE(TestThatReadSegfaults(test_offset));
}

TEST_F(MapGrowsdownTest, MprotectMixGrowsdownAndRegular) {
  // Grow the region down by 2 pages by accessing a page in the guard region.
  intptr_t test_offset = initial_grows_down_low_offset() - 2 * page_size();
  ASSERT_EQ(ReadAtOffset(test_offset), 0);

  // Now there are 4 pages with protection PROT_READ | PROT_WRITE below grows_down_high_offset().
  // Reduce the protections on the second-lowest page to PROT_READ without the PROT_GROWSDOWN flag.
  SAFE_SYSCALL(mprotect(OffsetToAddress(initial_grows_down_low_offset() - page_size()), page_size(),
                        PROT_READ));

  // The lowest page of the mapping should still be PROT_READ | PROT_WRITE
  intptr_t lowest_page_offset = initial_grows_down_low_offset() - 2 * page_size();
  WriteAtOffset(lowest_page_offset);

  // Now set the third-lowest page to PROT_READ with the MAP_GROWSDOWN flag.
  SAFE_SYSCALL(mprotect(OffsetToAddress(initial_grows_down_low_offset()), page_size(),
                        PROT_READ | PROT_GROWSDOWN));

  // This page should now be read-only.
  ASSERT_TRUE(TestThatWriteSegfaults(initial_grows_down_low_offset()));
  ASSERT_EQ(ReadAtOffset(initial_grows_down_low_offset()), '\0');

  // The lowest page of the mapping should still be PROT_READ | PROT_WRITE.
  WriteAtOffset(lowest_page_offset);
}

}  // namespace
