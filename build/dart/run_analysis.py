#!/usr/bin/env python3.8
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import subprocess
import sys
import json
import yaml


def main():
    parser = argparse.ArgumentParser('Runs analysis on a given package')
    parser.add_argument(
        '--source-file', help='Path to the list of sources', required=True)
    parser.add_argument(
        '--dot-packages', help='Path to the .packages file', required=True)
    parser.add_argument(
        '--dartanalyzer',
        help='Path to the Dart analyzer executable',
        required=True)
    parser.add_argument(
        '--dart-sdk', help='Path to the Dart SDK', required=True)
    parser.add_argument(
        '--options', help='Path to analysis options', required=True)
    parser.add_argument(
        '--stamp',
        type=argparse.FileType('w'),
        help='File to touch when analysis succeeds',
        required=True)
    parser.add_argument(
        '--depname', help='Name of the depfile target', required=True)
    parser.add_argument(
        '--depfile',
        type=argparse.FileType('w'),
        help='Path to the depfile to generate',
        required=True)
    parser.add_argument(
        '--all-deps-sources-file',
        type=argparse.FileType('r'),
        help=
        'Path to a file containing sources this target and all of its direct and transitive dependencies',
        required=True)
    args = parser.parse_args()

    with open(args.source_file, 'r') as source_file:
        sources = source_file.read().strip().split('\n')

    args.depfile.write(
        '{}: {}'.format(
            args.depname,
            ' '.join(json.load(args.all_deps_sources_file)),
        ),
    )

    options = args.options
    while True:
        if not os.path.exists(options):
            print('Could not find options file: {}'.format(options))
            return 1
        args.depfile.write(' {}'.format(options))
        with open(options, 'r') as options_file:
            content = yaml.safe_load(options_file)
            if not 'include' in content:
                break
            included = content['include']
            if not os.path.isabs(included):
                included = os.path.join(os.path.dirname(options), included)
            options = included

    call_args = [
        args.dartanalyzer,
        '--packages={}'.format(args.dot_packages),
        '--dart-sdk={}'.format(args.dart_sdk),
        '--options={}'.format(args.options),
        '--fatal-warnings',
        '--fatal-hints',
        '--fatal-lints',
        '--enable-experiment',
        'non-nullable',
    ] + sources

    # Call Dart anaylzer.
    call = subprocess.Popen(
        call_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = call.communicate()

    # Convert output to strings, assuming UTF-8 output from dart.
    stdout = stdout.decode('utf-8')
    stderr = stderr.decode('utf-8')

    if call.returncode:
        print(stdout + stderr)
        return 1

    args.stamp.write('Success!')
    return 0


if __name__ == '__main__':
    sys.exit(main())
