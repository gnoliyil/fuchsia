#!/bin/sh
# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e

if [[ ! -f src/starnix/lib/syncio/wrapper.h ]]; then
  echo 'Please run this script from the root of your Fuchsia source tree.'
  exit 1
fi

readonly RAW_LINES="// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_camel_case_types)]
#![allow(dead_code)]"

PATH="$PWD/prebuilt/third_party/rust/linux-x64/bin:$PATH" \
./prebuilt/third_party/rust_bindgen/linux-x64/bindgen \
  --no-layout-tests \
  --with-derive-default \
  --allowlist-function "zxio_.*" \
  --allowlist-var "ZXIO_SHUTDOWN.*" \
  --allowlist-var "ZXIO_NODE_PROTOCOL.*" \
  --allowlist-var "ZXIO_SEEK_ORIGIN.*" \
  --allowlist-var "E[A-Z]*" \
  --raw-line "${RAW_LINES}" \
  -o src/starnix/lib/syncio/src/zxio.rs \
  src/starnix/lib/syncio/wrapper.h \
  -- \
  -I sdk/lib/zxio/include \
  -I zircon/system/public
