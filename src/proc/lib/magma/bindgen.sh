#!/bin/sh
# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e

if [[ ! -f src/graphics/lib/magma/include/magma/magma.h ]]; then
  echo 'Please run this script from the root of your Fuchsia source tree.'
  exit 1
fi

readonly RAW_LINES="// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use zerocopy::{AsBytes, FromBytes};

// TODO: BEGIN remove these hardcoded values when bindgen is fixed
pub const MAGMA_STATUS_OK: u32 = 0;
pub const MAGMA_STATUS_INVALID_ARGS: i32 = -2;
pub const MAGMA_IMAGE_CREATE_FLAGS_PRESENTABLE: u32 = 1;
pub const MAGMA_IMAGE_CREATE_FLAGS_VULKAN_USAGE: u32 = 2;
pub const MAGMA_MAX_IMAGE_PLANES: u32 = 4;
pub const MAGMA_COHERENCY_DOMAIN_CPU: u32 = 0;
pub const MAGMA_COHERENCY_DOMAIN_RAM: u32 = 1;
pub const MAGMA_COHERENCY_DOMAIN_INACCESSIBLE: u32 = 2;
pub const MAGMA_POLL_TYPE_SEMAPHORE: u32 = 1;
pub const MAGMA_POLL_TYPE_HANDLE: u32 = 2;
// TODO: END remove these hardcoded values when bindgen is fixed
"

PATH="$PWD/prebuilt/third_party/rust/linux-x64/bin:$PATH" \
./prebuilt/third_party/rust_bindgen/linux-x64/bindgen \
  --no-layout-tests \
  --size_t-is-usize \
  --with-derive-default \
  --explicit-padding \
  --raw-line "${RAW_LINES}" \
  -o src/proc/lib/magma/src/magma.rs \
  src/proc/lib/magma/wrapper.h \
  -- \
  -I zircon/system/public \
  -I out/default/linux_x64/gen/src/graphics/lib/magma/include \
  -I src/graphics/lib/magma/src \
  -I src/graphics/lib/magma/include

# TODO: Figure out how to get bindgen to derive AsBytes and FromBytes.
#       See https://github.com/rust-lang/rust-bindgen/issues/1089
sed -i \
  's/derive(Debug, Default, Copy, Clone)/derive(Debug, Default, Copy, Clone, AsBytes, FromBytes)/' \
  src/proc/lib/magma/src/magma.rs
