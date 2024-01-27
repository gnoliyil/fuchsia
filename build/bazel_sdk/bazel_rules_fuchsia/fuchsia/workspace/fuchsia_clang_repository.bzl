# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Defines a WORKSPACE rule for loading a version of clang."""

load("//fuchsia/workspace:utils.bzl", "normalize_os", "workspace_path")
load("//cipd:defs.bzl", "fetch_cipd_contents")

# Base URL for Fuchsia clang archives.
_CLANG_URL_TEMPLATE = "https://chrome-infra-packages.appspot.com/dl/fuchsia/third_party/clang/{os}-amd64/+/{tag}"

_LOCAL_FUCHSIA_PLATFORM_BUILD = "LOCAL_FUCHSIA_PLATFORM_BUILD"
_LOCAL_FUCHSIA_CLANG_DIR = "../../prebuilt/third_party/clang"

def _clang_url(os, tag):
    # Return the URL of clang given an Operating System string and a CIPD tag.
    return _CLANG_URL_TEMPLATE.format(os = os, tag = tag)

def _instantiate_local_archive(ctx):
    # Extracts the clang from a local archive file.
    ctx.report_progress("Extracting local clang archive")
    ctx.extract(archive = ctx.attr.local_archive)

def _instantiate_from_local_dir(ctx, local_clang):
    # buildifier: disable=print
    # local_path can be either a string or Path object.
    if type(local_clang) == type("str"):
        local_clang = ctx.path(local_clang)

    ctx.report_progress("Copying local clang from %s" % local_clang)

    # Symlink top-level items from Clang prebuilt install to repository directory
    # Note that this is possible because our C++ toolchain configuration redefine
    # the "dependency_file" feature to use relative file paths.
    for f in local_clang.readdir():
        ctx.symlink(f, f.basename)

def _instantiate_from_local_fuchsia_tree(ctx):
    # Copies clang prebuilt from a local Fuchsia platform tree.
    local_fuchsia_dir = ctx.os.environ[_LOCAL_FUCHSIA_PLATFORM_BUILD]
    local_clang = ctx.path("%s/%s" % (local_fuchsia_dir, _LOCAL_FUCHSIA_CLANG_DIR))
    if not local_clang.exists:
        fail("Cannot find clang prebuilt in local Fuchsia tree. Please ensure it exists: %s" % str(local_clang))
    local_clang_archs = local_clang.readdir()
    if len(local_clang_archs) != 1:
        fail("Expected a single host architecture subdirectory in local clang: %s" % str(local_clang))

    local_clang_arch = local_clang_archs[0]
    _instantiate_from_local_dir(ctx, local_clang_arch)

def _fuchsia_clang_repository_impl(ctx):
    # Pre-evaluate paths of templated output files so that the repository does not need to be
    # re-fetched after potentially talking to the network
    ctx.path("BUILD.bazel")
    ctx.path("cc_toolchain_config.bzl")

    crosstool_template = Label("//fuchsia/workspace/clang_templates:crosstool_template.BUILD")
    toolchain_config_template = Label("//fuchsia/workspace/clang_templates:cc_toolchain_config_template.bzl")

    ctx.path(crosstool_template)
    ctx.path(toolchain_config_template)

    # Hack to get the path to the sysroot directory, see
    # https://github.com/bazelbuild/bazel/issues/3901
    fuchsia_sdk_path = ctx.path(ctx.attr.sdk_root_label.relative("//:BUILD.bazel")).dirname

    ctx.file("WORKSPACE.bazel", content = "")

    ctx.symlink(
        Label("//fuchsia/workspace/clang_templates:defs.bzl"),
        "defs.bzl",
    )

    # Create symlinks to the @fuchsia_sdk sysroots, which allows defining
    # a filegroup() in this repository that uses glob() to list all the files.
    # (see fuchsia-sysroot-{arch} definitions in cc_toolchain_config.bzl.
    ctx.symlink(str(fuchsia_sdk_path) + "/arch/arm64/sysroot", "fuchsia_sysroot_aarch64")
    ctx.symlink(str(fuchsia_sdk_path) + "/arch/x64/sysroot", "fuchsia_sysroot_x86_64")
    ctx.symlink(str(fuchsia_sdk_path) + "/arch/riscv64/sysroot", "fuchsia_sysroot_riscv64")

    normalized_os = normalize_os(ctx)
    if ctx.attr.local_path:
        local_clang = workspace_path(ctx, ctx.attr.local_path)
        _instantiate_from_local_dir(ctx, local_clang)
    elif _LOCAL_FUCHSIA_PLATFORM_BUILD in ctx.os.environ:
        _instantiate_from_local_fuchsia_tree(ctx)
    elif ctx.attr.local_archive:
        _instantiate_local_archive(ctx)
    elif ctx.attr.cipd_tag:
        sha256 = ""
        if ctx.attr.sha256:
            sha256 = ctx.attr.sha256[normalized_os]
        ctx.download_and_extract(
            _clang_url(normalized_os, ctx.attr.cipd_tag),
            type = "zip",
            sha256 = sha256,
        )
    else:
        fetch_cipd_contents(ctx, ctx.attr._cipd_bin, ctx.attr._cipd_ensure_file)

    # find the clang version as the largest number
    clang_version = "0"
    for v in ctx.path("lib/clang").readdir():
        v = str(v.basename)
        if v.split(".")[0].isdigit() and v > clang_version:
            clang_version = v

    # Set up the BUILD file from the Fuchsia SDK.
    ctx.template(
        "BUILD.bazel",
        crosstool_template,
        substitutions = {
            "%{CLANG_VERSION}": clang_version,
        },
    )

    # To properly use a custom Bazel C++ sysroot, the following are necessary:
    #
    # - The cc_toolchain() `compiler_files` argument must list
    #   the sysroot headers, to ensure they are exposed to the sandbox
    #   at compile time.
    #
    # - The cc_toolchain() `linker_files` argument must list
    #   the link-time sysroot libraries, to ensure they are exposed to
    #   the sandbox at link time.
    #
    # - The create_cc_toolchain_config_info() `builtin_sysroot` argument
    #   is a string that is passed directly to the compile and link command
    #   lines (as `--sysroot=<value>`) without further interpretation.
    #
    #   To ensure that build outputs are remote cacheable, using a path
    #   relative to the execroot is necessary. An absolute path will work,
    #   but will pollute the cache keys and prevent sharing artifacts between
    #   different workspaces.
    #
    # - The create_cc_toolchain_config_info() `cxx_builtin_include_directories`
    #   must list a series of directories that Bazel uses to ensure that the
    #   compiler-generated .d files only contain input paths that belong to
    #   this list.
    #
    #   These path strings can have several formats (for full details
    #   see the CcToolchainProviderHelper.resolveIncludeDir() method in
    #   the Bazel sources):
    #
    #   - An absolute path is used as-is, and does not hurt cacheability.
    #
    #   - A prefix of %sysroot% is replaced with the sysroot path.
    #
    #   - A prefix of %crosstool_top% is replaced with the crosstool directory,
    #     which may *not* be the same as this repository's directory. It is
    #     best to avoid them when using platforms.
    #
    #   - A relative directory is resolved relative to %crosstop_top% as well!
    #
    #   - A %package(<label>)/folder form is resolved properly. However, this
    #     doesn't seem to generate working results.
    #
    #   For now, use absolute paths because they are simple and they work,
    #   and there is no way to debug the expansions performed for these path
    #   expressions!
    #

    # The sysroot path value that will be used in build commands must
    # be relative to the execroot to ensure remote cacheability of build.
    #
    # The line below uses ctx.name because enabling BzlMod will rename the
    # repository's directory path when it is generated by a module extension.
    sysroot_path_prefix = "external/%s/fuchsia_sysroot_" % ctx.name

    # Set up the toolchain config file from the template.
    ctx.template(
        "cc_toolchain_config.bzl",
        toolchain_config_template,
        substitutions = {
            "%{SYSROOT_PATH_PREFIX}": sysroot_path_prefix,
            "%{CROSSTOOL_ROOT}": str(ctx.path(".")),
            "%{CLANG_VERSION}": clang_version,
            "%{SDK_ROOT}": ctx.attr.sdk_root_label.workspace_name,
        },
    )

fuchsia_clang_repository = repository_rule(
    doc = """
Loads a particular version of clang.

One of cipd_tag or local_archive must be set.

If cipd_tag is set, sha256 can optionally be set to verify the downloaded file
and to allow Bazel to cache the file.

If cipd_tag is not set, local_archive must be set to the path of a core IDK
archive file.
""",
    implementation = _fuchsia_clang_repository_impl,
    environ = [_LOCAL_FUCHSIA_PLATFORM_BUILD],
    attrs = {
        "cipd_tag": attr.string(
            doc = "CIPD tag for the version to load.",
        ),
        "sha256": attr.string_dict(
            doc = "Optional SHA-256 hash of the clang archive. Valid keys are mac and linux",
        ),
        "local_archive": attr.string(
            doc = "local clang archive file.",
        ),
        "local_path": attr.string(
            doc = "local clang installation directory, relative to workspace root.",
        ),
        "sdk_root_label": attr.label(
            doc = "The fuchsia sdk root label. eg: @fuchsia_sdk",
            default = "@fuchsia_sdk",
        ),
        "rules_fuchsia_root_label": attr.label(
            doc = "The fuchsia workspace rules root label. eg: @fuchsia_sdk",
            default = "@fuchsia_sdk",
        ),
        "_cipd_ensure_file": attr.label(
            doc = "A cipd ensure file to use to download clang.",
            default = "//fuchsia/manifests:clang.ensure",
        ),
        "_cipd_bin": attr.label(
            doc = "The cipd binary that will be used to download the sdk",
            default = "@cipd_tool//:cipd",
        ),
    },
)

def _fuchsia_clang_repository_ext(ctx):
    cipd_tag = None
    sha256 = None
    local_archive = None
    local_path = None
    sdk_root_label = None
    rules_fuchsia_root_label = None

    for mod in ctx.modules:
        # only the root module can set tags
        if mod.is_root:
            if mod.tags.labels:
                labels = mod.tags.labels[0]
                sdk_root_label = labels.sdk_root_label
                rules_fuchsia_root_label = labels.rules_fuchsia_root_label
            if mod.tags.cipd:
                cipd = mod.tags.cipd[0]
                cipd_tag = cipd.cipd_tag
                sha256 = cipd.sha256
            if mod.tags.archive:
                local_archive = mod.tags.archive[0].local_archive
            if mod.tags.local:
                local_path = mod.tags.local[0].local_path

    fuchsia_clang_repository(
        name = "fuchsia_clang",
        cipd_tag = cipd_tag,
        sha256 = sha256,
        local_archive = local_archive,
        local_path = local_path,
        sdk_root_label = sdk_root_label,
        rules_fuchsia_root_label = rules_fuchsia_root_label,
    )

_labels_tag = tag_class(
    attrs = {
        "sdk_root_label": attr.label(
            doc = "The fuchsia sdk root label. eg: @fuchsia_sdk",
            default = "@fuchsia_sdk",
        ),
        "rules_fuchsia_root_label": attr.label(
            doc = "The fuchsia workspace rules root label. eg: @fuchsia_sdk",
            default = "@fuchsia_sdk",
        ),
    },
)

_cipd_tag = tag_class(
    attrs = {
        "cipd_tag": attr.string(
            doc = "CIPD tag for the version to load.",
        ),
        "sha256": attr.string_dict(
            doc = "Optional SHA-256 hash of the clang archive. Valid keys are mac and linux",
        ),
    },
)

_archive_tag = tag_class(
    attrs = {"local_archive": attr.string(
        doc = "local clang archive file.",
    )},
)

_local_tag = tag_class(
    attrs = {"local_path": attr.string(
        doc = "local clang installation path, relative to workspace_root.",
    )},
)

fuchsia_clang_ext = module_extension(
    implementation = _fuchsia_clang_repository_ext,
    tag_classes = {
        "labels": _labels_tag,
        "cipd": _cipd_tag,
        "archive": _archive_tag,
        "local": _local_tag,
    },
)
