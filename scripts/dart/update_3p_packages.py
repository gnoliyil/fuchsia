#!/usr/bin/env fuchsia-vendored-python
# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import paths
import subprocess
import sys

# These are the locations of pubspec files that are roots of the dependency
# graph.  If they contain conflicting requirements for a package 'pub get' will
# error out and the conflicts will have to be resolved before the packages can
# be updated.
ROOT_PUBSPECS = [
    'prebuilt/third_party/sky_engine',
]

FLUTTER_GIT = 'https://github.com/flutter/flutter.git'
FLUTTER_ROOT = 'third_party/dart-pkg/git/flutter'
FLUTTER_PUBSPECS = [
    'packages/flutter',
    'packages/flutter_driver',
    'packages/flutter_test',
    'packages/flutter_tools',
    'packages/fuchsia_remote_debug_protocol',
]

# These are the locations of yaml files listing the Dart dependencies of a git
# project.
PROJECT_DEPENDENCIES = [
    'src/dart',
]


def main():
    parser = argparse.ArgumentParser('Update third-party Dart packages')
    parser.add_argument(
        '--changelog', help='Path to the changelog file to write', default=None)
    parser.add_argument(
        '--debug', help='Turns on debugging mode', action='store_true')
    # Accept an optional flutter commit/revision id.
    parser.add_argument(
        '--flutter-revision', help='A git hash within the flutter repo')
    script_args = parser.parse_args()

    if sys.platform.startswith('linux'):
        platform = 'linux-x64'
    elif sys.platform.startswith('darwin'):
        platform = 'mac-x64'
    else:
        print('Unsupported platform: %s' % sys.platform)
        return 1
    dart_path = os.path.join(
        paths.FUCHSIA_ROOT, 'prebuilt', 'third_party', 'dart', platform, 'bin',
        'dart')
    importer_path = os.path.join(
        paths.FUCHSIA_ROOT, 'scripts', 'dart', 'package_importer.py')
    output_path = os.path.join(
        paths.FUCHSIA_ROOT, 'third_party', 'dart-pkg', 'pub')
    flutter_absolute_root = os.path.join(paths.FUCHSIA_ROOT, FLUTTER_ROOT)

    # flutter --version has the side effect of creating a version file that pub
    # uses to find which package versions are compatible with the current version
    # of flutter
    flutter_tool = os.path.join(flutter_absolute_root, 'bin', 'flutter')
    subprocess.check_call([flutter_tool, '--version'])

    python_tool = os.path.join(
        paths.FUCHSIA_ROOT, 'prebuilt', 'third_party', 'python3', platform,
        'bin', 'python3.8')
    args = [python_tool, importer_path]
    if script_args.debug:
        args.append('--debug')
    args.extend(['--dart', dart_path])
    args.extend(['--output', output_path])
    args.append('--pubspecs')
    for root in ROOT_PUBSPECS:
        args.append(os.path.join(paths.FUCHSIA_ROOT, root))
    if script_args.flutter_revision:
        args.append('--git-pubspecs')
        for flutter_pubspec in FLUTTER_PUBSPECS:
            args.append(
                ','.join(
                    [
                        flutter_pubspec.split('/').pop(), FLUTTER_GIT,
                        script_args.flutter_revision, flutter_pubspec
                    ]))
    else:
        for flutter_pubspec in FLUTTER_PUBSPECS:
            args.append(os.path.join(flutter_absolute_root, flutter_pubspec))
    args.append('--projects')
    for project in PROJECT_DEPENDENCIES:
        args.append(os.path.join(paths.FUCHSIA_ROOT, project))
    if script_args.changelog:
        args.extend([
            '--changelog',
            script_args.changelog,
        ])

    env = dict(os.environ, FLUTTER_ROOT=flutter_absolute_root)
    subprocess.check_call(args, env=env)


if __name__ == '__main__':
    sys.exit(main())
