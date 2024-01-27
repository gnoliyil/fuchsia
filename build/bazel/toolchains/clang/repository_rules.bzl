# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Repository rules used to populate Clang-based repositories."""

load(
    "@fuchsia_sdk_common//:toolchains/clang/repository_utils.bzl",
    "prepare_clang_repository",
)

# generate_prebuilt_clang_toolchain_repository() is used to generate
# an external repository that contains a copy of the prebuilt
# Clang toolchain that is already available in the Fuchsia source
# tree, then adding Bazel specific files there.
#
# Because repository_ctx.symlink only works for real files and
# not directories, we have to invoke a special script to perform
# the copy, possibly using hard links.
#
def _generate_prebuilt_clang_toolchain_impl(repo_ctx):
    prepare_clang_repository(repo_ctx, repo_ctx.attr.clang_install_dir)
    workspace_dir = str(repo_ctx.workspace_root)

    if hasattr(repo_ctx.attr, "repository_version_file"):
        # Force Bazel to record an association with this file, if it is provided.
        # If its content changes (for example after a `jiri update` that modifies
        # the toolchain directory), this Bazel repository will be automatically
        # re-generated.
        repo_ctx.path(Label("@//:" + repo_ctx.attr.repository_version_file))

    repo_ctx.file("WORKSPACE.bazel", content = "")

    repo_ctx.symlink(
        workspace_dir + "/build/bazel/toolchains/clang/prebuilt_clang.BUILD.bazel",
        "BUILD.bazel",
    )

generate_prebuilt_clang_toolchain_repository = repository_rule(
    implementation = _generate_prebuilt_clang_toolchain_impl,
    attrs = {
        "clang_install_dir": attr.string(
            doc = "Clang installation directory, relative to workspace root.",
            mandatory = True,
        ),
        "repository_version_file": attr.string(
            doc = "Clang toolchain content identification file, relative to workspace root.",
            mandatory = False,
        ),
    },
)

def _generate_prebuilt_llvm_repository_impl(repo_ctx):
    repo_ctx.file("WORKSPACE.bazel", content = "")

    workspace_dir = str(repo_ctx.workspace_root)

    # Symlink the content of the LLVM installation directory into the repository.
    # This allows us to add Bazel-specific files in this location.

    # Resolve full path of script before executing it, this ensures that the repository
    # rule will be re-run everytime the invoked script is modified.
    script_path = str(repo_ctx.path(Label("@//:build/bazel/scripts/symlink-directory.py")))

    repo_ctx.execute(
        [
            script_path,
            repo_ctx.attr.llvm_install_dir,
            ".",
        ],
        quiet = False,  # False for debugging!
    )

    # Symlink the BUILD.bazel file.
    repo_ctx.symlink(
        workspace_dir + "/build/bazel/toolchains/clang/prebuilt_llvm.BUILD.bazel",
        "BUILD.bazel",
    )

generate_prebuilt_llvm_repository = repository_rule(
    implementation = _generate_prebuilt_llvm_repository_impl,
    attrs = {
        "llvm_install_dir": attr.string(
            mandatory = True,
            doc = "Location of prebuilt LLVM toolchain installation",
        ),
    },
)
