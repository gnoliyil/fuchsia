// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        target::{CustomBuildTarget, GnTarget},
        types::*,
    },
    anyhow::{anyhow, Context as _, Result},
    cargo_metadata::{DependencyKind, Metadata, Package, PackageId},
    std::collections::{HashMap, HashSet},
    std::convert::TryFrom,
};

pub struct GnBuildGraph<'a> {
    metadata: &'a Metadata,
    // hashset because the same target can get added multiple times
    targets: HashSet<GnTarget<'a>>,
}

impl<'a> GnBuildGraph<'a> {
    pub fn new(metadata: &'a Metadata) -> Self {
        GnBuildGraph { metadata, targets: HashSet::new() }
    }

    pub fn targets(&'a self) -> impl Iterator<Item = &'a GnTarget<'_>> {
        self.targets.iter()
    }

    pub fn find_library_target(&self, package: &str, version: &str) -> Option<&GnTarget<'_>> {
        self.targets().find(|t| match t.target_type {
            GnRustType::Library
            | GnRustType::Rlib
            | GnRustType::Staticlib
            | GnRustType::Dylib
            | GnRustType::Cdylib
            | GnRustType::ProcMacro => t.pkg_name == package && t.version() == version,
            _ => false,
        })
    }

    pub fn find_binary_target(
        &self,
        package: &str,
        version: &str,
        target: &str,
    ) -> Option<&GnTarget<'_>> {
        self.targets().find(|t| match t.target_type {
            GnRustType::Binary => {
                t.pkg_name == package && t.version() == version && t.target_name == target
            }
            _ => false,
        })
    }

    /// Add a cargo package to the target list. If the dependencies
    /// are not already in the target graph, add them as well
    pub fn add_cargo_package(&mut self, cargo_pkg_id: PackageId) -> Result<()> {
        let package = &self.metadata[&cargo_pkg_id];
        for node in self
            .metadata
            .resolve
            .as_ref()
            .unwrap()
            .nodes
            .iter()
            .filter(|node| node.id == cargo_pkg_id)
        {
            // check if this crate has a build script in it
            let mut build_script = package.targets.iter().find_map(|target| {
                for kind in &target.kind {
                    if GnRustType::try_from(kind.as_str())
                        .with_context(|| {
                            format!("Failed to resolve GN target type for: {:?}", &target)
                        })
                        .unwrap()
                        == GnRustType::BuildScript
                    {
                        return Some(CustomBuildTarget {
                            dependencies: vec![],
                            path: &target.src_path,
                        });
                    }
                }
                None
            });

            let mut dependencies = HashMap::<Option<Platform>, Vec<(&'a Package, String)>>::new();

            // collect the dependency edges for this node
            for node_dep in node.deps.iter() {
                let id = &node_dep.pkg;
                if node_dep.dep_kinds.is_empty() {
                    return Err(anyhow!("Must use rustc 1.41+ to use cargo-gnaw. Needs dependency kind information."));
                }
                for kinds in &node_dep.dep_kinds {
                    let kind = &kinds.kind;
                    let target = &kinds.target;
                    match kind {
                        DependencyKind::Normal => {
                            self.add_cargo_package(id.clone())?;
                            let platform = target.as_ref().map(|x| format!("{}", x));
                            let platform_deps = dependencies.entry(platform).or_default();

                            // Somehow we get duplicates of the same dependency
                            // from cargo on some targets. Filter those out
                            // here. This is technically quadratic, but that's
                            // unlikely to matter for the number of deps on a
                            // single crate.
                            if !platform_deps.iter().any(|dep| dep.1 == node_dep.name.clone()) {
                                platform_deps.push((&self.metadata[id], node_dep.name.clone()));
                            }
                        }
                        DependencyKind::Build => {
                            if let Some(ref mut build_script) = build_script {
                                build_script.dependencies.push(&self.metadata[id]);
                            }
                        }
                        DependencyKind::Development => {}
                        err => {
                            return Err(anyhow!(
                                "Don't know how to handle this kind of dependency edge: {:?}",
                                err
                            ))
                        }
                    }
                }
            }

            for rust_target in package.targets.iter() {
                for kind in &rust_target.kind {
                    let target_type = GnRustType::try_from(kind.as_str())?;
                    match target_type {
                        GnRustType::Library | GnRustType::Rlib | GnRustType::ProcMacro => {
                            let gn_target = GnTarget::new(
                                &node.id,
                                &rust_target.name,
                                &package.name,
                                &package.edition,
                                &rust_target.src_path,
                                &package.version,
                                target_type,
                                node.features.as_slice(),
                                build_script.clone(),
                                dependencies.clone(),
                            );
                            self.targets.insert(gn_target);
                        }
                        GnRustType::Binary => {
                            // If a crate contains both a library and binary targets,
                            // cargo implicitly adds a dependency from each binary to the library.
                            //
                            // This logic imitates that behaviour.
                            let mut deps = dependencies.clone();
                            let maybe_library = package.targets.iter().find(|v| {
                                v.kind.iter().any(|kind| {
                                    matches!(
                                        GnRustType::try_from(kind.as_str()),
                                        Ok(GnRustType::Library)
                                    )
                                })
                            });
                            if let Some(lib) = maybe_library {
                                deps.entry(None).or_default().push((
                                    &self.metadata[&node.id],
                                    lib.name.clone().replace('-', "_"),
                                ));
                            }
                            let gn_target = GnTarget::new(
                                &node.id,
                                &rust_target.name,
                                &package.name,
                                &package.edition,
                                &rust_target.src_path,
                                &package.version,
                                target_type,
                                node.features.as_slice(),
                                build_script.clone(),
                                deps,
                            );
                            self.targets.insert(gn_target);
                        }

                        // FIXME(http://fxbug.dev/91791): support staticlib, dylib, and
                        // cdylib crate types.
                        GnRustType::Staticlib | GnRustType::Dylib | GnRustType::Cdylib => (),

                        // BuildScripts are handled as part of the targets
                        GnRustType::BuildScript => (),

                        // TODO support building tests. Should integrate with whatever GN
                        // metadata collection system used by the main build.
                        GnRustType::Example | GnRustType::Bench | GnRustType::Test => (),
                    }
                }
            }
        }
        Ok(())
    }
}
