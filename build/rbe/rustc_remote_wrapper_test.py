#!/usr/bin/env python3.8
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Tests for rustc_remote_wrapper."""

import contextlib
import io
import os
import pathlib
import subprocess
import sys
import unittest

from pathlib import Path
from unittest import mock

import rustc_remote_wrapper
from rustc_remote_wrapper import _relpath

import fuchsia
import remote_action

from typing import Any, Iterable, Sequence


def _strs(items: Sequence[Any]) -> Sequence[str]:
    return [str(i) for i in items]


def _paths(items: Sequence[Any]) -> Sequence[Path]:
    if isinstance(items, list):
        return [Path(i) for i in items]
    elif isinstance(items, set):
        return {Path(i) for i in items}
    elif isinstance(items, tuple):
        return tuple(Path(i) for i in items)

    t = type(items)
    raise TypeError("Unhandled sequence type: {t}")


class DepfileInputsByLineTests(unittest.TestCase):

    def test_no_phony_deps(self):
        lines = [
            'a: b c',
            'd: e f g',
        ]
        self.assertEqual(
            list(rustc_remote_wrapper.depfile_inputs_by_line(lines)), [])

    def test_phony_deps(self):
        lines = [
            'a:',
            'b: c',
            'd:',
            'z: e f g',
        ]
        self.assertEqual(
            list(rustc_remote_wrapper.depfile_inputs_by_line(lines)),
            _paths(['a', 'd']))


class TestAccompanyRlibWithSo(unittest.TestCase):

    def test_not_rlib(self):
        deps = Path('../path/to/foo.rs')
        self.assertEqual(
            list(rustc_remote_wrapper.accompany_rlib_with_so([deps])), [deps])

    def test_rlib_without_so(self):
        deps = Path('obj/build/foo.rlib')
        with mock.patch.object(Path, 'is_file',
                               return_value=False) as mock_isfile:
            self.assertEqual(
                list(rustc_remote_wrapper.accompany_rlib_with_so([deps])),
                [deps])
        mock_isfile.assert_called_once()

    def test_rlib_with_so(self):
        deps = Path('obj/build/foo.rlib')
        with mock.patch.object(Path, 'is_file',
                               return_value=True) as mock_isfile:
            self.assertEqual(
                list(rustc_remote_wrapper.accompany_rlib_with_so([deps])),
                [deps, Path('obj/build/foo.so')])
            mock_isfile.assert_called_once()


class RustRemoteActionPrepareTests(unittest.TestCase):

    def generate_prepare_mocks(
        self,
        depfile_contents: Sequence[str] = None,
        compiler_shlibs: Sequence[Path] = None,
        clang_rt_libdir: Path = None,
        libcxx_static: Path = None,
    ) -> Iterable[mock.patch.object]:
        """common mocks needed for RustRemoteAction.prepare()"""
        # depfile only command
        yield mock.patch.object(
            rustc_remote_wrapper, '_make_local_depfile', return_value=0)

        if depfile_contents:
            yield mock.patch.object(
                rustc_remote_wrapper,
                '_readlines_from_file',
                return_value=depfile_contents)

        # check compiler is executable
        yield mock.patch.object(
            rustc_remote_wrapper, '_tool_is_executable', return_value=True)

        if compiler_shlibs:
            yield mock.patch.object(
                rustc_remote_wrapper.RustRemoteAction,
                '_remote_rustc_shlibs',
                return_value=iter(compiler_shlibs))

        yield mock.patch.object(
            rustc_remote_wrapper, '_libcxx_isfile', return_value=True)

        if clang_rt_libdir:
            yield mock.patch.object(
                fuchsia,
                'remote_clang_runtime_libdirs',
                return_value=[clang_rt_libdir])

        if libcxx_static:
            yield mock.patch.object(
                fuchsia,
                'remote_clang_libcxx_static',
                return_value=libcxx_static or '')

    def test_prepare_basic(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        command = _strs([compiler, source, '-o', rlib])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs, set([compiler, shlib_rel, source] + deps))
        self.assertEqual(remote_output_files, {rlib})

    def test_prepare_depfile(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_path = Path('obj/foo.rlib.d')
        depfile_contents = [str(d) + ':' for d in deps]
        command = _strs(
            [compiler, source, '-o', rlib, f'--emit=dep-info={depfile_path}'])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs, set([compiler, shlib_rel, source] + deps))
        self.assertEqual(remote_output_files, {rlib, depfile_path})

    def test_prepare_externs(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        externs = _paths([
            'obj/path/to/this.rlib',
            'obj/path/to/that.rlib',
        ])
        extern_flags = [
            '--extern',
            f'this={externs[0]}',
            '--extern',
            f'that={externs[1]}',
        ]
        command = _strs([compiler, source, '-o', rlib]) + extern_flags
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs, set([compiler, shlib_rel, source] + deps + externs))
        self.assertEqual(remote_output_files, {rlib})

    def test_prepare_link_arg_files(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        link_arg = Path('obj/some/random.a')
        command = _strs(
            [compiler, source, '-o', rlib, f'-Clink-arg={link_arg}'])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs,
            set([compiler, shlib_rel, source] + deps + [link_arg]))
        self.assertEqual(remote_output_files, {rlib})

    def test_prepare_needs_linker(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        exec_root_rel = _relpath(exec_root, start=working_dir)
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        linker = Path('../tools/bin/linker')
        lld = Path('../tools/bin/ld.lld')
        clang_rt_libdir = Path('../fake/lib/clang/x86_64-unknown-linux')
        libcxx = Path('fake/clang/libc++.a')
        source = Path('../foo/src/lib.rs')
        target = 'x86_64-unknown-linux-gnu'
        exe = Path('obj/foo.exe')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        command = _strs(
            [
                compiler, source, '-o', exe, '--crate-type', 'bin',
                f'-Clinker={linker}', f'--target={target}'
            ])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
            clang_rt_libdir=clang_rt_libdir,
            libcxx_static=libcxx,
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs,
            set(
                _paths(
                    [compiler, shlib_rel, source] + deps +
                    [linker, lld, clang_rt_libdir, exec_root_rel / libcxx])))
        self.assertEqual(remote_output_files, {exe})

    def test_prepare_remote_option_forwarding(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        remote_flag = '--some_forwarded_rewrapper_flag=value'  # not real
        command = _strs(
            [compiler, source, '-o', rlib, f'--remote-flag={remote_flag}'])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs, set([compiler, shlib_rel, source] + deps))
        self.assertEqual(remote_output_files, {rlib})
        # Position matters, not just presence.
        # Could override the set of standard options.
        self.assertEqual(r.remote_options[-1], remote_flag)

    def test_prepare_environment_names_input_file(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        env_file = Path('../path/to/config.json')
        command = _strs(
            [
                f'CONFIGURE_THINGY={env_file}', compiler, source, '-o', rlib,
                f'--remote-inputs={env_file}'
            ])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            # We are not scanning the environment variables automatically.
            # Users should follow the above example command and
            # explicitly pass --remote-inputs.
            with mock.patch.object(rustc_remote_wrapper, '_env_file_exists',
                                   return_value=True) as mock_exists:
                prepare_status = r.prepare()
            mock_exists.assert_not_called()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs,
            set(_paths([compiler, shlib_rel, source, env_file] + deps)))
        self.assertEqual(remote_output_files, {rlib})

    def test_prepare_remote_cross_compile(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/mac-arm64/bin/rustc')
        remote_compiler = Path('../tools/linux-x64/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_contents = [str(d) + ':' for d in deps]
        command = _strs([compiler, source, '-o', rlib])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        self.assertEqual(r.remote_compiler, remote_compiler)
        a = r.remote_action
        self.assertEqual(a.local_command, _strs([remote_compiler, source, '-o', rlib]))
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs, set([remote_compiler, shlib_rel, source] + deps))
        self.assertEqual(remote_output_files, {rlib})


    def test_post_run_actions(self):
        exec_root = Path('/home/project')
        working_dir = exec_root / 'build-here'
        compiler = Path('../tools/bin/rustc')
        shlib = Path('tools/lib/librusteze.so')
        shlib_abs = exec_root / shlib
        shlib_rel = _relpath(shlib_abs, start=working_dir)
        source = Path('../foo/src/lib.rs')
        rlib = Path('obj/foo.rlib')
        deps = [Path('../foo/src/other.rs')]
        depfile_path = Path('obj/foo.rlib.d')
        depfile_contents = [str(d) + ':' for d in deps]
        command = _strs(
            [compiler, source, '-o', rlib, f'--emit=dep-info={depfile_path}'])
        r = rustc_remote_wrapper.RustRemoteAction(
            ['--'] + command,
            exec_root=exec_root,
            working_dir=working_dir,
        )

        prepare_mocks = self.generate_prepare_mocks(
            depfile_contents=depfile_contents,
            compiler_shlibs=[shlib_rel],
        )
        with contextlib.ExitStack() as stack:
            for m in prepare_mocks:
                stack.enter_context(m)
            prepare_status = r.prepare()

        self.assertEqual(prepare_status, 0)  # success
        a = r.remote_action
        remote_inputs = set(a.inputs_relative_to_working_dir)
        remote_output_files = set(a.output_files_relative_to_working_dir)
        self.assertEqual(
            remote_inputs, set([compiler, shlib_rel, source] + deps))
        self.assertEqual(remote_output_files, {rlib, depfile_path})

        run_mocks = [
            mock.patch.object(
                remote_action.RemoteAction,
                'run_with_main_args',
                return_value=0),
            mock.patch.object(
                rustc_remote_wrapper.RustRemoteAction,
                '_depfile_exists',
                return_value=True),
            mock.patch.object(
                rustc_remote_wrapper.RustRemoteAction, '_cleanup'),
        ]
        with contextlib.ExitStack() as stack:
            for m in run_mocks:
                stack.enter_context(m)
            with mock.patch.object(rustc_remote_wrapper.RustRemoteAction,
                                   '_rewrite_depfile') as mock_rewrite:
                run_status = r.run()
            mock_rewrite.assert_called_with()


class MainTests(unittest.TestCase):

    def test_help_implicit(self):
        # Just make sure help exits successfully, without any exceptions
        # due to argument parsing.
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            with mock.patch.object(sys, 'exit') as mock_exit:
                # Normally, the following would not be reached due to exit(),
                # but for testing it needs to be mocked out.
                with mock.patch.object(rustc_remote_wrapper.RustRemoteAction,
                                       'run', return_value=0):
                    self.assertEqual(rustc_remote_wrapper.main([]), 0)
            mock_exit.assert_called_with(0)

    def test_help_flag(self):
        # Just make sure help exits successfully, without any exceptions
        # due to argument parsing.
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            with mock.patch.object(sys, 'exit') as mock_exit:
                # Normally, the following would not be reached due to exit(),
                # but for testing it needs to be mocked out.
                with mock.patch.object(rustc_remote_wrapper.RustRemoteAction,
                                       'run', return_value=0):
                    self.assertEqual(rustc_remote_wrapper.main(['--help']), 0)
            mock_exit.assert_called_with(0)


if __name__ == '__main__':
    unittest.main()
