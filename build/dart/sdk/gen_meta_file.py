#!/usr/bin/env fuchsia-vendored-python
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import os
import sys
import yaml

# The list of packages that should be pulled from a Flutter SDK instead of pub.
FLUTTER_PACKAGES = [
    'flutter',
    'flutter_driver',
    'flutter_test',
    'flutter_tools',
]


def main():
    parser = argparse.ArgumentParser('Builds a metadata file')
    parser.add_argument('--out', help='Path to the output file', required=True)
    parser.add_argument(
        '--name', help='Name of the original package', required=True)
    parser.add_argument(
        '--root', help='Root of the package in the SDK', required=True)
    parser.add_argument(
        '--specs', help='Path to spec files of dependencies', nargs='*')
    parser.add_argument(
        '--third-party-specs',
        help='Path to pubspec files of 3p dependencies',
        nargs='*')
    parser.add_argument('--sources', help='List of library sources', nargs='+')
    parser.add_argument(
        '--dart-library-null-safe',
        help='Null safety flag for dart libraries',
        action='store_true')
    args = parser.parse_args()

    metadata = {
        'type': 'dart_library',
        'name': args.name,
        'root': args.root,
        'sources': args.sources,
    }

    if args.dart_library_null_safe:
        metadata['dart_library_null_safe'] = True

    third_party_deps = []
    for spec in args.third_party_specs:
        with open(spec, 'r') as spec_file:
            manifest = yaml.safe_load(spec_file)
            name = manifest['name']
            dep = {
                'name': name,
            }
            if name in FLUTTER_PACKAGES:
                dep['version'] = 'flutter_sdk'
            else:
                if 'version' not in manifest:
                    raise Exception('%s does not specify a version.' % spec)
                dep['version'] = manifest['version']
            third_party_deps.append(dep)
    metadata['third_party_deps'] = third_party_deps

    deps = []
    fidl_deps = []
    for spec in args.specs:
        with open(spec, 'r') as spec_file:
            data = json.load(spec_file)
        type = data['type']
        name = data['name']
        if type == 'dart_library':
            deps.append(name)
        elif type == 'fidl_library':
            fidl_deps.append(name)
        else:
            raise Exception('Unsupported dependency type: %s' % type)
    metadata['deps'] = deps
    metadata['fidl_deps'] = fidl_deps

    with open(args.out, 'w') as out_file:
        json.dump(
            metadata,
            out_file,
            indent=2,
            sort_keys=True,
            separators=(',', ': '))

    return 0


if __name__ == '__main__':
    sys.exit(main())
