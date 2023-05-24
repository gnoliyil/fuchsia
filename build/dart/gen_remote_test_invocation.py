#!/usr/bin/env fuchsia-vendored-python
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import stat
import string
import sys


def main():
    parser = argparse.ArgumentParser(
        description='Generate a script that invokes the Dart tester')
    parser.add_argument(
        '--out', help='Path to the invocation file to generate', required=True)
    parser.add_argument(
        '--sources', help='Path to test files', required=True, nargs='+')
    parser.add_argument(
        '--dart-binary', help='Path to the dart binary.', required=True)
    parser.add_argument(
        '--dot-packages', help='Path to the .packages file', required=True)
    args = parser.parse_args()

    test_file = args.out
    test_path = os.path.dirname(test_file)
    if not os.path.exists(test_path):
        os.makedirs(test_path)

    sources_string = ' '.join(args.sources)
    script_template = string.Template(
        '''#!/bin/sh
# This artifact was generated by //build/dart/gen_remote_test_invocation.py
# Expects arg 1 to be the fuchsia remote address, and arg 2 to be the (optional)
# SSH config path.

set -e

export FUCHSIA_DEVICE_URL="$$1"
if [[ ! -z "$$2" ]]; then
  export FUCHSIA_SSH_CONFIG="$$2"
fi

for TEST in $sources_string; do
  $dart_binary --packages="$dot_packages" "$$TEST"
done
''')
    with open(test_file, 'w') as file:
        file.write(
            script_template.substitute(
                args.__dict__, sources_string=sources_string))
    permissions = (
        stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR | stat.S_IRGRP |
        stat.S_IWGRP | stat.S_IXGRP | stat.S_IROTH)
    os.chmod(test_file, permissions)


if __name__ == '__main__':
    sys.exit(main())
