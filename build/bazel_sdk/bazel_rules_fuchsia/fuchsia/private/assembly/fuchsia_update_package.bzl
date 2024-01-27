# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Rule for creating an update package."""

load(
    ":providers.bzl",
    "FuchsiaAssemblyConfigInfo",
    "FuchsiaProductImageInfo",
    "FuchsiaUpdatePackageInfo",
)
load("//fuchsia/private:ffx_tool.bzl", "get_ffx_assembly_inputs")

def _fuchsia_update_package_impl(ctx):
    fuchsia_toolchain = ctx.toolchains["@fuchsia_sdk//fuchsia:toolchain"]
    partitions_configuration = ctx.attr.partitions_config[FuchsiaAssemblyConfigInfo].config
    system_a_out = ctx.attr.product_image[FuchsiaProductImageInfo].images_out

    out_dir = ctx.actions.declare_directory(ctx.label.name + "_out")
    ffx_isolate_dir = ctx.actions.declare_directory(ctx.label.name + "_ffx_isolate_dir")

    inputs = get_ffx_assembly_inputs(fuchsia_toolchain) + [partitions_configuration, ctx.file.update_version_file] + ctx.files.product_image + ctx.files.partitions_config
    outputs = [out_dir, ffx_isolate_dir]

    # Gather all the arguments to pass to ffx.
    ffx_invocation = [
        fuchsia_toolchain.ffx.path,
        "--config \"assembly_enabled=true\"",
        "--isolate-dir",
        ffx_isolate_dir.path,
        "assembly",
        "create-update",
        "--partitions",
        partitions_configuration.path,
        "--board-name",
        ctx.attr.board_name,
        "--version-file",
        ctx.file.update_version_file.path,
        "--epoch",
        ctx.attr.update_epoch,
        "--outdir",
        out_dir.path,
        "--system-a",
        system_a_out.path + "/images.json",
    ]

    if ctx.attr.recovery_image:
        system_r_out = ctx.attr.recovery_image[FuchsiaProductImageInfo].images_out
        ffx_invocation += [
            "--system-r",
            system_r_out.path + "/images.json",
        ]
        inputs += ctx.files.recovery_image

    script_lines = [
        "set -e",
        "mkdir -p " + ffx_isolate_dir.path,
        " ".join(ffx_invocation),
    ]
    script = "\n".join(script_lines)

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = outputs,
        command = script,
        progress_message = "Create update package for %s" % ctx.label.name,
    )
    return [
        DefaultInfo(files = depset(direct = outputs + inputs)),
        OutputGroupInfo(
            debug_files = depset([ffx_isolate_dir]),
            all_files = depset([out_dir, ffx_isolate_dir] + inputs),
        ),
        FuchsiaUpdatePackageInfo(
            update_out = out_dir,
        ),
    ]

fuchsia_update_package = rule(
    doc = """Declares a Fuchsia update package.""",
    implementation = _fuchsia_update_package_impl,
    toolchains = ["@fuchsia_sdk//fuchsia:toolchain"],
    provides = [FuchsiaUpdatePackageInfo],
    attrs = {
        "product_image": attr.label(
            doc = "fuchsia_product_image target to put in slot A",
            providers = [FuchsiaProductImageInfo],
            mandatory = True,
        ),
        "recovery_image": attr.label(
            doc = "fuchsia_product_image target to put in slot R",
            providers = [FuchsiaProductImageInfo],
        ),
        "board_name": attr.string(
            doc = "Name of the board this update package runs on. E.g. qemu-x64",
            mandatory = True,
        ),
        "partitions_config": attr.label(
            doc = "Partitions config to use",
            mandatory = True,
        ),
        "update_version_file": attr.label(
            doc = "version file needed to create update package",
            allow_single_file = True,
            mandatory = True,
        ),
        "update_epoch": attr.string(
            doc = "epoch needed to create update package",
            mandatory = True,
        ),
    },
)
