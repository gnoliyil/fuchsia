#!/usr/bin/env fuchsia-vendored-python
# Copyright (c) 2018 Google Inc.

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import re
import sys

HEADER = """#include <string>

namespace media::audio {

// This is a char[] type because that requires no code to run
// initialization, so other constants can be initialized with
// this value.
//
// It is constexpr so that it can be used in the initialization
// of other constexprs. 
static constexpr char %s[] =
"""

FOOTER = """
}  // namespace media::audio
"""


def main():
    if len(sys.argv) != 3:
        print("Usage: %s <input_file> <output_file>" % (sys.argv[0],))
        exit(-1)

    lines = open(sys.argv[1], 'r').readlines()
    out = open(sys.argv[2], 'w')
    varname = re.sub(
        '_([a-zA-Z0-9])', lambda m: m.group(1).upper(),
        'k_' + os.path.splitext(os.path.split(sys.argv[1])[1])[0])

    out.write(HEADER % (varname,))

    for i in range(len(lines)):
        l = lines[i].replace('\n', '')  # Remove the trailing newline
        l = re.sub('//.*', '', l)  # Remove any comments
        l = re.sub('(^\s+|\s+$)', '', l)  # Remove leading/trailing whitespace
        l = l.replace('\\', '\\\\')  # Escape all fwd slash
        l = l.replace('"', '\\"')  # Escape all double-quotes

        # Skip empty lines
        if len(l) == 0:
            continue

        out.write('    "%s"' % (l,))
        if ((i + 1) == len(lines)):
            out.write(';\n')
        else:
            out.write('\n')

    out.write(FOOTER)
    out.close()


if __name__ == '__main__':
    main()
