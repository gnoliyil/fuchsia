// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <limits.h>

#include <zxtest/zxtest.h>

namespace {

zx_device_t* ddk_test_dev;

const char* TEST_STRING = "testing 1 2 3";

TEST(MetadataTest, AddMetadata) {
  char buffer[32] = {};
  zx_status_t status;
  size_t actual;

  status = device_get_metadata(ddk_test_dev, 1, buffer, sizeof(buffer), &actual);
  ASSERT_EQ(status, ZX_ERR_NOT_FOUND, "device_get_metadata did not return ZX_ERR_NOT_FOUND");

  status = device_get_metadata_size(ddk_test_dev, 1, &actual);
  ASSERT_EQ(status, ZX_ERR_NOT_FOUND, "device_get_metadata_size should return ZX_ERR_NOT_FOUND");

  status = device_add_metadata(ddk_test_dev, 1, TEST_STRING, strlen(TEST_STRING) + 1);
  ASSERT_EQ(status, ZX_OK, "device_add_metadata failed");

  status = device_get_metadata_size(ddk_test_dev, 1, &actual);
  ASSERT_EQ(strlen(TEST_STRING) + 1, actual, "Incorrect output length was returned.");
  status = device_get_metadata(ddk_test_dev, 1, buffer, sizeof(buffer), &actual);
  ASSERT_EQ(status, ZX_OK, "device_get_metadata failed");
  ASSERT_EQ(actual, strlen(TEST_STRING) + 1, "");
  ASSERT_EQ(strcmp(buffer, TEST_STRING), 0, "");
}

TEST(MetadataTest, AddMetadataLargeInput) {
  size_t large_len = 1024u * 16;
  auto large = std::make_unique<char[]>(large_len);
  zx_status_t status = device_add_metadata(ddk_test_dev, 1, large.get(), large_len);
  EXPECT_EQ(status, ZX_ERR_INVALID_ARGS, "device_add_metadata should return ZX_ERR_INVALID_ARGS");
}

TEST(MetadataTest, GetMetadataWouldOverflow) {
  char buffer[32] = {};
  zx_status_t status;
  size_t actual;

  status = device_add_metadata(ddk_test_dev, 2, TEST_STRING, strlen(TEST_STRING) + 1);
  ASSERT_EQ(status, ZX_OK, "");

  status = device_get_metadata(ddk_test_dev, 2, buffer, 1, &actual);
  ASSERT_EQ(status, ZX_ERR_BUFFER_TOO_SMALL, "device_get_metadata overflowed buffer");
}

// A special LogSink that just redirects all output to zxlogf
class LogSink : public zxtest::LogSink {
 public:
  void Write(const char* format, ...) override {
    std::array<char, 1024> line_buf;
    va_list args;
    va_start(args, format);
    vsnprintf(line_buf.data(), line_buf.size(), format, args);
    va_end(args);
    line_buf[line_buf.size() - 1] = 0;
    zxlogf(INFO, "%s", line_buf.data());
  }
  void Flush() override {}
};

zx_status_t metadata_test_bind(void* ctx, zx_device_t* parent) {
  zxtest::Runner::GetInstance()->mutable_reporter()->set_log_sink(std::make_unique<LogSink>());
  ddk_test_dev = parent;
  if (RUN_ALL_TESTS(0, nullptr) != 0) {
    return ZX_ERR_BAD_STATE;
  }

  static zx_protocol_device_t proto = {
      .version = DEVICE_OPS_VERSION,
      .release = [](void* ctx) {},
  };
  device_add_args_t args = {
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "temp",
      .ctx = parent,
      .ops = &proto,
  };

  zx_device_t* out_device;
  return device_add(parent, &args, &out_device);
}

static zx_driver_ops_t metadata_test_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = metadata_test_bind,
};

}  // namespace

ZIRCON_DRIVER(metadata_test, metadata_test_driver_ops, "zircon", "0.1");
