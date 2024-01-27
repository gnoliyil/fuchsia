#!/usr/bin/env python3
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Update or generate Bazel workspace for the platform build.

The script first checks whether the Ninja build plan needs to be updated.
After that, it checks whether the Bazel workspace used by the platform
build, and associatedd files (e.g. bazel launcher script) and
directories (e.g. output_base) need to be updated. It simply exits
if no update is needed, otherwise, it will regenerate everything
appropriately.

The TOPDIR directory argument will be populated with the following
files:

  $TOPDIR/
    bazel                   Bazel launcher script.
    generated-info.json     State of inputs during last generation.
    output_base/            Bazel output base.
    output_user_root/       Baze output user root.
    workspace/              Bazel workspace directory.

The workspace/ sub-directory will be populated with symlinks
mirroring the top FUCHSIA_DIR directory, except for the 'out'
sub-directory and a few other files. It will also include a few
generated files (e.g. `.bazelrc`).

The script tracks the file and sub-directory entries of $FUCHSIA_DIR,
and is capable of updating the workspace if new ones are added, or
old ones are removed.
"""

import argparse
import difflib
import errno
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys


def get_host_platform() -> str:
    '''Return host platform name, following Fuchsia conventions.'''
    if sys.platform == 'linux':
        return 'linux'
    elif sys.platform == 'darwin':
        return 'mac'
    else:
        return os.uname().sysname


def get_host_arch() -> str:
    '''Return host CPU architecture, following Fuchsia conventions.'''
    host_arch = os.uname().machine
    if host_arch == 'x86_64':
        return 'x64'
    elif host_arch.startswith(('armv8', 'aarch64')):
        return 'arm64'
    else:
        return host_arch


def get_host_tag() -> str:
    '''Return host tag, following Fuchsia conventions.'''
    return '%s-%s' % (get_host_platform(), get_host_arch())


def force_symlink(target_path: str, dst_path: str):
    '''Create a symlink at |dst_path| that points to |target_path|.'''
    dst_dir = os.path.dirname(dst_path)
    os.makedirs(dst_dir, exist_ok=True)
    target_path = os.path.relpath(target_path, dst_dir)
    try:
        os.symlink(target_path, dst_path)
    except OSError as e:
        if e.errno == errno.EEXIST:
            os.remove(dst_path)
            os.symlink(target_path, dst_path)
        else:
            raise


def make_removeable(path: str):
    '''Ensure the file at |path| is removeable.'''

    islink = os.path.islink(path)

    # Skip if the input path is a symlink, and chmod with
    # `follow_symlinks=False` is not supported. Linux is the most notable
    # platform that meets this requirement, and adding S_IWUSR is not necessary
    # for removing symlinks.
    if islink and (os.chmod not in os.supports_follow_symlinks):
        return

    info = os.stat(path, follow_symlinks=False)
    if info.st_mode & stat.S_IWUSR == 0:
        try:
            if islink:
                os.chmod(
                    path, info.st_mode | stat.S_IWUSR, follow_symlinks=False)
            else:
                os.chmod(path, info.st_mode | stat.S_IWUSR)
        except Exception as e:
            raise RuntimeError(
                f'Failed to chmod +w to {path}, islink: {islink}, info: {info}, error: {e}'
            )


def remove_dir(path):
    '''Properly remove a directory.'''
    # shutil.rmtree() does not work well when there are readonly symlinks to
    # directories. This results in weird NotADirectory error when trying to
    # call os.scandir() or os.rmdir() on them (which happens internally).
    #
    # Re-implement it correctly here. This is not as secure as it could
    # (see "shutil.rmtree symlink attack"), but is sufficient for the Fuchsia
    # build.
    all_files = []
    all_dirs = []
    for root, subdirs, files in os.walk(path):
        # subdirs may contain actual symlinks which should be treated as
        # files here.
        real_subdirs = []
        for subdir in subdirs:
            if os.path.islink(os.path.join(root, subdir)):
                files.append(subdir)
            else:
                real_subdirs.append(subdir)

        for file in files:
            file_path = os.path.join(root, file)
            all_files.append(file_path)
            make_removeable(file_path)
        for subdir in real_subdirs:
            dir_path = os.path.join(root, subdir)
            all_dirs.append(dir_path)
            make_removeable(dir_path)

    for file in reversed(all_files):
        os.remove(file)
    for dir in reversed(all_dirs):
        os.rmdir(dir)
    os.rmdir(path)


def create_clean_dir(path):
    '''Create a clean directory.'''
    if os.path.exists(path):
        remove_dir(path)
    os.makedirs(path)


def get_fx_build_dir(fuchsia_dir):
    """Return the path to the Ninja build directory set through 'fx set'.

    Args:
      fuchsia_dir: The Fuchsia top-level source directory.
    Returns:
      The path to the Ninja build directory, or None if it could not be
      determined, which happens on infra bots which do not use 'fx set'
      but 'fint set' directly.
    """
    fx_build_dir_path = os.path.join(fuchsia_dir, '.fx-build-dir')
    if not os.path.exists(fx_build_dir_path):
        return None

    with open(fx_build_dir_path) as f:
        build_dir = f.read().strip()
        return os.path.join(fuchsia_dir, build_dir)


def get_reclient_config(fuchsia_dir):
    """Return reclient configuration."""
    reclient_config_path = os.path.join(
        fuchsia_dir, 'build', 'rbe', 'fuchsia-re-client.cfg')

    instance_prefix = "instance="
    container_image_prefix = "platform=container-image="

    instance_name = None
    container_image = None

    with open(reclient_config_path) as f:
        for line in f:
            line = line.strip()
            if line.startswith(instance_prefix):
                instance_name = line[len(instance_prefix):]
            elif line.startswith(container_image_prefix):
                container_image = line[len(container_image_prefix):]

    if not instance_name:
        print(
            'ERROR: Missing instance name from %s' % config_file_path,
            file=sys.stderr)
        sys.exit(1)
    if not container_image:
        print(
            'ERROR: Missing container image name from %s' % config_file_path,
            file=sys.stderr)
        sys.exit(1)

    return {
        "instance_name": instance_name,
        "container_image": container_image,
    }


def generate_fuchsia_build_config(fuchsia_dir):
    """Generate a dictionary used for the fuchsia_build_config.bzl template."""
    rbe_config = get_reclient_config(fuchsia_dir)
    host_os = get_host_platform()
    host_arch = get_host_arch()
    host_tag = get_host_tag()

    host_target_triple = {
        "linux-x64": "x86_64-unknown-linux-gnu",
        "linux-arm64": "aarch64-unknown-linux-gnu",
        "mac-x64": "x86_64-apple-darwin",
        "mac-arm64": "aarch64-apple-darwin",
    }.get(host_tag)

    host_os_constraint = {
        "linux": "@platforms//os:linux",
        "mac": "@platforms//os:macos",
    }.get(host_os)

    host_cpu_constraint = {
        "x64": "@platforms//cpu:x86_64",
        "arm64": "@platforms//cpu:aarch64",
    }.get(host_arch)

    rbe_instance_name = rbe_config.get('instance_name', '')
    rbe_project = rbe_instance_name.split('/')[1]
    return {
        'host_os': host_os,
        'host_arch': host_arch,
        'host_tag': host_tag,
        'host_tag_alt': host_tag.replace('-', '_'),
        'host_target_triple': host_target_triple,
        'host_os_constraint': host_os_constraint,
        "host_cpu_constraint": host_cpu_constraint,
        'rbe_instance_name': rbe_instance_name,
        'rbe_container_image': rbe_config.get('container_image', ''),
        'rbe_project': rbe_project,
    }


def md5_all_files(paths):
    h = hashlib.new('md5')
    for p in paths:
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()


def all_sdk_metas(sdk_root):
    sdk_manifest_path = os.path.join(sdk_root, 'meta', 'manifest.json')
    if not os.path.exists(sdk_manifest_path):
        # The manifest does not exist yet, which happens when this script
        # is called before the SDK has been built.
        return []
    with open(sdk_manifest_path, 'r') as f:
        sdk_manifest = json.load(f)
    part_metas = [
        os.path.join(sdk_root, part["meta"]) for part in sdk_manifest["parts"]
    ]
    return [sdk_manifest_path] + part_metas


class GeneratedFiles(object):
    """Models the content of a generated Bazel workspace."""

    def __init__(self, files={}):
        self._files = files

    def _check_new_path(self, path):
        assert path not in self._files, (
            'File entry already in generated list: ' + path)

    def add_symlink(self, dst_path, target_path):
        self._check_new_path(dst_path)
        self._files[dst_path] = {
            "type": "symlink",
            "target": target_path,
        }

    def add_file(self, dst_path, content, executable=False):
        self._check_new_path(dst_path)
        entry = {
            "type": "file",
            "content": content,
        }
        if executable:
            entry["executable"] = True
        self._files[dst_path] = entry

    def add_file_hash(self, dst_path):
        self._check_new_path(dst_path)
        self._files[dst_path] = {
            "type": "md5",
            "hash": md5_all_files([dst_path]),
        }

    def add_top_entries(self, fuchsia_dir, subdir, excluded_file):
        for name in os.listdir(fuchsia_dir):
            if not excluded_file(name):
                self.add_symlink(
                    os.path.join(subdir, name), os.path.join(fuchsia_dir, name))

    def to_json(self):
        """Convert to JSON file."""
        return json.dumps(self._files, indent=2, sort_keys=True)

    def write(self, out_dir):
        """Write workspace content to directory."""
        for path, entry in self._files.items():
            type = entry["type"]
            if type == "symlink":
                target_path = entry["target"]
                link_path = os.path.join(out_dir, path)
                force_symlink(target_path, link_path)
            elif type == "file":
                file_path = os.path.join(out_dir, path)
                with open(file_path, 'w') as f:
                    f.write(entry["content"])
                if entry.get("executable", False):
                    os.chmod(file_path, 0o755)
            elif type == 'md5':
                # Nothing to do here.
                pass
            else:
                assert False, 'Unknown entry type: ' % entry["type"]


def maybe_regenerate_ninja(gn_output_dir, ninja):
    '''Regenerate Ninja build plan if needed, returns True on update.'''
    # This reads the build.ninja.d directly and tries to stat() all
    # dependencies in it directly (around 7000+), which is much
    # faster than Ninja trying to stat all build graph paths!
    build_ninja_d = os.path.join(gn_output_dir, 'build.ninja.d')
    if not os.path.exists(build_ninja_d):
        return False

    with open(build_ninja_d) as f:
        build_ninja_deps = f.read().split(' ')

    assert len(build_ninja_deps) > 1
    ninja_stamp = os.path.join(gn_output_dir, build_ninja_deps[0][:-1])
    ninja_stamp_timestamp = os.stat(ninja_stamp).st_mtime

    try:
        for dep in build_ninja_deps[1:]:
            dep_path = os.path.join(gn_output_dir, dep)
            dep_timestamp = os.stat(dep_path).st_mtime
            if dep_timestamp > ninja_stamp_timestamp:
                return True
    except FileNotFoundError:
        return True

    return False


_VALID_TARGET_CPUS = ('arm64', 'x64')


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument(
        '--fuchsia-dir',
        help='Path to the Fuchsia source tree, auto-detected by default.')
    parser.add_argument(
        '--gn_output_dir',
        help='GN output directory, auto-detected by default.')
    parser.add_argument(
        '--target_arch',
        help='Equivalent to `target_cpu` in GN. Defaults to args.gn setting.',
        choices=_VALID_TARGET_CPUS)
    parser.add_argument(
        '--bazel-bin',
        help=
        'Path to bazel binary, defaults to $FUCHSIA_DIR/prebuilt/third_party/bazel/${host_platform}/bazel'
    )
    parser.add_argument(
        '--topdir',
        help='Top output directory. Defaults to GN_OUTPUT_DIR/gen/build/bazel')
    parser.add_argument(
        '--use-bzlmod',
        action='store_true',
        help='Use BzlMod to generate external repositories.')
    parser.add_argument(
        '--verbose', action='count', default=1, help='Increase verbosity')
    parser.add_argument(
        '--quiet', action='count', default=0, help='Reduce verbosity')
    parser.add_argument(
        '--force',
        action='store_true',
        help='Force workspace regeneration, by default this only happens ' +
        'the script determines there is a need for it.')
    parser.add_argument(
        '--depfile',
        type=argparse.FileType('w'),
        help='If set, write a depfile at this path')
    args = parser.parse_args()

    verbosity = args.verbose - args.quiet

    if not args.fuchsia_dir:
        # Assume this script is in 'build/bazel/scripts/'
        # //build/bazel:generate_main_workspace always sets this argument,
        # this feature is a convenience when calling this script manually
        # during platform build development and debugging.
        args.fuchsia_dir = os.path.join(
            os.path.dirname(__file__), '..', '..', '..')

    fuchsia_dir = os.path.abspath(args.fuchsia_dir)

    if not args.gn_output_dir:
        args.gn_output_dir = get_fx_build_dir(fuchsia_dir)
        if not args.gn_output_dir:
            parser.error(
                'Could not auto-detect build directory, please use --gn_output_dir=DIR'
            )
            return 1

    gn_output_dir = os.path.abspath(args.gn_output_dir)

    if not args.target_arch:
        # Extract default target architecture from args.json file, which is
        # created after calling `gn gen`. If the file is missing print an error
        args_json_path = os.path.join(args.gn_output_dir, 'args.json')
        if not os.path.exists(args_json_path):
            parser.error(
                f'Cannot determine target architecture ({args_json_path} is missing). Please use --target-arch=ARCH'
            )
        with open(args_json_path) as f:
            args_json = json.load(f)
            target_cpu = args_json.get('target_cpu', None)
            if target_cpu not in _VALID_TARGET_CPUS:
                parser.error(
                    f'Unsupported target cpu value "{target_cpu}" from {args_json_path}'
                )
            args.target_arch = target_cpu

    if not args.topdir:
        args.topdir = os.path.join(gn_output_dir, 'gen/build/bazel')

    topdir = os.path.abspath(args.topdir)

    logs_dir = os.path.join(topdir, 'logs')

    build_config = generate_fuchsia_build_config(fuchsia_dir)

    host_tag = build_config['host_tag']
    host_tag_alt = host_tag.replace('-', '_')

    ninja_binary = os.path.join(
        fuchsia_dir, 'prebuilt', 'third_party', 'ninja', host_tag, 'ninja')

    python_prebuilt_dir = os.path.join(
        fuchsia_dir, 'prebuilt', 'third_party', 'python3', host_tag)

    output_base_dir = os.path.abspath(os.path.join(topdir, 'output_base'))
    output_user_root = os.path.abspath(os.path.join(topdir, 'output_user_root'))
    workspace_dir = os.path.abspath(os.path.join(topdir, 'workspace'))

    if not args.bazel_bin:
        args.bazel_bin = os.path.join(
            fuchsia_dir, 'prebuilt', 'third_party', 'bazel', host_tag, 'bazel')

    bazel_bin = os.path.abspath(args.bazel_bin)

    bazel_launcher = os.path.abspath(os.path.join(topdir, 'bazel'))

    def log(message, level=1):
        if verbosity >= level:
            print(message)

    def log2(message):
        log(message, 2)

    log2(
        '''Using directories and files:
  Fuchsia:                {}
  GN build:               {}
  Ninja binary:           {}
  Bazel source:           {}
  Topdir:                 {}
  Logs directory:         {}
  Bazel workspace:        {}
  Bazel output_base:      {}
  Bazel output user root: {}
  Bazel launcher:         {}
'''.format(
            fuchsia_dir, gn_output_dir, ninja_binary, bazel_bin, topdir,
            logs_dir, workspace_dir, output_base_dir, output_user_root,
            bazel_launcher))

    if maybe_regenerate_ninja(gn_output_dir, ninja_binary):
        log('Re-generating Ninja build plan!')
        subprocess.run([ninja_binary, '-C', gn_output_dir, 'build.ninja'])
    else:
        log2('Ninja build plan up to date.')

    generated = GeneratedFiles()

    def expand_template_file(filename: str, **kwargs) -> str:
        """Expand a template file and add it to the set of tracked input files."""
        template_file = os.path.join(templates_dir, filename)
        generated.add_file_hash(os.path.abspath(template_file))
        with open(template_file) as f:
            return f.read().format(**kwargs)

    def write_workspace_file(path, content):
        generated.add_file('workspace/' + path, content)

    def create_workspace_symlink(path, target_path):
        generated.add_symlink('workspace/' + path, target_path)

    script_path = os.path.relpath(__file__, fuchsia_dir)

    templates_dir = os.path.join(fuchsia_dir, 'build', 'bazel', 'templates')

    if args.use_bzlmod:
        generated.add_file(
            'workspace/WORKSPACE.bazel',
            '# Empty on purpose, see MODULE.bazel\n')

        generated.add_symlink(
            'workspace/WORKSPACE.bzlmod',
            os.path.join(
                fuchsia_dir, 'build', 'bazel', 'toplevel.WORKSPACE.bzlmod'))

        generated.add_symlink(
            'workspace/MODULE.bazel',
            os.path.join(
                fuchsia_dir, 'build', 'bazel', 'toplevel.MODULE.bazel'))
    else:
        workspace_content = expand_template_file(
            'template.WORKSPACE.bazel',
            host_os=build_config['host_os'],
            host_tag=build_config['host_tag'],
            host_tag_alt=build_config['host_tag_alt'],
            ninja_output_dir=os.path.relpath(gn_output_dir, workspace_dir),
        )
        generated.add_file(
            'workspace/WORKSPACE.bazel',
            workspace_content,
        )

    # Generate symlinks

    def excluded_file(path):
        """Return true if a file path must be excluded from the symlink list."""
        # Never symlink to the 'out' directory.
        if path == "out":
            return True
        # Don't symlink the Jiri files, this can confuse Jiri during an 'jiri update'
        # Don't symlink the .fx directory (TODO(digit): I don't remember why?)
        # Don´t symlink the .git directory as well, since it needs to be handled separately.
        if path.startswith(('.jiri', '.fx', '.git')):
            return True
        return False

    generated.add_top_entries(fuchsia_dir, 'workspace', excluded_file)

    generated.add_symlink(
        'workspace/BUILD.bazel',
        os.path.join(fuchsia_dir, 'build', 'bazel', 'toplevel.BUILD.bazel'))

    # The top-level .git directory must be symlinked because some actions actually
    # launch git commands (e.g. to generate a build version identifier). On the other
    # hand Jiri will complain if it finds a .git repository with Jiri metadata that
    # it doesn't know about in its manifest. The error looks like:
    #
    # ```
    # [17:49:48.200] WARN: Project "fuchsia" has path /work/fx-bazel-build, but was found in /work/fx-bazel-build/out/default/gen/build/bazel/output_base/execroot/main.
    # jiri will treat it as a stale project. To remove this warning please delete this or move it out of your root folder
    # ```
    #
    # Looking at the Jiri sources reveals that it is looking at a `.git/jiri` sub-directory
    # in all git directories it finds during a `jiri update` operation. To avoid the complaint
    # then symlink all $FUCHSIA_DIR/.git/ files and directories, except the 'jiri' one.
    # Also ignore the JIRI_HEAD / JIRI_LAST_BASE files to avoid confusion.
    fuchsia_git_dir = os.path.join(fuchsia_dir, '.git')
    for git_file in os.listdir(fuchsia_git_dir):
        if not (git_file == 'jiri' or git_file.startswith('JIRI')):
            generated.add_symlink(
                'workspace/.git/' + git_file,
                os.path.join(fuchsia_git_dir, git_file))

    # Generate a DownloaderUrlRewriter configuration file.
    # See https://cs.opensource.google/bazel/bazel/+/master:src/main/java/com/google/devtools/build/lib/bazel/repository/downloader/UrlRewriterConfig.java;drc=63bc1c7d0853dc187e4b96a490d733fb29f79664;l=31
    download_config = '''# Auto-generated - DO NOT EDIT!
all_blocked_message Repository downloads are forbidden for Fuchsia platform builds
block *
'''
    generated.add_file(
        'download_config_file',
        download_config,
    )

    # Extract remote instance name from reclient configuration file.
    # and store it in a .bzl file in the workspace.
    fuchsia_build_config_content = expand_template_file(
        'template.fuchsia_build_config.bzl', **build_config)
    generated.add_file(
        'workspace/fuchsia_build_config.bzl', fuchsia_build_config_content)

    # Generate a platform mapping file to ensure that using --platforms=<value>
    # also sets --cpu properly, as required by the Bazel SDK rules. See comments
    # in template file for more details.
    _BAZEL_CPU_MAP = {'x64': 'k8', 'arm64': 'aarch64'}
    host_os = get_host_platform()
    host_cpu = get_host_arch()
    platform_mappings_content = expand_template_file(
        'template.platform_mappings',
        host_os=host_os,
        host_cpu=host_cpu,
        bazel_host_cpu=_BAZEL_CPU_MAP[host_cpu],
    )
    generated.add_file('workspace/platform_mappings', platform_mappings_content)

    platform_version_path = os.path.join(
        fuchsia_dir, 'build', 'config', 'fuchsia', 'platform_version.json')
    with open(platform_version_path, 'r') as f:
        platform_version = json.load(f)
    # Generate the content of .bazelrc
    bazelrc_content = expand_template_file(
        'template.bazelrc',
        default_platform=f'fuchsia_{args.target_arch}',
        host_platform=host_tag_alt,
        log_file=os.path.join(logs_dir, 'workspace-events.log'),
        config_file=os.path.join(topdir, 'download_config_file'),
        remote_instance_name=build_config['rbe_instance_name'],
        rbe_project=build_config['rbe_project'],
        fuchsia_api_level=platform_version['in_development_api_level'],
    )
    if args.use_bzlmod:
        bazelrc_content += '''
# Enable BlzMod, i.e. support for MODULE.bazel files.
common --experimental_enable_bzlmod
'''
    generated.add_file('workspace/.bazelrc', bazelrc_content)

    # Create a symlink to the GN-generated file that will contain the list
    # of @legacy_ninja_build_outputs entries. This file is generated by the
    # GN target //build/bazel:legacy_ninja_build_outputs.
    generated.add_symlink(
        'workspace/bazel_inputs_manifest.json',
        os.path.join(
            workspace_dir, '..',
            'legacy_ninja_build_outputs.inputs_manifest.json'))

    # Generate wrapper script in topdir/bazel that invokes Bazel with the right --output_base.
    bazel_launcher_content = expand_template_file(
        'template.bazel.sh',
        ninja_output_dir=os.path.abspath(gn_output_dir),
        ninja_prebuilt=os.path.abspath(ninja_binary),
        bazel_bin_path=os.path.abspath(bazel_bin),
        logs_dir=os.path.abspath(logs_dir),
        python_prebuilt_dir=os.path.abspath(python_prebuilt_dir),
        download_config_file='download_config_file',
        workspace=os.path.relpath(workspace_dir, topdir),
        output_base=os.path.relpath(output_base_dir, topdir),
        output_user_root=os.path.relpath(output_user_root, topdir),
    )
    generated.add_file('bazel', bazel_launcher_content, executable=True)

    # Ensure regeneration when this script's content changes!
    generated.add_file_hash(os.path.abspath(__file__))

    # Create hash files to capture current state of locally generated SDKs.
    sdk_root = os.path.join(gn_output_dir, 'sdk', 'exported')

    all_core_sdk_metas = all_sdk_metas(os.path.join(sdk_root, 'core'))
    fuchsia_sdk_hash = os.path.join('workspace', 'fuchsia_sdk_hash')
    generated.add_file(fuchsia_sdk_hash, md5_all_files(all_core_sdk_metas))
    if args.depfile:
        out = os.path.relpath(
            os.path.join(topdir, fuchsia_sdk_hash), gn_output_dir)
        ins = ' '.join(
            os.path.relpath(p, gn_output_dir) for p in all_core_sdk_metas)
        args.depfile.write(f'{out}: {ins}\n')

    all_internal_part_metas = all_sdk_metas(os.path.join(sdk_root, 'platform'))
    internal_sdk_hash = os.path.join('workspace', 'internal_sdk_hash')
    generated.add_file(
        internal_sdk_hash, md5_all_files(all_internal_part_metas))
    if args.depfile:
        out = os.path.relpath(
            os.path.join(topdir, internal_sdk_hash), gn_output_dir)
        ins = ' '.join(
            os.path.relpath(p, gn_output_dir) for p in all_internal_part_metas)
        args.depfile.write(f'{out}: {ins}\n')

    force = args.force
    generated_json = generated.to_json()
    generated_info_file = os.path.join(topdir, 'generated-info.json')
    if not os.path.exists(generated_info_file):
        log2("Missing file: " + generated_info_file)
        force = True
    elif not os.path.isdir(workspace_dir):
        log2("Missing directory: " + workspace_dir)
        force = True
    elif not os.path.isdir(output_base_dir):
        log2("Missing directory: " + output_base_dir)
        force = True
    else:
        with open(generated_info_file) as f:
            existing_info = f.read()

        if existing_info != generated_json:
            log2("Changes in %s" % (generated_info_file))
            if verbosity >= 2:
                print(
                    '\n'.join(
                        difflib.unified_diff(
                            existing_info.splitlines(),
                            generated_json.splitlines())))
            force = True

    if force:
        log(
            "Regenerating Bazel workspace%s." %
            (", --force used" if args.force else ""))
        create_clean_dir(workspace_dir)
        create_clean_dir(output_base_dir)
        generated.write(topdir)
        with open(generated_info_file, 'w') as f:
            f.write(generated_json)
    else:
        log2("Nothing to do (no changes detected)")

    # Done!
    return 0


if __name__ == "__main__":
    sys.exit(main())
