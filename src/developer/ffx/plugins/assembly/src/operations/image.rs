// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::base_package::{construct_base_package, BasePackage};
use crate::blobfs::construct_blobfs;
use crate::config::{from_reader, BoardConfig, PartialProductConfig, ProductConfig};
use crate::fvm::{construct_fvm, Fvms};
use crate::update_package::{construct_update, UpdatePackage};
use crate::vbmeta::construct_vbmeta;
use crate::zbi::{construct_zbi, vendor_sign_zbi};

use anyhow::{Context, Result};
use assembly_images_manifest::{Image, ImagesManifest};
use ffx_assembly_args::ImageArgs;
use ffx_config::get_sdk;
use futures::executor::block_on;
use log::info;
use std::fs::File;
use std::path::{Path, PathBuf};

pub fn assemble(args: ImageArgs) -> Result<()> {
    let ImageArgs { product: products, board, outdir, gendir } = args;

    info!("Loading configuration files.");
    info!("  product:");
    for product in &products {
        info!("    {}", product.display());
    }
    info!("    board:  {}", board.display());

    let (products, board) = read_configs(&products, board)?;
    let product = ProductConfig::try_from_partials(products)?;

    let gendir = gendir.unwrap_or(outdir.clone());

    // Use the sdk to get the host tool paths.
    let sdk = block_on(get_sdk())?;

    let base_package: Option<BasePackage> = if has_base_package(&product) {
        info!("Creating base package");
        Some(construct_base_package(&outdir, &gendir, &board.base_package_name, &product)?)
    } else {
        info!("Skipping base package creation");
        None
    };

    let blobfs_path: Option<PathBuf> = if let Some(base_package) = &base_package {
        info!("Creating the blobfs");
        Some(construct_blobfs(
            sdk.get_host_tool("blobfs")?,
            &outdir,
            &gendir,
            &product,
            &board.blobfs,
            &base_package,
        )?)
    } else {
        info!("Skipping blobfs creation");
        None
    };

    let fvms: Option<Fvms> = if let Some(fvm_config) = &board.fvm {
        info!("Creating the fvm");
        Some(construct_fvm(
            sdk.get_host_tool("fvm")?,
            sdk.get_host_tool("minfs")?,
            &outdir,
            &fvm_config,
            blobfs_path.as_ref(),
        )?)
    } else {
        info!("Skipping fvm creation");
        None
    };

    // If the FVM should be embedded in the ZBI, select the default one.
    let fvm_for_zbi: Option<&PathBuf> = match (&board.zbi.embed_fvm_in_zbi, &fvms) {
        (true, None) => {
            anyhow::bail!("Config indicates FVM should embed in ZBI, but no FVM was generated");
        }
        (true, Some(fvms)) => Some(&fvms.default),
        (false, _) => None,
    };

    info!("Creating the ZBI");
    let zbi_path = construct_zbi(
        sdk.get_host_tool("zbi")?,
        &outdir,
        &gendir,
        &product,
        &board,
        base_package.as_ref(),
        fvm_for_zbi,
    )?;

    let vbmeta_path: Option<PathBuf> = if let Some(vbmeta_config) = &board.vbmeta {
        info!("Creating the VBMeta image");
        Some(construct_vbmeta(&outdir, &board.zbi.name, vbmeta_config, &zbi_path)?)
    } else {
        info!("Skipping vbmeta creation");
        None
    };

    info!("Creating images manifest");
    let mut images_manifest = ImagesManifest::default();
    images_manifest.images.push(Image::ZBI(zbi_path.clone()));
    if let Some(base_package) = &base_package {
        images_manifest.images.push(Image::BasePackage(base_package.path.clone()));
    }
    if let Some(blobfs_path) = &blobfs_path {
        images_manifest.images.push(Image::BlobFS(blobfs_path.clone()));
    }
    if let Some(fvms) = &fvms {
        images_manifest.images.push(Image::FVM(fvms.default.clone()));
        images_manifest.images.push(Image::FVMSparse(fvms.sparse.clone()));
        images_manifest.images.push(Image::FVMSparseBlob(fvms.sparse_blob.clone()));
        if let Some(path) = &fvms.fastboot {
            images_manifest.images.push(Image::FVMFastboot(path.clone()));
        }
    }
    if let Some(vbmeta_path) = &vbmeta_path {
        images_manifest.images.push(Image::VBMeta(vbmeta_path.clone()));
    }
    let images_json_path = outdir.join("images.json");
    let images_json = File::create(images_json_path).context("Failed to create images.json")?;
    serde_json::to_writer(images_json, &images_manifest)
        .context("Failed to write to images.json")?;

    // If the board specifies a vendor-specific signing script, use that to
    // post-process the ZBI, and then use the post-processed ZBI in the update
    // package and the
    let zbi_for_update_path = if let Some(signing_config) = &board.zbi.signing_script {
        info!("Vendor signing the ZBI");
        vendor_sign_zbi(&outdir, &board, signing_config, &zbi_path)?
    } else {
        zbi_path
    };

    info!("Creating the update package");
    let _update_package: UpdatePackage = construct_update(
        &outdir,
        &gendir,
        &product,
        &board,
        &zbi_for_update_path,
        vbmeta_path,
        base_package.as_ref(),
    )?;

    Ok(())
}

fn read_configs(
    products: &[impl AsRef<Path>],
    board: impl AsRef<Path>,
) -> Result<(Vec<PartialProductConfig>, BoardConfig)> {
    let products = products
        .iter()
        .map(read_config)
        .collect::<Result<Vec<PartialProductConfig>>>()
        .context("Unable to parse product configs")?;

    let board: BoardConfig = read_config(board).context("Failed to read the board config")?;
    Ok((products, board))
}

fn read_config<T>(path: impl AsRef<Path>) -> Result<T>
where
    T: serde::de::DeserializeOwned,
{
    let mut file = File::open(path.as_ref())
        .context(format!("Unable to open file: {}", path.as_ref().display()))?;
    from_reader(&mut file)
}

fn has_base_package(product: &ProductConfig) -> bool {
    return !(product.base.is_empty() && product.cache.is_empty() && product.system.is_empty());
}
