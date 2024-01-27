#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Mobly Controller for Fuchsia Device"""

import logging
from typing import Any, Dict, List

import honeydew
from honeydew.interfaces.device_classes import \
    fuchsia_device as fuchsia_device_interface
from honeydew.utils import properties

MOBLY_CONTROLLER_CONFIG_NAME = "FuchsiaDevice"

_LOGGER: logging.Logger = logging.getLogger(__name__)


def create(
    configs: List[Dict[str,
                       Any]]) -> List[fuchsia_device_interface.FuchsiaDevice]:
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
    _LOGGER.debug(
        "FuchsiaDevice controller configs received in testbed yml file is '%s'",
        configs)

    fuchsia_devices: List[fuchsia_device_interface.FuchsiaDevice] = []
    for config in configs:
        device_config: Dict[str, str] = _get_device_config(config)
        fuchsia_devices.append(
            honeydew.create_device(
                device_name=device_config["name"],
                ssh_private_key=device_config["ssh_private_key"],
                ssh_user=device_config["ssh_user"],
                device_ip_address=device_config.get("ip_address")))
    return fuchsia_devices


def destroy(
        fuchsia_devices: List[fuchsia_device_interface.FuchsiaDevice]) -> None:
    """Closes all created fuchsia devices.

    Required for Mobly controller registration.

    Args:
        fuchsia_devices: A list of FuchsiaDevice objects.
    """
    for fuchsia_device in fuchsia_devices:
        fuchsia_device.close()


def get_info(
    fuchsia_devices: List[fuchsia_device_interface.FuchsiaDevice]
) -> List[Dict[str, Any]]:
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


def _get_fuchsia_device_info(
        fuchsia_device: fuchsia_device_interface.FuchsiaDevice
) -> Dict[str, Any]:
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

        attr_type: Any | None = getattr(type(fuchsia_device), attr, None)
        if isinstance(attr_type, properties.DynamicProperty):
            device_info["dynamic"][attr] = getattr(fuchsia_device, attr)
        elif isinstance(attr_type, properties.PersistentProperty):
            device_info["persistent"][attr] = getattr(fuchsia_device, attr)

    return device_info


# TODO(fxbug.dev/123746): Remove this after fxbug.dev/123746 is fixed.
def _get_device_config(config: Dict[str, str]) -> Dict[str, str]:
    """Parse, validate and update the mobly configuration associated with
    FuchsiaDevice controller.

    Args:
        config: mobly configuration associated with FuchsiaDevice controller.

    Returns:
        Validated mobly configuration associated with FuchsiaDevice controller.

    Raises:
        RuntimeError: If any required information is missing.
    """
    _LOGGER.debug(
        "FuchsiaDevice controller config received in testbed yml file is '%s'",
        config)

    device_config: Dict[str, str] = {
        "name":
            "",
        "ssh_private_key":
            "",
        "ssh_user":
            config.get("ssh_user", fuchsia_device_interface.DEFAULT_SSH_USER),
        "ip_address":
            ""
    }

    # Sample testbed file format for FuchsiaDevice controller used in infra...
    # - Controllers:
    #     FuchsiaDevice:
    #     - ipv4: ''
    #       ipv6: fe80::93e3:e3d4:b314:6e9b%qemu
    #       nodename: botanist-target-qemu
    #       serial_socket: ''
    #       ssh_key: private_key

    if config.get("name"):
        device_config["name"] = config["name"]
    elif config.get("nodename"):
        device_config["name"] = config["nodename"]
    else:
        raise RuntimeError("Missing fuchsia device name in the config")

    if config.get("ssh_private_key"):
        device_config["ssh_private_key"] = config["ssh_private_key"]
    elif config.get("ssh_key"):
        device_config["ssh_private_key"] = config["ssh_key"]
    else:
        raise RuntimeError("Missing SSH private key in the config")

    if config.get("ip_address"):
        device_config["ip_address"] = config["ip_address"]
    elif config.get("ipv6"):
        device_config["ip_address"] = config["ipv6"]
    elif config.get("ipv4"):
        device_config["ip_address"] = config["ipv4"]

    _LOGGER.debug(
        "Updated FuchsiaDevice controller config after the validation is '%s'",
        config)
    return device_config
