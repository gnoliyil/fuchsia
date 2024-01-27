// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/unittest/unittest.h>

#include <ktl/atomic.h>

#include <ktl/enforce.h>

// Verify that 16-byte atomics are unconditionally supported, regardless of
// compiler intrinsic support. But on RISC-V they are not available at all.
// See atomic.cc for details on 'polyfills'.

#ifndef HAVE_ATOMIC_128
#error "<ktl/atomic.h> should define HAVE_ATOMIC_128 to 0 or 1"
#endif

#if HAVE_ATOMIC_128

namespace {

constexpr unsigned __int128 kValue =
    (static_cast<unsigned __int128>(0x0123456789abcdef) << 64) + 0xfedcba9876543210;

bool Load16Test() {
  BEGIN_TEST;

  {
    ktl::atomic<unsigned __int128> v = 0u;
    EXPECT_EQ(v.load(), 0u);
  }

  {
    ktl::atomic<unsigned __int128> v = kValue;
    EXPECT_EQ(v.load(), kValue);
  }

  END_TEST;
}

bool Store16Test() {
  BEGIN_TEST;

  ktl::atomic<unsigned __int128> v = 0u;
  EXPECT_EQ(v.load(), 0u);
  v.store(kValue);
  EXPECT_EQ(v.load(), kValue);

  END_TEST;
}

bool CompareExchange16Test() {
  BEGIN_TEST;

  {
    // Comparison fails.
    ktl::atomic<unsigned __int128> v = kValue;
    unsigned __int128 expected = kValue - 1;
    EXPECT_FALSE(v.compare_exchange_strong(expected, 0u));
    EXPECT_EQ(expected, kValue);
    EXPECT_EQ(v.load(), kValue);
  }

  {
    // Comparison succeeds.
    ktl::atomic<unsigned __int128> v = kValue;
    unsigned __int128 expected = kValue;
    constexpr unsigned __int128 kDesired = 0xaaaabbbbccccdddd;
    EXPECT_TRUE(v.compare_exchange_strong(expected, kDesired));
    EXPECT_EQ(expected, kValue);
    EXPECT_EQ(v.load(), kDesired);
  }

  END_TEST;
}

// Most of atomic_ref's tests are in stdcompat, along the rest of the polyfill
// library.  We test __int128 specifically in the kernel unit tests, since
// __int128 is unconditionally available in the kernel environment.
bool KtlAtomicRef128Test() {
  BEGIN_TEST;

  unsigned __int128 i = 0;
  ktl::atomic_ref<unsigned __int128> int_ref(i);

  int_ref.store(1);
  EXPECT_EQ(1u, int_ref.load());

  unsigned __int128 expected = 1u;
  EXPECT_EQ(true, int_ref.compare_exchange_strong(expected, 2u));
  EXPECT_EQ(2u, int_ref.load());
  expected = 0;
  EXPECT_EQ(false, int_ref.compare_exchange_strong(expected, 3u));
  EXPECT_EQ(expected, 2u);
  int_ref.store(kValue, ktl::memory_order_release);
  EXPECT_EQ(int_ref.load(ktl::memory_order_acquire), kValue);
  int_ref.store(kValue + 1, ktl::memory_order_release);
  EXPECT_EQ(int_ref.load(ktl::memory_order_acquire), kValue + 1);
  // TODO(fxbug.dev/47117): gcc __int128 is not considered is_lock_free, even though it
  // generates lock-free code for load/store/compare_exchange.
#ifdef __clang__
  EXPECT_EQ(true, int_ref.is_lock_free());
#endif

  END_TEST;
}

}  // namespace

UNITTEST_START_TESTCASE(libc_atomic_tests)
UNITTEST("load_16", Load16Test)
UNITTEST("store_16", Store16Test)
UNITTEST("compare_exchange_16", CompareExchange16Test)
UNITTEST("ktl::atomic_ref_128", KtlAtomicRef128Test)
UNITTEST_END_TESTCASE(libc_atomic_tests, "atomic", "atomic operations test")

#endif  // HAVE_ATOMIC_128
