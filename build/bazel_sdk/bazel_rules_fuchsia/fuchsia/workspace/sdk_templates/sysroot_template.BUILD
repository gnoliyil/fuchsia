load(
    "@rules_fuchsia//fuchsia:defs.bzl",
    "fuchsia_cpu_select",
)
load("//:generated_constants.bzl", "constants")

alias(
    name = "dist",
    actual = fuchsia_cpu_select(
        {
            "arm64": {
                "@rules_fuchsia//fuchsia/constraints:is_fuchsia_arm64": "//{{relative_dir}}/arm64:dist",
            },
            "x64": {
                "@rules_fuchsia//fuchsia/constraints:is_fuchsia_x64": "//{{relative_dir}}/x64:dist",
            },
        },
        constants.target_cpus,
    ),
)
