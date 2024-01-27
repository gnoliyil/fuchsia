// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_COMMON_STRING_UTIL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_COMMON_STRING_UTIL_H_

#include <string_view>

#include "src/developer/debug/zxdb/common/int128_t.h"

namespace zxdb {

// This is a version of std::to_string for hex numbers. The output is always treated as unsigned
// so signed negative numbers will be the two's compliment using printf rules.
//
// This has two options: digits is the number of digits to 0-pad out to. Use 0 for no zero-padding.
// include_prefix (the default) will prepend "0x" to the output. Otherwise the output will have no
// prefix.
std::string to_hex_string(int8_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(uint8_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(int16_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(uint16_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(int32_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(uint32_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(int64_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(uint64_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(int128_t i, int digits = 0, bool include_prefix = true);
std::string to_hex_string(uint128_t i, int digits = 0, bool include_prefix = true);

// Similar to to_hex_string but additional can optionally write byte separators. If nonzero, the
// given byte_separator characer will be output for every 8 bits.
template <typename T>
std::string to_bin_string(T i, int digits = 0, bool include_prefix = true, char byte_separator = 0);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_COMMON_STRING_UTIL_H_
