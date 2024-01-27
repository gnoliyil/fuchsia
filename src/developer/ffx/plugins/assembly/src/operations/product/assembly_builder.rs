// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::compiled_package::CompiledPackageBuilder;
use anyhow::{anyhow, ensure, Context, Result};
use assembly_config_data::ConfigDataBuilder;
use assembly_config_schema::{
    assembly_config::{AssemblyInputBundle, CompiledPackageDefinition, ShellCommands},
    image_assembly_config::{PartialImageAssemblyConfig, PartialKernelConfig},
    product_config::{ProductConfigData, ProductPackageDetails, ProductPackagesConfig},
    DriverDetails, FileEntry,
};
use assembly_driver_manifest::DriverManifestBuilder;
use assembly_package_utils::{PackageInternalPathBuf, PackageManifestPathBuf};
use assembly_platform_configuration::{ComponentConfigs, PackageConfigs, PackageConfiguration};
use assembly_shell_commands::ShellCommandsBuilder;
use assembly_structured_config::Repackager;
use assembly_tool::ToolProvider;
use assembly_util as util;
use assembly_util::{DuplicateKeyError, InsertAllUniqueExt, InsertUniqueExt, MapEntry};
use camino::{Utf8Path, Utf8PathBuf};
use fuchsia_pkg::PackageManifest;
use fuchsia_url::UnpinnedAbsolutePackageUrl;
use std::collections::{BTreeMap, BTreeSet};

type ConfigDataMap = BTreeMap<String, FileEntryMap>;

pub struct ImageAssemblyConfigBuilder {
    /// The base packages from the AssemblyInputBundles
    base: PackageSet,

    /// The cache packages from the AssemblyInputBundles
    cache: PackageSet,

    /// The base driver packages from the AssemblyInputBundles
    base_drivers: NamedMap<DriverDetails>,

    /// The system packages from the AssemblyInputBundles
    system: PackageSet,

    /// The bootfs packages from the AssemblyInputBundles
    bootfs_packages: PackageSet,

    /// The boot_args from the AssemblyInputBundles
    boot_args: BTreeSet<String>,

    /// The bootfs_files from the AssemblyInputBundles
    bootfs_files: FileEntryMap,

    /// The config_data entries, by package and by destination path.
    config_data: ConfigDataMap,

    /// Modifications that must be made to structured config within bootfs.
    bootfs_structured_config: ComponentConfigs,

    /// Modifications that must be made to configuration for packages.
    package_configs: PackageConfigs,

    kernel_path: Option<Utf8PathBuf>,
    kernel_args: BTreeSet<String>,
    kernel_clock_backstop: Option<u64>,

    qemu_kernel: Option<Utf8PathBuf>,
    shell_commands: ShellCommands,

    /// A set of all unique packageUrls across all AIBs passed to the builder
    package_urls: BTreeSet<UnpinnedAbsolutePackageUrl>,

    /// The packages for assembly to create specified by AIBs
    packages_to_compile: BTreeMap<String, CompiledPackageBuilder>,
}

/// An enum representing unique identifiers for the 4 types of supported package sets in the
/// ImageAssemblyConfigBuilder. Used to dereference PackageSet fields on the builder by name
#[derive(Debug)]
#[allow(clippy::upper_case_acronyms)]
enum PackageSets {
    BASE,
    CACHE,
    SYSTEM,
    BOOTFS,
}

impl PackageSets {
    fn list_all() -> Vec<PackageSets> {
        vec![Self::BASE, Self::CACHE, Self::BOOTFS, Self::SYSTEM]
    }
}

impl Default for ImageAssemblyConfigBuilder {
    fn default() -> Self {
        Self::new()
    }
}

impl ImageAssemblyConfigBuilder {
    pub fn new() -> Self {
        Self {
            base: PackageSet::new("base packages"),
            cache: PackageSet::new("cache packages"),
            base_drivers: NamedMap::new("base_drivers"),
            system: PackageSet::new("system packages"),
            bootfs_packages: PackageSet::new("bootfs packages"),
            boot_args: BTreeSet::default(),
            shell_commands: ShellCommands::default(),
            bootfs_files: FileEntryMap::new("bootfs files"),
            config_data: ConfigDataMap::default(),
            bootfs_structured_config: ComponentConfigs::default(),
            package_configs: PackageConfigs::default(),
            kernel_path: None,
            kernel_args: BTreeSet::default(),
            kernel_clock_backstop: None,
            qemu_kernel: None,
            package_urls: BTreeSet::default(),
            packages_to_compile: BTreeMap::default(),
        }
    }

    /// Add an Assembly Input Bundle to the builder, via the path to its
    /// manifest.
    ///
    /// If any of the items it's trying to add are duplicates (either of itself
    /// or others, this will return an error.)
    pub fn add_bundle(&mut self, bundle_path: impl AsRef<Utf8Path>) -> Result<()> {
        let bundle = util::read_config(bundle_path.as_ref())?;

        // Strip filename from bundle path.
        let bundle_path =
            bundle_path.as_ref().parent().map(Utf8PathBuf::from).unwrap_or_else(|| "".into());

        // Now add the parsed bundle
        self.add_parsed_bundle(bundle_path, bundle)
    }

    /// Add an Assembly Input Bundle to the builder, using a parsed
    /// AssemblyInputBundle, and the path to the folder that contains it.
    ///
    /// If any of the items it's trying to add are duplicates (either of itself
    /// or others, this will return an error.)
    pub fn add_parsed_bundle(
        &mut self,
        bundle_path: impl AsRef<Utf8Path>,
        bundle: AssemblyInputBundle,
    ) -> Result<()> {
        let bundle_path = bundle_path.as_ref();
        let AssemblyInputBundle {
            image_assembly: bundle,
            config_data,
            blobs: _,
            base_drivers,
            shell_commands,
            packages_to_compile,
        } = bundle;

        self.add_bundle_packages(bundle_path, &bundle)?;

        // Base drivers are added to the base packages
        for driver_details in base_drivers {
            let driver_package_path = &bundle_path.join(&driver_details.package);
            self.add_unique_package_from_path(driver_package_path, &PackageSets::BASE)?;

            let package_url = DriverManifestBuilder::get_base_package_url(driver_package_path)?;
            self.base_drivers.try_insert_unique(package_url, driver_details)?;
        }

        self.boot_args
            .try_insert_all_unique(bundle.boot_args)
            .map_err(|arg| anyhow!("duplicate boot_arg found: {}", arg))?;

        for entry in Self::file_entry_paths_from_bundle(bundle_path, bundle.bootfs_files) {
            self.bootfs_files.add_entry(entry)?;
        }

        if let Some(kernel) = bundle.kernel {
            assembly_util::set_option_once_or(
                &mut self.kernel_path,
                kernel.path.map(|p| bundle_path.join(p)),
                anyhow!("Only one input bundle can specify a kernel path"),
            )?;

            self.kernel_args
                .try_insert_all_unique(kernel.args)
                .map_err(|arg| anyhow!("duplicate kernel arg found: {}", arg))?;

            assembly_util::set_option_once_or(
                &mut self.kernel_clock_backstop,
                kernel.clock_backstop,
                anyhow!("Only one input bundle can specify a kernel clock backstop"),
            )?;
        }

        for (package, entries) in config_data {
            for entry in Self::file_entry_paths_from_bundle(bundle_path, entries) {
                self.add_config_data_entry(&package, entry)?;
            }
        }

        for (package, binaries) in shell_commands {
            for binary in binaries {
                self.add_shell_command_entry(&package, binary)?;
            }
        }

        for compiled_package in packages_to_compile {
            self.add_compiled_package(&compiled_package, bundle_path)?;
        }

        assembly_util::set_option_once_or(
            &mut self.qemu_kernel,
            bundle.qemu_kernel.map(|p| bundle_path.join(p)),
            anyhow!("Only one input bundle can specify a qemu kernel path"),
        )?;

        Ok(())
    }

    fn add_unique_package_from_path(
        &mut self,
        path: impl AsRef<Utf8Path>,
        to_package_set: &PackageSets,
    ) -> Result<()> {
        // Create PackageEntry
        let package_entry = PackageEntry::parse_from(path.as_ref().to_owned())?;

        self.add_unique_package_entry(package_entry, to_package_set)
    }

    /// Adds a package via PackageEntry to the package set, only if it is unique across all other
    /// packages in the builder
    fn add_unique_package_entry(
        &mut self,
        package_entry: PackageEntry,
        to_package_set: &PackageSets,
    ) -> Result<()> {
        let package_set = match to_package_set.to_owned() {
            PackageSets::BASE => &mut self.base,
            PackageSets::CACHE => &mut self.cache,
            PackageSets::SYSTEM => &mut self.system,
            PackageSets::BOOTFS => &mut self.bootfs_packages,
        };

        let package_url = package_entry.manifest.package_url()?.ok_or_else(|| {
            anyhow::anyhow!("Something happened when trying to get the package_url")
        })?;
        self.package_urls.try_insert_unique(package_url).map_err(|e| {
            anyhow::anyhow!("duplicate package {} found in {}", e, package_set.name)
        })?;
        package_set.add_package(package_entry)?;

        Ok(())
    }

    /// Add a set of packages from a bundle, resolving each path to a package
    /// manifest from the bundle's path to locate it.
    fn add_bundle_packages(
        &mut self,
        bundle_path: impl AsRef<Utf8Path>,
        bundle: &PartialImageAssemblyConfig,
    ) -> Result<()> {
        for package_set in PackageSets::list_all() {
            let bundle_package_paths = match package_set {
                PackageSets::BASE => &bundle.base,
                PackageSets::CACHE => &bundle.cache,
                PackageSets::SYSTEM => &bundle.system,
                PackageSets::BOOTFS => &bundle.bootfs_packages,
            };

            for path in bundle_package_paths {
                let manifest_path = bundle_path.as_ref().join(path);
                self.add_unique_package_from_path(manifest_path, &package_set)?;
            }
        }

        Ok(())
    }

    fn file_entry_paths_from_bundle(
        base: &Utf8Path,
        entries: impl IntoIterator<Item = FileEntry>,
    ) -> Vec<FileEntry> {
        entries
            .into_iter()
            .map(|entry| FileEntry {
                destination: entry.destination,
                source: base.join(entry.source),
            })
            .collect()
    }

    /// Add all the product-provided packages to the assembly configuration.
    ///
    /// This should be performed after the platform's bundles have been added,
    /// so that any packages that are in conflict with the platform bundles are
    /// flagged as being the issue (and not the platform being the issue).
    pub fn add_product_packages(&mut self, packages: ProductPackagesConfig) -> Result<()> {
        let mut add_package =
            |entries: Vec<ProductPackageDetails>, to_package_set: PackageSets| -> Result<()> {
                for entry in entries {
                    // Parse the package_manifest.json into a PackageManifest, returning
                    // both along with any config_data entries defined for the package.
                    let (manifest_path, pkg_manifest, config_data) =
                        Self::parse_product_package_entry(entry)?;
                    let package_manifest_name = pkg_manifest.name().to_string();
                    let package_entry =
                        PackageEntry { path: manifest_path.into(), manifest: pkg_manifest };
                    self.add_unique_package_entry(package_entry, &to_package_set)?;
                    // Add the config data entries to the map
                    if !config_data.is_empty() {
                        self.config_data
                            .entry(package_manifest_name)
                            .or_default()
                            .add_all(config_data)?;
                    };
                }
                Ok(())
            };

        // Add the config data entries to the map
        add_package(packages.base, PackageSets::BASE)?;
        add_package(packages.cache, PackageSets::CACHE)?;

        Ok(())
    }

    /// Add the product-provided drivers to the assembly configuration.
    ///
    /// This should be performed after all the platform bundles have
    /// been added as it is for packages. Packages specified as
    /// base driver packages should not be in the base package set and
    /// are added automatically.
    pub fn add_product_drivers(&mut self, drivers: Vec<DriverDetails>) -> Result<()> {
        // Base drivers are added to the base packages
        // Config data is not supported for driver packages since it is deprecated.
        for driver_details in drivers {
            let manifest =
                PackageManifest::try_load_from(&driver_details.package).with_context(|| {
                    format!("parsing {} as a package manifest", driver_details.package)
                })?;
            self.base
                .add_package(PackageEntry { path: driver_details.package.clone(), manifest })
                .context(format!("Adding driver {}", &driver_details.package))?;
            let package_url = DriverManifestBuilder::get_base_package_url(&driver_details.package)?;
            self.base_drivers.try_insert_unique(package_url, driver_details)?;
        }
        Ok(())
    }

    /// Given the parsed json of the product package set entry, parse out the
    /// package manifest, and any configuration associated with the package.
    fn parse_product_package_entry(
        entry: ProductPackageDetails,
    ) -> Result<(PackageManifestPathBuf, PackageManifest, Vec<FileEntry>)> {
        // Load the PackageManifest from the given path
        let manifest = PackageManifest::try_load_from(&entry.manifest)
            .with_context(|| format!("parsing {} as a package manifest", &entry.manifest))?;

        // If there are config_data entries, convert the TypedPathBuf pairs into
        // FileEntry objects.  From this point on, they are handled as FileEntry
        // TODO(tbd): Switch FileEntry to use TypedPathBuf instead of String and
        // PathBuf.
        let config_data_entries = entry
            .config_data
            .into_iter()
            // Explicitly call out the path types to make sure that they
            // are ordered as expected in the tuple.
            .map(|ProductConfigData { destination, source }| FileEntry {
                destination: destination.to_string(),
                source: source.into(),
            })
            .collect();
        Ok((entry.manifest, manifest, config_data_entries))
    }

    /// Add an entry to `config_data` for the given package.  If the entry
    /// duplicates an existing entry, return an error.
    fn add_config_data_entry(&mut self, package: impl AsRef<str>, entry: FileEntry) -> Result<()> {
        self.config_data.entry(package.as_ref().into()).or_default().add_entry(entry)
    }

    fn add_shell_command_entry(
        &mut self,
        package_name: impl AsRef<str>,
        binary: PackageInternalPathBuf,
    ) -> Result<()> {
        self.shell_commands
            .entry(package_name.as_ref().into())
            .or_default()
            .try_insert_unique(binary)
            .map_err(|dup| {
                anyhow!(
                    "duplicate shell command found in package: {} = {}",
                    package_name.as_ref(),
                    dup
                )
            })
    }

    pub fn set_bootfs_structured_config(&mut self, config: ComponentConfigs) {
        self.bootfs_structured_config = config;
    }

    /// Set the configuration updates for a package. Can only be called once per
    /// package.
    pub fn set_package_config(
        &mut self,
        package: impl AsRef<str>,
        config: PackageConfiguration,
    ) -> Result<()> {
        if self.package_configs.insert(package.as_ref().to_owned(), config).is_none() {
            Ok(())
        } else {
            Err(anyhow::format_err!("duplicate config patch"))
        }
    }

    pub fn add_compiled_package(
        &mut self,
        compiled_package_def: &CompiledPackageDefinition,
        bundle_path: &Utf8Path,
    ) -> Result<()> {
        let name = compiled_package_def.name();
        self.packages_to_compile
            .entry(name.to_string())
            .or_insert_with(|| CompiledPackageBuilder::new(name))
            .add_package_def(compiled_package_def, bundle_path)
            .context("adding package def")?;
        Ok(())
    }

    /// Construct an ImageAssembly ImageAssemblyConfig from the collected items in the
    /// builder.
    ///
    /// If there are config_data entries, the config_data package will be
    /// created in the outdir, and it will be added to the returned
    /// ImageAssemblyConfig.
    ///
    /// If there are compiled packages specified, the compiled packages will
    /// also be created in the outdir and added to the ImageAssemblyConfig.
    ///
    /// If this cannot create a completed ImageAssemblyConfig, it will return an error
    /// instead.
    pub fn build(
        self,
        outdir: impl AsRef<Utf8Path>,
        tools: &impl ToolProvider,
    ) -> Result<assembly_config_schema::ImageAssemblyConfig> {
        let outdir = outdir.as_ref();
        // Decompose the fields in self, so that they can be recomposed into the generated
        // image assembly configuration.
        let Self {
            package_configs,
            mut base,
            mut cache,
            base_drivers,
            mut system,
            boot_args,
            mut bootfs_files,
            mut bootfs_packages,
            bootfs_structured_config,
            config_data,
            kernel_path,
            kernel_args,
            kernel_clock_backstop,
            qemu_kernel,
            shell_commands,
            package_urls: _,
            packages_to_compile,
        } = self;

        let cmc_tool = tools.get_tool("cmc")?;

        // add structured config value files to bootfs
        let mut bootfs_repackager = Repackager::for_bootfs(&mut bootfs_files.entries, outdir);
        for (component, values) in bootfs_structured_config {
            // check if we should try to configure the component before attempting so we can still
            // return errors for other conditions like a missing config field or a wrong type
            if bootfs_repackager.has_component(&component) {
                bootfs_repackager.set_component_config(&component, values.fields)?;
            } else {
                // TODO(https://fxbug.dev/101556) return an error here
            }
        }

        // repackage any matching packages
        for (package, config) in package_configs {
            // Only process configs that have component entries for structured config.
            if !config.components.is_empty() {
                // get the manifest for this package name, returning the set from which it was removed
                if let Some((manifest, source_package_set)) = remove_package_from_sets(
                    &package,
                    [&mut base, &mut cache, &mut system, &mut bootfs_packages],
                )
                .with_context(|| format!("removing {package} for repackaging"))?
                {
                    let outdir = outdir.join("repackaged").join(&package);
                    let mut repackager = Repackager::new(manifest, outdir)
                        .with_context(|| format!("reading existing manifest for {package}"))?;

                    // Iterate over the components to get their structured config values
                    for (component, values) in &config.components {
                        repackager
                            .set_component_config(component, values.fields.clone())
                            .with_context(|| format!("setting new config for {component}"))?;
                    }
                    let new_path = repackager
                        .build()
                        .with_context(|| format!("building repackaged {package}"))?;
                    let new_entry = PackageEntry::parse_from(new_path)
                        .with_context(|| format!("parsing repackaged {package}"))?;
                    source_package_set.insert(new_entry.name().to_owned(), new_entry);
                } else {
                    // TODO(https://fxbug.dev/101556) return an error here
                }
            }
        }

        // TODO(https://fxbug.dev/98103) Make the presence of the base package an explicit parameter
        // Add the base drivers package to the base package if we're generating a base package
        if !base.is_empty() || !cache.is_empty() || !system.is_empty() {
            // Build the driver-manager-base-config package and add it to the base packages
            let mut driver_manifest_builder = DriverManifestBuilder::default();
            for (package_url, driver_details) in base_drivers.entries {
                driver_manifest_builder
                    .add_driver(driver_details, &package_url)
                    .with_context(|| format!("adding driver {}", &package_url))?;
            }
            let driver_manifest_package_manifest_path = driver_manifest_builder
                .build_driver_manifest_package(outdir)
                .context("building driver manifest package")?;

            base.add_package(PackageEntry::parse_from(driver_manifest_package_manifest_path)?)?;
        }

        if !config_data.is_empty() {
            // Build the config_data package
            let mut config_data_builder = ConfigDataBuilder::default();
            for (package_name, entries) in config_data {
                for entry in entries.into_file_entries() {
                    config_data_builder.add_entry(
                        &package_name,
                        entry.destination.into(),
                        entry.source,
                    )?;
                }
            }
            let manifest_path = config_data_builder
                .build(outdir)
                .context("writing the 'config_data' package metafar.")?;
            base.add_package_from_path(manifest_path)
                .context("adding generated config-data package")?;
        }

        if !shell_commands.is_empty() {
            let mut shell_commands_builder = ShellCommandsBuilder::new();
            shell_commands_builder.add_shell_commands(shell_commands, "fuchsia.com".to_string());
            let manifest =
                shell_commands_builder.build(outdir).context("building shell commands package")?;
            base.add_package_from_path(manifest)
                .context("adding shell commands package to base")?;
        }

        for (_, package_builder) in packages_to_compile {
            let package_manifest_path = package_builder
                .build(cmc_tool.as_ref(), outdir)
                .context("building compiled package")?;
            base.add_package_from_path(package_manifest_path).context("adding compiled package")?;
        }

        // Construct a single "partial" config from the combined fields, and
        // then pass this to the ImageAssemblyConfig::try_from_partials() to get the
        // final validation that it's complete.
        let partial = assembly_config_schema::PartialImageAssemblyConfig {
            system: system.into_paths().collect(),
            base: base.into_paths().collect(),
            cache: cache.into_paths().collect(),
            kernel: Some(PartialKernelConfig {
                path: kernel_path,
                args: kernel_args.into_iter().collect(),
                clock_backstop: kernel_clock_backstop,
            }),
            qemu_kernel,
            boot_args: boot_args.into_iter().collect(),
            bootfs_files: bootfs_files.into_file_entries(),
            bootfs_packages: bootfs_packages.into_paths().collect(),
        };

        let image_assembly_config = assembly_config_schema::ImageAssemblyConfig::try_from_partials(
            std::iter::once(partial),
        )?;

        Ok(image_assembly_config)
    }
}

/// Remove a package with a matching name from the provided package sets, returning its parsed
/// manifest and a mutable reference to the set from which it was removed.
fn remove_package_from_sets<'a, 'b: 'a, const N: usize>(
    package_name: &str,
    package_sets: [&'a mut PackageSet; N],
) -> anyhow::Result<Option<(PackageManifest, &'a mut PackageSet)>> {
    let mut matches_name = None;

    for package_set in package_sets {
        if let Some(entry) = package_set.remove(package_name) {
            ensure!(
                matches_name.is_none(),
                "only one package with a given name is allowed per product"
            );
            matches_name = Some((entry.manifest, package_set));
        }
    }

    Ok(matches_name)
}

#[derive(Debug)]
struct PackageEntry {
    path: Utf8PathBuf,
    manifest: PackageManifest,
}

impl PackageEntry {
    fn parse_from(path: impl Into<Utf8PathBuf>) -> Result<Self> {
        let path = path.into();
        let manifest = PackageManifest::try_load_from(&path)
            .with_context(|| format!("parsing {path} as a package manifest"))?;
        Ok(Self { path, manifest })
    }

    fn name(&self) -> &str {
        self.manifest.name().as_ref()
    }
}

#[derive(Default, Debug)]
/// A named set of things, which are mapped by a String key.

struct NamedMap<T> {
    /// The name of the Map.
    name: String,

    /// The entries in the map.
    entries: BTreeMap<String, T>,
}

impl<T> NamedMap<T>
where
    T: std::fmt::Debug,
{
    /// Create a new, named, map.
    fn new(name: &str) -> Self {
        Self { name: name.to_owned(), entries: BTreeMap::new() }
    }

    fn try_insert_unique(&mut self, name: String, value: T) -> Result<()> {
        let result = self.entries.try_insert_unique(MapEntry(name, value)).map_err(|e| {
            format!(
                "key: '{}'\n  existing value: {:#?}\n  new value: {:#?}",
                e.key(),
                e.previous_value(),
                e.new_value()
            )
        });
        // The error is mapped a second time to separate the borrow of entries
        // from the borrow of name.
        result.map_err(|e| anyhow!("duplicate entry in {}:\n  {}", self.name, e))
    }
}

impl<T> std::ops::Deref for NamedMap<T> {
    type Target = BTreeMap<String, T>;

    fn deref(&self) -> &Self::Target {
        &self.entries
    }
}

impl<T> std::ops::DerefMut for NamedMap<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.entries
    }
}

impl<T> IntoIterator for NamedMap<T> {
    type Item = T;

    type IntoIter = std::collections::btree_map::IntoValues<String, Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        self.entries.into_values()
    }
}

/// A named set of packages with their manifests parsed into memory, keyed by package name.
type PackageSet = NamedMap<PackageEntry>;
impl PackageSet {
    /// Add the package described by the ProductPackageSetEntry to the
    /// PackageSet
    fn add_package(&mut self, entry: PackageEntry) -> Result<()> {
        self.try_insert_unique(entry.name().to_owned(), entry)
    }

    /// Parse the given path as a PackageManifest, and add it to the PackageSet.
    fn add_package_from_path<P: Into<Utf8PathBuf>>(&mut self, path: P) -> Result<()> {
        {
            let entry = PackageEntry::parse_from(path)?;
            self.add_package(entry)
        }
        .with_context(|| format!("Adding package to set: {}", self.name))
    }

    /// Convert the PackageSet into an iterable collection of Paths.
    fn into_paths(self) -> impl Iterator<Item = Utf8PathBuf> {
        self.entries.into_values().map(|e| e.path)
    }
}

type FileEntryMap = NamedMap<Utf8PathBuf>;
impl FileEntryMap {
    /// Add a single FileEntry to the map, if the 'destination' path is a
    /// duplicate, return an error, otherwise add the entry.
    fn add_entry(&mut self, entry: FileEntry) -> Result<()> {
        self.try_insert_unique(entry.destination, entry.source)
            .with_context(|| format!("Adding entry to set: {}", self.name))
    }

    /// Add FileEntries to the map, ensuring that the destination paths are all
    /// unique within the map.
    fn add_all(&mut self, entries: impl IntoIterator<Item = FileEntry>) -> Result<()> {
        for entry in entries.into_iter() {
            self.add_entry(entry)?;
        }
        Ok(())
    }

    /// Return the contents of the FileMap as a Vec<FileEntry>.
    fn into_file_entries(self) -> Vec<FileEntry> {
        self.entries
            .into_iter()
            .map(|(destination, source)| FileEntry { destination, source })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use assembly_config_schema::assembly_config::{
        AdditionalPackageContents, MainPackageDefinition, ShellCommands,
    };
    use assembly_driver_manifest::DriverManifest;
    use assembly_package_utils::PackageManifestPathBuf;
    use assembly_test_util::generate_test_manifest;
    use assembly_tool::testing::FakeToolProvider;
    use assembly_tool::ToolCommandLog;
    use camino::{Utf8Path, Utf8PathBuf};
    use fuchsia_archive;
    use fuchsia_pkg::{PackageBuilder, PackageManifest};
    use itertools::Itertools;
    use serde_json::json;
    use std::fs::File;
    use std::io::BufReader;
    use std::io::Write;
    use tempfile::TempDir;

    struct TempdirPathsForTest {
        _tmp: TempDir,
        pub outdir: Utf8PathBuf,
        pub bundle_path: Utf8PathBuf,
        pub config_data_target_package_name: String,
        pub config_data_target_package_dir: Utf8PathBuf,
        pub config_data_file_path: Utf8PathBuf,
    }

    impl TempdirPathsForTest {
        fn new() -> Self {
            let tmp = TempDir::new().unwrap();
            let outdir = Utf8Path::from_path(tmp.path()).unwrap().to_path_buf();
            let bundle_path = outdir.join("bundle");
            let config_data_target_package_name = "base_package0".to_owned();
            let config_data_target_package_dir =
                bundle_path.join("config_data").join(&config_data_target_package_name);
            let config_data_file_path =
                config_data_target_package_dir.join("config_data_source_file");
            Self {
                _tmp: tmp,
                outdir,
                bundle_path,
                config_data_target_package_name,
                config_data_target_package_dir,
                config_data_file_path,
            }
        }
    }

    fn write_empty_pkg(
        path: impl AsRef<Utf8Path>,
        name: &str,
        repo: Option<&str>,
    ) -> PackageManifestPathBuf {
        let path = path.as_ref();
        let mut builder = PackageBuilder::new(name);
        let manifest_path = path.join(name);
        builder.manifest_path(&manifest_path);
        if let Some(repo_name) = repo {
            builder.repository(repo_name);
        } else {
            builder.repository("fuchsia.com");
        }
        builder.build(path, path.join(format!("{name}_meta.far"))).unwrap();
        manifest_path.into()
    }

    fn make_test_assembly_bundle(bundle_path: &Utf8Path) -> AssemblyInputBundle {
        let write_empty_bundle_pkg = |name: &str| write_empty_pkg(bundle_path, name, None).clone();
        AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: vec![write_empty_bundle_pkg("base_package0")],
                system: vec![write_empty_bundle_pkg("sys_package0")],
                cache: vec![write_empty_bundle_pkg("cache_package0")],
                bootfs_packages: vec![write_empty_bundle_pkg("bootfs_package0")],
                kernel: Some(PartialKernelConfig {
                    path: Some("kernel/path".into()),
                    args: vec!["kernel_arg0".into()],
                    clock_backstop: Some(56244),
                }),
                qemu_kernel: Some("path/to/qemu/kernel".into()),
                boot_args: vec!["boot_arg0".into()],
                bootfs_files: vec![FileEntry {
                    source: "source/path/to/file".into(),
                    destination: "dest/file/path".into(),
                }],
            },
            base_drivers: Vec::default(),
            config_data: BTreeMap::default(),
            blobs: Vec::default(),
            shell_commands: ShellCommands::default(),
            packages_to_compile: Vec::default(),
        }
    }

    fn make_test_driver(package_name: &str, outdir: impl AsRef<Utf8Path>) -> Result<DriverDetails> {
        let driver_package_manifest_file_path = outdir.as_ref().join(package_name);
        let mut driver_package_manifest_file = File::create(&driver_package_manifest_file_path)?;
        let package_manifest = generate_test_manifest(package_name, None);
        serde_json::to_writer(&driver_package_manifest_file, &package_manifest)?;
        driver_package_manifest_file.flush()?;

        Ok(DriverDetails {
            package: driver_package_manifest_file_path,
            components: vec![Utf8PathBuf::from("meta/foobar.cm")],
        })
    }

    /// Create an ImageAssemblyConfigBuilder with a minimal AssemblyInputBundle
    /// for testing product configuration.
    ///
    /// # Arguments
    ///
    /// * `package_names` - names for empty stub packages to create and add to the
    ///    base set.
    fn get_minimum_config_builder(
        outdir: impl AsRef<Utf8Path>,
        package_names: Vec<String>,
    ) -> ImageAssemblyConfigBuilder {
        let minimum_bundle = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: package_names
                    .iter()
                    .map(|package_name| write_empty_pkg(&outdir, package_name, None).into())
                    .collect(),
                kernel: Some(PartialKernelConfig {
                    path: Some("kernel/path".into()),
                    args: Vec::default(),
                    clock_backstop: Some(0),
                }),
                qemu_kernel: Some("kernel/qemu/path".into()),
                ..assembly_config_schema::PartialImageAssemblyConfig::default()
            },
            base_drivers: Vec::default(),
            config_data: BTreeMap::default(),
            blobs: Vec::default(),
            shell_commands: ShellCommands::default(),
            packages_to_compile: Vec::default(),
        };
        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(outdir.as_ref().join("minimum_bundle"), minimum_bundle).unwrap();
        builder
    }

    #[test]
    fn test_builder() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tools = FakeToolProvider::default();

        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(outdir, make_test_assembly_bundle(outdir)).unwrap();
        let result: assembly_config_schema::ImageAssemblyConfig =
            builder.build(outdir, &tools).unwrap();

        assert_eq!(
            result.base,
            vec![
                outdir.join("base_package0"),
                outdir.join("driver-manager-base-config/package_manifest.json")
            ]
        );
        assert_eq!(result.cache, vec![outdir.join("cache_package0")]);
        assert_eq!(result.system, vec![outdir.join("sys_package0")]);
        assert_eq!(result.bootfs_packages, vec![outdir.join("bootfs_package0")]);
        assert_eq!(result.boot_args, vec!("boot_arg0".to_string()));
        assert_eq!(
            result.bootfs_files,
            vec!(FileEntry {
                source: outdir.join("source/path/to/file"),
                destination: "dest/file/path".into()
            })
        );

        assert_eq!(result.kernel.path, outdir.join("kernel/path"));
        assert_eq!(result.kernel.args, vec!("kernel_arg0".to_string()));
        assert_eq!(result.kernel.clock_backstop, 56244);
        assert_eq!(result.qemu_kernel, outdir.join("path/to/qemu/kernel"));
    }

    fn setup_builder(
        vars: &TempdirPathsForTest,
        bundles: Vec<AssemblyInputBundle>,
    ) -> ImageAssemblyConfigBuilder {
        let mut builder = ImageAssemblyConfigBuilder::default();

        // Write a file to the temp dir for use with config_data.
        std::fs::create_dir_all(&vars.config_data_target_package_dir).unwrap();
        std::fs::write(&vars.config_data_file_path, "configuration data").unwrap();
        for bundle in bundles {
            builder.add_parsed_bundle(&vars.bundle_path, bundle).unwrap();
        }
        builder
    }

    #[test]
    fn test_builder_with_config_data() {
        let vars = TempdirPathsForTest::new();
        let tools = FakeToolProvider::default();

        // Create an assembly bundle and add a config_data entry to it.
        let mut bundle = make_test_assembly_bundle(&vars.bundle_path);

        bundle.config_data.insert(
            vars.config_data_target_package_name.clone(),
            vec![FileEntry {
                source: vars.config_data_file_path.clone(),
                destination: "dest/file/path".to_owned(),
            }],
        );

        let builder = setup_builder(&vars, vec![bundle]);
        let result: assembly_config_schema::ImageAssemblyConfig =
            builder.build(&vars.outdir, &tools).unwrap();

        // config_data's manifest is in outdir
        let expected_config_data_manifest_path =
            vars.outdir.join("config_data").join("package_manifest.json");

        // Validate that the base package set contains config_data.
        assert_eq!(result.base.len(), 3);
        assert!(result.base.contains(&vars.bundle_path.join("base_package0")));
        assert!(result.base.contains(&expected_config_data_manifest_path));

        // Validate the contents of config_data is what is, expected by:
        // 1.  Reading in the package manifest to get the metafar path
        // 2.  Opening the metafar
        // 3.  Reading the config_data entry's file
        // 4.  Validate the contents of the file

        // 1. Read the config_data package manifest
        let config_data_manifest =
            PackageManifest::try_load_from(expected_config_data_manifest_path).unwrap();
        assert_eq!(config_data_manifest.name().as_ref(), "config-data");

        // and get the metafar path.
        let blobs = config_data_manifest.into_blobs();
        let metafar_blobinfo = blobs.get(0).unwrap();
        assert_eq!(metafar_blobinfo.path, "meta/");

        // 2. Read the metafar.
        let mut config_data_metafar = File::open(&metafar_blobinfo.source_path).unwrap();
        let mut far_reader = fuchsia_archive::Utf8Reader::new(&mut config_data_metafar).unwrap();

        // 3.  Read the configuration file.
        let config_file_data = far_reader
            .read_file(&format!(
                "meta/data/{}/dest/file/path",
                vars.config_data_target_package_name
            ))
            .unwrap();

        // 4.  Validate its contents.
        assert_eq!(config_file_data, "configuration data".as_bytes());
    }

    #[test]
    fn test_builder_with_shell_commands() {
        let vars = TempdirPathsForTest::new();
        let tools = FakeToolProvider::default();

        // Make an assembly input bundle with Shell Commands in it
        let mut bundle = make_test_assembly_bundle(&vars.bundle_path);
        bundle.shell_commands.insert(
            "package1".to_string(),
            BTreeSet::from([
                PackageInternalPathBuf::from("bin/binary1"),
                PackageInternalPathBuf::from("bin/binary2"),
            ]),
        );
        let builder = setup_builder(&vars, vec![bundle]);

        let result: assembly_config_schema::ImageAssemblyConfig =
            builder.build(&vars.outdir, &tools).unwrap();

        // config_data's manifest is in outdir
        let expected_manifest_path =
            vars.outdir.join("shell-commands").join("package_manifest.json");

        // Validate that the base package set contains shell_commands.
        assert_eq!(result.base.len(), 3);
        assert!(result.base.contains(&expected_manifest_path));
    }

    #[test]
    fn test_builder_with_product_packages_and_config() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tools = FakeToolProvider::default();

        // Create some config_data source files
        let config_data_source_dir = outdir.join("config_data_source");
        let config_data_source_a = config_data_source_dir.join("cfg.txt");
        let config_data_source_b = config_data_source_dir.join("other.json");
        std::fs::create_dir_all(&config_data_source_dir).unwrap();
        std::fs::write(&config_data_source_a, "source a").unwrap();
        std::fs::write(&config_data_source_b, "{}").unwrap();

        let packages = ProductPackagesConfig {
            base: vec![
                write_empty_pkg(outdir, "base_a", None).into(),
                ProductPackageDetails {
                    manifest: write_empty_pkg(outdir, "base_b", None),
                    config_data: Vec::default(),
                },
                ProductPackageDetails {
                    manifest: write_empty_pkg(outdir, "base_c", None),
                    config_data: vec![
                        ProductConfigData {
                            destination: "dest/path/cfg.txt".into(),
                            source: config_data_source_a.into(),
                        },
                        ProductConfigData {
                            destination: "other_data.json".into(),
                            source: config_data_source_b.into(),
                        },
                    ],
                },
            ],
            cache: vec![
                write_empty_pkg(outdir, "cache_a", None).into(),
                write_empty_pkg(outdir, "cache_b", None).into(),
            ],
        };

        let mut builder = get_minimum_config_builder(
            outdir,
            vec!["platform_a".to_owned(), "platform_b".to_owned()],
        );
        builder.add_product_packages(packages).unwrap();
        let result: assembly_config_schema::ImageAssemblyConfig =
            builder.build(outdir, &tools).unwrap();

        assert_eq!(
            result.base,
            [
                "base_a",
                "base_b",
                "base_c",
                "config_data/package_manifest.json",
                "driver-manager-base-config/package_manifest.json",
                "platform_a",
                "platform_b",
            ]
            .iter()
            .map(|p| outdir.join(p))
            .collect::<Vec<_>>()
        );
        assert_eq!(result.cache, vec![outdir.join("cache_a"), outdir.join("cache_b")]);

        // Validate product-provided config-data is correct
        let config_data_pkg =
            PackageManifest::try_load_from(outdir.join("config_data/package_manifest.json"))
                .unwrap();
        let metafar_blobinfo = config_data_pkg.blobs().iter().find(|b| b.path == "meta/").unwrap();
        let mut far_reader =
            fuchsia_archive::Utf8Reader::new(File::open(&metafar_blobinfo.source_path).unwrap())
                .unwrap();

        // Assert both config_data files match those written above
        let config_data_a_bytes =
            far_reader.read_file("meta/data/base_c/dest/path/cfg.txt").unwrap();
        let config_data_a = std::str::from_utf8(&config_data_a_bytes).unwrap();
        let config_data_b_bytes = far_reader.read_file("meta/data/base_c/other_data.json").unwrap();
        let config_data_b = std::str::from_utf8(&config_data_b_bytes).unwrap();
        assert_eq!(config_data_a, "source a");
        assert_eq!(config_data_b, "{}");
    }

    #[test]
    fn test_builder_with_product_drivers() -> Result<()> {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tools = FakeToolProvider::default();

        let mut builder = get_minimum_config_builder(
            outdir,
            vec!["platform_a".to_owned(), "platform_b".to_owned()],
        );
        let base_driver_1 = make_test_driver("driver1", outdir)?;
        let base_driver_2 = make_test_driver("driver2", outdir)?;

        builder.add_product_drivers(vec![base_driver_1, base_driver_2])?;
        let result: assembly_config_schema::ImageAssemblyConfig =
            builder.build(outdir, &tools).unwrap();

        assert_eq!(
            result.base.iter().map(|p| p.to_owned()).sorted().collect::<Vec<_>>(),
            vec![
                "driver-manager-base-config/package_manifest.json",
                "driver1",
                "driver2",
                "platform_a",
                "platform_b"
            ]
            .iter()
            .map(|p| outdir.join(p))
            .sorted()
            .collect::<Vec<_>>()
        );

        let driver_manifest: Vec<DriverManifest> = serde_json::from_reader(BufReader::new(
            File::open(outdir.join("driver-manager-base-config/config/base-driver-manifest.json"))?,
        ))?;
        assert_eq!(
            driver_manifest,
            vec![
                DriverManifest {
                    driver_url: "fuchsia-pkg://testrepository.com/driver1#meta/foobar.cm"
                        .to_owned()
                },
                DriverManifest {
                    driver_url: "fuchsia-pkg://testrepository.com/driver2#meta/foobar.cm"
                        .to_owned()
                }
            ]
        );

        Ok(())
    }

    #[test]
    fn test_builder_with_compiled_packages() -> Result<()> {
        let vars = TempdirPathsForTest::new();
        let tools = FakeToolProvider::default();
        // Write the expected output component files since the component
        // compiler is mocked.
        let component1_dir = vars.outdir.join("foo/component1");
        let component2_dir = vars.outdir.join("foo/component2");
        std::fs::create_dir_all(&component1_dir).unwrap();
        std::fs::create_dir_all(&component2_dir).unwrap();
        std::fs::write(component1_dir.join("component1.cm"), "component fake contents").unwrap();
        std::fs::write(component2_dir.join("component2.cm"), "component fake contents").unwrap();

        // Create 2 assembly bundle and add a config_data entry to it.
        let mut bundle1 = make_test_assembly_bundle(&vars.bundle_path);
        bundle1.packages_to_compile.push(CompiledPackageDefinition::MainDefinition(
            MainPackageDefinition {
                name: "foo".into(),
                components: BTreeMap::from([
                    ("component1".into(), "cml1".into()),
                    ("component2".into(), "cml2".into()),
                ]),
                contents: Vec::default(),
                includes: Vec::default(),
            },
        ));
        let bundle2 = AssemblyInputBundle {
            packages_to_compile: vec![CompiledPackageDefinition::Additional(
                AdditionalPackageContents {
                    name: "foo".into(),
                    component_shards: BTreeMap::from([(
                        "component2".into(),
                        vec!["shard1".into()],
                    )]),
                },
            )],
            ..Default::default()
        };

        let builder = setup_builder(&vars, vec![bundle1, bundle2]);
        let _: assembly_config_schema::ImageAssemblyConfig =
            builder.build(&vars.outdir, &tools).unwrap();

        // Make sure all the components and CML shards from the separate bundles
        // are merged.
        let expected_commands: ToolCommandLog = serde_json::from_value(json!({
            "commands": [
                {
                    "tool": "./host_x64/cmc",
                    "args": [
                        "merge",
                         "--output",
                          vars.outdir.join("foo/component1/component1.cml").as_str(),
                          vars.outdir.join("bundle/cml1").as_str()
                    ]
                },
                {
                    "tool": "./host_x64/cmc",
                    "args": [
                        "compile",
                        "--includeroot",
                        vars.outdir.join("bundle/compiled_packages/include").as_str(),
                        "--includepath",
                        vars.outdir.join("bundle/compiled_packages/include").as_str(),
                        "-o",
                        vars.outdir.join("foo/component1/component1.cm").as_str(),
                        vars.outdir.join("foo/component1/component1.cml").as_str()
                    ]
                },
                {
                    "tool": "./host_x64/cmc",
                    "args": [
                        "merge",
                        "--output",
                        vars.outdir.join("foo/component2/component2.cml").as_str(),
                        vars.outdir.join("bundle/cml2").as_str(),
                        vars.outdir.join("bundle/shard1")
                    ]
                },
                {
                    "tool": "./host_x64/cmc",
                    "args": [
                        "compile",
                        "--includeroot",
                        vars.outdir.join("bundle/compiled_packages/include").as_str(),
                        "--includepath",
                        vars.outdir.join("bundle/compiled_packages/include").as_str(),
                        "-o",
                        vars.outdir.join("foo/component2/component2.cm").as_str(),
                        vars.outdir.join("foo/component2/component2.cml").as_str()
                    ]
                }
            ]
        }))
        .unwrap();
        assert_eq!(&expected_commands, tools.log());

        Ok(())
    }

    #[test]
    fn test_builder_with_product_packages_catches_duplicates() -> Result<()> {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();

        let packages = ProductPackagesConfig {
            base: vec![write_empty_pkg(outdir, "base_a", None).into()],
            ..ProductPackagesConfig::default()
        };
        let mut builder = get_minimum_config_builder(outdir, vec!["base_a".to_owned()]);

        let result = builder.add_product_packages(packages);
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_builder_with_product_drivers_catches_duplicates() -> Result<()> {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();

        let base_driver_1 = make_test_driver("driver1", outdir)?;
        let mut builder = get_minimum_config_builder(outdir, vec!["driver1".to_owned()]);

        let result = builder.add_product_drivers(vec![base_driver_1]);

        assert!(result.is_err());
        Ok(())
    }

    /// Helper to duplicate the first item in an Vec<T: Clone> and make it also
    /// the last item. This intentionally panics if the Vec is empty.
    fn duplicate_first<T: Clone>(vec: &mut Vec<T>) {
        vec.push(vec.first().unwrap().clone());
    }

    #[test]
    fn test_builder_catches_dupe_base_pkgs_in_aib() {
        let temp = TempDir::new().unwrap();
        let root = Utf8Path::from_path(temp.path()).unwrap();

        let mut aib = make_test_assembly_bundle(root);
        duplicate_first(&mut aib.image_assembly.base);

        let mut builder = ImageAssemblyConfigBuilder::default();
        assert!(builder.add_parsed_bundle(root, aib).is_err());
    }

    #[test]
    fn test_builder_catches_dupe_cache_pkgs_in_aib() {
        let temp = TempDir::new().unwrap();
        let root = Utf8Path::from_path(temp.path()).unwrap();

        let mut aib = make_test_assembly_bundle(root);
        duplicate_first(&mut aib.image_assembly.cache);

        let mut builder = ImageAssemblyConfigBuilder::default();
        assert!(builder.add_parsed_bundle(root, aib).is_err());
    }

    #[test]
    fn test_builder_catches_dupe_system_pkgs_in_aib() {
        let temp = TempDir::new().unwrap();
        let root = Utf8Path::from_path(temp.path()).unwrap();

        let mut aib = make_test_assembly_bundle(root);
        duplicate_first(&mut aib.image_assembly.system);

        let mut builder = ImageAssemblyConfigBuilder::default();
        assert!(builder.add_parsed_bundle(root, aib).is_err());
    }

    #[test]
    fn test_builder_catches_dupe_bootfs_pkgs_in_aib() {
        let temp = TempDir::new().unwrap();
        let root = Utf8Path::from_path(temp.path()).unwrap();

        let mut aib = make_test_assembly_bundle(root);
        duplicate_first(&mut aib.image_assembly.bootfs_packages);

        let mut builder = ImageAssemblyConfigBuilder::default();
        assert!(builder.add_parsed_bundle(root, aib).is_err());
    }

    fn test_duplicates_across_aibs_impl<
        T: Clone,
        F: Fn(&mut AssemblyInputBundle) -> &mut Vec<T>,
    >(
        accessor: F,
    ) {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();

        let mut aib = make_test_assembly_bundle(outdir);
        let mut second_aib = AssemblyInputBundle::default();

        let first_list = (accessor)(&mut aib);
        let second_list = (accessor)(&mut second_aib);

        // Clone the first item in the first AIB into the same list in the
        // second AIB to create a duplicate item across the two AIBs.
        let value = first_list.get(0).unwrap();
        second_list.push(value.clone());

        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(outdir, aib).unwrap();
        assert!(builder.add_parsed_bundle(outdir.join("second"), second_aib).is_err());
    }

    #[test]
    fn test_builder_catches_dupe_base_pkgs_across_aibs() {
        test_duplicates_across_aibs_impl(|a| &mut a.image_assembly.base);
    }

    #[test]
    fn test_builder_catches_dupe_cache_pkgs_across_aibs() {
        test_duplicates_across_aibs_impl(|a| &mut a.image_assembly.cache);
    }

    #[test]
    fn test_builder_catches_dupe_system_pkgs_across_aibs() {
        test_duplicates_across_aibs_impl(|a| &mut a.image_assembly.system);
    }

    #[test]
    fn test_builder_catches_dupe_bootfs_files_across_aibs() {
        test_duplicates_across_aibs_impl(|a| &mut a.image_assembly.bootfs_files);
    }

    fn assert_two_pkgs_same_name_diff_path_errors() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tmp_path1 = TempDir::new_in(outdir).unwrap();
        let dir_path1 = Utf8Path::from_path(tmp_path1.path()).unwrap();
        let tmp_path2 = TempDir::new_in(outdir).unwrap();
        let dir_path2 = Utf8Path::from_path(tmp_path2.path()).unwrap();
        let aib = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: vec![
                    write_empty_pkg(dir_path1, "base_package2", None).into(),
                    write_empty_pkg(dir_path2, "base_package2", None).into(),
                ],
                ..Default::default()
            },
            ..Default::default()
        };
        let mut builder = ImageAssemblyConfigBuilder::default();
        assert!(builder.add_parsed_bundle(outdir, aib).is_err());
    }

    #[test]
    /// Asserts that attempting to add a package to the base package set with the same
    /// PackageName but a different package manifest path will result in an error if coming
    /// from the same AIB
    fn test_builder_catches_same_name_diff_path_one_aib() {
        assert_two_pkgs_same_name_diff_path_errors();
    }

    fn assert_two_pkgs_same_name_diff_path_across_aibs_errors() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tmp_path1 = TempDir::new_in(outdir).unwrap();
        let dir_path1 = Utf8Path::from_path(tmp_path1.path()).unwrap();
        let tmp_path2 = TempDir::new_in(outdir).unwrap();
        let dir_path2 = Utf8Path::from_path(tmp_path2.path()).unwrap();
        let tools = FakeToolProvider::default();
        let aib = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: vec![write_empty_pkg(dir_path1, "base_package2", None).into()],
                ..Default::default()
            },
            ..Default::default()
        };

        let aib2 = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: vec![write_empty_pkg(dir_path2, "base_package2", None).into()],
                ..Default::default()
            },
            ..Default::default()
        };

        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(outdir, aib).ok();
        builder.add_parsed_bundle(outdir, aib2).ok();
        assert!(builder.build(outdir, &tools).is_err());
    }
    /// Asserts that attempting to add a package to the base package set with the same
    /// PackageName but a different package manifest path will result in an error if coming
    /// from DIFFERENT AIBs
    #[test]
    fn test_builder_catches_same_name_diff_path_multi_aib() {
        assert_two_pkgs_same_name_diff_path_across_aibs_errors();
    }

    #[test]
    fn test_builder_catches_dupes_across_package_sets() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tmp_path1 = TempDir::new_in(outdir).unwrap();
        let dir_path1 = Utf8Path::from_path(tmp_path1.path()).unwrap();
        let tmp_path2 = TempDir::new_in(outdir).unwrap();
        let dir_path2 = Utf8Path::from_path(tmp_path2.path()).unwrap();
        let aib = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: vec![write_empty_pkg(dir_path1, "foo", None).into()],
                ..Default::default()
            },
            ..Default::default()
        };

        let aib2 = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                cache: vec![write_empty_pkg(dir_path2, "foo", None).into()],
                ..Default::default()
            },
            ..Default::default()
        };
        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(outdir, aib).ok();
        assert!(builder.add_parsed_bundle(outdir, aib2).is_err());
    }

    #[test]
    fn test_builder_allows_dupes_assuming_different_package_url() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let tmp_path1 = TempDir::new_in(outdir).unwrap();
        let dir_path1 = Utf8Path::from_path(tmp_path1.path()).unwrap();
        let tmp_path2 = TempDir::new_in(outdir).unwrap();
        let dir_path2 = Utf8Path::from_path(tmp_path2.path()).unwrap();
        let aib = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                base: vec![write_empty_pkg(dir_path1, "foo", Some("fuchsia.com")).into()],
                ..Default::default()
            },
            ..Default::default()
        };

        let aib2 = AssemblyInputBundle {
            image_assembly: assembly_config_schema::PartialImageAssemblyConfig {
                cache: vec![write_empty_pkg(dir_path2, "foo", Some("google.com")).into()],
                ..Default::default()
            },
            ..Default::default()
        };

        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(outdir, aib).ok();
        builder.add_parsed_bundle(outdir, aib2).unwrap();
    }

    #[test]
    fn test_builder_catches_dupe_config_data_across_aibs() {
        let temp = TempDir::new().unwrap();
        let root = Utf8Path::from_path(temp.path()).unwrap();

        let mut first_aib = make_test_assembly_bundle(root);
        let mut second_aib = AssemblyInputBundle::default();

        let config_data_file_entry = FileEntry {
            source: "source/path/to/file".into(),
            destination: "dest/file/path".into(),
        };

        first_aib.config_data.insert("base_package0".into(), vec![config_data_file_entry.clone()]);
        second_aib.config_data.insert("base_package0".into(), vec![config_data_file_entry]);

        let mut builder = ImageAssemblyConfigBuilder::default();
        builder.add_parsed_bundle(root, first_aib).unwrap();
        assert!(builder.add_parsed_bundle(root.join("second"), second_aib).is_err());
    }
}
