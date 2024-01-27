#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Unit tests for honeydew.device_classes.fuchsia_device_base.py."""

import base64
import subprocess
import unittest
from http.client import RemoteDisconnected
from typing import Any, Dict
from unittest import mock

from honeydew import errors
from honeydew.device_classes import fuchsia_device_base
from parameterized import parameterized

# pylint: disable=protected-access
_INPUT_ARGS: Dict[str, str] = {
    "device_name": "fuchsia-emulator",
    "ssh_private_key": "/tmp/.ssh/pkey",
    "ssh_user": "root",
    "device_ip_address": "11.22.33.44"
}

_MOCK_ARGS: Dict[str, Any] = {
    "device_name": "fuchsia-emulator",
    "device_ip_address": "12.34.56.78",
    "device_type": "qemu-x64",
    "sl4f_request": fuchsia_device_base._SL4F_METHODS["GetDeviceName"],
    "sl4f_response": {
        "id": "",
        "result": "fuchsia-emulator",
        "error": None,
    },
    "sl4f_error_response": {
        "id": "",
        "error": "some error",
    },
}

_BASE64_ENCODED_STR = "some base64 encoded string=="


def _custom_test_name_func(testcase_func, _, param) -> str:
    """Custom test name function method."""
    test_func_name: str = testcase_func.__name__

    params_dict: Dict[str, Any] = param.args[0]
    test_label: str = parameterized.to_safe_name(params_dict["label"])

    return f"{test_func_name}_with_{test_label}"


class FuchsiaDeviceBaseTests(unittest.TestCase):
    """Unit tests for honeydew.device_classes.fuchsia_device_base.py."""

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={"result": _INPUT_ARGS["device_name"]},
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.fuchsia_device.ffx_cli,
        "get_target_address",
        return_value=_MOCK_ARGS["device_ip_address"],
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.subprocess,
        "check_output",
        return_value=b"some output",
        autospec=True)
    def setUp(
            self, mock_check_output, mock_get_target_address,
            mock_send_sl4f_command) -> None:
        super().setUp()

        self.fd_obj = fuchsia_device_base.FuchsiaDeviceBase(
            device_name=_INPUT_ARGS["device_name"],
            ssh_private_key=_INPUT_ARGS["ssh_private_key"])

        mock_get_target_address.assert_called_once()

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["GetDeviceName"])

        mock_check_output.assert_called_once()

    # List all the tests related to __init__ in alphabetical order
    @parameterized.expand(
        [
            (
                {
                    "label": "all_optional_params",
                    "mandatory_params":
                        {
                            "device_name": _INPUT_ARGS["device_name"],
                            "ssh_private_key": _INPUT_ARGS["ssh_private_key"],
                        },
                    "optional_params":
                        {
                            "ssh_user":
                                _INPUT_ARGS["ssh_user"],
                            "device_ip_address":
                                _INPUT_ARGS["device_ip_address"]
                        },
                },),
            (
                {
                    "label": "no_ssh_user",
                    "mandatory_params":
                        {
                            "device_name": _INPUT_ARGS["device_name"],
                            "ssh_private_key": _INPUT_ARGS["ssh_private_key"],
                        },
                    "optional_params":
                        {
                            "device_ip_address":
                                _INPUT_ARGS["device_ip_address"]
                        },
                },),
            (
                {
                    "label": "no_device_ip_address",
                    "mandatory_params":
                        {
                            "device_name": _INPUT_ARGS["device_name"],
                            "ssh_private_key": _INPUT_ARGS["ssh_private_key"],
                        },
                    "optional_params": {
                        "ssh_user": _INPUT_ARGS["ssh_user"],
                    },
                },),
        ],
        name_func=_custom_test_name_func)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.fuchsia_device.ffx_cli,
        "get_target_address",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.subprocess,
        "check_output",
        return_value=b"some output",
        autospec=True)
    def test_fuchsia_device_base_init(
            self, parameterized_dict, mock_check_output,
            mock_get_target_address, mock_send_sl4f_command) -> None:
        """Verify FuchsiaDeviceBase class instantiation."""
        optional_params: Dict[str, Any] = parameterized_dict["optional_params"]

        device_name: str = parameterized_dict["mandatory_params"]["device_name"]
        ssh_private_key: str = parameterized_dict["mandatory_params"][
            "ssh_private_key"]

        mock_send_sl4f_command.return_value = {"result": device_name}

        fd_obj = fuchsia_device_base.FuchsiaDeviceBase(
            device_name=device_name,
            ssh_private_key=ssh_private_key,
            **optional_params)

        self.assertIsInstance(fd_obj, fuchsia_device_base.FuchsiaDeviceBase)

        mock_check_output.assert_called_once()

        mock_send_sl4f_command.assert_called_once_with(
            fd_obj, method=fuchsia_device_base._SL4F_METHODS["GetDeviceName"])

        if "device_ip_address" not in optional_params:
            mock_get_target_address.assert_called_once()
        else:
            mock_get_target_address.assert_not_called()

    # List all the tests related to static properties in alphabetical order
    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "get_target_type",
        return_value=_MOCK_ARGS["device_type"],
        autospec=True)
    def test_device_type(self, mock_ffx_cli_get_target_type) -> None:
        """Testcase for FuchsiaDeviceBase.device_type property"""
        self.assertEqual(self.fd_obj.device_type, _MOCK_ARGS["device_type"])
        mock_ffx_cli_get_target_type.assert_called_once_with(self.fd_obj.name)

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={
            "result":
                {
                    "manufacturer": "default-manufacturer",
                    "model": "default-model",
                    "name": "default-product-name",
                }
        },
        autospec=True)
    def test_manufacturer(self, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.manufacturer property"""
        self.assertEqual(self.fd_obj.manufacturer, "default-manufacturer")

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["GetProductInfo"])

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={
            "result":
                {
                    "manufacturer": "default-manufacturer",
                    "model": "default-model",
                    "name": "default-product-name",
                }
        },
        autospec=True)
    def test_model(self, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.model property"""
        self.assertEqual(self.fd_obj.model, "default-model")

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["GetProductInfo"])

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={
            "result":
                {
                    "manufacturer": "default-manufacturer",
                    "model": "default-model",
                    "name": "default-product-name",
                }
        },
        autospec=True)
    def test_product_name(self, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.product_name property"""
        self.assertEqual(self.fd_obj.product_name, "default-product-name")

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["GetProductInfo"])

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={"result": {
            "serial_number": "default-serial-number",
        }},
        autospec=True)
    def test_serial_number(self, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.serial_number property"""
        self.assertEqual(self.fd_obj.serial_number, "default-serial-number")

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["GetDeviceInfo"])

    # List all the tests related to dynamic properties in alphabetical order
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={"result": "1.2.3"},
        autospec=True)
    def test_firmware_version(self, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.firmware_version property"""
        self.assertEqual(self.fd_obj.firmware_version, "1.2.3")

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj, method=fuchsia_device_base._SL4F_METHODS["GetVersion"])

    # List all the tests related to public methods in alphabetical order
    # Note - Test case for FuchsiaDeviceBase.start_sl4f_server() is covered in
    # test_fuchsia_device_base_init

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={"result": _MOCK_ARGS["device_name"]},
        autospec=True)
    def test_check_sl4f_connection(self, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.check_sl4f_connection()"""
        self.fd_obj.check_sl4f_connection()

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["GetDeviceName"])

    # pytype: disable=attribute-error
    def test_close(self) -> None:
        """Testcase for FuchsiaDeviceBase.close()"""
        self.fd_obj.close()

    # pytype: enable=attribute-error

    @parameterized.expand(
        [
            (
                {
                    "label": "info_level",
                    "log_level": fuchsia_device_base.custom_types.LEVEL.INFO,
                    "sl4f_method": "LogInfo",
                    "log_message": "info message",
                },),
            (
                {
                    "label": "warning_level",
                    "log_level": fuchsia_device_base.custom_types.LEVEL.WARNING,
                    "sl4f_method": "LogWarning",
                    "log_message": "warning message",
                },),
            (
                {
                    "label": "error_level",
                    "log_level": fuchsia_device_base.custom_types.LEVEL.ERROR,
                    "sl4f_method": "LogError",
                    "log_message": "error message",
                },),
        ],
        name_func=_custom_test_name_func)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        autospec=True)
    def test_log_message_to_device(
            self, parameterized_dict, mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.log_message_to_device()"""
        self.fd_obj.log_message_to_device(
            level=parameterized_dict["log_level"],
            message=parameterized_dict["log_message"])

        mock_send_sl4f_command.assert_called_once()

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_wait_for_bootup_complete",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_wait_for_offline",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "log_message_to_device",
        autospec=True)
    def test_power_cycle(
            self, mock_log_message_to_device, mock_wait_for_offline,
            mock_wait_for_bootup_complete) -> None:
        """Testcase for FuchsiaDeviceBase.power_cycle()"""
        power_switch = mock.MagicMock(
            spec=fuchsia_device_base.power_switch_interface.PowerSwitch)
        self.fd_obj.power_cycle(power_switch=power_switch, outlet=5)

        self.assertEqual(mock_log_message_to_device.call_count, 2)

        power_switch.power_off.assert_called_once()
        mock_wait_for_offline.assert_called_once()

        power_switch.power_on.assert_called_once()
        mock_wait_for_bootup_complete.assert_called_once()

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_wait_for_bootup_complete",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_wait_for_offline",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "log_message_to_device",
        autospec=True)
    def test_reboot(
            self, mock_log_message_to_device, mock_send_sl4f_command,
            mock_wait_for_offline, mock_wait_for_bootup_complete) -> None:
        """Testcase for FuchsiaDeviceBase.reboot()"""
        self.fd_obj.reboot()

        self.assertEqual(mock_log_message_to_device.call_count, 2)

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj,
            method=fuchsia_device_base._SL4F_METHODS["Reboot"],
            exceptions_to_skip=[RemoteDisconnected])

        mock_wait_for_offline.assert_called_once()

        mock_wait_for_bootup_complete.assert_called_once()

    @parameterized.expand(
        [
            (
                {
                    "label": "no_snapshot_file_arg",
                    "directory": "/tmp",
                    "optional_params": {},
                },),
            (
                {
                    "label": "snapshot_file_arg",
                    "directory": "/tmp",
                    "optional_params": {
                        "snapshot_file": "snapshot.zip",
                    },
                },),
        ],
        name_func=_custom_test_name_func)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "send_sl4f_command",
        return_value={"result": {
            "zip": _BASE64_ENCODED_STR
        }},
        autospec=True)
    @mock.patch.object(fuchsia_device_base.os, "makedirs", autospec=True)
    def test_snapshot(
            self, parameterized_dict, mock_makedirs,
            mock_send_sl4f_command) -> None:
        """Testcase for FuchsiaDeviceBase.snapshot()"""
        directory: str = parameterized_dict["directory"]
        optional_params: Dict[str, Any] = parameterized_dict["optional_params"]

        with mock.patch("builtins.open", mock.mock_open()) as mocked_file:
            snapshot_file_path: str = self.fd_obj.snapshot(
                directory=directory, **optional_params)

        if "snapshot_file" in optional_params:
            self.assertEqual(
                snapshot_file_path,
                f"{directory}/{optional_params['snapshot_file']}")
        else:
            self.assertRegex(
                snapshot_file_path,
                f"{directory}/Snapshot_{self.fd_obj.name}_.*.zip")

        mocked_file.assert_called_once_with(snapshot_file_path, "wb")
        mocked_file().write.assert_called_once_with(
            base64.b64decode(_BASE64_ENCODED_STR))

        mock_makedirs.assert_called_once_with(directory)

        mock_send_sl4f_command.assert_called_once_with(
            self.fd_obj, method=fuchsia_device_base._SL4F_METHODS["Snapshot"])

    # List all the tests related to private methods in alphabetical order

    @mock.patch("time.sleep", autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_run_ssh_command_on_host",
        side_effect=[subprocess.CalledProcessError, b"some output"],
        autospec=True)
    def test_check_ssh_connection_to_device_success(
            self, mock_run_ssh_command_on_host, mock_sleep) -> None:
        """Testcase for FuchsiaDeviceBase._check_ssh_connection_to_device()
        success case"""
        self.fd_obj._check_ssh_connection_to_device(timeout=5)

        mock_run_ssh_command_on_host.assert_called_with(
            self.fd_obj, command=fuchsia_device_base._CMDS["ECHO"])
        mock_sleep.assert_called_with(1)

    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_run_ssh_command_on_host",
        side_effect=subprocess.CalledProcessError,
        autospec=True)
    def test_check_ssh_connection_to_device_fail(
            self, mock_run_ssh_command_on_host) -> None:
        """Testcase for FuchsiaDeviceBase._check_ssh_connection_to_device()
        failure case"""
        with self.assertRaisesRegex(
                fuchsia_device_base.errors.FuchsiaDeviceError,
                f"Failed to connect to '{self.fd_obj.name}' via SSH."):
            self.fd_obj._check_ssh_connection_to_device(timeout=2)

        mock_run_ssh_command_on_host.assert_called_with(
            self.fd_obj, command=fuchsia_device_base._CMDS["ECHO"])

    @mock.patch("time.sleep", autospec=True)
    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "get_target_address",
        side_effect=[
            errors.FuchsiaDeviceError, _MOCK_ARGS["device_ip_address"]
        ],
        autospec=True)
    def test_get_device_ip_address_success(
            self, mock_get_target_address, mock_sleep) -> None:
        """Testcase for FuchsiaDeviceBase._get_device_ip_address() success
        case"""
        self.fd_obj._get_device_ip_address(timeout=5)

        mock_get_target_address.assert_called_with(self.fd_obj.name, timeout=5)
        mock_sleep.assert_called_with(1)

    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "get_target_address",
        side_effect=errors.FuchsiaDeviceError,
        autospec=True)
    def test_get_device_ip_address_fail(self, mock_get_target_address) -> None:
        """Testcase for FuchsiaDeviceBase._get_device_ip_address() failure
        case"""

        with self.assertRaisesRegex(
                fuchsia_device_base.errors.FuchsiaDeviceError,
                f"Failed to get the ip address of '{self.fd_obj.name}'."):
            self.fd_obj._get_device_ip_address(timeout=2)

        mock_get_target_address.assert_called_with(self.fd_obj.name, timeout=2)

    @parameterized.expand(
        [
            (
                {
                    "label": "ipv4",
                    "py_ver": (3, 10),
                    "ip": "12.34.56.78",
                    "expected": "12.34.56.78",
                },),
            (
                {
                    "label": "ipv6_with_scope_id_on_py38",
                    "py_ver": (3, 8),
                    "ip": "fe80::1f91:2f5c:5e9b:7ff3%qemu",
                    "expected": "fe80::1f91:2f5c:5e9b:7ff3",
                },),
            (
                {
                    "label": "ipv6_with_scope_id_on_py310",
                    "py_ver": (3, 10),
                    "ip": "fe80::1f91:2f5c:5e9b:7ff3%qemu",
                    "expected": "fe80::1f91:2f5c:5e9b:7ff3%qemu",
                },),
            (
                {
                    "label": "ipv6_without_scope_id",
                    "py_ver": (3, 10),
                    "ip": "fe80::1f91:2f5c:5e9b:7ff3",
                    "expected": "fe80::1f91:2f5c:5e9b:7ff3",
                },),
        ],
        name_func=_custom_test_name_func)
    def test_normalize_ip_addr(self, parameterized_dict) -> None:
        """Test case for FuchsiaDeviceBase._normalize_ip_addr()"""
        with mock.patch.object(fuchsia_device_base.sys, "version_info",
                               parameterized_dict["py_ver"]):
            self.assertEqual(
                self.fd_obj._normalize_ip_addr(parameterized_dict["ip"]),
                parameterized_dict["expected"])

    @mock.patch.object(
        fuchsia_device_base.subprocess,
        "check_output",
        return_value=b"some output",
        autospec=True)
    def test_run_ssh_command_on_host(self, mock_check_output) -> None:
        """Testcase for FuchsiaDeviceBase._run_ssh_command_on_host()"""
        command = "some_command"

        self.assertEqual(
            self.fd_obj._run_ssh_command_on_host(command=command),
            "some output")

        mock_check_output.assert_called_once()

    @mock.patch.object(
        fuchsia_device_base.subprocess,
        "check_output",
        side_effect=subprocess.CalledProcessError(
            returncode=1, cmd="ssh fuchsia@12.34.56.78:ls"),
        autospec=True)
    def test_run_ssh_command_on_host_exception(self, mock_check_output) -> None:
        """Testcase for FuchsiaDeviceBase._run_ssh_command_on_host() raising
        errors.SSHCommandError exception"""
        command = "some_command"

        with self.assertRaises(errors.SSHCommandError):
            self.fd_obj._run_ssh_command_on_host(command=command)

        mock_check_output.assert_called_once()

    @parameterized.expand(
        [
            (
                {
                    "label": "just_mandatory_method_arg",
                    "method": _MOCK_ARGS["sl4f_request"],
                    "optional_params": {},
                    "mock_http_response": _MOCK_ARGS["sl4f_response"],
                },),
            (
                {
                    "label": "optional_params_arg",
                    "method": _MOCK_ARGS["sl4f_request"],
                    "optional_params": {
                        "params": {
                            "message": "message"
                        },
                    },
                    "mock_http_response": _MOCK_ARGS["sl4f_response"],
                },),
        ],
        name_func=_custom_test_name_func)
    @mock.patch.object(
        fuchsia_device_base.http_utils,
        "send_http_request",
        return_value=_MOCK_ARGS["sl4f_response"],
        autospec=True)
    def test_send_sl4f_command_success(
            self, parameterized_dict, mock_send_http_request) -> None:
        """Testcase for FuchsiaDeviceBase.send_sl4f_command() success case"""
        method = parameterized_dict["method"]

        response = self.fd_obj.send_sl4f_command(method=method)

        self.assertEqual(response, parameterized_dict["mock_http_response"])

        mock_send_http_request.assert_called_once()

    @mock.patch.object(
        fuchsia_device_base.http_utils,
        "send_http_request",
        return_value=_MOCK_ARGS["sl4f_error_response"],
        autospec=True)
    def test_send_sl4f_command_fail_because_of_error_in_resp(
            self, mock_send_http_request) -> None:
        """Testcase for FuchsiaDeviceBase.send_sl4f_command() failure case when
        there is 'error' in SL4F response received."""
        method: str = _MOCK_ARGS["sl4f_request"]
        expected_error: str | None = _MOCK_ARGS["sl4f_error_response"]["error"]
        with self.assertRaisesRegex(errors.FuchsiaDeviceError,
                                    f"Error: '{expected_error}'"):
            self.fd_obj.send_sl4f_command(method=method, attempts=5, interval=0)

        self.assertEqual(mock_send_http_request.call_count, 5)

    @mock.patch.object(
        fuchsia_device_base.http_utils,
        "send_http_request",
        side_effect=RuntimeError("some run time error"),
        autospec=True)
    def test_send_sl4f_command_fail_because_of_exception(
            self, mock_send_http_request) -> None:
        """Testcase for FuchsiaDeviceBase.send_sl4f_command() failure case when
        there is an exception thrown while sending HTTP request."""
        method: str = _MOCK_ARGS["sl4f_request"]
        with self.assertRaisesRegex(
                errors.FuchsiaDeviceError,
                f"SL4F method '{method}' failed on '{self.fd_obj.name}'."):
            self.fd_obj.send_sl4f_command(method=method, attempts=5, interval=0)

        mock_send_http_request.assert_called_once()

    @mock.patch.object(
        fuchsia_device_base.bluetooth_default.BluetoothDefault,
        "sys_init",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "device_type",
        new_callable=mock.PropertyMock,
        return_value="x64")
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "start_sl4f_server",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_check_ssh_connection_to_device",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_wait_for_online",
        autospec=True)
    @mock.patch.object(
        fuchsia_device_base.FuchsiaDeviceBase,
        "_get_device_ip_address",
        autospec=True)
    def test_wait_for_bootup_complete(
            self, mock_get_device_ip_address, mock_wait_for_online,
            mock_check_ssh_connection_to_device, mock_start_sl4f_server,
            mock_device_type, mock_bluetooth_sys_init) -> None:
        """Testcase for FuchsiaDeviceBase._wait_for_bootup_complete()"""
        self.fd_obj._wait_for_bootup_complete(timeout=10)

        mock_get_device_ip_address.assert_called_once()
        mock_wait_for_online.assert_called_once()
        mock_check_ssh_connection_to_device.assert_called_once()
        mock_start_sl4f_server.assert_called_once()
        mock_device_type.assert_called_once()
        mock_bluetooth_sys_init.assert_called()

    @mock.patch("time.sleep", autospec=True)
    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "check_ffx_connection",
        side_effect=[True, False],
        autospec=True)
    def test_wait_for_offline_success(
            self, mock_check_ffx_connection, mock_sleep) -> None:
        """Testcase for FuchsiaDeviceBase._wait_for_offline() success case"""
        self.fd_obj._wait_for_offline()

        mock_check_ffx_connection.assert_called_with(self.fd_obj.name)
        mock_sleep.assert_called()

    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "check_ffx_connection",
        return_value=True,
        autospec=True)
    def test_wait_for_offline_fail(self, mock_check_ffx_connection) -> None:
        """Testcase for FuchsiaDeviceBase._wait_for_offline() failure case"""
        with self.assertRaisesRegex(
                fuchsia_device_base.errors.FuchsiaDeviceError,
                "failed to go offline"):
            self.fd_obj._wait_for_offline(timeout=2)

        mock_check_ffx_connection.assert_called_with(self.fd_obj.name)

    @mock.patch("time.sleep", autospec=True)
    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "check_ffx_connection",
        side_effect=[False, True],
        autospec=True)
    def test_wait_for_online_success(
            self, mock_check_ffx_connection, mock_sleep) -> None:
        """Testcase for FuchsiaDeviceBase._wait_for_online() success case"""
        self.fd_obj._wait_for_online()

        mock_check_ffx_connection.assert_called_with(self.fd_obj.name)
        mock_sleep.assert_called()

    @mock.patch.object(
        fuchsia_device_base.ffx_cli,
        "check_ffx_connection",
        return_value=False,
        autospec=True)
    def test_wait_for_online_fail(self, mock_check_ffx_connection) -> None:
        """Testcase for FuchsiaDeviceBase._wait_for_online() failure case"""
        with self.assertRaisesRegex(
                fuchsia_device_base.errors.FuchsiaDeviceError,
                "failed to go online"):
            self.fd_obj._wait_for_online(timeout=2)

        mock_check_ffx_connection.assert_called_with(self.fd_obj.name)


if __name__ == "__main__":
    unittest.main()
