#!/usr/bin/env fuchsia-vendored-python
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import sys
from pathlib import Path

FUCHSIA_DIR = Path(os.environ["FUCHSIA_DIR"])


def main(args):
    """Given a newline separate list of files as stdin (probably generated by
    using a tool like `find` or similar), check to see if we can add a
    `deprecated_syntax` token to them, and do so if one does not already exist.
    For example, when the pwd is root, and we are building to out/default in
    this repo, we would pipe the result of

      find ./out/default/. -name "*.fidl" -type f | xargs readlink -f

    into this script.
    """

    root_path = Path(args.root)
    for filepath in sys.stdin:
        path = Path(filepath.strip())
        if not path.is_file():
            continue
        relpath = str(path.relative_to(root_path))
        if relpath.startswith("out/default") or relpath.startswith("prebuilt/"):
            continue
        if relpath.endswith("goodformat.test.fidl"):
            continue

        with open(path, "r") as f:
            lines = f.readlines()
            # already converted from a previous run; skip
            if any(l.startswith("deprecated_syntax;") for l in lines):
                continue

            lib_line = None
            for i, line in enumerate(lines):
                is_comment = line.startswith("//") and not line.startswith(
                    "///"
                )
                if not is_comment:
                    lib_line = i
                    break
            if lib_line is None:
                print(path)
            lines.insert(lib_line, "deprecated_syntax;\n")

        with open(path, "w") as f:
            f.writelines(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Add `deprecated_syntax` token "
        "to all FIDL files in GN-ified a "
        "repo."
    )
    parser.add_argument(
        "root", help="The root path of the repo being converted"
    )
    main(parser.parse_args())
