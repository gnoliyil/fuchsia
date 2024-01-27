// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! FFX plugin for constructing product bundles, which are distributable containers for a product's
//! images and packages, and can be used to emulate, flash, or update a product.

use anyhow::{Context, Result};
use assembly_manifest::{AssemblyManifest, BlobfsContents, Image, PackagesMetadata};
use assembly_partitions_config::PartitionsConfig;
use assembly_tool::{SdkToolProvider, ToolProvider};
use assembly_update_package::{Slot, UpdatePackageBuilder};
use assembly_update_packages_manifest::UpdatePackagesManifest;
use camino::{Utf8Path, Utf8PathBuf};
use epoch::EpochFile;
use ffx_core::ffx_plugin;
use ffx_product_create_args::CreateCommand;
use fuchsia_pkg::PackageManifest;
use fuchsia_repo::{
    repo_builder::RepoBuilder, repo_keys::RepoKeys, repository::FileSystemRepository,
};
use sdk_metadata::{
    ProductBundle, ProductBundleV2, Repository, VirtualDevice, VirtualDeviceManifest,
};
use std::fs::File;
use tempfile::TempDir;

/// Create a product bundle.
#[ffx_plugin("product.experimental")]
pub async fn pb_create(cmd: CreateCommand) -> Result<()> {
    let sdk_tools = SdkToolProvider::try_new().context("getting sdk tools")?;
    pb_create_with_tools(cmd, Box::new(sdk_tools)).await
}

/// Create a product bundle using the provided `tools`.
pub async fn pb_create_with_tools(cmd: CreateCommand, tools: Box<dyn ToolProvider>) -> Result<()> {
    // We build an update package if `update_version_file` or `update_epoch` is provided.
    // If we decide to build an update package, we need to ensure that both of them
    // are provided.
    let update_details =
        if cmd.update_package_version_file.is_some() || cmd.update_package_epoch.is_some() {
            if cmd.tuf_keys.is_none() {
                anyhow::bail!("TUF keys must be provided to build an update package");
            }
            let version = cmd.update_package_version_file.ok_or(anyhow::anyhow!(
                "A version file must be provided to build an update package"
            ))?;
            let epoch = cmd
                .update_package_epoch
                .ok_or(anyhow::anyhow!("A epoch must be provided to build an update package"))?;
            Some((version, epoch))
        } else {
            None
        };

    // Make sure `out_dir` is created and empty.
    if cmd.out_dir.exists() {
        if cmd.out_dir == "" || cmd.out_dir == "/" {
            anyhow::bail!("Avoiding deletion of an unsafe out directory: {}", &cmd.out_dir);
        }
        std::fs::remove_dir_all(&cmd.out_dir).context("Deleting the out_dir")?;
    }
    std::fs::create_dir_all(&cmd.out_dir).context("Creating the out_dir")?;

    let partitions = load_partitions_config(&cmd.partitions, &cmd.out_dir.join("partitions"))?;
    let (system_a, packages_a) =
        load_assembly_manifest(&cmd.system_a, &cmd.out_dir.join("system_a"))?;
    let (system_b, packages_b) =
        load_assembly_manifest(&cmd.system_b, &cmd.out_dir.join("system_b"))?;
    let (system_r, _packages_r) =
        load_assembly_manifest(&cmd.system_r, &cmd.out_dir.join("system_r"))?;

    // Generate the update packages if necessary.
    let (_gen_dir, update_package_hash, update_packages) =
        if let Some((version, epoch)) = update_details {
            let epoch: EpochFile = EpochFile::Version1 { epoch };
            let abi_revision = None;
            let gen_dir = TempDir::new().context("creating temporary directory")?;
            let mut builder = UpdatePackageBuilder::new(
                tools,
                partitions.clone(),
                partitions.hardware_revision.clone(),
                version,
                epoch,
                abi_revision,
                Utf8Path::from_path(gen_dir.path())
                    .context("checkinf if temporary directory is UTF-8")?,
            );
            let mut all_packages = UpdatePackagesManifest::default();
            for (_path, package) in &packages_a {
                all_packages.add_by_manifest(&package)?;
            }
            builder.add_packages(all_packages);
            if let Some(manifest) = &system_a {
                builder.add_slot_images(Slot::Primary(manifest.clone()));
            }
            if let Some(manifest) = &system_r {
                builder.add_slot_images(Slot::Recovery(manifest.clone()));
            }
            let update_package = builder.build()?;
            (Some(gen_dir), Some(update_package.merkle), update_package.package_manifests)
        } else {
            (None, None, vec![])
        };

    let repositories = if let Some(tuf_keys) = &cmd.tuf_keys {
        let repo_path = &cmd.out_dir;
        let metadata_path = repo_path.join("repository");
        let blobs_path = repo_path.join("blobs");
        let repo = FileSystemRepository::new(metadata_path.to_path_buf(), blobs_path.to_path_buf());
        let repo_keys =
            RepoKeys::from_dir(tuf_keys.as_std_path()).context("Gathering repo keys")?;

        RepoBuilder::create(&repo, &repo_keys)
            .add_package_manifests(packages_a.into_iter())
            .await?
            .add_package_manifests(packages_b.into_iter())
            .await?
            .add_package_manifests(update_packages.into_iter().map(|manifest| (None, manifest)))
            .await?
            .commit()
            .await
            .context("Building the repo")?;
        let name = "fuchsia.com".to_string();
        vec![Repository { name, metadata_path, blobs_path }]
    } else {
        vec![]
    };

    let mut virtual_devices_path = None;
    if !cmd.virtual_device.is_empty() {
        let vd_path = cmd.out_dir.join("virtual_devices");
        std::fs::create_dir_all(&vd_path).context("Creating the virtual_devices directory.")?;
        let mut manifest = VirtualDeviceManifest::default();
        for path in cmd.virtual_device {
            let device = VirtualDeviceManifest::parse_virtual_device_file(&path)
                .with_context(|| format!("Parsing file as virtual device: '{}'", path))?;
            match device {
                VirtualDevice::V1(ref device) => {
                    let template_path = path
                        .parent()
                        .expect(&format!("Given path has no parent: '{}'", path))
                        .join(&device.start_up_args_template);
                    copy_file(&template_path, &vd_path).with_context(|| {
                        format!("Copying template file to target directory: '{}'", template_path)
                    })?;
                }
            }
            let vd_file = File::create(
                vd_path
                    .join(path.file_name().expect(&format!("Path has no file name: '{}'", path))),
            )?;
            let name = path
                .file_stem()
                .expect(&format!("Couldn't determine virtual device name from path: '{}'", path));
            serde_json::to_writer(vd_file, &device)
                .context("Couldn't serialize virtual device to disk.")?;
            manifest.device_paths.insert(name.to_string(), path);
        }
        manifest.recommended = cmd.recommended_device;
        let manifest_path = vd_path.join("manifest.json");
        let manifest_file = File::create(&manifest_path)
            .with_context(|| format!("Couldn't create manifest file '{}'", manifest_path))?;
        serde_json::to_writer(manifest_file, &manifest)
            .context("Couldn't serialize manifest to disk.")?;
        virtual_devices_path = Some(manifest_path);
    }

    let product_bundle = ProductBundleV2 {
        partitions,
        system_a,
        system_b,
        system_r,
        repositories,
        update_package_hash,
        virtual_devices_path,
    };
    let product_bundle = ProductBundle::V2(product_bundle);
    product_bundle.write(&cmd.out_dir).context("writing product bundle")?;
    Ok(())
}

/// Open and parse a PartitionsConfig from a path, copying the images into `out_dir`.
fn load_partitions_config(
    path: impl AsRef<Utf8Path>,
    out_dir: impl AsRef<Utf8Path>,
) -> Result<PartitionsConfig> {
    let path = path.as_ref();
    let out_dir = out_dir.as_ref();

    // Make sure `out_dir` is created.
    std::fs::create_dir_all(&out_dir).context("Creating the out_dir")?;

    let partitions_file = File::open(path).context("Opening partitions config")?;
    let mut config: PartitionsConfig =
        serde_json::from_reader(partitions_file).context("Parsing partitions config")?;

    for cred in &mut config.unlock_credentials {
        *cred = copy_file(&cred, &out_dir)?;
    }
    for bootstrap in &mut config.bootstrap_partitions {
        bootstrap.image = copy_file(&bootstrap.image, &out_dir)?;
    }
    for bootloader in &mut config.bootloader_partitions {
        bootloader.image = copy_file(&bootloader.image, &out_dir)?;
    }

    Ok(config)
}

/// Open and parse an AssemblyManifest from a path, copying the images into `out_dir`.
/// Returns None if the given path is None.
fn load_assembly_manifest(
    path: &Option<Utf8PathBuf>,
    out_dir: impl AsRef<Utf8Path>,
) -> Result<(Option<AssemblyManifest>, Vec<(Option<Utf8PathBuf>, PackageManifest)>)> {
    let out_dir = out_dir.as_ref();

    if let Some(path) = path {
        // Make sure `out_dir` is created.
        std::fs::create_dir_all(&out_dir).context("Creating the out_dir")?;

        let file =
            File::open(path).with_context(|| format!("Opening assembly manifest: {}", path))?;
        let manifest: AssemblyManifest = serde_json::from_reader(file)
            .with_context(|| format!("Parsing assembly manifest: {}", path))?;

        // Filter out the base package, and the blobfs contents.
        let mut images = Vec::new();
        let mut packages = Vec::new();
        for image in manifest.images.into_iter() {
            match image {
                Image::BasePackage(..) => {}
                Image::BlobFS { path, contents } => {
                    let PackagesMetadata { base, cache } = contents.packages;
                    let all_packages = [base.0, cache.0].concat();
                    for package in all_packages {
                        let manifest = PackageManifest::try_load_from(&package.manifest)
                            .with_context(|| {
                                format!("reading package manifest: {}", package.manifest)
                            })?;
                        packages.push((Some(package.manifest), manifest));
                    }
                    images.push(Image::BlobFS { path, contents: BlobfsContents::default() });
                }
                _ => {
                    images.push(image);
                }
            }
        }

        // Copy the images to the `out_dir`.
        let mut new_images = Vec::<Image>::new();
        for mut image in images.into_iter() {
            let dest = copy_file(image.source(), &out_dir)?;
            image.set_source(dest);
            new_images.push(image);
        }

        Ok((Some(AssemblyManifest { images: new_images }), packages))
    } else {
        Ok((None, vec![]))
    }
}

/// Copy a file from `source` to `out_dir` preserving the filename.
/// Returns the destination, which is equal to {out_dir}{filename}.
fn copy_file(source: impl AsRef<Utf8Path>, out_dir: impl AsRef<Utf8Path>) -> Result<Utf8PathBuf> {
    let source = source.as_ref();
    let out_dir = out_dir.as_ref();
    let filename = source.file_name().context("getting file name")?;
    let destination = out_dir.join(filename);

    // Attempt to hardlink, if that fails, fall back to copying.
    if let Err(_) = std::fs::hard_link(source, &destination) {
        // falling back to copying.
        std::fs::copy(source, &destination)
            .with_context(|| format!("copying file '{}'", source))?;
    }
    Ok(destination)
}

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::bail;
    use assembly_manifest::AssemblyManifest;
    use assembly_partitions_config::PartitionsConfig;
    use assembly_tool::testing::FakeToolProvider;
    use fuchsia_repo::test_utils;
    use sdk_metadata::{VirtualDevice, VirtualDeviceV1};
    use std::io::Write;
    use tempfile::TempDir;

    const VIRTUAL_DEVICE_VALID: &str =
        include_str!("../../../../../../../build/sdk/meta/test_data/virtual_device.json");

    #[test]
    fn test_copy_file() {
        let temp1 = TempDir::new().unwrap();
        let tempdir1 = Utf8Path::from_path(temp1.path()).unwrap();
        let temp2 = TempDir::new().unwrap();
        let tempdir2 = Utf8Path::from_path(temp2.path()).unwrap();

        let source_path = tempdir1.join("source.txt");
        let mut source_file = File::create(&source_path).unwrap();
        write!(source_file, "contents").unwrap();
        let destination = copy_file(&source_path, tempdir2).unwrap();
        assert!(destination.exists());
    }

    #[test]
    fn test_load_partitions_config() {
        let temp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(temp.path()).unwrap();
        let pb_dir = tempdir.join("pb");

        let config_path = tempdir.join("config.json");
        let config_file = File::create(&config_path).unwrap();
        serde_json::to_writer(&config_file, &PartitionsConfig::default()).unwrap();

        let error_path = tempdir.join("error.json");
        let mut error_file = File::create(&error_path).unwrap();
        error_file.write_all("error".as_bytes()).unwrap();

        let parsed = load_partitions_config(&config_path, &pb_dir);
        assert!(parsed.is_ok());

        let error = load_partitions_config(&error_path, &pb_dir);
        assert!(error.is_err());
    }

    #[test]
    fn test_load_assembly_manifest() {
        let temp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(temp.path()).unwrap();
        let pb_dir = tempdir.join("pb");

        let manifest_path = tempdir.join("manifest.json");
        let manifest_file = File::create(&manifest_path).unwrap();
        serde_json::to_writer(&manifest_file, &AssemblyManifest::default()).unwrap();

        let error_path = tempdir.join("error.json");
        let mut error_file = File::create(&error_path).unwrap();
        error_file.write_all("error".as_bytes()).unwrap();

        let (parsed, packages) = load_assembly_manifest(&Some(manifest_path), &pb_dir).unwrap();
        assert!(parsed.is_some());
        assert_eq!(packages, Vec::new());

        let error = load_assembly_manifest(&Some(error_path), &pb_dir);
        assert!(error.is_err());

        let (none, _) = load_assembly_manifest(&None, &pb_dir).unwrap();
        assert!(none.is_none());
    }

    #[fuchsia::test]
    async fn test_pb_create_minimal() {
        let temp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(temp.path()).unwrap();
        let pb_dir = tempdir.join("pb");

        let partitions_path = tempdir.join("partitions.json");
        let partitions_file = File::create(&partitions_path).unwrap();
        serde_json::to_writer(&partitions_file, &PartitionsConfig::default()).unwrap();

        let tools = FakeToolProvider::default();
        pb_create_with_tools(
            CreateCommand {
                partitions: partitions_path,
                system_a: None,
                system_b: None,
                system_r: None,
                tuf_keys: None,
                update_package_version_file: None,
                update_package_epoch: None,
                virtual_device: vec![],
                recommended_device: None,
                out_dir: pb_dir.clone(),
            },
            Box::new(tools),
        )
        .await
        .unwrap();

        let pb = ProductBundle::try_load_from(pb_dir).unwrap();
        assert_eq!(
            pb,
            ProductBundle::V2(ProductBundleV2 {
                partitions: PartitionsConfig::default(),
                system_a: None,
                system_b: None,
                system_r: None,
                repositories: vec![],
                update_package_hash: None,
                virtual_devices_path: None,
            })
        );
    }

    #[fuchsia::test]
    async fn test_pb_create_a_and_r() {
        let temp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(temp.path()).unwrap();
        let pb_dir = tempdir.join("pb");

        let partitions_path = tempdir.join("partitions.json");
        let partitions_file = File::create(&partitions_path).unwrap();
        serde_json::to_writer(&partitions_file, &PartitionsConfig::default()).unwrap();

        let system_path = tempdir.join("system.json");
        let system_file = File::create(&system_path).unwrap();
        serde_json::to_writer(&system_file, &AssemblyManifest::default()).unwrap();

        let tools = FakeToolProvider::default();
        pb_create_with_tools(
            CreateCommand {
                partitions: partitions_path,
                system_a: Some(system_path.clone()),
                system_b: None,
                system_r: Some(system_path.clone()),
                tuf_keys: None,
                update_package_version_file: None,
                update_package_epoch: None,
                virtual_device: vec![],
                recommended_device: None,
                out_dir: pb_dir.clone(),
            },
            Box::new(tools),
        )
        .await
        .unwrap();

        let pb = ProductBundle::try_load_from(pb_dir).unwrap();
        assert_eq!(
            pb,
            ProductBundle::V2(ProductBundleV2 {
                partitions: PartitionsConfig::default(),
                system_a: Some(AssemblyManifest::default()),
                system_b: None,
                system_r: Some(AssemblyManifest::default()),
                repositories: vec![],
                update_package_hash: None,
                virtual_devices_path: None,
            })
        );
    }

    #[fuchsia::test]
    async fn test_pb_create_a_and_r_and_repository() {
        let temp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(temp.path()).unwrap();
        let pb_dir = tempdir.join("pb");

        let partitions_path = tempdir.join("partitions.json");
        let partitions_file = File::create(&partitions_path).unwrap();
        serde_json::to_writer(&partitions_file, &PartitionsConfig::default()).unwrap();

        let system_path = tempdir.join("system.json");
        let system_file = File::create(&system_path).unwrap();
        serde_json::to_writer(&system_file, &AssemblyManifest::default()).unwrap();

        let tuf_keys = tempdir.join("keys");
        test_utils::make_repo_keys_dir(&tuf_keys);

        let tools = FakeToolProvider::default();
        pb_create_with_tools(
            CreateCommand {
                partitions: partitions_path,
                system_a: Some(system_path.clone()),
                system_b: None,
                system_r: Some(system_path.clone()),
                tuf_keys: Some(tuf_keys),
                update_package_version_file: None,
                update_package_epoch: None,
                virtual_device: vec![],
                recommended_device: None,
                out_dir: pb_dir.clone(),
            },
            Box::new(tools),
        )
        .await
        .unwrap();

        let pb = ProductBundle::try_load_from(&pb_dir).unwrap();
        assert_eq!(
            pb,
            ProductBundle::V2(ProductBundleV2 {
                partitions: PartitionsConfig::default(),
                system_a: Some(AssemblyManifest::default()),
                system_b: None,
                system_r: Some(AssemblyManifest::default()),
                repositories: vec![Repository {
                    name: "fuchsia.com".into(),
                    metadata_path: pb_dir.join("repository"),
                    blobs_path: pb_dir.join("blobs"),
                }],
                update_package_hash: None,
                virtual_devices_path: None,
            })
        );
    }

    #[fuchsia::test]
    async fn test_pb_create_with_update() {
        let tmp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(tmp.path()).unwrap();

        let pb_dir = tempdir.join("pb");

        let partitions_path = tempdir.join("partitions.json");
        let partitions_file = File::create(&partitions_path).unwrap();
        serde_json::to_writer(&partitions_file, &PartitionsConfig::default()).unwrap();

        let version_path = tempdir.join("version.txt");
        std::fs::write(&version_path, "").unwrap();

        let tuf_keys = tempdir.join("keys");
        test_utils::make_repo_keys_dir(&tuf_keys);

        let tools = FakeToolProvider::default();
        pb_create_with_tools(
            CreateCommand {
                partitions: partitions_path,
                system_a: None,
                system_b: None,
                system_r: None,
                tuf_keys: Some(tuf_keys),
                update_package_version_file: Some(version_path),
                update_package_epoch: Some(1),
                virtual_device: vec![],
                recommended_device: None,
                out_dir: pb_dir.clone(),
            },
            Box::new(tools),
        )
        .await
        .unwrap();

        let pb = ProductBundle::try_load_from(&pb_dir).unwrap();
        // NB: do not assert on the package hash because this test is not hermetic; platform
        // changes such as API level bumps may change the package hash and such changes are
        // immaterial to the code under test here.
        assert_matches::assert_matches!(
            pb,
            ProductBundle::V2(ProductBundleV2 {
                partitions,
                system_a: None,
                system_b: None,
                system_r: None,
                repositories,
                update_package_hash: Some(_),
                virtual_devices_path: None,
            }) if partitions == Default::default() && repositories == &[Repository {
                name: "fuchsia.com".into(),
                metadata_path: pb_dir.join("repository"),
                blobs_path: pb_dir.join("blobs"),
            }]
        );
    }

    #[fuchsia::test]
    async fn test_pb_create_with_virtual_devices() -> Result<()> {
        let temp = TempDir::new().unwrap();
        let tempdir = Utf8Path::from_path(temp.path()).unwrap();
        let pb_dir = tempdir.join("pb");

        let partitions_path = tempdir.join("partitions.json");
        let partitions_file = File::create(&partitions_path)?;
        serde_json::to_writer(&partitions_file, &PartitionsConfig::default())?;

        let vd_path1 = tempdir.join("device_1.json");
        let vd_path2 = tempdir.join("device_2.json");
        let template_path = tempdir.join("device_1.json.template");
        let mut vd_file1 = File::create(&vd_path1)?;
        let mut vd_file2 = File::create(&vd_path2)?;
        File::create(&template_path)?;
        vd_file1.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        vd_file2.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;

        let tools = FakeToolProvider::default();
        pb_create_with_tools(
            CreateCommand {
                partitions: partitions_path,
                system_a: None,
                system_b: None,
                system_r: None,
                tuf_keys: None,
                update_package_version_file: None,
                update_package_epoch: None,
                virtual_device: vec![vd_path1, vd_path2],
                recommended_device: Some("device_2".to_string()),
                out_dir: pb_dir.clone(),
            },
            Box::new(tools),
        )
        .await
        .unwrap();

        let pb = ProductBundle::try_load_from(&pb_dir).unwrap();
        assert_eq!(
            pb,
            ProductBundle::V2(ProductBundleV2 {
                partitions: PartitionsConfig::default(),
                system_a: None,
                system_b: None,
                system_r: None,
                repositories: vec![],
                update_package_hash: None,
                virtual_devices_path: Some(pb_dir.join("virtual_devices/manifest.json")),
            })
        );

        let internal_pb = match pb {
            ProductBundle::V2(pb) => pb,
            _ => bail!("We defined it as PBv2 above, so this can't happen."),
        };

        let path = internal_pb.get_virtual_devices_path();
        let manifest =
            VirtualDeviceManifest::from_path(&path).context("Manifest file from_path")?;
        let default = manifest.default_device();
        assert!(matches!(default, Ok(Some(VirtualDevice::V1(_)))), "{:?}", default);

        let devices = manifest.device_names();
        assert_eq!(devices.len(), 2);
        assert!(devices.contains(&"device_1".to_string()));
        assert!(devices.contains(&"device_2".to_string()));

        let device1 = manifest.device("device_1");
        assert!(device1.is_ok(), "{:?}", device1.unwrap_err());
        assert!(matches!(device1, Ok(VirtualDevice::V1(VirtualDeviceV1 { .. }))));

        let device2 = manifest.device("device_2");
        assert!(device2.is_ok(), "{:?}", device2.unwrap_err());
        assert!(matches!(device2, Ok(VirtualDevice::V1(VirtualDeviceV1 { .. }))));

        Ok(())
    }
}
