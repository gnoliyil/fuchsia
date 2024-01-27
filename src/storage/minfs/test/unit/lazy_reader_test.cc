// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/minfs/lazy_reader.h"

#include <lib/fit/defer.h>

#include <gtest/gtest.h>
#include <storage/buffer/resizeable_vmo_buffer.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/minfs/bcache.h"
#include "src/storage/minfs/writeback.h"

namespace minfs {
namespace {

using block_client::FakeBlockDevice;

class Mapper : public MapperInterface {
 public:
  zx::result<DeviceBlockRange> Map(BlockRange range) {
    auto iter = mappings_.find(range.Start());
    if (iter != mappings_.end()) {
      return zx::ok(DeviceBlockRange(iter->second, 1));
    } else {
      return zx::ok(DeviceBlockRange(DeviceBlock::Unmapped(), 1));
    }
  }

  zx::result<DeviceBlockRange> MapForWrite(PendingWork* transaction, BlockRange range,
                                           bool* allocated) {
    EXPECT_FALSE(transaction == nullptr);
    EXPECT_TRUE(*allocated == false);
    auto iter = mappings_.find(range.Start());
    if (iter == mappings_.end()) {
      EXPECT_LT(range.Start(), 10ul);
      // Reverses and spaces every logical block out by 2.
      DeviceBlockRange device_range = DeviceBlockRange(20 - range.Start() * 2, 1);
      mappings_[range.Start()] = device_range.block();
      *allocated = true;
      return zx::ok(device_range);
    } else {
      return zx::ok(DeviceBlockRange(iter->second, 1));
    }
  }

  const std::map<size_t, size_t>& mappings() const { return mappings_; }

 private:
  std::map<size_t, size_t> mappings_;
};

class StubTransaction : public PendingWork {
 public:
  void EnqueueMetadata(storage::Operation operation, storage::BlockBuffer* buffer) override {}
  void EnqueueData(storage::Operation operation, storage::BlockBuffer* buffer) override {}
  size_t AllocateBlock() override { return 0; }
  void DeallocateBlock(size_t) override {}
};

TEST(LazyReaderTest, ReadSucceeds) {
  static const int kBlockCount = 21;
  auto device = std::make_unique<FakeBlockDevice>(kBlockCount, kMinfsBlockSize);
  auto bcache_or = Bcache::Create(std::move(device), kBlockCount);
  ASSERT_TRUE(bcache_or.is_ok());
  // Write to logical block 1.
  char data[kMinfsBlockSize] = "hello";
  StubTransaction transaction;
  Mapper mapper;
  bool allocated = false;
  DeviceBlockRange device_range =
      mapper.MapForWrite(&transaction, BlockRange(1, 2), &allocated).value();
  ASSERT_TRUE(bcache_or->Writeblk(static_cast<blk_t>(device_range.block()), data).is_ok());

  // Now read the data back using the lazy_reader.
  storage::ResizeableVmoBuffer buffer(kMinfsBlockSize);
  ASSERT_TRUE(buffer.Attach("LazyReaderTest", bcache_or.value().get()).is_ok());
  auto detach =
      fit::defer([&]() { [[maybe_unused]] auto _ = buffer.Detach(bcache_or.value().get()); });
  ASSERT_TRUE(buffer.Grow(kBlockCount).is_ok());
  MappedFileReader reader(bcache_or.value().get(), &mapper, &buffer);
  LazyReader lazy_reader;
  ASSERT_TRUE(lazy_reader.Read(ByteRange(kMinfsBlockSize, kMinfsBlockSize + 6), &reader).is_ok());

  // We should see the same data read back.
  EXPECT_EQ(memcmp(buffer.Data(1), "hello", 6), 0);
}

TEST(LazyReaderTest, UnmappedBlockIsZeroed) {
  static const int kBlockCount = 21;
  auto device = std::make_unique<FakeBlockDevice>(kBlockCount, kMinfsBlockSize);
  auto bcache_or = Bcache::Create(std::move(device), kBlockCount);
  ASSERT_TRUE(bcache_or.is_ok());

  storage::ResizeableVmoBuffer buffer(kMinfsBlockSize);
  ASSERT_TRUE(buffer.Attach("LazyReaderTest", bcache_or.value().get()).is_ok());
  auto detach =
      fit::defer([&]() { [[maybe_unused]] auto _ = buffer.Detach(bcache_or.value().get()); });
  *static_cast<uint8_t*>(buffer.Data(0)) = 0xab;
  Mapper mapper;
  MappedFileReader reader(bcache_or.value().get(), &mapper, &buffer);
  LazyReader lazy_reader;

  // There is no mapping for the first block so it should get zeroed.
  EXPECT_TRUE(lazy_reader.Read(ByteRange(0, 1), &reader).is_ok());
  EXPECT_EQ(0, *static_cast<uint8_t*>(buffer.Data(0)));

  // Reading again should not zero it again.
  *static_cast<uint8_t*>(buffer.Data(0)) = 0xab;
  EXPECT_TRUE(lazy_reader.Read(ByteRange(0, 1), &reader).is_ok());
  EXPECT_EQ(0xab, *static_cast<uint8_t*>(buffer.Data(0)));
}

class MockReader : public LazyReader::ReaderInterface {
 public:
  zx::result<uint64_t> Enqueue(BlockRange range) override {
    if (return_error_for_enqueue_) {
      return zx::error(ZX_ERR_NO_MEMORY);
    }
    enqueued_.push_back(range);
    return zx::ok(range.Length());
  }

  // Issues the queued reads and returns the result.
  zx::result<> RunRequests() override {
    if (return_error_for_run_requests_) {
      return zx::error(ZX_ERR_IO);
    }
    run_requests_called_ = true;
    return zx::ok();
  }

  uint32_t BlockSize() const override { return 512; }

  void Reset() {
    enqueued_.clear();
    run_requests_called_ = false;
    return_error_for_enqueue_ = false;
    return_error_for_run_requests_ = false;
  }

  const std::vector<BlockRange>& enqueued() const { return enqueued_; }
  bool run_requests_called() const { return run_requests_called_; }

  void set_return_error_for_enqueue(bool return_error) { return_error_for_enqueue_ = return_error; }
  void set_return_error_for_run_requests(bool return_error) {
    return_error_for_run_requests_ = return_error;
  }

 private:
  std::vector<BlockRange> enqueued_;
  bool run_requests_called_ = false;
  bool return_error_for_enqueue_ = false;
  bool return_error_for_run_requests_ = false;
};

TEST(LazyReaderTest, ZeroLengthReadIsNotEnqeued) {
  LazyReader lazy_reader;
  MockReader reader;

  ASSERT_TRUE(lazy_reader.Read(ByteRange(100, 100), &reader).is_ok());
  EXPECT_EQ(reader.enqueued().size(), 0ul);
}

TEST(LazyReaderTest, ReadForMutlipleBlocksAfterOneBlockReadEnqueuedCorrectly) {
  LazyReader lazy_reader;
  MockReader reader;

  // Read one block.
  ASSERT_TRUE(lazy_reader.Read(ByteRange(530, 531), &reader).is_ok());
  ASSERT_EQ(reader.enqueued().size(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].Start(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].End(), 2ul);
  EXPECT_TRUE(reader.run_requests_called());
  reader.Reset();

  // Now read through blocks 0 to 4.
  ASSERT_TRUE(
      lazy_reader.Read(ByteRange(reader.BlockSize() - 1, 4 * reader.BlockSize() + 1), &reader)
          .is_ok());

  // We read one block earlier which shouldn't read again. This should result in Enqueue(0, 1),
  // Enqueue(2, 5).
  ASSERT_EQ(reader.enqueued().size(), 2ul);
  EXPECT_EQ(reader.enqueued()[0].Start(), 0ul);
  EXPECT_EQ(reader.enqueued()[0].End(), 1ul);
  EXPECT_EQ(reader.enqueued()[1].Start(), 2ul);
  EXPECT_EQ(reader.enqueued()[1].End(), 5ul);
}

TEST(LazyReaderTest, EnqueueError) {
  LazyReader lazy_reader;
  MockReader reader;
  reader.set_return_error_for_enqueue(true);
  EXPECT_EQ(lazy_reader.Read(ByteRange(530, 531), &reader).status_value(), ZX_ERR_NO_MEMORY);
  EXPECT_FALSE(reader.run_requests_called());
  reader.Reset();

  // If we try again with no error, it should proceed with the read.
  ASSERT_TRUE(lazy_reader.Read(ByteRange(530, 531), &reader).is_ok());
  ASSERT_EQ(reader.enqueued().size(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].Start(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].End(), 2ul);
  EXPECT_TRUE(reader.run_requests_called());
}

TEST(LazyReaderTest, RunRequestsError) {
  LazyReader lazy_reader;
  MockReader reader;
  reader.set_return_error_for_run_requests(true);
  EXPECT_EQ(lazy_reader.Read(ByteRange(530, 531), &reader).status_value(), ZX_ERR_IO);
  EXPECT_FALSE(reader.run_requests_called());
  reader.Reset();

  // If we try again with no error, it should proceed with the read.
  ASSERT_TRUE(lazy_reader.Read(ByteRange(530, 531), &reader).is_ok());
  ASSERT_EQ(reader.enqueued().size(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].Start(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].End(), 2ul);
  EXPECT_TRUE(reader.run_requests_called());
}

TEST(LazyReaderTest, SetLoadedMarksBlocksAsLoaded) {
  LazyReader lazy_reader;

  lazy_reader.SetLoaded(BlockRange(1, 2), true);

  MockReader reader;
  ASSERT_TRUE(
      lazy_reader.Read(ByteRange(reader.BlockSize() - 1, 2 * reader.BlockSize() + 1), &reader)
          .is_ok());
  ASSERT_EQ(reader.enqueued().size(), 2ul);
  EXPECT_EQ(reader.enqueued()[0].Start(), 0ul);
  EXPECT_EQ(reader.enqueued()[0].End(), 1ul);
  EXPECT_EQ(reader.enqueued()[1].Start(), 2ul);
  EXPECT_EQ(reader.enqueued()[1].End(), 3ul);
}

TEST(LazyReaderTest, ClearLoadedMarksBlocksAsNotLoaded) {
  LazyReader lazy_reader;
  MockReader reader;
  ASSERT_TRUE(
      lazy_reader.Read(ByteRange(reader.BlockSize() - 1, 2 * reader.BlockSize() + 1), &reader)
          .is_ok());

  lazy_reader.SetLoaded(BlockRange(1, 2), false);

  reader.Reset();
  ASSERT_TRUE(
      lazy_reader.Read(ByteRange(reader.BlockSize() - 1, 2 * reader.BlockSize() + 1), &reader)
          .is_ok());
  ASSERT_EQ(reader.enqueued().size(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].Start(), 1ul);
  EXPECT_EQ(reader.enqueued()[0].End(), 2ul);
}

}  // namespace
}  // namespace minfs
