# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Rule for creating product bundle for flashing Fuchsia images to target devices."""

load(
    ":providers.bzl",
    "FuchsiaAssemblyConfigInfo",
    "FuchsiaProductBundleInfo",
    "FuchsiaProductImageInfo",
    "FuchsiaScrutinyConfigInfo",
)

def _scrutiny_validation(
        ctx,
        ffx_tool,
        pb_out_dir,
        scrutiny_config,
        is_recovery = False):
    deps = []
    ffx_invocation = [
        ffx_tool.path,
        "scrutiny",
        "verify",
    ]
    if is_recovery:
        ffx_invocation += ["--recovery"]

    deps += [_verify_bootfs_filelist(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.bootfs_files,
        scrutiny_config.bootfs_packages,
    )]
    deps += [_verify_kernel_cmdline(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.kernel_cmdline,
    )]
    deps += _verify_route_sources(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.routes_config_golden,
    )
    deps += _verify_component_resolver_allowlist(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.component_resolver_allowlist,
    )
    deps += _verify_routes(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.component_route_exceptions,
        scrutiny_config.component_tree_config,
    )
    deps += _verify_base_packages(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.base_packages,
    )
    deps += _verify_structured_config(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        scrutiny_config.structured_config_policy,
    )
    deps += _extract_structured_config(
        ctx,
        ffx_tool,
        pb_out_dir,
        is_recovery,
    )
    return deps

def _verify_bootfs_filelist(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        zbi_bootfs_filelist_goldens,
        zbi_bootfs_packages_goldens):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_bootfs.stamp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "bootfs",
        "--product-bundle",
        pb_out_dir.path,
    ]

    for golden_file in zbi_bootfs_filelist_goldens:
        _ffx_invocation += [
            "--golden",
            golden_file.path,
        ]
    for golden_package in zbi_bootfs_packages_goldens:
        _ffx_invocation += [
            "--golden-packages",
            golden_package.path,
        ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir] + zbi_bootfs_filelist_goldens + zbi_bootfs_packages_goldens

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file],
        command = "\n".join(script_lines),
        progress_message = "Verify Bootfs file list for %s" % ctx.label.name,
    )
    return stamp_file

def _verify_kernel_cmdline(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        zbi_kernel_cmdline_goldens):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_kernel.stamp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "kernel-cmdline",
        "--product-bundle",
        pb_out_dir.path,
    ]

    for golden_file in zbi_kernel_cmdline_goldens:
        _ffx_invocation += [
            "--golden",
            golden_file.path,
        ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir] + zbi_kernel_cmdline_goldens

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file],
        command = "\n".join(script_lines),
        progress_message = "Verify Kernel Cmdline for %s" % ctx.label.name,
    )
    return stamp_file

def _verify_route_sources(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        routes_config_golden):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_route.stamp")
    tmp_dir = ctx.actions.declare_directory(ctx.label.name + "_route.tmp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "--tmp-dir",
        tmp_dir.path,
        "route-sources",
        "--product-bundle",
        pb_out_dir.path,
        "--config",
        routes_config_golden.path,
    ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir, routes_config_golden]

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file, tmp_dir],
        command = "\n".join(script_lines),
        progress_message = "Verify Route sources for %s" % ctx.label.name,
    )
    return [stamp_file, tmp_dir]

def _verify_component_resolver_allowlist(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        component_resolver_allowlist):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_component_resolver.stamp")
    tmp_dir = ctx.actions.declare_directory(ctx.label.name + "_component_resolver.tmp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "--tmp-dir",
        tmp_dir.path,
        "component-resolvers",
        "--product-bundle",
        pb_out_dir.path,
        "--allowlist",
        component_resolver_allowlist.path,
    ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir, component_resolver_allowlist]

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file, tmp_dir],
        command = "\n".join(script_lines),
        progress_message = "Verify Component Resolver for %s" % ctx.label.name,
    )
    return [stamp_file, tmp_dir]

def _verify_routes(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        allow_lists,
        component_tree_config):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_routes.stamp")
    tmp_dir = ctx.actions.declare_directory(ctx.label.name + "_routes.tmp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "--tmp-dir",
        tmp_dir.path,
        "routes",
        "--product-bundle",
        pb_out_dir.path,
    ]
    if component_tree_config:
        _ffx_invocation += [
            "--component-tree-config",
            component_tree_config.path,
        ]
    for allow_list in allow_lists:
        _ffx_invocation += [
            "--allowlist",
            allow_list.path,
        ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir, component_tree_config] + allow_lists

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file, tmp_dir],
        command = "\n".join(script_lines),
        progress_message = "Verify Routes for %s" % ctx.label.name,
    )
    return [stamp_file, tmp_dir]

def _verify_base_packages(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        base_packages):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_static_pkgs.stamp")
    tmp_dir = ctx.actions.declare_directory(ctx.label.name + "_static_pkgs.tmp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "--tmp-dir",
        tmp_dir.path,
        "static-pkgs",
        "--product-bundle",
        pb_out_dir.path,
        "--golden",
        base_packages.path,
    ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir, base_packages]

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file, tmp_dir],
        command = "\n".join(script_lines),
        progress_message = "Verify Static pkgs for %s" % ctx.label.name,
    )
    return [stamp_file, tmp_dir]

def _verify_structured_config(
        ctx,
        ffx_invocation,
        ffx_tool,
        pb_out_dir,
        structured_config_policy):
    stamp_file = ctx.actions.declare_file(ctx.label.name + "_structured_config.stamp")
    tmp_dir = ctx.actions.declare_directory(ctx.label.name + "_structured_config.tmp")
    _ffx_invocation = []
    _ffx_invocation.extend(ffx_invocation)
    _ffx_invocation += [
        "--stamp",
        stamp_file.path,
        "--tmp-dir",
        tmp_dir.path,
        "structured-config",
        "--product-bundle",
        pb_out_dir.path,
        "--policy",
        structured_config_policy.path,
    ]

    script_lines = [
        "set -e",
        " ".join(_ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir, structured_config_policy]

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [stamp_file, tmp_dir],
        command = "\n".join(script_lines),
        progress_message = "Verify structured config for %s" % ctx.label.name,
    )
    return [stamp_file, tmp_dir]

def _extract_structured_config(ctx, ffx_tool, pb_out_dir, is_recovery):
    structured_config = ctx.actions.declare_file(ctx.label.name + "_structured_config")
    depfile = ctx.actions.declare_file(ctx.label.name + "_depfile")
    ffx_invocation = [
        ffx_tool.path,
        "scrutiny",
        "extract",
        "structured-config",
        "--product-bundle",
        pb_out_dir.path,
        "--output",
        structured_config.path,
        # These args are currently required, but the outputs are ignored.
        "--build-path",
        pb_out_dir.path,
        "--depfile",
        depfile.path,
    ]

    if is_recovery:
        ffx_invocation += ["--recovery"]

    script_lines = [
        "set -e",
        " ".join(ffx_invocation),
    ]
    inputs = [ffx_tool, pb_out_dir]

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [structured_config, depfile],
        command = "\n".join(script_lines),
        progress_message = "Extract structured config for %s" % ctx.label.name,
    )
    return [structured_config, depfile]

def _fuchsia_product_bundle_impl(ctx):
    partitions_configuration = ctx.attr.partitions_config[FuchsiaAssemblyConfigInfo].config
    system_a_out = ctx.attr.product_image[FuchsiaProductImageInfo].images_out
    ffx_tool = ctx.toolchains["@rules_fuchsia//fuchsia:toolchain"].ffx
    pb_out_dir = ctx.actions.declare_directory(ctx.label.name + "_out")

    # Gather all the arguments to pass to ffx.
    ffx_invocation = [
        "$ORIG_DIR/$FFX",
        "--config \"product.experimental=true\"",
        "product",
        "create",
        "--partitions $ORIG_DIR/$PARTITIONS_PATH",
        "--system-a $ORIG_DIR/$SYSTEM_A_MANIFEST",
        "--out-dir $ORIG_DIR/$OUTDIR",
    ]

    # Gather the environment variables needed in the script.
    env = {
        "FFX": ffx_tool.path,
        "OUTDIR": pb_out_dir.path,
        "PARTITIONS_PATH": partitions_configuration.path,
        "SYSTEM_A_MANIFEST": system_a_out.path + "/images.json",
        "ARTIFACTS_BASE_PATH": ctx.attr.artifacts_base_path,
    }

    # Gather all the inputs.
    inputs = [partitions_configuration] + ctx.files.product_image

    # If recovery is supplied, add it to the product bundle.
    if ctx.attr.recovery != None:
        system_r_out = ctx.attr.recovery[FuchsiaProductImageInfo].images_out
        ffx_invocation.append("--system-r $ORIG_DIR/$SYSTEM_R_MANIFEST")
        env["SYSTEM_R_MANIFEST"] = system_r_out.path + "/images.json"
        inputs.extend(ctx.files.recovery)

    # If update info is supplied, add it to the product bundle.
    if ctx.file.update_version_file != None:
        if ctx.file.repository_keys == None:
            fail("Repository keys must be supplied in order to build an update package")

        ffx_invocation.extend([
            "--update-package-version-file $ORIG_DIR/$UPDATE_VERSION_FILE",
            "--update-package-epoch $UPDATE_EPOCH",
        ])
        env["UPDATE_VERSION_FILE"] = ctx.file.update_version_file.path
        env["UPDATE_EPOCH"] = ctx.attr.update_epoch
        inputs.append(ctx.file.update_version_file)

    # If a repository is supplied, add it to the product bundle.
    if ctx.file.repository_keys != None:
        ffx_invocation.append("--tuf-keys $ORIG_DIR/$REPOKEYS")
        env["REPOKEYS"] = ctx.file.repository_keys.path
        inputs.append(ctx.file.repository_keys)

    script_lines = [
        "set -e",
        "ORIG_DIR=$(pwd)",
        "cd $ARTIFACTS_BASE_PATH",
        " ".join(ffx_invocation),
    ]

    if ctx.file.repository_keys != None:
        script_lines.append("cp -r $ORIG_DIR/$REPOKEYS $ORIG_DIR/$OUTDIR")
    script = "\n".join(script_lines)

    ctx.actions.run_shell(
        inputs = inputs,
        outputs = [pb_out_dir],
        command = script,
        env = env,
        progress_message = "Creating product bundle for %s" % ctx.label.name,
    )
    deps = [pb_out_dir]

    # Scrutiny Validation
    if ctx.attr.product_image_scrutiny_config:
        product_image_scrutiny_config = ctx.attr.product_image_scrutiny_config[FuchsiaScrutinyConfigInfo]
        deps += _scrutiny_validation(ctx, ffx_tool, pb_out_dir, product_image_scrutiny_config)
    if ctx.attr.recovery_scrutiny_config:
        recovery_scrutiny_config = ctx.attr.recovery_scrutiny_config[FuchsiaScrutinyConfigInfo]
        deps += _scrutiny_validation(ctx, ffx_tool, pb_out_dir, recovery_scrutiny_config, True)

    return [DefaultInfo(files = depset(direct = deps)), FuchsiaProductBundleInfo(
        product_bundle = pb_out_dir,
        is_remote = False,
    )]

fuchsia_product_bundle = rule(
    doc = """Creates pb for flashing Fuchsia images to target devices.""",
    implementation = _fuchsia_product_bundle_impl,
    toolchains = ["@rules_fuchsia//fuchsia:toolchain"],
    attrs = {
        "partitions_config": attr.label(
            doc = "Partitions config to use, can be a fuchsia_partitions_configuration target, or a JSON file",
            allow_single_file = [".json"],
            mandatory = True,
        ),
        "product_image": attr.label(
            doc = "fuchsia_product_image target to put in slot A",
            providers = [FuchsiaProductImageInfo],
            mandatory = True,
        ),
        "recovery": attr.label(
            doc = "fuchsia_product_image target to put in slot R",
            providers = [FuchsiaProductImageInfo],
        ),
        "artifacts_base_path": attr.string(
            doc = "Artifacts base directories that items in config files are relative to.",
            default = ".",
        ),
        "repository_keys": attr.label(
            doc = "Directory of repository keys",
            allow_single_file = True,
            default = None,
        ),
        "update_version_file": attr.label(
            doc = "version file needed to create update package",
            allow_single_file = True,
            default = None,
        ),
        "update_epoch": attr.string(
            doc = "epoch needed to create update package",
            default = "1",
        ),
        "product_image_scrutiny_config": attr.label(
            doc = "Scrutiny config for slot A",
            providers = [FuchsiaScrutinyConfigInfo],
        ),
        "recovery_scrutiny_config": attr.label(
            doc = "Scrutiny config for recovery",
            providers = [FuchsiaScrutinyConfigInfo],
        ),
    },
)
