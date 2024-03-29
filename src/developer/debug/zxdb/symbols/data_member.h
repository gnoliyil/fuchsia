// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DATA_MEMBER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DATA_MEMBER_H_

#include "src/developer/debug/zxdb/symbols/file_line.h"
#include "src/developer/debug/zxdb/symbols/value.h"

namespace zxdb {

// Represents a data member in a class. Not to be confused with function parameters and local
// variables which are represented by a Variable.
//
// The type and name come from the Value base class.
class DataMember final : public Value {
 public:
  // Construct with fxl::MakeRefCounted().

  // The byte offset from the containing class or struct of this data member. This is only valid
  // if !is_external() and data_bit_offset is not set -- see the base class' Value::is_external().
  uint32_t member_location() const { return member_location_; }
  void set_member_location(uint32_t m) { member_location_ = m; }

  bool is_bitfield() const { return bit_size_ != 0; }

  // The DW_AT_data_bit_offset is used in DWARF 4 to indicate the bit position of a member bitfield.
  // Unlike bit_offset, this will measure from the beginning of the containing structure.
  //
  // If data_bit_offset is set, then member_location, byte_size, and bit_offset will not be set.
  uint32_t data_bit_offset() const { return data_bit_offset_; }
  void set_data_bit_offset(uint32_t dbo) { data_bit_offset_ = dbo; }

  // The DW_AT_byte_size attribute on the data member. It won't be set if data_bit_offset is set.
  //
  // Normally the byte size will be 0 which means to take the size from the type. For bitfields
  // this will indicate the size of the larger field containing the bitfield, and the bit_offset
  // will count from the high bit inside this byte_size (where byte_size is read as little-endian
  // on little-endian machines).
  //
  // As of this writing I've only ever seen GCC and Clang generated byte_size() that matches the
  // type().byte_size() even when the actual data occupies more physical bytes than the size of
  // the type (because it's not aligned). This case will generated negative bit offsets.
  //
  // Note that for bitfields reading this many bytes may read off the end of the structure. For
  // example:
  //
  //   struct MyStruct {
  //     long long data : 2;
  //   };
  //
  // MyStruct will have a byte_size of 1, while MyStruct.data will have a byte_size of 8! The bit
  // offset will select the value bits from the structure.
  uint32_t byte_size() const { return byte_size_; }
  void set_byte_size(uint32_t bs) { byte_size_ = bs; }

  // Bit offset from the member_location(). This is counting from the high bit of the byte_size().
  // When that many bytes is read into memory (swapping the byte order for little-endian).
  //
  // As an example, a little-endian "int foo : 3;" might be defined as having:
  //   - byte_size = 4
  //   - bit_offset = 29
  //   - bit_size = 3
  //   - data_bit_offset = 0
  //
  // Bit layout (stored in the low 3 bits of a 32-bit integer):
  //
  //   Byte 0 (low)      Byte 1             Byte 2            Byte 3 (high bits)
  //   7 6 5 4 3 2 1 0   7 6 5 4 3 2 1 0    7 6 5 4 3 2 1 0   7 6 5 4 3 2 1 0
  //             x x x
  //
  // It can be negative which means it starts *beyond* the byte_size(). I have not seen this be
  // more than one byte extra since otherwise the address will likely be one greater. For example:
  //
  //   struct {
  //     // member_location = 0
  //     // byte_size = 1
  //     // bit_size = 6
  //     // bit_offset = 2
  //     // data_bit_offset = 0
  //     bool a : 6;
  //
  //     // member_location = 0
  //     // byte_size = 1
  //     // bit_size = 7
  //     // bit_offset = -5
  //     // data_bit_offset = 6
  //     bool b : 7;
  //   };
  //
  // Bit layout:
  //
  //   Byte 0            Byte 1
  //   7 6 5 4 3 2 1 0   7 6 5 4 3 2 1 0
  //   b b a a a a a a         b b b b b
  //                           ^ -5 bits in little-endian from the high bit of byte 0.
  int32_t bit_offset() const { return bit_offset_; }
  void set_bit_offset(int32_t bo) { bit_offset_ = bo; }

  // Number of bits that count. 0 means all.
  uint32_t bit_size() const { return bit_size_; }
  void set_bit_size(uint32_t bs) { bit_size_ = bs; }

  // The location of the declaration, used by Rust generators.
  const FileLine& decl_line() const { return decl_line_; }
  void set_decl_line(FileLine decl) { decl_line_ = std::move(decl); }

 protected:
  // Symbol protected overrides.
  const DataMember* AsDataMember() const override;

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(DataMember);
  FRIEND_MAKE_REF_COUNTED(DataMember);

  DataMember();
  DataMember(const std::string& assigned_name, LazySymbol type, uint32_t member_loc);
  ~DataMember();

  // TODO: use std::optional to distinguish unset values and 0.
  uint32_t member_location_ = 0;
  uint32_t data_bit_offset_ = 0;
  uint32_t byte_size_ = 0;
  int32_t bit_offset_ = 0;
  uint32_t bit_size_ = 0;
  FileLine decl_line_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DATA_MEMBER_H_
