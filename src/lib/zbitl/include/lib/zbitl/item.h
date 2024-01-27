// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ZBITL_INCLUDE_LIB_ZBITL_ITEM_H_
#define SRC_LIB_ZBITL_INCLUDE_LIB_ZBITL_ITEM_H_

#include <lib/zbi-format/zbi.h>

#include <string_view>

namespace zbitl {

/// This returns the canonical name string for this zbi_header_t.type value.
/// It returns the default-constructed (empty()) string_view for unknown types.
std::string_view TypeName(uint32_t);
inline std::string_view TypeName(const zbi_header_t& header) { return TypeName(header.type); }

/// This returns the canonical file name extension string for this
/// zbi_header_t.type value.  It returns the default-constructed (i.e. empty())
/// string_view for unknown types.
std::string_view TypeExtension(uint32_t);
inline std::string_view TypeExtension(const zbi_header_t& header) {
  return TypeExtension(header.type);
}

/// Returns true for any kernel item type.
inline bool TypeIsKernel(uint32_t type) {
  return (type & ZBI_TYPE_KERNEL_MASK) == ZBI_TYPE_KERNEL_PREFIX;
}

inline bool TypeIsKernel(const zbi_header_t& header) { return TypeIsKernel(header.type); }

/// Returns true for any ZBI_TYPE_STORAGE_* type.
/// These share a protocol for other header fields, compression, etc.
bool TypeIsStorage(uint32_t);
inline bool TypeIsStorage(const zbi_header_t& header) { return TypeIsStorage(header.type); }

/// This returns the length of the item payload after decompression.
/// If this is not a ZBI_TYPE_STORAGE_* item, this is just `header.length`.
inline uint32_t UncompressedLength(const zbi_header_t& header) {
  return TypeIsStorage(header) ? header.extra : header.length;
}

}  // namespace zbitl

#endif  // SRC_LIB_ZBITL_INCLUDE_LIB_ZBITL_ITEM_H_
