// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/stdcompat/span.h>

#include <memory>
#include <utility>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fbl/string.h>
#include <fbl/tests/lfsr.h>
#include <fbl/vector.h>
#include <zxtest/zxtest.h>

namespace {

// Different container classes for the types of objects
// which should be tested within a vector (raw types,
// unique pointers, ref pointers).

using ValueType = size_t;

struct TestObject {
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(TestObject);
  TestObject() = delete;
  explicit TestObject(ValueType val) : alive_(true), val_(val) {
    ++live_obj_count_;
    ++ctor_count_;
  }
  TestObject(TestObject&& r) : alive_(r.alive_), val_(r.val_) {
    r.alive_ = false;
    ++ctor_count_;
  }
  TestObject& operator=(TestObject&& r) {
    val_ = r.val_;
    alive_ = r.alive_;
    r.alive_ = false;
    return *this;
  }
  ~TestObject() {
    if (alive_) {
      --live_obj_count_;
    }
    ++dtor_count_;
  }

  ValueType value() const { return val_; }
  static size_t live_obj_count() { return live_obj_count_; }
  static void ResetLiveObjCount() { live_obj_count_ = 0; }

  bool alive_;
  ValueType val_;

  static size_t live_obj_count_;
  static size_t ctor_count_;
  static size_t dtor_count_;
};

size_t TestObject::live_obj_count_ = 0;
size_t TestObject::ctor_count_ = 0;
size_t TestObject::dtor_count_ = 0;

struct ValueTypeTraits {
  using ItemType = ValueType;
  static ItemType Create(ValueType val) { return val; }
  static ValueType GetValue(const ItemType& c) { return c; }
  // We have no way of managing the "live count" of raw types, so we don't.
  static bool CheckLiveCount(size_t expected) { return true; }
  static bool CheckCtorDtorCount() { return true; }
};

struct StructTypeTraits {
  using ItemType = TestObject;
  static ItemType Create(ValueType val) { return TestObject(val); }
  static ValueType GetValue(const ItemType& c) { return c.value(); }
  static bool CheckLiveCount(size_t expected) { return TestObject::live_obj_count() == expected; }
  static bool CheckCtorDtorCount() { return TestObject::ctor_count_ == TestObject::dtor_count_; }
};

struct UniquePtrTraits {
  using ItemType = std::unique_ptr<TestObject>;

  static ItemType Create(ValueType val) {
    fbl::AllocChecker ac;
    ItemType ptr(new (&ac) TestObject(val));
    ZX_ASSERT(ac.check());
    return ptr;
  }
  static ValueType GetValue(const ItemType& c) { return c->value(); }
  static bool CheckLiveCount(size_t expected) { return TestObject::live_obj_count() == expected; }
  static bool CheckCtorDtorCount() { return TestObject::ctor_count_ == TestObject::dtor_count_; }
};

template <typename T>
struct RefCountedItem : public fbl::RefCounted<RefCountedItem<T>> {
  RefCountedItem(T v) : val(std::move(v)) {}
  DISALLOW_COPY_ASSIGN_AND_MOVE(RefCountedItem);
  T val;
};

struct RefPtrTraits {
  using ItemType = fbl::RefPtr<RefCountedItem<TestObject>>;

  static ItemType Create(ValueType val) {
    fbl::AllocChecker ac;
    auto ptr = AdoptRef(new (&ac) RefCountedItem<TestObject>(TestObject(val)));
    ZX_ASSERT(ac.check());
    return ptr;
  }
  static ValueType GetValue(const ItemType& c) { return c->val.value(); }
  static bool CheckLiveCount(size_t expected) { return TestObject::live_obj_count() == expected; }
  static bool CheckCtorDtorCount() { return TestObject::ctor_count_ == TestObject::dtor_count_; }
};

// Helper classes

template <typename ItemTraits>
struct Generator {
  using ItemType = typename ItemTraits::ItemType;

  constexpr static ValueType seed = 0xa2328b73e323fd0f;
  ValueType NextValue() { return key_lfsr_.GetNext(); }
  ItemType NextItem() { return ItemTraits::Create(NextValue()); }
  void Reset() { key_lfsr_.SetCore(seed); }
  fbl::tests::Lfsr<ValueType> key_lfsr_{seed};
};

struct TestAllocatorTraits : public fbl::DefaultAllocatorTraits {
  static void* Allocate(size_t size) {
    void* result = DefaultAllocatorTraits::Allocate(size);
    // Intentionally fill the allocated portion of memory
    // with non-zero data to break the assumption that
    // the heap will return "clean" data. This helps
    // catch bugs where the vector might call move assignment
    // operators into portions of uninitialized memory.
    if (result) {
      memset(result, 'f', size);
    }
    return result;
  }
};

// Actual tests

template <typename ItemTraits>
void AccessRelease(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  Generator<ItemTraits> gen;
  // Create the vector, verify its contents
  {
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    fbl::AllocChecker ac;
    vector.reserve(size, &ac);
    ASSERT_TRUE(ac.check());
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      vector.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));

    gen.Reset();
    ItemType* data = vector.data();
    for (size_t i = 0; i < size; i++) {
      auto base = gen.NextValue();
      // Verify the contents using the [] operator
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), base);
      // Verify the contents using the underlying array
      ASSERT_EQ(ItemTraits::GetValue(data[i]), base);
    }

    // Release the vector's underlying array before it is destructed
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    vector.reset();
    ASSERT_EQ(vector.size(), 0);
    ASSERT_EQ(vector.capacity(), 0);
    ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

struct CountedAllocatorTraits : public TestAllocatorTraits {
  static void* Allocate(size_t size) {
    allocation_count++;
    return TestAllocatorTraits::Allocate(size);
  }
  static size_t allocation_count;
};

size_t CountedAllocatorTraits::allocation_count = 0;

template <typename ItemTraits>
void PushBackInCapacity(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  CountedAllocatorTraits::allocation_count = 0;
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    fbl::Vector<ItemType, CountedAllocatorTraits> vector;
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 0);
    fbl::AllocChecker ac;
    vector.reserve(size, &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 1);

    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      vector.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 1);

    gen.Reset();
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void PushBackByConstRefInCapacity(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  CountedAllocatorTraits::allocation_count = 0;
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    fbl::Vector<ItemType, CountedAllocatorTraits> vector;
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 0);
    fbl::AllocChecker ac;
    vector.reserve(size, &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 1);

    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      const ItemType item = gen.NextItem();
      vector.push_back(item, &ac);
      ASSERT_TRUE(ac.check());
    }
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 1);

    gen.Reset();
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void PushBackBeyondCapacity(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    // Create an empty vector, push back beyond its capacity
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      fbl::AllocChecker ac;
      vector.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void PushBackByConstRefBeyondCapacity(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    // Create an empty vector, push back beyond its capacity
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      const ItemType item = gen.NextItem();
      fbl::AllocChecker ac;
      vector.push_back(item, &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void PopBack(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    // Create a vector filled with objects
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      fbl::AllocChecker ac;
      vector.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }

    // Pop one at a time, and check the vector is still valid
    while (vector.size()) {
      vector.pop_back();
      ASSERT_TRUE(ItemTraits::CheckLiveCount(vector.size()));
      gen.Reset();
      for (size_t i = 0; i < vector.size(); i++) {
        ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
      }
    }

    ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

struct FailingAllocatorTraits {
  static void* Allocate(size_t size) { return nullptr; }
  static void Deallocate(void* object) { return; }
};

// Allocator which will fail allocations over a given size "S".
//
// Thread-unsafe: unprotected static variables in use.
class PartiallyFailingAllocatorTraits : public fbl::DefaultAllocatorTraits {
 public:
  static void* Allocate(size_t size) {
    if (size <= failure_threshold_) {
      return DefaultAllocatorTraits::Allocate(size);
    }
    return nullptr;
  }

  // Fail all allocations exceeding size "s".
  static void SetFailureThreshold(size_t s) { failure_threshold_ = s; }

 private:
  static size_t failure_threshold_;
};

size_t PartiallyFailingAllocatorTraits::failure_threshold_;

template <typename ItemTraits>
void AllocationFailure(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  // Test that a failing allocator cannot take on additional elements
  {
    fbl::Vector<ItemType, FailingAllocatorTraits> vector;
    fbl::AllocChecker ac;
    vector.reserve(0, &ac);
    ASSERT_TRUE(ac.check());
    vector.reserve(1, &ac);
    ASSERT_FALSE(ac.check());
    vector.reserve(size, &ac);
    ASSERT_FALSE(ac.check());

    ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
    vector.push_back(gen.NextItem(), &ac);
    ASSERT_FALSE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  // Test that a partially failing allocator stops taking on additional
  // elements
  {
    PartiallyFailingAllocatorTraits::SetFailureThreshold(size * sizeof(ItemType));
    fbl::Vector<ItemType, PartiallyFailingAllocatorTraits> vector;
    fbl::AllocChecker ac;
    vector.reserve(0, &ac);
    ASSERT_TRUE(ac.check());
    vector.reserve(1, &ac);
    ASSERT_TRUE(ac.check());
    vector.reserve(size, &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_EQ(vector.capacity(), size);

    ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
    gen.Reset();
    while (vector.size() < size) {
      vector.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
      ASSERT_TRUE(ItemTraits::CheckLiveCount(vector.size()));
    }
    vector.push_back(gen.NextItem(), &ac);
    ASSERT_FALSE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    ASSERT_EQ(vector.size(), size);
    ASSERT_EQ(vector.capacity(), size);

    // All the elements we were able to push back should still be present
    gen.Reset();
    for (size_t i = 0; i < vector.size(); i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void Move(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  // Test move constructor
  {
    fbl::Vector<ItemType, TestAllocatorTraits> vectorA;
    ASSERT_TRUE(vectorA.is_empty());
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      fbl::AllocChecker ac;
      vectorA.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();
    ASSERT_FALSE(vectorA.is_empty());
    ASSERT_EQ(vectorA.size(), size);
    fbl::Vector<ItemType, TestAllocatorTraits> vectorB(std::move(vectorA));
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    ASSERT_TRUE(vectorA.is_empty());
    ASSERT_EQ(vectorA.size(), 0);
    ASSERT_EQ(vectorB.size(), size);
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vectorB[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  // Test move assignment operator
  {
    gen.Reset();
    fbl::Vector<ItemType, TestAllocatorTraits> vectorA;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      fbl::AllocChecker ac;
      vectorA.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();
    ASSERT_EQ(vectorA.size(), size);
    fbl::Vector<ItemType, TestAllocatorTraits> vectorB;
    vectorB = std::move(vectorA);
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    ASSERT_EQ(vectorA.size(), 0);
    ASSERT_EQ(vectorB.size(), size);
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vectorB[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void Swap(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    fbl::Vector<ItemType, TestAllocatorTraits> vectorA;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      fbl::AllocChecker ac;
      vectorA.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }
    fbl::Vector<ItemType, TestAllocatorTraits> vectorB;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(size + i));
      fbl::AllocChecker ac;
      vectorB.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();

    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vectorA[i]), gen.NextValue());
    }
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vectorB[i]), gen.NextValue());
    }

    ASSERT_TRUE(ItemTraits::CheckLiveCount(size * 2));
    vectorA.swap(vectorB);
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size * 2));

    gen.Reset();

    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vectorB[i]), gen.NextValue());
    }
    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vectorA[i]), gen.NextValue());
    }
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void Iterator(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  {
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      fbl::AllocChecker ac;
      vector.push_back(gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    gen.Reset();
    for (auto& e : vector) {
      auto base = gen.NextValue();
      ASSERT_EQ(ItemTraits::GetValue(e), base);
      // Take the element out, and put it back... just to check
      // that we can.
      auto other = std::move(e);
      e = std::move(other);
      ASSERT_EQ(ItemTraits::GetValue(e), base);
    }

    gen.Reset();
    const auto* cvector = &vector;
    for (const auto& e : *cvector) {
      ASSERT_EQ(ItemTraits::GetValue(e), gen.NextValue());
    }
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void Resize(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  CountedAllocatorTraits::allocation_count = 0;
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
  {
    fbl::AllocChecker ac;
    fbl::Vector<ItemType, CountedAllocatorTraits> vector;
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 0);
    vector.resize(size, &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 1);
    ASSERT_EQ(vector.size(), size);

    for (size_t i = 0; i < size; i++) {
      ASSERT_EQ(vector[i], ItemType());
    }

    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));

    for (size_t i = 0; i < size; i++) {
      const ItemType item = gen.NextItem();
      vector[i] = item;
      ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    }

    {
      const ItemType item = gen.NextItem();
      vector.resize(size + 10, item, &ac);
    }
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 10));
    ASSERT_EQ(vector.size(), size + 10);

    vector.resize(size + 1, &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 1));
    ASSERT_EQ(vector.size(), size + 1);

    {
      const ItemType item = gen.NextItem();
      vector.resize(size + 40, item, &ac);
    }
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 40));
    ASSERT_EQ(vector.size(), size + 40);

    vector.resize(size + 2, &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));
    ASSERT_EQ(vector.size(), size + 2);

    gen.Reset();
    for (size_t i = 0; i < size + 2; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));
  }

  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void InsertDelete(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  {
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    fbl::AllocChecker ac;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      vector.insert(i, gen.NextItem(), &ac);
      ASSERT_TRUE(ac.check());
    }

    // Insert at position zero and one
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    vector.insert(0, gen.NextItem(), &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 1));
    vector.insert(1, gen.NextItem(), &ac);
    ASSERT_TRUE(ac.check());
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));
    gen.Reset();

    // Verify the contents
    for (size_t i = 2; i < size + 2; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_EQ(ItemTraits::GetValue(vector[0]), gen.NextValue());
    ASSERT_EQ(ItemTraits::GetValue(vector[1]), gen.NextValue());
    gen.Reset();

    {
      // Erase from position one
      ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));
      auto erasedval1 = vector.erase(1);
      // Erase from position zero
      ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));
      auto erasedval0 = vector.erase(0);
      ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));

      // Verify the remaining contents
      for (size_t i = 0; i < size; i++) {
        ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
      }
      ASSERT_EQ(ItemTraits::GetValue(erasedval0), gen.NextValue());
      ASSERT_EQ(ItemTraits::GetValue(erasedval1), gen.NextValue());
      ASSERT_TRUE(ItemTraits::CheckLiveCount(size + 2));
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(size));
    gen.Reset();

    // Erase the remainder of the vector
    for (size_t i = 0; i < size; i++) {
      vector.erase(0);
    }
    ASSERT_EQ(vector.size(), 0);
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void NoAllocCheck(size_t size) {
  using ItemType = typename ItemTraits::ItemType;

  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  {
    Generator<ItemTraits> gen;
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      vector.push_back(gen.NextItem());
    }
    gen.Reset();
    for (auto& e : vector) {
      auto base = gen.NextValue();
      ASSERT_EQ(ItemTraits::GetValue(e), base);
    }
  }

  {
    Generator<ItemTraits> gen;
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    vector.reserve(size);
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      vector.push_back(gen.NextItem());
    }
    gen.Reset();
    for (auto& e : vector) {
      auto base = gen.NextValue();
      ASSERT_EQ(ItemTraits::GetValue(e), base);
    }
  }

  {
    Generator<ItemTraits> gen;
    fbl::Vector<ItemType, TestAllocatorTraits> vector;
    for (size_t i = 0; i < size; i++) {
      ASSERT_TRUE(ItemTraits::CheckLiveCount(i));
      vector.insert(i, gen.NextItem());
    }
    gen.Reset();
    for (auto& e : vector) {
      auto base = gen.NextValue();
      ASSERT_EQ(ItemTraits::GetValue(e), base);
    }
  }

  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

template <typename ItemTraits>
void InitializerList() {
  using ItemType = typename ItemTraits::ItemType;

  Generator<ItemTraits> gen;

  CountedAllocatorTraits::allocation_count = 0;
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());

  // empty
  {
    fbl::Vector<ItemType, CountedAllocatorTraits> vector{};
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 0);
    ASSERT_EQ(0, vector.size());
    ASSERT_EQ(0, vector.capacity());
  }

  // 5 items
  {
    fbl::Vector<ItemType, CountedAllocatorTraits> vector{
        gen.NextItem(), gen.NextItem(), gen.NextItem(), gen.NextItem(), gen.NextItem()};
    ASSERT_EQ(CountedAllocatorTraits::allocation_count, 1);
    ASSERT_EQ(5, vector.size());
    ASSERT_EQ(5, vector.capacity());

    gen.Reset();
    for (size_t i = 0; i < 5; i++) {
      ASSERT_EQ(ItemTraits::GetValue(vector[i]), gen.NextValue());
    }
    ASSERT_TRUE(ItemTraits::CheckLiveCount(5));
  }
  ASSERT_TRUE(ItemTraits::CheckLiveCount(0));
  ASSERT_TRUE(ItemTraits::CheckCtorDtorCount());
}

TEST(VectorTest, ImplicitConversion) {
  {
    fbl::Vector<fbl::String> v;
    v.push_back(fbl::String("First"));
    v.push_back("Second");
    v.insert(2, fbl::String("Third"));
    v.insert(3, "Fourth");

    ASSERT_EQ(strcmp(v[0].c_str(), "First"), 0);
    ASSERT_EQ(strcmp(v[1].c_str(), "Second"), 0);
    ASSERT_EQ(strcmp(v[2].c_str(), "Third"), 0);
    ASSERT_EQ(strcmp(v[3].c_str(), "Fourth"), 0);
  }

  {
    fbl::Vector<fbl::String> v;
    fbl::AllocChecker ac;
    v.push_back(fbl::String("First"), &ac);
    ASSERT_TRUE(ac.check());
    v.push_back("Second", &ac);
    ASSERT_TRUE(ac.check());
    v.insert(2, fbl::String("Third"), &ac);
    ASSERT_TRUE(ac.check());
    v.insert(3, "Fourth", &ac);
    ASSERT_TRUE(ac.check());

    ASSERT_EQ(strcmp(v[0].c_str(), "First"), 0);
    ASSERT_EQ(strcmp(v[1].c_str(), "Second"), 0);
    ASSERT_EQ(strcmp(v[2].c_str(), "Third"), 0);
    ASSERT_EQ(strcmp(v[3].c_str(), "Fourth"), 0);
  }
}

TEST(VectorTest, VectorDataConstness) {
  fbl::Vector<int> vector_int;

  auto& ref_vector_int = vector_int;
  const auto& const_ref_vector_int = vector_int;

  auto int_ptr = ref_vector_int.data();
  auto const_int_ptr = const_ref_vector_int.data();

  static_assert(!std::is_const_v<std::remove_pointer_t<decltype(int_ptr)>>);
  static_assert(std::is_const_v<std::remove_pointer_t<decltype(const_int_ptr)>>);
}

// The set of sizes we run each vector test with.
const size_t kTestSizes[] = {1, 2, 10, 32, 64, 100};

#define RUN_TRAIT_TEST(test_base, test_trait)  \
  TEST(VectorTest, test_base##_##test_trait) { \
    auto fn = test_base<test_trait>;           \
    for (size_t size : kTestSizes) {           \
      ASSERT_NO_FAILURES(fn(size));            \
    }                                          \
  }

#define RUN_FOR_ALL_TRAITS(test_base)         \
  RUN_TRAIT_TEST(test_base, ValueTypeTraits)  \
  RUN_TRAIT_TEST(test_base, StructTypeTraits) \
  RUN_TRAIT_TEST(test_base, UniquePtrTraits)  \
  RUN_TRAIT_TEST(test_base, RefPtrTraits)

#define RUN_FOR_ALL(test_base) RUN_FOR_ALL_TRAITS(test_base)

RUN_FOR_ALL(AccessRelease)
RUN_FOR_ALL(PushBackInCapacity)
RUN_TRAIT_TEST(PushBackByConstRefInCapacity, ValueTypeTraits)
RUN_FOR_ALL(PushBackBeyondCapacity)
RUN_TRAIT_TEST(PushBackByConstRefBeyondCapacity, ValueTypeTraits)
RUN_FOR_ALL(PopBack)
RUN_FOR_ALL(AllocationFailure)
RUN_FOR_ALL(Move)
RUN_FOR_ALL(Swap)
RUN_FOR_ALL(Iterator)
RUN_FOR_ALL(InsertDelete)
RUN_TRAIT_TEST(Resize, ValueTypeTraits)
RUN_FOR_ALL(NoAllocCheck)

TEST(VectorTest, InitializerListValueType) {
  ASSERT_NO_FAILURES(InitializerList<ValueTypeTraits>());
}

TEST(VectorTest, InitializerListRefPtr) { ASSERT_NO_FAILURES(InitializerList<RefPtrTraits>()); }

TEST(VectorTest, ImplicitlyConvertibleToSpan) {
  fbl::Vector<int> vector_int = {1, 2, 3, 4, 5};

  cpp20::span<int> vector_span(vector_int);
  EXPECT_EQ(vector_int.size(), vector_span.size());
  EXPECT_EQ(vector_int.data(), vector_span.data());
}

TEST(VectorTest, ImplicitlyConvertibleToConstSpan) {
  const fbl::Vector<int> vector_int = {1, 2, 3, 4, 5};

  cpp20::span<const int> vector_span(vector_int);
  EXPECT_EQ(vector_int.size(), vector_span.size());
  EXPECT_EQ(vector_int.data(), vector_span.data());
}

}  // namespace
