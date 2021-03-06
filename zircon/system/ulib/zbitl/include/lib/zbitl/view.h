// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZBITL_VIEW_H_
#define LIB_ZBITL_VIEW_H_

#include <inttypes.h>
#include <lib/cksum.h>
#include <lib/fitx/result.h>
#include <zircon/assert.h>
#include <zircon/boot/image.h>

#include <functional>
#include <optional>
#include <type_traits>
#include <variant>

#include "checking.h"
#include "decompress.h"
#include "item.h"
#include "storage_traits.h"

namespace zbitl {

// Forward declaration; defined in image.h.
template <typename Storage>
class Image;

///
/// ExampleContainerTraits serves as an definitional examplar for how
/// "container traits" should be structured. Container traits provide types,
/// and static constants and methods that abstract how to parse and navigate
/// a particular container format (e.g., ZBI or BOOTFS).
///
/// An "item" is an entry within the container, which is expected to be encoded
/// by an ("item header", "payload") pair. The payload is the raw binary
/// content of the item, while the item header provides its metadata, most
/// important of which is the payload's size and its location in the container.
/// When parsing, the traits should provide a means of navigating from an item
/// header to either its payload or to the next item header.
///
/// The container is expected to have a special header at offset 0, its
/// "container header", giving metadata on the container itself, including its
/// total size. The first item header is expected to immediately follow the
/// container header.
struct ExampleContainerTraits {
  /// The type of a container header, expected to be POD.
  struct container_header_type {};

  /// The type of an item header, expected to be POD.
  struct item_header_type {};

  /// Error encapsulates errors encountered in navigating the container, either
  /// those coming from the storage backend or from structural issues with the
  /// container itself. ErrorTraits corresponds to the `ErrorTraits` member
  /// type of a StorageTraits specialization; it serves as a template parameter
  /// so that Error may be defined in terms of the associated storage error
  /// type (e.g., as a member).
  template <typename ErrorTraits>
  struct Error {};

  /// The name of the associated C++ container type. This is given as a C-style
  /// string (as opposed to a std::string_view) as the constant is only meant
  /// to provide context within printf() statements.
  static constexpr const char* kContainerType = "zbitl::ExampleContainer";

  /// The expected alignment - within the container - of an item header. Must
  /// be a power of two.
  static constexpr uint32_t kItemAlignment = 1;

  /// Payloads are expected to be followed by padding up to a multiple of this
  /// value. This quantity is unrelated to the size of the payload itself.
  static constexpr uint32_t kPayloadPaddingAlignment = 1;

  /// Whether the payloads lie within the container. A container format may not
  /// include them properly and instead point to the data elsewhere in the
  /// storage (as is the case with BOOTFS).
  static constexpr bool kPayloadsAreContained = false;

  /// Whether the container format permits mutations of its contents (provided
  /// the backing storage is writable).
  static constexpr bool kIsMutableFormat = false;

  /// Returns the size of a container, as it is encoded in the header. The size
  /// includes that of the header. It is the responsibility of the caller to
  /// validate the returned size against the actual storage capacity.
  static uint32_t ContainerSize(const container_header_type& header) { return sizeof(header); }

  /// Returns the exact size of an item's payload (excluding padding).
  static uint32_t PayloadSize(const item_header_type& header) { return 0; }

  /// Updates the size of the associated payload. It is the responsibility of
  /// the caller to ensure that this size is sensible; no check is performed.
  /// This trait is only required if `kIsMutable` is true.
  template <typename T = ExampleContainerTraits, typename = std::enable_if_t<T::kIsMutable>>
  static void SetPayloadSize(item_header_type& header, uint32_t new_size) {}

  /// Returns the offset at which a payload is to be found, given the
  /// associated item header and that header's offset into the container.
  static uint32_t PayloadOffset(const item_header_type& header, uint32_t item_offset) { return 0; }

  /// Returns the offset of the next item header, given a current item header
  /// and its offset into the container.
  ///
  /// TODO(joshuaseaton): in general, a container header may affect navigation
  static uint32_t NextItemOffset(const item_header_type& header, uint32_t item_offset) { return 0; }

  /// Validates item and container headers, returning a description of the
  /// failure in that event. The check is agnostic of storage capacity; for
  /// example, whether any encoded lengths are sensible are left to the caller
  /// to validate against the actual storage capacity.
  static fitx::result<std::string_view> CheckContainerHeader(const container_header_type& header) {
    return fitx::error{"unimplemented"};
  }

  static fitx::result<std::string_view> CheckItemHeader(const item_header_type& header) {
    return fitx::error{"unimplemented"};
  }

  /// Modify a header so that it passes checks. This can be used to mint new
  /// items from a designated initializer that omits uninteresting bits.
  /// This trait is only required if `kIsMutable` is true.
  template <typename T = ExampleContainerTraits, typename = std::enable_if_t<T::kIsMutable>>
  static item_header_type SanitizeHeader(item_header_type header) {
    return header;
  }

  /// Converts the context of an iteration failure into an Error.
  template <typename StorageTraits>
  static Error<typename StorageTraits::ErrorTraits> ToError(
      typename StorageTraits::storage_type& storage,  //
      std::string_view reason,                        //
      uint32_t item_offset,   // Offset of the item in the iteration context.
      uint32_t error_offset,  // Offset at which the error occurred.
      std::optional<item_header_type> header = std::nullopt,  //
      std::optional<typename StorageTraits::ErrorTraits::error_type> storage_error = std::nullopt) {
    return {};
  }
};

///
/// ZbiTraits gives a container trait implementation - per
/// ExampleContainerTraits above - of the ZBI format.
///
struct ZbiTraits {
  using container_header_type = zbi_header_t;
  using item_header_type = zbi_header_t;

  static constexpr const char* kContainerType = "zbitl::View";
  static constexpr uint32_t kItemAlignment = ZBI_ALIGNMENT;
  static constexpr uint32_t kPayloadPaddingAlignment = ZBI_ALIGNMENT;
  static constexpr bool kPayloadsAreContained = true;
  static constexpr bool kIsMutable = true;

  static uint32_t ContainerSize(const container_header_type& header) {
    return sizeof(header) + header.length;
  }

  static uint32_t PayloadSize(const item_header_type& header) { return header.length; }

  static void SetPayloadSize(item_header_type& header, uint32_t new_size) {
    header.length = new_size;
  }

  static uint32_t PayloadOffset(const item_header_type& header, uint32_t item_offset) {
    return item_offset + sizeof(header);
  }

  static uint32_t NextItemOffset(const item_header_type& header, uint32_t item_offset) {
    return item_offset + sizeof(header) + ZBI_ALIGN(header.length);
  }

  static fitx::result<std::string_view> CheckContainerHeader(const container_header_type& header);
  static fitx::result<std::string_view> CheckItemHeader(const item_header_type& header);

  // Aliased from <lib/zbitl/checking.h>.
  static constexpr auto SanitizeHeader = &::zbitl::SanitizeHeader;

  template <typename ErrorTraits>
  struct Error {
    /// `storage_error_string` gives a redirect to ErrorTraits's static
    /// `error_string` method for stringifying storage  errors; this is used to
    /// stringify the entirety of Error in contexts where the associated traits
    /// are not known or accessible.
    static constexpr auto storage_error_string = &ErrorTraits::error_string;

    /// A string constant describing the error.
    std::string_view zbi_error{};

    /// This is the offset into the storage object at which an error occurred.
    /// This is zero for problems with the overall container, which begin()
    /// detects. In iterator operations, it refers to the offset into the image
    /// where the item header was (or should have been).
    uint32_t item_offset = 0;

    /// This reflects the underlying error from accessing the Storage object,
    /// if any.  If storage_error.has_value() is false, then the error is in
    /// the format of the contents of the ZBI, not in accessing the contents.
    std::optional<typename ErrorTraits::error_type> storage_error{};
  };

  template <typename StorageTraits>
  static Error<typename StorageTraits::ErrorTraits> ToError(
      typename StorageTraits::storage_type& storage,          //
      std::string_view reason,                                //
      uint32_t item_offset,                                   //
      uint32_t error_offset,                                  //
      std::optional<item_header_type> header = std::nullopt,  //
      std::optional<typename StorageTraits::ErrorTraits::error_type> storage_error = std::nullopt) {
    return {reason, error_offset, storage_error};
  }
};

/// The zbitl::View class provides functionality for processing ZBI items in various
/// storage formats.
///
/// For example, the entries in a ZBI present in memory can be enumerated as follows:
///
/// ```
/// void ProcessZbiEntries(std::string_view data) {
///   // Create the view.
///   zbitl::View<std::string_view> view{data};
///
///   // Iterate over entries.
///   for (const auto& entry : view) {
///     printf("Found entry of type %x with payload size %ld.\n",
///            entry.header->type,     // entry.header has type similar to "zbi_header_t *".
///            entry.payload.size());  // entry.payload has type "std::string_view".
///   }
///
///   // Callers are required to check for errors (or call "ignore_error")
///   // prior to object destruction. See "Error checking" below.
///   if (auto error = view.take_error(); error.is_error()) {
///     printf("Error encountered!\n");
///     // ...
///   }
/// }
//  ```
///
/// zbitl::View satisfies the C++20 std::forward_range concept; it satisfies the
/// std::view concept if the Storage and the associated error_type types support
/// constant-time copy/move/assignment.
///
/// ## Error checking
///
/// The "error-checking view" pattern means that the container/range/view API
/// of begin() and end() iterators is supported, but when begin() or
/// iterator::operator++() encounters an error, it simply returns end() so that
/// loops terminate normally.  Thereafter, take_error() must be called to check
/// whether the loop terminated because it iterated past the last item or
/// because it encountered an error.  Once begin() has been called,
/// take_error() must be called before the View is destroyed, so no error goes
/// undetected.  Since all use of iterators updates the error state, use of any
/// zbitl::View object must be serialized and after begin() or operator++()
/// yields end(), take_error() must be checked before using begin() again.
///
/// ## Iteration
///
/// Each time begin() is called the underlying storage is examined afresh, so
/// it's safe to reuse a zbitl::View object after changing the data.  Reducing
/// the size of the underlying storage invalidates any iterators that pointed
/// past the new end of the image.  It's simplest just to assume that changing
/// the underlying storage always invalidates all iterators.
///
/// ## Storage
///
/// The Storage type is some type that can be abstractly considered to have
/// non-owning "view" semantics: it doesn't hold the storage of the ZBI, it
/// just refers to it somehow.  The zbitl::View:Error type describes errors
/// encountered while iterating.  It uses the associated error_type type to
/// propagate errors caused by access to the underlying storage.
///
/// Usually Storage and error_type types are small and can be copied.
/// zbitl::View is move-only if Storage is move-only or if error_type is
/// move-only.  Note that copying zbitl::View copies its error-checking state
/// exactly, so if the original View needed to be checked for errors before
/// destruction then both the original and the copy need to be checked before
/// their respective destructions.  A moved-from zbitl::View can always be
/// destroyed without checking.
template <typename Storage>
class View {
 private:
  using ContainerTraits = ZbiTraits;
  using container_header_type = typename ContainerTraits::container_header_type;
  using item_header_type = typename ContainerTraits::item_header_type;

 public:
  using storage_type = Storage;
  using Traits = ExtendedStorageTraits<storage_type>;
  using storage_error_type = typename Traits::ErrorTraits::error_type;
  using Error = typename ContainerTraits::template Error<typename Traits::ErrorTraits>;

  View() = default;
  View(const View&) = default;
  View& operator=(const View&) = default;

  // This is almost the same as the default move behavior.  But it also
  // explicitly resets the moved-from error state to kUnused so that the
  // moved-from View can be destroyed without checking it.
  View(View&& other)
      : storage_(std::move(other.storage_)), error_(std::move(other.error_)), limit_(other.limit_) {
    other.error_ = Unused{};
    other.limit_ = 0;
  }
  View& operator=(View&& other) {
    error_ = std::move(other.error_);
    other.error_ = Unused{};
    storage_ = std::move(other.storage_);
    limit_ = other.limit_;
    other.limit_ = 0;
    return *this;
  }

  explicit View(storage_type storage) : storage_(std::move(storage)) {}

  ~View() {
    ZX_ASSERT_MSG(!std::holds_alternative<Error>(error_), "%s destroyed after error without check",
                  ContainerTraits::kContainerType);
    ZX_ASSERT_MSG(!std::holds_alternative<NoError>(error_),
                  "%s destroyed after successful iteration without check",
                  ContainerTraits::kContainerType);
  }

  /// The header is represented by an opaque type that can be dereferenced as
  /// if it were `const item_header_t*`, i.e. `*header` or `header->member`.
  /// Either it stores the `item_header_type` directly or it holds a pointer
  /// into someplace owned or viewed by the Storage object.  In the latter
  /// case, i.e. when Storage represents something already in memory,
  /// `item_header_wrapper` should be no larger than a plain pointer.
  class item_header_wrapper {
   public:
    item_header_wrapper() = default;
    item_header_wrapper(const item_header_wrapper&) = default;
    item_header_wrapper(item_header_wrapper&&) = default;
    item_header_wrapper& operator=(const item_header_wrapper&) = default;
    item_header_wrapper& operator=(item_header_wrapper&&) = default;

    /// `*header` always copies, so lifetime of `this` doesn't matter.
    item_header_type operator*() const {
      if constexpr (kCopy) {
        return stored_;
      } else {
        return *stored_;
      }
    }

    /// `header->member` refers to the header in place, so never do
    /// `&header->member` but always dereference a member directly.
    const item_header_type* operator->() const {
      if constexpr (kCopy) {
        return &stored_;
      } else {
        return stored_;
      }
    }

    /// A size accessor that avoids copying.
    uint32_t size() const {
      if constexpr (kCopy) {
        return ContainerTraits::PayloadSize(stored_);
      } else {
        return ContainerTraits::PayloadSize(*stored_);
      }
    }

   private:
    using TraitsHeader = typename Traits::template LocalizedReadResult<item_header_type>;
    static constexpr bool kCopy = std::is_same_v<TraitsHeader, item_header_type>;
    static constexpr bool kReference =
        std::is_same_v<TraitsHeader, std::reference_wrapper<const item_header_type>>;
    static_assert(kCopy || kReference,
                  "zbitl::StorageTraits specialization's Header function returns wrong type");

    friend View;
    template <typename ImageStorage>
    friend class Image;

    using HeaderStorage = std::conditional_t<kCopy, item_header_type, const item_header_type*>;
    HeaderStorage stored_;

    // This can only be used by begin(), below - and by Image's Append.
    template <typename T>
    explicit item_header_wrapper(const T& header)
        : stored_([&header]() {
            if constexpr (kCopy) {
              static_assert(std::is_same_v<item_header_type, T>);
              return header;
            } else {
              static_assert(std::is_same_v<std::reference_wrapper<const item_header_type>, T>);
              return &(header.get());
            }
          }()) {}
  };

  /// The payload type is provided by the StorageTraits specialization.  It's
  /// opaque to View, but must be default-constructible, copy-constructible,
  /// and copy-assignable.  It's expected to have "view"-style semantics,
  /// i.e. be small and not own any storage itself but only refer to storage
  /// owned by the Storage object.
  using payload_type = typename Traits::payload_type;

  /// The element type is a trivial struct morally equivalent to
  /// std::pair<item_header_wrapper, payload_type>.  Both member types are
  /// default-constructible, copy-constructible, and copy-assignable, so
  /// value_type as a whole is as well.
  struct value_type {
    item_header_wrapper header;
    payload_type payload;
  };

  /// An error type encompassing both read and write failures in accessing the
  /// source and destination storage objects in the context of a copy
  /// operation. In the event of a read error, we expect the write_* fields to
  /// remain unset; in the event of a write error, we expect the read_* fields
  /// to remain unset.
  template <typename CopyStorage>
  struct CopyError {
    using WriteTraits = StorageTraits<std::decay_t<CopyStorage>>;
    using WriteError = typename WriteTraits::ErrorTraits::error_type;
    using ReadError = storage_error_type;

    static auto read_error_string(ReadError error) {
      return Traits::ErrorTraits::error_string(error);
    }

    static auto write_error_string(WriteError error) {
      return WriteTraits::ErrorTraits::error_string(error);
    }

    /// A string constant describing the error.
    std::string_view zbi_error{};

    /// This is the offset into the storage object at which a read error
    /// occured. This field is expected to be unset in the case of a write
    /// error.
    uint32_t read_offset = 0;

    /// This reflects the underlying error from accessing the storage object
    /// that from which the copy was attempted. This field is expected to be
    /// std::nullopt in the case of a write error.
    std::optional<storage_error_type> read_error{};

    /// This is the offset into the storage object at which a write error
    /// occured. This field is expected to be unset in the case of a read
    /// error.
    uint32_t write_offset = 0;

    /// This reflects the underlying error from accessing the storage object
    /// that to which the copy was attempted. This field is expected to be
    /// std::nullopt in the case of a read error.
    std::optional<WriteError> write_error{};
  };

  /// Check the container for errors after using iterators.  When begin() or
  /// iterator::operator++() encounters an error, it simply returns end() so
  /// that loops terminate normally.  Thereafter, take_error() must be called
  /// to check whether the loop terminated because it iterated past the last
  /// item or because it encountered an error.  Once begin() has been called,
  /// take_error() must be called before the View is destroyed, so no error
  /// goes undetected.  After take_error() is called the error state is
  /// consumed and take_error() cannot be called again until another begin() or
  /// iterator::operator++() call has been made.
  [[nodiscard]] fitx::result<Error> take_error() {
    ErrorState result = std::move(error_);
    error_ = Taken{};
    if (std::holds_alternative<Error>(result)) {
      return fitx::error{std::move(std::get<Error>(result))};
    }
    ZX_ASSERT_MSG(!std::holds_alternative<Taken>(result), "%s::take_error() was already called",
                  ContainerTraits::kContainerType);
    return fitx::ok();
  }

  /// If you explicitly don't care about any error that might have terminated
  /// the last loop early, then call ignore_error() instead of take_error().
  void ignore_error() { static_cast<void>(take_error()); }

  /// Trivial accessors for the underlying Storage (view) object.
  storage_type& storage() { return storage_; }
  const storage_type& storage() const { return storage_; }

  class iterator {
   public:
    /// The default-constructed iterator is invalid for all uses except
    /// equality comparison.
    iterator() = default;

    iterator& operator=(const iterator&) = default;

    bool operator==(const iterator& other) const {
      return other.view_ == view_ && other.offset_ == offset_;
    }

    bool operator!=(const iterator& other) const { return !(*this == other); }

    iterator& operator++() {  // prefix
      Assert(__func__);
      view_->StartIteration();
      const uint32_t next_item_offset = ContainerTraits::NextItemOffset(*value_.header, offset_);
      Update(next_item_offset);
      return *this;
    }

    iterator operator++(int) {  // postfix
      iterator old = *this;
      ++*this;
      return old;
    }

    const View::value_type& operator*() const {
      Assert(__func__);
      return value_;
    }

    const View::value_type* operator->() const {
      Assert(__func__);
      return &value_;
    }

    uint32_t item_offset() const { return offset_; }

    uint32_t payload_offset() const {
      Assert(__func__);
      return ContainerTraits::PayloadOffset(*(value_.header), offset_);
    }

    View& view() const {
      ZX_ASSERT_MSG(view_, "%s on default-constructed %s::iterator", __func__,
                    ContainerTraits::kContainerType);
      return *view_;
    }

    // Iterator traits.
    using iterator_category = std::input_iterator_tag;
    using reference = View::value_type&;
    using value_type = View::value_type;
    using pointer = View::value_type*;
    using difference_type = size_t;

   private:
    // Private fields accessed by Image<Storage>::Append().
    template <typename ImageStorage>
    friend class Image;

    // The default-constructed state is almost the same as the end() state:
    // nothing but operator==() should ever be called if view_ is nullptr.
    View* view_ = nullptr;

    // The offset into the ZBI of the current item's header.  This is 0 in
    // default-constructed iterators and kEnd_ in end() iterators, where
    // operator*() can never be called.  A valid non-end() iterator holds the
    // header and payload (references) of the current item for operator*() to
    // return. If offset_ is at the end of the container, then operator++()
    // will yield end().
    uint32_t offset_ = 0;

    // end() uses a different offset_ value to distinguish a true end iterator
    // from a particular view from a default-constructed iterator from nowhere.
    static constexpr uint32_t kEnd_ = std::numeric_limits<uint32_t>::max();

    // This is left uninitialized until a successful increment sets it.
    // It is only examined by a dereference, which is invalid without
    // a successful increment.
    value_type value_{};

    // This is called only by begin() and end().
    friend class View;
    iterator(View* view, bool is_end) : view_(view) {
      ZX_DEBUG_ASSERT(view_);
      if (is_end) {
        offset_ = kEnd_;
      } else {
        Update(sizeof(container_header_type));
      }
    }

    // Updates the state of the iterator to reflect a new offset.
    void Update(uint32_t next_item_offset) {
      ZX_DEBUG_ASSERT(next_item_offset >= sizeof(container_header_type));
      ZX_DEBUG_ASSERT_MSG(next_item_offset <= view_->limit_,
                          "%s::iterator next_item_offset %#" PRIx32 " > limit_ %#" PRIx32,
                          ContainerTraits::kContainerType, next_item_offset, view_->limit_);
      ZX_DEBUG_ASSERT(next_item_offset % ContainerTraits::kItemAlignment == 0);

      if (next_item_offset == view_->limit_) {
        // Reached the end.
        *this = view_->end();
        return;
      }
      if (view_->limit_ < next_item_offset ||
          view_->limit_ - next_item_offset < sizeof(item_header_type)) {
        Fail("container too short for next item header");
        return;
      }

      if (auto header = view_->ItemHeader(next_item_offset); header.is_error()) {
        // Failed to read the next header.
        Fail("cannot read item header", std::move(header.error_value()));
        return;
      } else if (auto header_error = ContainerTraits::CheckItemHeader(header.value());
                 header_error.is_error()) {
        Fail(header_error.error_value());
        return;
      } else {
        value_.header = item_header_wrapper(header.value());
      }

      // If payloads lie within the container, we validate that this particular
      // payload does indeed fit within; else, we can only check that it fits
      // within the storage itself.
      //
      // TODO(fxbug.dev/68585): while this level of generality is neither
      // useful nor sensible to View, it soon will be for a BOOTFS-related
      // refactoring of this logic.
      uint32_t payload_limit = view_->limit_;
      if constexpr (!ContainerTraits::kPayloadsAreContained) {
        auto result = Traits::Capacity(view_->storage());
        if (result.is_error()) {
          Fail("cannot determine storage capacity", std::move(result).error_value(), 0);
          return;
        }
        payload_limit = std::move(result).value();
      }

      const uint32_t payload_offset =
          ContainerTraits::PayloadOffset(*value_.header, next_item_offset);
      const uint32_t payload_size = value_.header.size();
      const uint32_t padded_payload_size =
          (payload_size + ContainerTraits::kPayloadPaddingAlignment - 1) &
          -ContainerTraits::kPayloadPaddingAlignment;
      if (payload_offset > payload_limit ||
          padded_payload_size < payload_size ||  // ensure aligned size didn't overflow
          padded_payload_size > payload_limit - payload_offset) {
        if constexpr (ContainerTraits::kPayloadsAreContained) {
          Fail("container too short for next item payload");
        } else {
          Fail("storage too small for next item payload");
        }
        return;
      }

      if (auto payload = Traits::Payload(view_->storage(), payload_offset, payload_size);
          payload.is_error()) {
        Fail("cannot extract payload view", std::move(payload.error_value()), payload_offset);
        return;
      } else {
        value_.payload = std::move(payload.value());
      }
      offset_ = next_item_offset;
    }

    void Fail(std::string_view sv, std::optional<storage_error_type> storage_error = std::nullopt,
              std::optional<uint32_t> error_offset = std::nullopt) {
      view_->Fail(ContainerTraits::template ToError<Traits>(
          view_->storage(), sv, offset_, error_offset.value_or(offset_), *value_.header,
          std::move(storage_error)));
      *this = view_->end();
    }

    void Assert(const char* func) const {
      ZX_ASSERT_MSG(view_, "%s on default-constructed %s::iterator", func,
                    ContainerTraits::kContainerType);
      ZX_ASSERT_MSG(offset_ != kEnd_, "%s on %s::end() iterator", func,
                    ContainerTraits::kContainerType);
    }
  };

  // This returns its own error state and does not affect the `take_error()`
  // state of the View.
  fitx::result<Error, container_header_type> container_header() {
    auto to_error = [this](
                        std::string_view reason, uint32_t error_offset = 0,
                        std::optional<storage_error_type> storage_error = std::nullopt) -> Error {
      return ContainerTraits::template ToError<Traits>(storage(), reason, 0, error_offset,
                                                       std::nullopt, std::move(storage_error));
    };
    auto capacity_error = Traits::Capacity(storage());
    if (capacity_error.is_error()) {
      return fitx::error{to_error("cannot determine storage capacity", 0,
                                  std::move(capacity_error).error_value())};
    }
    uint32_t capacity = capacity_error.value();

    // Minimal bounds check before trying to read.
    if (capacity < sizeof(container_header_type)) {
      return fitx::error(to_error("container header doesn't fit. Truncated?", capacity));
    }

    // Read and validate the container header.
    auto header_error = ContainerHeader();
    if (header_error.is_error()) {
      // Failed to read the container header.
      return fitx::error{
          to_error("cannot read container header", 0, std::move(header_error).error_value())};
    }

    container_header_type header = std::move(header_error).value();

    auto check_error = ContainerTraits::CheckContainerHeader(header);
    if (check_error.is_error()) {
      return fitx::error{to_error(check_error.error_value())};
    }
    const uint32_t size = ContainerTraits::ContainerSize(header);
    if (size < sizeof(header) || size > capacity) {
      return fitx::error{to_error("container doesn't fit. Truncated?")};
    }

    return fitx::ok(header);
  }

  /// After calling begin(), it's mandatory to call take_error() before
  /// destroying the View object.  An iteration that encounters an error will
  /// simply end early, i.e. begin() or operator++() will yield an iterator
  /// that equals end().  At the end of a loop, call take_error() to check for
  /// errors.  It's also acceptable to call take_error() during an iteration
  /// that hasn't reached end() yet, but it cannot be called again before the
  /// next begin() or operator++() call.
  iterator begin() {
    StartIteration();
    auto header = container_header();
    if (header.is_error()) {
      Fail(header.error_value());
      limit_ = 0;  // Reset from past uses.
      return end();
    }
    // The container's "payload" is all the items.  Don't scan past it.
    limit_ = ContainerTraits::ContainerSize(header.value());
    return {this, false};
  }

  iterator end() { return {this, true}; }

  size_t size_bytes() {
    if (std::holds_alternative<Unused>(error_)) {
      ZX_ASSERT(limit_ == 0);

      // Taking the size before doing begin() takes extra work.
      auto capacity_error = Traits::Capacity(storage());
      if (capacity_error.is_ok()) {
        uint32_t capacity = capacity_error.value();
        if (capacity >= sizeof(container_header_type)) {
          auto header_error = ContainerHeader();
          if (header_error.is_ok()) {
            container_header_type header = std::move(header_error).value();
            const uint32_t size = ContainerTraits::ContainerSize(header);
            if (sizeof(header) <= size && size <= capacity) {
              return size;
            }
          }
        }
      }
    }
    return limit_;
  }

  // Replace an item's header with a new one, using an iterator into this
  // view.  This never changes the existing item's length (nor its payload).
  // So the header can be `{.type = XYZ}` alone or whatever fields and flags
  // matter.  Note this returns only the storage error type, not an Error since
  // no ZBI format errors are possible here, only a storage failure to update.
  //
  // This method is not available if zbitl::StorageTraits<storage_type>
  // doesn't support mutation.
  template <typename T = Traits, typename = std::enable_if_t<T::CanWrite()>,
            typename C = ContainerTraits, typename = std::enable_if_t<C::kIsMutable>>
  fitx::result<storage_error_type> EditHeader(const iterator& item,
                                              const item_header_type& header) {
    item.Assert(__func__);
    if (auto result = WriteHeader(header, item.item_offset(), item.value_.header.size());
        result.error_value()) {
      return result.take_error();
    }
    return fitx::ok();
  }

  // When the iterator is mutable and not a temporary, make the next
  // operator*() consistent with the new header if it worked.  For kReference
  // storage types, the change is reflected intrinsically.
  template <typename T = Traits, typename = std::enable_if_t<T::CanWrite()>,
            typename C = ContainerTraits, typename = std::enable_if_t<C::kIsMutable>>
  fitx::result<storage_error_type> EditHeader(iterator& item, const item_header_type& header) {
    item.Assert(__func__);
    auto result = WriteHeader(header, item.item_offset(), item.value_.header.size());
    if constexpr (item_header_wrapper::kCopy) {
      if (result.is_ok()) {
        item.value_.header.stored_ = result.value();
      }
    }
    if (result.is_error()) {
      return result.take_error();
    }
    return fitx::ok();
  }

  // Verifies that a given View iterator points to an item with a valid CRC32.
  fitx::result<Error, bool> CheckCrc32(iterator it) {
    auto [header, payload] = *it;
    if (!(header->flags & ZBI_FLAG_CRC32)) {
      return fitx::ok(true);
    }

    uint32_t item_crc32 = 0;
    auto compute_crc32 = [&item_crc32](ByteView chunk) -> fitx::result<fitx::failed> {
      // The cumulative value in principle will not be updated by the
      // CRC32 of empty data, so do not bother with computation in
      // this case; doing so, we also sidestep any issues around how
      // `crc32()` handles the corner case of a nullptr.
      if (!chunk.empty()) {
        item_crc32 =
            crc32(item_crc32, reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
      }
      return fitx::ok();
    };

    // An item's CRC32 is computed as the hash of its header with its
    // crc32 field set to 0, combined with the hash of its payload.
    zbi_header_t header_without_crc32 = *header;
    header_without_crc32.crc32 = 0;
    static_cast<void>(compute_crc32(
        {reinterpret_cast<std::byte*>(&header_without_crc32), sizeof(header_without_crc32)}));

    auto result = Read(payload, header->length, compute_crc32);
    if (result.is_error()) {
      return fitx::error{Error{
          .zbi_error = "cannot compute item CRC32",
          .item_offset = it.item_offset(),
          .storage_error = std::move(result).error_value(),
      }};
    }
    ZX_DEBUG_ASSERT(result.value().is_ok());
    return fitx::ok(item_crc32 == header->crc32);
  }

  // Copy a range of the underlying storage into an existing piece of storage,
  // which can be any mutable type with sufficient capacity.  The Error return
  // value is for a read error.  The "success" return value indicates there was
  // no read error.  It's another fitx::result<storage_error_type> for the
  // writing side (which may be different than the type used in
  // Error::storage_error).  The optional `to_offset` argument says where in
  // `to` the data is written, as a byte offset that is zero by default.
  template <typename CopyStorage>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> Copy(CopyStorage&& to, uint32_t offset,
                                                          uint32_t length, uint32_t to_offset = 0) {
    using CopyTraits = typename View<std::decay_t<CopyStorage>>::Traits;
    using ErrorType = CopyError<std::decay_t<CopyStorage>>;

    if (size_t size = size_bytes(); length > size || offset > size - length) {
      return fitx::error{ErrorType{.zbi_error = "offset + length exceeds ZBI size"}};
    } else if (to_offset + length < std::max(to_offset, length)) {
      return fitx::error{ErrorType{.zbi_error = "to_offset + length overflows"}};
    }

    if (auto result = CopyTraits::EnsureCapacity(to, to_offset + length); result.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot increase capacity",
          .write_offset = to_offset + length,
          .write_error = std::move(result).error_value(),
      }};
    }

    auto payload = Traits::Payload(storage(), offset, length);
    if (payload.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot translate ZBI offset to storage",
          .read_offset = offset,
          .read_error = std::move(std::move(payload).error_value()),
      }};
    }
    if constexpr (Traits::CanUnbufferedRead() && CopyTraits::CanUnbufferedWrite()) {
      // Combine buffered reading with mapped writing to do it all at once.
      auto mapped = CopyTraits::Write(to, to_offset, length);
      if (mapped.is_error()) {
        // No read error detected because a "write" error was detected first.
        return fitx::error{ErrorType{
            .zbi_error = "cannot write to destination storage",
            .write_offset = to_offset,
            .write_error = std::move(mapped).error_value(),
        }};
      }
      auto result = Traits::Read(storage(), payload.value(), mapped.value(), length);
      if (result.is_error()) {
        return fitx::error{ErrorType{
            .zbi_error = "cannot read from source storage",
            .read_offset = offset,
            .read_error = std::move(result).error_value(),
        }};
      }
      // No read error, no write error.
      return fitx::ok();
    } else {
      auto write = [&to, to_offset](ByteView chunk) mutable  //
          -> fitx::result<typename CopyTraits::ErrorTraits::error_type> {
        if (auto result = CopyTraits::Write(to, to_offset, chunk); result.is_error()) {
          return std::move(result).take_error();
        }
        to_offset += static_cast<uint32_t>(chunk.size());
        return fitx::ok();
      };
      auto result = Read(payload.value(), length, write);
      if (result.is_error()) {
        return fitx::error{ErrorType{
            .zbi_error = "cannot read from source storage",
            .read_offset = offset,
            .read_error = std::move(std::move(result).error_value()),
        }};
      }
      if (auto write_result = std::move(result).value(); write_result.is_error()) {
        return fitx::error{ErrorType{
            .zbi_error = "cannot write to destination storage",
            .write_offset = to_offset,
            .write_error = std::move(write_result).error_value(),
        }};
      }
      return fitx::ok();
    }
  }

  // Copy a range of the underlying storage into a freshly-created new piece of
  // storage (whatever that means for this storage type).  The Error return
  // value is for a read error.  The "success" return value indicates there was
  // no read error.  It's another fitx::result<read_error_type, T> for some
  // T akin to storage_type, possibly storage_type itself.  For example, all
  // the unowned VMO storage types yield zx::vmo as the owning equivalent
  // storage type.  If the optional `to_offset` argument is nonzero, the new
  // storage starts with that many zero bytes before the copied data.
  template <typename T = Traits,  // SFINAE check for Traits::Create method.
            typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<CopyError<CreateStorage>, CreateStorage> Copy(uint32_t offset, uint32_t length,
                                                             uint32_t to_offset = 0) {
    auto copy = CopyWithSlop(offset, length, to_offset,
                             [to_offset](uint32_t slop) { return slop == to_offset; });
    if (copy.is_error()) {
      return std::move(copy).take_error();
    }
    auto [new_storage, slop] = std::move(copy).value();
    ZX_DEBUG_ASSERT(slop == to_offset);
    return fitx::ok(std::move(new_storage));
  }

  // Copy a single item's payload into supplied storage.
  template <typename CopyStorage>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> CopyRawItem(CopyStorage&& to,
                                                                 const iterator& it) {
    return Copy(std::forward<CopyStorage>(to), it.payload_offset(), (*it).header->length);
  }

  // Copy a single item's payload into newly-created storage.
  template <  // SFINAE check for Traits::Create method.
      typename T = Traits,
      typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<CopyError<CreateStorage>, CreateStorage> CopyRawItem(const iterator& it) {
    return Copy(it.payload_offset(), (*it).header->length);
  }

  // Copy a single item's header and payload into supplied storage.
  template <typename CopyStorage>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> CopyRawItemWithHeader(CopyStorage&& to,
                                                                           const iterator& it) {
    return Copy(std::forward<CopyStorage>(to), it.item_offset(),
                sizeof(zbi_header_t) + (*it).header->length);
  }

  // Copy a single item's header and payload into newly-created storage.
  template <  // SFINAE check for Traits::Create method.
      typename T = Traits,
      typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<CopyError<CreateStorage>, CreateStorage> CopyRawItemWithHeader(const iterator& it) {
    return Copy(it.item_offset(), sizeof(zbi_header_t) + (*it).header->length);
  }

  // Copy a single item's payload into supplied storage, including
  // decompressing a ZBI_TYPE_STORAGE_* item if necessary.  This statically
  // determines based on the input and output storage types whether it has to
  // use streaming decompression or can use the one-shot mode (which is more
  // efficient and requires less scratch memory).  So the unused part of the
  // decompression library can be elided at link time.
  //
  // If decompression is necessary, then this calls `scratch(size_t{bytes})` to
  // allocate scratch memory for the decompression engine.  This returns
  // `fitx::result<std::string_view, T>` where T is any movable object that has
  // a `get()` method returning a pointer (of any type implicitly converted to
  // `void*`) to the scratch memory.  The returned object is destroyed after
  // decompression is finished and the scratch memory is no longer needed.
  //
  // zbitl::decompress:DefaultAllocator is a default-constructible class that
  // can serve as `scratch`.  The overloads below with fewer arguments use it.
  template <typename CopyStorage, typename ScratchAllocator>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> CopyStorageItem(CopyStorage&& to,
                                                                     const iterator& it,
                                                                     ScratchAllocator&& scratch) {
    if (auto compressed = IsCompressedStorage(*(*it).header)) {
      return DecompressStorage(std::forward<CopyStorage>(to), it,
                               std::forward<ScratchAllocator>(scratch));
    }
    return CopyRawItem(std::forward<CopyStorage>(to), it);
  }

  template <typename ScratchAllocator, typename T = Traits,
            typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<CopyError<CreateStorage>, CreateStorage> CopyStorageItem(
      const iterator& it, ScratchAllocator&& scratch) {
    using ErrorType = CopyError<CreateStorage>;

    if (auto compressed = IsCompressedStorage(*(*it).header)) {
      // Create new storage to decompress the payload into.
      auto to = Traits::Create(storage(), *compressed, 0);
      if (to.is_error()) {
        // No read error because a "write" error happened first.
        return fitx::error{ErrorType{
            .zbi_error = "cannot create storage",
            .write_offset = 0,
            .write_error = std::move(to).error_value(),
        }};
      }
      auto to_storage = std::move(to).value();
      if (auto result = DecompressStorage(to_storage, it, std::forward<ScratchAllocator>(scratch));
          result.is_error()) {
        return result.take_error();
      }
      return fitx::ok(std::move(to_storage));
    }
    return CopyRawItem(it);
  }

  // These overloads have the effect of default arguments for the allocator
  // arguments to the general versions above, but template argument deduction
  // doesn't work with default arguments.

  template <typename CopyStorage>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> CopyStorageItem(CopyStorage&& to,
                                                                     const iterator& it) {
    return CopyStorageItem(std::forward<CopyStorage>(to), it, decompress::DefaultAllocator);
  }

  template <typename T = Traits, typename = std::enable_if_t<T::CanCreate()>>
  auto CopyStorageItem(const iterator& it) {
    return CopyStorageItem(it, decompress::DefaultAllocator);
  }

  // Copy the subrange `[first,last)` of the ZBI into supplied storage.
  // The storage will contain a new ZBI container with only those items.
  template <typename CopyStorage>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> Copy(CopyStorage&& to, const iterator& first,
                                                          const iterator& last) {
    using CopyTraits = StorageTraits<std::decay_t<CopyStorage>>;
    using ErrorType = CopyError<std::decay_t<CopyStorage>>;

    auto [offset, length] = RangeBounds(first, last);
    if (auto result = Copy(to, offset, length, sizeof(zbi_header_t)); result.is_error()) {
      return std::move(result).take_error();
    }
    const zbi_header_t header = ZBI_CONTAINER_HEADER(length);
    ByteView out{reinterpret_cast<const std::byte*>(&header), sizeof(header)};
    if (auto result = CopyTraits::Write(to, 0, out); result.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot write container header",
          .write_offset = 0,
          .write_error = std::move(result).error_value(),
      }};
    }
    return fitx::ok();
  }

  // Copy the subrange `[first,last)` of the ZBI into newly-created storage.
  // The storage will contain a new ZBI container with only those items.
  template <typename T = Traits,
            typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<CopyError<CreateStorage>, CreateStorage> Copy(const iterator& first,
                                                             const iterator& last) {
    using CopyTraits = StorageTraits<CreateStorage>;
    using ErrorType = CopyError<CreateStorage>;

    auto [offset, length] = RangeBounds(first, last);

    // We allow the copy to leave padding ("slop") prior to the copied objects
    // if desired. This lets some storage backends to be more efficient (e.g.,
    // VMOs can clone pages instead of copying them).
    //
    // The amount of slop must be large enough for us to insert a container
    // header and possibly an additional discard item.
    constexpr auto slopcheck = [](uint32_t slop) {
      return slop == sizeof(zbi_header_t) ||
             (slop >= 2 * sizeof(zbi_header_t) && slop % ZBI_ALIGNMENT == 0);
    };
    auto copy = CopyWithSlop(offset, length, sizeof(zbi_header_t), slopcheck);
    if (copy.is_error()) {
      return std::move(copy).take_error();
    }
    auto [new_storage, slop] = std::move(copy).value();

    if (slop > sizeof(zbi_header_t)) {
      // Write out a discarded item header to take up all the slop left over
      // after the container header.
      ZX_DEBUG_ASSERT(slop >= 2 * sizeof(zbi_header_t));

      zbi_header_t hdr{};
      hdr.type = ZBI_TYPE_DISCARD;
      hdr.length = slop - (2 * sizeof(zbi_header_t));
      hdr = SanitizeHeader(hdr);
      ByteView out{reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr)};
      uint32_t to_offset = sizeof(zbi_header_t);
      if (auto result = CopyTraits::Write(new_storage, to_offset, out); result.is_error()) {
        return fitx::error{ErrorType{
            .zbi_error = "cannot write discard item",
            .write_offset = to_offset,
            .write_error = std::move(result).error_value(),
        }};
      }
      length += sizeof(zbi_header_t) + hdr.length;
    }

    // Write the new container header.
    const zbi_header_t hdr = ZBI_CONTAINER_HEADER(length);
    ByteView out{reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr)};
    if (auto result = CopyTraits::Write(new_storage, 0, out); result.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot write container header",
          .write_offset = 0,
          .write_error = std::move(result).error_value(),
      }};
    }

    return fitx::ok(std::move(new_storage));
  }

  // This is public mostly just for tests to assert on it.
  template <typename CopyStorage = Storage>
  static constexpr bool CanZeroCopy() {
    // Reading directly into buffer has no extra copies for a receiver that can
    // do unbuffered writes.
    using CopyTraits = typename View<std::decay_t<CopyStorage>>::Traits;
    return Traits::template CanOneShotRead<std::byte, /*LowLocality=*/false>() ||
           (Traits::CanUnbufferedRead() && CopyTraits::CanUnbufferedWrite());
  }

 protected:
  // Fetches the container header.
  fitx::result<storage_error_type,
               typename Traits::template LocalizedReadResult<container_header_type>>
  ContainerHeader() {
    return Traits::template LocalizedRead<container_header_type>(storage(), 0);
  }

  // Fetches an item header at a given offset.
  fitx::result<storage_error_type, typename Traits::template LocalizedReadResult<item_header_type>>
  ItemHeader(uint32_t offset) {
    return Traits::template LocalizedRead<item_header_type>(storage(), offset);
  }

  // WriteHeader sanitizes and optionally updates the length of a provided
  // header, writes it to the provided offset, and returns the modified header
  // on success.
  template <typename C = ContainerTraits, typename = std::enable_if_t<C::kIsMutable>>
  fitx::result<storage_error_type, item_header_type> WriteHeader(
      item_header_type header, uint32_t offset, std::optional<uint32_t> new_length = std::nullopt) {
    header = ContainerTraits::SanitizeHeader(header);
    if (new_length.has_value()) {
      ContainerTraits::SetPayloadSize(header, new_length.value());
    }
    if (auto result = Traits::Write(storage(), offset, AsBytes(header)); result.is_error()) {
      return fitx::error{std::move(result.error_value())};
    }
    return fitx::ok(header);
  }

 private:
  struct Unused {};
  struct NoError {};
  struct Taken {};
  using ErrorState = std::variant<Unused, NoError, Error, Taken>;

  void StartIteration() {
    ZX_ASSERT_MSG(!std::holds_alternative<Error>(error_),
                  "%s iterators used without taking prior error", ContainerTraits::kContainerType);
    error_ = NoError{};
  }

  void Fail(Error error) {
    ZX_DEBUG_ASSERT_MSG(!std::holds_alternative<Error>(error_),
                        "Fail in error state: missing %s::StartIteration() call?",
                        ContainerTraits::kContainerType);
    ZX_DEBUG_ASSERT_MSG(!std::holds_alternative<Unused>(error_),
                        "Fail in Unused: missing %s::StartIteration() call?",
                        ContainerTraits::kContainerType);
    error_ = std::move(error);
  }

  template <typename Callback>
  auto Read(payload_type payload, uint32_t length, Callback&& callback)
      -> fitx::result<storage_error_type, decltype(callback(ByteView{}))> {
    if constexpr (Traits::template CanOneShotRead<std::byte, /*LowLocality=*/false>()) {
      if (auto result = Traits::template Read<std::byte, false>(storage(), payload, length);
          result.is_error()) {
        return result.take_error();
      } else {
        return fitx::ok(callback(result.value()));
      }
    } else {
      return Traits::Read(storage(), payload, length, std::forward<Callback>(callback));
    }
  }

  template <typename SlopCheck,
            // SFINAE check for Traits::Create method.
            typename T = Traits,
            typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<CopyError<CreateStorage>, std::pair<CreateStorage, uint32_t>> CopyWithSlop(
      uint32_t offset, uint32_t length, uint32_t to_offset, SlopCheck&& slopcheck) {
    using ErrorType = CopyError<CreateStorage>;

    if (size_t size = size_bytes(); length > size || offset > size - length) {
      return fitx::error{ErrorType{.zbi_error = "offset + length exceeds ZBI size"}};
    } else if (to_offset + length < std::max(to_offset, length)) {
      return fitx::error{ErrorType{.zbi_error = "to_offset + length overflows"}};
    }

    if (auto result = Clone(offset, length, to_offset, std::forward<SlopCheck>(slopcheck));
        result.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot read from storage",
          .read_offset = offset,
          .read_error = std::move(result).error_value(),
      }};
    } else if (result.value()) {
      // Clone did the job!
      return fitx::ok(std::move(*std::move(result).value()));
    }

    // Fall back to Create and copy via Read and Write.
    if (auto result = Traits::Create(storage(), to_offset + length, to_offset); result.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot create storage",
          .read_offset = offset,
          .read_error = std::move(result).error_value(),
      }};
    } else {
      auto copy = std::move(result).value();
      static_assert(
          std::is_convertible_v<typename StorageTraits<decltype(copy)>::ErrorTraits::error_type,
                                storage_error_type>,
          "StorageTraits::Create yields type with incompatible error_type");
      auto copy_result = Copy(copy, offset, length, to_offset);
      if (copy_result.is_error()) {
        return std::move(copy_result).take_error();
      }
      return fitx::ok(std::make_pair(std::move(copy), uint32_t{to_offset}));
    }
  }

  template <typename SlopCheck, typename T = Traits>
  fitx::result<storage_error_type, typename T::template CloneResult<>> Clone(
      uint32_t offset, uint32_t length, uint32_t to_offset, SlopCheck&& slopcheck) {
    return Traits::Clone(storage(), offset, length, to_offset, std::forward<SlopCheck>(slopcheck));
  }

  // This overload is only used if SFINAE detected no Traits::Clone method.
  template <typename T = Traits,  // SFINAE check for Traits::Create method.
            typename CreateStorage = std::decay_t<typename T::template CreateResult<>>>
  fitx::result<storage_error_type, std::optional<std::pair<CreateStorage, uint32_t>>> Clone(...) {
    return fitx::ok(std::nullopt);  // Can't do it.
  }

  // Returns [offset, length] in the storage to cover the given item range.
  auto RangeBounds(const iterator& first, const iterator& last) {
    uint32_t offset = first.item_offset();
    uint32_t limit = limit_;
    if (last != end()) {
      limit = last.item_offset();
    }
    return std::make_pair(offset, limit - offset);
  }

  static constexpr std::optional<uint32_t> IsCompressedStorage(const zbi_header_t& header) {
    const bool compressible = TypeIsStorage(header.type);
    const bool compressed = header.flags & ZBI_FLAG_STORAGE_COMPRESSED;
    if (compressible && compressed) {
      return header.extra;
    }
    return std::nullopt;
  }

  template <typename CopyStorage, typename ScratchAllocator>
  fitx::result<CopyError<std::decay_t<CopyStorage>>> DecompressStorage(CopyStorage&& to,
                                                                       const iterator& it,
                                                                       ScratchAllocator&& scratch) {
    using ErrorType = CopyError<std::decay_t<CopyStorage>>;
    using ToTraits = typename View<std::decay_t<CopyStorage>>::Traits;
    constexpr bool bufferless_output = ToTraits::CanUnbufferedWrite();

    const auto [header, payload] = *it;
    const uint32_t compressed_size = header->length;
    const uint32_t uncompressed_size = header->extra;

    if (auto result = ToTraits::EnsureCapacity(to, uncompressed_size); result.is_error()) {
      return fitx::error{ErrorType{
          .zbi_error = "cannot increase capacity",
          .write_offset = uncompressed_size,
          .write_error = std::move(result).error_value(),
      }};
    }

    auto decompress_error = [&](auto&& result) {
      return fitx::error{ErrorType{
          .zbi_error = result.error_value(),
          .read_offset = it.item_offset(),
      }};
    };

    constexpr std::string_view kZbiErrorCorruptedOrBadData =
        "bad or corrupted data: uncompressed length not as expected";

    if constexpr (Traits::template CanOneShotRead<std::byte, /*LowLocality=*/false>()) {
      // All the data is on hand in one shot.  Fetch it first.
      ByteView compressed_data;
      if (auto result =
              Traits::template Read<std::byte, false>(storage(), payload, compressed_size);
          result.is_error()) {
        return fitx::error{ErrorType{
            .zbi_error = "cannot read compressed payload",
            .read_offset = it.item_offset(),
            .read_error = std::move(result).error_value(),
        }};
      } else {
        compressed_data = result.value();
      }

      if constexpr (bufferless_output) {
        // Decompression can write directly into the output storage in memory.
        // So this can use one-shot decompression.

        auto mapped = ToTraits::Write(to, 0, uncompressed_size);
        if (mapped.is_error()) {
          // Read succeeded, but write failed.
          return fitx::error{ErrorType{
              .zbi_error = "cannot write to storage in-place",
              .write_offset = 0,
              .write_error = std::move(mapped).error_value(),
          }};
        }
        const auto uncompressed_data = static_cast<std::byte*>(mapped.value());

        auto result =
            decompress::OneShot::Decompress({uncompressed_data, uncompressed_size}, compressed_data,
                                            std::forward<ScratchAllocator>(scratch));
        if (result.is_error()) {
          return decompress_error(result);
        }
      } else {
        // Writing to the output storage requires a temporary buffer.
        auto create_result = decompress::Streaming::Create<true>(
            compressed_data, std::forward<ScratchAllocator>(scratch));
        if (create_result.is_error()) {
          return decompress_error(create_result);
        }
        auto& decompress = create_result.value();
        uint32_t outoffset = 0;
        while (!compressed_data.empty()) {
          // Decompress as much data as the decompressor wants to.
          // It updates compressed_data to remove what it's consumed.
          ByteView out;
          if (auto result = decompress(compressed_data); result.is_error()) {
            return decompress_error(result);
          } else {
            out = {result.value().data(), result.value().size()};
          }
          if (!out.empty()) {
            // Flush the output buffer to the storage.
            if (auto write = ToTraits::Write(to, outoffset, out); write.is_error()) {
              // Read succeeded, but write failed.
              return fitx::error{ErrorType{
                  .zbi_error = "cannot write to storage",
                  .write_offset = outoffset,
                  .write_error = std::move(write).error_value(),
              }};
            }
            outoffset += static_cast<uint32_t>(out.size());
          }
        }

        if (outoffset != uncompressed_size) {
          return fitx::error{ErrorType{.zbi_error = kZbiErrorCorruptedOrBadData}};
        }
      }
    } else {
      std::byte* outptr = nullptr;
      size_t outlen = 0;
      uint32_t outoffset = 0;
      if constexpr (bufferless_output) {
        // Decompression can write directly into the output storage in memory.
        auto mapped = ToTraits::Write(to, 0, uncompressed_size);
        if (mapped.is_error()) {
          // Read succeeded, but write failed.
          return fitx::error{ErrorType{
              .zbi_error = "cannot write to storage in-place",
              .write_offset = 0,
              .write_error = std::move(mapped).error_value(),
          }};
        }

        outptr = static_cast<std::byte*>(mapped.value());
        outlen = uncompressed_size;
      }

      auto create = [&](ByteView probe) {
        return decompress::Streaming::Create<!bufferless_output>(
            probe, std::forward<ScratchAllocator>(scratch));
      };
      std::optional<std::decay_t<decltype(create({}).value())>> decompressor;

      // We have to read the first chunk just to decode its compression header.
      auto read_chunk = [&](ByteView chunk) -> fitx::result<ErrorType> {
        using ChunkError = fitx::error<ErrorType>;

        if (!decompressor) {
          // First chunk.  Set up the decompressor.
          if (auto result = create(chunk); result.is_error()) {
            return decompress_error(result);
          } else {
            decompressor.emplace(std::move(result).value());
          }
        }

        // Decompress the chunk.
        while (!chunk.empty()) {
          if constexpr (bufferless_output) {
            auto result = (*decompressor)({outptr, outlen}, chunk);
            if (result.is_error()) {
              return ChunkError(decompress_error(result));
            }
            outptr = result.value().data();
            outlen = result.value().size();
            outoffset += uncompressed_size - static_cast<uint32_t>(outlen);
          } else {
            ByteView out;
            if (auto result = (*decompressor)(chunk); result.is_error()) {
              return ChunkError(decompress_error(result));
            } else {
              out = {result.value().data(), result.value().size()};
            }
            if (!out.empty()) {
              // Flush the output buffer to the storage.
              auto write = ToTraits::Write(to, outoffset, out);
              if (write.is_error()) {
                // Read succeeded, but write failed.
                return ChunkError(ErrorType{
                    .zbi_error = "cannot write to storage",
                    .write_offset = outoffset,
                    .write_error = std::move(write).error_value(),
                });
              }
              outoffset += static_cast<uint32_t>(out.size());
            }
          }
        }

        if (outoffset != uncompressed_size) {
          return ChunkError(ErrorType{.zbi_error = kZbiErrorCorruptedOrBadData});
        }
        return fitx::ok();
      };

      auto result = Traits::Read(storage(), payload, compressed_size, read_chunk);
      if (result.is_error()) {
        return fitx::error{ErrorType{
            .zbi_error = "cannot read compressed payload",
            .read_offset = it.item_offset(),
            .read_error = std::move(result).error_value(),
        }};
      }

      auto read_chunk_result = std::move(result).value();
      if (read_chunk_result.is_error()) {
        return read_chunk_result.take_error();
      }
    }

    return fitx::ok();
  }

  storage_type storage_;
  ErrorState error_;
  uint32_t limit_ = 0;
};

// Deduction guide: View v(T{}) instantiates View<T>.
template <typename Storage>
explicit View(Storage) -> View<Storage>;

// Convert a pointer to an in-memory ZBI into a Storage type.
//
// We require that `zbi` is a pointer to a valid ZBI container header followed
// by its payload. Basic magic checks on the header are performed; if they
// fail, we return a Storage spanning just the header but no payload under the
// assumption that the "length" field of the header is invalid.
//
// The template parameter `Storage` may be any storage type that can be
// constructed with arguments the arguments (const std::byte*, size_t),
// representing the start and length of the in-memory ZBI.
template <typename Storage = ByteView>
Storage StorageFromRawHeader(const zbi_header_t* zbi) {
  if (zbi->magic != ZBI_ITEM_MAGIC || zbi->type != ZBI_TYPE_CONTAINER ||
      zbi->extra != ZBI_CONTAINER_MAGIC) {
    // Invalid header. Don't trust the `length` field.
    return Storage(reinterpret_cast<const std::byte*>(zbi), sizeof(zbi_header_t));
  }

  // Return Storage covering the entire header and payload.
  return Storage(reinterpret_cast<const std::byte*>(zbi), sizeof(zbi_header_t) + zbi->length);
}

}  // namespace zbitl

#endif  // LIB_ZBITL_VIEW_H_
