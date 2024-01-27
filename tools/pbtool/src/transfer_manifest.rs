// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Generate a transfer manifest for uploading and downloading a product bundle.

use anyhow::{bail, Context, Result};
use argh::FromArgs;
use assembly_manifest::AssemblyManifest;
use camino::{Utf8Path, Utf8PathBuf};
use pathdiff::diff_utf8_paths;
use sdk_metadata::{ProductBundle, VirtualDevice, VirtualDeviceManifest};
use serde::Serialize;
use std::collections::HashSet;
use std::fs::File;
use std::str::FromStr;
use transfer_manifest::{
    ArtifactEntry, ArtifactType, TransferEntry, TransferManifest, TransferManifestV1,
};
use walkdir::{DirEntry, WalkDir};

/// Generate a transfer manifest for uploading and downloading a product bundle.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "generate-transfer-manifest")]
pub struct GenerateTransferManifest {
    /// path to a product bundle.
    #[argh(option)]
    product_bundle: Utf8PathBuf,

    /// path to the directory to write the transfer manifest, all_blobs.json, images.json, and
    /// targets.json.
    #[argh(option)]
    out_dir: Utf8PathBuf,
}

/// The format of all_blobs.json.
type AllBlobsJson = Vec<AllBlobsEntry>;

/// A single blob entry in all_blobs.json.
#[derive(Serialize, Debug, Hash, PartialEq, PartialOrd, Eq, Ord)]
struct AllBlobsEntry {
    /// The merkle-root hash of the blob.
    merkle: fuchsia_merkle::Hash,
}

impl GenerateTransferManifest {
    /// Generate the transfer manifest into `output`.
    pub async fn generate(self) -> Result<()> {
        let product_bundle = ProductBundle::try_load_from(&self.product_bundle)?;
        let product_bundle = match product_bundle {
            ProductBundle::V1(_) => bail!("Only v2 product bundles are supported"),
            ProductBundle::V2(pb) => pb,
        };

        let canonical_out_dir =
            &self.out_dir.canonicalize_utf8().context("canonicalizing out_dir")?;
        let canonical_product_bundle_path = &self
            .product_bundle
            .canonicalize_utf8()
            .context("canonicalizing product bundle path")?;

        let mut entries = vec![];

        // Add all the blobs to the transfer manifest.
        // And collect them into all_blobs.json.
        let mut all_blobs = HashSet::new();
        for repository in &product_bundle.repositories {
            let canonical_blobs_path = canonical_product_bundle_path.join(&repository.blobs_path);
            let blobs = repository
                .blobs()
                .await
                .with_context(|| format!("gathering blobs from repository: {}", repository.name))?;
            let mut blob_entries: Vec<ArtifactEntry> =
                blobs.iter().map(|p| ArtifactEntry { name: p.into() }).collect();
            blob_entries.sort();
            for blob in blobs.iter() {
                let merkle = fuchsia_merkle::Hash::from_str(&blob.to_string())?;
                all_blobs.insert(AllBlobsEntry { merkle });
            }
            let local = diff_utf8_paths(canonical_blobs_path, canonical_out_dir)
                .context("rebasing blobs path")?;
            let blob_transfer = TransferEntry {
                artifact_type: ArtifactType::Blobs,
                local,
                remote: "blobs".into(),
                entries: blob_entries,
            };
            entries.push(blob_transfer);
        }

        // Collect all the product bundle entries.
        let mut product_bundle_entries = vec![];
        product_bundle_entries.push(ArtifactEntry { name: "product_bundle.json".into() });
        for partition in &product_bundle.partitions.bootstrap_partitions {
            product_bundle_entries.push(ArtifactEntry {
                name: diff_utf8_paths(&partition.image, &canonical_product_bundle_path)
                    .context("rebasing bootstrap partition")?,
            });
        }
        for partition in &product_bundle.partitions.bootloader_partitions {
            product_bundle_entries.push(ArtifactEntry {
                name: diff_utf8_paths(&partition.image, &canonical_product_bundle_path)
                    .context("rebasing bootloader partition")?,
            });
        }
        for credential in &product_bundle.partitions.unlock_credentials {
            product_bundle_entries.push(ArtifactEntry {
                name: diff_utf8_paths(&credential, &canonical_product_bundle_path)
                    .context("rebasing unlock credential")?,
            });
        }

        // Add the images from the systems.
        let mut system = |system: &Option<AssemblyManifest>| -> Result<()> {
            if let Some(system) = system {
                for image in &system.images {
                    product_bundle_entries.push(ArtifactEntry {
                        name: diff_utf8_paths(image.source(), &canonical_product_bundle_path)
                            .context("rebasing system image")?,
                    });
                }
            }
            Ok(())
        };
        system(&product_bundle.system_a)?;
        system(&product_bundle.system_b)?;
        system(&product_bundle.system_r)?;

        // Add virtual devices.
        if let Some(manifest_path) = &product_bundle.virtual_devices_path {
            product_bundle_entries.push(ArtifactEntry {
                name: diff_utf8_paths(manifest_path, &canonical_product_bundle_path)
                    .context("rebasing virtual device manifest path")?,
            });
            let manifest_dir = manifest_path.parent().unwrap_or("".into());
            let manifest = VirtualDeviceManifest::from_path(&product_bundle.virtual_devices_path)
                .context("manifest from_path")?;
            for path in manifest.device_paths.values() {
                let virtual_device_path = manifest_dir.join(path);
                product_bundle_entries.push(ArtifactEntry {
                    name: diff_utf8_paths(&virtual_device_path, &canonical_product_bundle_path)
                        .context("rebasing virtual device path")?,
                });
                let virtual_device_dir = virtual_device_path.parent().unwrap_or("".into());
                match VirtualDevice::try_load_from(&virtual_device_path)? {
                    VirtualDevice::V1(virtual_device) => {
                        let template_path =
                            virtual_device_dir.join(&virtual_device.start_up_args_template);
                        product_bundle_entries.push(ArtifactEntry {
                            name: diff_utf8_paths(template_path, &canonical_product_bundle_path)
                                .context("rebasing virtual device template path")?,
                        });
                    }
                }
            }
        }

        // Add the tuf metadata by walking the metadata directories and listing all the files inside.
        for repository in &product_bundle.repositories {
            let entries: Result<Vec<DirEntry>, _> =
                WalkDir::new(&repository.metadata_path).into_iter().collect();
            let entries = entries.with_context(|| {
                format!("collecting tuf metadata from repository: {}", repository.name)
            })?;
            for entry in entries {
                if entry.file_type().is_file() {
                    let entry_path =
                        Utf8Path::from_path(entry.path()).context("converting to UTF-8")?;

                    product_bundle_entries.push(ArtifactEntry {
                        name: diff_utf8_paths(entry_path, canonical_product_bundle_path)
                            .context("rebasing tuf metadata")?,
                    });
                }
            }
        }
        product_bundle_entries.sort();

        let rebased_product_bundle_path =
            diff_utf8_paths(canonical_product_bundle_path, canonical_out_dir)
                .context("rebasing product bundle directory")?;
        entries.push(TransferEntry {
            artifact_type: ArtifactType::Files,
            local: rebased_product_bundle_path,
            remote: "product_bundle".into(),
            entries: product_bundle_entries,
        });

        // Add targets.json for MOS if we have a repository.
        let mut mos_entries = vec![];
        if let Some(repository) = product_bundle.repositories.get(0) {
            let targets_path = self.out_dir.join("targets.json");
            std::fs::copy(repository.targets_path(), &targets_path)
                .context("copying the targets.json")?;
            mos_entries.push(ArtifactEntry { name: "targets.json".into() });
        }

        // Add all_blobs.json for MOS.
        let mut all_blobs = AllBlobsJson::from_iter(all_blobs);
        let all_blobs_path = self.out_dir.join("all_blobs.json");
        let all_blobs_file = File::create(&all_blobs_path).context("creating all_blobs.json")?;
        all_blobs.sort();
        serde_json::to_writer(all_blobs_file, &all_blobs).context("writing all_blobs.json")?;
        mos_entries.push(ArtifactEntry { name: "all_blobs.json".into() });

        // Add images.json for MOS and tests.
        // Find the first assembly in the preferential order of A, B, then R.
        let mut assembly = if let Some(a) = product_bundle.system_a {
            a.clone()
        } else if let Some(b) = product_bundle.system_b {
            b.clone()
        } else if let Some(r) = product_bundle.system_r {
            r.clone()
        } else {
            bail!("The product bundle does not have any assembly systems");
        };
        for image in &mut assembly.images {
            image.set_source(
                diff_utf8_paths(image.source(), canonical_out_dir)
                    .context("rebasing image path")?,
            );
        }
        let images_path = self.out_dir.join("images.json");
        let images_file = File::create(&images_path).context("creating images.json")?;
        serde_json::to_writer(images_file, &assembly).context("writing images.json")?;
        mos_entries.push(ArtifactEntry { name: "images.json".into() });

        entries.push(TransferEntry {
            artifact_type: ArtifactType::Files,
            local: "".into(),
            remote: "".into(),
            entries: mos_entries,
        });

        // Write the transfer manifest.
        let transfer_manifest = TransferManifest::V1(TransferManifestV1 { entries });
        let transfer_manifest_path = self.out_dir.join("transfer.json");
        let file = File::create(transfer_manifest_path).context("creating transfer manifest")?;
        serde_json::to_writer(file, &transfer_manifest).context("writing transfer manifest")?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use assembly_manifest::Image;
    use assembly_partitions_config::PartitionsConfig;
    use camino::Utf8Path;
    use fuchsia_repo::test_utils;
    use sdk_metadata::virtual_device::Hardware;
    use sdk_metadata::{ProductBundleV2, Repository, VirtualDeviceV1};
    use serde_json::json;
    use std::io::Write;
    use tempfile::tempdir;

    #[fuchsia::test]
    async fn test_generate() {
        let tmp = tempdir().unwrap();
        let tempdir = Utf8Path::from_path(tmp.path()).unwrap();

        let pb_path = tempdir.join("product_bundle");
        std::fs::create_dir_all(&pb_path).unwrap();

        let create_temp_file = |name: &str| -> Utf8PathBuf {
            let path = pb_path.join(name);
            let mut file = File::create(&path).unwrap();
            write!(file, "{}", name).unwrap();
            path
        };

        let _repo = test_utils::make_repo_dir(
            pb_path.join("repository").as_std_path(),
            &pb_path.join("blobs").as_std_path(),
        )
        .await;

        let vd_dir = pb_path.join("virtual_devices");
        std::fs::create_dir_all(&vd_dir).unwrap();

        let vd_manifest_path = vd_dir.join("manifest.json");
        let mut vd_manifest = VirtualDeviceManifest::default();
        vd_manifest.device_paths = [
            ("virtual_device_A".into(), "virtual_device_A.json".into()),
            ("virtual_device_B".into(), "virtual_device_B.json".into()),
            ("virtual_device_C".into(), "virtual_device_C.json".into()),
        ]
        .into();
        for (name, path) in &vd_manifest.device_paths {
            let mut vd = VirtualDeviceV1::new(name, Hardware::default());
            vd.start_up_args_template = vd_dir.join(format!("{name}_flags.json.template"));
            VirtualDevice::V1(vd).write(vd_dir.join(path)).unwrap();
        }
        let vd_manifest_file = File::create(vd_manifest_path.clone()).unwrap();
        serde_json::to_writer(vd_manifest_file, &vd_manifest).unwrap();

        let pb = ProductBundle::V2(ProductBundleV2 {
            product_name: "".to_string(),
            partitions: PartitionsConfig::default(),
            system_a: Some(AssemblyManifest {
                images: vec![
                    Image::ZBI { path: create_temp_file("zbi"), signed: false },
                    Image::FVM(create_temp_file("fvm")),
                    Image::QemuKernel(create_temp_file("kernel")),
                ],
            }),
            system_b: None,
            system_r: None,
            repositories: vec![Repository {
                name: "fuchsia.com".into(),
                metadata_path: pb_path.join("repository"),
                blobs_path: pb_path.join("blobs"),
            }],
            update_package_hash: None,
            virtual_devices_path: Some(vd_manifest_path),
        });
        pb.write(&pb_path).unwrap();

        let cmd = GenerateTransferManifest {
            product_bundle: pb_path.clone(),
            out_dir: tempdir.to_path_buf(),
        };
        cmd.generate().await.unwrap();

        let output = tempdir.join("transfer.json");
        let transfer_manifest_file = File::open(&output).unwrap();
        let transfer_manifest: TransferManifest =
            serde_json::from_reader(transfer_manifest_file).unwrap();
        assert_eq!(
            transfer_manifest,
            TransferManifest::V1(TransferManifestV1 {
                entries: vec![
                    TransferEntry {
                        artifact_type: transfer_manifest::ArtifactType::Blobs,
                        local: "product_bundle/blobs".into(),
                        remote: "blobs".into(),
                        entries: vec![
                            ArtifactEntry { name: "050907f009ff634f9aa57bff541fb9e9c2c62b587c23578e77637cda3bd69458".into() },
                            ArtifactEntry { name: "2881455493b5870aaea36537d70a2adc635f516ac2092598f4b6056dabc6b25d".into() },
                            ArtifactEntry { name: "548981eb310ddc4098fb5c63692e19ac4ae287b13d0e911fbd9f7819ac22491c".into() },
                            ArtifactEntry { name: "72e1e7a504f32edf4f23e7e8a3542c1d77d12541142261cfe272decfa75f542d".into() },
                            ArtifactEntry { name: "8a8a5f07f935a4e8e1fd1a1eda39da09bb2438ec0adfb149679ddd6e7e1fbb4f".into() },
                            ArtifactEntry { name: "ecc11f7f4b763c5a21be2b4159c9818bbe22ca7e6d8100a72f6a41d3d7b827a9".into() },
                        ]
                    },
                    TransferEntry {
                        artifact_type: transfer_manifest::ArtifactType::Files,
                        local: "product_bundle".into(),
                        remote: "product_bundle".into(),
                        entries: vec![
                            ArtifactEntry { name: "fvm".into() },
                            ArtifactEntry { name: "kernel".into() },
                            ArtifactEntry { name: "product_bundle.json".into() },
                            ArtifactEntry { name: "repository/1.root.json".into() },
                            ArtifactEntry { name: "repository/1.snapshot.json".into() },
                            ArtifactEntry { name: "repository/1.targets.json".into() },
                            ArtifactEntry { name: "repository/root.json".into() },
                            ArtifactEntry { name: "repository/snapshot.json".into() },
                            ArtifactEntry { name: "repository/targets/package1/2008b04d3e1c6a116619b4989973a1cee19d1fad3d89365cf2b020e65cd870d7.0".into() },
                            ArtifactEntry { name: "repository/targets/package2/1b0e8a06a242d49fbcdf24fa6bd1f8c0f2606afacafb47ba37bb1c45e700cce6.0".into() },
                            ArtifactEntry { name: "repository/targets.json".into() },
                            ArtifactEntry { name: "repository/timestamp.json".into() },
                            ArtifactEntry { name: "virtual_devices/manifest.json".into() },
                            ArtifactEntry { name: "virtual_devices/virtual_device_A.json".into() },
                            ArtifactEntry { name: "virtual_devices/virtual_device_A_flags.json.template".into() },
                            ArtifactEntry { name: "virtual_devices/virtual_device_B.json".into() },
                            ArtifactEntry { name: "virtual_devices/virtual_device_B_flags.json.template".into() },
                            ArtifactEntry { name: "virtual_devices/virtual_device_C.json".into() },
                            ArtifactEntry { name: "virtual_devices/virtual_device_C_flags.json.template".into() },
                            ArtifactEntry { name: "zbi".into() },
                        ]
                    },
                    TransferEntry {
                        artifact_type: transfer_manifest::ArtifactType::Files,
                        local: "".into(),
                        remote: "".into(),
                        entries: vec![
                            ArtifactEntry { name: "targets.json".into() },
                            ArtifactEntry { name: "all_blobs.json".into() },
                            ArtifactEntry { name: "images.json".into() },
                        ]
                    }
                ]
            }),
        );

        let all_blobs_path = tempdir.join("all_blobs.json");
        let all_blobs_file = File::open(&all_blobs_path).unwrap();
        let all_blobs_value: serde_json::Value = serde_json::from_reader(all_blobs_file).unwrap();
        assert_eq!(
            all_blobs_value,
            json!([
                   {
                       "merkle": "050907f009ff634f9aa57bff541fb9e9c2c62b587c23578e77637cda3bd69458"
                   },
                   {
                       "merkle": "2881455493b5870aaea36537d70a2adc635f516ac2092598f4b6056dabc6b25d"
                   },
                   {
                       "merkle": "548981eb310ddc4098fb5c63692e19ac4ae287b13d0e911fbd9f7819ac22491c"
                   },
                   {
                       "merkle": "72e1e7a504f32edf4f23e7e8a3542c1d77d12541142261cfe272decfa75f542d"
                   },
                   {
                       "merkle": "8a8a5f07f935a4e8e1fd1a1eda39da09bb2438ec0adfb149679ddd6e7e1fbb4f"
                   },
                   {
                       "merkle": "ecc11f7f4b763c5a21be2b4159c9818bbe22ca7e6d8100a72f6a41d3d7b827a9"
                   },
                ]
            )
        );

        let images_path = tempdir.join("images.json");
        let images_file = File::open(&images_path).unwrap();
        let images: AssemblyManifest = serde_json::from_reader(images_file).unwrap();
        assert_eq!(
            images,
            AssemblyManifest {
                images: vec![
                    Image::ZBI { path: "product_bundle/zbi".into(), signed: false },
                    Image::FVM("product_bundle/fvm".into()),
                    Image::QemuKernel("product_bundle/kernel".into()),
                ],
            },
        );

        let targets_path = tempdir.join("targets.json");
        assert!(targets_path.exists());
    }
}
