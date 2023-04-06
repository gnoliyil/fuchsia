#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Abstract base class for SL4F transport."""

import abc
from typing import Any, Dict, Iterable, Optional, Type

TIMEOUTS: Dict[str, float] = {
    "SL4F_RESPONSE": 30,
}

DEFAULTS: Dict[str, int] = {
    "ATTEMPTS": 3,
    "INTERVAL": 3,
}


class SL4F(abc.ABC):
    """Abstract base class for SL4F transport."""

    # List all the public methods in alphabetical order
    @abc.abstractmethod
    def check_sl4f_connection(self) -> None:
        """Checks SL4F connection by sending a SL4F request to device.

        Raises:
            errors.FuchsiaDeviceError: If SL4F connection is not successful.
        """

    @abc.abstractmethod
    def send_sl4f_command(
        self,
        method: str,
        params: Optional[Dict[str, Any]] = None,
        timeout: float = TIMEOUTS["SL4F_RESPONSE"],
        attempts: int = DEFAULTS["ATTEMPTS"],
        interval: int = DEFAULTS["INTERVAL"],
        exceptions_to_skip: Optional[Iterable[Type[Exception]]] = None
    ) -> Dict[str, Any]:
        """Sends SL4F command from host to Fuchsia device and returns the
        response.

        Args:
            method: SL4F method.
            params: Any optional params needed for method param.
            timeout: How long in sec to wait for SL4F response.
            attempts: number of attempts to try in case of a failure.
            interval: wait time in sec before each retry in case of a failure.
            exceptions_to_skip: Any non fatal exceptions for which retry will
                not be attempted and no error will be raised.

        Returns:
            SL4F command response returned by the Fuchsia device.
                Note: If SL4F command raises any exception specified in
                exceptions_to_skip then a empty dict will be returned.

        Raises:
            errors.FuchsiaDeviceError: On failure.
        """

    @abc.abstractmethod
    def start_sl4f_server(self) -> None:
        """Starts the SL4F server on fuchsia device.

        Raises:
            errors.FuchsiaDeviceError: Failed to start the SL4F server.
        """
