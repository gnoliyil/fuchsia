// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_FIDL_METADATA_I2C_H_
#define SRC_DEVICES_LIB_FIDL_METADATA_I2C_H_

#include <lib/stdcompat/span.h>
#include <lib/zx/result.h>
#include <stdint.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <vector>

constexpr size_t kMaxI2cNameLength = 64;

namespace fidl_metadata::i2c {
struct Channel {
  uint32_t bus_id;
  uint16_t address;

  uint32_t vid;
  uint32_t pid;
  uint32_t did;

  char name[kMaxI2cNameLength];
};

// Convert an array of i2c_channel to fuchsia.hardware.i2c.I2CBusMetadata encoded
// in a FIDL bytestream.
// TODO(fxbug.dev/120971): Remove after all users have switched to the version below.
zx::result<std::vector<uint8_t>> I2CChannelsToFidl(cpp20::span<const Channel> channels);

zx::result<std::vector<uint8_t>> I2CChannelsToFidl(uint32_t bus_id,
                                                   cpp20::span<const Channel> channels);

}  // namespace fidl_metadata::i2c

#endif  // SRC_DEVICES_LIB_FIDL_METADATA_I2C_H_
