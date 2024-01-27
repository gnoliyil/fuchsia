#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Fuchsia base test class."""

import enum
import logging
from typing import List

from honeydew.device_classes import fuchsia_device_base
from honeydew.interfaces.device_classes import fuchsia_device
from honeydew.mobly_controller import \
    fuchsia_device as fuchsia_device_mobly_controller
from honeydew.utils import ffx_cli
from mobly import base_test, test_runner

_LOGGER: logging.Logger = logging.getLogger(__name__)


class SnapshotOn(enum.Enum):
    """How often we need to collect the snapshot"""
    TEARDOWN_CLASS = enum.auto()
    ON_FAIL = enum.auto()
    TEARDOWN_TEST = enum.auto()


class FuchsiaBaseTest(base_test.BaseTestClass):
    """Fuchsia base test class.

    Attributes:
        fuchsia_devices: List of FuchsiaDevice objects.
        test_case_path: Directory pointing to a specific test case artifacts.
        snapshot_on: `snapshot_on` test param value converted into SnapshotOn
            Enum.

    Required Mobly Test Params:
        snapshot_on (str): One of "teardown_class", "teardown_test", "on_fail".
            Default value is "teardown_class".
    """

    def setup_class(self) -> None:
        """setup_class is called once before running tests.

        It does the following things:
            * Reads user params passed to the test
            * Instantiates all fuchsia devices into self.fuchsia_devices
        """
        self._process_user_params()

        # Call `ffx_cli.setup` before calling `register_controller` as
        # `register_controller` results in calling an FFX command and we
        # wouldn't want to miss those FFX logs
        ffx_cli.setup(logs_dir=f"{self.log_path}/ffx/")

        self.fuchsia_devices: List[
            fuchsia_device.FuchsiaDevice] = self.register_controller(
                fuchsia_device_mobly_controller)

    def setup_test(self) -> None:
        """setup_test is called once before running each test.

        It does the following things:
            * Stores the current test case path into self.test_case_path
        """
        self.test_case_path: str = \
            f"{self.log_path}/{self.current_test_info.name}"
        self._health_check()

    def teardown_test(self) -> None:
        """teardown_test is called once after running each test.

        It does the following things:
            * Takes snapshot of all the fuchsia devices and stores it under
              test case directory if `snapshot_on` test param is set to
              "teardown_test"
        """
        if self.snapshot_on == SnapshotOn.TEARDOWN_TEST:
            self._collect_snapshot(directory=self.test_case_path)

    def teardown_class(self) -> None:
        """teardown_class is called once after running all tests.

        It does the following things:
            * Takes snapshot of all the fuchsia devices and stores it under
              "<log_path>/teardown_class" directory if `snapshot_on` test param
              is set to "teardown_class"
        """
        self._teardown_class_artifacts: str = f"{self.log_path}/teardown_class"
        if self.snapshot_on == SnapshotOn.TEARDOWN_CLASS:
            self._collect_snapshot(directory=self._teardown_class_artifacts)
        ffx_cli.close()

    def on_fail(self, _) -> None:
        """on_fail is called once when a test case fails.

        It does the following things:
            * Takes snapshot of all the fuchsia devices and stores it under
              test case directory if `snapshot_on` test param is set to
              "on_fail"
        """
        if self.snapshot_on == SnapshotOn.ON_FAIL:
            self._collect_snapshot(directory=self.test_case_path)

    def _collect_snapshot(self, directory: str) -> None:
        """Collects snapshots for all the FuchsiaDevice objects and stores them
        in the directory specified.

        Args:
            directory: Absolute path on the host where snapshot file need to be
                saved.
        """
        if not hasattr(self, "fuchsia_devices"):
            return

        _LOGGER.info(
            "Collecting snapshots of all the FuchsiaDevice objects in '%s'...",
            self.snapshot_on.name)
        for fx_device in self.fuchsia_devices:
            # type narrowing (https://mypy.readthedocs.io/en/stable/type_narrowing.html)
            # fx_device object to FuchsiaDeviceBase - needed for code completion
            # to work in vscode IDE
            assert isinstance(fx_device, fuchsia_device_base.FuchsiaDeviceBase)

            try:
                fx_device.snapshot(directory=directory)
            except Exception:  # pylint: disable=broad-except
                _LOGGER.warning("Unable to take snapshot of %s", fx_device.name)

    def _health_check(self) -> None:
        """Ensure all FuchsiaDevice objects are healthy."""
        _LOGGER.info(
            "Performing health checks on all the FuchsiaDevice objects...")
        for fx_device in self.fuchsia_devices:
            # type narrowing (https://mypy.readthedocs.io/en/stable/type_narrowing.html)
            # fx_device object to FuchsiaDeviceBase - needed for code completion
            # to work in vscode IDE
            assert isinstance(fx_device, fuchsia_device_base.FuchsiaDeviceBase)

            fx_device.sl4f.check_connection()

    def _process_user_params(self) -> None:
        """Reads, processes and stores the test params used by this module."""
        _LOGGER.info(
            "user_params associated with the test: %s", self.user_params)

        try:
            snapshot_on: str = self.user_params.get(
                "snapshot_on", SnapshotOn.TEARDOWN_CLASS.name).upper()
            self.snapshot_on: SnapshotOn = SnapshotOn[snapshot_on]
        except KeyError as err:
            _LOGGER.warning(
                "Invalid value %s passed in 'snapshot_on' test param. "
                "Valid values for this test param include: '%s', '%s','%s'. "
                "Proceeding with default value: '%s'", err,
                SnapshotOn.TEARDOWN_CLASS.name, SnapshotOn.TEARDOWN_TEST.name,
                SnapshotOn.ON_FAIL.name, SnapshotOn.TEARDOWN_CLASS.name)
            self.snapshot_on = SnapshotOn.TEARDOWN_CLASS


if __name__ == "__main__":
    test_runner.main()
