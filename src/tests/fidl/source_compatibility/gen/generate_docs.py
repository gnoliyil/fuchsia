# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
""" Tool for generating markdown documentation for a test. """

import datetime
import difflib
from operator import itemgetter
import os
from pathlib import Path
from typing import IO, List, Tuple
import yaml

from types_ import CompatTest, FidlStep, SourceStep, HLCPP, LLCPP, RUST, GO

# Lines of context to show in diffs. We can be very liberal with this number
# since we already filter out the boilerplate before passing code to difflib.
DIFF_CONTEXT = 12

# Languages to specify in markdown for syntax highlighting
MD_LANG = {
    'fidl': 'fidl',
    HLCPP: 'cpp',
    LLCPP: 'cpp',
    RUST: 'rust',
    GO: 'go',
}

DOC_DIR = 'docs/development/languages/fidl/guides/compatibility'

COMPAT_GUIDE_TOC_ENTRY = {
    'title': 'Overview',
    'path': '/docs/development/languages/fidl/guides/compatibility/README.md'
}

TOC_HEADER = '''
# Copyright {year} The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# WARNING: This file is machine generated by //src/tests/fidl/source_compatibility/gen, do not edit.

'''.format(year=datetime.datetime.now().year).lstrip()

DOC_HEADER = '''<!-- WARNING: This file is machine generated by //src/tests/fidl/source_compatibility/gen, do not edit. -->

Note: This document covers API impact only. For more details, see the
[ABI compatibility page]({path})

'''.format(path=COMPAT_GUIDE_TOC_ENTRY['path'])


def binding_title(binding: str) -> str:
    """ Pretty prints a binding string for use as a markdown header. """
    return binding.upper() if binding in [HLCPP, LLCPP
                                         ] else binding.capitalize()


def write_instructions(out: IO, ins: List[str]):
    for i in ins:
        out.write(f'- {i}\n')
    out.write('\n')


def remove_boilerplate(lines: List[str]) -> List[str]:
    """ Remove boilerplate lines. """
    filtered = []
    within_contents = False
    for line in lines:
        if '[START contents]' in line:
            within_contents = True
        elif '[END contents]' in line:
            return filtered
        elif within_contents:
            filtered.append(line)
    if not filtered:
        raise RuntimeError('Did not find [START contents] tag in test file')
    raise RuntimeError('Did not find [END contents] tag in test file')


def cat(out: IO, binding: str, path: Path):
    """ Render contents of file at path to out. """
    with open(path) as source:
        lines = remove_boilerplate(source.readlines())
        out.write(f'```{MD_LANG[binding]}\n{"".join(lines)}```\n\n')


def diff(out: IO, pre: Path, post: Path):
    '''
    Render a diff of pre and post to out.
    '''
    pre_lines = remove_boilerplate(open(pre).readlines())
    post_lines = remove_boilerplate(open(post).readlines())

    matcher = difflib.SequenceMatcher(
        None, pre_lines, post_lines, autojunk=False)
    for opcodes in matcher.get_grouped_opcodes(DIFF_CONTEXT):
        out.write('```diff\n')
        for tag, pre_start, pre_end, post_start, post_end in opcodes:
            if tag == 'equal':
                for line in pre_lines[pre_start:pre_end]:
                    out.write('  ' + line)
                continue
            if tag in {'replace', 'delete'}:
                for line in pre_lines[pre_start:pre_end]:
                    out.write('- ' + line)
            if tag in {'replace', 'insert'}:
                for line in post_lines[post_start:post_end]:
                    out.write('+ ' + line)
        out.write('\n```\n\n')


def generate_docs(test_root: Path, test: CompatTest, out: IO) -> str:
    """ Generate transition documentation. """
    out.write(DOC_HEADER)
    # Title
    out.write(f'# {test.title}\n\n')

    # Overview
    out.write('## Overview\n\n')
    step_nums = sorted(
        {s.step_num for t in test.bindings.values() for s in t.steps})
    num_steps = step_nums[-1] if step_nums else 0
    step_cols = [f'|[step {i}](#step-{i})' for i in step_nums]
    out.write('-|[init](#init)' + ''.join(step_cols) + '\n')
    out.write('|'.join(['---'] * (2 + num_steps)) + '\n')

    fidl_step_nums = {
        s.step_num
        for t in test.bindings.values()
        for s in t.steps
        if isinstance(s, FidlStep)
    }
    out.write('fidl|[link](#fidl-init)')
    for i in step_nums:
        out.write('|')
        if i in fidl_step_nums:
            out.write(f'[link](#fidl-{i})')
    out.write('\n')

    bindings = sorted(test.bindings.items())
    for b, transition in bindings:
        out.write(f'{b}|[link](#{b}-init)')
        src_step_nums = {
            s.step_num for s in transition.steps if isinstance(s, SourceStep)
        }
        for i in step_nums:
            out.write('|')
            if i in src_step_nums:
                out.write(f'[link](#{b}-{i})')
        out.write('\n')
    out.write('\n')

    # Initial FIDL
    out.write('## Initial State {#init}\n\n')
    out.write(f'### FIDL {{#fidl-init}}\n\n')
    starting_fidl = next(iter(test.bindings.values())).starting_fidl
    cat(out, 'fidl', test_root / test.fidl[starting_fidl].source)

    # Initial bindings
    prev_fidl = test.fidl[starting_fidl].source
    prev_srcs = {}
    for b, t in bindings:
        out.write(f'### {binding_title(b)} {{#{b}-init}}\n\n')
        cat(out, b, test_root / t.starting_src)
        prev_srcs[b] = t.starting_src

    # Transition steps
    remaining_steps = {b: list(t.steps) for b, t in bindings}
    current_step = 1
    while any(remaining_steps.values()):
        is_first_write = True
        remaining_steps = {k: v for k, v in remaining_steps.items() if v}
        for b in remaining_steps:
            step = remaining_steps[b][0]
            if step.step_num != current_step:
                continue
            remaining_steps[b].pop(0)
            # FIDL step
            if isinstance(step, FidlStep) and is_first_write:
                out.write(
                    f'## Update FIDL Library {{#step-{step.step_num}}}\n\n')
                write_instructions(out, test.fidl[step.fidl].instructions)

                source = test.fidl[step.fidl].source
                diff(out, test_root / prev_fidl, test_root / source)
                prev_fidl = source
                is_first_write = False
            # Binding step
            elif isinstance(step, SourceStep):
                if is_first_write:
                    out.write(
                        f'## Update Source Code {{#step-{step.step_num}}}\n\n')
                    is_first_write = False
                out.write(
                    f'### {binding_title(b)} {{#{b}-{step.step_num}}}\n\n')
                write_instructions(out, step.instructions)
                diff(out, test_root / prev_srcs[b], test_root / step.source)
                prev_srcs[b] = step.source
        current_step += 1


def write_docs(test_root: Path, test: CompatTest):
    doc_filename = test_root.name.replace('-', '_') + '.md'
    doc_path = Path(
        os.environ['FUCHSIA_DIR']) / DOC_DIR / to_filename(test_root)
    with open(doc_path, 'w') as f:
        generate_docs(test_root, test, f)

    with open(test_root / 'README.md', 'w') as f:
        f.write(f'See //{DOC_DIR}/{doc_filename}')


def regen_toc(all_tests: List[Tuple[Path, CompatTest]]):
    lines = [to_yaml(**COMPAT_GUIDE_TOC_ENTRY)]
    all_tests = sorted(all_tests, key=itemgetter(0))
    for test_root, test in all_tests:
        path = f'/{DOC_DIR}/{to_filename(test_root)}'
        lines.append(to_yaml(test.title, path))
        # print debug statements in markdown xref format, for easy copy/pasting
        print(f'[example-{test_root.name}]: {path}')

    toc = TOC_HEADER + 'toc:\n' + '\n'.join(lines) + '\n'
    toc_path = Path(os.environ['FUCHSIA_DIR']) / DOC_DIR / '_toc.yaml'
    with open(toc_path, 'w') as f:
        f.write(toc)


def to_filename(test_root: Path) -> str:
    return test_root.name.replace('-', '_') + '.md'


def to_yaml(title: str, path: str) -> str:
    return f'- title: {title}\n  path: {path}'
