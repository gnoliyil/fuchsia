#!/bin/bash
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

### Runs a Rust helper script inside the fx environment under the lock
### used by `fx build`

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&2 && pwd)"/../lib/vars.sh || exit $?
fx-config-read

# Ninja actions that use RBE need a running reproxy process.
# The following wrapper starts/shuts down reproxy around any command.
rbe_wrapper=()
if fx-rbe-enabled ; then rbe_wrapper=("${RBE_WRAPPER[@]}") ; fi

fx-try-locked "${rbe_wrapper[@]}" "${PREBUILT_PYTHON3_DIR}/bin/python3.8" "${FUCHSIA_DIR}/tools/devshell/contrib/lib/rust/$(basename $0).py" "${@:1}" --out-dir=$FUCHSIA_BUILD_DIR
