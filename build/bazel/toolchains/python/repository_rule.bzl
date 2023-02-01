# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Repository rule used to generate a better Python runtime for the Fuchsia build.

The idea is to use a single zip archive to hold all lib/python<version>/ modules
to drastically limit the number of files exposed in Bazel sandboxes. From 4940 to 3!

To do so requires the following:

1) A zip archive that contains all files from <python_prefix>/lib/python<version>/,
   named "lib_python.zip" in this repository.

2) A symlink to the real python interpreter, named "python3-real" in the repository.

3) A wrapper script that sets PYTHONPATH to point to lib_python.zip directly,
   and invokes python3-real with the -S flag (to avoid reading site-specific
   module installs).

None of the definitions here are Fuchsia specific.
"""

def _compact_python_runtime_impl(repo_ctx):
    repo_ctx.file("WORKSPACE.bzl", "")

    # Create a symlink to the real interpreter.
    project_root = str(repo_ctx.workspace_root) + "/"
    python3_real = "python3-real"
    repo_ctx.symlink(project_root + repo_ctx.attr.interpreter_path, python3_real)

    # Either symlink or create a zip archive that contains the content of
    # <python_install_dir>/lib/python<version>/
    lib_python_zip = "lib_python.zip"
    if repo_ctx.attr.lib_python_zip:
        if repo_ctx.attr.lib_python_path:
            fail("Only one of lib_python_zip or lib_python_path can be defined!")
        repo_ctx.symlink(project_root + repo_ctx.attr.lib_python_zip, lib_python_zip)
    elif repo_ctx.attr.lib_python_path:
        # Create the zip archive using a custom Python script, since this is
        # more portable than relying on a host `zip` tool being available.
        # On Linux, this is slightly slower than using the host zip command
        # (i.e. 0.77s vs 0.483s).
        zip_directory_script = repo_ctx.path(project_root + "build/bazel/scripts/zip-directory.py")
        repo_ctx.execute(
            [
                str(zip_directory_script),
                str(repo_ctx.path(lib_python_zip)),
                project_root + repo_ctx.attr.lib_python_path,
            ],
            quiet = False,  # False for debugging!
        )
    else:
        fail("One of lib_python_zip or lib_python_path must be defined.")

    # Create a launcher shell script named 'python3' that invokes 'python3-real'
    #
    # - PYTHONHOME is set to _SCRIPT_DIR to ensure sys.path only contains
    #   paths relative to it. Otherwise, some paths hard-coded in the interpreter
    #   binary will be used (e.g. `/work/out/python3`), which could lead to
    #   bad surprises.
    #
    # - PYTHONPATH is extended to point to the zip archive, and allows the
    #   interpreter to find all system libraries from it.
    #
    # - The `-S` flag disables site-specific module lookups.
    #
    # - The `-s` flag disables user-specific module lookups.
    #
    # Note that `python3` also supports the `-I` flag to run in `isolated` mode,
    # where PYTHONPATH and PYTHONHOME are ignored, but this forces sys.path to
    # strictly hard-coded values that are unusable here.
    #
    python3_launcher = "python3"
    repo_ctx.file(
        python3_launcher,
        content = '''#!/bin/bash
# AUTO-GENERATED - DO NOT EDIT
readonly _SCRIPT_DIR="$(dirname "${{BASH_SOURCE[0]}}")"
PYTHONHOME="${{_SCRIPT_DIR}}" \\
PYTHONPATH="${{_SCRIPT_DIR}}/{lib_python_zip}:${{PYTHONPATH}}" \\
exec "${{_SCRIPT_DIR}}/{python3_real}" -S -s "$@"
'''.format(python3_real = python3_real, lib_python_zip = lib_python_zip),
        executable = True,
    )

    # Create targets to be used in a py_runtime() declaration.

    repo_ctx.template(
        "BUILD.bazel",
        project_root + "build/bazel/toolchains/python/template.BUILD.bazel",
        substitutions = {
            "{python_launcher}": python3_launcher,
            "{python_real}": python3_real,
            "{lib_python_zip}": lib_python_zip,
            "{repository_dir}": repo_ctx.attr.name,
        },
    )

compact_python_runtime_repository = repository_rule(
    implementation = _compact_python_runtime_impl,
    attrs = {
        "interpreter_path": attr.string(
            doc = "Path to the Python interpreter, relative to project root.",
            mandatory = True,
        ),
        "lib_python_path": attr.string(
            doc = "Path to the lib/python<version> directory, relative to project root." +
                  "Incompatible with lib_python_zip. Either one is required.",
        ),
        "lib_python_zip": attr.string(
            doc = "Path to prebuilt lib_python.zip archive, relative to project root." +
                  "Incompatible with lib_python_path. Either one is required. For best " +
                  "build performance, do not use compression.",
        ),
    },
)
