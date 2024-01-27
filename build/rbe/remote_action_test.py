#!/usr/bin/env python3.8
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import fuchsia
import remote_action
import cl_utils

from typing import Any, Sequence


def _write_file_contents(path: Path, contents: str):
    with open(path, 'w') as f:
        f.write(contents)


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


class FileMatchTests(unittest.TestCase):

    def test_match(self):
        with tempfile.TemporaryDirectory() as td:
            f1path = Path(td, 'left.txt')
            f2path = Path(td, 'right.txt')
            _write_file_contents(f1path, 'a\n')
            _write_file_contents(f2path, 'a\n')
            self.assertTrue(remote_action._files_match(f1path, f2path))
            self.assertTrue(remote_action._files_match(f2path, f1path))

    def test_not_match(self):
        with tempfile.TemporaryDirectory() as td:
            f1path = Path(td, 'left.txt')
            f2path = Path(td, 'right.txt')
            _write_file_contents(f1path, 'a\n')
            _write_file_contents(f2path, 'b\n')
            self.assertFalse(remote_action._files_match(f1path, f2path))
            self.assertFalse(remote_action._files_match(f2path, f1path))


class DetailDiffTests(unittest.TestCase):

    def test_called(self):
        with mock.patch.object(subprocess, 'call', return_value=0) as mock_call:
            self.assertEqual(
                remote_action._detail_diff(
                    Path('file1.txt'), Path('file2.txt')), 0)
        mock_call.assert_called_once()
        first_call = mock_call.call_args_list[0]
        args, unused_kwargs = first_call
        command = args[0]  # list
        self.assertTrue(
            command[0].endswith(str(remote_action._DETAIL_DIFF_SCRIPT)))


class TextDiffTests(unittest.TestCase):

    def test_called(self):
        with mock.patch.object(subprocess, 'call', return_value=0) as mock_call:
            self.assertEqual(
                remote_action._text_diff('file1.txt', 'file2.txt'), 0)
        mock_call.assert_called_once()
        first_call = mock_call.call_args_list[0]
        args, unused_kwargs = first_call
        command = args[0]  # list
        self.assertEqual(command[0], 'diff')
        self.assertEqual(command[-2:], ['file1.txt', 'file2.txt'])


class FilesUnderDirTests(unittest.TestCase):

    def test_walk(self):
        with tempfile.TemporaryDirectory() as td:
            f1path = Path(td) / 'left.txt'
            subdir = Path(td) / 'sub'
            os.mkdir(subdir)
            f2path = subdir / 'right.txt'
            _write_file_contents(f1path, '\n')
            _write_file_contents(f2path, '\n')
            self.assertEqual(
                set(remote_action._files_under_dir(td)),
                _paths({'left.txt', 'sub/right.txt'}))


class CommonFilesUnderDirsTests(unittest.TestCase):

    def test_none_in_common(self):
        with mock.patch.object(remote_action, '_files_under_dir',
                               side_effect=[iter(['a', 'b', 'c']),
                                            iter(['d', 'e', 'f'])]) as mock_lsr:
            self.assertEqual(
                remote_action._common_files_under_dirs('foo-dir', 'bar-dir'),
                set())

    def test_some_in_common(self):
        with mock.patch.object(remote_action, '_files_under_dir',
                               side_effect=[iter(_paths(['a', 'b/x', 'c'])),
                                            iter(_paths(['d', 'c',
                                                         'b/x']))]) as mock_lsr:
            self.assertEqual(
                remote_action._common_files_under_dirs(
                    Path('foo-dir'), Path('bar-dir')), _paths({'b/x', 'c'}))


class ExpandCommonFilesBetweenDirs(unittest.TestCase):

    def test_common(self):
        # Normally returns a set, but mock-return a list for deterministic
        # ordering.
        with mock.patch.object(remote_action, '_common_files_under_dirs',
                               return_value=_paths(['y/z', 'x'])) as mock_ls:
            self.assertEqual(
                list(
                    remote_action._expand_common_files_between_dirs(
                        [_paths(('c', 'd')),
                         _paths(('a', 'b'))])), [
                             _paths(('c/x', 'd/x')),
                             _paths(('c/y/z', 'd/y/z')),
                             _paths(('a/x', 'b/x')),
                             _paths(('a/y/z', 'b/y/z')),
                         ])


class FileLinesMatchingTests(unittest.TestCase):

    def test_empty(self):
        with mock.patch('builtins.open',
                        mock.mock_open(read_data='')) as mock_file:
            self.assertEqual(
                list(
                    remote_action._file_lines_matching(
                        'log.txt', 'never-match')),
                [],
            )

    def test_matches(self):
        with mock.patch('builtins.open',
                        mock.mock_open(read_data='ab\nbc\ncd\n')) as mock_file:
            self.assertEqual(
                list(remote_action._file_lines_matching('file.txt', 'c')),
                ['bc\n', 'cd\n'],
            )


class ReclientCanonicalWorkingDirTests(unittest.TestCase):

    def test_empty(self):
        self.assertEqual(
            remote_action.reclient_canonical_working_dir(Path('')), Path(''))

    def test_one_level(self):
        self.assertEqual(
            remote_action.reclient_canonical_working_dir(Path('build-here')),
            Path('set_by_reclient'))

    def test_two_levels(self):
        self.assertEqual(
            remote_action.reclient_canonical_working_dir(Path('build/there')),
            Path('set_by_reclient/a'))

    def test_three_levels(self):
        self.assertEqual(
            remote_action.reclient_canonical_working_dir(
                Path('build/inside/there')), Path('set_by_reclient/a/a'))


class RemoveWorkingDirAbspathsTests(unittest.TestCase):

    def test_blank_no_change(self):
        for t in ('', '\n'):
            self.assertEqual(
                remote_action.remove_working_dir_abspaths(t, Path('build-out')),
                t)

    def test_phony_dep_with_local_build_dir(self):
        self.assertEqual(
            remote_action.remove_working_dir_abspaths(
                f'{remote_action._REMOTE_PROJECT_ROOT}/out/foo/path/to/thing.txt:\n',
                Path('out/foo')),
            'path/to/thing.txt:\n',
        )

    def test_phony_dep_with_canonical_build_dir(self):
        self.assertEqual(
            remote_action.remove_working_dir_abspaths(
                f'{remote_action._REMOTE_PROJECT_ROOT}/set_by_reclient/a/path/to/somethingelse.txt:\n',
                Path('out/foo')),
            'path/to/somethingelse.txt:\n',
        )

    def test_typical_dep_line_multiple_occurrences(self):
        root = remote_action._REMOTE_PROJECT_ROOT
        subdir = Path('out/inside/here')
        wd = root / subdir
        self.assertEqual(
            remote_action.remove_working_dir_abspaths(
                f'obj/foo.o: {wd}/foo/bar.h {wd}/baz/quux.h\n', subdir),
            'obj/foo.o: foo/bar.h baz/quux.h\n',
        )


class ResolvedShlibsFromLddTests(unittest.TestCase):

    def test_sample(self):
        ldd_output = """
	linux-vdso.so.1 (0x00007ffd653b2000)
	librustc_driver-897e90da9cc472c4.so => /usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/librustc_driver-897e90da9cc472c4.so (0x00007f6fdf600000)
	libstd-374958b5d3497a8f.so => /usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/libstd-374958b5d3497a8f.so (0x00007f6fdf45c000)
	libdl.so.2 => /lib/x86_64-linux-gnu/libdl.so.2 (0x00007f6fe2cc6000)
	librt.so.1 => /lib/x86_64-linux-gnu/librt.so.1 (0x00007f6fe2cc1000)
	libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0 (0x00007f6fe2cba000)
	libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f6fdf27b000)
	libLLVM-15-rust-1.70.0-nightly.so => /usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/../lib/libLLVM-15-rust-1.70.0-nightly.so (0x00007f6fdb000000)
	libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x00007f6fe2921000)
	/lib64/ld-linux-x86-64.so.2 (0x00007f6fe2ce6000)
"""
        self.assertEqual(
            list(
                remote_action.resolved_shlibs_from_ldd(
                    ldd_output.splitlines())),
            _paths(
                [
                    '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/librustc_driver-897e90da9cc472c4.so',
                    '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/libstd-374958b5d3497a8f.so',
                    '/lib/x86_64-linux-gnu/libdl.so.2',
                    '/lib/x86_64-linux-gnu/librt.so.1',
                    '/lib/x86_64-linux-gnu/libpthread.so.0',
                    '/lib/x86_64-linux-gnu/libc.so.6',
                    '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/../lib/libLLVM-15-rust-1.70.0-nightly.so',
                    '/lib/x86_64-linux-gnu/libm.so.6',
                ]))


class HostToolNonsystemShlibsTests(unittest.TestCase):

    def test_sample(self):
        unfiltered_shlibs = _paths(
            [
                '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/librustc_driver-897e90da9cc472c4.so',
                '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/libstd-374958b5d3497a8f.so',
                '/lib/x86_64-linux-gnu/libdl.so.2',
                '/lib/x86_64-linux-gnu/librt.so.1',
                '/lib/x86_64-linux-gnu/libpthread.so.0',
                '/lib/x86_64-linux-gnu/libc.so.6',
                '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/../lib/libLLVM-15-rust-1.70.0-nightly.so',
                '/lib/x86_64-linux-gnu/libm.so.6',
                '/usr/lib/something_else.so',
            ])
        with mock.patch.object(
                remote_action, 'host_tool_shlibs',
                return_value=unfiltered_shlibs) as mock_host_tool_shlibs:
            self.assertEqual(
                list(
                    remote_action.host_tool_nonsystem_shlibs(
                        '../path/to/rustc')),
                _paths(
                    [
                        '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/librustc_driver-897e90da9cc472c4.so',
                        '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/libstd-374958b5d3497a8f.so',
                        '/usr/home/janedoe/my_project/tools/rust/linux-x64/bin/../lib/../lib/libLLVM-15-rust-1.70.0-nightly.so',
                    ]))
        mock_host_tool_shlibs.assert_called_once()


class RewrapperArgParserTests(unittest.TestCase):

    def test_exec_root(self):
        args, _ = remote_action._REWRAPPER_ARG_PARSER.parse_known_args(
            ['--exec_root=/foo/bar'])
        self.assertEqual(args.exec_root, '/foo/bar')


class RemoteActionMainParserTests(unittest.TestCase):

    @property
    def default_cfg(self):
        return Path('default.cfg')

    @property
    def default_bindir(self):
        return Path('/opt/reclient/bin')

    def _make_main_parser(self) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser()
        remote_action.inherit_main_arg_parser_flags(
            parser,
            default_cfg=self.default_cfg,
            default_bindir=self.default_bindir)
        return parser

    def test_defaults(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--', 'echo', 'hello'])
        self.assertEqual(main_args.cfg, self.default_cfg)
        self.assertEqual(main_args.bindir, self.default_bindir)
        self.assertFalse(main_args.dry_run)
        self.assertFalse(main_args.verbose)
        self.assertEqual(main_args.label, '')
        self.assertEqual(main_args.remote_log, '')
        self.assertFalse(main_args.save_temps)
        self.assertFalse(main_args.auto_reproxy)
        self.assertIsNone(main_args.fsatrace_path)
        self.assertFalse(main_args.compare)
        self.assertFalse(main_args.diagnose_nonzero)
        self.assertEqual(main_args.command, ['echo', 'hello'])

    def test_cfg(self):
        p = self._make_main_parser()
        cfg = Path('other.cfg')
        main_args, other = p.parse_known_args([f'--cfg={cfg}', '--', 'echo'])
        self.assertEqual(main_args.cfg, cfg)
        action = remote_action.remote_action_from_args(main_args)
        self.assertEqual(action.local_command, ['echo'])
        self.assertEqual(action.options, ['--cfg', str(cfg)])

    def test_bindir(self):
        p = self._make_main_parser()
        bindir = Path('/usr/local/bin')
        main_args, other = p.parse_known_args(
            ['--bindir', str(bindir), '--', 'echo'])
        self.assertEqual(main_args.bindir, bindir)
        action = remote_action.remote_action_from_args(main_args)
        self.assertEqual(action.local_command, ['echo'])

    def test_local_command_with_env(self):
        local_command = (['FOO=BAR', 'echo'])
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--'] + local_command)
        action = remote_action.remote_action_from_args(main_args)
        self.assertEqual(
            action.local_command, [cl_utils._ENV] + local_command)

    def test_verbose(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--verbose', '--', 'echo'])
        self.assertTrue(main_args.verbose)

    def test_dry_run(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--dry-run', '--', 'echo'])
        self.assertTrue(main_args.dry_run)
        action = remote_action.remote_action_from_args(main_args)
        with mock.patch.object(remote_action.RemoteAction, 'run') as mock_run:
            exit_code = action.run_with_main_args(main_args)
        self.assertEqual(exit_code, 0)
        mock_run.assert_not_called()

    @mock.patch.object(fuchsia, 'REPROXY_WRAP', '/path/to/reproxy-wrap.sh')
    def test_auto_reproxy(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--auto-reproxy', '--', 'echo'])
        self.assertTrue(main_args.auto_reproxy)
        action = remote_action.remote_action_from_args(main_args)
        self.assertEqual(action.local_command, ['echo'])
        self.assertEqual(
            action.launch_command[:2], ['/path/to/reproxy-wrap.sh', '--'])

    def test_save_temps(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--save-temps', '--', 'echo'])
        self.assertTrue(main_args.save_temps)
        action = remote_action.remote_action_from_args(main_args)
        self.assertEqual(action.local_command, ['echo'])
        self.assertTrue(action.save_temps)

    def test_label(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--label=//build/this:that', '--', 'echo'])
        self.assertEqual(main_args.label, '//build/this:that')

    def test_diagnose_nonzero(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--diagnose-nonzero', '--', 'echo'])
        self.assertTrue(main_args.diagnose_nonzero)
        action = remote_action.remote_action_from_args(main_args)
        self.assertTrue(action.diagnose_nonzero)

    def test_remote_log_named(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--log', 'bar.remote-log', '--', 'echo'])
        self.assertEqual(main_args.remote_log, 'bar.remote-log')

    def test_remote_log_unnamed(self):
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(['--log', '--', 'echo'])
        self.assertEqual(main_args.remote_log, '<AUTO>')

    def test_remote_log_from_main_args_auto_named(self):
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--log', '--', 'touch', str(output)])
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        self.assertEqual(
            [remote_action._REMOTE_LOG_SCRIPT],
            action.inputs_relative_to_project_root)
        self.assertEqual(
            {build_dir / output, build_dir / (str(output) + '.remote-log')},
            set(action.output_files_relative_to_project_root))
        # Ignore the rewrapper portion of the command
        full_command = action.launch_command
        first_ddash = full_command.index('--')
        # Confirm that the remote command is wrapped with the logger script.
        remote_command = full_command[first_ddash + 1:]
        next_ddash = remote_command.index('--')
        self.assertEqual(
            remote_command[:next_ddash],
            _strs(
                [
                    Path('..', remote_action._REMOTE_LOG_SCRIPT), '--log',
                    str(output) + '.remote-log'
                ]))

    def test_remote_log_from_main_args_explicitly_named(self):
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        log_base = 'debug'
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--log', log_base, '--', 'touch',
             str(output)])
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        self.assertEqual(
            [remote_action._REMOTE_LOG_SCRIPT],
            action.inputs_relative_to_project_root)
        self.assertEqual(
            {
                build_dir / output,
                build_dir / (log_base + '.remote-log'),
            }, set(action.output_files_relative_to_project_root))
        # Ignore the rewrapper portion of the command
        full_command = action.launch_command
        first_ddash = full_command.index('--')
        # Confirm that the remote command is wrapped with the logger script.
        remote_command = full_command[first_ddash + 1:]
        next_ddash = remote_command.index('--')
        self.assertEqual(
            remote_command[:next_ddash],
            _strs(
                [
                    Path('..', remote_action._REMOTE_LOG_SCRIPT), '--log',
                    log_base + '.remote-log'
                ]))

    def test_remote_fsatrace_from_main_args(self):
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        fake_fsatrace = Path('tools/debug/fsatrace')
        fake_fsatrace_rel = Path(f'../{fake_fsatrace}')
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            _strs(
                ['--fsatrace-path', fake_fsatrace_rel, '--', 'touch', output]))
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        self.assertEqual(
            {fake_fsatrace, fake_fsatrace.with_suffix('.so')},
            set(action.inputs_relative_to_project_root))
        self.assertEqual(
            {
                build_dir / output, build_dir /
                (str(output) + '.remote-fsatrace')
            }, set(action.output_files_relative_to_project_root))
        # Ignore the rewrapper portion of the command
        full_command = action.launch_command
        first_ddash = full_command.index('--')
        # Confirm that the remote command is wrapped with fsatrace
        remote_command = full_command[first_ddash + 1:]
        next_ddash = remote_command.index('--')
        self.assertIn(str(fake_fsatrace_rel), remote_command[:next_ddash])
        self.assertEqual(
            remote_command[:next_ddash + 1],
            action._fsatrace_command_prefix(
                Path(str(output) + '.remote-fsatrace')))
        self.assertEqual(
            remote_command[next_ddash + 1:], ['touch', str(output)])

    def test_remote_log_and_fsatrace_from_main_args(self):
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        fake_fsatrace = Path('tools/debug/fsatrace')
        fake_fsatrace_rel = Path('..', fake_fsatrace)
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            _strs(
                [
                    '--fsatrace-path', fake_fsatrace_rel, '--log', '--',
                    'touch', output
                ]))
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        self.assertEqual(
            {
                remote_action._REMOTE_LOG_SCRIPT,
                fake_fsatrace,
                fake_fsatrace.with_suffix('.so'),
            }, set(action.inputs_relative_to_project_root))
        self.assertEqual(
            {
                build_dir / output,
                build_dir / (str(output) + '.remote-log'),
                build_dir / (str(output) + '.remote-fsatrace'),
            }, set(action.output_files_relative_to_project_root))
        # Ignore the rewrapper portion of the command
        full_command = action.launch_command
        first_ddash = full_command.index('--')
        # Confirm that the outer wrapper is for logging
        remote_command = full_command[first_ddash + 1:]
        second_ddash = remote_command.index('--')
        self.assertEqual(
            remote_command[:second_ddash],
            _strs(
                [
                    Path('..', remote_action._REMOTE_LOG_SCRIPT), '--log',
                    str(output) + '.remote-log'
                ]))
        # Confirm that the inner wrapper is for fsatrace
        logged_command = remote_command[second_ddash + 1:]
        third_ddash = logged_command.index('--')
        self.assertEqual(
            logged_command[:third_ddash + 1],
            action._fsatrace_command_prefix(
                Path(str(output) + '.remote-fsatrace')))
        self.assertEqual(
            logged_command[third_ddash + 1:], ['touch', str(output)])

    def test_local_remote_compare_no_diffs_from_main_args(self):
        # Same as test_remote_fsatrace_from_main_args, but with --compare
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        base_command = ['touch', str(output)]
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--compare', '--'] + base_command)
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        unnamed_mocks = [
            # we don't bother to check the call details of these mocks
            mock.patch.object(os, 'rename'),
            mock.patch.object(Path, 'is_file', return_value=True),
            # Pretend comparison finds no differences
            mock.patch.object(remote_action, '_files_match', return_value=True),
        ]
        with contextlib.ExitStack() as stack:
            for m in unnamed_mocks:
                stack.enter_context(m)

            # both local and remote commands succeed
            with mock.patch.object(remote_action.RemoteAction, '_run_locally',
                                   return_value=0) as mock_local_launch:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_run_maybe_remotely',
                                       return_value=cl_utils.SubprocessResult(
                                           0)) as mock_remote_launch:
                    with mock.patch.object(
                            remote_action.RemoteAction,
                            '_compare_fsatraces') as mock_compare_traces:
                        with mock.patch.object(os, 'remove') as mock_cleanup:
                            exit_code = action.run_with_main_args(main_args)

        remote_command = action.launch_command
        self.assertEqual(exit_code, 0)  # remote success and compare success
        mock_compare_traces.assert_not_called()
        mock_local_launch.assert_called_once()
        mock_remote_launch.assert_called_once()
        self.assertEqual(remote_command[-2:], base_command)
        mock_cleanup.assert_called_with(Path(str(output) + '.remote'))

    def test_local_remote_compare_found_diffs_from_main_args(self):
        # Same as test_remote_fsatrace_from_main_args, but with --compare
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        base_command = ['touch', str(output)]
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            ['--compare', '--'] + base_command)
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        unnamed_mocks = [
            # we don't bother to check the call details of these mocks
            mock.patch.object(os, 'rename'),
            mock.patch.object(Path, 'is_file', return_value=True),
            # Pretend comparison finds differences
            mock.patch.object(
                remote_action, '_files_match', return_value=False),
            mock.patch.object(remote_action, '_detail_diff'),
        ]
        with contextlib.ExitStack() as stack:
            for m in unnamed_mocks:
                stack.enter_context(m)

            # both local and remote commands succeed
            with mock.patch.object(remote_action.RemoteAction, '_run_locally',
                                   return_value=0) as mock_local_launch:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_run_maybe_remotely',
                                       return_value=cl_utils.SubprocessResult(
                                           0)) as mock_remote_launch:
                    with mock.patch.object(
                            remote_action.RemoteAction,
                            '_compare_fsatraces') as mock_compare_traces:
                        exit_code = action.run_with_main_args(main_args)

        remote_command = action.launch_command
        self.assertEqual(exit_code, 1)  # remote success, but compare failure
        mock_compare_traces.assert_not_called()
        mock_local_launch.assert_called_once()
        mock_remote_launch.assert_called_once()
        self.assertEqual(remote_command[-2:], base_command)

    def test_local_remote_compare_with_fsatrace_from_main_args(self):
        # Same as test_remote_fsatrace_from_main_args, but with --compare
        exec_root = Path('/home/project')
        build_dir = Path('build-out')
        working_dir = exec_root / build_dir
        output = Path('hello.txt')
        fake_fsatrace = Path('tools/debug/fsatrace')
        fake_fsatrace_rel = Path('..', fake_fsatrace)
        p = self._make_main_parser()
        main_args, other = p.parse_known_args(
            _strs(
                [
                    '--compare', '--fsatrace-path', fake_fsatrace_rel, '--',
                    'touch', output
                ]))
        action = remote_action.remote_action_from_args(
            main_args,
            output_files=[output],
            exec_root=exec_root,
            working_dir=working_dir,
        )

        # not repeating the same asserts from
        #   test_remote_fsatrace_from_main_args:

        unnamed_mocks = [
            # we don't bother to check the call details of these mocks
            mock.patch.object(os, 'rename'),
            mock.patch.object(Path, 'is_file', return_value=True),
            # Pretend comparison finds differences
            mock.patch.object(
                remote_action, '_files_match', return_value=False),
            mock.patch.object(remote_action, '_detail_diff'),
            # in RemoteAction._compare_fsatraces:
            mock.patch.object(remote_action, '_transform_file_by_lines'),
        ]
        with contextlib.ExitStack() as stack:
            for m in unnamed_mocks:
                stack.enter_context(m)

            # both local and remote commands succeed
            with mock.patch.object(remote_action.RemoteAction, '_run_locally',
                                   return_value=0) as mock_local_launch:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_run_maybe_remotely',
                                       return_value=cl_utils.SubprocessResult(
                                           0)) as mock_remote_launch:
                    with mock.patch.object(remote_action, '_text_diff',
                                           return_value=0) as mock_trace_diff:
                        exit_code = action.run_with_main_args(main_args)

        remote_command = action.launch_command
        # make sure local command is also traced
        local_command = list(action._generate_local_launch_command())
        self.assertEqual(exit_code, 1)  # remote success, but compare failure
        mock_remote_launch.assert_called_once()
        mock_local_launch.assert_called_once()
        self.assertIn(str(fake_fsatrace_rel), remote_command)
        self.assertIn(str(fake_fsatrace_rel), local_command)
        remote_trace = str(output) + '.remote-fsatrace'
        local_trace = str(output) + '.local-fsatrace'
        self.assertIn(remote_trace, remote_command)
        self.assertIn(local_trace, local_command)
        mock_trace_diff.assert_called_with(
            Path(local_trace + '.norm'), Path(remote_trace + '.norm'))


class RemoteActionFlagParserTests(unittest.TestCase):

    def test_defaults(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args([])
        self.assertFalse(remote_args.disable)
        self.assertEqual(remote_args.inputs, [])
        self.assertEqual(remote_args.output_files, [])
        self.assertEqual(remote_args.output_dirs, [])
        self.assertEqual(remote_args.flags, [])
        self.assertEqual(other, [])

    def test_command_without_forwarding(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        command = [
            'clang++', '--target=powerpc-apple-darwin8',
            '-fcrash-diagnostics-dir=nothing/to/see/here', '-c', 'hello.cxx',
            '-o', 'hello.o'
        ]
        remote_args, other = p.parse_known_args(command)
        self.assertFalse(remote_args.disable)
        self.assertEqual(remote_args.inputs, [])
        self.assertEqual(remote_args.output_files, [])
        self.assertEqual(remote_args.output_dirs, [])
        self.assertEqual(remote_args.flags, [])
        self.assertEqual(other, command)

    def test_disable(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args(
            ['cat', 'foo.txt', '--remote-disable'])
        self.assertTrue(remote_args.disable)
        self.assertEqual(other, ['cat', 'foo.txt'])

    def test_inputs(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args(
            [
                'cat', '--remote-inputs=bar.txt', 'bar.txt',
                '--remote-inputs=quux.txt', 'quux.txt'
            ])
        self.assertEqual(remote_args.inputs, ['bar.txt', 'quux.txt'])
        self.assertEqual(other, ['cat', 'bar.txt', 'quux.txt'])

    def test_inputs_comma(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args(
            [
                'cat', '--remote-inputs=w,x', 'bar.txt', '--remote-inputs=y,z',
                'quux.txt'
            ])
        self.assertEqual(
            list(cl_utils.flatten_comma_list(remote_args.inputs)),
            ['w', 'x', 'y', 'z'])
        self.assertEqual(other, ['cat', 'bar.txt', 'quux.txt'])

    def test_output_files_comma(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args(
            [
                './generate.sh', '--remote-outputs=w,x', 'bar.txt',
                '--remote-outputs=y,z', 'quux.txt'
            ])
        self.assertEqual(
            list(cl_utils.flatten_comma_list(remote_args.output_files)),
            ['w', 'x', 'y', 'z'])
        self.assertEqual(other, ['./generate.sh', 'bar.txt', 'quux.txt'])

    def test_output_dirs_comma(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args(
            [
                './generate_dirs.sh', '--remote-output-dirs=w,x', 'bar.txt',
                '--remote-output-dirs=y,z', 'quux.txt'
            ])
        self.assertEqual(
            list(cl_utils.flatten_comma_list(remote_args.output_dirs)),
            ['w', 'x', 'y', 'z'])
        self.assertEqual(other, ['./generate_dirs.sh', 'bar.txt', 'quux.txt'])

    def test_flags(self):
        p = remote_action.REMOTE_FLAG_ARG_PARSER
        remote_args, other = p.parse_known_args(
            [
                'cat', '--remote-flag=--foo=bar', 'bar.txt',
                '--remote-flag=--opt=quux', 'quux.txt'
            ])
        self.assertEqual(remote_args.flags, ['--foo=bar', '--opt=quux'])
        self.assertEqual(other, ['cat', 'bar.txt', 'quux.txt'])


class RemoteActionConstructionTests(unittest.TestCase):

    _PROJECT_ROOT = Path('/my/project/root')
    _WORKING_DIR = _PROJECT_ROOT / 'build_dir'

    def test_minimal(self):
        rewrapper = '/path/to/rewrapper'
        command = ['cat', 'meow.txt']
        action = remote_action.RemoteAction(
            rewrapper=rewrapper,
            command=command,
            exec_root=self._PROJECT_ROOT,
            working_dir=self._WORKING_DIR,
        )
        self.assertEqual(action.local_command, command)
        self.assertEqual(action.exec_root, self._PROJECT_ROOT)
        self.assertEqual(action.exec_root_rel, Path('..'))
        self.assertFalse(action.save_temps)
        self.assertFalse(action.auto_reproxy)
        self.assertFalse(action.remote_disable)
        self.assertEqual(action.build_subdir, Path('build_dir'))
        self.assertEqual(
            action.launch_command,
            [rewrapper, f'--exec_root={self._PROJECT_ROOT}', '--'] + command)

    def test_path_setup_implicit(self):
        command = ['beep', 'boop']
        fake_root = Path('/home/project')
        fake_builddir = Path('out/not-default')
        fake_cwd = fake_root / fake_builddir
        with mock.patch.object(os, 'curdir', fake_cwd):
            with mock.patch.object(remote_action, 'PROJECT_ROOT', fake_root):
                action = remote_action.RemoteAction(
                    rewrapper='/path/to/rewrapper',
                    command=command,
                )
                self.assertEqual(action.exec_root, fake_root)
                self.assertEqual(action.exec_root_rel, Path('../..'))
                self.assertEqual(action.build_subdir, fake_builddir)

    def test_path_setup_explicit(self):
        command = ['beep', 'boop']
        fake_root = Path('/home/project')
        fake_builddir = Path('out/not-default')
        fake_cwd = fake_root / fake_builddir
        with mock.patch.object(os, 'curdir', fake_cwd):
            action = remote_action.RemoteAction(
                rewrapper='/path/to/rewrapper',
                command=command,
                exec_root=fake_root,
            )
            self.assertEqual(action.exec_root, fake_root)
            self.assertEqual(action.exec_root_rel, Path('../..'))
            self.assertEqual(action.build_subdir, fake_builddir)

    def test_inputs_outputs(self):
        command = ['cat', '../src/meow.txt']
        action = remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=self._PROJECT_ROOT,
            working_dir=self._WORKING_DIR,
            inputs=_paths(['../src/meow.txt']),
            output_files=_paths(['obj/woof.txt']),
            output_dirs=_paths(['.debug']),
        )
        self.assertEqual(action.build_subdir, Path('build_dir'))
        self.assertEqual(
            action.inputs_relative_to_project_root, _paths(['src/meow.txt']))
        self.assertEqual(
            action.output_files_relative_to_project_root,
            _paths(['build_dir/obj/woof.txt']))
        self.assertEqual(
            action.output_dirs_relative_to_project_root,
            _paths(['build_dir/.debug']))
        with mock.patch.object(
                remote_action.RemoteAction, '_inputs_list_file',
                return_value=Path(
                    'obj/woof.txt.inputs')) as mock_input_list_file:
            self.assertEqual(
                action.launch_command, [
                    '/path/to/rewrapper',
                    f'--exec_root={self._PROJECT_ROOT}',
                    '--input_list_paths=obj/woof.txt.inputs',
                    '--output_files=build_dir/obj/woof.txt',
                    '--output_directories=build_dir/.debug',
                    '--',
                    'cat',
                    '../src/meow.txt',
                ])
            mock_input_list_file.assert_called_once()
            with mock.patch.object(
                    remote_action.RemoteAction, '_run_maybe_remotely',
                    return_value=cl_utils.SubprocessResult(0)) as mock_call:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_cleanup') as mock_cleanup:
                    self.assertEqual(action.run(), 0)
                    mock_call.assert_called_once()
                    mock_cleanup.assert_called_once()

    def test_save_temps(self):
        command = ['echo', 'hello']
        action = remote_action.RemoteAction(
            rewrapper=Path('/path/to/rewrapper'),
            command=command,
            exec_root=self._PROJECT_ROOT,
            save_temps=True,
        )
        self.assertEqual(action.local_command, command)
        self.assertEqual(action.exec_root, self._PROJECT_ROOT)
        self.assertTrue(action.save_temps)
        with mock.patch.object(
                remote_action.RemoteAction, '_run_maybe_remotely',
                return_value=cl_utils.SubprocessResult(0)) as mock_call:
            with mock.patch.object(remote_action.RemoteAction,
                                   '_cleanup') as mock_cleanup:
                self.assertEqual(action.run(), 0)
                mock_call.assert_called_once()
                mock_cleanup.assert_not_called()

    def test_flag_forwarding(self):
        command = [
            'cat', '--remote-flag=--exec_strategy=racing', '../src/cow/moo.txt'
        ]
        action = remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=self._PROJECT_ROOT,
            working_dir=self._WORKING_DIR,
        )
        self.assertEqual(action.local_command, ['cat', '../src/cow/moo.txt'])
        self.assertEqual(action.options, ['--exec_strategy=racing'])

    def test_fail_no_retry(self):
        command = ['echo', 'hello']
        action = remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=self._PROJECT_ROOT,
        )
        self.assertEqual(action.local_command, command)
        self.assertEqual(action.exec_root, self._PROJECT_ROOT)

        for exit_code in (1, 2):
            with mock.patch.object(remote_action.RemoteAction,
                                   '_run_maybe_remotely',
                                   return_value=cl_utils.SubprocessResult(
                                       exit_code)) as mock_call:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_cleanup') as mock_cleanup:
                    self.assertEqual(action.run(), exit_code)

            mock_cleanup.assert_called_once()
            mock_call.assert_called_once()  # no retry

    def test_file_not_found_no_retry(self):
        command = ['echo', 'hello']
        action = remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=self._PROJECT_ROOT,
        )
        self.assertEqual(action.local_command, command)
        self.assertEqual(action.exec_root, self._PROJECT_ROOT)

        with mock.patch.object(
                remote_action.RemoteAction,
                '_run_maybe_remotely',
                return_value=cl_utils.SubprocessResult(
                    returncode=2, stderr=['ERROR: file not found: /bin/smash',
                                          'going home now']),
        ) as mock_call:
            with mock.patch.object(remote_action.RemoteAction,
                                   '_cleanup') as mock_cleanup:
                self.assertEqual(action.run(), 2)

        mock_cleanup.assert_called_once()
        mock_call.assert_called_once()  # no retry

    def test_retry_once_successful(self):
        command = ['echo', 'hello']
        action = remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=self._PROJECT_ROOT,
        )
        self.assertEqual(action.local_command, command)
        self.assertEqual(action.exec_root, self._PROJECT_ROOT)

        for exit_code in remote_action._RETRIABLE_REWRAPPER_STATUSES:
            with mock.patch.object(
                    remote_action.RemoteAction,
                    '_run_maybe_remotely',
                    side_effect=[
                        # If at first you don't succeed,
                        cl_utils.SubprocessResult(exit_code),
                        # try, try again (and succeed).
                        cl_utils.SubprocessResult(0),
                    ]) as mock_call:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_cleanup') as mock_cleanup:
                    self.assertEqual(action.run(), 0)

            mock_cleanup.assert_called_once()
            # expect called twice, second time is the retry
            self.assertEqual(len(mock_call.call_args_list), 2)

    def test_retry_once_fails_again(self):
        command = ['echo', 'hello']
        action = remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=self._PROJECT_ROOT,
        )
        self.assertEqual(action.local_command, command)
        self.assertEqual(action.exec_root, self._PROJECT_ROOT)

        for exit_code in remote_action._RETRIABLE_REWRAPPER_STATUSES:
            with mock.patch.object(
                    remote_action.RemoteAction,
                    '_run_maybe_remotely',
                    side_effect=[
                        # If at first you don't succeed,
                        cl_utils.SubprocessResult(exit_code),
                        # try, try again (and fail again).
                        cl_utils.SubprocessResult(exit_code),
                    ]) as mock_call:
                with mock.patch.object(remote_action.RemoteAction,
                                       '_cleanup') as mock_cleanup:
                    self.assertEqual(action.run(), exit_code)  # fail

            mock_cleanup.assert_called_once()
            # expect called twice, second time is the retry
            self.assertEqual(len(mock_call.call_args_list), 2)


class RbeDiagnosticsTests(unittest.TestCase):

    def _make_action(self, **kwargs):
        command = ['echo', 'hello']
        exec_root = Path('/path/to/project/root')
        working_dir = exec_root / 'build/stuff/here'
        return remote_action.RemoteAction(
            rewrapper='/path/to/rewrapper',
            command=command,
            exec_root=exec_root,
            working_dir=working_dir,
            **kwargs,
        )

    def test_analyze_conditions_positive(self):
        action = self._make_action(diagnose_nonzero=True)

        with mock.patch.object(
                remote_action.RemoteAction, '_run_maybe_remotely',
                return_value=cl_utils.SubprocessResult(1)) as mock_run:
            with mock.patch.object(remote_action.RemoteAction,
                                   '_cleanup') as mock_cleanup:
                with mock.patch.object(remote_action,
                                       'analyze_rbe_logs') as mock_analyze:
                    self.assertEqual(action.run(), 1)

        mock_cleanup.assert_called_once()
        mock_run.assert_called_once()
        mock_analyze.assert_called_once()
        args, kwargs = mock_analyze.call_args_list[0]
        self.assertEqual(kwargs["action_log"], action._action_log)

    def test_analyzing_not_requested(self):
        action = self._make_action(diagnose_nonzero=False)

        with mock.patch.object(
                remote_action.RemoteAction, '_run_maybe_remotely',
                return_value=cl_utils.SubprocessResult(1)) as mock_run:
            with mock.patch.object(remote_action.RemoteAction,
                                   '_cleanup') as mock_cleanup:
                with mock.patch.object(remote_action,
                                       'analyze_rbe_logs') as mock_analyze:
                    self.assertEqual(action.run(), 1)

        mock_cleanup.assert_called_once()
        mock_run.assert_called_once()
        mock_analyze.assert_not_called()

    def test_not_analyzing_on_success(self):
        action = self._make_action(diagnose_nonzero=True)

        with mock.patch.object(
                remote_action.RemoteAction, '_run_maybe_remotely',
                return_value=cl_utils.SubprocessResult(0)) as mock_run:
            with mock.patch.object(remote_action.RemoteAction,
                                   '_cleanup') as mock_cleanup:
                with mock.patch.object(remote_action,
                                       'analyze_rbe_logs') as mock_analyze:
                    self.assertEqual(action.run(), 0)

        mock_cleanup.assert_called_once()
        mock_run.assert_called_once()
        mock_analyze.assert_not_called()

    def test_not_analyzing_local_execution(self):
        action = self._make_action(diagnose_nonzero=True, exec_strategy="local")

        with mock.patch.object(
                remote_action.RemoteAction, '_run_maybe_remotely',
                return_value=cl_utils.SubprocessResult(1)) as mock_run:
            with mock.patch.object(remote_action.RemoteAction,
                                   '_cleanup') as mock_cleanup:
                with mock.patch.object(remote_action,
                                       'analyze_rbe_logs') as mock_analyze:
                    self.assertEqual(action.run(), 1)

        mock_cleanup.assert_called_once()
        mock_run.assert_called_once()
        mock_analyze.assert_not_called()

    def test_analyze_flow(self):
        pid = 6789
        action_log = Path('obj/my_action.rrpl')
        fake_rewrapper_logs = [
            f'/noisy/log/rewrapper.where.who.log.INFO.when.{pid}',
            f'/noisy/log/rewrapper.where.who.log.ERROR.when.{pid}',
        ]
        unnamed_mocks = [
            mock.patch.object(
                remote_action,
                '_reproxy_log_dir',
                return_value='/path/to/tmp/reproxy.999999'),
            mock.patch.object(
                remote_action,
                '_rewrapper_log_dir',
                return_value='/path/to/tmp/reproxy.999999/wrapper/logz'),
            mock.patch.object(Path, 'is_file', return_value=True),
        ]
        with contextlib.ExitStack() as stack:
            for m in unnamed_mocks:
                stack.enter_context(m)

            with mock.patch.object(
                    Path, 'glob',
                    return_value=fake_rewrapper_logs) as mock_glob:
                with mock.patch.object(
                        remote_action,
                        '_parse_rewrapper_action_log') as mock_parse_action_log:
                    with mock.patch.object(remote_action,
                                           '_file_lines_matching',
                                           return_value=['this is interesting'
                                                        ]) as mock_read_log:
                        with mock.patch.object(remote_action,
                                               '_diagnose_reproxy_error_line'
                                              ) as mock_diagnose_line:
                            remote_action.analyze_rbe_logs(
                                rewrapper_pid=pid,
                                action_log=action_log,
                            )
        mock_glob.assert_called_once()
        mock_read_log.assert_called_once()
        mock_parse_action_log.assert_called_with(action_log)
        mock_diagnose_line.assert_called()

    def test_parse_reproxy_log_record_lines(self):
        exec_id = "xx-yy-zzzz"
        digest = "2afd98ae7274456b2bfc208e10f4cbe75fca88c2c41e352e57cb6b9ad840bf64/144"
        log_lines = f"""
command:  {{
        identifiers:  {{
                command_id:  "8ea55c85-0ae36078"
                invocation_id:  "979eedda-4643-45da-af15-0d2f8531ba98"
                tool_name:  "re-client"
                execution_id:  "{exec_id}"
        }}
}}
remote_metadata:  {{
        command_digest:  "e9d024e5dc99438b08f4592a09379e65146149821480e2057c7afc285d30f090/181"
        action_digest:  "{digest}"
}}
        """.splitlines()
        log_entry = remote_action._parse_reproxy_log_record_lines(log_lines)
        self.assertEqual(log_entry.execution_id, exec_id)
        self.assertEqual(log_entry.action_digest, digest)

    def test_diagnose_uninteresting_log_line(self):
        line = "This diagnostic does not appear interesting."
        f = io.StringIO()
        with contextlib.redirect_stdout(f):
            remote_action._diagnose_reproxy_error_line(line)
        self.assertEqual(f.getvalue(), '')

    def test_diagnose_fail_to_dial(self):
        line = "Fail to dial something something unix:///path/to/reproxy.socket"
        f = io.StringIO()
        with contextlib.redirect_stdout(f):
            remote_action._diagnose_reproxy_error_line(line)
        self.assertIn("reproxy is not running", f.getvalue())

    def test_diagnose_rbe_permissions(self):
        line = "Error connecting to remote execution client: rpc error: code = PermissionDenied.  You have no power here!"
        f = io.StringIO()
        with contextlib.redirect_stdout(f):
            remote_action._diagnose_reproxy_error_line(line)
        self.assertIn(
            "You might not have permssion to access the RBE instance",
            f.getvalue())

    def test_diagnose_missing_input_file(self):
        path = "../oops/did/I/forget/this.file"
        line = f"Status:LocalErrorResultStatus ... Err:stat {path}: no such file or directory"
        f = io.StringIO()
        with contextlib.redirect_stdout(f):
            remote_action._diagnose_reproxy_error_line(line)
        self.assertIn(
            f"missing a local input file for uploading: {path} (source)",
            f.getvalue())


class MainTests(unittest.TestCase):

    def test_help_flag(self):
        stdout = io.StringIO()
        # Just make sure help exits successfully, without any exceptions
        # due to argument parsing.
        with contextlib.redirect_stdout(stdout):
            with mock.patch.object(sys, 'exit') as mock_exit:
                # Normally, the following would not be reached due to exit(),
                # but for testing it needs to be mocked out.
                with mock.patch.object(remote_action.RemoteAction,
                                       'run_with_main_args', return_value=0):
                    self.assertEqual(remote_action.main(['--help']), 0)
            mock_exit.assert_called_with(0)


if __name__ == '__main__':
    unittest.main()
