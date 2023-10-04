# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This BUILD file is mapped into a downloaded Fuchsia SDK.
#
# This is referenced by //fuchsia/private/fuchsia_sdk_repository.bzl, see
# the `fuchsia_sdk_repository` rule for more information.
load("@fuchsia_sdk//fuchsia:defs.bzl", "fuchsia_debug_symbols", "fuchsia_toolchain_info")
load("api_version.bzl", "DEFAULT_FIDL_TARGET_API", "DEFAULT_TARGET_API")
load("@fuchsia_sdk//fuchsia/workspace:sdk_host_tool.bzl", "sdk_host_tool")

# Export all files as individual targets.
exports_files(glob(["**/*"]))

# A single target that includes all files in the SDK.
filegroup(
    name = "all_files",
    srcs = glob(["**/*"]),
    visibility = ["//visibility:public"],
)

constraint_value(
    name = "fuchsia_toolchain_version_sdk",
    constraint_setting = "//fuchsia/constraints:version",
    visibility = ["//visibility:public"],
)

platform(
    name = "fuchsia_platform_sdk",
    constraint_values = [":fuchsia_toolchain_version_sdk"],
    visibility = ["//visibility:public"],
)

fuchsia_debug_symbols(
    name = "debug_symbols",
    build_dir = "//:BUILD.bazel",
    build_id_dirs = ["//:.build-id"],
    visibility = ["//visibility:public"],
)

fuchsia_toolchain_info(
    name = "fuchsia_toolchain_info",
    aemu_runfiles = "//tools:aemu_internal_{{HOST_CPU}}",
    bindc = "//tools:{{HOST_CPU}}/bindc",
    blobfs = "//tools:{{HOST_CPU}}/blobfs_do_not_depend",
    blobfs_manifest = "//tools:{{HOST_CPU}}/blobfs_do_not_depend-meta.json",
    bootserver = "//tools:{{HOST_CPU}}/bootserver",
    cmc = "//tools:{{HOST_CPU}}/cmc",
    cmc_includes = select({
        "@platforms//os:fuchsia": "//:cmc_includes",
        "//conditions:default": None,
    }),
    cmc_manifest = "//tools:{{HOST_CPU}}/cmc-meta.json",
    default_fidl_target_api = DEFAULT_FIDL_TARGET_API,
    default_target_api = DEFAULT_TARGET_API,
    elf_test_runner_shard = "sys/testing/elf_test_runner.shard.cml",
    exec_cpu = "{{HOST_CPU}}",
    far = "//tools:{{HOST_CPU}}/far",
    ffx = "//tools:{{HOST_CPU}}/ffx",
    ffx_assembly = "//tools:{{HOST_CPU}}/ffx_tools/ffx-assembly",
    ffx_assembly_fho_meta = "//tools:{{HOST_CPU}}/ffx_tools/ffx-assembly.json",
    ffx_assembly_manifest = "//tools:{{HOST_CPU}}/ffx_tools/ffx-assembly-meta.json",
    ffx_package = "//tools:{{HOST_CPU}}/ffx_tools/ffx-package",
    ffx_package_fho_meta = "//tools:{{HOST_CPU}}/ffx_tools/ffx-package.json",
    ffx_package_manifest = "//tools:{{HOST_CPU}}/ffx_tools/ffx-package-meta.json",
    ffx_product = "//tools:{{HOST_CPU}}/ffx_tools/ffx-product",
    ffx_product_fho_meta = "//tools:{{HOST_CPU}}/ffx_tools/ffx-product.json",
    ffx_product_manifest = "//tools:{{HOST_CPU}}/ffx_tools/ffx-product-meta.json",
    ffx_scrutiny = "//tools:{{HOST_CPU}}/ffx_tools/ffx-scrutiny",
    ffx_scrutiny_fho_meta = "//tools:{{HOST_CPU}}/ffx_tools/ffx-scrutiny.json",
    ffx_scrutiny_manifest = "//tools:{{HOST_CPU}}/ffx_tools/ffx-scrutiny-meta.json",
    fidlc = "//tools:{{HOST_CPU}}/fidlc",
    fidlgen_cpp = "//tools:{{HOST_CPU}}/fidlgen_cpp",
    fidlgen_hlcpp = "//tools:{{HOST_CPU}}/fidlgen_hlcpp",
    fssh = "//tools:{{HOST_CPU}}/fssh",
    fvm = "//tools:{{HOST_CPU}}/fvm",
    fvm_manifest = "//tools:{{HOST_CPU}}/fvm-meta.json",
    gtest_runner_shard = "sys/testing/gtest_runner.shard.cml",
    merkleroot = "//tools:{{HOST_CPU}}/merkleroot",
    minfs = "//tools:{{HOST_CPU}}/minfs",
    minfs_manifest = "//tools:{{HOST_CPU}}/minfs-meta.json",
    pm = "//tools:{{HOST_CPU}}/pm",
    sdk_id = "{{SDK_ID}}",
    sdk_manifest = "//:meta/manifest.json",
    zbi = "//tools:{{HOST_CPU}}/zbi",
    zbi_manifest = "//tools:{{HOST_CPU}}/zbi-meta.json",
)

toolchain(
    name = "fuchsia_toolchain_sdk",
    toolchain = ":fuchsia_toolchain_info",
    toolchain_type = "@fuchsia_sdk//fuchsia:toolchain",
)

# The following rules expose the sdk tools that are put in the fuchsia_toolchain
# so uses can run them directly. These targets can be invoked by calling
# `bazel run @fuchsia_sdk//:<tool_name> -- args`
# Additionally, the run_sdk_tool.sh script knows how to invoke these tools
# and can be used as a drop in replacement for the tool.
sdk_host_tool(name = "ffx")

sdk_host_tool(name = "fssh")

sdk_host_tool(name = "cmc")

sdk_host_tool(name = "pm")

sdk_host_tool(name = "far")

{{__FUCHSIA_SDK_INCLUDE__}}
