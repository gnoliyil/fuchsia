// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FBL_VECTOR_H_
#define FBL_VECTOR_H_

#include <new>
#include <string.h>

#include <fbl/alloc_checker.h>
#include <fbl/macros.h>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <zircon/assert.h>

namespace fbl {

namespace internal {

template <typename U>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<U>>;

}  // namespace internal

struct DefaultAllocatorTraits {
  // Allocate receives a request for "size" contiguous bytes.
  // size will always be greater than zero.
  // The return value must be "nullptr" on error, or a non-null
  // pointer on success. This same pointer may later be passed
  // to deallocate when resizing.
  static void* Allocate(size_t size) {
    ZX_DEBUG_ASSERT(size > 0);
    AllocChecker ac;
    void* object = new (&ac) char[size];
    return ac.check() ? object : nullptr;
  }

  // Deallocate receives a pointer to an object which is
  // 1) Either a pointer provided by Allocate, or
  // 2) nullptr.
  // If the pointer is not null, deallocate must free
  // the underlying memory used by the object.
  static void Deallocate(void* object) {
    if (object != nullptr) {
      delete[] reinterpret_cast<char*>(object);
    }
  }
};

template <typename T>
struct AlignedAllocatorTraits {
  static constexpr bool NeedsCustomAlignment() {
    return alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  }
  // Allocate receives a request for "size" contiguous bytes.
  // size will always be greater than zero.
  // The return value must be "nullptr" on error, or a non-null
  // pointer on success. This same pointer may later be passed
  // to deallocate when resizing.
  static void* Allocate(size_t size) {
    ZX_DEBUG_ASSERT(size > 0);
    AllocChecker ac;
    void* object;
    if (NeedsCustomAlignment()) {
      object = new (static_cast<std::align_val_t>(alignof(T)), &ac) char[size];
    } else {
      object = new (&ac) char[size];
    }
    return ac.check() ? object : nullptr;
  }

  // Deallocate receives a pointer to an object which is
  // 1) Either a pointer provided by Allocate, or
  // 2) nullptr.
  // If the pointer is not null, deallocate must free
  // the underlying memory used by the object.
  static void Deallocate(void* object) {
    if (object != nullptr) {
      delete[] reinterpret_cast<char*>(object);
    }
  }
};

// Vector<> is an implementation of a dynamic array, implementing
// a limited set of functionality of std::vector.
//
// Notably, Vector<> returns information about allocation failures,
// rather than throwing exceptions. Furthermore, Vector<> does
// not allow copying or insertions / deletions from anywhere except
// the end.
//
// This Vector supports O(1) indexing and O(1) (amortized) insertion and
// deletion at the end (due to possible reallocations during push_back
// and pop_back).
template <typename T, typename AllocatorTraits = AlignedAllocatorTraits<T>>
class __OWNER(T) Vector {
 public:
  // move semantics only
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Vector);

  constexpr Vector() : ptr_(nullptr), size_(0U), capacity_(0U) {}

  Vector(Vector&& other) : ptr_(nullptr), size_(other.size_), capacity_(other.capacity_) {
    ptr_ = other.release();
  }

#ifndef _KERNEL
  Vector(std::initializer_list<T> init)
      : ptr_(init.size() != 0U
                 ? reinterpret_cast<T*>(AllocatorTraits::Allocate(init.size() * sizeof(T)))
                 : nullptr),
        size_(init.size()),
        capacity_(size_) {
    T* out = ptr_;
    for (const T* in = init.begin(); in != init.end(); ++in, ++out) {
      new (out) T(*in);
    }
  }
#endif

  Vector& operator=(Vector&& o) {
    auto size = o.size_;
    auto capacity = o.capacity_;
    reset(o.release(), size, capacity);
    return *this;
  }

  size_t size() const { return size_; }

  size_t capacity() const { return capacity_; }

  ~Vector() { reset(); }

  // Reserve enough size to hold at least capacity elements.
  void reserve(size_t capacity, AllocChecker* ac) {
    if (capacity <= capacity_) {
      ac->arm(0U, true);
      return;
    }
    reallocate(capacity, ac);
  }

#ifndef _KERNEL
  void reserve(size_t capacity) {
    if (capacity <= capacity_) {
      return;
    }
    reallocate(capacity);
  }
#endif  // _KERNEL

  void reset() { reset(nullptr, 0U, 0U); }

  void swap(Vector& other) {
    T* t = ptr_;
    ptr_ = other.ptr_;
    other.ptr_ = t;
    size_t size = size_;
    size_t capacity = capacity_;

    size_ = other.size_;
    capacity_ = other.capacity_;

    other.size_ = size;
    other.capacity_ = capacity;
  }

  void push_back(T&& value, AllocChecker* ac) { push_back_internal(std::move(value), ac); }

  void push_back(const T& value, AllocChecker* ac) { push_back_internal(value, ac); }

#ifndef _KERNEL
  void push_back(T&& value) { push_back_internal(std::move(value)); }

  void push_back(const T& value) { push_back_internal(value); }
#endif  // _KERNEL

  void insert(size_t index, T&& value, AllocChecker* ac) {
    insert_internal(index, std::move(value), ac);
  }

  void insert(size_t index, const T& value, AllocChecker* ac) { insert_internal(index, value, ac); }

#ifndef _KERNEL
  void insert(size_t index, T&& value) { insert_internal(index, std::move(value)); }

  void insert(size_t index, const T& value) { insert_internal(index, value); }
#endif  // _KERNEL

  void resize(size_t size, AllocChecker* ac) { resize_internal(size, ac); }

  void resize(size_t size, const T& value, AllocChecker* ac) { resize_internal(size, value, ac); }

#ifndef _KERNEL
  void resize(size_t size) { resize_internal(size); }

  void resize(size_t size, const T& value) { resize_internal(size, value); }
#endif  // _KERNEL

  // Remove an element from the |index| position in the vector, shifting
  // all subsequent elements one position to fill in the gap.
  // Returns the removed element.
  //
  // Index must be less than the size of the vector.
  T erase(size_t index) {
    ZX_DEBUG_ASSERT(index < size_);
    auto val = std::move(ptr_[index]);
    shift_forward(index);
    consider_shrinking();
    return val;
  }

  void pop_back() {
    ZX_DEBUG_ASSERT(size_ > 0);
    ptr_[--size_].~T();
    consider_shrinking();
  }

  const T* data() const { return ptr_; }

  T* data() { return ptr_; }

  bool is_empty() const { return size_ == 0; }

  const T& operator[](size_t i) const {
    ZX_DEBUG_ASSERT(i < size_);
    return ptr_[i];
  }

  T& operator[](size_t i) {
    ZX_DEBUG_ASSERT(i < size_);
    return ptr_[i];
  }

  T* begin() { return ptr_; }
  const T* begin() const { return ptr_; }

  T* end() { return &ptr_[size_]; }
  const T* end() const { return &ptr_[size_]; }

 private:
  // TODO(smklein): In the future, if we want to be able to push back
  // function pointers and arrays with impunity without requiring exact
  // types, this 'remove_cvref_t' call should probably be replaced with a
  // 'fbl::decay' call.
  template <typename U, typename = std::enable_if_t<std::is_same_v<internal::remove_cvref_t<U>, T>>>
  void push_back_internal(U&& value, AllocChecker* ac) {
    if (!grow_for_new_element(ac)) {
      return;
    }
    new (&ptr_[size_++]) T(std::forward<U>(value));
  }

  template <typename U, typename = std::enable_if_t<std::is_same_v<internal::remove_cvref_t<U>, T>>>
  void push_back_internal(U&& value) {
    grow_for_new_element();
    new (&ptr_[size_++]) T(std::forward<U>(value));
  }

  // Insert an element into the |index| position in the vector, shifting
  // all subsequent elements back one position.
  //
  // Index must be less than or equal to the size of the vector.
  template <typename U, typename = std::enable_if_t<std::is_same_v<internal::remove_cvref_t<U>, T>>>
  void insert_internal(size_t index, U&& value, AllocChecker* ac) {
    ZX_DEBUG_ASSERT(index <= size_);
    if (!grow_for_new_element(ac)) {
      return;
    }
    insert_complete(index, std::forward<U>(value));
  }

  template <typename U, typename = std::enable_if_t<std::is_same_v<internal::remove_cvref_t<U>, T>>>
  void insert_internal(size_t index, U&& value) {
    ZX_DEBUG_ASSERT(index <= size_);
    grow_for_new_element();
    insert_complete(index, std::forward<U>(value));
  }

  // The second half of 'insert', which asumes that there is enough
  // room for a new element.
  template <typename U, typename = std::enable_if_t<std::is_same_v<internal::remove_cvref_t<U>, T>>>
  void insert_complete(size_t index, U&& value) {
    if (index == size_) {
      // Inserting into the end of the vector; nothing to shift
      size_++;
      new (&ptr_[index]) T(std::forward<U>(value));
    } else {
      // Avoid calling both a destructor and move constructor, preferring
      // to simply call a move assignment operator if the index contains
      // a valid (yet already moved-from) object.
      shift_back(index);
      ptr_[index] = std::forward<U>(value);
    }
  }

  // Moves all objects in the internal storage (at & after index) back by one,
  // leaving an 'empty' object at index.
  // Increases the size of the vector by one.
  template <typename U = T>
  std::enable_if_t<std::is_pod_v<U>, void> shift_back(size_t index) {
    ZX_DEBUG_ASSERT(size_ < capacity_);
    ZX_DEBUG_ASSERT(size_ > 0);
    size_++;
    memmove(&ptr_[index + 1], &ptr_[index], sizeof(T) * (size_ - (index + 1)));
  }

  template <typename U = T>
  std::enable_if_t<!std::is_pod_v<U>, void> shift_back(size_t index) {
    ZX_DEBUG_ASSERT(size_ < capacity_);
    ZX_DEBUG_ASSERT(size_ > 0);
    size_++;
    new (&ptr_[size_ - 1]) T(std::move(ptr_[size_ - 2]));
    for (size_t i = size_ - 2; i > index; i--) {
      ptr_[i] = std::move(ptr_[i - 1]);
    }
  }

  // Moves all objects in the internal storage (after index) forward by one.
  // Decreases the size of the vector by one.
  template <typename U = T>
  std::enable_if_t<std::is_pod_v<U>, void> shift_forward(size_t index) {
    ZX_DEBUG_ASSERT(size_ > 0);
    memmove(&ptr_[index], &ptr_[index + 1], sizeof(T) * (size_ - (index + 1)));
    size_--;
  }

  template <typename U = T>
  std::enable_if_t<!std::is_pod_v<U>, void> shift_forward(size_t index) {
    ZX_DEBUG_ASSERT(size_ > 0);
    for (size_t i = index; (i + 1) < size_; i++) {
      ptr_[i] = std::move(ptr_[i + 1]);
    }
    ptr_[--size_].~T();
  }

  template <typename U = T>
  std::enable_if_t<std::is_pod_v<U>, void> transfer_to(T* newPtr, size_t elements) {
    // We must avoid calling memcpy if ptr_ is nullptr -- it's UB to call mempcy
    // on nullptr, even if the size is 0.
    // When the vector's size is zero, ptr_ will be nullptr.
    if (elements > 0) {
      memcpy(newPtr, ptr_, elements * sizeof(T));
    }
  }

  template <typename U = T>
  std::enable_if_t<!std::is_pod_v<U>, void> transfer_to(T* newPtr, size_t elements) {
    for (size_t i = 0; i < elements; i++) {
      new (&newPtr[i]) T(std::move(ptr_[i]));
      ptr_[i].~T();
    }
  }

  // Grows the vector's capacity to accommodate one more element.
  // Returns true on success, false on failure.
  bool grow_for_new_element(AllocChecker* ac) {
    ZX_DEBUG_ASSERT(size_ <= capacity_);
    if (size_ == capacity_) {
      size_t newCapacity =
          capacity_ < kCapacityMinimum ? kCapacityMinimum : capacity_ * kCapacityGrowthFactor;
      if (!reallocate(newCapacity, ac)) {
        return false;
      }
    } else {
      ac->arm(0U, true);
    }
    return true;
  }

  void grow_for_new_element() {
    ZX_DEBUG_ASSERT(size_ <= capacity_);
    if (size_ == capacity_) {
      size_t newCapacity =
          capacity_ < kCapacityMinimum ? kCapacityMinimum : capacity_ * kCapacityGrowthFactor;
      reallocate(newCapacity);
    }
  }

  // Shrink the vector to fit a smaller number of elements, if we reach
  // under the shrink factor.
  void consider_shrinking() {
    if (size_ * kCapacityShrinkFactor < capacity_ && capacity_ > kCapacityMinimum) {
      // Try to shrink the underlying storage
      static_assert((kCapacityMinimum + 1) >= kCapacityShrinkFactor,
                    "Capacity heuristics risk reallocating to zero capacity");
      size_t newCapacity = capacity_ / kCapacityShrinkFactor;

      // If the vector cannot be reallocated to a smaller size (reallocate fails) it will
      // continue to use a larger capacity.
      AllocChecker ac;
      reallocate(newCapacity, &ac);
      ac.check();
    }
  }

  // Forces capacity to become newCapacity.
  // Returns true on success, false on failure.
  // If reallocate fails, the old "ptr_" array is unmodified.
  bool reallocate(size_t newCapacity, AllocChecker* ac) {
    ZX_DEBUG_ASSERT(newCapacity > 0);
    ZX_DEBUG_ASSERT(newCapacity >= size_);
    auto newPtr = reinterpret_cast<T*>(AllocatorTraits::Allocate(newCapacity * sizeof(T)));
    if (newPtr == nullptr) {
      ac->arm(1U, false);
      return false;
    }
    transfer_to(newPtr, size_);
    AllocatorTraits::Deallocate(ptr_);
    capacity_ = newCapacity;
    ptr_ = newPtr;
    ac->arm(0U, true);
    return true;
  }

#ifndef _KERNEL
  void reallocate(size_t newCapacity) {
    ZX_DEBUG_ASSERT(newCapacity > 0);
    ZX_DEBUG_ASSERT(newCapacity >= size_);
    auto newPtr = reinterpret_cast<T*>(AllocatorTraits::Allocate(newCapacity * sizeof(T)));
    transfer_to(newPtr, size_);
    AllocatorTraits::Deallocate(ptr_);
    capacity_ = newCapacity;
    ptr_ = newPtr;
  }
#endif

  // Release returns the underlying storage of the vector,
  // while emptying out the vector itself (so it can be destroyed
  // without deleting the release storage).
  T* release() {
    T* t = ptr_;
    ptr_ = nullptr;
    size_ = 0;
    capacity_ = 0;
    return t;
  }

  void resize_internal(size_t size, AllocChecker* ac) {
    if (size_ > size) {
      destruct_at_end(size_ - size);
      consider_shrinking();
      ac->arm(0U, true);
    } else if (size_ < size) {
      reserve(size, ac);
      if (!ac->check()) {
        return;
      }
      construct_at_end(size - size_);
    }
  }

  void resize_internal(size_t size, const T& value, AllocChecker* ac) {
    if (size_ > size) {
      destruct_at_end(size_ - size);
      consider_shrinking();
      ac->arm(0U, true);
    } else if (size_ < size) {
      reserve(size, ac);
      if (!ac->check()) {
        return;
      }
      construct_at_end(size - size_, value);
    }
  }

#ifndef _KERNEL
  void resize_internal(size_t size) {
    if (size_ > size) {
      destruct_at_end(size_ - size);
      consider_shrinking();
    } else if (size_ < size) {
      reserve(size);
      construct_at_end(size - size_);
    }
  }

  void resize_internal(size_t size, const T& value) {
    if (size_ > size) {
      destruct_at_end(size_ - size);
      consider_shrinking();
    } else if (size_ < size) {
      reserve(size);
      construct_at_end(size - size_, value);
    }
  }
#endif

  void construct_at_end(size_t count) {
    ZX_DEBUG_ASSERT(size_ + count <= capacity_);
    while (count--) {
      new (&ptr_[size_++]) T();
    }
  }

  void construct_at_end(size_t count, const T& value) {
    ZX_DEBUG_ASSERT(size_ + count <= capacity_);
    while (count--) {
      new (&ptr_[size_++]) T(value);
    }
  }

  void destruct_at_end(size_t count) {
    ZX_DEBUG_ASSERT(count <= size_);
    while (count--) {
      ptr_[--size_].~T();
    }
  }

  void reset(T* t, size_t size, size_t capacity) {
    ZX_DEBUG_ASSERT(size <= capacity);
    destruct_at_end(size_);
    T* ptr = ptr_;
    ptr_ = t;
    size_ = size;
    capacity_ = capacity;
    AllocatorTraits::Deallocate(ptr);
  }

  T* ptr_;
  size_t size_;
  size_t capacity_;

  static constexpr size_t kCapacityMinimum = 16;
  static constexpr size_t kCapacityGrowthFactor = 2;
  static constexpr size_t kCapacityShrinkFactor = 4;
};

}  // namespace fbl

#endif  // FBL_VECTOR_H_
