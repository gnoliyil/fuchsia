#!/usr/bin/env fuchsia-vendored-python
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
A script used to populate a sysroot include directory exactly as specified by an input .json file.
Note that the script will ensure that any stale items in the output directory that are not listed
in the input file will be removed. Without this, when a header is removed from the sysroot
description, its old copy would linger on in the build output directory, which can lead to
silently broken builds that are hard to detect.
"""

# IMPORTANT: This relies on the JSON schema definition described in BUILD.gn in
# the same directory. Keep this script and the code in that file in sync at all
# times for correctness!

from __future__ import print_function

import argparse
import filecmp
import json
import os
import shutil
import sys


def _get_directory_contents(path):
    """Return the files and directories under |path| as a list of paths."""
    result = []
    for root, dirs, files in os.walk(path):
        assert root.startswith(path), 'root %s prefix %s' % (root, path)
        subdir = root[len(path):]
        if subdir:
            assert subdir[0] == os.sep, 'Invalid subdir %s' % subdir
            subdir = subdir[1:]
        for file in files:
            result.append(os.path.join(subdir, file))
        for dir in dirs:
            result.append(os.path.join(subdir, dir) + os.sep)
    return set(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)

    parser.add_argument(
        '--src-dir', required=True, help='Root source directory.')

    parser.add_argument(
        '--build-dir',
        help=
        'Build output directory of generated headers. Default to current one.')

    parser.add_argument(
        '--sysroot-json',
        required=True,
        help='Input JSON file describing the content of the sysroot.')

    parser.add_argument(
        '--sysroot-dir',
        required=True,
        help='Output directory to be populated by this script.')

    parser.add_argument(
        '--debug',
        action='store_true',
        help='Enable verbose mode for debugging this script.')

    parser.add_argument(
        '--dep-file',
        help='Optional dependency file to be generated by this script.')

    args = parser.parse_args()

    if args.build_dir:
        root_build_dir = os.path.abspath(args.build_dir)
    else:
        root_build_dir = os.getcwd()

    sysroot_dir = os.path.abspath(args.sysroot_dir)
    sysroot_include_dir = os.path.join(sysroot_dir, 'include')

    sysroot_include_old_files = _get_directory_contents(sysroot_include_dir)
    if args.debug:
        print('CURRENT CONTENTS FOR %s:' % sysroot_include_dir)
        print('\n'.join(sorted(sysroot_include_old_files)))

    with open(args.sysroot_json) as f:
        sysroot_json = json.load(f)

    root_src_dir = os.path.abspath(args.src_dir)

    # This maps destination file paths (relative to sysroot_include_dir) to
    # their source file location as in appears in the input JSON file.
    # Note that source file paths that begin with // are relative to the
    # root source directory, while others are relative to the build output
    # directory instead.
    sysroot_copy_map = {}

    sysroot_include_new_files = set()
    for entry in sysroot_json:
        if 'sdk' not in entry:
            continue
        sdk = entry['sdk']
        if 'include_dir' not in sdk:
            continue

        # This is a list of headers to copy.
        for header in sdk['headers']:
            sysroot_include_new_files.add(header)
            sysroot_include_new_files.add(os.path.dirname(header) + os.sep)
            sysroot_copy_map[header] = os.path.join(sdk['include_dir'], header)

    if args.debug:
        print('SYSROOT/include INPUT CONTENTS:')
        print('\n'.join(sorted(sysroot_include_new_files)))

    # For any input file, compare with the existing version if any to avoid
    # touching the sysroot file if they are the same.
    dep_entries = []
    for entry in sorted(sysroot_include_new_files):
        dst_path = os.path.join(sysroot_include_dir, entry)

        if entry.endswith(os.sep):
            # This is a directory entry.
            if not os.path.exists(dst_path):
                if args.debug:
                    print('MKDIR ' + dst_path)
                os.makedirs(dst_path)
        else:
            # This is a file entry.
            src_path = sysroot_copy_map[entry]
            if src_path.startswith('//'):
                src_path = os.path.join(root_src_dir, src_path[2:])
            else:
                src_path = os.path.join(root_build_dir, src_path)

            dep_entries += [
                '%s: %s' % (
                    os.path.relpath(dst_path, root_build_dir),
                    os.path.relpath(src_path, root_build_dir))
            ]
            if os.path.exists(dst_path) and filecmp.cmp(dst_path, src_path):
                if args.debug:
                    print('SKIP ' + dst_path)
                continue

            if args.debug:
                print('COPY %s <-- %s' % (dst_path, src_path))
            shutil.copyfile(src_path, dst_path)

    # Remove any extra files or directories that are no longer needed.
    # Use reverse sort to ensure that files are removed before the
    # directories that contain them.
    for extra in sorted(sysroot_include_old_files - sysroot_include_new_files,
                        reverse=True):
        extra_path = os.path.join(sysroot_include_dir, extra)
        if extra.endswith(os.sep):
            if args.debug:
                print('REMOVING DIR  ' + extra_path)
            os.rmdir(extra_path)
        else:
            if args.debug:
                print('REMOVING FILE ' + extra_path)
            os.unlink(extra_path)

    if args.dep_file:
        with open(args.dep_file, 'wt') as f:
            f.write('\n'.join(dep_entries))

    return 0


if __name__ == '__main__':
    sys.exit(main())
