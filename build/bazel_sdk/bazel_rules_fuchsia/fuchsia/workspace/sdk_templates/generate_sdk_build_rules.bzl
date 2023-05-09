# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

""" Generates BUILD.bazel rules for items in the Fuchsia SDK. """

load("//fuchsia/workspace:utils.bzl", "symlink_or_copy", "workspace_path")

def _header():
    return """# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# DO NOT EDIT: GENERATED BY generate_sdk_build_rules

package(default_visibility = ["//visibility:public"])

"""

def _get_target_name(name):
    return name.rpartition("/")[2]

def _get_starlark_label_list(values, package_prefix = "", target_prefix = "", target_suffix = ""):
    if not values:
        return ""

    values_str = ""
    for v in values:
        values_str += "    \"" + package_prefix + v + ":" + target_prefix + v.rpartition("/")[2] + target_suffix + "\",\n"
    return values_str

def _get_starlark_list(values, prefix = "", suffix = "", remove_prefix = ""):
    if not values:
        return ""

    values_str = ""
    for v in values:
        if type(v) == "dict":
            values_str += "    " + _get_starlark_dict(v) + ",\n"
        else:
            if len(remove_prefix) > 0 and v.startswith(remove_prefix):
                v = v[len(remove_prefix):]

            # does not add prefix if value is absolute (starts with "/")
            p = prefix
            if v.startswith("/"):
                p = ""
            values_str += "    \"" + p + v + suffix + "\",\n"
    return values_str

def _get_starlark_dict(entries):
    if not entries:
        return ""

    entries_str = ""
    for k in entries.keys():
        v = entries[k]
        if type(v) == "list":
            v_str = "[" + _get_starlark_list(v).replace("\n", "") + "]"
        else:
            v_str = "\"" + str(v) + "\""
        entries_str += "    \"" + k + "\": " + v_str + ",\n"
    return "{" + entries_str + "}"

def _get_parent_workspace(ctx):
    if ctx.attr.parent_sdk:
        return "@" + ctx.attr.parent_sdk.workspace_name
    else:
        return ""

def _find_dep_path(dep, root_for_relative, parent_sdk, parent_sdk_contents):
    """Determines the dep path to use for the input dep.

    For dep exists in both parent and internal SDKs, the one from parent SDK
    takes precedence.
    """

    # Does not add root if value is absolute (starts with "/")
    p = root_for_relative
    if dep.startswith("/"):
        p = ""
    new_dep = p + dep
    if parent_sdk and parent_sdk_contents and new_dep in parent_sdk_contents:
        with_parent = "@{parent_sdk}//{dep}".format(parent_sdk = parent_sdk.workspace_name, dep = new_dep)
        new_dep = with_parent
    elif not dep.startswith("/"):
        new_dep = "//" + new_dep
    return new_dep

def _find_dep_paths(deps, root_for_relative, parent_sdk, parent_sdk_contents):
    return [
        _find_dep_path(dep, root_for_relative, parent_sdk, parent_sdk_contents)
        for dep in deps
    ]

# buildifier: disable=unused-variable
def _generate_bind_library_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._bind_library_template)
    lib_base_path = meta["root"] + "/"
    _merge_template(
        ctx,
        build_file,
        tmpl,
        {
            "{{deps}}": _get_starlark_list(meta["deps"], "//bind/"),
            "{{cc_deps}}": _get_starlark_list(meta["deps"], "//bind/", "", "_cc"),
            "{{name}}": _get_target_name(meta["name"]),
            "{{sources}}": _get_starlark_list(meta["sources"], remove_prefix = lib_base_path),
        },
    )
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(meta["sources"])

# buildifier: disable=unused-variable
def _generate_sysroot_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    files = []
    for ifs_file in meta.get("ifs_files", []):
        files.append("pkg/sysroot/" + ifs_file)
    arch_list = process_context.constants.target_cpus
    for arch in arch_list:
        meta_for_arch = meta["versions"][arch]
        if "debug_libs" in meta_for_arch:
            files.extend(meta_for_arch["debug_libs"])
        if "dist_libs" in meta_for_arch:
            files.extend(meta_for_arch["dist_libs"])
        if "headers" in meta_for_arch:
            files.extend(meta_for_arch["headers"])
        if "link_libs" in meta_for_arch:
            files.extend(meta_for_arch["link_libs"])
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(files)

    tmpl = ctx.path(ctx.attr._sysroot_template)
    _merge_template(ctx, build_file, tmpl, {
        "{{relative_dir}}": relative_dir,
    })

    arch_tmpl = ctx.path(ctx.attr._sysroot_arch_subtemplate)
    for arch in arch_list:
        srcs = {}
        libs_base = meta["versions"][arch]["dist_dir"] + "/dist/lib/"
        for dist_lib in meta["versions"][arch]["dist_libs"]:
            variant, _, _ = dist_lib.removeprefix(libs_base).rpartition("/")
            variant_config = _FUCHSIA_CLANG_VARIANT_MAP[variant]
            srcs.setdefault(variant_config, []).append("//:" + dist_lib)
        strip_prefix = "../%s/%s" % (ctx.attr.name, libs_base)

        per_arch_build_file = build_file.dirname.get_child(arch).get_child("BUILD.bazel")
        ctx.file(per_arch_build_file, content = _header())
        _merge_template(
            ctx,
            per_arch_build_file,
            arch_tmpl,
            {
                "{{srcs}}": _get_starlark_dict(srcs),
                "{{strip_prefix}}": strip_prefix,
            },
        )

def _ffx_tool_files(meta, files_str):
    for (name, collection) in meta.items():
        if name == "executable" or name == "executable_metadata":
            # each set of file collections can only have one each of an
            # executable and executable_metadata item, so we expect to just
            # add one item.
            files_str.append(collection)
        else:
            # any other kind of collection must be an array if present, so
            # we extend the list.
            files_str.extend(collection)

# buildifier: disable=unused-variable
def _generate_ffx_tool_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._ffx_tool_template)

    # Include meta manifest itself because ffx uses it to locate ffx tools.
    files_str = [meta["_meta_path"]]
    if "files" in meta:
        _ffx_tool_files(meta["files"], files_str)

    if "target_files" in meta:
        for arch in meta["target_files"]:
            _ffx_tool_files(meta["target_files"][arch], files_str)

    # normalize root to not have trailing slashes so the
    # slice below won't fail if there's a / at the end.
    meta_root = meta["root"].rstrip("/")
    relative_files = []
    for file in files_str:
        relative_file = file[len(meta["root"]) + 1:]
        relative_files.append(relative_file)

    _merge_template(
        ctx,
        build_file,
        tmpl,
        {
            "{{files}}": _get_starlark_list(relative_files),
        },
    )
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(files_str)

# buildifier: disable=unused-variable
def _generate_host_tool_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._host_tool_template)

    # Include meta manifest itself because ffx uses it to locate host tools.
    files_str = [meta["_meta_path"]]
    if "files" in meta:
        files_str.extend(meta["files"])
    elif "target_files" in meta:
        for arch in meta["target_files"]:
            files_str.extend(meta["target_files"][arch])

    relative_files = []
    for file in files_str:
        relative_file = file[len(meta["root"]) + 1:]
        relative_files.append(relative_file)

    _merge_template(
        ctx,
        build_file,
        tmpl,
        {
            "{{files}}": _get_starlark_list(relative_files),
        },
    )
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(files_str)

# buildifier: disable=unused-variable
def _generate_companion_host_tool_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._companion_host_tool_template)

    # the metadata file itself is needed by the emulator
    files_str = [str(meta["_meta_path"])]

    if "files" in meta:
        files_str.extend(meta["files"])
    elif "target_files" in meta:
        for arch in meta["target_files"]:
            files_str.extend(meta["target_files"][arch])

    relative_files = []
    for file in files_str:
        relative_file = file[len(meta["root"]) + 1:]
        relative_files.append(relative_file)

    # SDK metadata has one companion_host_tool metadata for each architecture, but they are rooted in the same location (//tools)
    # and have the same name (eg aemu_internal). If we just reuse the metadata name, there will be a conflict in Bazel, as there
    # will be two Bazel rules with the same name in the same BUILD.bazel file, one for each host architecture.
    # To avoid this conflict, we find the architecture in the metadata file path and append it to the metadata name, eg aemu_internal_x64
    if meta["_meta_path"].find("x64") >= 0:
        name = meta["name"] + "_x64"
    elif meta["_meta_path"].find("arm64") >= 0:
        name = meta["name"] + "_arm64"
    else:
        name = meta["name"]

    _merge_template(
        ctx,
        build_file,
        tmpl,
        {
            "{{name}}": name,
            "{{files}}": _get_starlark_list(relative_files),
        },
    )

    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(files_str)

# buildifier: disable=unused-variable
def _generate_api_version_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):  # @unused
    tmpl = ctx.path(ctx.attr._api_version_template)
    versions = []
    max_api = -1
    if "data" in meta and "api_levels" in meta["data"]:
        for api_level, value in meta["data"]["api_levels"].items():
            versions.append({"abi_revision": value["abi_revision"], "api_level": api_level})
            max_api = max(max_api, int(api_level))

    # unlike other template rules that affect the corresponding BUILD.bazel file,
    # the api_version template creates a api_version.bzl file that is loaded in
    # the top-level BUILD.bazel created by fuchsia_sdk_repository_template.BUILD.
    bzl_file = ctx.path(build_file).dirname.get_child("api_version.bzl")

    fidl_api_level_override = ctx.attr.fuchsia_api_level_override
    clang_api_level_override = ctx.attr.fuchsia_api_level_override
    if ctx.attr.fuchsia_api_level_override == "HEAD":
        # TODO(https://fxbug.dev/104513) upstream clang support for HEAD
        # Emulate a "HEAD" API level since it is not supported directly by clang.
        # Fuchsia API levels are unsigned 64-bit integers, but clang stores API levels as 32-bit,
        # so we define this as `((uint32_t)-1)`. clang expects API levels to be integer literals.
        clang_api_level_override = 4294967295
        fidl_api_level_override = "HEAD"

    _merge_template(
        ctx,
        bzl_file,
        tmpl,
        {
            "{{default_target_api}}": str(max_api),
            "{{default_clang_target_api}}": str(clang_api_level_override) or str(max_api),
            "{{default_fidl_target_api}}": str(fidl_api_level_override) or str(max_api),
            "{{valid_target_apis}}": _get_starlark_list(versions),
        },
    )

# buildifier: disable=unused-variable
def _generate_fidl_library_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._fidl_library_template)
    lib_base_path = meta["root"] + "/"

    deps = _find_dep_paths(meta["deps"], "fidl/", ctx.attr.parent_sdk, parent_sdk_contents)

    _merge_template(
        ctx,
        build_file,
        tmpl,
        {
            "{{deps}}": _get_starlark_list(deps),
            "{{name}}": _get_target_name(meta["name"]),
            "{{sources}}": _get_starlark_list(meta["sources"], remove_prefix = lib_base_path),
            "{{parent_sdk}}": _get_parent_workspace(ctx),
            # the substitutions below are only used by legacy fidl libraries. Remove them when
            # the legacy fidl cc libraries are removed.
            "{{cc_deps}}": _get_starlark_label_list(deps, "", "", "_cc"),
            "{{llcpp_deps}}": _get_starlark_label_list(deps, "", "", "_llcpp_cc"),
        },
    )
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(meta["sources"])

# buildifier: disable=unused-variable
def _generate_component_manifest_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._component_manifest_template)

    if "data" in meta:
        lib_name = meta["name"]
        for f in meta["data"]:
            if f.endswith(".cml"):
                # The include_path for a package like "/pkg/inspect/client.shard.cml is "/pkg", but
                # this information is not explicitly encoded in the metadata.
                # The code below finds the includepath by breaking the shard filename into three pieces:
                #     <include_path></lib_name/><shard_filename>
                parts = f.partition("/%s/" % lib_name)
                include_path = parts[0]
                shard_name = parts[2]
                name = shard_name.removesuffix(".cml").removesuffix(".cmx").removesuffix(".shard")

                # Keep track of all the labels that we create to add to the toolchain
                target_label = "//{}/{}:{}".format(include_path, lib_name, name)
                process_context.component_manifest_targets.append(target_label)

                _merge_template(
                    ctx,
                    build_file,
                    tmpl,
                    {
                        "{{name}}": name,
                        "{{source}}": shard_name,
                        "{{include_path}}": include_path,
                    },
                )
                process_context.files_to_copy[meta["_meta_sdk_root"]].append(f)

# buildifier: disable=unused-variable
def _generate_cc_source_library_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._cc_library_template)
    lib_base_path = meta["root"] + "/"
    fidl_deps = []
    fidl_llcpp_deps = []
    if "fidl_binding_deps" in meta:
        # Example:    { "binding_type": "hlcpp", "deps": [ "fuchsia.images" ] }
        for deps_per_type in meta["fidl_binding_deps"]:
            binding_type = deps_per_type["binding_type"]
            if binding_type == "hlcpp":
                suffix = "cc"
            else:
                suffix = binding_type
            for fidl in deps_per_type["deps"]:
                dep_path = _find_dep_path(fidl, "fidl/", ctx.attr.parent_sdk, parent_sdk_contents)
                fidl_deps.append(dep_path + ":" + fidl + "_" + suffix)
    elif "fidl_deps" in meta:
        for fidl in meta["fidl_deps"]:
            dep_path = _find_dep_path(fidl, "fidl/", ctx.attr.parent_sdk, parent_sdk_contents)
            fidl_deps.append(dep_path + ":" + fidl + "_cc")
            fidl_llcpp_deps.append(dep_path + ":" + fidl + "_llcpp_cc")

    deps = _find_dep_paths(meta["deps"], "pkg/", ctx.attr.parent_sdk, parent_sdk_contents)

    _merge_template(
        ctx,
        build_file,
        tmpl,
        {
            "{{deps}}": _get_starlark_list(deps),
            "{{fidl_deps}}": _get_starlark_list(fidl_deps),
            "{{fidl_llcpp_deps}}": _get_starlark_list(fidl_llcpp_deps),
            "{{name}}": _get_target_name(meta["name"]),
            "{{sources}}": _get_starlark_list(meta["sources"], remove_prefix = lib_base_path),
            "{{headers}}": _get_starlark_list(meta["headers"], remove_prefix = lib_base_path),
            "{{relative_include_dir}}": meta["include_dir"][len(lib_base_path):],
        },
    )
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(meta["sources"])
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(meta["headers"])

# Maps a Bazel cpu name to its Fuchsia name.
_TO_FUCHSIA_CPU_NAME_MAP = {
    "x86_64": "x64",
    "k8": "x64",
    "aarch64": "arm64",
}

def _to_fuchsia_cpu_name(cpu_name):
    return _TO_FUCHSIA_CPU_NAME_MAP.get(cpu_name, cpu_name)

# Maps a Fuchsia cpu name to the corresponding config_setting() label in
# @fuchsia_sdk//fuchsia/constraints
_FUCHSIA_CPU_CONSTRAINT_MAP = {
    "x64": "@fuchsia_sdk//fuchsia/constraints:cpu_x64",
    "arm64": "@fuchsia_sdk//fuchsia/constraints:cpu_arm64",
}

# Maps a variant name to the corresponding config_setting() label in
# @fuchsia_clang
_FUCHSIA_CLANG_VARIANT_MAP = {
    "": "@fuchsia_clang//:novariant",
    "asan": "@fuchsia_clang//:asan_variant",
}

def _generate_cc_prebuilt_library_build_rules(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents):
    tmpl = ctx.path(ctx.attr._cc_prebuilt_library_template)
    tmpl_linklib = ctx.path(ctx.attr._cc_prebuilt_library_linklib_subtemplate)
    tmpl_distlib = ctx.path(ctx.attr._cc_prebuilt_library_distlib_subtemplate)
    lib_base_path = meta["root"] + "/"
    deps = _find_dep_paths(meta["deps"], "pkg/", ctx.attr.parent_sdk, parent_sdk_contents)
    subs = {
        "{{deps}}": _get_starlark_list(deps),
        "{{name}}": _get_target_name(meta["name"]),
        "{{headers}}": _get_starlark_list(meta["headers"], remove_prefix = lib_base_path),
        "{{relative_include_dir}}": meta["include_dir"][len(lib_base_path):],
    }
    files = meta["headers"]
    if "ifs" in meta:
        files.append(lib_base_path + meta["ifs"])
    process_context.files_to_copy[meta["_meta_sdk_root"]].extend(files)

    prebuilt_select = {}
    dist_select = {}

    # add all supported architectures to the select, even if they are not available in the current SDK,
    # so that SDKs for different architectures can be composed by a simple directory merge.
    arch_list = process_context.constants.target_cpus
    for arch in arch_list:
        constraint = _FUCHSIA_CPU_CONSTRAINT_MAP[arch]
        dist_select[constraint] = ["//%s/%s:dist" % (relative_dir, arch)]
        prebuilt_select[constraint] = ["//%s/%s:prebuilts" % (relative_dir, arch)]

    has_distlibs = False

    for arch in arch_list:
        per_arch_build_file = build_file.dirname.get_child(arch).get_child("BUILD.bazel")
        ctx.file(per_arch_build_file, content = _header())

        linklib = meta["binaries"][arch]["link"]
        _merge_template(ctx, per_arch_build_file, tmpl_linklib, {
            "{{link_lib}}": "//:" + linklib,
            "{{library_type}}": meta["format"],
        })
        process_context.files_to_copy[meta["_meta_sdk_root"]].append(linklib)

        if "dist" in meta["binaries"][arch]:
            has_distlibs = True
            distlib = meta["binaries"][arch]["dist"]
            _merge_template(ctx, per_arch_build_file, tmpl_distlib, {
                "{{dist_lib}}": "//:" + distlib,
                "{{dist_path}}": meta["binaries"][arch]["dist_path"],
            })
            process_context.files_to_copy[meta["_meta_sdk_root"]].append(distlib)

    prebuilt_select_str = "fuchsia_select(" + _get_starlark_dict(prebuilt_select) + ")"

    # Assumption: if one architecture doesn't have distlib binaries for this library,
    # the other architectures will also not have them.
    if has_distlibs:
        dist_select_str = "fuchsia_select(" + _get_starlark_dict(dist_select) + ")"
    else:
        dist_select_str = "[]"

    subs.update({
        "{{prebuilt_select}}": prebuilt_select_str,
        "{{dist_select}}": dist_select_str,
    })
    _merge_template(ctx, build_file, tmpl, subs)

def _merge_template(ctx, target_build_file, template_file, subs):
    if ctx.path(target_build_file).exists:
        existing_content = ctx.read(target_build_file)
    else:
        existing_content = ""

    ctx.template(target_build_file, template_file, subs)

    if existing_content != "":
        new_content = ctx.read(target_build_file)
        ctx.file(target_build_file, content = existing_content + "\n" + new_content)

def _process_dir(ctx, relative_dir, libraries, process_context, parent_sdk_contents):
    generators = {
        "sysroot": _generate_sysroot_build_rules,
        "fidl_library": _generate_fidl_library_build_rules,
        "companion_host_tool": _generate_companion_host_tool_build_rules,
        "host_tool": _generate_host_tool_build_rules,
        "ffx_tool": _generate_ffx_tool_build_rules,
        "cc_source_library": _generate_cc_source_library_build_rules,
        "cc_prebuilt_library": _generate_cc_prebuilt_library_build_rules,
        "bind_library": _generate_bind_library_build_rules,
        "component_manifest": _generate_component_manifest_rules,
        "version_history": _generate_api_version_rules,
    }

    build_file = ctx.path(relative_dir).get_child("BUILD.bazel")

    for meta in libraries:
        t = _type_from_meta(meta)

        if t not in generators:
            continue

        if not build_file.exists:
            ctx.file(build_file, content = _header())

        generator = generators[t]
        generator(ctx, meta, relative_dir, build_file, process_context, parent_sdk_contents)

def _write_cmc_includes(ctx, process_context):
    # write the cmc includes to the BUILD file
    build_file = _root_build_file(ctx)
    _merge_template(ctx, build_file, ctx.attr._component_manifest_collection_template, {
        "{{name}}": "cmc_includes",
        "{{deps}}": str(process_context.component_manifest_targets),
    })

def _root_build_file(ctx):
    return ctx.path("BUILD.bazel")

def _type_from_meta(meta):
    if "type" in meta:
        return meta["type"]
    elif "data" in meta and "type" in meta["data"]:
        return meta["data"]["type"]
    else:
        fail("Internal SDK generation error: cannot identify type of library: %s" % meta)

def _path_in_root(repo_ctx, root, rel_path):
    return repo_ctx.path("%s/%s" % (root, rel_path))

def sdk_id_from_manifests(ctx, manifests):
    # buildifier: disable=function-docstring-args
    # buildifier: disable=function-docstring-return
    """ Gets the SDK id from the given manifests.

    This assumes all of the manifests have the same id an thus only uses the first manifest
    in the list. If no id is found an empty string will be returned.
    """
    id = ""
    if len(manifests) > 0:
        manifest = manifests[0]
        root = manifest.get("root")
        manifest_path = manifest.get("manifest")
        json_obj = json.decode(ctx.read(_path_in_root(ctx, root, manifest_path)))
        id = json_obj.get("id")

    return id

# buildifier: disable=function-docstring
def load_parent_sdk_metadata(ctx, parent_sdk_contents):
    if not ctx.attr.parent_sdk or not ctx.attr.parent_sdk_local_paths:
        return

    for path in ctx.attr.parent_sdk_local_paths:
        local_sdk = ctx.path(workspace_path(ctx, path))
        if not local_sdk.exists:
            fail("Cannot find parent SDK: %s" % local_sdk)

        root = "%s/." % local_sdk
        manifest_path = "meta/manifest.json"
        json_obj = json.decode(ctx.read(_path_in_root(ctx, root, manifest_path)))

        for part in json_obj["parts"]:
            meta_file_rel = part["meta"]
            meta_file_with_root = _path_in_root(ctx, root, meta_file_rel)
            meta = json.decode(ctx.read(meta_file_with_root))

            if "root" in meta:
                dir = meta["root"]
            else:
                dir = meta_file_rel.rpartition("/")[0]

            if "name" in meta:
                key = "%s" % dir
                parent_sdk_contents[key] = meta

def generate_sdk_constants(repo_ctx, manifests):
    """Generates generated_constants.bzl from the sdk metadata

    Args:
        repo_ctx: the repository context
        manifests: a list of paths to the meta data manifests.

    Returns:
        A struct mapping the content of generated_constants.bzl
    """
    host_cpu_names_set = {}
    target_cpu_names_set = {}
    for manifest_obj in manifests:
        root = manifest_obj.get("root")
        manifest_path = manifest_obj.get("manifest")
        json_obj = json.decode(repo_ctx.read(_path_in_root(repo_ctx, root, manifest_path)))
        host_os = json_obj["arch"]["host"]
        host_cpu = _to_fuchsia_cpu_name(host_os.split("-")[0])
        host_cpu_names_set[host_cpu] = None
        for cpu_name in json_obj["arch"]["target"]:
            target_cpu_names_set[cpu_name] = None

    host_cpu_names = sorted(host_cpu_names_set.keys())
    target_cpu_names = sorted(target_cpu_names_set.keys())

    constants = struct(
        host_cpus = host_cpu_names,
        target_cpus = target_cpu_names,
    )
    generated_content = "# AUTO-GENERATED - DO NOT EDIT!\n\n"
    generated_content += "# The following list of CPU names use Fuchsia conventions.\n"
    generated_content += "constants = %s\n" % constants

    repo_ctx.file("generated_constants.bzl", generated_content)

    return constants

def generate_sdk_build_rules(ctx, manifests, copy_content_strategy, constants, filter_types = None, exclude_types = None):
    """ Generates BUILD.bazel rules from the sdk metadata

    Args:
        ctx: the repository context
        manifests: a list of paths to the meta data manifests.
        copy_content_strategy: "symlink" to create symlinks to Fuchsia SDK artifacts or "copy" to attempt
                to create hardlinks, or standard copy if hardlinks are not possible
        constants: A struct returned by generated_sdk_constants
        filter_types: tuple of sdk element types. If given, do not process any sdk element types that are not in this tuple
        exclude_types: tuple of sdk element types. If given, do not process any sdk element types in this tuple
    """
    files_to_copy = {}  # key: sdk root path, value: list of files (string) relative to sdk
    dir_to_meta = {}
    parent_sdk_contents = {}
    load_parent_sdk_metadata(ctx, parent_sdk_contents)
    for manifest_obj in manifests:
        root = manifest_obj.get("root")
        manifest_path = manifest_obj.get("manifest")
        json_obj = json.decode(ctx.read(_path_in_root(ctx, root, manifest_path)))

        # Since all SDK manifests are in the same location, if we copy them all, they would clobber. So we only copy the first one and assume it is the main (core)
        files_to_copy[str(root)] = [manifest_path] if len(files_to_copy) == 0 else []

        # Collect all of the parts into a mapping for later templating. We need to
        # collect these here since we need to apply special logic to certain types
        # and cannot just generically apply templating to all types.
        for part in json_obj["parts"]:
            meta_file_rel = part["meta"]
            meta_file_with_root = _path_in_root(ctx, root, meta_file_rel)
            meta = json.decode(ctx.read(meta_file_with_root))
            t = _type_from_meta(meta)
            if (filter_types and t not in filter_types) or (exclude_types and t in exclude_types):
                continue
            meta["_meta_path"] = meta_file_rel
            meta["_meta_sdk_root"] = str(root)
            if "root" in meta:
                dir = meta["root"]
            else:
                dir = meta_file_rel.rpartition("/")[0]

            if "name" in meta:
                key = "%s" % dir
                if key in parent_sdk_contents:
                    # TODO(fxbug.dev/118016): compare meta with parent and fail if they are not similar.
                    continue

            # we need to group all meta files per root directory, but only process each file once
            if dir in dir_to_meta:
                dir_to_meta[dir][meta_file_rel] = meta
            else:
                dir_to_meta[dir] = {meta_file_rel: meta}

    process_context = struct(
        files_to_copy = files_to_copy,
        component_manifest_targets = [],
        constants = constants,
    )

    for dir in dir_to_meta.keys():
        _process_dir(ctx, dir, dir_to_meta[dir].values(), process_context, parent_sdk_contents)

    _write_cmc_includes(ctx, process_context)

    symlink_or_copy(ctx, copy_content_strategy, files_to_copy)
