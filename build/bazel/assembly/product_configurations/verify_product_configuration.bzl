# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Product configuration verification."""

load(
    "@fuchsia_sdk//fuchsia/private/assembly:providers.bzl",
    "FuchsiaProductConfigInfo",
)

def _verify_product_configuration_impl(ctx):
    actual_product_config_file = ctx.attr.product_configuration_target[FuchsiaProductConfigInfo].product_config
    expected_product_config_file = ctx.file.product_configuration_json

    diff_output = ctx.actions.declare_file(ctx.label.name + "_diff")
    ctx.actions.run(
        executable = ctx.executable._diff_json,
        outputs = [diff_output],
        inputs = [actual_product_config_file, expected_product_config_file],
        arguments = [
            actual_product_config_file.path,
            expected_product_config_file.path,
            diff_output.path,
        ],
    )

    return DefaultInfo(files = depset(direct = [diff_output]))

verify_product_configuration = rule(
    doc = """Compares a product configuration generated by Bazel with a JSON file""",
    implementation = _verify_product_configuration_impl,
    attrs = {
        "product_configuration_target": attr.label(
            providers = [FuchsiaProductConfigInfo],
            mandatory = True,
        ),
        "product_configuration_json": attr.label(
            mandatory = True,
            allow_single_file = [".json"],
        ),
        "_diff_json": attr.label(
            default = "//build/bazel/assembly/product_configurations:diff_json",
            executable = True,
            cfg = "exec",
        ),
    },
)
