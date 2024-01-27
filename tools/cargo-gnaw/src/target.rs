// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::gn::add_version_suffix,
    crate::types::*,
    camino::{Utf8Path, Utf8PathBuf},
    cargo_metadata::{Package, PackageId, Version},
    std::borrow::Cow,
    std::cmp::Ordering,
    std::collections::{hash_map::DefaultHasher, HashMap},
    std::hash::{Hash, Hasher},
};

#[derive(Clone)]
pub struct CustomBuildTarget<'a> {
    pub path: &'a Utf8Path,
    pub dependencies: Vec<&'a Package>,
}

pub struct GnTarget<'a> {
    /// Package ID from the Cargo metadata
    cargo_pkg_id: &'a PackageId,
    /// Version of the Package from Cargo
    version: &'a Version,
    /// Name of the target given in Cargo.toml
    pub target_name: &'a str,
    /// Name of the package given in Cargo.toml
    pub pkg_name: &'a str,
    /// Path to the root of the crate
    pub crate_root: &'a Utf8Path,
    /// Rust Edition of the target
    /// rustc: --edition
    pub edition: &'a str,
    /// Type of crate
    /// rustc: --crate-type
    pub target_type: GnRustType,
    /// Rust features enabled on this target
    /// rustc: --cfg=feature=<string>
    pub features: &'a [String],
    /// Target depends on Cargo running a custom build-script
    pub build_script: Option<CustomBuildTarget<'a>>,
    /// Target depends on Cargo running a custom build-script
    pub dependencies: HashMap<Option<Platform>, Vec<(&'a Package, String)>>,
}

impl std::fmt::Debug for GnTarget<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let display_name = if self.target_name != self.pkg_name {
            Cow::Owned(format!("{}.{}", self.pkg_name, self.target_name))
        } else {
            Cow::Borrowed(self.pkg_name)
        };
        if self.build_script.is_some() {
            write!(f, "{:?} with custom-build: {}", self.target_type, display_name)
        } else {
            write!(f, "{:?}: {}", self.target_type, display_name)
        }
    }
}

impl PartialEq for GnTarget<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.cargo_pkg_id == other.cargo_pkg_id
            && self.target_name == other.target_name
            && self.target_type == other.target_type
    }
}
impl Eq for GnTarget<'_> {}

impl PartialOrd for GnTarget<'_> {
    fn partial_cmp(&self, other: &GnTarget<'_>) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for GnTarget<'_> {
    fn cmp(&self, other: &GnTarget<'_>) -> Ordering {
        self.cargo_pkg_id
            .cmp(other.cargo_pkg_id)
            .then(self.target_name.cmp(other.target_name))
            .then(self.target_type.cmp(&other.target_type))
    }
}

impl Hash for GnTarget<'_> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.cargo_pkg_id.hash(state);
        self.target_name.hash(state);
        self.target_type.hash(state);
    }
}

impl<'a> GnTarget<'a> {
    pub fn new(
        cargo_pkg_id: &'a PackageId,
        target_name: &'a str,
        pkg_name: &'a str,
        edition: &'a str,
        crate_root: &'a Utf8Path,
        version: &'a Version,
        target_type: GnRustType,
        features: &'a [Feature],
        build_script: Option<CustomBuildTarget<'a>>,
        dependencies: HashMap<Option<Platform>, Vec<(&'a Package, String)>>,
    ) -> Self {
        GnTarget {
            cargo_pkg_id,
            target_name,
            pkg_name,
            edition,
            crate_root,
            version,
            target_type,
            features,
            build_script,
            dependencies,
        }
    }

    pub fn uses_build_script(&self) -> bool {
        self.build_script.is_some()
    }

    /// Name of the target given in Cargo.toml
    pub fn name(&self) -> String {
        self.target_name.to_owned()
    }

    /// Version of the Package from Cargo
    pub fn version(&self) -> String {
        self.version.to_string()
    }

    pub fn metadata_hash(&self) -> String {
        let mut hasher = DefaultHasher::new();
        self.gn_target_name().hash(&mut hasher);
        format!("{:x}", hasher.finish())
    }

    /// with version
    pub fn gn_target_name(&self) -> String {
        let prefix = match self.target_type {
            GnRustType::Library | GnRustType::Rlib | GnRustType::ProcMacro => {
                Cow::Borrowed(self.pkg_name)
            }
            GnRustType::Binary => Cow::Owned(format!("{}-{}", self.pkg_name, self.target_name)),
            ty => panic!("Don't know how to represent this type \"{:?}\" in GN", ty),
        };
        add_version_suffix(&prefix, &self.version)
    }

    pub fn package_root(&self) -> Utf8PathBuf {
        let mut package_root = self.crate_root.canonicalize_utf8().unwrap();

        while !package_root.join("Cargo.toml").exists() {
            package_root = package_root
                .parent()
                .expect("searching up from the crate root we must find a cargo.toml")
                .to_path_buf();
        }

        assert!(
            !package_root.ends_with("third_party/rust_crates"),
            "must find a cargo.toml before the root one"
        );

        package_root.to_owned()
    }

    pub fn gn_target_type(&self) -> String {
        match self.target_type {
            GnRustType::Library | GnRustType::Rlib => String::from("rust_library"),
            GnRustType::Binary => String::from("executable"),
            GnRustType::ProcMacro => String::from("rust_proc_macro"),
            ty => panic!("Don't know how to represent this type \"{:?}\" in GN", ty),
        }
    }
}
