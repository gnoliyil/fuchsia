# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Workbench platform config

This is a platform configuration for workbench, which can be used by other
products. This platform configuration is meant to feed into Fuchsia's product
assembly process.
"""

load("@legacy_ninja_build_outputs//:build_args.bzl", "delegated_network_provisioning")
load(
    "@fuchsia_sdk//fuchsia:assembly.bzl",
    "BUILD_TYPES",
)

workbench_platform_config = {
    "build_type": BUILD_TYPES.ENG,
    "battery": {
        "enabled": True,
    },
    "connectivity": {
        "network": {
            "networking": "standard",
            "netcfg_config_path": "LABEL(//src/connectivity/policy/netcfg/config:%s.json)" % ("delegated_network_provisioning" if delegated_network_provisioning else "netcfg_default"),
        },
    },
    "forensics": {
        "feedback": {
            "low_memory": True,
        },
    },
    "icu": {
        "revision": "default",
    },
    "intl": {
        "config_type": "default",
    },
    "setui": {
        "use_icu": "with_icu",
        "with_camera": False,
    },
    "media": {
        "audio_device_registry_enabled": True,
    },
    "starnix": {
        "enabled": True,
        "enable_android_support": True,
    },
    "storage": {
        "configure_fshost": True,
        "live_usb_enabled": False,
    },
    "session": {
        "enabled": True,
    },
    "ui": {
        "enabled": True,
        "supported_input_devices": [
            "button",
            "keyboard",
            "mouse",
            "touchscreen",
        ],
    },
    "power": {
        "suspend_enabled": True,
    },
}
