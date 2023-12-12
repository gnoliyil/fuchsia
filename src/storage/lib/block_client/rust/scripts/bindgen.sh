#!/usr/bin/env bash
#
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Run this script whenever the $source_file has changed.
# TODO(fxbug.dev/73858): replace this script with a GN bindgen target when there
# is in-tree support.

set -euo pipefail

# Determine paths for this script and its directory, and set $FUCHSIA_DIR.
readonly FULL_PATH="${BASH_SOURCE[0]}"
readonly SCRIPT_DIR="$(cd "$(dirname "${FULL_PATH}")" >/dev/null 2>&1 && pwd)"
source "${SCRIPT_DIR}/../../../../../../tools/devshell/lib/vars.sh"

readonly BINDGEN="${PREBUILT_RUST_BINDGEN_DIR}/bindgen"
readonly ZEROCOPY_SYMS_REGEX="BlockFifo(Command|Request|Response)"

readonly target_file="$FUCHSIA_DIR/src/storage/lib/block_client/rust/src/fifo.rs"
readonly source_file_within_tree="src/devices/block/drivers/core/block-fifo.h"
readonly source_file="$FUCHSIA_DIR/$source_file_within_tree"

readonly copyright_line=$(grep -E "^// Copyright [0-9]+" "${source_file}" || \
  echo "// Copyright $(date +%Y) The Fuchsia Authors. All rights reserved.")

readonly RAW_LINES="${copyright_line}
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Generated by src/storage/lib/block_client/rust/scripts/bindgen.sh
// Run the above script whenever $source_file_within_tree
// has changed.

#![allow(dead_code)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

use zerocopy::{AsBytes, FromBytes, FromZeros};"

"${BINDGEN}" \
  --raw-line "${RAW_LINES}" \
  --with-derive-default \
  --with-derive-custom-struct=${ZEROCOPY_SYMS_REGEX}={AsBytes,FromBytes,FromZeros} \
  --impl-debug \
  --allowlist-type=${ZEROCOPY_SYMS_REGEX} \
  "${source_file}" | \
  grep -vF 'pub type __' > "${target_file}"
