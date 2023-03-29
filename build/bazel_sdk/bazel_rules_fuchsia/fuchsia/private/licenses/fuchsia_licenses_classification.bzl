# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Rule for classifying OSS licenses."""

def _fuchsia_licenses_classification_impl(ctx):
    out_json = ctx.actions.declare_file(ctx.label.name)

    inputs = [ctx.file.spdx_input, ctx.executable.identify_license]
    arguments = [
        "--spdx_input=%s" % ctx.file.spdx_input.path,
        "--identify_license_bin=%s" % ctx.executable.identify_license.path,
        "--output_file=%s" % out_json.path,
    ]

    if ctx.attr.default_condition:
        arguments.append("--default_condition=%s" % ctx.attr.default_condition)
    if ctx.attr.allowed_conditions:
        arguments.append("--allowed_conditions")
        arguments.extend(ctx.attr.allowed_conditions)
    if ctx.attr.fail_on_disallowed_conditions:
        arguments.append("--fail_on_disallowed_conditions=True")
    if ctx.files.policy_override_rules:
        inputs.extend(ctx.files.policy_override_rules)
        arguments.append("--policy_override_rules")
        arguments.extend([f.path for f in ctx.files.policy_override_rules])

    ctx.actions.run(
        progress_message = "Generating license classifications into %s" % out_json.path,
        inputs = inputs,
        outputs = [out_json],
        executable = ctx.executable._generate_licenses_classification_tool,
        arguments = arguments,
    )

    return [DefaultInfo(files = depset([out_json]))]

fuchsia_licenses_classification = rule(
    doc = """
Produces a json file with license classification output.

The [name].json has the following schema:

```
{
    // Dictionary of license_ids : lists of classifcations
    "[license_id]":
        [
            // list of named classsifications
            {
                "name": str,
                "confidence": float,
                "start_line": int,
                "end_line": int,
            },
            ...
        ],
    ,
    ...
}
```
""",
    implementation = _fuchsia_licenses_classification_impl,
    attrs = {
        "spdx_input": attr.label(
            doc = "The target to aggregate the licenses from.",
            allow_single_file = True,
            mandatory = True,
        ),
        "identify_license": attr.label(
            doc = """The location of the 'identify_license' tool from
https://github.com/google/licenseclassifier/tree/main/tools/identify_license
or a program with a similar I/O. Different organizations should configure
and build identify_license to match their organization OSS compliance policies.
""",
            executable = True,
            allow_single_file = True,
            cfg = "exec",
            mandatory = True,
        ),
        # TODO(tqr/121609): Remove once v/g usage is removed.
        "conditions_policy": attr.label(
            doc = """Unused""",
            allow_single_file = True,
            mandatory = False,
            default = None,
        ),
        "policy_override_rules": attr.label_list(
            doc = """Condition override rule files""",
            allow_files = True,
            mandatory = False,
            default = [],
        ),
        "default_condition": attr.string(
            doc = "The default condition for unmapped or unidentified licenses",
            mandatory = False,
            default = "",
        ),
        "allowed_conditions": attr.string_list(
            doc = """List of allowed conditions.""",
            mandatory = False,
            default = [],
        ),
        "fail_on_disallowed_conditions": attr.bool(
            doc = """The rule will fail if identified licenses map to disallowed conditions.""",
            mandatory = False,
            default = False,
        ),
        "_generate_licenses_classification_tool": attr.label(
            executable = True,
            cfg = "exec",
            default = "//fuchsia/tools/licenses:generate_licenses_classification",
        ),
    },
)
