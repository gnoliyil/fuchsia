#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Unit tests for honeydew.affordances.bluetooth_default.py."""

import unittest
from typing import Any, Dict
from unittest import mock

from honeydew.affordances import bluetooth_default
from honeydew.transports import sl4f as sl4f_transport
from parameterized import parameterized


def _custom_test_name_func(testcase_func, _, param) -> str:
    """Custom name function method."""
    test_func_name: str = testcase_func.__name__

    params_dict: Dict[str, Any] = param.args[0]
    test_label: str = parameterized.to_safe_name(params_dict["label"])

    return f"{test_func_name}_with_{test_label}"


# pylint: disable=protected-access
class BluetoothDefaultTests(unittest.TestCase):
    """Unit tests for honeydew.affordances.bluetooth_default.py."""

    def setUp(self) -> None:
        super().setUp()

        self.sl4f_obj = mock.MagicMock(spec=sl4f_transport.SL4F)
        self.bluetooth_obj = bluetooth_default.BluetoothDefault(
            device_name="fuchsia-emulator", sl4f=self.sl4f_obj)

        self.sl4f_obj.run.assert_called()
        self.sl4f_obj.reset_mock()

    def test_sys_init(self) -> None:
        """Test for BluetoothDefault.sys_init() method."""
        self.bluetooth_obj.sys_init()

        self.sl4f_obj.run.assert_called()

    @parameterized.expand(
        [
            ({
                "label": "discovery_true",
                "discovery": True
            },),
            ({
                "label": "discovery_false",
                "discovery": False
            },),
        ],
        name_func=_custom_test_name_func)
    def test_request_discovery(self, parameterized_dict) -> None:
        """Test for BluetoothDefault.request_discovery() method."""
        self.bluetooth_obj.request_discovery(
            discovery=parameterized_dict["discovery"])

        self.sl4f_obj.run.assert_called()


if __name__ == "__main__":
    unittest.main()
