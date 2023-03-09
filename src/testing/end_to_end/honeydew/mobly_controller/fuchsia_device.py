#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Mobly Controller for Fuchsia Device"""

from typing import Any, Dict, List

import honeydew
from honeydew.interfaces.device_classes.fuchsia_device import (
    DEFAULT_SSH_USER, FuchsiaDevice)
from honeydew.utils.properties import DynamicProperty, PersistentProperty

MOBLY_CONTROLLER_CONFIG_NAME = "FuchsiaDevice"


def create(configs: List[Dict[str, Any]]) -> List[FuchsiaDevice]:
    """Create Fuchsia device controller(s) and returns them.

    Required for Mobly controller registration.

    Args:
        configs: List of dicts. Each dict representing a configuration for a
            Fuchsia device.

            Ensure to have following keys in the config dict:
            * name - Device name returned by `ffx target list`.
            * ssh_key - Absolute path to the SSH private key file needed to SSH
                into fuchsia device.
            * ssh_user - Username to be used to SSH into fuchsia device.
            * ip_address - Device IP (V4|V6) address.

    Returns:
        A list of FuchsiaDevice objects.
    """
    fuchsia_devices = []
    for config in configs:
        fuchsia_devices.append(
            honeydew.create_device(
                device_name=config["name"],
                ssh_private_key=config["ssh_private_key"],
                ssh_user=config.get("ssh_user", DEFAULT_SSH_USER),
                device_ip_address=config.get("ip_address")))
    return fuchsia_devices


def destroy(fuchsia_devices: List[FuchsiaDevice]) -> None:
    """Closes all created fuchsia devices.

    Required for Mobly controller registration.

    Args:
        fuchsia_devices: A list of FuchsiaDevice objects.
    """
    for fuchsia_device in fuchsia_devices:
        fuchsia_device.close()


def get_info(fuchsia_devices: List[FuchsiaDevice]) -> List[Dict[str, Any]]:
    """Gets information from a list of FuchsiaDevice objects.

    Optional for Mobly controller registration.

    Args:
        fuchsia_devices: A list of FuchsiaDevice objects.

    Returns:
        A list of dict, each representing info for an FuchsiaDevice objects.
    """
    return [
        _get_fuchsia_device_info(fuchsia_device)
        for fuchsia_device in fuchsia_devices
    ]


def _get_fuchsia_device_info(fuchsia_device: FuchsiaDevice) -> Dict[str, Any]:
    """Returns information of a specific fuchsia device object.

    Args:
        fuchsia_device: FuchsiaDevice object.

    Returns:
        Dict containing information of a fuchsia device.
    """
    device_info: Dict[str, Any] = {
        "device_class": fuchsia_device.__class__.__name__,
        "persistent": {},
        "dynamic": {},
    }

    for attr in dir(fuchsia_device):
        if attr.startswith("_"):
            continue

        attr_type = getattr(type(fuchsia_device), attr, None)
        if isinstance(attr_type, DynamicProperty):
            device_info["dynamic"][attr] = getattr(fuchsia_device, attr)
        elif isinstance(attr_type, PersistentProperty):
            device_info["persistent"][attr] = getattr(fuchsia_device, attr)

    return device_info
