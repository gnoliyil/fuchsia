# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Helper functions to define Clang toolchain related targets."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
    "with_feature_set",
)
load("//:toolchains/clang/clang_utils.bzl", "to_clang_target_tuple")
load("//:toolchains/clang/providers.bzl", "ClangInfo")
load("//:toolchains/clang/sanitizer.bzl", "sanitizer_features")

def compute_clang_features():
    """Compute list of C++ toolchain features required by Clang.

    Returns:
      A list of feature() objects.
    """
    # Redefine the dependency_files feature in order to use -MMD instead of -MD
    # which ensures that system headers files are not listed in the dependency file.
    # Doing this allows remote builds to work with our prebuilt toolchain. Otherwise
    # the paths in cxx_builtin_include_directories will not match the ones that are
    # generated in the remote builder, resulting in an error like:
    #
    # ```
    # ERROR: ..../workspace/build/bazel/tests/hello_world/BUILD.bazel:5:10: Compiling build/bazel/tests/hello_world/main.cc failed: undeclared inclusion(s) in rule '//build/bazel/tests/hello_world:hello_world':
    # this rule is missing dependency declarations for the following files included by 'build/bazel/tests/hello_world/main.cc':
    # '/b/f/w/external/prebuilt_clang/include/c++/v1/stdio.h'
    # '/b/f/w/external/prebuilt_clang/include/c++/v1/__config'
    # '/b/f/w/external/prebuilt_clang/include/x86_64-unknown-linux-gnu/c++/v1/__config_site'
    # '/b/f/w/external/prebuilt_clang/include/c++/v1/stddef.h'
    # '/b/f/w/external/prebuilt_clang/lib/clang/16.0.0/include/stddef.h'
    # '/b/f/w/external/prebuilt_clang/include/c++/v1/wchar.h'
    # '/b/f/w/external/prebuilt_clang/lib/clang/16.0.0/include/stdarg.h'
    # Target //build/bazel/tests/hello_world:hello_world failed to build
    # ```
    # Where `/b/f/w/` is the path of the execroot in the remote builder.
    #
    # Note that trying to set paths relative to the execroot in cxx_builtin_include_directories
    # does not work (Bazel resolves them to absolute paths before recording them and trying to launch
    # remote builds).
    #
    # Another benefit is that we can symlink the prebuilt clang directory
    # in our @prebuilt_clang repository now. This does not work with -MD because
    # the path generated by the compiler for dependencies are fully resolved
    # and would not match the content of cxx_builtin_include_directories,
    # resulting in errors like the one above, where the path listed in the
    # dependency would be something like
    # `/fuchsia/prebuilt/third_party/clang/linux-x64/include/c++/v1/wchar.h`
    # instead of the expected `external/prebuilt_clang/include/c++/c1/wchar.h`

    # See https://cs.opensource.google/bazel/bazel/+/master:tools/cpp/unix_cc_toolchain_config.bzl;drc=ed03d3edc3bab62942f2f9fab51f342fc8280930;l=1009
    # for the original feature() definition.
    dependency_file_feature = feature(
        name = "dependency_file",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-MMD", "-MF", "%{dependency_file}"],
                        expand_if_available = "dependency_file",
                    ),
                ],
            ),
        ],
    )

    ml_inliner_feature = feature(
        name = "ml_inliner",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-mllvm",
                            "-enable-ml-inliner=release",
                        ],
                    ),
                ],
                with_features = [with_feature_set(
                    features = ["opt"],
                )],
            ),
        ],
    )

    coverage_feature = feature(
        name = "coverage",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_link_dynamic_library,
                    ACTION_NAMES.cpp_link_executable,
                    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fprofile-instr-generate",
                            "-fcoverage-mapping",
                            # Apply some optimizations so tests are fast,
                            # but not too much so that coverage is inaccurate.
                            "-O1",
                        ],
                    ),
                ],
            ),
        ],
    )

    features = [
        dependency_file_feature,
        ml_inliner_feature,
        coverage_feature,
    ] + sanitizer_features

    return features

# buildifier: disable=unnamed-macro
def _generate_libcxx_filegroups(clang_constants, os, arch):
    """Generate filegroups for libc++ headers and runtime libraries.

    Args:
      clang_constants: A struct containing Clang configuration information.
      os: Target os for the toolchain.
      arch: Target cpu for the toolchain.

    Returns:
      A struct giving the names of the filegroups for the headers and runtime
      libraries for libc++.
    """
    clang_target_tuple = to_clang_target_tuple(os, arch)

    prefix = clang_target_tuple
    names = struct(
        headers = prefix + "_libcxx_headers",
        runtime_libs = prefix + "_libcxx_runtime",
    )

    native.filegroup(
        name = names.headers,
        srcs = native.glob([
            "lib/clang/%s/include/**" % clang_constants.long_version,
            "include/c++/v1/**",
            "include/%s/c++/v1/**" % clang_target_tuple,
        ]),
    )

    native.filegroup(
        name = names.runtime_libs,
        srcs = native.glob([
            "lib/%s/**" % clang_target_tuple,
        ]) + [
            "lib/clang/%s/lib/%s/libclang_rt.builtins.a" % (clang_constants.short_version, clang_target_tuple),
        ],
    )

    return names

def _prebuilt_clang_cc_toolchain_config_impl(ctx):
    # See CppConfiguration.java class in Bazel sources for the list of
    # all tool_path() names that must be defined and relative to the
    # clang repository directory.
    tool_paths = [
        tool_path(name = "ar", path = "bin/llvm-ar"),
        tool_path(name = "cpp", path = "bin/cpp"),
        tool_path(name = "gcc", path = "bin/clang"),
        tool_path(name = "gcov", path = "/usr/bin/false"),
        tool_path(name = "gcov-tool", path = "/usr/bin/false"),
        tool_path(name = "ld", path = "bin/llvm-ld"),
        tool_path(name = "llvm-cov", path = "bin/llvm-cov"),
        tool_path(name = "nm", path = "bin/llvm-nm"),
        tool_path("objcopy", path = "bin/llvm-objcopy"),
        tool_path("objdump", path = "bin/llvm-objdump"),
        tool_path("strip", path = "bin/llvm-strip"),
        tool_path(name = "dwp", path = "/usr/bin/false"),
        tool_path(name = "llvm-profdata", path = "bin/llvm-profdata"),
    ]

    # TODO(digit): Change features list based on target_os and build variants
    # For now, this is only enough for host toolchains.
    features = compute_clang_features()

    clang_info = ctx.attr.clang_info[ClangInfo]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "prebuilt_clang",
        tool_paths = tool_paths,
        features = features,
        action_configs = [],
        cxx_builtin_include_directories = clang_info.builtin_include_paths,
        builtin_sysroot = ctx.attr.sysroot,
        target_cpu = "_".join([ctx.attr.target_os, ctx.attr.target_arch]),

        # Required by constructor, but otherwise ignored by Bazel.
        # These string values are arbitrary, but are easy to grep
        # in our source tree if they ever happen to appear in
        # build error messages.
        host_system_name = "__bazel_host_system_name__",
        target_system_name = "__bazel_target_system_name__",
        target_libc = "__bazel_target_libc__",
        abi_version = "__bazel_abi_version__",
        abi_libc_version = "__bazel_abi_libc_version__",
        compiler = "__bazel_compiler__",
    )

_prebuilt_clang_cc_toolchain_config = rule(
    implementation = _prebuilt_clang_cc_toolchain_config_impl,
    attrs = {
        "target_os": attr.string(mandatory = True),
        "target_arch": attr.string(mandatory = True),
        "sysroot": attr.string(default = ""),
        "clang_info": attr.label(
            mandatory = True,
            providers = [ClangInfo],
        ),
    },
)

def generate_clang_cc_toolchain(
        name,
        host_os,
        host_arch,
        target_os,
        target_arch,
        clang_constants,
        sysroot_files = [],
        sysroot_path = ""):
    """Define C++ toolchain related targets for a prebuilt Clang installation.

    This defines cc_toolchain(), cc_toolchain_config() and toolchain() targets
    for a given Clang prebuilt installation and target (os,arch) pair.

    Args:
       name: Name of the cc_toolchain() target. The corresponding
         toolchain() target will use "${name}_cc_toolchain", and
         the cc_toolchain_config() will use "${name}_cc_toolchain_config".

       host_os: Host os string, using Bazel conventions.
       host_arch: Host cpu architecture string, using Bazel conventions.
       target_os: Target os string, using Bazel conventions.
       target_arch: Target cpu architecture string, using Bazel conventions.

       clang_constants: TBW

       sysroot_files: (optiona) A target list for the sysroot files to be used.

       sysroot_path: (optional) Path to the sysroot directory to be used.
    """
    _prebuilt_clang_cc_toolchain_config(
        name = name + "_cc_toolchain_config",
        target_os = target_os,
        target_arch = target_arch,
        sysroot = sysroot_path,
        clang_info = "//:clang_info",
    )

    libcxx = _generate_libcxx_filegroups(clang_constants, target_os, target_arch)

    common_compiler_files = [
        ":clang_compiler_binaries",
        ":" + libcxx.headers,
    ]

    common_linker_files = [
        ":clang_linker_binaries",
        ":" + libcxx.runtime_libs,
    ]

    compiler_files = name + "_compiler_files"
    native.filegroup(
        name = compiler_files,
        srcs = common_compiler_files + sysroot_files,
    )

    linker_files = name + "_linker_files"
    native.filegroup(
        name = linker_files,
        srcs = common_linker_files + sysroot_files,
    )

    all_files = name + "_all_files"
    native.filegroup(
        name = all_files,
        srcs = common_compiler_files + common_linker_files + sysroot_files,
    )

    native.cc_toolchain(
        name = name,
        all_files = ":" + all_files,
        ar_files = ":clang_ar_binaries",
        as_files = ":clang_empty",
        compiler_files = ":" + compiler_files,
        dwp_files = ":clang_empty",
        linker_files = ":" + linker_files,
        objcopy_files = ":clang_objcopy_binaries",
        strip_files = ":clang_strip_binaries",
        # `supports_param_files = 1` means that the toolchain supports
        # reading arguments from a responsde file (e.g. `@arguments.rsp`)
        supports_param_files = 1,
        toolchain_config = name + "_cc_toolchain_config",
        toolchain_identifier = "prebuilt_clang",
    )

    native.toolchain(
        name = name + "_cc_toolchain",
        exec_compatible_with = [
            "@platforms//os:" + host_os,
            "@platforms//cpu:" + host_arch,
        ],
        target_compatible_with = [
            "@platforms//os:" + target_os,
            "@platforms//cpu:" + target_arch,
        ],
        toolchain = ":" + name,
        toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    )
