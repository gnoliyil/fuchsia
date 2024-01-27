#!/bin/bash
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# This script is required by the emulator to bring up the TAP network
# interface on Mac OSX. Do not remove or edit this script
# without testing it against ffx emu.
#
# If you are using `--net tap` option with ffx emu, configure this file as
# the upscript to use, by running:
# ffx config set emu.upscript $FUCHSIA_ROOT/tools/devshell/lib/emu-ifup-macos.sh

sudo ifconfig tap0 inet6 fc00::/7 up
