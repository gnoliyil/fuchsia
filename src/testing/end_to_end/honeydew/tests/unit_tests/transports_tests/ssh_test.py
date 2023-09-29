#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Unit tests for honeydew.transports.ssh.py."""

import subprocess
from typing import Any, Dict
import unittest
from unittest import mock

from parameterized import parameterized

from honeydew import custom_types
from honeydew import errors
from honeydew.transports import ssh

_INPUT_ARGS: Dict[str, Any] = {
    "device_name": "fuchsia-emulator",
    "ssh_private_key": "/tmp/.ssh/pkey",
    "ssh_user": "root",
}

_MOCK_ARGS: Dict[str, Any] = {
    "target_ssh_address": custom_types.TargetSshAddress(
        ip="11.22.33.44", port=22
    ),
}


def _custom_test_name_func(testcase_func, _, param) -> str:
    """Custom name function method."""
    test_func_name: str = testcase_func.__name__

    params_dict: Dict[str, Any] = param.args[0]
    test_label: str = parameterized.to_safe_name(params_dict["label"])

    return f"{test_func_name}_with_{test_label}"


class SshTests(unittest.TestCase):
    """Unit tests for honeydew.transports.ssh.py."""

    def setUp(self) -> None:
        super().setUp()

        self.ssh_obj = ssh.SSH(
            device_name=_INPUT_ARGS["device_name"],
            private_key=_INPUT_ARGS["ssh_private_key"],
        )

    @mock.patch("time.sleep", autospec=True)
    @mock.patch.object(
        ssh.SSH,
        "run",
        side_effect=[subprocess.CalledProcessError, b"some output"],
        autospec=True,
    )
    def test_ssh_check_connection_success(
        self, mock_ssh_run, mock_sleep
    ) -> None:
        """Testcase for SSH.check_connection() success case"""
        self.ssh_obj.check_connection(timeout=5)

        mock_ssh_run.assert_called()
        mock_sleep.assert_called()

    @mock.patch("time.sleep", autospec=True)
    @mock.patch("time.time", side_effect=[0, 1, 2], autospec=True)
    @mock.patch.object(
        ssh.SSH, "run", side_effect=subprocess.CalledProcessError, autospec=True
    )
    def test_ssh_check_connection_fail(
        self, mock_ssh_run, mock_time, mock_sleep
    ) -> None:
        """Testcase for SSH.check_connection() failure case"""
        with self.assertRaises(errors.SSHCommandError):
            self.ssh_obj.check_connection(timeout=2)

        mock_ssh_run.assert_called()
        mock_time.assert_called()
        mock_sleep.assert_called()

    @mock.patch.object(
        ssh.subprocess,
        "check_output",
        return_value=b"some output",
        autospec=True,
    )
    @mock.patch.object(
        ssh.ffx_transport.FFX,
        "get_target_ssh_address",
        return_value=_MOCK_ARGS["target_ssh_address"],
        autospec=True,
    )
    def test_ssh_run(
        self, mock_get_target_ssh_address, mock_check_output
    ) -> None:
        """Testcase for SSH.run()"""
        self.assertEqual(
            self.ssh_obj.run(command="some_command"), "some output"
        )

        mock_get_target_ssh_address.assert_called()
        mock_check_output.assert_called()

    @parameterized.expand(
        [
            (
                {
                    "label": "CalledProcessError",
                    "side_effect": subprocess.CalledProcessError(
                        returncode=1,
                        cmd="ssh fuchsia@12.34.56.78:ls",
                        output="command output",
                        stderr="command error",
                    ),
                },
            ),
            (
                {
                    "label": "RuntimeError",
                    "side_effect": RuntimeError("command failed"),
                },
            ),
        ],
        name_func=_custom_test_name_func,
    )
    @mock.patch.object(ssh.subprocess, "check_output", autospec=True)
    @mock.patch.object(
        ssh.ffx_transport.FFX,
        "get_target_ssh_address",
        return_value=_MOCK_ARGS["target_ssh_address"],
        autospec=True,
    )
    def test_ssh_run_exception(
        self, parameterized_dict, mock_get_target_ssh_address, mock_check_output
    ) -> None:
        """Testcase for SSH.run() raising errors.SSHCommandError exception"""
        mock_check_output.side_effect = parameterized_dict["side_effect"]

        with self.assertRaises(errors.SSHCommandError):
            self.ssh_obj.run(command="some_command")

        mock_get_target_ssh_address.assert_called()
        mock_check_output.assert_called()
