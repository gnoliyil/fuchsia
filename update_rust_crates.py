#!/usr/bin/env python
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import paths
import platform
import re
import shutil
import subprocess
import sys
import tempfile

sys.path += [os.path.join(paths.FUCHSIA_ROOT, "third_party", "pytoml")]
import pytoml as toml

from check_rust_licenses import check_licenses


CONFIGS = [
    "apps/xi/modules/xi-core",
    "lib/fidl/rust/fidl",
    "rust/magenta-rs",
    "rust/magenta-rs/magenta-sys",
    "rust/rust_sample_module",
]

NATIVE_LIBS = {
    "cairo": "//third_party/cairo",
}


def get_cargo_bin():
    host_os = platform.system()
    if host_os == "Darwin":
        host_triple = "x86_64-apple-darwin"
    elif host_os == "Linux":
        host_triple = "x86_64-unknown-linux-gnu"
    else:
        raise Exception("Platform not supported: %s" % host_os)
    return os.path.join(paths.FUCHSIA_ROOT, "buildtools", "rust",
                        "rust-%s" % host_triple, "bin", "cargo")


def parse_dependencies(lock_path):
    """Extracts the crate dependency tree from a lockfile."""
    result = []
    with open(lock_path, "r") as lock_file:
        content = toml.load(lock_file)
        dep_matcher = re.compile("^([^\s]+)\s([^\s]+)")
        for package in content["package"]:
            deps = []
            if "dependencies" in package:
                for dep in package["dependencies"]:
                    match = dep_matcher.match(dep)
                    if match:
                        deps.append("%s-%s" % (match.group(1), match.group(2)))
            label = "%s-%s" % (package["name"], package["version"])
            result.append({
                "name": package["name"],
                "label": label,
                "deps": deps,
            })
    return result


def filter_non_vendor_crates(crates, vendor_dir):
    """Removes crates that are not vendored from the given list."""
    def is_3p(c): return os.path.isdir(os.path.join(vendor_dir, c["label"]))
    return filter(is_3p, crates)


def add_native_libraries(crates, vendor_dir):
    """Returns true if all native libraries could be identified and added to
       the given crate metadata."""
    result = True
    for crate in crates:
        config_path = os.path.join(vendor_dir, crate["label"], "Cargo.toml")
        with open(config_path, "r") as config_file:
            config = toml.load(config_file)
            if "links" in config["package"]:
                library = config["package"]["links"]
                if library not in NATIVE_LIBS:
                    print("Unknown native library: %s" % library)
                    result = False
                    continue
                crate["native_lib"] = library
    return result


def generate_build_file(build_path, crates):
    """Creates a BUILD.gn file for the given crates."""
    crates.sort(key=lambda c: c["label"])
    with open(build_path, "w") as build_file:
        build_file.write("""# Generated by //scripts/update_rust_crates.py.

import("//build/rust/rust_info.gni")
""")
        for info in crates:
            build_file.write("""
rust_info("%s") {
  name = "%s"

  deps = [
""" % (info["label"], info["name"]))
            for dep in info["deps"]:
                build_file.write("    \":%s\",\n" % dep)
            build_file.write("  ]\n")
            if "native_lib" in info:
                lib = info["native_lib"]
                build_file.write("""
  native_lib = \"%s\"

  non_rust_deps = [
    \"%s\",
  ]
""" % (lib, NATIVE_LIBS[lib]))
            build_file.write("}\n")


def call_or_exit(args, dir):
    if subprocess.call(args, cwd=dir) != 0:
        raise Exception("Command failed in %s: %s" % (dir, " ".join(args)))


def main():
    parser = argparse.ArgumentParser("Updates third-party Rust crates")
    parser.add_argument("--cargo-vendor",
                        help="Path to the cargo-vendor command",
                        required=True)
    parser.add_argument("--debug",
                        help="Debug mode",
                        action="store_true")
    args = parser.parse_args()

    # Use the root of the tree as the working directory. Ideally a temporary
    # directory would be used, but unfortunately this would break the flow as
    # the configs used to seed the vendor directory must be under a common
    # parent directory.
    base_dir = paths.FUCHSIA_ROOT

    toml_path = os.path.join(base_dir, "Cargo.toml")
    lock_path = os.path.join(base_dir, "Cargo.lock")

    try:
        print("Downloading dependencies for:")
        for config in CONFIGS:
            print(" - %s" % config)

        # Create Cargo.toml.
        def mapper(p): return os.path.join(paths.FUCHSIA_ROOT, p)
        config = {
            "workspace": {
                "members": list(map(mapper, CONFIGS))
            }
        }
        with open(toml_path, "w") as config_file:
            toml.dump(config, config_file)

        cargo_bin = get_cargo_bin()

        # Generate Cargo.lock.
        lockfile_args = [
            cargo_bin,
            "generate-lockfile",
        ]
        call_or_exit(lockfile_args, base_dir)

        crates = parse_dependencies(lock_path)

        # Populate the vendor directory.
        vendor_args = [
            args.cargo_vendor,
            "-x",
            "--sync",
            lock_path,
            "vendor",
        ]
        call_or_exit(vendor_args, base_dir)
    finally:
        if not args.debug:
            os.remove(toml_path)
            os.remove(lock_path)

    crates_dir = os.path.join(paths.FUCHSIA_ROOT, "third_party", "rust-crates")
    vendor_dir = os.path.join(crates_dir, "vendor")
    shutil.rmtree(vendor_dir)
    shutil.move(os.path.join(paths.FUCHSIA_ROOT, "vendor"), vendor_dir)

    # Remove members of the workspace from the list of crates.
    crates = filter_non_vendor_crates(crates, vendor_dir)

    if not add_native_libraries(crates, vendor_dir):
        print("Unable to identify all required native libraries.")
        return 1

    build_path = os.path.join(crates_dir, "BUILD.gn")
    generate_build_file(build_path, crates)

    print("Verifying licenses...")
    if not check_licenses(vendor_dir):
        print("Some licenses are missing!")
        return 1

    update_file = os.path.join(crates_dir, ".vendor-update.stamp")
    # Write the timestamp file.
    # This file is necessary in order to trigger rebuilds of Rust artifacts
    # whenever third-party dependencies are updated.
    with open(update_file, "a"):
        os.utime(update_file, None)

    print("Vendor directory updated at %s" % vendor_dir)


if __name__ == '__main__':
    sys.exit(main())
