#!/usr/bin/env fuchsia-vendored-python
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Script to format JSON files.

This script accepts a list of files as arguments, and for each of them attempts
to parse it as JSON, and update it in-place with a pretty-printed version. Stops
on the first error.
"""

import argparse
import json
import os


def sort(data):
    if isinstance(data, dict):
        return {
            key: (
                sort(value) if key not in [
                    "args", "arguments", "children", "injected-services",
                    "networks", "setup", "test"
                ] else value) for key, value in data.items()
        }
    elif isinstance(data, list):
        inner = (sort(datum) for datum in data)
        if any(isinstance(datum, dict) for datum in data):
            return list(inner)
        return sorted(inner)
    else:
        return data


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        'file',
        type=argparse.FileType('r+'),
        nargs='+',
        help='JSON file to be pretty-printed.')
    parser.add_argument(
        '--sort-keys',
        default=True,
        action=argparse.BooleanOptionalAction,
        dest='sort_keys',
        help='Indicates whether object keys should be sorted.')

    args = parser.parse_args()
    for json_file in args.file:
        try:
            with json_file:
                original = json_file.read()
                data = json.loads(original)
                (root, ext) = os.path.splitext(json_file.name)
                if ext == '.cmx':
                    data = sort(data)
                formatted = json.dumps(
                    data,
                    indent=4,
                    sort_keys=args.sort_keys,
                    separators=(',', ': '))
                if original != formatted:
                    json_file.seek(0)
                    json_file.truncate()
                    json_file.write(formatted + '\n')
        except json.JSONDecodeError:
            print(f'Exception encountered while processing {json_file.name}')
            raise


if __name__ == "__main__":
    main()
