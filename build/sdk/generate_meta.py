#!/usr/bin/env fuchsia-vendored-python
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import sys

from sdk_common import Atom


def sort_atoms(atoms):

    def key(ad):
        return (ad['meta'], ad['type'])

    return sorted(
        ({
            'meta': a.metadata,
            'type': a.type,
        } for a in atoms), key=key)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--manifest', help='Path to the SDK\'s manifest file', required=True)
    parser.add_argument(
        '--meta', help='Path to output metadata file', required=True)
    parser.add_argument(
        '--target-arch',
        help='Architecture of precompiled target atoms',
        required=True)
    parser.add_argument(
        '--host-arch', help='Architecture of host tools', required=True)
    parser.add_argument(
        '--id', help='Opaque identifier for the SDK', default='')
    parser.add_argument(
        '--schema-version',
        help='Opaque identifier for the metadata schemas',
        required=True)
    args = parser.parse_args()

    with open(args.manifest, 'r') as manifest_file:
        manifest = json.load(manifest_file)

    # sdk_noop_atoms may contain empty meta, skip them.
    atoms = [Atom(a) for a in manifest['atoms'] if a['meta']]
    meta = {
        'arch': {
            'host': args.host_arch,
            'target': [args.target_arch,],
        },
        'id': args.id,
        'parts': sort_atoms(atoms),
        'root': manifest['root'],
        'schema_version': args.schema_version,
    }

    with open(args.meta, 'w') as meta_file:
        json.dump(
            meta, meta_file, indent=2, sort_keys=True, separators=(',', ': '))


if __name__ == '__main__':
    sys.exit(main())
