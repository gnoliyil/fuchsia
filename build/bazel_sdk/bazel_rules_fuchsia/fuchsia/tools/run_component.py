#!/usr/bin/env python3
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import subprocess

from fuchsia_task_lib import *


class FuchsiaTaskRunComponent(FuchsiaTask):
    def parse_args(self, parser: ScopedArgumentParser) -> argparse.Namespace:
        '''Parses arguments.'''

        parser.add_argument(
            '--ffx',
            type=parser.path_arg(),
            help='A path to the ffx tool.',
            required=True,
        )
        parser.add_argument(
            '--moniker',
            type=str,
            help='The moniker to add the component to.',
            required=True,
        )
        parser.add_argument(
            '--url',
            type=str,
            help='The full component url.',
            required=True,
        )
        parser.add_argument(
            '--session',
            action='store_true',
            help='Whether to add this component to the session.',
            required=False,
        )
        parser.add_argument(
            '--target',
            help='Optionally specify the target fuchsia device.',
            required=False,
            scope=ArgumentScope.GLOBAL,
        )
        return parser.parse_args()


    def run(self, parser: ScopedArgumentParser) -> None:
        args = self.parse_args(parser)
        ffx = [args.ffx] + (['--target', args.target] if args.target else [])

        if args.session:
            subprocess.check_call([
                *ffx,
                'session',
                'add',
                args.url,
            ])
        else:
            subprocess.check_call([
                *ffx,
                'component',
                'run',
                args.moniker,
                args.url,
                '--recreate',
            ])


if __name__ == '__main__':
    FuchsiaTaskRunComponent.main()
