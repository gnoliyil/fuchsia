#!/usr/bin/env fuchsia-vendored-python
# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import filecmp
import sys
import subprocess

# Verifies that two files have matching contents.


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--result-file', help='Path to the victory file', required=True)
    parser.add_argument(
        '--diff-on-failure',
        help='Run diff on the files if they differ',
        action="store_true")
    parser.add_argument(
        '--custom-error-message',
        help='Error to print if the files differ',
        default=None)
    parser.add_argument('first')
    parser.add_argument('second')
    args = parser.parse_args()

    if not filecmp.cmp(args.first, args.second):
        # Flush is used to make sure that the print statement occurs before the
        # output of diff.
        print(
            f'Error: file contents differ:\n  {args.first}\n  {args.second}',
            flush=True)
        if args.custom_error_message:
            print(args.custom_error_message, flush=True)
        if args.diff_on_failure:
            subprocess.call(
                ['diff', '-u', args.first, args.second], stderr=sys.stdout)
        return 1

    with open(args.result_file, 'w') as result_file:
        result_file.write('Match!\n')

    return 0


if __name__ == '__main__':
    sys.exit(main())
