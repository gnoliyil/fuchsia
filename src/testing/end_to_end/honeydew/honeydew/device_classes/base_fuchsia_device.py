#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Fuchsia device common implementation with transport-independent logic."""

import abc
from datetime import datetime
import logging
import os
import time
from typing import Any, Callable, Dict, List, Optional

from honeydew import custom_types
from honeydew import errors
from honeydew.affordances.ffx import session as session_ffx
from honeydew.interfaces.affordances import session
from honeydew.interfaces.auxiliary_devices import (
    power_switch as power_switch_interface,
)
from honeydew.interfaces.device_classes import affordances_capable
from honeydew.interfaces.device_classes import fuchsia_device
from honeydew.interfaces.device_classes import transports_capable
from honeydew.transports import fastboot as fastboot_transport
from honeydew.transports import ffx as ffx_transport
from honeydew.transports import ssh as ssh_transport
from honeydew.utils import properties

_TIMEOUTS: Dict[str, float] = {
    "SLEEP": 0.5,
    "OFFLINE_ATTEMPT": 2,
}

_LOGGER: logging.Logger = logging.getLogger(__name__)


class BaseFuchsiaDevice(
    fuchsia_device.FuchsiaDevice,
    affordances_capable.RebootCapableDevice,
    affordances_capable.SessionCapableDevice,
    transports_capable.FastbootCapableDevice,
    transports_capable.FFXCapableDevice,
    transports_capable.SSHCapableDevice,
):
    """Common implementation for Fuchsia devices using different transports.
    Every device running Fuchsia contains common functionality as well as the
    FFX and SSH transports. This logic is centralized here.
    """

    def __init__(
        self,
        device_name: str,
        ssh_private_key: Optional[str] = None,
        ssh_user: Optional[str] = None,
    ) -> None:
        _LOGGER.debug("Initializing FuchsiaDevice")
        self._name: str = device_name
        self._ssh_private_key: Optional[str] = ssh_private_key
        self._ssh_user: Optional[str] = ssh_user
        self._on_device_boot_fns: List[Callable[[], None]] = []
        self.health_check()

    # List all the persistent properties in alphabetical order
    @properties.PersistentProperty
    def device_name(self) -> str:
        """Returns the name of the device.

        Returns:
            Name of the device.
        """
        return self._name

    @properties.PersistentProperty
    def device_type(self) -> str:
        """Returns the type of the device.

        Returns:
            Type of the device.

        Raises:
            errors.FfxCommandError: On failure.
        """
        return self.ffx.get_target_type()

    @properties.PersistentProperty
    def manufacturer(self) -> str:
        """Returns the manufacturer of the device.

        Returns:
            Manufacturer of device.

        Raises:
            errors.FuchsiaDeviceError: On failure.
        """
        return self._product_info["manufacturer"]

    @properties.PersistentProperty
    def model(self) -> str:
        """Returns the model of the device.

        Returns:
            Model of device.

        Raises:
            errors.FuchsiaDeviceError: On failure.
        """
        return self._product_info["model"]

    @properties.PersistentProperty
    def product_name(self) -> str:
        """Returns the product name of the device.

        Returns:
            Product name of the device.

        Raises:
            errors.FuchsiaDeviceError: On failure.
        """
        return self._product_info["name"]

    @properties.PersistentProperty
    def serial_number(self) -> str:
        """Returns the serial number of the device.

        Returns:
            Serial number of the device.
        """
        return self._device_info["serial_number"]

    # List all the dynamic properties in alphabetical order
    @properties.DynamicProperty
    def firmware_version(self) -> str:
        """Returns the firmware version of the device.

        Returns:
            Firmware version of the device.
        """
        return self._build_info["version"]

    # List all transports in alphabetical order
    @properties.Transport
    def fastboot(self) -> fastboot_transport.Fastboot:
        """Returns the Fastboot transport object.

        Returns:
            Fastboot object.
        """
        fastboot_obj: fastboot_transport.Fastboot = fastboot_transport.Fastboot(
            device_name=self.device_name, reboot_affordance=self
        )
        return fastboot_obj

    @properties.Transport
    def ffx(self) -> ffx_transport.FFX:
        """Returns the FFX transport object.

        Returns:
            FFX object.

        Raises:
            errors.FfxCommandError: Failed to instantiate.
        """
        ffx_obj: ffx_transport.FFX = ffx_transport.FFX(target=self.device_name)
        return ffx_obj

    @properties.Transport
    def ssh(self) -> ssh_transport.SSH:
        """Returns the SSH transport object.
        Returns:
            SSH object.
        Raises:
            errors.SSHCommandError: Failed to instantiate.
        """
        if not self._ssh_private_key:
            raise errors.SSHCommandError(
                "ssh_private_key argument need to be passed during device "
                "init in-order to SSH into the device"
            )
        ssh_obj: ssh_transport.SSH = ssh_transport.SSH(
            device_name=self.device_name,
            username=self._ssh_user,
            private_key=self._ssh_private_key,
        )
        return ssh_obj

    @properties.Affordance
    def session(self) -> session.Session:
        """Returns a session affordance object.

        Returns:
            session.Session object
        """
        return session_ffx.Session(device_name=self.device_name, ffx=self.ffx)

    # List all the public methods in alphabetical order
    def health_check(self) -> None:
        """Ensure device is healthy."""
        if self._ssh_private_key:
            self.ssh.check_connection()
        self.ffx.check_connection()

    def log_message_to_device(
        self, message: str, level: custom_types.LEVEL
    ) -> None:
        """Log message to fuchsia device at specified level.

        Args:
            message: Message that need to logged.
            level: Log message level.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """
        timestamp: str = datetime.now().strftime("%Y-%m-%d-%I-%M-%S-%p")
        message = f"[Host Time: {timestamp}] - {message}"
        self._send_log_command(tag="lacewing", message=message, level=level)

    @abc.abstractmethod
    def on_device_boot(self) -> None:
        """Take actions after the device is rebooted.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """
        for on_device_boot_fn in self._on_device_boot_fns:
            _LOGGER.info("Calling %s", on_device_boot_fn.__qualname__)
            on_device_boot_fn()

    def power_cycle(
        self,
        power_switch: power_switch_interface.PowerSwitch,
        outlet: Optional[int] = None,
    ) -> None:
        """Power cycle (power off, wait for delay, power on) the device.

        Args:
            power_switch: Implementation of PowerSwitch interface.
            outlet (int): If required by power switch hardware, outlet on
                power switch hardware where this fuchsia device is connected.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """
        _LOGGER.info("Power cycling %s...", self.device_name)
        self.log_message_to_device(
            message=f"Powering cycling {self.device_name}...",
            level=custom_types.LEVEL.INFO,
        )

        _LOGGER.info("Powering off %s...", self.device_name)
        power_switch.power_off(outlet)
        self.wait_for_offline()

        _LOGGER.info("Powering on %s...", self.device_name)
        power_switch.power_on(outlet)
        self.wait_for_online()

        self.on_device_boot()

        self.log_message_to_device(
            message=f"Successfully power cycled {self.device_name}...",
            level=custom_types.LEVEL.INFO,
        )

    def reboot(self) -> None:
        """Soft reboot the device.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """
        _LOGGER.info("Rebooting %s...", self.device_name)
        self.log_message_to_device(
            message=f"Rebooting {self.device_name}...",
            level=custom_types.LEVEL.INFO,
        )

        self._send_reboot_command()

        self.wait_for_offline()
        self.wait_for_online()
        self.on_device_boot()

        self.log_message_to_device(
            message=f"Successfully rebooted {self.device_name}...",
            level=custom_types.LEVEL.INFO,
        )

    def register_for_on_device_boot(self, fn: Callable[[], None]) -> None:
        """Register a function that will be called in on_device_boot."""
        self._on_device_boot_fns.append(fn)

    def snapshot(
        self, directory: str, snapshot_file: Optional[str] = None
    ) -> str:
        """Captures the snapshot of the device.

        Args:
            directory: Absolute path on the host where snapshot file will be
                saved. If this directory does not exist, this method will create
                it.

            snapshot_file: Name of the output snapshot file.
                If not provided, API will create a name using
                "Snapshot_{device_name}_{'%Y-%m-%d-%I-%M-%S-%p'}" format.

        Returns:
            Absolute path of the snapshot file.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """
        _LOGGER.info("Collecting snapshot on %s...", self.device_name)
        # Take the snapshot before creating the directory or file, as
        # _send_snapshot_command may raise an exception.
        snapshot_bytes: bytes = self._send_snapshot_command()

        directory = os.path.abspath(directory)
        try:
            os.makedirs(directory)
        except FileExistsError:
            pass

        if not snapshot_file:
            timestamp: str = datetime.now().strftime("%Y-%m-%d-%I-%M-%S-%p")
            snapshot_file = f"Snapshot_{self.device_name}_{timestamp}.zip"
        snapshot_file_path: str = os.path.join(directory, snapshot_file)

        with open(snapshot_file_path, "wb") as snapshot_binary_zip:
            snapshot_binary_zip.write(snapshot_bytes)

        _LOGGER.info("Snapshot file has been saved @ '%s'", snapshot_file_path)
        return snapshot_file_path

    def wait_for_offline(
        self, timeout: float = fuchsia_device.TIMEOUTS["OFFLINE"]
    ) -> None:
        """Wait for Fuchsia device to go offline.

        Args:
            timeout: How long in sec to wait for device to go offline.

        Raises:
            errors.FuchsiaDeviceError: If device is not offline.
        """
        _LOGGER.info("Waiting for %s to go offline...", self.device_name)
        start_time: float = time.time()
        end_time: float = start_time + timeout
        while time.time() < end_time:
            # TODO(b/304371114): Once `ffx target wait --down` is available, use
            # it instead of below logic which relies on `ffx target wait` to
            # fail.

            # Intentionally using OFFLINE_ATTEMPT as 2 sec.
            # Fuchsia device may reboot quicker. So using a higher timeout such
            # as 15sec may result in false failures.
            # And also `ffx target wait` may take a sec or so to establish SSH
            # connection if needed. So using anything lower than 1 sec is also
            # not advised.
            # 2 secs seems to be a sweet spot for knowing if device is offline
            # or not using `ffx target wait --timeout` command
            if not self.ffx.is_target_connected(
                timeout=_TIMEOUTS["OFFLINE_ATTEMPT"]
            ):
                _LOGGER.info("%s is offline.", self.device_name)
                break
            time.sleep(_TIMEOUTS["SLEEP"])
        else:
            raise errors.FuchsiaDeviceError(
                f"'{self.device_name}' failed to go offline in {timeout}sec."
            )

    def wait_for_online(
        self, timeout: float = fuchsia_device.TIMEOUTS["ONLINE"]
    ) -> None:
        """Wait for Fuchsia device to go online.

        Args:
            timeout: How long in sec to wait for device to go offline.

        Raises:
            errors.FuchsiaDeviceError: If device is not online.
        """
        _LOGGER.info("Waiting for %s to go online...", self.device_name)
        try:
            self.ffx.wait_for_rcs_connection(timeout=timeout)
            _LOGGER.info("%s is online.", self.device_name)
        except Exception as err:  # pylint: disable=broad-except
            raise errors.FuchsiaDeviceError(
                f"'{self.device_name}' failed to go online in {timeout}sec."
            ) from err

    # List all private properties in alphabetical order
    @property
    @abc.abstractmethod
    def _build_info(self) -> Dict[str, Any]:
        """Returns the build information of the device.

        Returns:
            Build info dict.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """

    @property
    @abc.abstractmethod
    def _device_info(self) -> Dict[str, Any]:
        """Returns the device information of the device.

        Returns:
            Device info dict.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """

    @property
    @abc.abstractmethod
    def _product_info(self) -> Dict[str, Any]:
        """Returns the product information of the device.

        Returns:
            Product info dict.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """

    # List all private methods, in alphabetical order
    @abc.abstractmethod
    def _send_log_command(
        self, tag: str, message: str, level: custom_types.LEVEL
    ) -> None:
        """Send a device command to write to the syslog.

        Args:
            tag: Tag to apply to the message in the syslog.
            message: Message that need to logged.
            level: Log message level.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """

    @abc.abstractmethod
    def _send_reboot_command(self) -> None:
        """Send a device command to trigger a soft reboot.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.
        """

    @abc.abstractmethod
    def _send_snapshot_command(self) -> bytes:
        """Send a device command to take a snapshot.

        Raises:
            errors.FuchsiaControllerError: On communications failure.
            errors.Sl4FError: On communications failure.

        Returns:
            Bytes containing snapshot data as a zip archive.
        """
