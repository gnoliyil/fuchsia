# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
from dataclasses import dataclass
import typing

import termout
import util.arg_option as arg_option


@dataclass
class Flags:
    """Command line flags for fx test.

    See `parse_args` for documentation.
    """

    dry: bool
    info: bool

    build: bool
    updateifinbase: bool

    selection: typing.List[str]

    random: bool

    output: bool
    simple: bool
    style: bool
    log: bool
    logpath: typing.Optional[str]
    status: typing.Optional[bool]
    verbose: bool

    def validate(self):
        """Validate incoming flags, raising an exception on failure.

        Raises:
            FlagError: If the flags are invalid.
        """
        if self.simple and self.status:
            raise FlagError("--simple is incompatible with --status")
        if self.simple and self.style:
            raise FlagError("--simple is incompatible with --style")

        if not termout.is_valid() and self.status:
            raise FlagError("Refusing to output interactive status to a non-TTY.")

        if self.simple:
            self.style = False
            self.status = False
        else:
            if self.style is None:
                self.style = termout.is_valid()
            if self.status is None:
                self.status = termout.is_valid()


class FlagError(Exception):
    """Raised if there was a problem parsing command line flags."""


def parse_args(cli_args: typing.Optional[typing.List[str]] = None) -> Flags:
    """Parse command line flags.

    Returns:
        Flags: Typed representation of the command line for this program.
    """
    parser = argparse.ArgumentParser(
        "fx test",
        description="Test Executor for Humans",
    )
    utility = parser.add_argument_group("Utility Options")
    utility.add_argument(
        "--dry",
        action="store_true",
        help="Do not actually run tests.",
    )
    utility.add_argument(
        "--info",
        action="store_true",
        help="Print the test specs in key:value format, and exit",
    )

    build = parser.add_argument_group("Build Options")
    build.add_argument(
        "--build",
        action=arg_option.BooleanOptionalAction,
        help="Invoke `fx build` before running the test suite (defaults to on)",
        default=True,
    )
    build.add_argument(
        "--updateifinbase",
        action=arg_option.BooleanOptionalAction,
        help="Invoke `fx update-if-in-base` before running device tests (defaults to on)",
        default=True,
    )

    selection = parser.add_argument_group("Test Selection Options")
    selection.add_argument(
        "-p",
        "--package",
        action=arg_option.SelectionAction,
        dest="selection",
        nargs="*",
        help="Match tests against their Fuchsia package name",
    )
    selection.add_argument(
        "-c",
        "--component",
        action=arg_option.SelectionAction,
        dest="selection",
        nargs="*",
        help="Match tests against their Fuchsia component name",
    )
    selection.add_argument(
        "-a",
        "--and",
        action=arg_option.SelectionAction,
        dest="selection",
        nargs="*",
        help="Add requirements to the preceding filter",
    )
    selection.add_argument("selection", action=arg_option.SelectionAction, nargs="*")

    execution = parser.add_argument_group("Execution Options")
    execution.add_argument(
        "-r",
        "--random",
        action="store_true",
        help="Randomize test execution order",
        default=False,
    )

    output = parser.add_argument_group("Output Options")
    output.add_argument(
        "-o",
        "--output",
        action="store_true",
        help="Display the output from passing tests. Some test arguments may be needed.",
    )
    output.add_argument(
        "--simple",
        action="store_true",
        help="Remove any color or decoration from output. Disable pretty status printing. Implies --no-style",
    )
    output.add_argument(
        "--style",
        action=arg_option.BooleanOptionalAction,
        default=None,
        help="Remove color and decoration from output. Does not disable pretty status printing. Default is to only style for TTY output.",
    )
    output.add_argument(
        "--log",
        action=arg_option.BooleanOptionalAction,
        help="Emit command events to a file. Turned on when running real tests unless `--no-log` is passed.",
        default=True,
    )
    output.add_argument(
        "--logpath",
        help="If passed and --log is enabled, custimizes the destination of the target log.",
        default=None,
    )
    output.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        default=False,
        help="Print verbose logs to the console",
    )
    output.add_argument(
        "--status",
        action=arg_option.BooleanOptionalAction,
        default=None,
        help="Toggle interactive status printing to console. Default is to vary behavior depending on if output is to a TTY. Setting to True on a non-TTY is an error.",
    )

    flags: Flags = Flags(**vars(parser.parse_args(cli_args)))
    return flags
