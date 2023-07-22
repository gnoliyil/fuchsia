// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{collections::HashMap, fs::File, io::Cursor};

use anyhow::{Context, Result};
use argh::FromArgs;
use camino::{Utf8Path, Utf8PathBuf};
use ext4_extract::ext4_extract;
use fuchsia_pkg::{PackageBuilder, PackageManifest};
use fuchsia_url::RelativePackageUrl;

mod depfile;
mod hal_manifest;
mod remote_bundle;

use crate::{depfile::Depfile, remote_bundle::Writer};

/// Construct a starnix container that can include an Android system and HALs.
#[derive(FromArgs)]
struct Command {
    /// name of the starnix container.
    #[argh(option)]
    name: String,

    /// directory to place outputs into.
    #[argh(option)]
    outdir: Utf8PathBuf,

    /// path to package archive containing additional resources to include.
    #[argh(option)]
    base: Utf8PathBuf,

    /// path to an Android system image.
    #[argh(option)]
    system: Utf8PathBuf,

    /// path to an Android vendor partition image.
    #[argh(option)]
    vendor: Option<Utf8PathBuf>,

    /// path to hal package archive.
    #[argh(option)]
    hal: Vec<Utf8PathBuf>,

    /// path to a depfile to write.
    #[argh(option)]
    depfile: Option<Utf8PathBuf>,

    /// whether the action always populates the ODM filesystem with
    /// configurations for HALs, or skipping depending on the system image.
    /// TODO(fxbug.dev/129576): Remove after soft-migration.
    #[argh(switch)]
    always_populate_odm: bool,
}

fn main() -> Result<()> {
    let cmd: Command = argh::from_env();
    generate(cmd)
}

fn add_ext4_image(
    name: impl AsRef<Utf8Path>,
    outdir: impl AsRef<Utf8Path>,
    image_path: impl AsRef<Utf8Path>,
    builder: &mut PackageBuilder,
) -> Result<HashMap<String, String>> {
    // Put all the system image files into the container.
    let name = name.as_ref();
    let outdir = outdir.as_ref();
    let image_path = image_path.as_ref();

    let image_outdir = outdir.join(name);
    std::fs::create_dir_all(&image_outdir)
        .with_context(|| format!("Preparing directory for image files: {}", &image_outdir))?;
    let image_files = ext4_extract(image_path.as_str(), image_outdir.as_str())
        .context("Extracting system files")?;
    for (dst, src) in &image_files {
        let dst = format!("data/{}/{}", name, dst);
        builder
            .add_file_as_blob(dst, &src)
            .with_context(|| format!("Adding blob from file: {}", &src))?;
    }

    Ok(image_files)
}

fn add_to_odm(
    src: &fuchsia_pkg::BlobInfo,
    dst: &[&str],
    odm_writer: &mut Writer,
) -> Result<String> {
    let src = &src.source_path;
    File::open(src)
        .and_then(|mut file| odm_writer.add_file(dst, &mut file))
        .with_context(|| format!("Adding {src} in HAL package to {dst:?}"))?;
    Ok(src.clone())
}

fn clone_package(
    manifest_path: &Utf8PathBuf,
    outdir: &String,
    deps: &mut Depfile,
) -> Result<PackageBuilder> {
    let manifest = PackageManifest::try_load_from(manifest_path)
        .with_context(|| format!("Reading base starnix package: {}", manifest_path))?;

    // Our tool will eventually read everything in the base package.
    deps.track_inputs(manifest.blobs().iter().map(|b| b.source_path.clone()));

    // [`PackageBuilder::from_manifest`] will unpack the contents of the `meta.far` into `outdir`.
    // Track those outputs too.
    if let Some(blob) =
        manifest.blobs().iter().find(|b| b.path == PackageManifest::META_FAR_BLOB_PATH)
    {
        let bytes = std::fs::read(&blob.source_path)
            .with_context(|| format!("reading {}", blob.source_path))?;
        let meta_far =
            fuchsia_archive::Utf8Reader::new(Cursor::new(bytes)).context("reading FAR")?;
        deps.track_outputs(meta_far.list().map(|e| format!("{}/{}", outdir, e.path())));
    }

    let builder =
        PackageBuilder::from_manifest(manifest, outdir).context("Parsing base starnix package")?;

    Ok(builder)
}

/// TODO(fxbug.dev/129576): Delete after soft migrating the rc/xml files.
struct SoftMigrate129576 {
    should_copy: bool,
}

impl SoftMigrate129576 {
    fn new(system: &HashMap<String, String>) -> Result<SoftMigrate129576> {
        // Conditionally control whether to copy, by checking for the number of HALs
        // in `system/vendor/etc/vintf/manifest.xml`. If there are less than 11 HALs,
        // which is a signal that migration has begun, activate copying.
        // See https://bugs.fuchsia.dev/p/fuchsia/issues/detail?id=129576#c26 for
        // context on this constant.
        use ext4_metadata::{Metadata, ROOT_INODE_NUM};
        let metadata = std::fs::read(&system["metadata.v1"]).context("Reading metadata")?;
        let m = Metadata::deserialize(&metadata)?;
        let Ok(manifest) = m.lookup(ROOT_INODE_NUM, "system").and_then(
            |n| m.lookup(n, "vendor")
        ).and_then(
            |n| m.lookup(n, "etc")
        ).and_then(
            |n| m.lookup(n, "vintf")
        ).and_then(
            |n| m.lookup(n, "manifest.xml")
        ) else {
            // If failed, assume the file does not exist.
            return Ok(SoftMigrate129576 { should_copy: true });
        };
        let manifest =
            std::fs::read(&system[&format!("{manifest}")]).context("Reading manifest")?;
        Ok(SoftMigrate129576 {
            should_copy: String::from_utf8_lossy(&manifest).matches("<hal ").count() < 11,
        })
    }

    fn should_copy(&self) -> bool {
        self.should_copy
    }
}

fn generate(cmd: Command) -> Result<()> {
    // Track inputs and outputs for producing a depfile for incremental build correctness.
    let mut deps = Depfile::new();

    // Bootstrap the package builder with the contents of the base package, but update the
    // internal and published names.
    let mut builder = clone_package(&cmd.base, &cmd.outdir.to_string(), &mut deps)?;
    builder.name(&cmd.name);
    builder.published_name(&cmd.name);

    let system_files = add_ext4_image("system", &cmd.outdir, &cmd.system, &mut builder)?;
    let soft_migrate = SoftMigrate129576::new(&system_files)?;
    deps.track_input(cmd.system.to_string());
    deps.track_outputs(system_files.into_values());

    // Combine the vendor image with the system image.
    if let Some(vendor_path) = cmd.vendor {
        let vendor_files = add_ext4_image("vendor", &cmd.outdir, &vendor_path, &mut builder)?;
        deps.track_input(vendor_path.to_string());
        deps.track_outputs(vendor_files.into_values());
    }

    // Initialize ODM filesystem.
    let odm_outdir = cmd.outdir.join("odm");
    std::fs::create_dir_all(&odm_outdir)
        .with_context(|| format!("Preparing directory for ODM files: {}", &odm_outdir))?;
    let mut odm_writer = Writer::new(&odm_outdir)?;
    odm_writer.add_directory(&["etc"]);
    odm_writer.add_directory(&["etc", "init"]);
    odm_writer.add_directory(&["etc", "vintf"]);
    odm_writer.add_directory(&["etc", "vintf", "manifest"]);

    // Add all the HALs as subpackages.
    for hal in &cmd.hal {
        let manifest = PackageManifest::try_load_from(&hal)
            .with_context(|| format!("Reading hal package manifest: {}", hal))?;
        let name: RelativePackageUrl = manifest.name().to_owned().into();
        builder
            .add_subpackage(&name, manifest.hash(), hal.into())
            .with_context(|| format!("Adding subpackage from manifest: {}", &hal))?;

        let hal_package_name = manifest.name().to_string();
        let (hal_manifest, hal_manifest_source_path) =
            hal_manifest::load_from_package(&manifest)
                .with_context(|| format!("Reading hal manifest from package: {}", hal))?;
        deps.track_inputs(hal_manifest_source_path);
        // If a HAL manifest contains `init_rc`, copy that file to
        // `etc/init/{hal_package_name}.rc` in the ODM filesystem.
        if let Some(blob) = hal_manifest.init_rc {
            if soft_migrate.should_copy() || cmd.always_populate_odm {
                deps.track_input(add_to_odm(
                    &blob,
                    &["etc", "init", &format!("{hal_package_name}.rc")],
                    &mut odm_writer,
                )?);
            }
        }
        // If a HAL manifest contains `vintf_manifest`, copy that file to
        // `etc/vintf/manifest/{hal_package_name}.xml` in the ODM filesystem.
        if let Some(blob) = hal_manifest.vintf_manifest {
            if soft_migrate.should_copy() || cmd.always_populate_odm {
                deps.track_input(add_to_odm(
                    &blob,
                    &["etc", "vintf", "manifest", &format!("{hal_package_name}.xml")],
                    &mut odm_writer,
                )?);
            }
        }
    }

    // Put all the ODM files into the container.
    let odm_files = odm_writer.inner.export().context("Exporting ODM files")?;
    for (dst, src) in &odm_files {
        let dst = format!("data/odm/{}", dst);
        builder
            .add_file_as_blob(dst, &src)
            .with_context(|| format!("Adding blob from file: {}", &src))?;
        deps.track_output(src.clone());
    }

    // Build the starnix container.
    let metafar_path = cmd.outdir.join("meta.far");
    let manifest_path = cmd.outdir.join("package_manifest.json");
    builder.manifest_path(manifest_path);
    builder.build(&cmd.outdir, &metafar_path).context("Building starnix container")?;
    deps.track_outputs(
        vec![
            cmd.outdir.join("meta.far"),
            cmd.outdir.join("meta/fuchsia.abi/abi-revision"),
            cmd.outdir.join("meta/fuchsia.pkg/subpackages"),
            cmd.outdir.join("meta/package"),
        ]
        .iter()
        .map(|p| p.to_string()),
    );

    if let Some(depfile) = cmd.depfile {
        let deps: String = deps.into();
        std::fs::write(&depfile, &deps).with_context(|| format!("Writing depfile: {}", depfile))?;
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use camino::Utf8Path;
    use ext4_metadata::{Metadata, NodeInfo, ROOT_INODE_NUM};
    use std::str::FromStr;
    use tempfile::TempDir;

    const EXT4_IMAGE_PATH: &str = "host_x64/test_data/gen-android-starnix-container/test.img";

    fn fake_base(outdir: &Utf8Path) -> Utf8PathBuf {
        // Build a fake "base".
        let base_manifest_path = outdir.join("base_package_manifest.json");
        let mut builder = PackageBuilder::new("test-base");
        builder.add_contents_as_blob("data/test", "test-base-blob", &outdir).unwrap();
        builder.manifest_path(&base_manifest_path);
        let _ = builder.build(&outdir, outdir.join("base-meta.far")).unwrap();
        base_manifest_path
    }

    #[test]
    fn test_generate() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let base_manifest_path = fake_base(outdir);

        // Build a fake HAL.
        let hal_manifest_path = outdir.join("hal_package_manifest.json");
        let mut builder = PackageBuilder::new("test-hal");
        builder.add_contents_as_blob("data/hal", "test-hal-blob", &outdir).unwrap();
        builder.manifest_path(&hal_manifest_path);
        let _ = builder.build(&outdir, outdir.join("hal-meta.far")).unwrap();

        // Run the generator.
        let cmd = Command {
            name: "test-name".into(),
            outdir: outdir.to_owned(),
            base: base_manifest_path,
            system: Utf8PathBuf::from_str(EXT4_IMAGE_PATH).unwrap(),
            vendor: Some(Utf8PathBuf::from_str(EXT4_IMAGE_PATH).unwrap()),
            hal: vec![hal_manifest_path],
            depfile: None,
            always_populate_odm: false,
        };
        generate(cmd).unwrap();

        // Read the package manifest, and ensure the correct files are present as blobs, and the
        // HALs are listed as subpackages.
        let manifest_path = outdir.join("package_manifest.json");
        let manifest = PackageManifest::try_load_from(manifest_path).unwrap();
        assert_eq!(manifest.name().as_ref(), "test-name");
        let (blobs, subpackages) = manifest.into_blobs_and_subpackages();
        assert_eq!(blobs.len(), 7);
        assert_eq!(subpackages.len(), 1);
        let blob_filenames: Vec<String> = blobs.into_iter().map(|b| b.path).collect();
        let subpackage_names: Vec<String> = subpackages.into_iter().map(|s| s.name).collect();
        assert_eq!(
            blob_filenames,
            vec![
                "meta/".to_string(),
                "data/odm/metadata.v1".to_string(),
                "data/system/13".to_string(),
                "data/system/metadata.v1".to_string(),
                "data/test".to_string(),
                "data/vendor/13".to_string(),
                "data/vendor/metadata.v1".to_string(),
            ]
        );
        assert_eq!(subpackage_names, vec!["test-hal".to_string(),]);
    }

    #[test]
    fn test_hal_init_rc() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let base_manifest_path = fake_base(outdir);

        // Build a fake HAL with an init.rc file.
        let hal_manifest_path = outdir.join("hal_package_manifest.json");
        let mut builder = PackageBuilder::new("test-hal");
        builder.add_contents_as_blob("data/hal", "test-hal-blob", &outdir).unwrap();
        builder.add_contents_as_blob("system/init.rc", "service foo bar", &outdir).unwrap();
        builder
            .add_contents_as_blob(
                "__android_config__/manifest.json",
                r#"{ "init_rc": "system/init.rc" }"#,
                &outdir,
            )
            .unwrap();
        builder.manifest_path(&hal_manifest_path);
        let _ = builder.build(&outdir, outdir.join("hal-meta.far")).unwrap();

        // Run the generator.
        let cmd = Command {
            name: "test-name".into(),
            outdir: outdir.to_owned(),
            base: base_manifest_path,
            system: Utf8PathBuf::from_str(EXT4_IMAGE_PATH).unwrap(),
            hal: vec![hal_manifest_path],
            depfile: None,
            vendor: None,
            always_populate_odm: false,
        };
        generate(cmd).unwrap();

        // Read the package manifest, and ensure the correct files are present as blobs, and
        // there is an additional `.rc` file corresponding to `test-hal`.
        let manifest_path = outdir.join("package_manifest.json");
        let manifest = PackageManifest::try_load_from(manifest_path).unwrap();
        assert_eq!(manifest.name().as_ref(), "test-name");
        let (blobs, _subpackages) = manifest.into_blobs_and_subpackages();
        assert_eq!(blobs.len(), 6);
        let blob_filenames: Vec<String> = blobs.iter().map(|b| b.path.clone()).collect();
        assert_eq!(
            blob_filenames,
            vec![
                "meta/".to_string(),
                "data/odm/7".to_string(),
                "data/odm/metadata.v1".to_string(),
                "data/system/13".to_string(),
                "data/system/metadata.v1".to_string(),
                "data/test".to_string(),
            ]
        );

        // Find the rc file and check its properties.
        let odm_metadata_path =
            &blobs.iter().find(|b| b.path == "data/odm/metadata.v1").unwrap().source_path;
        let m = Metadata::deserialize(
            &std::fs::read(odm_metadata_path).expect("Failed to read metadata"),
        )
        .expect("Failed to deserialize metadata");
        let etc = m.lookup(ROOT_INODE_NUM, "etc").expect("etc not found");
        let init = m.lookup(etc, "init").expect("init not found");
        let rc = m.lookup(init, "test-hal.rc").expect("rc not found");
        let rc = m.get(rc).expect("rc not found");
        assert_matches!(rc.info(), NodeInfo::File(_));
        assert_eq!(rc.mode, 0o100444);
        assert_eq!(rc.uid, 0);
        assert_eq!(rc.gid, 0);
    }

    #[test]
    fn test_hal_vintf_manifest() {
        let tmp = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmp.path()).unwrap();
        let base_manifest_path = fake_base(outdir);

        // Build a fake HAL with an init.rc file.
        let hal_manifest_path = outdir.join("hal_package_manifest.json");
        let mut builder = PackageBuilder::new("test-hal");
        builder.add_contents_as_blob("data/hal", "test-hal-blob", &outdir).unwrap();
        builder
            .add_contents_as_blob("system/manifest.xml", "<manifest></manifest>", &outdir)
            .unwrap();
        builder
            .add_contents_as_blob(
                "__android_config__/manifest.json",
                r#"{ "vintf_manifest": "system/manifest.xml" }"#,
                &outdir,
            )
            .unwrap();
        builder.manifest_path(&hal_manifest_path);
        let _ = builder.build(&outdir, outdir.join("hal-meta.far")).unwrap();

        // Run the generator.
        let cmd = Command {
            name: "test-name".into(),
            outdir: outdir.to_owned(),
            base: base_manifest_path,
            system: Utf8PathBuf::from_str(EXT4_IMAGE_PATH).unwrap(),
            hal: vec![hal_manifest_path],
            depfile: None,
            vendor: None,
            always_populate_odm: false,
        };
        generate(cmd).unwrap();

        // Read the package manifest, and ensure the correct files are present as blobs, and
        // there is an additional `.xml` file corresponding to `test-hal`.
        let manifest_path = outdir.join("package_manifest.json");
        let manifest = PackageManifest::try_load_from(manifest_path).unwrap();
        assert_eq!(manifest.name().as_ref(), "test-name");
        let (blobs, _subpackages) = manifest.into_blobs_and_subpackages();
        assert_eq!(blobs.len(), 6);
        let blob_filenames: Vec<String> = blobs.iter().map(|b| b.path.clone()).collect();
        assert_eq!(
            blob_filenames,
            vec![
                "meta/".to_string(),
                "data/odm/7".to_string(),
                "data/odm/metadata.v1".to_string(),
                "data/system/13".to_string(),
                "data/system/metadata.v1".to_string(),
                "data/test".to_string(),
            ]
        );

        // Find the xml file and check its properties.
        let odm_metadata_path =
            &blobs.iter().find(|b| b.path == "data/odm/metadata.v1").unwrap().source_path;
        let m = Metadata::deserialize(
            &std::fs::read(odm_metadata_path).expect("Failed to read metadata"),
        )
        .expect("Failed to deserialize metadata");
        let etc = m.lookup(ROOT_INODE_NUM, "etc").expect("etc not found");
        let vintf = m.lookup(etc, "vintf").expect("vintf not found");
        let manifest = m.lookup(vintf, "manifest").expect("manifest not found");
        let xml = m.lookup(manifest, "test-hal.xml").expect("xml not found");
        let xml = m.get(xml).expect("xml not found");
        assert_matches!(xml.info(), NodeInfo::File(_));
        assert_eq!(xml.mode, 0o100444);
        assert_eq!(xml.uid, 0);
        assert_eq!(xml.gid, 0);
    }
}
