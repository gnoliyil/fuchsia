#!/usr/bin/env python3
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""SDK Frontend generator for transforming the IDK into the GN SDK.

This class accepts a directory or tarball of an instance of the Integrator
Developer Kit (IDK) and uses the metadata from the IDK to drive the
construction of a specific SDK for use in projects using GN.
"""

import argparse
import glob
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import xml.etree.ElementTree

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FUCHSIA_ROOT = os.path.dirname(  # $root
    os.path.dirname(  # scripts
        os.path.dirname(  # sdk
            SCRIPT_DIR)))  # gn
PIGZ_PATH = os.path.join(
    FUCHSIA_ROOT, "prebuilt", "third_party", "pigz", "pigz")

sys.path += [os.path.join(FUCHSIA_ROOT, 'scripts', 'sdk', 'common')]
from files import copy_tree
from frontend import Frontend
import template_model as model

from collections import namedtuple

# Capture the version of required prebuilts from the jiri manifest. Note
# that ${platform} is actually part of the XML package name, so should
# not be interpreted.
EXTRA_PREBUILTS = {
    'fuchsia/third_party/android/aemu/release/${platform}': 'aemu',
    'fuchsia/third_party/grpcwebproxy/${platform}': 'grpcwebproxy',
    'fuchsia/vdl/${platform}': 'device_launcher',
}


class GNBuilder(Frontend):
    """Frontend for GN.

  Based on the metadata in the IDK used, this frontend generates GN
  specific build rules for using the contents of the IDK. It also adds a
  collection of tools for interacting with devices and emulators running
  Fuchsia.

  The templates used are in the templates directory, and the static content that
  is added to the SDK is in the base directory.
  """

    def __init__(
            self,
            local_dir,
            output,
            archive='',
            directory='',
            jiri_manifest=None):
        """Initializes an instance of the GNBuilder.

        Note that only one of archive or directory should be specified.

    Args:
      local_dir: The local directory used to find additional resources used
        during the transformation.
     output: The output directory. The contents of this directory are removed
       before processing the IDK.
      archive: The tarball archive to process. Can be empty meaning use the
        directory parameter as input.
      directory: The directory containing an unpackaged IDK. Can be empty
        meaning use the archive parameter as input.
      jiri_manifest: File containing external references in XML format.
        This is typically $FUCHSIA_ROOT/.jiri_root/update_history/latest
    """
        super(GNBuilder, self).__init__(
            local_dir=local_dir,
            output=output,
            archive=archive,
            directory=directory)
        self.target_arches = []
        self.fidl_targets = []  # List of all FIDL library targets generated
        self.cc_prebuilt_targets = [
        ]  # List of all CC prebuilt library targets generated
        self.cc_source_targets = [
        ]  # List of all CC source library targets generated
        self.loadable_module_targets = [
        ]  # List of all loadable module targets generated
        self.sysroot_targets = []  # List of all sysroot targets generated
        self.build_files = []
        if jiri_manifest:
            self.jiri_manifest = jiri_manifest
        else:
            self.jiri_manifest = os.path.join(
                FUCHSIA_ROOT, '.jiri_root', 'update_history', 'latest')

    def prepare(self, arch, types):
        """Called before elements are processed.

    This is called by the base class before the elements in the IDK are
    processed.
    At this time, the contents of the 'base' directory are copied to the
    output directory, as well as the 'meta' directory from the IDK.

    Args:
      arch: A json object describing the host and target architectures.
      types: The list of atom types contained in the metadata.
    """
        del types  # types is not used.
        self.target_arches = arch['target']

        # Capture versions of various prebuilts contained in the jiri manifest
        prebuilt_results = get_prebuilts(EXTRA_PREBUILTS, self.jiri_manifest)
        for prebuilt, version in prebuilt_results.items():
            with open(self.dest('bin', prebuilt + '.version'), 'w') as output:
                output.write(version)

        # Propagate the manifest for the Core SDK into the GN SDK. Metadata
        # schemas are copied by the metadata_schemas documentation atom.
        self.copy_file(os.path.join('meta', 'manifest.json'))

    def finalize(self, arch, types):
        # Copy the common files. The destination of these files is relative to
        # the root of the fuchsia_sdk.
        copy_tree(self.local('base'), self.output, allow_overwrite=False)

        self.write_additional_files()
        # Find CIPD prebuilt gn.
        gn_path = os.path.join(
            FUCHSIA_ROOT, 'prebuilt', 'third_party', 'gn', '*', 'gn')
        gn = glob.glob(gn_path)[0]
        # Format gn files.
        for root, _, files in os.walk(self.output):
            for f in (f for f in files if f.endswith(('.gn', '.gni'))):
                subprocess.call([gn, 'format', os.path.join(root, f)])

    def write_additional_files(self):
        self.write_file(self.dest('.gitignore'), 'gitignore', self)
        self.write_file(
            self.dest("build", "test_targets.gni"), 'test_targets', self)
        self.write_build_file_metadata(
            'gn-build-files', 'build/gn-build-files-meta.json')

        # Propagate the Core SDK manifest, incorporating our changes.
        self.update_metadata(
            [
                'build/gn-rules-meta.json', 'build/gn-build-files-meta.json',
                'readme-meta.json'
            ] + [
                os.path.relpath(file, self.output)
                for file in glob.glob(self.dest('bin/*.json'))
            ], 'meta/manifest.json')

    def update_metadata(self, entries, metafile):
        with open(self.dest(metafile), 'r') as input:
            metadata = json.load(input)

            # Documentation, data and (companion_)host_tool metadata
            # descriptions are named inconsistently, so read the manifest and
            # copy them explicitly.
            for atom in metadata['parts']:
                if atom['type'] in ['data', 'companion_host_tool',
                                    'documentation', 'host_tool', 'ffx_tool']:
                    self.copy_file(atom['meta'])

            # There are dart components in the Core SDK which are not part
            # of the GN SDK. Remove them from the manifest.
            metadata['parts'] = [
                atom for atom in metadata['parts']
                if not atom['type'] == 'dart_library'
            ]

            for entry in entries:
                with open(self.dest(entry), 'r') as entry_meta:
                    new_meta = json.load(entry_meta)
                    metadata['parts'].append(
                        {
                            'meta': entry,
                            'type': self.get_atom_type(new_meta)
                        })
            # sort parts list so it is in a stable order
            def meta_type_key(part):
                return '%s_%s' % (part['meta'], part['type'])

            metadata['parts'].sort(key=meta_type_key)

            with open(self.dest(metafile), 'w') as output:
                json.dump(
                    metadata,
                    output,
                    indent=2,
                    sort_keys=True,
                    separators=(',', ': '))

    def write_build_file_metadata(self, name, filename):
        """Writes a metadata atom defining the build files.

        Args:
            name: the name of atom.
            filename: the relative filename to write the metadata file.
        """
        self.build_files.sort()
        data = {'name': name, 'type': 'documentation', 'docs': self.build_files}

        with open(self.dest(filename), 'w') as output:
            json.dump(
                data, output, indent=2, sort_keys=True, separators=(',', ': '))

    def write_atom_metadata(self, filename, atom):
        full_name = self.dest(filename)
        if not os.path.exists(os.path.dirname(full_name)):
            os.makedirs(os.path.dirname(full_name))

        with open(full_name, 'w') as atom_metadata:
            json.dump(
                atom,
                atom_metadata,
                indent=2,
                sort_keys=True,
                separators=(',', ': '))

    def _install_ffx_tool_atom_files(self, root, collections):
        """Copies the file collections (generic or target specific) in an
        ffx_tool atom.

        The executable and executable_metadata 'collections' in an ffx_tool
        atom are single files, while any other collection is a list of
        additional files.
        """
        for collection, files in collections.items():
            if collection == "executable" or collection == "executable_metadata":
                # These are always expected to be a single file, so 'files'
                # is actually a single file.
                self.copy_file(files, root, 'tools')
            else:
                # Anything else is expected to be multiple files, so we copy
                # them in plural.
                self.copy_files(files, root, 'tools')

    # Handlers for SDK atoms

    def install_documentation_atom(self, atom):
        self.copy_files(atom['docs'])

    def install_version_history_atom(self, atom, manifest):
        self.write_atom_metadata(manifest['meta'], atom)

    def install_cc_prebuilt_library_atom(self, atom):
        name = atom['name']
        # Add atom to test targets
        self.cc_prebuilt_targets.append(name)
        base = self.dest('pkg', name)
        library = model.CppPrebuiltLibrary(name)
        library.relative_path_to_root = os.path.relpath(self.output, start=base)
        library.is_static = atom['format'] == 'static'

        self.copy_files(atom['headers'], atom['root'], base, library.hdrs)

        if 'ifs' in atom:
            self.copy_file(
                os.path.join(atom['root'], atom['ifs']), atom['root'], base)

        for arch in self.target_arches:
            binaries = atom['binaries'][arch]
            prebuilt_set = model.CppPrebuiltSet(binaries['link'])
            self.copy_file(binaries['link'])

            if 'dist' in binaries:
                prebuilt_set.dist_lib = binaries['dist']
                prebuilt_set.dist_path = binaries['dist_path']
                self.copy_file(binaries['dist'])

            if 'debug' in binaries:
                self.copy_file(binaries['debug'])

            library.prebuilts[arch] = prebuilt_set

        for dep in atom['deps']:
            library.deps.append('../' + dep)

        library.includes = os.path.relpath(atom['include_dir'], atom['root'])

        if not os.path.exists(base):
            os.makedirs(base)
        self.write_file(
            os.path.join(base, 'BUILD.gn'), 'cc_prebuilt_library', library)
        self.write_atom_metadata(os.path.join(base, 'meta.json'), atom)
        self.build_files.append(
            os.path.relpath(os.path.join(base, 'BUILD.gn'), self.output))

    def install_cc_source_library_atom(self, atom):
        name = atom['name']
        # Add atom to test targets
        self.cc_source_targets.append(name)
        base = self.dest('pkg', name)
        library = model.CppSourceLibrary(name)
        library.relative_path_to_root = os.path.relpath(self.output, start=base)

        self.copy_files(atom['headers'], atom['root'], base, library.hdrs)
        self.copy_files(atom['sources'], atom['root'], base, library.srcs)

        for dep in atom['deps']:
            library.deps.append('../' + dep)

        for dep in atom['fidl_deps']:
            dep_name = dep
            library.deps.append('../../fidl/' + dep_name)

        for layer in atom['fidl_binding_deps']:
            for dep in layer['deps']:
                library.deps.append(
                    '../../fidl/{}:{}_{}'.format(
                        dep, dep, layer['binding_type']))

        library.includes = os.path.relpath(atom['include_dir'], atom['root'])

        self.write_file(os.path.join(base, 'BUILD.gn'), 'cc_library', library)
        self.write_atom_metadata(os.path.join(base, 'meta.json'), atom)
        self.build_files.append(
            os.path.relpath(os.path.join(base, 'BUILD.gn'), self.output))

    def install_companion_host_tool_atom(self, atom):
        self.install_host_tool_atom(atom)

    def install_fidl_library_atom(self, atom):
        name = atom['name']
        base = self.dest('fidl', name)
        data = model.FidlLibrary(name, atom['name'])
        data.relative_path_to_root = os.path.relpath(self.output, start=base)
        data.short_name = name.split('.')[-1]
        data.namespace = '.'.join(name.split('.')[0:-1])

        self.copy_files(atom['sources'], atom['root'], base, data.srcs)
        for dep in atom['deps']:
            data.deps.append(dep)

        self.write_file(os.path.join(base, 'BUILD.gn'), 'fidl_library', data)
        self.fidl_targets.append(name)
        self.write_atom_metadata(os.path.join(base, 'meta.json'), atom)
        self.build_files.append(
            os.path.relpath(os.path.join(base, 'BUILD.gn'), self.output))

    def install_host_tool_atom(self, atom):
        if 'files' in atom:
            self.copy_files(atom['files'], atom['root'], 'tools')
        if 'target_files' in atom:
            for files in atom['target_files'].values():
                self.copy_files(files, atom['root'], 'tools')

    def install_ffx_tool_atom(self, atom):
        if 'files' in atom:
            self._install_ffx_tool_atom_files(atom['root'], atom['files'])
        if 'target_files' in atom:
            for files in atom['target_files'].values():
                self._install_ffx_tool_atom_files(atom['root'], files)

    def install_loadable_module_atom(self, atom):
        name = atom['name']
        if name != 'vulkan_layers':
            raise RuntimeError('Unsupported loadable_module: %s' % name)

        # Add loadable modules to test targets
        self.loadable_module_targets.append(name)

        # Copy resources and binaries
        resources = atom['resources']
        self.copy_files(resources)

        binaries = atom['binaries']
        for arch in self.target_arches:
            self.copy_files(binaries[arch])

        def _filename_no_ext(name):
            return os.path.splitext(os.path.basename(name))[0]

        data = model.VulkanLibrary()
        # Pair each json resource with its corresponding binary. Each such pair
        # is a "layer". We only need to check one arch because each arch has the
        # same list of binaries.
        arch = next(iter(binaries))
        binary_names = binaries[arch]
        # TODO(jdkoren): use atom[root] once fxr/360094 lands
        local_pkg = os.path.join('pkg', name)

        for res in resources:
            layer_name = _filename_no_ext(res)

            # Filter binaries for a matching name.
            filtered = [
                n for n in binary_names if _filename_no_ext(n) == layer_name
            ]

            if not filtered:
                # If the binary could not be found then do not generate a
                # target for this layer. The missing targets will cause a
                # mismatch with the "golden" outputs.
                continue

            # Replace harcoded arch in the found binary filename.
            binary = filtered[0].replace('/' + arch + '/', "/${target_cpu}/")

            layer = model.VulkanLayer(
                name=layer_name,
                config=os.path.relpath(res, start=local_pkg),
                binary=os.path.relpath(binary, start=local_pkg))
            # Special case: VkLayer_image_pipe_swapchain has an undocumented
            # data_dep on trace-engine. Currently atom metadata does not
            # contain this information (fxbug.dev/46250).
            if layer_name == 'VkLayer_image_pipe_swapchain':
                layer.data_deps.append('../trace-engine')

            data.layers.append(layer)

        base = self.dest(local_pkg)
        self.write_file(os.path.join(base, 'BUILD.gn'), 'vulkan_module', data)
        self.build_files.append(
            os.path.relpath(os.path.join(base, 'BUILD.gn'), self.output))
        self.write_atom_metadata(os.path.join(base, 'meta.json'), atom)

    def install_data_atom(self, atom):
        # Don't copy the metadata as that is handled by update_metadata since
        # the metadata is named inconsistently across all data types:
        # config, license, and component_manifest.
        self.copy_files(atom['data'])

    def install_sysroot_atom(self, atom):
        base = self.dest('pkg', 'sysroot')
        pkg_sysroot = os.path.join('pkg', 'sysroot')
        for ifs_file in atom['ifs_files']:
            self.copy_file(
                os.path.join(pkg_sysroot, ifs_file), pkg_sysroot, base)
        for arch in self.target_arches:
            base = self.dest('arch', arch, 'sysroot')
            arch_data = atom['versions'][arch]
            arch_root = arch_data['root']
            self.copy_files(arch_data['headers'], arch_root, base)
            self.copy_files(arch_data['link_libs'], arch_root, base)
            # We maintain debug files in their original location.
            self.copy_files(arch_data['debug_libs'])
            # Files in dist_libs are required for toolchain config. They are the
            # same for all architectures and shouldn't change names, so we
            # hardcode those names in build/config/BUILD.gn. This may need to
            # change if/when we support sanitized builds.
            self.copy_files(arch_data['dist_libs'], arch_root, base)
        self.write_atom_metadata(self.dest('pkg', 'sysroot', 'meta.json'), atom)


class TestData(object):
    """Class representing test data to be added to the run_py mako template"""

    def __init__(self):
        self.fuchsia_root = FUCHSIA_ROOT


def make_executable(path):
    st = os.stat(path)
    os.chmod(path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def get_prebuilts(in_extra_prebuilts, jiri_manifest):
    """ Lookup version strings for a set of prebuilts

    Given a file such as $FUCHSIA_ROOT/.jiri_root/update_history/latest, find matches for each of the name
    strings, capture the version string, and return back a dictionary with the mappings.

    Args:
        in_extra_prebuilts: Dictionary of name strings to search, and the corresponding short name
        jiri_manifest: XML file to read, such as $FUCHSIA_ROOT/.jiri_root/update_history/latest

    Returns:
        Dictionary of short name to version mappings

    Raises:
        RuntimeError: If in_extra_prebuilts contains items not found in the prebuilts file
        or the jiri manifest can't be parsed
    """

    # Copy the input list since we modify it in the loop
    extra_prebuilts = in_extra_prebuilts.copy()
    prebuilt_results = {}
    try:
        manifest_root = xml.etree.ElementTree.parse(jiri_manifest).getroot()
    except Exception as e:
        raise RuntimeError(
            'Unable to parse jiri manifest at %s: %s' % (jiri_manifest, e))
    remaining_prebuilts = extra_prebuilts
    for packages in manifest_root.iter('package'):
        prebuilt = remaining_prebuilts.pop(packages.attrib['name'], None)
        if prebuilt:
            prebuilt_results[prebuilt] = packages.attrib['version']
    if remaining_prebuilts:
        raise RuntimeError(
            'Could not find entries in %s for the remaining EXTRA_PREBUILTS: %s'
            % (jiri_manifest, remaining_prebuilts))
    return prebuilt_results


def create_test_workspace(output):
    # Remove any existing output.
    shutil.rmtree(output, True)
    # Copy the base tests.
    copy_tree(os.path.join(SCRIPT_DIR, 'test_project'), output)
    # run.py file
    builder = Frontend(local_dir=SCRIPT_DIR)
    run_py_path = os.path.join(output, 'run.py')
    builder.write_file(
        path=run_py_path, template_name='run_py', data=TestData())
    make_executable(run_py_path)

    return True


def create_archive(output_archive, output):
    # If file already exists remove it
    if os.path.exists(output_archive):
        os.remove(output_archive)
    # If GN SDK directory is empty throw an error
    if not os.path.exists(output):
        return False
    # Create GN SDK archive
    if os.path.isfile(PIGZ_PATH):
        temp_tar = output_archive + ".tmp.tar"
        with tarfile.open(temp_tar, "w") as archive_file:
            archive_file.add(output, arcname='')
        subprocess.run([PIGZ_PATH, temp_tar, "-9"])
        os.rename(temp_tar + ".gz", output_archive)
    else:
        with tarfile.open(output_archive, "w:gz") as archive_file:
            archive_file.add(output, arcname='')

    return True


def main(args_list=None):
    parser = argparse.ArgumentParser(
        description='Creates a GN SDK for a given SDK tarball.')
    source_group = parser.add_mutually_exclusive_group(required=True)
    source_group.add_argument(
        '--archive', help='Path to the SDK archive to ingest', default='')
    source_group.add_argument(
        '--directory', help='Path to the SDK directory to ingest', default='')
    parser.add_argument(
        '--output',
        help='Path to the directory where to install the SDK',
        required=True)
    parser.add_argument(
        '--output-archive',
        help='Path to add the SDK archive file (e.g. "path/to/gn.tar.gz")')
    parser.add_argument(
        '--output-archive-only',
        help=
        'Whether to output the SDK archive and skip installing to a directory',
        default=False,
    )
    parser.add_argument(
        '--tests', help='Path to the directory where to generate tests')
    parser.add_argument(
        '--jiri-manifest',
        help=
        'File containing external references in XML format (e.g. ".jiri_root/update_history/latest")'
    )
    if args_list:
        args = parser.parse_args(args_list)
    else:
        args = parser.parse_args()

    return run_generator(
        archive=args.archive,
        directory=args.directory,
        output=args.output,
        tests=args.tests,
        output_archive=args.output_archive,
        output_archive_only=args.output_archive_only,
        jiri_manifest=args.jiri_manifest)


def run_generator(
        archive,
        directory,
        output,
        output_archive_only=False,
        tests='',
        output_archive='',
        jiri_manifest=None):
    """Run the generator. Returns 0 on success, non-zero otherwise.

    Note that only one of archive or directory should be specified.

    Args:
        archive: Path to an SDK tarball archive.
        directory: Path to an unpackaged SDK.
        output: The output directory.
        tests: The directory where to generate tests. Defaults to empty string.
    """

    # Remove any existing output.
    if os.path.exists(output):
        shutil.rmtree(output)

    builder = GNBuilder(
        archive=archive,
        directory=directory,
        output=output,
        local_dir=SCRIPT_DIR,
        jiri_manifest=jiri_manifest)
    if not builder.run():
        return 1

    if tests:
        # Create the tests workspace
        if not create_test_workspace(tests):
            return 1
        # Copy the GN SDK to the test workspace
        wrkspc_sdk_dir = os.path.join(tests, 'third_party', 'fuchsia-sdk')
        copy_tree(output, wrkspc_sdk_dir)

        # Copy test metadata to the workspace. meta/manifest.json tells the
        # emulator tools where the SDK for integration testing is located.
        # This must be done after the GN SDK is copied into the test workspace,
        # which would otherwise overwrite these contents.
        testdata = os.path.join(SCRIPT_DIR, 'testdata')
        copy_tree(
            os.path.join(testdata, 'meta'),
            os.path.join(wrkspc_sdk_dir, 'meta'))

    if output_archive:
        if not create_archive(output_archive, output):
            return 1

    if output_archive_only:
        shutil.rmtree(output)

    return 0


if __name__ == '__main__':
    sys.exit(main())
