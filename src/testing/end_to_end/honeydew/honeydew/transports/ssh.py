#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Provides methods for Host-(Fuchsia)Target interactions via SSH."""

import ipaddress
import logging
import subprocess
import time
from typing import Any

from honeydew import custom_types, errors
from honeydew.transports import ffx

_DEFAULTS: dict[str, Any] = {
    "USERNAME": "fuchsia",
}

_CMDS: dict[str, str] = {
    "ECHO": "echo",
}

_TIMEOUTS: dict[str, float] = {
    "COMMAND_ARG": 3,
    "COMMAND_RESPONSE": 60,
    "CONNECTION": 60,
}

_OPTIONS_LIST: list[str] = [
    "-oPasswordAuthentication=no",
    "-oStrictHostKeyChecking=no",
    f"-oConnectTimeout={_TIMEOUTS['COMMAND_ARG']}",
]
_OPTIONS: str = " ".join(_OPTIONS_LIST)
_SSH_COMMAND_WITH_PORT: str = (
    "ssh {options} -i {private_key} -p {port} {username}@{ip_address} {command}"
)
_SSH_COMMAND_WITHOUT_PORT: str = (
    "ssh {options} -i {private_key} {username}@{ip_address} {command}"
)

_LOGGER: logging.Logger = logging.getLogger(__name__)


class SSH:
    """Provides methods for Host-(Fuchsia)Target interactions via SSH.

    Args:
        name: Fuchsia device name.

        private_key: Absolute path to the SSH private key file needed to SSH
            into fuchsia device.

        ffx_transport: ffx.FFX object.

        ip_port: Fuchsia device's SSH IP Address and Port.

        username: Username to be used to SSH into fuchsia device.
            Default is "fuchsia".
    """

    def __init__(
        self,
        device_name: str,
        private_key: str,
        ffx_transport: ffx.FFX,
        ip_port: custom_types.IpPort | None = None,
        username: str | None = None,
    ) -> None:
        self._name: str = device_name
        self._ip_port: custom_types.IpPort | None = ip_port
        self._private_key: str = private_key
        self._username: str = username or _DEFAULTS["USERNAME"]

        self._ffx_transport: ffx.FFX = ffx_transport

    def check_connection(
        self, timeout: float = _TIMEOUTS["CONNECTION"]
    ) -> None:
        """Checks the SSH connection from host to Fuchsia device.

        Args:
            timeout: How long in sec to wait for SSH connection.

        Raises:
            errors.SshConnectionError
        """
        start_time: float = time.time()
        end_time: float = start_time + timeout

        _LOGGER.debug("Waiting for %s to allow ssh connection...", self._name)
        err = None
        while time.time() < end_time:
            try:
                self.run(command=_CMDS["ECHO"])
                break
            except Exception as e:  # pylint: disable=broad-except
                err = e
                time.sleep(1)
        else:
            raise errors.SshConnectionError(
                f"SSH connection check failed for {self._name}"
            ) from err
        _LOGGER.debug("%s is available via ssh.", self._name)

    @property
    def target_address(self) -> custom_types.TargetSshAddress:
        """Gets the address used on SSH.

        Returns:
            The IP address and port used for SSH.
        """
        if self._ip_port:
            return custom_types.TargetSshAddress(
                ip=self._ip_port.ip, port=self._ip_port.port
            )
        return self._ffx_transport.get_target_ssh_address()

    def run(
        self, command: str, timeout: float = _TIMEOUTS["COMMAND_RESPONSE"]
    ) -> str:
        """Run command on Fuchsia device from host via SSH and return output.

        Args:
            command: Command to run on the Fuchsia device.
            timeout: How long in sec to wait for SSH command to complete.

        Returns:
            Command output.

        Raises:
            errors.SSHCommandError: On failure.
            errors.FfxCommandError: If failed to get the target SSH address.
        """
        process = self.popen(command)
        stdout, stderr = process.communicate(timeout=timeout)
        if process.returncode != 0:
            if stdout:
                _LOGGER.debug("stdout returned by the command is: %s", stdout)
            if stderr:
                _LOGGER.debug("stderr returned by the command is: %s", stdout)
            raise errors.SSHCommandError(
                f"Unexpected returncode: {process.returncode}"
            )
        _LOGGER.debug(
            "Output returned by SSH command '%s' is: '%s'",
            command,
            stdout,
        )
        return stdout.decode()

    def popen(self, command: str) -> subprocess.Popen[Any]:
        """Run command on Fuchsia device from host via SSH and return the underlying subprocess.

        It is up to callers to detect and handle potential errors, and make sure
        to close this process eventually (e.g. with `popen.terminate` method).


        Args:
            command: Command to run on the Fuchsia device.

        Returns:
            The underlying subprocess.

        Raises:
            errors.SSHCommandError: On failure.
            errors.FfxCommandError: If failed to get the target SSH address.
        """
        ip: ipaddress.IPv4Address | ipaddress.IPv6Address | None = None
        port: int | None = None
        ssh_command: str

        address = self.target_address
        ip, port = address.ip, address.port

        if port:
            ssh_command = _SSH_COMMAND_WITH_PORT.format(
                options=_OPTIONS,
                private_key=self._private_key,
                port=port,
                username=self._username,
                ip_address=ip,
                command=command,
            )
        else:
            ssh_command = _SSH_COMMAND_WITHOUT_PORT.format(
                options=_OPTIONS,
                private_key=self._private_key,
                username=self._username,
                ip_address=ip,
                command=command,
            )

        _LOGGER.debug("Running the SSH command: '%s'...", ssh_command)
        try:
            return subprocess.Popen(
                ssh_command.split(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except Exception as err:  # pylint: disable=broad-except
            raise errors.SSHCommandError(err) from err
