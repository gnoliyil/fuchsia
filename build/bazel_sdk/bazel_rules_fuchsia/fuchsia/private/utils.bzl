# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Common utilities needed by rules_fuchsia rules."""

load(":providers.bzl", "FuchsiaProvidersInfo")

_INVALID_LABEL_CHARACTERS = "\"!%@^_#$&'()*+,;<=>?[]{|}~/".elems()

def normalized_target_name(label):
    label = label.lower()
    for c in _INVALID_LABEL_CHARACTERS:
        label = label.replace(c, ".")
    return label

def label_name(label):
    # convert the label to a single word
    # //foo/bar -> bar
    # :bar -> bar
    # //foo:bar -> bar
    return label.split("/")[-1].split(":")[-1]

def get_project_execroot(ctx):
    # Gets the project/workspace execroot relative to the output base.
    # See https://bazel.build/docs/output_directories.
    return "execroot/%s" % ctx.workspace_name

def get_target_execroot(ctx, target):
    # Gets the execroot for a given target, relative to the project execroot.
    # See https://bazel.build/docs/output_directories.
    return target[DefaultInfo].files_to_run.runfiles_manifest.dirname + "/" + ctx.workspace_name

def stub_executable(ctx):
    """Returns a stub executable that fails with a message."""
    executable_file = ctx.actions.declare_file(ctx.label.name + "_fail.sh")
    content = """#!/bin/bash
    echo "---------------------------------------------------------"
    echo "ERROR: Attempting to run a target or dependency that is not runnable"
    echo "Got {target}"
    echo "---------------------------------------------------------"
    exit 1
    """.format(target = ctx.attr.name)

    ctx.actions.write(
        output = executable_file,
        content = content,
        is_executable = True,
    )

    return executable_file

def flatten(elements):
    """Flattens an arbitrarily nested list of lists to non-list elements while preserving order."""
    result = []
    unprocessed = list(elements)
    for _ in range(len(str(unprocessed))):
        if not unprocessed:
            return result
        elem = unprocessed.pop(0)
        if type(elem) in ("list", "tuple"):
            unprocessed = list(elem) + unprocessed
        else:
            result.append(elem)
    fail("Unable to flatten list!")

def collect_runfiles(ctx, *elements, ignore_types = []):
    """Collects multiple types of elements (...files, ...targets, ...runfiles) into runfiles."""

    # Map to runfiles objects.
    runfiles = []
    for elem in flatten(elements):
        if type(elem) == "Target":
            runfiles.append(elem[DefaultInfo].default_runfiles)
            files_to_run = elem[DefaultInfo].files_to_run
            if files_to_run.executable and files_to_run.runfiles_manifest:
                runfiles.append(ctx.runfiles([
                    files_to_run.executable,
                    files_to_run.runfiles_manifest,
                ]))
        elif type(elem) == "File":
            runfiles.append(ctx.runfiles([elem]))
        elif type(elem) == "runfiles":
            runfiles.append(elem)
        elif type(elem) not in ignore_types:
            fail("Unable to get runfiles from %s: %s" % (type(elem), str(elem)))

    # Merges runfiles for a given target.
    return ctx.runfiles().merge_all(runfiles)

def wrap_executable(ctx, executable, *arguments, script_name = None):
    """Wraps an executable with predefined command line arguments.

    Creates a wrapper script that invokes an underlying executable with
    predefined command line arguments.

    script_name defaults to `run_${target_name}.sh`.
    """
    wrapper = ctx.actions.declare_file(script_name or "run_%s.sh" % ctx.attr.name)

    # Convert file arguments into strings and serialize arguments.
    def serialize(arg):
        readlink = False
        if type(arg) == "Target":
            arg = arg[DefaultInfo].files_to_run.executable
            readlink = True
        if type(arg) == "File":
            arg = arg.short_path
        arg = "'%s'" % arg.replace("'", "\\'")

        # Follow symlink for complex tool executables, otherwise we will run
        # into issues with nested runfiles symlink farms.
        if readlink:
            arg = "$(readlink -f %s)" % arg
        return arg

    command = [serialize(arg) for arg in [executable] + list(arguments)]

    ctx.actions.write(wrapper, """#!/bin/bash
%s $@
""" % " ".join(command), is_executable = True)
    return wrapper, collect_runfiles(ctx, executable, arguments, ignore_types = ["string"])

def _add_providers_info(implementation):
    def _impl(ctx):
        return track_providers(implementation(ctx))

    return _impl

def _add_default_executable(implementation):
    def _impl(ctx):
        providers = implementation(ctx)
        if not [provider for provider in providers if type(provider) == "DefaultInfo"]:
            providers.append(DefaultInfo(executable = stub_executable(ctx)))
        return providers

    return _impl

def rule_variants(implementation, variants = [], attrs = {}, **rule_kwargs):
    """Creates variants of a rule.

    Valid variants:
     - None: Behaves like `rule` natively.
     - "executable": Sets executable = True and adds a stub executable if
       DefaultInfo is not provided by implementation.
     - "test": Sets test = True and adds a stub executable if DefaultInfo is not
       provided by implementation.

    All other arguments will be forwarded to rule.
    """
    return [rule(
        executable = variant == "executable",
        test = variant == "test",
        attrs = dict(attrs, _variant = attr.string(default = variant or "")),
        implementation = _add_providers_info(
            implementation if variant == None else _add_default_executable(implementation),
        ),
        **rule_kwargs
    ) for variant in variants]

def rule_variant(implementation, variant = None, attrs = {}, **rule_kwargs):
    """Creates a variant of a rule. See rule_variants for argument descriptions."""
    return rule_variants(variants = [variant], attrs = attrs, implementation = implementation, **rule_kwargs)[0]

def track_providers(providers):
    return providers + [FuchsiaProvidersInfo(
        providers = [
            provider
            for provider in providers
            if type(provider) != "DefaultInfo"
        ],
    )]

def forward_providers(ctx, target, *providers, rename_executable = None):
    default_info = target[DefaultInfo]
    if default_info.files_to_run and default_info.files_to_run.executable:
        executable = default_info.files_to_run.executable
        executable_symlink = ctx.actions.declare_file(
            rename_executable or "_" + executable.basename,
        )
        ctx.actions.symlink(
            output = executable_symlink,
            target_file = executable,
            is_executable = True,
        )
        default_info = DefaultInfo(
            files = depset([executable_symlink] + [
                file
                for file in default_info.files.to_list()
                if file != executable
            ]) if rename_executable else default_info.files,
            runfiles = default_info.default_runfiles,
            executable = executable_symlink,
        )
    target_provider_info = target[FuchsiaProvidersInfo] if (
        FuchsiaProvidersInfo in target
    ) else struct(providers = [])
    return [
        target[Provider]
        for Provider in providers
        if Provider in target
    ] + target_provider_info.providers + [default_info]

def _forward_providers(ctx):
    return forward_providers(ctx, ctx.attr.actual)

_alias, _alias_for_executable, _alias_for_test = rule_variants(
    variants = (None, "executable", "test"),
    implementation = _forward_providers,
    attrs = {
        "actual": attr.label(
            doc = "The test workflow entity target to alias.",
            providers = [FuchsiaProvidersInfo],
            mandatory = True,
        ),
    },
)

def alias(*, name, executable, testonly = False, **kwargs):
    """
    We have to create our own alias macro because Bazel is unreasonable:
    https://github.com/bazelbuild/bazel/issues/10893

    The underlying target must be created with `rule_variant(s)` or manually
    include `FuchsiaProvidersInfo` in order to forward providers.
    """
    return ((
        _alias_for_test if testonly else _alias_for_executable
    ) if executable else _alias)(
        name = name,
        testonly = testonly,
        **kwargs
    )

def filter(obj, value = None, exclude = True):
    """Recursively removes matching fields/elements from an object by mutating."""
    if type(obj) not in ("dict", "list"):
        fail("Unsupported data type.")

    nested_fields = [obj]

    # Since dictionaries and lists can be represented as DAGs, this represents
    # one filter operation within an iterative BFS.
    def filter_next():
        obj = nested_fields.pop(0)

        # Lists and dictionaries can both be represented as key-value pairs.
        for k, nested in (obj.items() if type(obj) == "dict" else enumerate(obj)):
            if type(nested) in ("dict", "list"):
                # Add a nested object to the BFS queue.
                nested_fields.append(nested)
            elif (nested == value) == exclude:
                # Remove the matching value's field by mutating the object.
                obj.pop(k)

    # Using and iterative BFS to filter all matching values within `obj` should
    # take less than `len(str(obj))` iterations.
    for _ in range(len(str(obj))):
        # Empty nested_fields means that we're done with our BFS.
        if not nested_fields:
            return obj
        filter_next()

    # In case the previous assumption is violated.
    fail("Unable to filter all none values!")

def make_resource_struct(src, dest):
    return struct(
        src = src,
        dest = dest,
    )

def get_runfiles(target):
    # Helper function to get the runfiles as a list of files from a target.
    return [symlink.target_file for symlink in target[DefaultInfo].default_runfiles.root_symlinks.to_list()]

# Libs all end with .so or .so followed by a semantic version.
# Examples: libname.so, libname.so.1, libname.so.1.1
def is_lib(file):
    rparts = file.basename.rpartition(".so")
    if (rparts[1] != ".so"):
        return False
    for char in rparts[2].elems():
        if not (char.isdigit() or char == "."):
            return False
    return True
