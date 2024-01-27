#!/usr/bin/env fuchsia-vendored-python
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from tempfile import TemporaryDirectory
import os
import unittest

from verify_deps_in_sdk import VerifyDepsInSDK


class VerifyDepsInSDKTests(unittest.TestCase):

    def create_empty_sdk_manifest(self, root_build_dir, manifest_name):
        manifest_path = root_build_dir + '/sdk/manifest/'
        if not os.path.exists(manifest_path):
            os.makedirs(manifest_path)
        sdk_manifest = manifest_path + manifest_name
        if not os.path.isfile(sdk_manifest):
            with open(sdk_manifest, 'w') as f:
                f.write("")

        return sdk_manifest

    def test_init(self):
        output_file = 'test.unused'
        invoker_label = "//sdk/ctf/build:verify_deps_in_sdk_test"
        deps = ['//zircon/system/ulib/zxtest:zxtest']
        allowed_dirs = ['//sdk/*']

        with TemporaryDirectory() as root_build_dir:
            sdk_manifests = [
                self.create_empty_sdk_manifest(root_build_dir, "core")
            ]
            try:
                VerifyDepsInSDK(
                    root_build_dir, output_file, invoker_label, deps, deps,
                    allowed_dirs, sdk_manifests)
            except Exception as e:
                self.assertTrue(False, e)

            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    '', output_file, invoker_label, deps, deps, allowed_dirs,
                    sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    '/this/path/doesnt/exist', output_file, invoker_label, deps,
                    deps, allowed_dirs, sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    root_build_dir, '', invoker_label, deps, deps, allowed_dirs,
                    sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    root_build_dir, output_file, '', deps, deps, allowed_dirs,
                    sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    root_build_dir, output_file, invoker_label, [], deps,
                    allowed_dirs, sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    root_build_dir, output_file, invoker_label, deps, [],
                    allowed_dirs, sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    root_build_dir, output_file, invoker_label, deps, deps, [],
                    sdk_manifests)
            with self.assertRaises(ValueError):
                VerifyDepsInSDK(
                    root_build_dir, output_file, invoker_label, deps, deps,
                    allowed_dirs, ['/this/path/doesnt/exist'])

    def test_get_ctf_file_path(self):
        output_file = 'test.unused'
        invoker_label = "//sdk/ctf/build:verify_deps_in_sdk_test"
        deps = ['//zircon/system/ulib/zxtest:zxtest']
        allowed_dirs = ['//sdk/*']
        with TemporaryDirectory() as root_build_dir:
            sdk_manifests = [
                self.create_empty_sdk_manifest(root_build_dir, "core")
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, deps,
                allowed_dirs, sdk_manifests)

            dep = '//zircon/system/ulib/zxtest:zxtest'
            self.assertEqual(
                root_build_dir +
                '/ctf/zircon/system/ulib/zxtest/zxtest.this_is_ctf',
                ctf_element.get_ctf_file_path(dep))

            dep = '//zircon/system/ulib/zxtest'
            self.assertEqual(
                root_build_dir +
                '/ctf/zircon/system/ulib/zxtest/zxtest.this_is_ctf',
                ctf_element.get_ctf_file_path(dep))

            dep = '//sdk'
            self.assertEqual(
                root_build_dir + '/ctf/sdk/sdk.this_is_ctf',
                ctf_element.get_ctf_file_path(dep))

    def test_verify_deps_in_sdk(self):
        output_file = 'test.unused'
        invoker_label = "//sdk/ctf/build:verify_deps_in_sdk_test"
        allowed_deps = ['//zircon/system/ulib/zxtest:zxtest']
        allowed_dirs = ['//third_party/dart-pkg/pub/*']
        deps = [
            '//sdk/fidl/fuchsia.io', '//sdk/lib/fdio:fdio',
            '//sdk/lib/private_atom:private_atom'
        ]

        fdio_atom = '//sdk/lib/fdio:fdio_sdk_manifest'
        fuchsia_io_atom = '//sdk/fidl/fuchsia.io:fuchsia.io_sdk'
        fuchsia_git_atom = '//sdk/lib/private_atom:private_atom'

        # Assert deps that are not in a manifest or allow list fail verification.
        with TemporaryDirectory() as root_build_dir:
            manifest = []
            sdk_manifest = self.create_empty_sdk_manifest(
                root_build_dir, "core")

            deps = ['//sdk/lib/fdio:fdio']
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, [sdk_manifest])
            self.assertEqual(deps, ctf_element.verify_deps_in_sdk(deps))

        # Assert deps in manifest do not fail verification.
        with TemporaryDirectory() as root_build_dir:
            manifest = []
            sdk_manifest = self.create_empty_sdk_manifest(
                root_build_dir, "core")
            deps = [
                '//sdk/lib/fdio:fdio',
                '//sdk/fidl/fuchsia.io:fuchsia.io',
                '//sdk/lib/private_atom',
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, [sdk_manifest])

            manifest.append(fdio_atom)
            with open(sdk_manifest, 'w') as f:
                for atom in manifest:
                    f.write(f'{atom}\n')
            self.assertEqual(deps[1:], ctf_element.verify_deps_in_sdk(deps))

            manifest.append(fuchsia_io_atom)
            with open(sdk_manifest, 'w') as f:
                for atom in manifest:
                    f.write(f'{atom}\n')
            self.assertEqual(deps[2:], ctf_element.verify_deps_in_sdk(deps))

        # Assert deps are verified from multiple manifests.
        with TemporaryDirectory() as root_build_dir:
            manifest = []
            manifest1 = self.create_empty_sdk_manifest(root_build_dir, "core")
            manifest.append(fdio_atom)
            with open(manifest1, 'w') as f:
                for atom in manifest:
                    f.write(f'{atom}\n')
            manifest = []
            manifest2 = self.create_empty_sdk_manifest(root_build_dir, "core2")
            manifest.append(fuchsia_io_atom)
            with open(manifest2, 'w') as f:
                for atom in manifest:
                    f.write(f'{atom}\n')

            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, [manifest1, manifest2])

            deps = [
                '//sdk/lib/fdio:fdio',
                '//sdk/fidl/fuchsia.io:fuchsia.io',
                '//sdk/lib/private_atom',
            ]
            self.assertEqual(deps[2:], ctf_element.verify_deps_in_sdk(deps))

    def test_verify_deps(self):
        output_file = 'test.unused'
        invoker_label = "//sdk/ctf/build:verify_deps_in_sdk_test"
        allowed_deps = ['//zircon/system/ulib/zxtest:zxtest']
        allowed_dirs = ['//sdk/*']

        # Verify deps from allowlisted directories do not fail verification.
        with TemporaryDirectory() as root_build_dir:
            sdk_manifests = [
                self.create_empty_sdk_manifest(root_build_dir, "core")
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, allowed_deps,
                allowed_deps, allowed_dirs, sdk_manifests)
            self.assertListEqual(ctf_element.verify_deps(), [])

            deps = ['//this/dep/isnt/from/an:sdk']
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            self.assertListEqual(ctf_element.verify_deps(), deps)

            deps = [
                '//this/dep/isnt/from/an:sdk',
                '//this/dep/isnt/from/an:sdk2',
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            self.assertListEqual(ctf_element.verify_deps(), deps)

            deps = ['//sdk/this/is/a/real:target']
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            self.assertListEqual(ctf_element.verify_deps(), [])

            deps = [
                '//sdk/this/is/a/real:target',
                '//zircon/system/ulib/zxtest:zxtest',
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            self.assertListEqual(ctf_element.verify_deps(), [])

            deps = [
                '//sdk/lib/fdio:fdio',
                '//third_party/dart-pkg/pub/some-dart-pkg',
                '//zircon/system/ulib/zxtest:zxtest',
            ]
            manifest = ['//sdk/lib/fdio:fdio_sdk_manifest']
            with open(sdk_manifests[0], 'w') as sdk_manifest:
                for atom in manifest:
                    sdk_manifest.write(f'{atom}\n')
            allowed_dirs = ['//third_party/dart-pkg/pub/*']
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            self.assertListEqual(ctf_element.verify_deps(), [])

    def test_create_output_file(self):
        invoker_label = "//sdk/ctf/build:verify_deps_in_sdk_test"
        deps = ['//sdk:sdk', '//zircon/system/ulib/zxtest:zxtest']
        allowed_deps = ['//zircon/system/ulib/zxtest:zxtest']
        allowed_dirs = ['//sdk/*']

        with TemporaryDirectory() as root_build_dir:
            output_file = root_build_dir + '/create_output_file.unused'
            sdk_manifests = [
                self.create_empty_sdk_manifest(root_build_dir, "core")
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            ctf_element.create_output_file()
            self.assertTrue(os.path.exists(output_file))
            with open(output_file) as f:
                lines = [line.strip() for line in f.readlines()]
                self.assertListEqual(deps, lines)

        with TemporaryDirectory() as root_build_dir:
            output_file = root_build_dir + '/ctf/create_output_file.unused'
            sdk_manifests = [
                self.create_empty_sdk_manifest(root_build_dir, "core")
            ]
            ctf_element = VerifyDepsInSDK(
                root_build_dir, output_file, invoker_label, deps, allowed_deps,
                allowed_dirs, sdk_manifests)
            ctf_element.create_output_file()
            self.assertTrue(os.path.exists(output_file))
            with open(output_file) as f:
                lines = [line.strip() for line in f.readlines()]
                self.assertListEqual(deps, lines)


if __name__ == '__main__':
    unittest.main()
