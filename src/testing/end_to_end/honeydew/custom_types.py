#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Custom data types."""

import enum
from typing import NamedTuple


class LEVEL(enum.Enum):
    """Logging level that need to specified to log a message onto device"""
    INFO = enum.auto()
    WARNING = enum.auto()
    ERROR = enum.auto()


class TargetSshAddress(NamedTuple):
    """Tuple that holds target's ssh address information.

    Args:
        ip: Target's SSH IP Address
        port: Target's SSH port
    """
    ip: str
    port: int


class Sl4fServerAddress(NamedTuple):
    """Tuple that holds sl4f server address information.

    Args:
        ip: IP Address of SL4F server
        port: Port where SL4F server is listening for SL4F requests
    """
    ip: str
    port: int
