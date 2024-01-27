// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FBL_TESTS_INTRUSIVE_CONTAINERS_TEST_THUNKS_H_
#define FBL_TESTS_INTRUSIVE_CONTAINERS_TEST_THUNKS_H_

#include <zxtest/zxtest.h>

namespace fbl {
namespace tests {
namespace intrusive_containers {

#define MAKE_TEST_THUNK(_test_name) \
  static void _test_name() {        \
    TestEnvironmentClass env;       \
    env._test_name();               \
    env.Reset();                    \
  }

// A utility class used to generate static test thunks for the various
// combinations of test environments and test object types.
template <typename TestEnvironmentClass>
struct TestThunks {
  // Generic tests
  MAKE_TEST_THUNK(Clear)
  MAKE_TEST_THUNK(ClearUnsafe)
  MAKE_TEST_THUNK(IsEmpty)
  MAKE_TEST_THUNK(Iterate)
  MAKE_TEST_THUNK(IterErase)
  MAKE_TEST_THUNK(DirectErase)
  MAKE_TEST_THUNK(ObjRemoveFromContainer)
  MAKE_TEST_THUNK(NodeRemoveFromContainer)
  MAKE_TEST_THUNK(GlobalRemoveFromContainer)
  MAKE_TEST_THUNK(MakeIterator)
  MAKE_TEST_THUNK(MaterializeIterator)
  MAKE_TEST_THUNK(ReverseIterate)
  MAKE_TEST_THUNK(ReverseIterErase)
  MAKE_TEST_THUNK(Swap)
  MAKE_TEST_THUNK(RvalueOps)
  MAKE_TEST_THUNK(Scope)
  MAKE_TEST_THUNK(TwoContainer)
  MAKE_TEST_THUNK(ThreeContainerHelper)
  MAKE_TEST_THUNK(IterCopyPointer)
  MAKE_TEST_THUNK(EraseIf)
  MAKE_TEST_THUNK(FindIf)

  // Sequence specific tests
  MAKE_TEST_THUNK(PushFront)
  MAKE_TEST_THUNK(PopFront)
  MAKE_TEST_THUNK(PushBack)
  MAKE_TEST_THUNK(PopBack)
  MAKE_TEST_THUNK(SeqIterate)
  MAKE_TEST_THUNK(SeqReverseIterate)
  MAKE_TEST_THUNK(EraseNext)
  MAKE_TEST_THUNK(InsertAfter)
  MAKE_TEST_THUNK(Insert)
  MAKE_TEST_THUNK(DirectInsert)
  MAKE_TEST_THUNK(Splice)
  MAKE_TEST_THUNK(SplitAfter)
  MAKE_TEST_THUNK(ReplaceIfCopy)
  MAKE_TEST_THUNK(ReplaceIfMove)
  MAKE_TEST_THUNK(ReplaceCopy)
  MAKE_TEST_THUNK(ReplaceMove)

  // Associative container specific tests
  MAKE_TEST_THUNK(InsertByKey)
  MAKE_TEST_THUNK(FindByKey)
  MAKE_TEST_THUNK(EraseByKey)
  MAKE_TEST_THUNK(InsertOrFind)
  MAKE_TEST_THUNK(InsertOrReplace)

  // Ordered Associative container specific tests
  MAKE_TEST_THUNK(OrderedIter)
  MAKE_TEST_THUNK(OrderedReverseIter)
  MAKE_TEST_THUNK(UpperBound)
  MAKE_TEST_THUNK(LowerBound)
};
#undef MAKE_TEST_THUNK

// Macros used to define test object types, test environments, and test thunk
// structs used for exercising various containers and managed/unmanaged pointer
// types.
#define DEFINE_TEST_OBJECT(_container_type, _ptr_type, _base_type)                      \
  class _ptr_type##_container_type##TestObj                                             \
      : public _base_type<_container_type##Traits<                                      \
            typename ptr_type::_ptr_type<_ptr_type##_container_type##TestObj>::type>> { \
   public:                                                                              \
    explicit _ptr_type##_container_type##TestObj(size_t val) : _base_type(val) {}       \
  };                                                                                    \
  using _ptr_type##_container_type##TestTraits =                                        \
      _ptr_type##TestTraits<_ptr_type##_container_type##TestObj>

// Macro which declare static storage for things like custom deleters for each
// tested container type.  If new static storage is needed for testing custom
// pointer type or custom deleters, it should be declared here.
#define DECLARE_TEST_STORAGE(_container_type)                                              \
  template <>                                                                              \
  std::atomic<size_t>                                                                      \
      TestCustomDeleter<UniquePtrCustomDeleter##_container_type##TestObj>::delete_count_ { \
    0                                                                                      \
  }

#define DEFINE_TEST_OBJECTS(_container_type)                             \
  DEFINE_TEST_OBJECT(_container_type, Unmanaged, TestObj);               \
  DEFINE_TEST_OBJECT(_container_type, UniquePtrDefaultDeleter, TestObj); \
  DEFINE_TEST_OBJECT(_container_type, UniquePtrCustomDeleter, TestObj);  \
  DEFINE_TEST_OBJECT(_container_type, RefPtr, RefedTestObj);             \
  DECLARE_TEST_STORAGE(_container_type)

#define DEFINE_TEST_THUNK(_env_type, _container_type, _ptr_type) \
  TestThunks<_env_type##ContainerTestEnvironment<_ptr_type##_container_type##TestTraits>>

// Create containers for the various flavors of pointers, and statically assert
// that their sizes are what we expect.  Most of the intrusive containers are
// structured to have a known size, and we don't want to run the risk that
// adding something like an instance of an empty trait class
// changes that size unexpectedly.
#define VERIFY_CONTAINER_SIZE(_container_type, _ptr_type, _expected_size)                          \
  static_assert(sizeof(_ptr_type##_container_type##TestTraits::ContainerType) == (_expected_size), \
                "Container \"" #_ptr_type #_container_type                                         \
                "\" has unexpected size!  It should be exactly \"" #_expected_size "\"")

#define VERIFY_CONTAINER_SIZES(_container_type, _expected_size)                    \
  VERIFY_CONTAINER_SIZE(_container_type, Unmanaged, _expected_size);               \
  VERIFY_CONTAINER_SIZE(_container_type, UniquePtrDefaultDeleter, _expected_size); \
  VERIFY_CONTAINER_SIZE(_container_type, UniquePtrCustomDeleter, _expected_size);  \
  VERIFY_CONTAINER_SIZE(_container_type, RefPtr, _expected_size)

#define RUN_ZXTEST(_group, _flavor, _test) \
  TEST(_group, _test##_##_flavor) {        \
    auto fn = _flavor ::_test;             \
    ASSERT_NO_FAILURES(fn());              \
  }

}  // namespace intrusive_containers
}  // namespace tests
}  // namespace fbl

#endif  // FBL_TESTS_INTRUSIVE_CONTAINERS_TEST_THUNKS_H_
