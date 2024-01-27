load(
    "@fuchsia_sdk//fuchsia:defs.bzl",
    "fuchsia_select",
)

# Note: the cc_library / cc_import combo serves two purposes:
#  - it allows the use of a select clause to target the proper architecture;
#  - it works around an issue with cc_import which does not have an "includes"
#    nor a "deps" attribute.
cc_library(
    name = "{{name}}",
    hdrs = [
        {{headers}}
    ],
    deps = {{prebuilt_select}} + [ {{deps}} ],
    strip_include_prefix = "{{relative_include_dir}}",
    data = {{dist_select}},
    target_compatible_with = [ "@platforms//os:fuchsia" ],
)
