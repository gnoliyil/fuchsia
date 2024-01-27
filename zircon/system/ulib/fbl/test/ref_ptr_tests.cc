// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <utility>

#include <fbl/alloc_checker.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <zxtest/zxtest.h>

namespace {

class RefCallCounter {
 public:
  RefCallCounter() = default;
  ~RefCallCounter() { destroy_calls_++; }

  void AddRef() { add_ref_calls_++; }
  bool Release() {
    release_calls_++;
    return add_ref_calls_ == release_calls_;
  }
  void Adopt() {}

  static void ResetStatics() { destroy_calls_ = 0; }

  int add_ref_calls() const { return add_ref_calls_; }
  int release_calls() const { return release_calls_; }

  static int destroy_calls() { return destroy_calls_; }
  static void operator delete(void* ptr) {}

 private:
  int add_ref_calls_{0};
  int release_calls_{0};

  static inline int destroy_calls_{0};
};

static_assert(std::is_standard_layout_v<fbl::RefPtr<RefCallCounter>>,
              "fbl::RefPtr<T>'s should have a standard layout.");

class RefPtrTest : public zxtest::Test {
 public:
  RefPtrTest() = default;
  virtual ~RefPtrTest() = default;
  void SetUp() override { RefCallCounter::ResetStatics(); }
};

TEST_F(RefPtrTest, Basic) {
  using RefCallPtr = fbl::RefPtr<RefCallCounter>;

  RefCallCounter counter;
  RefCallPtr ptr = fbl::AdoptRef<RefCallCounter>(&counter);

  EXPECT_TRUE(&counter == ptr.get(), ".get() should point to object");
  EXPECT_TRUE(static_cast<bool>(ptr), "operator bool");
  EXPECT_TRUE(&counter == &(*ptr), "operator*");

  // Adoption should not manipulate the refcount.
  EXPECT_EQ(0, counter.add_ref_calls());
  EXPECT_EQ(0, counter.release_calls());
  EXPECT_EQ(0, RefCallCounter::destroy_calls());

  {
    RefCallPtr ptr2 = ptr;

    // Copying to a new RefPtr should call add once.
    EXPECT_EQ(1, counter.add_ref_calls());
    EXPECT_EQ(0, counter.release_calls());
    EXPECT_EQ(0, RefCallCounter::destroy_calls());
  }
  // Destroying the new RefPtr should release once.
  EXPECT_EQ(1, counter.add_ref_calls());
  EXPECT_EQ(1, counter.release_calls());
  EXPECT_EQ(1, RefCallCounter::destroy_calls());

  {
    RefCallPtr ptr2;

    EXPECT_TRUE(!static_cast<bool>(ptr2));

    ptr.swap(ptr2);

    // Swapping shouldn't cause any add or release calls, but should update
    // values.
    EXPECT_EQ(1, counter.add_ref_calls());
    EXPECT_EQ(1, counter.release_calls());
    EXPECT_EQ(1, RefCallCounter::destroy_calls());

    EXPECT_TRUE(!static_cast<bool>(ptr));
    EXPECT_TRUE(&counter == ptr2.get());

    ptr2.swap(ptr);
  }

  EXPECT_EQ(1, counter.add_ref_calls());
  EXPECT_EQ(1, counter.release_calls());
  EXPECT_EQ(1, RefCallCounter::destroy_calls());

  {
    RefCallPtr ptr2 = std::move(ptr);

    // Moving shouldn't cause any add or release but should update values.
    EXPECT_EQ(1, counter.add_ref_calls());
    EXPECT_EQ(1, counter.release_calls());
    EXPECT_EQ(1, RefCallCounter::destroy_calls());

    EXPECT_FALSE(static_cast<bool>(ptr));
    EXPECT_TRUE(&counter == ptr2.get());

    ptr2.swap(ptr);
  }

  // Reset should calls release and clear out the pointer.
  ptr.reset(nullptr);
  EXPECT_EQ(1, counter.add_ref_calls());
  EXPECT_EQ(2, counter.release_calls());
  EXPECT_EQ(1, RefCallCounter::destroy_calls());
  EXPECT_FALSE(static_cast<bool>(ptr));
  EXPECT_FALSE(ptr.get());
}

TEST_F(RefPtrTest, Compare) {
  using RefCallPtr = fbl::RefPtr<RefCallCounter>;

  RefCallCounter obj1, obj2;

  RefCallPtr ptr1 = fbl::AdoptRef<RefCallCounter>(&obj1);
  RefCallPtr ptr2 = fbl::AdoptRef<RefCallCounter>(&obj2);
  RefCallPtr also_ptr1 = ptr1;
  RefCallPtr null_ref_ptr;

  EXPECT_TRUE(ptr1 == ptr1);
  EXPECT_FALSE(ptr1 != ptr1);

  EXPECT_FALSE(ptr1 == ptr2);
  EXPECT_TRUE(ptr1 != ptr2);

  EXPECT_TRUE(ptr1 == also_ptr1);
  EXPECT_FALSE(ptr1 != also_ptr1);

  EXPECT_TRUE(ptr1 != null_ref_ptr);
  EXPECT_TRUE(ptr1 != nullptr);
  EXPECT_TRUE(nullptr != ptr1);
  EXPECT_FALSE(ptr1 == null_ref_ptr);
  EXPECT_FALSE(ptr1 == nullptr);
  EXPECT_FALSE(nullptr == ptr1);

  EXPECT_TRUE(null_ref_ptr == nullptr);
  EXPECT_FALSE(null_ref_ptr != nullptr);
  EXPECT_TRUE(nullptr == null_ref_ptr);
  EXPECT_FALSE(nullptr != null_ref_ptr);
}

namespace upcasting {

class Stats {
 public:
  Stats() {}
  ~Stats() { destroy_count_++; }

  static void Reset() {
    adopt_calls_ = 0;
    add_ref_calls_ = 0;
    release_calls_ = 0;
    destroy_count_ = 0;
  }

  void Adopt() { adopt_calls_++; }

  void AddRef() {
    ref_count_++;
    add_ref_calls_++;
  }

  bool Release() {
    ref_count_--;
    release_calls_++;
    return (ref_count_ <= 0);
  }

  static uint32_t adopt_calls() { return adopt_calls_; }
  static uint32_t add_ref_calls() { return add_ref_calls_; }
  static uint32_t release_calls() { return release_calls_; }
  static uint32_t destroy_count() { return destroy_count_; }

 private:
  int ref_count_ = 1;
  static uint32_t adopt_calls_;
  static uint32_t add_ref_calls_;
  static uint32_t release_calls_;
  static uint32_t destroy_count_;
};

uint32_t Stats::adopt_calls_ = 0;
uint32_t Stats::add_ref_calls_ = 0;
uint32_t Stats::release_calls_ = 0;
uint32_t Stats::destroy_count_ = 0;

class A : public Stats {
 public:
  virtual ~A() { stuff = 0u; }

 private:
  volatile uint32_t stuff;
};

class B {
 public:
  ~B() { stuff = 1u; }

 private:
  volatile uint32_t stuff;
};

class C : public A, public B {
 public:
  ~C() {}
};

class D : public A {
 public:
  virtual ~D() { stuff = 2u; }

 private:
  volatile uint32_t stuff;
};

template <typename T>
struct custom_delete {
  inline void operator()(T* ptr) const {
    enum { type_must_be_complete = sizeof(T) };
    delete ptr;
  }
};

template <typename RefPtrType>
static void handoff_lvalue_fn(const RefPtrType& ptr) {
  EXPECT_NOT_NULL(ptr.get());
}

template <typename RefPtrType>
static void handoff_copy_fn(RefPtrType ptr) {
  EXPECT_NOT_NULL(ptr.get());
}

template <typename RefPtrType>
static void handoff_rvalue_fn(RefPtrType&& ptr) {
  EXPECT_NOT_NULL(ptr.get());
}

class OverloadTestHelper {
 public:
  enum class Result {
    None,
    ClassA,
    ClassB,
    ClassD,
  };

  void PassByCopy(fbl::RefPtr<A>) { result_ = Result::ClassA; }
  void PassByCopy(fbl::RefPtr<D>) { result_ = Result::ClassD; }

#if TEST_WILL_NOT_COMPILE || 0
  // Enabling this overload should cause the overload test to fail to compile
  // due to ambiguity (it does not know whether to cast fbl::RefPtr<C> to fbl::RefPtr<A>
  // or fbl::RefPtr<B>)
  void PassByCopy(fbl::RefPtr<B>) { result_ = Result::ClassB; }
#endif

  void PassByMove(fbl::RefPtr<A>&&) { result_ = Result::ClassA; }
  void PassByMove(fbl::RefPtr<D>&&) { result_ = Result::ClassD; }

#if TEST_WILL_NOT_COMPILE || 0
  // Enabling this overload should cause the overload test to fail to compile
  // due to ambiguity (it does not know whether to cast fbl::RefPtr<C> to fbl::RefPtr<A>
  // or fbl::RefPtr<B>)
  void PassByMove(fbl::RefPtr<B>&&) { result_ = Result::ClassB; }
#endif

  Result result() const { return result_; }

 private:
  Result result_ = Result::None;
};

template <typename Base, typename Derived>
void DoUpcastTest() {
  fbl::AllocChecker ac;

  fbl::RefPtr<Derived> derived_ptr;

  // Construct RefPtr<Base> with a copy and implicit cast
  Stats::Reset();
  derived_ptr = fbl::AdoptRef<Derived>(new (&ac) Derived());
  ASSERT_TRUE(ac.check());
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(0, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    fbl::RefPtr<Base> base_ptr(derived_ptr);

    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_NOT_NULL(base_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  // Construct RefPtr<Base> with a move and implicit cast
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    fbl::RefPtr<Base> base_ptr(std::move(derived_ptr));

    EXPECT_NULL(derived_ptr);
    EXPECT_NOT_NULL(base_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  EXPECT_EQ(1, Stats::adopt_calls());
  EXPECT_EQ(1, Stats::add_ref_calls());
  EXPECT_EQ(2, Stats::release_calls());
  EXPECT_EQ(1, Stats::destroy_count());

  // Assign RefPtr<Base> at declaration time with a copy
  Stats::Reset();
  derived_ptr = fbl::AdoptRef<Derived>(new (&ac) Derived());
  ASSERT_TRUE(ac.check());
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(0, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    fbl::RefPtr<Base> base_ptr = derived_ptr;

    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_NOT_NULL(base_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  // Assign RefPtr<Base> at declaration time with a std::move
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    fbl::RefPtr<Base> base_ptr = std::move(derived_ptr);

    EXPECT_NULL(derived_ptr);
    EXPECT_NOT_NULL(base_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  EXPECT_EQ(1, Stats::adopt_calls());
  EXPECT_EQ(1, Stats::add_ref_calls());
  EXPECT_EQ(2, Stats::release_calls());
  EXPECT_EQ(1, Stats::destroy_count());

  // Assign RefPtr<Base> after declaration with a copy
  Stats::Reset();
  derived_ptr = fbl::AdoptRef<Derived>(new (&ac) Derived());
  ASSERT_TRUE(ac.check());
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(0, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    fbl::RefPtr<Base> base_ptr;
    base_ptr = derived_ptr;

    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_NOT_NULL(base_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  // Assign RefPtr<Base> after declaration with a std::move
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    fbl::RefPtr<Base> base_ptr;
    base_ptr = std::move(derived_ptr);

    EXPECT_NULL(derived_ptr);
    EXPECT_NOT_NULL(base_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  EXPECT_EQ(1, Stats::adopt_calls());
  EXPECT_EQ(1, Stats::add_ref_calls());
  EXPECT_EQ(2, Stats::release_calls());
  EXPECT_EQ(1, Stats::destroy_count());

  // Pass the pointer to a function as an lvalue reference with an implicit cast
  Stats::Reset();
  derived_ptr = fbl::AdoptRef<Derived>(new (&ac) Derived());
  ASSERT_TRUE(ac.check());
  {
    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(0, Stats::add_ref_calls());
    EXPECT_EQ(0, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());

    // Note: counter to intuition, we actually do expect this to bump the
    // reference count regardless of what the target function does with the
    // reference-to-pointer passed to it.  We are not passing a const
    // reference to a RefPtr<Derived>; instead we are creating a temp
    // RefPtr<Base> (which is where the addref happens) and then passing a
    // refernce to *that* to the function.
    auto fn = handoff_lvalue_fn<fbl::RefPtr<Base>>;
    ASSERT_NO_FAILURES(fn(derived_ptr));

    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(1, Stats::add_ref_calls());
    EXPECT_EQ(1, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  // Pass the pointer to a function with a copy and implicit cast
  {
    auto fn = handoff_copy_fn<fbl::RefPtr<Base>>;
    ASSERT_NO_FAILURES(fn(derived_ptr));

    EXPECT_NOT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(2, Stats::add_ref_calls());
    EXPECT_EQ(2, Stats::release_calls());
    EXPECT_EQ(0, Stats::destroy_count());
  }

  // Pass the pointer to a function as an rvalue reference and implicit cast
  {
    auto fn = handoff_rvalue_fn<fbl::RefPtr<Base>>;
    ASSERT_NO_FAILURES(fn(std::move(derived_ptr)));

    EXPECT_NULL(derived_ptr);
    EXPECT_EQ(1, Stats::adopt_calls());
    EXPECT_EQ(2, Stats::add_ref_calls());
    EXPECT_EQ(3, Stats::release_calls());
    EXPECT_EQ(1, Stats::destroy_count());
  }
}

TEST_F(RefPtrTest, Upcast) {
  // This should work.  C derives from A, A has a virtual destructor, and
  // everything is using the default deleter.
  auto do_test = DoUpcastTest<A, C>;
  ASSERT_NO_FAILURES(do_test());

#if TEST_WILL_NOT_COMPILE || 0
  // This should not work.  C derives from B, but B has no virtual destructor.
  auto do_test = DoUpcastTest<B, C>;
  ASSERT_NO_FAILURES(do_test());
#endif

#if TEST_WILL_NOT_COMPILE || 0
  // This should not work.  D has a virtual destructor, but it is not a base
  // class of C.
  auto do_test = DoUpcastTest<D, C>;
  ASSERT_NO_FAILURES(do_test());
#endif

  // Test overload resolution.  Make a C and the try to pass it to
  // OverloadTestHelper's various overloaded methods.  The compiler should
  // know which version to pick, and it should pick the RefPtr<A> version, not
  // the RefPtr<D> version.  If the TEST_WILL_NOT_COMPILE check is enabled in
  // OverloadTestHelper, a RefPtr<B> version will be enabled as well.  This
  // should cause the build to break because of ambiguity.
  fbl::AllocChecker ac;
  fbl::RefPtr<C> ptr = fbl::AdoptRef(new (&ac) C());
  ASSERT_TRUE(ac.check());

  {
    // Test pass by copy first (so we can reuse our object)
    OverloadTestHelper helper;
    helper.PassByCopy(ptr);

    ASSERT_NOT_NULL(ptr);
    EXPECT_EQ(OverloadTestHelper::Result::ClassA, helper.result());
  }

  {
    // Now test pass by move.
    OverloadTestHelper helper;
    helper.PassByMove(std::move(ptr));

    EXPECT_NULL(ptr);
    EXPECT_EQ(OverloadTestHelper::Result::ClassA, helper.result());
  }
}

}  // namespace upcasting

TEST_F(RefPtrTest, AdoptNull) {
  class C : public fbl::RefCounted<C> {};

  fbl::RefPtr<C> ptr = fbl::AdoptRef(static_cast<C*>(nullptr));
  EXPECT_NULL(ptr);
}

TEST_F(RefPtrTest, PtrToConst) {
  class C : public fbl::RefCounted<C> {
   public:
    explicit C(int x) : x_(x) {}

    int get_x() const { return x_; }

   private:
    int x_;
  };

  fbl::AllocChecker ac;
  fbl::RefPtr<C> refptr = fbl::AdoptRef<C>(new (&ac) C(23));
  ASSERT_TRUE(ac.check());

  // Copy a ref-ptr to a ref-ptr-to-const.
  fbl::RefPtr<const C> const_refptr = refptr;
  // refptr should have been copied, not moved.
  ASSERT_NOT_NULL(refptr.get());

  // Call a const member function on a ref-ptr-to-const.
  EXPECT_NOT_NULL(const_refptr.get());
  EXPECT_EQ(const_refptr->get_x(), 23);

  // Move a ref-ptr to a ref-ptr-to-const.
  fbl::RefPtr<const C> moved_const_refptr = std::move(refptr);
  ASSERT_NOT_NULL(moved_const_refptr.get());
  // Now refptr should have been nulled out.
  ASSERT_NULL(refptr.get());

  // Move a ref-ptr-to-const into another ref-ptr-to-const.
  const_refptr.reset();
  ASSERT_NULL(const_refptr);
  const_refptr = std::move(moved_const_refptr);
  ASSERT_NOT_NULL(const_refptr);
  ASSERT_NULL(moved_const_refptr);
}

TEST_F(RefPtrTest, MoveAssign) {
  using RefCallPtr = fbl::RefPtr<RefCallCounter>;

  RefCallCounter counter1, counter2;
  RefCallPtr ptr1 = fbl::AdoptRef<RefCallCounter>(&counter1);
  RefCallPtr ptr2 = fbl::AdoptRef<RefCallCounter>(&counter2);

  EXPECT_NE(ptr1.get(), ptr2.get());
  EXPECT_NOT_NULL(ptr1);
  EXPECT_NOT_NULL(ptr2);

  ptr1 = std::move(ptr2);
  EXPECT_EQ(ptr1.get(), &counter2);
  EXPECT_NULL(ptr2);

  EXPECT_EQ(1, counter1.release_calls());
  EXPECT_EQ(0, counter2.release_calls());

  // Test self-assignment
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
  ptr1 = std::move(ptr1);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  EXPECT_EQ(ptr1.get(), &counter2);
  EXPECT_EQ(0, counter2.release_calls());
}

}  // namespace
