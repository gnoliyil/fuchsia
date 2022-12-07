// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstddef>
#include <iterator>

#include <zxtest/zxtest.h>

#include "src/graphics/display/lib/edid/edid.h"
#include "src/graphics/display/lib/edid/test-support.h"

TEST(EdidTest, CaeValidationDtdOverflow) {
  edid::CeaEdidTimingExtension cea = {};
  cea.tag = edid::CeaEdidTimingExtension::kTag;
  cea.dtd_start_idx = 2;

  ASSERT_FALSE(cea.validate());
}

TEST(EdidTest, EisaVidLookup) {
  EXPECT_TRUE(!strcmp(edid::GetEisaVendorName(0x1e6d), "GOLDSTAR COMPANY LTD"));
  EXPECT_TRUE(!strcmp(edid::GetEisaVendorName(0x5a63), "VIEWSONIC CORPORATION"));
}

namespace {

// The I2C address for writing the DDC segment.
// VESA Enhanced Display Data Channel (E-DDC) Standard version 1.3 revised
// Dec 31 2020, Section 2.2.3 "DDC Addresses", page 17.
constexpr uint8_t kDdcSegmentI2cAddress = 0x30;

// The I2C address for writing the DDC data offset/reading DDC data
// VESA Enhanced Display Data Channel (E-DDC) Standard version 1.3 revised
// Dec 31 2020, Section 2.2.3 "DDC Addresses", page 17.
constexpr uint8_t kDdcDataI2cAddress = 0x50;

// Size of each data chunk in E-DDC sequential read sequence.
// VESA Enhanced Display Data Channel (E-DDC) Standard version 1.3 revised
// Dec 31 2020, Section 2.2.6.1 "DDC Operation", page 19.
constexpr int kBytesPerEdidBlock = 128;

// Size of each segment in E-DDC sequential read sequence.
// VESA Enhanced Display Data Channel (E-DDC) Standard version 1.3 revised
// Dec 31 2020, Section 2.2.5 "Segment Pointer", page 18.
constexpr int kBytesPerEdidSegment = 256;

// Simulates an EDID chip that can transfer E-EDID data over E-DDC protocol.
//
// This fake device only supports the recommended E-DDC access procedure
// specified in the E-DDC 1.3 standard, and will treat the other cases as
// failure:
// - Only segment address (0x30) and offset (0x50) can be written.
//   Drivers must only write one byte at a time.
// - Only data (0x50) can be read, and read must be in 128-byte chunks.
// - All the other E-DDC commands are invalid.
// - The buffer pointer provided must be valid.
// - The segment written at 0x30 and offset at 0x50 must not past the end of
//   provided EDID buffer.
class FakeDdcMemory {
 public:
  // Simulates an EDID chip storing the given data.
  explicit FakeDdcMemory(cpp20::span<const uint8_t> edid_data)
      : edid_data_(edid_data.begin(), edid_data.end()) {
    EXPECT_EQ(edid_data.size() % kBytesPerEdidBlock, 0u);
  }

  static bool i2c_transact(void* ctx, edid::ddc_i2c_msg_t* msgs, uint32_t msg_count) {
    FakeDdcMemory* const fake_ddc_memory = reinterpret_cast<FakeDdcMemory*>(ctx);
    return fake_ddc_memory->Transact(cpp20::span(msgs, msg_count));
  }

  size_t total_segment_write() const { return total_segment_write_; }
  size_t total_offset_write() const { return total_offset_write_; }
  size_t total_bytes_read() const { return total_bytes_read_; }

 private:
  bool Transact(cpp20::span<edid::ddc_i2c_msg_t> messages) {
    for (const auto& message : messages) {
      // Segment write at 0x30 past end of buffer? Fail
      // Offset write at 0x50 past end of buffer? Fail
      switch (message.addr) {
        case kDdcSegmentI2cAddress: {
          if (message.is_read) {
            ADD_FAILURE("Segment index must not be read");
            return false;
          }
          if (message.length != 1) {
            ADD_FAILURE("Invalid segment index length (%u); valid length is 1", message.length);
            return false;
          }
          if (message.buf == nullptr) {
            ADD_FAILURE("Invalid segment index pointer");
            return false;
          }

          uint8_t new_segment_index = *message.buf;
          size_t num_segments =
              (edid_data_.size() + kBytesPerEdidSegment - 1) / kBytesPerEdidSegment;
          if (new_segment_index >= num_segments) {
            ADD_FAILURE("Invalid segment index (%u); valid length must be less than %lu",
                        new_segment_index, num_segments);
            return false;
          }
          current_segment_index_ = new_segment_index;
          total_segment_write_++;

          break;
        }
        case kDdcDataI2cAddress:
          if (!message.is_read) {
            // Write data register to update offset.
            if (message.length != 1) {
              ADD_FAILURE("Invalid offset length (%u); valid length is 1", message.length);
              return false;
            }
            if (message.buf == nullptr) {
              ADD_FAILURE("Invalid offset buffer pointer");
              return false;
            }

            uint8_t new_offset_in_segment = *message.buf;
            if (new_offset_in_segment != 0x00 && new_offset_in_segment != 0x80) {
              ADD_FAILURE(
                  "Invalid offset value (%u); E-DDC standard should only "
                  "use 0x00 or 0x80 as offset",
                  new_offset_in_segment);
              return false;
            }

            size_t current_byte_location =
                current_segment_index_ * kBytesPerEdidSegment + new_offset_in_segment;
            if (current_byte_location >= edid_data_.size()) {
              ADD_FAILURE(
                  "Byte location (segment %u offset %u) out of range; "
                  "the byte location must not exceed EDID data size (%lu)",
                  current_segment_index_, new_offset_in_segment, edid_data_.size());
              return false;
            }
            current_byte_offset_in_segment_ = new_offset_in_segment;
            total_offset_write_++;

            break;
          } else {
            // Read EDID from I2C bus.
            if (message.length != 128) {
              ADD_FAILURE(
                  "Invalid EDID data length (%u); E-DDC recommends reading "
                  "EDID data in 128-byte chunks",
                  message.length);
              return false;
            }
            if (message.buf == nullptr) {
              ADD_FAILURE("Invalid EDID data buffer pointer");
              return false;
            }

            int start_byte_location =
                current_segment_index_ * kBytesPerEdidSegment + current_byte_offset_in_segment_;
            int end_byte_location = start_byte_location + message.length;
            auto begin = edid_data_.begin() + start_byte_location;
            auto end = edid_data_.begin() + end_byte_location;

            if (!(begin >= edid_data_.begin() && begin < edid_data_.end() &&
                  end > edid_data_.begin() && end <= edid_data_.end())) {
              ADD_FAILURE(
                  "Byte location [%d..%d] not in a valid range; "
                  "Byte location should be within [0..%lu]",
                  start_byte_location, start_byte_location + message.length, edid_data_.size() - 1);
            }

            std::copy(begin, end, message.buf);
            total_bytes_read_ += message.length;
            current_byte_offset_in_segment_ = 0u;

            break;
          }
          break;
        default:
          ADD_FAILURE("Invalid I2C address: 0x%02x", message.addr);
          return false;
      }
    }
    return true;
  }

  std::vector<uint8_t> edid_data_;

  uint8_t current_segment_index_ = 0;
  uint8_t current_byte_offset_in_segment_ = 0;

  size_t total_segment_write_ = 0;
  size_t total_offset_write_ = 0;
  size_t total_bytes_read_ = 0;
};

}  // namespace

TEST(EdidTest, ReadEdid_OneBlockOneSegment) {
  // One EDID blocks without extensions.
  FakeDdcMemory fake_ddc_memory(edid::kHpZr30wEdid);

  auto result = edid::ReadEdidFromDdcForTesting(&fake_ddc_memory, FakeDdcMemory::i2c_transact);
  ASSERT_FALSE(result.is_error, "Error while reading EDID: %s", result.error_message);

  const auto& edid_data_transacted = result.edid_bytes;
  ASSERT_EQ(edid_data_transacted.size(), 128u);
  EXPECT_BYTES_EQ(edid::kHpZr30wEdid, edid_data_transacted.data(), 128);

  EXPECT_EQ(fake_ddc_memory.total_segment_write(), 0);
  EXPECT_EQ(fake_ddc_memory.total_offset_write(), 1);
  EXPECT_EQ(fake_ddc_memory.total_bytes_read(), 128);
}

TEST(EdidTest, ReadEdid_TwoBlocksOneSegment) {
  // 2 EDID blocks, including one extension block.
  FakeDdcMemory fake_ddc_memory(edid::kDellP2719hEdid);

  auto result = edid::ReadEdidFromDdcForTesting(&fake_ddc_memory, FakeDdcMemory::i2c_transact);
  ASSERT_FALSE(result.is_error, "Error while reading EDID: %s", result.error_message);

  const auto& edid_data_transacted = result.edid_bytes;
  ASSERT_EQ(edid_data_transacted.size(), 256u);
  EXPECT_BYTES_EQ(edid::kDellP2719hEdid, edid_data_transacted.data(), 256);

  EXPECT_EQ(fake_ddc_memory.total_segment_write(), 0);
  EXPECT_EQ(fake_ddc_memory.total_offset_write(), 2);
  EXPECT_EQ(fake_ddc_memory.total_bytes_read(), 256);
}

TEST(EdidTest, ReadEdid_MultiSegment) {
  // 4 EDID blocks, including 3 extension blocks.
  FakeDdcMemory fake_ddc_memory(edid::kSamsungCrg9Edid);

  auto result = edid::ReadEdidFromDdcForTesting(&fake_ddc_memory, FakeDdcMemory::i2c_transact);
  ASSERT_FALSE(result.is_error, "Error while reading EDID: %s", result.error_message);

  const auto& edid_data_transacted = result.edid_bytes;
  ASSERT_EQ(edid_data_transacted.size(), 512u);
  EXPECT_BYTES_EQ(edid::kSamsungCrg9Edid, edid_data_transacted.data(), 512);

  EXPECT_EQ(fake_ddc_memory.total_segment_write(), 2);
  EXPECT_EQ(fake_ddc_memory.total_offset_write(), 4);
  EXPECT_EQ(fake_ddc_memory.total_bytes_read(), 512);
}
