#!/usr/bin/env python3.8
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Tests for rustc_remote_wrapper."""

import contextlib
import os
import parameterized
import subprocess
import unittest

from parameterized import parameterized
from unittest import mock

import rustc_remote_wrapper

import fuchsia
import remote_action

from typing import Iterable, Sequence


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
            ['a', 'd'])


class TestAccompanyRlibWithSo(unittest.TestCase):

    def test_not_rlib(self):
        deps = '../path/to/foo.rs'
        self.assertEqual(
            list(rustc_remote_wrapper.accompany_rlib_with_so([deps])), [deps])

    def test_rlib_without_so(self):
        deps = 'obj/build/foo.rlib'
        with mock.patch.object(os.path, 'isfile',
                               return_value=False) as mock_isfile:
            self.assertEqual(
                list(rustc_remote_wrapper.accompany_rlib_with_so([deps])),
                [deps])
        mock_isfile.assert_called_once()

    def test_rlib_with_so(self):
        deps = 'obj/build/foo.rlib'
        with mock.patch.object(os.path, 'isfile',
                               return_value=True) as mock_isfile:
            self.assertEqual(
                list(rustc_remote_wrapper.accompany_rlib_with_so([deps])),
                [deps, 'obj/build/foo.so'])
            mock_isfile.assert_called_once()


class RustRemoteActionPrepareTests(unittest.TestCase):

    def generate_prepare_mocks(
        self,
        depfile_contents: Sequence[str] = None,
        compiler_shlibs: Sequence[str] = None,
        clang_rt_libdir: str = None,
        libcxx_static: str = None,
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
        exec_root = '/home/project'
        working_dir = os.path.join(exec_root, 'build-here')
        compiler = '../tools/bin/rustc'
        shlib = 'tools/lib/librusteze.so'
        shlib_abs = os.path.join(exec_root, shlib)
        shlib_rel = os.path.relpath(shlib_abs, start=working_dir)
        source = '../foo/src/lib.rs'
        rlib = 'obj/foo.rlib'
        deps = ['../foo/src/other.rs']
        depfile_contents = [d + ':' for d in deps]
        command = [compiler, source, '-o', rlib]
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
        exec_root = '/home/project'
        working_dir = os.path.join(exec_root, 'build-here')
        compiler = '../tools/bin/rustc'
        shlib = 'tools/lib/librusteze.so'
        shlib_abs = os.path.join(exec_root, shlib)
        shlib_rel = os.path.relpath(shlib_abs, start=working_dir)
        source = '../foo/src/lib.rs'
        rlib = 'obj/foo.rlib'
        deps = ['../foo/src/other.rs']
        depfile_path = 'obj/foo.rlib.d'
        depfile_contents = [d + ':' for d in deps]
        command = [
            compiler, source, '-o', rlib, f'--emit=dep-info={depfile_path}'
        ]
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
        exec_root = '/home/project'
        working_dir = os.path.join(exec_root, 'build-here')
        compiler = '../tools/bin/rustc'
        shlib = 'tools/lib/librusteze.so'
        shlib_abs = os.path.join(exec_root, shlib)
        shlib_rel = os.path.relpath(shlib_abs, start=working_dir)
        source = '../foo/src/lib.rs'
        rlib = 'obj/foo.rlib'
        deps = ['../foo/src/other.rs']
        depfile_contents = [d + ':' for d in deps]
        externs = [
            'obj/path/to/this.rlib',
            'obj/path/to/that.rlib',
        ]
        extern_flags = [
            '--extern',
            f'this={externs[0]}',
            '--extern',
            f'that={externs[1]}',
        ]
        command = [compiler, source, '-o', rlib] + extern_flags
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
        exec_root = '/home/project'
        working_dir = os.path.join(exec_root, 'build-here')
        compiler = '../tools/bin/rustc'
        shlib = 'tools/lib/librusteze.so'
        shlib_abs = os.path.join(exec_root, shlib)
        shlib_rel = os.path.relpath(shlib_abs, start=working_dir)
        source = '../foo/src/lib.rs'
        rlib = 'obj/foo.rlib'
        deps = ['../foo/src/other.rs']
        depfile_contents = [d + ':' for d in deps]
        link_arg = 'obj/some/random.a'
        command = [compiler, source, '-o', rlib, f'-Clink-arg={link_arg}']
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
        exec_root = '/home/project'
        working_dir = os.path.join(exec_root, 'build-here')
        exec_root_rel = os.path.relpath(exec_root, start=working_dir)
        compiler = '../tools/bin/rustc'
        shlib = 'tools/lib/librusteze.so'
        shlib_abs = os.path.join(exec_root, shlib)
        shlib_rel = os.path.relpath(shlib_abs, start=working_dir)
        linker = '../tools/bin/linker'
        lld = '../tools/bin/ld.lld'
        clang_rt_libdir = '../fake/lib/clang/x86_64-unknown-linux'
        libcxx = 'fake/clang/libc++.a'
        source = '../foo/src/lib.rs'
        target = 'x86_64-unknown-linux-gnu'
        exe = 'obj/foo.exe'
        deps = ['../foo/src/other.rs']
        depfile_contents = [d + ':' for d in deps]
        command = [
            compiler, source, '-o', exe, '--crate-type', 'bin',
            f'-Clinker={linker}', f'--target={target}'
        ]
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
                [compiler, shlib_rel, source] + deps + [
                    linker, lld, clang_rt_libdir,
                    os.path.join(exec_root_rel, libcxx)
                ]))
        self.assertEqual(remote_output_files, {exe})


if __name__ == '__main__':
    unittest.main()
