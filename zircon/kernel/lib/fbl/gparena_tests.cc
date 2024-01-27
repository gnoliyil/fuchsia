// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/unittest/unittest.h>
#include <platform.h>

#include <arch/regs.h>
#include <fbl/alloc_checker.h>
#include <fbl/gparena.h>

using fbl::GPArena;

static bool can_declare_small_objectsize() {
  BEGIN_TEST;

  // This is just exercising the static_assert in GPArena that ensures we can have fairly small
  // objectsize even in the presence of preservation.
  GPArena<0, 8> smallest;
  GPArena<4, 16> smallest_with_preserve;

  END_TEST;
}

static bool basic_lifo() {
  BEGIN_TEST;

  GPArena<0, 8> arena;
  ASSERT_EQ(arena.Init("test", 4), ZX_OK);

  void* first = arena.Alloc();
  ASSERT_NONNULL(first);

  void* second = arena.Alloc();
  ASSERT_NONNULL(second);

  // Alloc should always return the last Free.
  arena.Free(second);
  EXPECT_EQ(second, arena.Alloc());

  // If we Free multiple we should get them back in last-in-first-out order.
  arena.Free(second);
  arena.Free(first);
  EXPECT_EQ(first, arena.Alloc());
  EXPECT_EQ(second, arena.Alloc());

  // Cleanup.
  arena.Free(second);
  arena.Free(first);

  END_TEST;
}

static bool out_of_memory() {
  BEGIN_TEST;

  // Use large objects so we can store all the allocations in a stack array.
  GPArena<0, 512> arena;
  constexpr int count = PAGE_SIZE / 512;
  ASSERT_EQ(arena.Init("test", count), ZX_OK);
  void* allocs[count];

  // Allocate all objects from the arena.
  for (int i = 0; i < count; i++) {
    allocs[i] = arena.Alloc();
    ASSERT_NONNULL(allocs[i]);
  }

  // Unless we calculated wrong, allocations should now fail.
  EXPECT_NULL(arena.Alloc());
  EXPECT_NULL(arena.Alloc());

  // Should be able to put objects back and then successfully re-allocate them.
  arena.Free(allocs[count - 1]);
  arena.Free(allocs[count - 2]);
  EXPECT_EQ(allocs[count - 2], arena.Alloc());
  EXPECT_EQ(allocs[count - 1], arena.Alloc());

  // Once we re-allocate the ones we put back, future allocations should be back to failing.
  EXPECT_NULL(arena.Alloc());
  EXPECT_NULL(arena.Alloc());

  // Cleanup.
  for (int i = 0; i < count; i++) {
    arena.Free(allocs[i]);
  }

  END_TEST;
}

static bool does_preserve() {
  BEGIN_TEST;

  constexpr int preserve = 8;
  constexpr char magic[preserve + 1] = "preserve";

  GPArena<preserve, 16> arena;
  constexpr int count = 4;
  ASSERT_EQ(arena.Init("test", count), ZX_OK);
  void* allocs[count];

  // Allocate all our objects, and initialize them with the magic data.
  for (int i = 0; i < count; i++) {
    allocs[i] = arena.Alloc();
    ASSERT_NONNULL(allocs[i]);
    memcpy(allocs[i], magic, preserve);
  }

  // Return the objects back to the allocator.
  for (int i = 0; i < count; i++) {
    arena.Free(allocs[i]);
  }

  // Whilst unallocated the preserve region should be unchanged.
  for (int i = 0; i < count; i++) {
    EXPECT_EQ(memcmp(allocs[i], magic, preserve), 0);
  }

  // Reallocate the object and validate that allocation didn't destroy the preserve region.
  for (int i = 0; i < count; i++) {
    allocs[i] = arena.Alloc();
    ASSERT_NONNULL(allocs[i]);
  }
  for (int i = 0; i < count; i++) {
    EXPECT_EQ(memcmp(allocs[i], magic, preserve), 0);
  }

  // Cleanup.
  for (int i = 0; i < count; i++) {
    arena.Free(allocs[i]);
  }

  END_TEST;
}

// Small helper to do offset arithmetic of an arena Base, keeping it as a void*
template <size_t P, size_t O>
static inline void* base_offset(const GPArena<P, O>& arena, size_t offset) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(arena.Base()) + offset);
}

static bool committed_monotonic() {
  BEGIN_TEST;
  GPArena<0, 8> arena;
  ASSERT_EQ(arena.Init("test", 4), ZX_OK);

  // Initially Alloc has not been called, and so Committed can never be true.
  EXPECT_FALSE(arena.Committed(base_offset(arena, 0)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 8)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 16)));

  // Perform an allocation check Committed is true for that value, but no other.
  EXPECT_EQ(arena.Alloc(), base_offset(arena, 0));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 0)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 8)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 16)));

  // Perform another allocation, Committed should be true for it and previous allocation.
  EXPECT_EQ(arena.Alloc(), base_offset(arena, 8));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 0)));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 8)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 16)));

  // Returning the allocated objects should have no impact on what Committed returns.
  arena.Free(base_offset(arena, 8));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 0)));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 8)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 16)));

  arena.Free(base_offset(arena, 0));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 0)));
  EXPECT_TRUE(arena.Committed(base_offset(arena, 8)));
  EXPECT_FALSE(arena.Committed(base_offset(arena, 16)));

  END_TEST;
}

// A helper that continuously allocates and frees a single object.
template <size_t P, size_t O>
struct ArenaAllocFixture {
  GPArena<P, O> arena;
  ktl::atomic<bool> stop_requested{};

  // A thread_start_routine for use with Thread::Create.
  static int run(void* arg) { return static_cast<ArenaAllocFixture<P, O>*>(arg)->Run(); }
  int Run() {
    unsigned int allocations = 0;
    while (1) {
      void* v = arena.Alloc();
      // On any failure just return. That we terminated at all is the error signal to our parent.
      if (v == nullptr) {
        return -1;
      }
      arena.Free(v);
      // Check every so often if someone is trying to stop us. We don't check every iteration to
      // maximize the chance of actually doing Alloc and Free concurrently.
      allocations++;
      if (allocations % 100 == 0 && stop_requested.load(ktl::memory_order_relaxed)) {
        return 0;
      }
    }
  }
};

static bool parallel_alloc() {
  BEGIN_TEST;

  using Fixture = ArenaAllocFixture<0, 8>;
  Fixture fixture;
  ASSERT_EQ(fixture.arena.Init("test", 4), ZX_OK);

  // Spin up two instances of the allocation fixture that will run in parallel.
  auto t1 = Thread::Create("gparena worker1", Fixture::run, &fixture, DEFAULT_PRIORITY);
  auto t2 = Thread::Create("gparena worker2", Fixture::run, &fixture, DEFAULT_PRIORITY);
  t1->Resume();
  t2->Resume();

  // Attempt to join one of the threads, letting it run for a bit. If the join succeeds this means
  // the fixture terminated, which indicates it encountered an error.
  zx_status_t status = t1->Join(nullptr, current_time() + ZX_MSEC(500));
  EXPECT_NE(status, ZX_OK);
  // Check that the other thread is still running as well.
  status = t2->Join(nullptr, current_time());
  EXPECT_NE(status, ZX_OK);

  // Cleanup.
  fixture.stop_requested.store(true, ktl::memory_order_relaxed);
  status = t1->Join(nullptr, current_time() + ZX_SEC(5));
  EXPECT_EQ(status, ZX_OK);
  status = t2->Join(nullptr, current_time() + ZX_SEC(5));
  EXPECT_EQ(status, ZX_OK);

  END_TEST;
}

static bool parallel_grow_memory() {
  BEGIN_TEST;

  constexpr int count = PAGE_SIZE * 64 / 8;

  fbl::AllocChecker ac;
  ktl::unique_ptr<void*[]> allocs = ktl::make_unique<void*[]>(&ac, count);
  ASSERT_TRUE(ac.check());

  using Fixture = ArenaAllocFixture<0, 8>;
  Fixture fixture;
  ASSERT_EQ(fixture.arena.Init("test", count), ZX_OK);

  // Spin up a worker that will perform allocations in parallel whilst we are causing the arena
  // to need to be grown.
  auto t = Thread::Create("gparena worker", Fixture::run, &fixture, DEFAULT_PRIORITY);
  t->Resume();

  // let the worker run for a bit to make sure its started.
  zx_status_t status = t->Join(nullptr, current_time() + ZX_MSEC(10));
  EXPECT_NE(status, ZX_OK);

  // Allocate all the rest of the objects causing arena to have to grow.
  for (int i = 0; i < count - 1; i++) {
    allocs[i] = fixture.arena.Alloc();
    EXPECT_NONNULL(allocs[i]);
  }

  // Worker should still be running fine.
  status = t->Join(nullptr, current_time() + ZX_MSEC(10));
  EXPECT_NE(status, ZX_OK);

  // Cleanup.
  fixture.stop_requested.store(true, ktl::memory_order_relaxed);
  status = t->Join(nullptr, current_time() + ZX_SEC(5));
  EXPECT_EQ(status, ZX_OK);

  for (int i = 0; i < count - 1; i++) {
    fixture.arena.Free(allocs[i]);
  }

  END_TEST;
}

static bool test_confine() {
  BEGIN_TEST;
  constexpr int kMaxItems = 32;
  constexpr int kSize = 64;

  GPArena<0, kSize> arena;
  void* allocs[kMaxItems];

  ASSERT_EQ(arena.Init("test", kMaxItems), ZX_OK);

  for (int i = 0; i < kMaxItems; i++) {
    allocs[i] = arena.Alloc();
  }

  const uintptr_t base = reinterpret_cast<uintptr_t>(arena.Base());
  for (int i = 0; i < 2 * kMaxItems; i++) {
    uintptr_t expected_object = base + i * kSize;
    if (i < kMaxItems) {
      EXPECT_EQ(expected_object, arena.Confine(expected_object));
    } else {
      EXPECT_EQ(base, arena.Confine(expected_object));
    }
  }

  EXPECT_EQ(base, arena.Confine(-1ull));
  EXPECT_EQ(base, arena.Confine(base - 1));
  EXPECT_EQ(base, arena.Confine(base - kSize - 1));

  for (int i = 0; i < kMaxItems; i++) {
    arena.Free(allocs[i]);
  }

  END_TEST;
}

#define GPARENA_UNITTEST(fname) UNITTEST(#fname, fname)

UNITTEST_START_TESTCASE(gparena_tests)
GPARENA_UNITTEST(can_declare_small_objectsize)
GPARENA_UNITTEST(basic_lifo)
GPARENA_UNITTEST(out_of_memory)
GPARENA_UNITTEST(does_preserve)
GPARENA_UNITTEST(committed_monotonic)
GPARENA_UNITTEST(parallel_alloc)
GPARENA_UNITTEST(parallel_grow_memory)
GPARENA_UNITTEST(test_confine)
UNITTEST_END_TESTCASE(gparena_tests, "gparena_tests", "GPArena test")
