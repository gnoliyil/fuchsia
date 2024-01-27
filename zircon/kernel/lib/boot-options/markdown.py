#!/usr/bin/env python3.8
# Copyright 2020 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

import argparse
import json
import os
import sys

# Some boot options have non-primitive types.
# This map is used to map the type of the boot-option
# to a string suitable for markdown documentation about the option.
SPECIAL_TYPES = {
    'SmallString': r'\<string>',
    'RedactedHex': r'\<hexadecimal>',
    'TestOption': 'test',
    'uart::all::Driver': r'\[none | legacy | qemu | \<type>,\<base>,\<irq>\]',
}


def generate_doc(option):
    # Types that are lists are enumerations of valid values for this option.
    # and are printed as [ <value1> | <value2> |...].
    if isinstance(option['type'], list):
        option['type'] = '\[%s\]' % ' | '.join(option['type'])
    else:
        # Non-list types could be a special type, or default to the
        # type passed in. This allows SPECIAL_TYPES to handle non-primitive
        # types and primitive types fall back to themselves as the default.
        option['type'] = SPECIAL_TYPES.get(
            option['type'], r'\<%s>' % option['type'])
    if option['default'] != '':
        if isinstance(option['default'], bool):
            option['default'] = 'true' if option['default'] else 'false'
        elif isinstance(option['default'], int):
            option['default'] = '%#x' % option['default']
        option['default'] = '**Default:** `%s`\n' % option['default']
    # Return the multiline string with the values interpolated.
    return '''
### {name}={type}

{default}
{documentation}
'''.format(**option)


def generate_docs(title, options):
    return '''
## Options {title}
{options}
'''.format(
        title=title,
        options=''.join(generate_doc(option) for option in options))


def main():
    parser = argparse.ArgumentParser(description='Produce boot-options.md')
    parser.add_argument(
        'output', help='Output file', metavar='//docs/gen/boot-options.md')
    parser.add_argument(
        'json', help='JSON input file', metavar='boot-options.json')
    parser.add_argument(
        'preamble', help='Markdown preamble file', metavar='preamble.md')
    parser.add_argument(
        'postamble', help='Markdown postamble file', metavar='postamble.md')
    args = parser.parse_args()

    with open(args.json) as f:
        options = json.load(f)

    with open(args.preamble) as f:
        preamble = f.read()

    with open(args.postamble) as f:
        postamble = f.read()

    text = preamble

    text += generate_docs('common to all machines', options['common'])
    del options['common']
    for arch, arch_options in sorted(options.items()):
        text += generate_docs(
            'available only on %s machines' % arch, arch_options)

    text += postamble

    if os.path.exists(args.output):
        with open(args.output) as f:
            if f.read() == text:
                return 0

    with open(args.output, 'w') as f:
        f.write(text)

    return 0


if __name__ == '__main__':
    sys.exit(main())
