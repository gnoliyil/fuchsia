#!/usr/bin/env bash

# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -eo pipefail

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ZIRCON_DIR="${DIR}/../../../../.."
SCRIPTS_DIR="${ZIRCON_DIR}/scripts"

# //zircon/scripts/package-image.sh
"${SCRIPTS_DIR}/package-image.sh" -b imx8mmevk \
    -d "$ZIRCON_DIR/kernel/target/arm64/dtb/dummy-device-tree.dtb" -D mkbootimg \
    -l -M 0x40000000 -K 0x400000 -B "$(pwd)" $@
