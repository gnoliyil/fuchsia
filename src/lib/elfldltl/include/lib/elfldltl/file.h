// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ELFLDLTL_INCLUDE_LIB_ELFLDLTL_FILE_H_
#define SRC_LIB_ELFLDLTL_INCLUDE_LIB_ELFLDLTL_FILE_H_

#include <lib/fit/result.h>
#include <lib/stdcompat/span.h>

#include <optional>
#include <type_traits>
#include <utility>

#include "diagnostics.h"

namespace elfldltl {

template <typename Handle>
inline auto DefaultMakeInvalidHandle() {
  return Handle();
}

// elfldltl::File<Handle, Offset, Read, MakeInvalidHandle> implements the File
// API (see memory.h) by holding a Handle object and calling Read as a function
// bool(Handle&, Offset, cpp20::span<byte>) that returns true for success.
// MakeInvalidHandle can be supplied if default-construction isn't the way.
template <class Diagnostics, typename Handle, typename Offset, auto Read,
          auto MakeInvalidHandle = &DefaultMakeInvalidHandle<Handle>>
class File {
 public:
  static_assert(std::is_invocable_v<decltype(Read), Handle&, Offset, cpp20::span<std::byte>>);
  static_assert(std::is_convertible_v<decltype(MakeInvalidHandle()), Handle>);

  explicit File(Diagnostics diag) : diag_(diag) {}
  File(const File&) noexcept(std::is_nothrow_copy_constructible_v<Handle>) = default;
  File(File&&) noexcept(std::is_nothrow_move_constructible_v<Handle>) = default;

  File(Handle handle, Diagnostics diag) noexcept(std::is_nothrow_move_constructible_v<Handle>)
      : handle_(std::move(handle)), diag_(diag) {}

  File& operator=(const File&) noexcept(std::is_nothrow_copy_assignable_v<Handle>) = default;
  File& operator=(File&&) noexcept(std::is_nothrow_move_assignable_v<Handle>) = default;

  const Handle& get() const { return handle_; }

  Handle release() { return std::exchange(handle_, MakeInvalidHandle()); }

  template <typename T>
  std::optional<T> ReadFromFile(Offset offset) {
    std::optional<T> result{std::in_place};
    cpp20::span<T> data{std::addressof(result.value()), 1};
    FinishReadFromFile(offset, data, result);
    return result;
  }

  template <typename T, typename Allocator>
  auto ReadArrayFromFile(off_t offset, Allocator&& allocator, size_t count) {
    auto result = allocator(count);
    if (result) {
      cpp20::span<T> data = *result;
      FinishReadFromFile(offset, data, result);
    }
    return result;
  }

 protected:
  Handle& handle() { return handle_; }
  const Handle& handle() const { return handle_; }

 private:
  template <typename T, typename Result>
  void FinishReadFromFile(Offset offset, cpp20::span<T> data, Result& result) {
    if (!Read(handle_, offset, cpp20::as_writable_bytes(data))) {
      DiagnoseError(offset);
      result = Result();
    }
  }

  void DiagnoseError(Offset offset) { diag_.FormatError("couldn't read file at offset: ", offset); }

  Handle handle_ = MakeInvalidHandle();
  [[no_unique_address]] Diagnostics diag_;
};

}  // namespace elfldltl

#endif  // SRC_LIB_ELFLDLTL_INCLUDE_LIB_ELFLDLTL_FILE_H_
