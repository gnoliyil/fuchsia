// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "nandpart-utils.h"

#include <lib/stdcompat/span.h>
#include <zircon/types.h>

#include <memory>

#include <zxtest/zxtest.h>

namespace nand {
namespace {

constexpr uint32_t kPageSize = ZX_PAGE_SIZE;
constexpr uint32_t kPagesPerBlock = 2;
constexpr uint32_t kNumBlocks = 5;
constexpr uint32_t kOobSize = 8;
constexpr nand_info_t kNandInfo = {
    .page_size = kPageSize,
    .pages_per_block = kPagesPerBlock,
    .num_blocks = kNumBlocks,
    .ecc_bits = 2,
    .oob_size = kOobSize,
    .nand_class = NAND_CLASS_BBS,
    .partition_guid = {},
};

zbi_partition_map_t MakePartitionMap(uint32_t partition_count) {
  return zbi_partition_map_t{
      .block_count = kNumBlocks * kPagesPerBlock,
      .block_size = kPageSize,
      .partition_count = partition_count,
      .reserved = 0,
      .guid = {},
  };
}

zbi_partition_t MakePartition(uint32_t first_block, uint32_t last_block) {
  return zbi_partition_t{
      .type_guid = {},
      .uniq_guid = {},
      .first_block = first_block,
      .last_block = last_block,
      .flags = 0,
      .name = {},
  };
}

cpp20::span<zbi_partition_t> GetPartitions(zbi_partition_map_t* pmap) {
  static_assert(alignof(zbi_partition_map_t) >= alignof(zbi_partition_t));
  return {reinterpret_cast<zbi_partition_t*>(pmap + 1), pmap->partition_count};
}

void ValidatePartition(zbi_partition_map_t* pmap, size_t partition_number, uint32_t first_block,
                       uint32_t last_block) {
  auto partitions = GetPartitions(pmap);
  EXPECT_EQ(partitions[partition_number].first_block, first_block);
  EXPECT_EQ(partitions[partition_number].last_block, last_block);
}

TEST(NandPartUtilsTest, SanitizeEmptyPartitionMapTest) {
  auto pmap = MakePartitionMap(0);
  ASSERT_NE(SanitizePartitionMap(&pmap, kNandInfo), ZX_OK);
}

TEST(NandPartUtilsTest, SanitizeSinglePartitionMapTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(1);
  GetPartitions(pmap)[0] = MakePartition(0, 9);
  ASSERT_OK(SanitizePartitionMap(pmap, kNandInfo));
  ASSERT_NO_FATAL_FAILURE(ValidatePartition(pmap, 0, 0, 4));
}

TEST(NandPartUtilsTest, SanitizeMultiplePartitionMapTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + 3 * sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(3);
  auto partitions = GetPartitions(pmap);
  partitions[0] = MakePartition(0, 3);
  partitions[1] = MakePartition(4, 7);
  partitions[2] = MakePartition(8, 9);

  ASSERT_OK(SanitizePartitionMap(pmap, kNandInfo));
  ASSERT_NO_FATAL_FAILURE(ValidatePartition(pmap, 0, 0, 1));
  ASSERT_NO_FATAL_FAILURE(ValidatePartition(pmap, 1, 2, 3));
  ASSERT_NO_FATAL_FAILURE(ValidatePartition(pmap, 2, 4, 4));
}

TEST(NandPartUtilsTest, SanitizeMultiplePartitionMapOutOfOrderTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + 2 * sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(2);
  auto partitions = GetPartitions(pmap);
  partitions[0] = MakePartition(4, 9);
  partitions[1] = MakePartition(0, 3);

  ASSERT_OK(SanitizePartitionMap(pmap, kNandInfo));
  ASSERT_NO_FATAL_FAILURE(ValidatePartition(pmap, 0, 0, 1));
  ASSERT_NO_FATAL_FAILURE(ValidatePartition(pmap, 1, 2, 4));
}

TEST(NandPartUtilsTest, SanitizeMultiplePartitionMapOverlappingTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + 3 * sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(3);
  auto partitions = GetPartitions(pmap);
  partitions[0] = MakePartition(0, 3);
  partitions[1] = MakePartition(8, 9);
  partitions[2] = MakePartition(4, 8);

  ASSERT_NE(SanitizePartitionMap(pmap, kNandInfo), ZX_OK);
}

TEST(NandPartUtilsTest, SanitizePartitionMapBadRangeTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + 2 * sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(2);
  auto partitions = GetPartitions(pmap);
  partitions[0] = MakePartition(1, 0);
  partitions[1] = MakePartition(1, 9);

  ASSERT_NE(SanitizePartitionMap(pmap, kNandInfo), ZX_OK);
}

TEST(NandPartUtilsTest, SanitizePartitionMapUnalignedTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + 2 * sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(2);
  auto partitions = GetPartitions(pmap);
  partitions[0] = MakePartition(0, 3);
  partitions[1] = MakePartition(5, 8);

  ASSERT_NE(SanitizePartitionMap(pmap, kNandInfo), ZX_OK);
}

TEST(NandPartUtilsTest, SanitizePartitionMapOutofBoundsTest) {
  std::unique_ptr<uint8_t[]> pmap_buffer(
      new uint8_t[sizeof(zbi_partition_map_t) + 2 * sizeof(zbi_partition_t)]);
  auto* pmap = reinterpret_cast<zbi_partition_map_t*>(pmap_buffer.get());
  *pmap = MakePartitionMap(2);
  auto partitions = GetPartitions(pmap);
  partitions[0] = MakePartition(0, 3);
  partitions[1] = MakePartition(4, 11);

  ASSERT_NE(SanitizePartitionMap(pmap, kNandInfo), ZX_OK);
}

}  // namespace
}  // namespace nand
