// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "src/lib/files/file.h"
#include "src/starnix/tests/syscalls/test_helper.h"

// VdsoModificationsDontAffectOtherPrograms invokes the test suite
// to run VdsoHasElfHeader, using gtest_filter. We need
// to keep them in sync.
#define ELF_HEADER_TEST_NAME "VdsoTest.VdsoHasElfHeader"

namespace {

// The ELF header has some bytes of padding that should be ignored by programs.
// We can modify those bytes and that shouldn't affect any vdso elf parsing logic.
constexpr size_t kEhdrEIPadOffset = 0x9;

bool IsEIPadFirstByteZero(void* addr) {
  return static_cast<uint8_t*>(addr)[kEhdrEIPadOffset] == 0x0;
}

void SetEIPadFirstByte(void* addr, uint8_t val) {
  static_cast<uint8_t*>(addr)[kEhdrEIPadOffset] = val;
}

bool IsElfMagic(void* addr) {
  uint8_t elf_magic[] = {'\x7f', 'E', 'L', 'F'};
  return memcmp(addr, elf_magic, sizeof(elf_magic)) == 0;
}

class VdsoProcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    vdso_base_ = reinterpret_cast<void*>(getauxval(AT_SYSINFO_EHDR));

    // To get the vdso size, we need to either parse the vdso elf or look up the
    // mapping size in /proc/self/maps. The latter is easier.
    std::string maps;
    ASSERT_TRUE(files::ReadFileToString("/proc/self/maps", &maps));
    auto vdso_mapping = find_memory_mapping(reinterpret_cast<uintptr_t>(vdso_base_), maps);
    ASSERT_NE(vdso_mapping, std::nullopt);
    ASSERT_EQ(vdso_mapping->pathname, "[vdso]");

    vdso_size_ = static_cast<size_t>(vdso_mapping->end - vdso_mapping->start);
  }

  void* vdso_base_;
  size_t vdso_size_;
};
}  // namespace

TEST(VdsoTest, AtSysinfoEhdrPresent) {
  uintptr_t addr = (uintptr_t)getauxval(AT_SYSINFO_EHDR);

  EXPECT_NE(addr, 0ul);
}

TEST_F(VdsoProcTest, VdsoMappingCannotBeSplit) {
  // TODO(fxbug.dev/129749): Find out why this test does not work on host in CQ
  if (!test_helper::IsStarnix()) {
    GTEST_SKIP() << "This test does not work on Linux in CQ";
  }

  const size_t page_size = SAFE_SYSCALL(sysconf(_SC_PAGE_SIZE));

  // This test will be disabled in Starnix until their vDSO grows to more than one page.
  if (vdso_size_ == page_size) {
    GTEST_SKIP() << "Need more than one vdso page to split it";
  }

  ForkHelper helper;

  // We cannot unmap one page of the vdso.
  helper.RunInForkedProcess([&] {
    EXPECT_NE(munmap(vdso_base_, page_size), 0)
        << "vdso: base " << vdso_base_ << " size " << vdso_size_ << " page size: " << page_size;
    EXPECT_EQ(errno, EINVAL);
  });
  EXPECT_TRUE(helper.WaitForChildren());

  // We cannot mprotect one page of the vdso.
  helper.RunInForkedProcess([&] {
    EXPECT_NE(mprotect(vdso_base_, page_size, PROT_NONE), 0);
    EXPECT_EQ(errno, EINVAL);
  });
  EXPECT_TRUE(helper.WaitForChildren());

  // We cannot map on top of one page of the vdso.
  helper.RunInForkedProcess([&] {
    EXPECT_EQ(
        mmap(vdso_base_, page_size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
        MAP_FAILED);
    EXPECT_EQ(errno, ENOMEM);
  });
  EXPECT_TRUE(helper.WaitForChildren());

  void* new_addr = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(new_addr, MAP_FAILED);

  // We cannot mremap one page of the vdso somewhere else.
  helper.RunInForkedProcess([&, new_addr] {
    EXPECT_EQ(mremap(vdso_base_, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, new_addr),
              MAP_FAILED);
    EXPECT_EQ(errno, EINVAL);
  });
  EXPECT_TRUE(helper.WaitForChildren());

  // We cannot mremap something else on top of one page of the vdso.
  helper.RunInForkedProcess([&, new_addr] {
    EXPECT_EQ(mremap(new_addr, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, vdso_base_),
              MAP_FAILED);
    EXPECT_EQ(errno, EINVAL);
  });
  EXPECT_TRUE(helper.WaitForChildren());

  ASSERT_EQ(munmap(new_addr, page_size), 0);
}

TEST_F(VdsoProcTest, VdsoCanBeUnmapped) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    EXPECT_EQ(munmap(vdso_base_, vdso_size_), 0);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoCanBeMprotected) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    EXPECT_EQ(mprotect(vdso_base_, vdso_size_, PROT_READ | PROT_WRITE | PROT_EXEC), 0);
    EXPECT_EQ(mprotect(vdso_base_, vdso_size_, PROT_NONE), 0);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoCanBeMappedInto) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    EXPECT_NE(
        mmap(vdso_base_, vdso_size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
        MAP_FAILED);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoCanBeRemapped) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    void* new_addr = mmap(NULL, vdso_size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(new_addr, MAP_FAILED);
    EXPECT_NE(mremap(vdso_base_, vdso_size_, vdso_size_, MREMAP_FIXED | MREMAP_MAYMOVE, new_addr),
              MAP_FAILED);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoCanBeRemappedInto) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    void* new_addr = mmap(NULL, vdso_size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(new_addr, MAP_FAILED);
    EXPECT_NE(mremap(new_addr, vdso_size_, vdso_size_, MREMAP_FIXED | MREMAP_MAYMOVE, vdso_base_),
              MAP_FAILED);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoModificationsDontAffectParent) {
  ASSERT_TRUE(IsEIPadFirstByteZero(vdso_base_));
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(mprotect(vdso_base_, vdso_size_, PROT_READ | PROT_WRITE | PROT_EXEC), 0);
    SetEIPadFirstByte(vdso_base_, 0x3F);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
  EXPECT_TRUE(IsEIPadFirstByteZero(vdso_base_));
}

TEST_F(VdsoProcTest, VdsoModificationsShowUpInFork) {
  ASSERT_TRUE(IsEIPadFirstByteZero(vdso_base_));
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(mprotect(vdso_base_, vdso_size_, PROT_READ | PROT_WRITE | PROT_EXEC), 0);
    // We are going to modify the vdso. Try not to use complex code.
    SetEIPadFirstByte(vdso_base_, 0x3F);

    pid_t child_pid = fork();
    if (child_pid == 0) {
      // We should not see the ELF magic header on the vDSO.
      _exit(IsEIPadFirstByteZero(vdso_base_) ? 1 : 0);
    }

    int status;
    pid_t waited = waitpid(child_pid, &status, 0);
    SetEIPadFirstByte(vdso_base_, 0x0);

    EXPECT_EQ(waited, child_pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    _exit(0);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoModificationsAferForkDontShowUpInChild) {
  ASSERT_TRUE(IsEIPadFirstByteZero(vdso_base_));
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(mprotect(vdso_base_, vdso_size_, PROT_READ | PROT_WRITE | PROT_EXEC), 0);

    SignalMaskHelper signal_helper = SignalMaskHelper();
    signal_helper.blockSignal(SIGUSR1);

    ForkHelper child_helper;
    pid_t child_pid = fork();
    if (child_pid == 0) {
      signal_helper.waitForSignal(SIGUSR1);
      // We should not see the vdso modified by our parent.
      _exit(IsEIPadFirstByteZero(vdso_base_) ? 0 : 1);
    }

    SetEIPadFirstByte(vdso_base_, 0x3F);
    ASSERT_EQ(kill(child_pid, SIGUSR1), 0);

    int status;
    pid_t waited = waitpid(child_pid, &status, 0);
    SetEIPadFirstByte(vdso_base_, 0x0);

    EXPECT_EQ(waited, child_pid) << errno;
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);

    signal_helper.restoreSigmask();
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST(VdsoTest, VdsoCanBeMadvised) {
  const size_t page_size = SAFE_SYSCALL(sysconf(_SC_PAGE_SIZE));
  void* vdso_addr = reinterpret_cast<void*>(getauxval(AT_SYSINFO_EHDR));

  std::vector<uint8_t> vdso_bkp(page_size, 0);
  memcpy((void*)vdso_bkp.data(), vdso_addr, page_size);

  EXPECT_EQ(0, madvise(vdso_addr, page_size, MADV_DONTNEED));
  EXPECT_EQ(0, memcmp(vdso_bkp.data(), vdso_addr, page_size));
}

TEST(VdsoTest, VdsoHasElfHeader) {
  // This test is invoked by other tests, so we need to make sure the naming is
  // consistent with ELF_HEADER_TEST_NAME.
  std::string test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
  std::string suite_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->test_suite_name();

  ASSERT_EQ(suite_name + "." + test_name, ELF_HEADER_TEST_NAME);
  void* vdso_addr = reinterpret_cast<void*>(getauxval(AT_SYSINFO_EHDR));
  EXPECT_TRUE(IsElfMagic(vdso_addr));

  EXPECT_TRUE(IsEIPadFirstByteZero(vdso_addr));
}

TEST_F(VdsoProcTest, VdsoModificationsDontAffectOtherPrograms) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    const char* argv[] = {"/proc/self/exe", "--gtest_filter=" ELF_HEADER_TEST_NAME, NULL};
    ASSERT_EQ(mprotect(vdso_base_, vdso_size_, PROT_READ | PROT_WRITE | PROT_EXEC), 0);

    // Don't print anything on the child.
    ASSERT_EQ(fcntl(fileno(stdout), F_SETFD, FD_CLOEXEC), 0);
    ASSERT_EQ(fcntl(fileno(stderr), F_SETFD, FD_CLOEXEC), 0);
    ASSERT_EQ(fcntl(fileno(stdin), F_SETFD, FD_CLOEXEC), 0);

    // We are going to modify the vdso. Try not to use complex code.
    SetEIPadFirstByte(vdso_base_, 0x3F);
    // From this point on, we can't use any vdso call.
    execve(argv[0], const_cast<char**>(&argv[0]), NULL);
    _exit(1);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(VdsoProcTest, VdsoModificationsBeforeForkingDontAffectOtherPrograms) {
  // Due to eager vmo copies in starnix fork, modifying the vdso before and
  // after fork has different effects. This test checks that if we modify the
  // vdso before forking, and execve into another binary, that binary will *not*
  // see our vdso modifications.
  const char* argv[] = {"/proc/self/exe", "--gtest_filter=" ELF_HEADER_TEST_NAME, NULL};
  ASSERT_EQ(mprotect(vdso_base_, vdso_size_, PROT_READ | PROT_WRITE | PROT_EXEC), 0);
  ASSERT_TRUE(IsEIPadFirstByteZero(vdso_base_));

  // Don't print anything on the child.
  ASSERT_EQ(fcntl(fileno(stdout), F_SETFD, FD_CLOEXEC), 0);
  ASSERT_EQ(fcntl(fileno(stderr), F_SETFD, FD_CLOEXEC), 0);
  ASSERT_EQ(fcntl(fileno(stdin), F_SETFD, FD_CLOEXEC), 0);

  // We are going to modify the vdso. Try not to use complex code.
  SetEIPadFirstByte(vdso_base_, 0x3F);

  // From this point on, we can't use any vdso call.
  pid_t child_pid = fork();

  if (child_pid == 0) {
    execve(argv[0], const_cast<char**>(&argv[0]), NULL);
    _exit(1);
  } else {
    int status;
    pid_t waited = waitpid(child_pid, &status, 0);

    // restore the vdso.
    SetEIPadFirstByte(vdso_base_, 0x00);

    EXPECT_EQ(waited, child_pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
}
