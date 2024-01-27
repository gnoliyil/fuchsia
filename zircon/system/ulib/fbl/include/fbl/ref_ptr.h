// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FBL_REF_PTR_H_
#define FBL_REF_PTR_H_

#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <type_traits>
#include <utility>

#include <fbl/alloc_checker.h>
#include <fbl/recycler.h>

namespace fbl {

template <typename T>
class RefPtr;

template <typename T>
RefPtr<T> AdoptRef(T* ptr);

template <typename T>
RefPtr<T> ImportFromRawPtr(T*);

namespace internal {
template <typename T>
RefPtr<T> MakeRefPtrNoAdopt(T* ptr);

template <typename T>
T* LeakToRawPtr(RefPtr<T>* ptr) __WARN_UNUSED_RESULT;
}  // namespace internal

// RefPtr<T> holds a reference to an intrusively-refcounted object of type
// T that deletes the object when the refcount drops to 0.
//
// T should be a subclass of fbl::RefCounted<>, or something that adheres to
// the same contract for AddRef() and Release().
//
// Except for initial construction (see below), this generally adheres to a
// subset of the interface for std::shared_ptr<>. Unlike std::shared_ptr<> this
// type does not support vending weak pointers, introspecting the reference
// count, or any operations that would result in allocating memory (unless
// T::AddRef or T::Release allocate memory).
// TODO(fxbug.dev/65796): Align RefPtr more closely with standard library smart
// pointers.
//
// Construction:  To create a RefPtr around a freshly created object, use the
// AdoptRef free function at the bottom of this header. To construct a RefPtr
// to hold a reference to an object that already exists use the copy or move
// constructor or assignment operator.
template <typename T>
class RefPtr final {
 public:
  using element_type = T;

  // Constructors
  constexpr RefPtr() : ptr_(nullptr) {}
  constexpr RefPtr(decltype(nullptr)) : RefPtr() {}
  // Constructs a RefPtr from a pointer that has already been adopted. See
  // AdoptRef() below for constructing the very first RefPtr to an object.
  explicit RefPtr(T* p) : ptr_(p) {
    if (ptr_)
      ptr_->AddRef();
  }

  // Copy construction.
  RefPtr(const RefPtr& r) : RefPtr(r.ptr_) {}

  // Implicit upcast via copy construction.
  //
  // @see the notes in unique_ptr.h
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  RefPtr(const RefPtr<U>& r) : RefPtr(r.ptr_) {
    static_assert((std::is_class_v<T> == std::is_class_v<U>)&&(!std::is_class_v<T> ||
                                                               std::has_virtual_destructor_v<T> ||
                                                               std::is_same_v<T, const U>),
                  "Cannot convert RefPtr<U> to RefPtr<T> unless neither T "
                  "nor U are class/struct types, or T has a virtual destructor,"
                  "or T == const U.");
  }

  // Assignment
  RefPtr& operator=(const RefPtr& r) {
    // Ref first so self-assignments work.
    if (r.ptr_) {
      r.ptr_->AddRef();
    }
    T* old = ptr_;
    ptr_ = r.ptr_;
    if (old && old->Release()) {
      recycle(old);
    }
    return *this;
  }

  // Move construction
  RefPtr(RefPtr&& r) : ptr_(r.ptr_) { r.ptr_ = nullptr; }

  // Implicit upcast via move construction.
  //
  // @see the notes in RefPtr.h
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  RefPtr(RefPtr<U>&& r) : ptr_(r.ptr_) {
    static_assert((std::is_class_v<T> == std::is_class_v<U>)&&(!std::is_class_v<T> ||
                                                               std::has_virtual_destructor_v<T> ||
                                                               std::is_same_v<T, const U>),
                  "Cannot convert RefPtr<U> to RefPtr<T> unless neither T "
                  "nor U are class/struct types, or T has a virtual destructor,"
                  "or T == const U");

    r.ptr_ = nullptr;
  }

  // Move assignment
  RefPtr& operator=(RefPtr&& r) {
    RefPtr(std::move(r)).swap(*this);
    return *this;
  }

  // Construct via explicit downcast.
  // ptr must be the same object as base.ptr_.
  template <typename BaseType>
  RefPtr(T* ptr, RefPtr<BaseType>&& base) : ptr_(ptr) {
    ZX_ASSERT(static_cast<BaseType*>(ptr_) == base.ptr_);
    base.ptr_ = nullptr;
  }

  // Downcast via static method invocation.  Depending on use case, the syntax
  // should look something like...
  //
  // fbl::RefPtr<MyBase> foo = MakeBase();
  // auto bar_copy = fbl::RefPtr<MyDerived>::Downcast(foo);
  // auto bar_move = fbl::RefPtr<MyDerived>::Downcast(std::move(foo));
  //
  template <typename BaseRefPtr>
  static RefPtr Downcast(BaseRefPtr base) {
    // Make certain that BaseRefPtr is some form of RefPtr<T>
    static_assert(std::is_same_v<BaseRefPtr, RefPtr<typename BaseRefPtr::element_type>>,
                  "BaseRefPtr must be a RefPtr<T>!");

    if (base != nullptr)
      return ImportFromRawPtr<T>(static_cast<T*>(internal::LeakToRawPtr(&base)));

    return nullptr;
  }

  ~RefPtr() {
    T* ptr = ptr_;
    // Clear ptr_ to help detect re-entrancy in ~T.
    ptr_ = nullptr;
    if (ptr && ptr->Release()) {
      recycle(ptr);
    }
  }

  void reset(T* ptr = nullptr) { RefPtr(ptr).swap(*this); }

  void swap(RefPtr& r) {
    T* p = ptr_;
    ptr_ = r.ptr_;
    r.ptr_ = p;
  }

  T* get() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return !!ptr_; }

  // Comparison against nullptr operators (of the form, myptr == nullptr).
  bool operator==(decltype(nullptr)) const { return (ptr_ == nullptr); }
  bool operator!=(decltype(nullptr)) const { return (ptr_ != nullptr); }

  bool operator==(const RefPtr<T>& other) const { return ptr_ == other.ptr_; }
  bool operator!=(const RefPtr<T>& other) const { return ptr_ != other.ptr_; }

 private:
  template <typename U>
  friend class RefPtr;
  friend RefPtr<T> AdoptRef<T>(T*);
  friend RefPtr<T> ImportFromRawPtr<>(T*);
  friend RefPtr<T> internal::MakeRefPtrNoAdopt<>(T* ptr);
  friend T* internal::LeakToRawPtr<>(RefPtr<T>*);

  enum AdoptTag { ADOPT };
  enum NoAdoptTag { NO_ADOPT };

  RefPtr(T* ptr, AdoptTag) : ptr_(ptr) {
    if (ptr_) {
      ptr_->Adopt();
    }
  }

  RefPtr(T* ptr, NoAdoptTag) : ptr_(ptr) {}

  static void recycle(T* ptr) {
    if constexpr (::fbl::internal::has_fbl_recycle_v<T>) {
      ::fbl::internal::recycler<T>::recycle(ptr);
    } else {
      delete ptr;
    }
  }

  T* ptr_;
};

// Comparison against nullptr operator (of the form, nullptr == myptr)
template <typename T>
static inline bool operator==(decltype(nullptr), const RefPtr<T>& ptr) {
  return (ptr.get() == nullptr);
}

template <typename T>
static inline bool operator!=(decltype(nullptr), const RefPtr<T>& ptr) {
  return (ptr.get() != nullptr);
}

// Constructs a RefPtr from a fresh object that has not been referenced before.
// Use like:
//
//   RefPtr<Happy> h = AdoptRef(new Happy);
//   if (!h)
//      // Deal with allocation failure here
//   h->DoStuff();
template <typename T>
inline RefPtr<T> AdoptRef(T* ptr) {
  return RefPtr<T>(ptr, RefPtr<T>::ADOPT);
}

// Export a pointer from a smart pointer to a raw pointer without modifying its
// reference count. The caller is responsible for maintaining the reference
// count, likely by calling ImportFromRawPtr() later on.
//
// Use this to store a pointer in code that can't use the C++
// type directly, such as in pure C code.
template <typename T>
inline T* ExportToRawPtr(RefPtr<T>* ptr) {
  return internal::LeakToRawPtr(ptr);
}

// Imports from a raw pointer to a RefPtr. Does not modify the reference count
// of the object. This should be used on values that have previously been
// exported with ExportToRawPtr().
template <typename T>
inline RefPtr<T> ImportFromRawPtr(T* ptr) {
  return internal::MakeRefPtrNoAdopt(ptr);
}

namespace internal {
// Constructs a RefPtr from a T* without attempt to either AddRef or Adopt the
// pointer.  Used by the internals of some intrusive container classes to store
// sentinels (special invalid pointers) in RefPtr<>s.
template <typename T>
inline RefPtr<T> MakeRefPtrNoAdopt(T* ptr) {
  return RefPtr<T>(ptr, RefPtr<T>::NO_ADOPT);
}

// Leaks the internal value to a raw pointer and resets the RefPtr to null.
// Does not change the reference count of the object.
template <typename T>
inline T* LeakToRawPtr(RefPtr<T>* ptr) {
  T* ret = ptr->ptr_;
  ptr->ptr_ = nullptr;
  return ret;
}

// This is a wrapper class that can be friended for a particular |T|, if you
// want to make |T|'s constructor private, but still use |MakeRefCounted()|
// (below). (You can't friend partial specializations.)
template <typename T>
class MakeRefCountedHelper final {
 public:
  template <typename... Args>
  static RefPtr<T> MakeRefCounted(Args&&... args) {
    return AdoptRef<T>(new T(std::forward<Args>(args)...));
  }

  template <typename... Args>
  static RefPtr<T> MakeRefCountedChecked(AllocChecker* ac, Args&&... args) {
    return AdoptRef<T>(new (ac) T(std::forward<Args>(args)...));
  }
};

}  // namespace internal

template <typename T, typename... Args>
RefPtr<T> MakeRefCounted(Args&&... args) {
  return internal::MakeRefCountedHelper<T>::MakeRefCounted(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
RefPtr<T> MakeRefCountedChecked(AllocChecker* ac, Args&&... args) {
  return internal::MakeRefCountedHelper<T>::MakeRefCountedChecked(ac, std::forward<Args>(args)...);
}

}  // namespace fbl

#endif  // FBL_REF_PTR_H_
