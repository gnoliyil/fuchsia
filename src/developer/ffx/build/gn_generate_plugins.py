#!/usr/bin/env fuchsia-vendored-python
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
import argparse
import filecmp
import os
import shutil
import string
import sys
import tempfile

from jinja2 import Environment, FileSystemLoader


def to_camel_case(snake_str):
    components = snake_str.split('_')
    return ''.join(x.title() for x in components[0:])


def wrap_deps(dep):
    return {'enum': to_camel_case(dep), 'lib': dep}


def main(args_list=None):
    parser = argparse.ArgumentParser(description='Generate FFX Plugin matcher')

    parser.add_argument(
        '--out', help='The output file to generate', required=True)

    parser.add_argument(
        '--deps',
        help='Comma-seperated libraries to generate code from',
        required=False)

    parser.add_argument('--args', help='args lib', required=True)

    parser.add_argument('--sub_command', help='sub command lib', required=False)

    parser.add_argument(
        '--includes_execution', help='includes execution code', default=False)

    parser.add_argument(
        '--includes_subcommands', help='includes subcommands', default=False)

    parser.add_argument(
        '--execution_lib', help='name of execution lib', required=True)

    parser.add_argument(
        '--template',
        help='The template file to use to generate code',
        required=True)

    if args_list:
        args = parser.parse_args(args_list)
    else:
        args = parser.parse_args()

    template_dir, template_name = os.path.split(args.template)
    env = Environment(
        loader=FileSystemLoader(template_dir),
        trim_blocks=True,
        lstrip_blocks=True)
    template = env.get_template(template_name)
    if args.deps:
        libraries = args.deps.split(',')
        plugins = list(map(wrap_deps, libraries))
    else:
        libraries = ""
        plugins = []
    temp_file = tempfile.NamedTemporaryFile(mode='w')
    with open(temp_file.name, 'w') as file:
        file.write(
            template.render(
                plugins=plugins,
                suite_subcommand_lib=args.sub_command,
                suite_args_lib=args.args,
                includes_subcommands=args.includes_subcommands,
                includes_execution=args.includes_execution,
                execution_lib=args.execution_lib))
        file.flush()
        if (not os.path.isfile(args.out) or
                not filecmp.cmp(temp_file.name, args.out, shallow=False)):
            shutil.copyfile(temp_file.name, args.out)


if __name__ == '__main__':
    sys.exit(main())
