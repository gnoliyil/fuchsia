// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

#include <zxtest/zxtest.h>

namespace {

#if defined(__x86_64__)

#include <cpuid.h>

// This is based on code from kernel/ which isn't usable by code in system/.
enum { X86_CPUID_ADDR_WIDTH = 0x80000008 };

uint32_t x86_linear_address_width() {
  uint32_t eax, ebx, ecx, edx;
  __cpuid(X86_CPUID_ADDR_WIDTH, eax, ebx, ecx, edx);
  return (eax >> 8) & 0xff;
}

#endif

TEST(MemoryMappingTest, AddressSpaceLimitsTest) {
#if defined(__x86_64__)
  size_t page_size = getpagesize();
  zx_handle_t vmo;
  EXPECT_OK(zx_vmo_create(page_size, 0, &vmo));
  EXPECT_NE(vmo, ZX_HANDLE_INVALID, "vm_object_create");

  // This is the lowest non-canonical address on x86-64.  We want to
  // make sure that userland cannot map a page immediately below
  // this address.  See docs/sysret_problem.md for an explanation of
  // the reason.
  uintptr_t noncanon_addr = ((uintptr_t)1) << (x86_linear_address_width() - 1);

  zx_info_vmar_t vmar_info;
  zx_status_t status = zx_object_get_info(zx_vmar_root_self(), ZX_INFO_VMAR, &vmar_info,
                                          sizeof(vmar_info), nullptr, nullptr);
  EXPECT_OK(status, "get_info");

  // Check that we cannot map a page ending at |noncanon_addr|.
  size_t offset = noncanon_addr - page_size - vmar_info.base;
  uintptr_t addr;
  status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_SPECIFIC,
                       offset, vmo, 0, page_size, &addr);
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status, "vm_map");

  // Check that we can map at the next address down.  This helps to
  // verify that the previous check didn't fail for some unexpected
  // reason.
  offset = noncanon_addr - page_size * 2 - vmar_info.base;
  status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_SPECIFIC,
                       offset, vmo, 0, page_size, &addr);
  EXPECT_OK(status, "vm_map");

  // Check that ZX_VM_SPECIFIC fails on already-mapped locations.
  // Otherwise, the previous mapping could have overwritten
  // something that was in use, which could cause problems later.
  status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_SPECIFIC,
                       offset, vmo, 0, page_size, &addr);
  EXPECT_EQ(ZX_ERR_ALREADY_EXISTS, status, "vm_map");

  // Clean up.
  EXPECT_OK(zx_vmar_unmap(zx_vmar_root_self(), addr, page_size));
  EXPECT_OK(zx_handle_close(vmo));
#endif
}

TEST(MemoryMappingTest, MmapZerofilledTest) {
  char* addr = (char*)mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  for (size_t i = 0; i < 16384; i++) {
    EXPECT_EQ('\0', addr[i], "non-zero memory found");
  }
  int unmap_result = munmap(addr, 16384);
  EXPECT_EQ(0, unmap_result, "munmap should have succeeded");
}

TEST(MemoryMappingTest, MmapLenTest) {
  uint32_t* addr = (uint32_t*)mmap(nullptr, 0, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  auto test_errno = errno;
  EXPECT_EQ(MAP_FAILED, addr, "mmap should fail when len == 0");
  EXPECT_EQ(EINVAL, test_errno, "mmap errno should be EINVAL when len == 0");

  addr = (uint32_t*)mmap(nullptr, PTRDIFF_MAX, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  test_errno = errno;
  EXPECT_EQ(MAP_FAILED, addr, "mmap should fail when len >= PTRDIFF_MAX");
  EXPECT_EQ(ENOMEM, test_errno, "mmap errno should be ENOMEM when len >= PTRDIFF_MAX");
}

TEST(MemoryMappingTest, MmapOffsetTest) {
  uint32_t* addr =
      (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 4);
  auto test_errno = errno;
  EXPECT_EQ(MAP_FAILED, addr, "mmap should fail for unaligned offset");
  EXPECT_EQ(EINVAL, test_errno, "mmap errno should be EINVAL for unaligned offset");
}

// Define a little fragment of code that we can copy.
extern "C" const uint8_t begin_add[], end_add[];
__asm__(
    ".pushsection .rodata.add_code\n"
    ".globl begin_add\n"
    "begin_add:"
#ifdef __x86_64__
    "mov %rdi, %rax\n"
    "add %rsi, %rax\n"
    "ret\n"
#elif defined(__aarch64__)
    "add x0, x0, x1\n"
    "ret\n"
#elif defined(__riscv)
    "add a0, a0, a1\n"
    "ret\n"
#else
#error "what machine?"
#endif
    ".globl end_add\n"
    "end_add:"
    ".popsection");

TEST(MemoryMappingTest, MmapProtExecTest) {
  if (getenv("NO_AMBIENT_MARK_VMO_EXEC")) {
    ZXTEST_SKIP("Running without the AMBIENT_MARK_VMO_EXEC policy, skipping test case.");
  }

  // Allocate a page that will later be made executable.
  size_t page_size = getpagesize();
  void* addr =
      mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  EXPECT_NE(MAP_FAILED, addr, "mmap should have succeeded for PROT_READ|PROT_WRITE");

  // Copy over code from our address space into the newly allocated memory.
  ASSERT_LE(static_cast<size_t>(end_add - begin_add), page_size);
  memcpy(addr, begin_add, end_add - begin_add);

  // mark the code executable
  int result = mprotect(addr, page_size, PROT_READ | PROT_EXEC);
  EXPECT_EQ(0, result, "Unable to mark pages PROT_READ|PROT_EXEC");

  // Execute the code from our new location.
  auto add_func = reinterpret_cast<int (*)(int, int)>(reinterpret_cast<uintptr_t>(addr));
  int add_result = add_func(1, 2);

  // Check that the result of adding 1+2 is 3.
  EXPECT_EQ(3, add_result);

  // Deallocate pages
  result = munmap(addr, page_size);
  EXPECT_EQ(0, result, "munmap unexpectedly failed");
}

TEST(MemoryMappingTest, MmapProtTest) {
  volatile uint32_t* addr =
      (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  EXPECT_NE(MAP_FAILED, addr, "mmap should have succeeded for PROT_NONE");

  addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  EXPECT_NE(MAP_FAILED, addr, "mmap failed for read-only alloc");

  // This is somewhat pointless, to have a private read-only mapping, but we
  // should be able to read it.
  EXPECT_EQ(*addr, *addr, "could not read from mmaped address");

  addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                         -1, 0);
  EXPECT_NE(MAP_FAILED, addr, "mmap failed for read-write alloc");

  // Now we test writing to the mapped memory, and verify that we can read it
  // back.
  *addr = 5678u;
  EXPECT_EQ(5678u, *addr, "writing to address returned by mmap failed");
}

TEST(MemoryMappingTest, MmapFlagsTest) {
  uint32_t* addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ, MAP_ANON, -1, 0);
  auto test_errno = errno;
  EXPECT_EQ(MAP_FAILED, addr, "mmap should fail without MAP_PRIVATE or MAP_SHARED");
  EXPECT_EQ(EINVAL, test_errno, "mmap errno should be EINVAL with bad flags");

  addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ, MAP_PRIVATE | MAP_SHARED | MAP_ANON,
                         -1, 0);
  test_errno = errno;
  EXPECT_EQ(MAP_FAILED, addr, "mmap should fail with both MAP_PRIVATE and MAP_SHARED");
  EXPECT_EQ(EINVAL, test_errno, "mmap errno should be EINVAL with bad flags");

  addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  EXPECT_NE(MAP_FAILED, addr, "mmap failed with MAP_PRIVATE flags");

  addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ, MAP_SHARED | MAP_ANON, -1, 0);
  EXPECT_NE(MAP_FAILED, addr, "mmap failed with MAP_SHARED flags");
}

TEST(MemoryMappingTest, MprotectTest) {
  uint32_t* addr = (uint32_t*)mmap(nullptr, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANON, -1, 0);
  ASSERT_NE(MAP_FAILED, addr, "mmap failed to map");

  int page_size = getpagesize();
  // Should be able to write.
  *addr = 10;
  EXPECT_EQ(10u, *addr, "read after write failed");

  int status = mprotect(addr, page_size, PROT_READ);
  EXPECT_EQ(0, status, "mprotect failed to downgrade to read-only");

  ASSERT_DEATH(([&addr]() {
                 uint32_t* intptr = static_cast<uint32_t*>(addr);
                 *intptr = 12;
               }),
               "write to addr should have caused a crash");

  status = mprotect(addr, page_size, PROT_WRITE);
  auto test_errno = errno;
  EXPECT_EQ(-1, status, "mprotect should fail for write-only");
  EXPECT_EQ(ENOTSUP, test_errno, "mprotect should return ENOTSUP for write-only");

  status = mprotect(addr, page_size, PROT_NONE);
  test_errno = errno;
  EXPECT_EQ(0, status, "mprotect should succeed for PROT_NONE");
}

}  // namespace
