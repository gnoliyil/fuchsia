// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::{BTreeMap, BTreeSet};

/// A struct for collecting multiple kinds of platform configuration
#[derive(Default)]
pub struct ConfigurationBuilder {
    // The Assembly Input Bundles to add
    bundles: BTreeSet<String>,

    // Per-package configuration
    package_configs: PackageConfigs,
}

impl ConfigurationBuilder {
    /// Add configuration to the builder for a package.
    pub fn package(&mut self, name: &str) -> &mut PackageConfig {
        self.package_configs.entry(name.to_string()).or_default()
    }

    pub fn completed(self, bootfs: ComponentConfigs) -> CompletedConfiguration {
        let Self { bundles, package_configs } = self;
        CompletedConfiguration { bundles, bootfs, package_configs }
    }
}

/// The struct containing the resultant configuration to apply.
pub struct CompletedConfiguration {
    /// The Assembly Input Bundles to add.
    pub bundles: BTreeSet<String>,

    /// Configuration for bootfs components.
    pub bootfs: ComponentConfigs,

    /// Per-package configuration.
    pub package_configs: PackageConfigs,
}

/// A map from package names to the configuration to apply to them.
pub type PackageConfigs = BTreeMap<String, PackageConfig>;

/// A map from component manifest path with a namespace to the values for for the component.
pub type ComponentConfigs = BTreeMap<String, ComponentConfig>;

#[derive(Clone, Debug, Default)]
pub struct PackageConfig {
    /// A map from manifest paths within the package namespace to the values for the component.
    pub components: ComponentConfigs,
}

impl PackageConfig {
    /// Add configuration to the builder for a component within a package.
    pub fn component(&mut self, pkg_path: &str) -> &mut ComponentConfig {
        assert!(
            self.components.insert(pkg_path.to_owned(), Default::default()).is_none(),
            "each component's config can only be defined once"
        );
        self.components.get_mut(pkg_path).expect("just inserted this value")
    }
}

#[derive(Clone, Debug, Default)]
pub struct ComponentConfig {
    pub fields: BTreeMap<String, serde_json::Value>,
}

impl ComponentConfig {
    /// Add a value for a Structured Configuration field for a given component.
    pub fn field(&mut self, key: &str, value: impl Into<serde_json::Value>) -> &mut Self {
        assert!(
            self.fields.insert(key.to_owned(), value.into()).is_none(),
            "each configuration key can only be defined once"
        );
        self
    }
}
