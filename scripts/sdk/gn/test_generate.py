#!/usr/bin/env fuchsia-vendored-python
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import filecmp
import imp
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GENERATE_FILEPATH = os.path.join(SCRIPT_DIR, 'generate.py')
generate = imp.load_source('generate', GENERATE_FILEPATH)

TMP_DIR_NAME = tempfile.mkdtemp(prefix='tmp_unittest_%s_' % 'GNGenerateTest')
TMP_ARCH_DIR = tempfile.mkdtemp(prefix='tmp_unittest_%s_' % 'GNGenArchiveTest')
TMP_ARCHIVE_PATH = os.path.join(TMP_ARCH_DIR, 'gn.tar.gz')

EXPECTED_PREBUILTS = {
    'aemu': 'bid:7927554',
    'grpcwebproxy': 'git_revision:b123456789abcdef0123456789abcdef01234567',
    'device_launcher': 'g3-revision:vdl_fuchsia_20200819_RC00',
}


class GNGenerateTest(unittest.TestCase):

    def setUp(self):
        # make sure TMP_DIR_NAME is empty
        if os.path.exists(TMP_DIR_NAME):
            shutil.rmtree(TMP_DIR_NAME)
        os.makedirs(TMP_DIR_NAME)

    def tearDown(self):
        if os.path.exists(TMP_DIR_NAME):
            shutil.rmtree(TMP_DIR_NAME)

    def testEmptyArchive(self):
        # Run the generator.
        generate.main(
            [
                "--output",
                TMP_DIR_NAME,
                "--output-archive",
                TMP_ARCHIVE_PATH,
                "--directory",
                os.path.join(SCRIPT_DIR, 'testdata'),
                "--jiri-manifest",
                os.path.join(
                    SCRIPT_DIR, 'testdata', '.jiri_root', 'update_history',
                    'latest'),
            ])
        self.verify_contents(TMP_DIR_NAME)
        # verify tarball
        tar = tarfile.open(TMP_ARCHIVE_PATH)
        tar.extractall(TMP_ARCH_DIR)
        tar.close()
        os.remove(TMP_ARCHIVE_PATH)
        self.verify_manifest(TMP_ARCH_DIR)

    def testPrebuilts(self):
        INPUT_PREBUILTS = generate.EXTRA_PREBUILTS
        prebuilt_results = generate.get_prebuilts(
            INPUT_PREBUILTS,
            os.path.join(
                SCRIPT_DIR, 'testdata', '.jiri_root', 'update_history',
                'latest'))
        if prebuilt_results != EXPECTED_PREBUILTS:
            self.fail(
                "Expected output %s but returned %s instead" %
                (EXPECTED_PREBUILTS, prebuilt_results))

    def verify_contents(self, outdir):
        # update_golden.py doesn't copy bin and build subdirectories because we
        # don't want duplicates of things in base, so ignore them here too.
        dcmp = filecmp.dircmp(
            outdir, os.path.join(SCRIPT_DIR, 'golden'), ignore=['bin', 'build'])
        self.verify_contents_recursive(dcmp)

        # Special case: outdir/build/test_targets.gni is a generated file.
        generated_file = os.path.join(outdir, 'build', 'test_targets.gni')
        golden_file = os.path.join(
            SCRIPT_DIR, 'golden', 'build', 'test_targets.gni')
        if not filecmp.cmp(generated_file, golden_file, False):
            self.fail(_gen_diff(generated_file, golden_file))

        # Special case: Check the prebuilts *.version files generated from the jiri manifest
        for prebuilt, version in EXPECTED_PREBUILTS.items():
            in_path = os.path.join(outdir, 'bin', prebuilt + '.version')
            with open(in_path, 'r') as in_file:
                in_version = in_file.read().strip()
                if in_version != version:
                    self.fail(
                        "Generated %s in %s does not match expected %s" %
                        (in_version, in_path))

    def verify_contents_recursive(self, dcmp):
        """Recursively checks for differences between two directories.

        Fails if the directories do not appear to be deeply identical in
        structure and content.

        Args:
            dcmp (filecmp.dircmp): A dircmp of the directories.
        """
        if dcmp.left_only or dcmp.right_only:
            self.fail("Generated SDK does not match golden files. " \
                "You can run ./update_golden.py to update them.\n" \
                "Only in {}:\n{}\n\n" \
                "Only in {}:\n{}\n\n"
                .format(dcmp.left, dcmp.left_only, dcmp.right, dcmp.right_only))
        elif dcmp.diff_files:
            # Show a diff of the culprit files. Need to run diff for each pair.
            diff_result = ''
            for file in dcmp.diff_files:
                diff_result += _gen_diff(
                    os.path.join(dcmp.left, file),
                    os.path.join(dcmp.right, file))
            self.fail("Generated SDK does not match golden files. " \
                "You can run ./update_golden.py to update them.\n" \
                "Left : {}\n" \
                "Right: {}\n" \
                "Different files: {}\n" \
                "{}"
                .format(dcmp.left, dcmp.right, dcmp.diff_files, diff_result))

        for sub_dcmp in dcmp.subdirs.values():
            self.verify_contents_recursive(sub_dcmp)

    def verify_manifest(self, sdk_dir):
        """Read the manifest and verify all files are referenced."""
        metafile = os.path.join(sdk_dir, 'meta', 'manifest.json')
        fileset = set()
        fileset.add(os.path.relpath(metafile, sdk_dir))
        with open(metafile, 'r') as input:
            metadata = json.load(input)
        for atom in metadata['parts']:
            fileset.add(atom['meta'])
            with open(os.path.join(sdk_dir, atom['meta']), 'r') as input:
                atom_meta = json.load(input)
                fileset.update(self.get_atom_files(atom_meta))
        self.assertTrue(len(fileset) != 0)
        # walk the sdk_dir matching the files in the set.
        for dir_name, _, file_list in os.walk(sdk_dir):
            for f in file_list:
                found_file = os.path.relpath(os.path.join(dir_name, f), sdk_dir)
                self.assertIn(found_file, fileset)
                fileset.remove(found_file)
        self.assertTrue(
            len(fileset) == 0, "Files missing from manifest: %s" % str(fileset))

    def get_atom_files(self, atom):
        files = set()
        if 'headers' in atom:
            files.update(atom['headers'])
        if 'sources' in atom:
            files.update(atom['sources'])
        if 'data' in atom:
            files.update(atom['data'])
        if 'docs' in atom:
            files.update(atom['docs'])
        if 'files' in atom:
            files.update(atom['files'])
        if 'resources' in atom:
            files.update(atom['resources'])
        if 'target_files' in atom:
            for a in atom['target_files']:
                files.update(atom['target_files'][a])
        if 'binaries' in atom:
            for a in atom['binaries']:
                arch_atom = atom['binaries'][a]
                if 'debug' in arch_atom:
                    files.add(arch_atom['debug'])
                if 'dist' in arch_atom:
                    files.add(arch_atom['dist'])
                if 'link' in arch_atom:
                    files.add(arch_atom['link'])
                if type(arch_atom) is list:
                    files.update(arch_atom)
        if 'versions' in atom:
            for a in atom['versions']:
                arch_atom = atom['versions'][a]
                if 'debug_libs' in arch_atom:
                    files.update(arch_atom['debug_libs'])
                if 'dist_libs' in arch_atom:
                    files.update(arch_atom['dist_libs'])
                if 'headers' in arch_atom:
                    files.update(arch_atom['headers'])
                if 'link_libs' in arch_atom:
                    files.update(arch_atom['link_libs'])

        return files


def _gen_diff(a, b):
    cmd_args = ['diff', '-U', '2', a, b]
    pipe = subprocess.Popen(cmd_args, stdout=subprocess.PIPE, text=True)
    out, err = pipe.communicate()
    return "diff of '{}':\n{}\n".format(os.path.basename(a), out)


def TestMain():
    unittest.main()


if __name__ == '__main__':
    TestMain()
