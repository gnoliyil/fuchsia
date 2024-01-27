// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <lib/zxio/zxio.h>
#include <limits.h>
#include <zircon/compiler.h>

#include <zxtest/zxtest.h>

#include "sdk/lib/zxio/private.h"

constexpr size_t kSize = 300;
constexpr zx_off_t kInitialSeek = 4;

constexpr const char* ALPHABET = "abcdefghijklmnopqrstuvwxyz";

TEST(Vmo, Create) {
  zx::vmo backing;
  ASSERT_OK(zx::vmo::create(kSize, 0u, &backing));

  zxio_storage_t storage;
  ASSERT_OK(zxio_create(backing.release(), &storage));
  zxio_t* io = &storage.io;

  zxio_node_attributes_t attr = {};
  ASSERT_OK(zxio_attr_get(io, &attr));
  EXPECT_EQ(kSize, attr.content_size);
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_attr_set(io, &attr));

  ASSERT_OK(zxio_close(io, /*should_wait=*/true));
}

class VmoTest : public zxtest::Test {
 public:
  void SetUp() override {
    ASSERT_OK(zx::vmo::create(kSize, 0u, &backing));
    ASSERT_OK(backing.write(ALPHABET, 0, len));
    ASSERT_OK(backing.write(ALPHABET, len, len + len));
    zx::stream stream;
    ASSERT_OK(zx::stream::create(ZX_STREAM_MODE_READ | ZX_STREAM_MODE_WRITE, backing, kInitialSeek,
                                 &stream));
    ASSERT_OK(zxio_vmo_init(&storage, std::move(backing), std::move(stream)));
    io = &storage.io;
  }

  void TearDown() override { ASSERT_OK(zxio_close(io, /*should_wait=*/true)); }

 protected:
  zx::vmo backing;
  size_t len = strlen(ALPHABET);
  zxio_storage_t storage;
  zxio_t* io;
};

TEST_F(VmoTest, Basic) {
  zxio_signals_t observed = ZXIO_SIGNAL_NONE;
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED,
                zxio_wait_one(io, ZXIO_SIGNAL_READABLE, ZX_TIME_INFINITE, &observed));

  zx::channel clone;
  ASSERT_OK(zxio_clone(io, clone.reset_and_get_address()));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_sync(io));

  zxio_node_attributes_t attr = {};
  ASSERT_OK(zxio_attr_get(io, &attr));
  EXPECT_EQ(kSize, attr.content_size);
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_attr_set(io, &attr));

  char buffer[1024];
  memset(buffer, 0, sizeof(buffer));
  size_t actual = 0u;
  ASSERT_OK(zxio_read(io, buffer, 8, 0, &actual));
  EXPECT_EQ(actual, 8);
  EXPECT_STREQ("efghijkl", buffer);
  memset(buffer, 0, sizeof(buffer));
  actual = 0u;
  ASSERT_OK(zxio_read_at(io, 1u, buffer, 6, 0, &actual));
  EXPECT_EQ(actual, 6);
  EXPECT_STREQ("bcdefg", buffer);

  size_t offset = 2u;
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_START, 2, &offset));
  EXPECT_EQ(offset, 2u);
  memset(buffer, 0, sizeof(buffer));
  actual = 0u;
  ASSERT_OK(zxio_read(io, buffer, 3, 0, &actual));
  EXPECT_STREQ("cde", buffer);
  ASSERT_STATUS(ZX_ERR_UNAVAILABLE, zxio_truncate(io, 0u));
  uint32_t flags = 0u;
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_flags_get(io, &flags));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_flags_set(io, flags));

  ASSERT_OK(zxio_write(io, buffer, sizeof(buffer), 0, &actual));
  EXPECT_EQ(actual, sizeof(buffer));
  ASSERT_OK(zxio_write_at(io, 0u, buffer, sizeof(buffer), 0, &actual));
  EXPECT_EQ(actual, sizeof(buffer));

  constexpr std::string_view name("hello");
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED,
                zxio_open_async(io, {}, name.data(), name.length(), ZX_HANDLE_INVALID));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_unlink(io, name.data(), name.length(), 0));
}

TEST_F(VmoTest, GetCopy) {
  zx::vmo vmo;
  ASSERT_OK(zxio_vmo_get_copy(io, vmo.reset_and_get_address()));
  EXPECT_NE(vmo.get(), ZX_HANDLE_INVALID);
  uint64_t size;
  ASSERT_OK(vmo.get_prop_content_size(&size));
  EXPECT_EQ(size, kSize);
}

TEST_F(VmoTest, GetClone) {
  zx::vmo vmo;
  ASSERT_STATUS(ZX_OK, zxio_vmo_get_clone(io, vmo.reset_and_get_address()));
  EXPECT_NE(vmo.get(), ZX_HANDLE_INVALID);
  uint64_t size;
  ASSERT_OK(vmo.get_prop_content_size(&size));
  EXPECT_EQ(size, kSize);
}

TEST_F(VmoTest, GetExact) {
  zx::vmo vmo;
  ASSERT_STATUS(ZX_OK, zxio_vmo_get_exact(io, vmo.reset_and_get_address()));
  EXPECT_NE(vmo.get(), ZX_HANDLE_INVALID);
  uint64_t size;
  ASSERT_OK(vmo.get_prop_content_size(&size));
  EXPECT_EQ(size, kSize);
}

TEST_F(VmoTest, SeekNegativeOverflow) {
  // We set up a large negative seek (larger than the page-rounded-up size of the VMO underlying
  // us).
  constexpr int64_t kTooFarBackwards = -8192;

  // Seek somewhere slightly more random than the start.
  size_t original_seek = 23;
  size_t new_seek = 42;
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_START, original_seek, &new_seek));
  ASSERT_EQ(original_seek, new_seek);

  // Seeking backwards from the start past zero should fail, without moving the seek pointer.
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                zxio_seek(io, ZXIO_SEEK_ORIGIN_START, kTooFarBackwards, &new_seek));
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, 0, &new_seek));
  ASSERT_EQ(original_seek, new_seek);

  new_seek = 42;

  // Seeking backwards from the seek pointer past zero should fail, without moving the seek pointer.
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, kTooFarBackwards, &new_seek));
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, 0, &new_seek));
  ASSERT_EQ(original_seek, new_seek);

  new_seek = 42;

  // Seeking backwards from the end past zero should fail, without moving the seek pointer.
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                zxio_seek(io, ZXIO_SEEK_ORIGIN_END, kTooFarBackwards, &new_seek));
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, 0, &new_seek));
  ASSERT_EQ(original_seek, new_seek);
}

// This sets up the same test case, but with a huge backing VMO. Specifically, the backing VMO needs
// to be large enough that adding a signed 64 bit value to its length is large enough to overflow an
// unsigned 64 bit value.

constexpr size_t kEighthOfMax = 0x2000000000000000;
static_assert(kEighthOfMax * 8 == 0);
constexpr size_t kHugeSize = kEighthOfMax * 7;

class HugeVmoTest : public zxtest::Test {
 public:
  void SetUp() override {
    ASSERT_OK(zx::vmo::create(kHugeSize, 0u, &backing));
    ASSERT_OK(backing.write(ALPHABET, 0, len));
    ASSERT_OK(backing.write(ALPHABET, len, len + len));
    zx::stream stream;
    ASSERT_OK(zx::stream::create(ZX_STREAM_MODE_READ | ZX_STREAM_MODE_WRITE, backing, 0u, &stream));
    ASSERT_OK(zxio_vmo_init(&storage, std::move(backing), std::move(stream)));
    io = &storage.io;
  }

  void TearDown() override { ASSERT_OK(zxio_close(io, /*should_wait=*/true)); }

 protected:
  zx::vmo backing;
  size_t len = strlen(ALPHABET);
  zxio_storage_t storage;
  zxio_t* io;
};

TEST_F(HugeVmoTest, SeekPositiveOverflow) {
  // Check that our expected-to-overflow values actually overflow.
  constexpr int64_t kTooFarForwards = kEighthOfMax * 2;
  static_assert(kHugeSize + kTooFarForwards < kHugeSize);

  // Seek to the end.
  size_t original_seek;
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_END, 0, &original_seek));
  ASSERT_EQ(original_seek, kHugeSize);

  constexpr size_t dummy_seek = 42;

  size_t new_seek = dummy_seek;

  // There's no tests for seeking forwards from the start of the file past infinity, since a int64_t
  // isn't big enough to cause the overflow.

  // Seeking forward from the seek pointer past past infinity should fail, without moving the seek
  // pointer.
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, kTooFarForwards, &new_seek));
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, 0, &new_seek));
  ASSERT_EQ(original_seek, new_seek);

  new_seek = 42;

  // Seeking forward from the end past past infinity should fail, without moving the seek
  // pointer.
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                zxio_seek(io, ZXIO_SEEK_ORIGIN_END, kTooFarForwards, &new_seek));
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_CURRENT, 0, &new_seek));
  ASSERT_EQ(original_seek, new_seek);
}

class VmoCloseTest : public VmoTest {
 public:
  void TearDown() override { /* The test case body will exercise closing */
  }
};

TEST_F(VmoCloseTest, UseAfterClose) {
  char buffer[16] = {};
  size_t actual = 0u;
  ASSERT_OK(zxio_read_at(io, 0, buffer, 6, 0, &actual));
  EXPECT_EQ(actual, 6);

  ASSERT_OK(zxio_close(io, /*should_wait=*/true));
  actual = 0;
  ASSERT_STATUS(zxio_read_at(io, 0, buffer, 6, 0, &actual), ZX_ERR_BAD_HANDLE);
  EXPECT_EQ(actual, 0);
}
