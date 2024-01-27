#!/usr/bin/env fuchsia-vendored-python
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Validate the sysmgr config for the product being built.

Sysmgr's configuration is provided through the config-data package as a set of
JSON files, which are all individually read by sysmgr at runtime and merged to
form its overall configuration. This tool validates that there are no conflicts
between files, e.g. multiple files providing different component URLs for the
same "services" key. For example, it catches invalid configuration like this::


  file1.config:
  {
    "services": {
      "fuchsia.my.Service": "fuchsia-pkg://fuchsia.com/package_a#meta/foo.cmx"
    }
  }

  file2.config:
  {
    "services": {
      "fuchsia.my.Service": "fuchsia-pkg://fuchsia.com/package_b#meta/bar.cmx"
    }
  }

The input provided to this tool is expected to be the config-data package
manifest, formatted like this::

  meta/data/some_package/foo=../../src/somewhere/foo
  meta/data/sysmgr/file1.config=../../src/bar/file1.config
  meta/data/other_package/baz=../../some/other/path/baz

where the path before the '=' is the destination path in the package, and the
path after the '=' is the source file (rebased to the root build directory).
"""

from collections import defaultdict
import argparse
import json
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--depfile',
        type=argparse.FileType('w'),
        help='Write a GN depfile to the given path',
        required=True)
    parser.add_argument(
        '--merged',
        type=argparse.FileType('w'),
        help='Write a snapshot of this sysmgr config to the given path',
        required=True)
    parser.add_argument(
        '--config-entries',
        type=argparse.FileType('r'),
        help='config_data entries',
        required=True)
    args = parser.parse_args()

    # Build a list of all the source paths that contribute to sysmgr's config data, and a map from
    # destination paths to the source paths which map to them (to detect collisions).
    config_entries = json.load(args.config_entries)
    sysmgr_config_files = []
    destination_source_files = defaultdict(set)
    sysmgr_config_files = []
    for e in config_entries:
        src = e['source']
        dst = e['destination']
        if dst.startswith('meta/data/sysmgr/'):
            sysmgr_config_files.append(src)
            destination_source_files[dst].add(src)

    # Detect whenever > 1 different source paths map to the same destination.
    destination_conflicts = False
    for dest_file, source_files in destination_source_files.items():
        if len(source_files) > 1:
            print(
                'Different sysmgr config files map to the same destination {}.  Source files: {}'
                .format(dest_file, ', '.join(source_files)))
            destination_conflicts = True

    # De-dup the list.
    sysmgr_config_files = list(dict.fromkeys(sysmgr_config_files))

    # Parse all config files.
    #
    # Build a list of all conflicts, rather than
    # failing immediately on the first conflict. This allows us to print a more
    # useful build failure so that developers don't need to play whack-a-mole with
    # multiple conflicts.
    configs = []
    files_by_service = defaultdict(list)
    for config_file in sysmgr_config_files:
        with open(config_file, 'r') as f:
            config = json.load(f)
            configs.append(config)

            services = config.get('services')
            if services:
                for service in services.keys():
                    files_by_service[service].append(config_file)

    # If any conflicts were detected, print a useful error message and then
    # exit.
    service_conflicts = False
    for service, config_files in files_by_service.items():
        if len(config_files) > 1:
            print(
                'Duplicate sysmgr configuration for service {} in files: {}'.
                format(service, ', '.join(config_files)))
            service_conflicts = True

    if service_conflicts or destination_conflicts:
        return 1

    # Create a single merged configuration analogous to sysmgr's init itself.
    merged_config = {}
    for config in configs:
        for category, values in config.items():
            existing = merged_config.get(category)
            if type(existing) is dict:
                merged_config[category].update(values)
            elif type(existing) is list:
                merged_config[category] += values
            else:
                merged_config[category] = values

    # Use the same options as //scripts/style/json-fmt.py.
    json.dump(
        merged_config,
        args.merged,
        indent=4,
        separators=(',', ': '),
        sort_keys=True)
    args.merged.write('\n')

    # Write the depfile, which is a Makefile format file that has a single output
    # (the merged file) and lists all input files as dependencies.
    args.depfile.write(
        '{}: {}\n'.format(args.merged, ' '.join(sysmgr_config_files)))

    return 0


if __name__ == '__main__':
    sys.exit(main())
