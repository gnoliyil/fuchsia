# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Configuration for the storage subsystem
"""

load(":providers.bzl", "FuchsiaStorageConfigInfo")

def _fuchsia_platform_storage_configuration_impl(ctx):
    return [
        FuchsiaStorageConfigInfo(
            live_usb_enabled = ctx.attr.live_usb_enabled,
            configure_fshost = ctx.attr.configure_fshost,
        ),
    ]

fuchsia_platform_storage_configuration = rule(
    doc = """Generates an storage configuration.""",
    implementation = _fuchsia_platform_storage_configuration_impl,
    provides = [FuchsiaStorageConfigInfo],
    attrs = {
        "live_usb_enabled": attr.bool(
            doc = "A bool value whether live_usb is enabled",
        ),
        "configure_fshost": attr.bool(
            doc = "A bool value whether to configure fshost using Assembly",
        ),
    },
)
