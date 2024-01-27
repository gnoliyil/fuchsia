#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import unittest
from unittest import mock
from unittest.mock import patch

import yaml

import common


class CommonTest(unittest.TestCase):

    @patch('builtins.open')
    @patch('yaml.load')
    def test_read_yaml_from_file_success(self, *unused_args):
        common.read_yaml_from_file('path/to/file')

    @patch('builtins.open', side_effect=IOError)
    def test_read_yaml_from_file_open_failure_raises_exception(
            self, *unused_args):
        with self.assertRaises(IOError):
            common.read_yaml_from_file('path/to/file')

    @patch('builtins.open')
    @patch('yaml.load', side_effect=yaml.YAMLError)
    def test_read_yaml_from_file_yaml_parse_failure_raises_exception(
            self, *unused_args):
        with self.assertRaises(common.InvalidFormatException):
            common.read_yaml_from_file('path/to/file')

    @patch('builtins.open')
    @patch('json.load')
    def test_read_json_from_file_success(self, *unused_args):
        common.read_json_from_file('path/to/file')

    @patch('builtins.open')
    def test_read_json_from_file_open_failure_raises_exception(self, mock_open):
        mock_open.side_effect = IOError
        with self.assertRaises(IOError):
            common.read_json_from_file('path/to/file')

    @patch('builtins.open')
    @patch('json.load', side_effect=json.JSONDecodeError('msg', 'doc', 0))
    def test_read_json_from_file_json_parse_failure_raises_exception(
            self, *unused_args):
        with self.assertRaises(common.InvalidFormatException):
            common.read_json_from_file('path/to/file')
