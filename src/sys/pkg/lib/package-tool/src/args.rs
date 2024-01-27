// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    argh::FromArgs,
    camino::Utf8PathBuf,
    chrono::{DateTime, Utc},
    fuchsia_repo::repository::CopyMode,
    std::path::PathBuf,
};

#[derive(Eq, FromArgs, PartialEq, Debug)]
/// create a package archive from a package_manifest.json
#[argh(subcommand, name = "create")]
pub struct PackageArchiveCreateCommand {
    /// output package archive
    #[argh(option, short = 'o')]
    pub out: PathBuf,

    /// root directory for paths in package_manifest.json
    #[argh(option, short = 'r', default = "Utf8PathBuf::from(\".\")")]
    pub root_dir: Utf8PathBuf,

    /// produce a depfile file at the provided path
    #[argh(option)]
    pub depfile: Option<Utf8PathBuf>,

    /// package_manifest.json to archive
    #[argh(positional)]
    pub package_manifest: Utf8PathBuf,
}

#[derive(Eq, FromArgs, PartialEq, Debug)]
/// extract the contents of <far_path> inside the Fuchsia package archive file to the output directory
#[argh(subcommand, name = "extract")]
pub struct PackageArchiveExtractCommand {
    /// output directory for writing the extracted files. Defaults to the current directory.
    #[argh(option, short = 'o', default = "Utf8PathBuf::from(\"./\")")]
    pub out: Utf8PathBuf,

    /// repository of the package
    #[argh(option)]
    pub repository: Option<String>,

    /// produce a meta.far.merkle file
    #[argh(switch)]
    pub meta_far_merkle: bool,

    /// produce a blobs.json file
    #[argh(switch)]
    pub blobs_json: bool,

    /// package archive
    #[argh(positional)]
    pub archive: PathBuf,
}

/// Builds a package.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "build")]
pub struct PackageBuildCommand {
    /// directory to save package artifacts
    #[argh(option, short = 'o', default = "Utf8PathBuf::from(\"./out\")")]
    pub out: Utf8PathBuf,

    /// package API level
    #[argh(option)]
    pub api_level: Option<u64>,

    /// package ABI revision
    #[argh(option)]
    pub abi_revision: Option<u64>,

    /// name of the package
    #[argh(option)]
    pub published_name: Option<String>,

    /// repository of the package
    #[argh(option)]
    pub repository: Option<String>,

    /// produce a depfile file
    #[argh(switch)]
    pub depfile: bool,

    /// produce a meta.far.merkle file
    #[argh(switch)]
    pub meta_far_merkle: bool,

    /// produce a blobs.json file
    #[argh(switch)]
    pub blobs_json: bool,

    /// produce a blobs.manifest file
    #[argh(switch)]
    pub blobs_manifest: bool,

    /// path to the subpackages build manifest file
    #[argh(option)]
    pub subpackages_build_manifest_path: Option<Utf8PathBuf>,

    /// path to the package build manifest file
    #[argh(positional)]
    pub package_build_manifest_path: Utf8PathBuf,
}

/// Create a repository.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "create")]
pub struct RepoCreateCommand {
    /// set repository version based on the current time rather than monotonically increasing version
    #[argh(switch)]
    pub time_versioning: bool,

    /// path to the repository keys directory. Default to generate keys at 'repo_path'/keys.
    #[argh(option)]
    pub keys: Option<PathBuf>,

    /// path to the repository directory
    #[argh(positional)]
    pub repo_path: Utf8PathBuf,
}

/// Publish packages.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "publish")]
pub struct RepoPublishCommand {
    /// path to the keys used to sign metadata, but not trust for key rotation
    #[argh(option)]
    pub signing_keys: Option<Utf8PathBuf>,

    /// path to the keys used to sign and trust metadata (default repository `keys/` directory)
    #[argh(option)]
    pub trusted_keys: Option<Utf8PathBuf>,

    /// path to the initial trusted root metadata (default is to use 1.root.json from the repository)
    #[argh(option)]
    pub trusted_root: Option<Utf8PathBuf>,

    /// path to a package manifest
    #[argh(option, long = "package")]
    pub package_manifests: Vec<Utf8PathBuf>,

    /// path to a packages list manifest
    #[argh(option, long = "package-list")]
    pub package_list_manifests: Vec<Utf8PathBuf>,

    /// path to a package archive
    #[argh(option, long = "package-archive")]
    pub package_archives: Vec<Utf8PathBuf>,

    /// set repository version based on time rather than monotonically increasing version
    #[argh(switch)]
    pub time_versioning: bool,

    /// the RFC 3339 time used to see if metadata has expired, and when new metadata should expire (default uses the current time)
    #[argh(option, default = "Utc::now()", from_str_fn(parse_datetime))]
    pub metadata_current_time: DateTime<Utc>,

    /// generate a new root metadata along side all the other metadata
    #[argh(switch)]
    pub refresh_root: bool,

    /// clean the repository so only new publications remain
    #[argh(switch)]
    pub clean: bool,

    /// produce a depfile file
    #[argh(option)]
    pub depfile: Option<Utf8PathBuf>,

    /// mode used to copy blobs to repository. Either 'copy', 'copy-overwrite', or 'hard-link' (default 'copy').
    #[argh(option, default = "CopyMode::Copy", from_str_fn(parse_copy_mode))]
    pub copy_mode: CopyMode,

    /// the type of delivery blob to generate (default no delivery blobs are generated)
    #[argh(option)]
    pub delivery_blob_type: Option<u32>,

    /// republish packages on file change
    #[argh(switch)]
    pub watch: bool,

    /// ignore if package paths do not exist
    #[argh(switch)]
    pub ignore_missing_packages: bool,

    /// path to write the blob manifest to
    #[argh(option)]
    pub blob_manifest: Option<Utf8PathBuf>,

    /// path to the blobs directory (default '$repo_path/repository/blobs')
    #[argh(option)]
    pub blob_repo_dir: Option<Utf8PathBuf>,

    /// path to the repository directory
    #[argh(positional)]
    pub repo_path: Utf8PathBuf,
}

/// Create package manifest list from repository.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "create-package-manifest-list")]
pub struct RepoPMListCommand {
    /// path to the source repository directory
    #[argh(positional)]
    pub src_repo_path: Utf8PathBuf,

    /// path to the manifest directory
    #[argh(positional)]
    pub manifest_dir: Utf8PathBuf,

    /// path to the initial trusted root metadata (Default is to use 1.root.json from the source repository)
    #[argh(option)]
    pub src_trusted_root_path: Option<Utf8PathBuf>,
}

fn parse_copy_mode(value: &str) -> Result<CopyMode, String> {
    match value {
        "copy" => Ok(CopyMode::Copy),
        "copy-overwrite" => Ok(CopyMode::CopyOverwrite),
        "hard-link" => Ok(CopyMode::HardLink),
        _ => Err(format!("unknown copy mode {value}")),
    }
}

fn parse_datetime(value: &str) -> Result<DateTime<Utc>, String> {
    DateTime::parse_from_rfc3339(value)
        .map(|ts| ts.with_timezone(&Utc))
        .map_err(|err| err.to_string())
}
