#!/usr/bin/env python
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import re
import sys


def main():
    parser = argparse.ArgumentParser(
        sys.argv[0], description="Generate main file for Fuchsia dart test")
    parser.add_argument(
        "--out", help="Path to .dart file to generate", required=True)
    parser.add_argument(
        '--source',
        help='Test source file',
        required=True,
        default=[],
        action='append')
    parser.add_argument(
        "--package",
        help="Name of the package containing the sources",
        required=True)
    args = parser.parse_args()

    test_files = args.source

    outfile = open(args.out, 'w')
    outfile.write(
        '''// Generated by %s

    // ignore_for_file: directives_ordering
    // ignore_for_file: avoid_relative_lib_imports

    import 'dart:async';
    import 'package:fuchsia_test_helper/fuchsia_test_helper.dart';
    ''' % os.path.basename(__file__))

    for i, name in enumerate(test_files):
        outfile.write(
            "import 'package:%s/%s' as test_%d;\n" % (args.package, name, i))

    outfile.write(
        '''
    Future<int> main(List<String> args) async {
      final int exitCode = await runFuchsiaTests(<MainFunction>[
    ''')

    for i in range(len(test_files)):
        outfile.write('test_%d.main,\n' % i)

    outfile.write(
        '''], args);

      exitFuchsiaTest(exitCode);
      return exitCode;
    }
    ''')

    outfile.close()


if __name__ == '__main__':
    main()
