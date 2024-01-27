// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use camino::Utf8PathBuf;
use fidl_fuchsia_developer_ffx as fidl;
use serde::{Deserialize, Serialize};
use std::convert::{TryFrom, TryInto};
use thiserror::Error;

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum RepositorySpec {
    FileSystem { metadata_repo_path: Utf8PathBuf, blob_repo_path: Utf8PathBuf },

    Pm { path: Utf8PathBuf },

    Http { metadata_repo_url: String, blob_repo_url: String },

    Gcs { metadata_repo_url: String, blob_repo_url: String },
}

impl TryFrom<fidl::RepositorySpec> for RepositorySpec {
    type Error = RepositoryError;

    fn try_from(repo: fidl::RepositorySpec) -> Result<Self, RepositoryError> {
        match repo {
            fidl::RepositorySpec::FileSystem(filesystem_spec) => {
                let metadata_repo_path = filesystem_spec
                    .metadata_repo_path
                    .ok_or(RepositoryError::MissingRepositorySpecField)?;
                let blob_repo_path = filesystem_spec
                    .blob_repo_path
                    .ok_or(RepositoryError::MissingRepositorySpecField)?;
                Ok(RepositorySpec::FileSystem {
                    metadata_repo_path: metadata_repo_path.into(),
                    blob_repo_path: blob_repo_path.into(),
                })
            }
            fidl::RepositorySpec::Pm(pm_spec) => {
                let path = pm_spec.path.ok_or(RepositoryError::MissingRepositorySpecField)?;
                Ok(RepositorySpec::Pm { path: path.into() })
            }
            fidl::RepositorySpec::Http(http_spec) => {
                let metadata_repo_url = http_spec
                    .metadata_repo_url
                    .ok_or(RepositoryError::MissingRepositorySpecField)?;
                let blob_repo_url =
                    http_spec.blob_repo_url.ok_or(RepositoryError::MissingRepositorySpecField)?;
                Ok(RepositorySpec::Http { metadata_repo_url, blob_repo_url })
            }
            fidl::RepositorySpec::Gcs(gcs_spec) => {
                let metadata_repo_url = gcs_spec
                    .metadata_repo_url
                    .ok_or(RepositoryError::MissingRepositorySpecField)?;
                let blob_repo_url =
                    gcs_spec.blob_repo_url.ok_or(RepositoryError::MissingRepositorySpecField)?;
                Ok(RepositorySpec::Gcs { metadata_repo_url, blob_repo_url })
            }
            fidl::RepositorySpecUnknown!() => Err(RepositoryError::UnknownRepositorySpec),
        }
    }
}

impl From<RepositorySpec> for fidl::RepositorySpec {
    fn from(repo: RepositorySpec) -> Self {
        match repo {
            RepositorySpec::FileSystem { metadata_repo_path, blob_repo_path } => {
                let metadata_repo_path = metadata_repo_path.into_string();
                let blob_repo_path = blob_repo_path.into_string();
                fidl::RepositorySpec::FileSystem(fidl::FileSystemRepositorySpec {
                    metadata_repo_path: Some(metadata_repo_path),
                    blob_repo_path: Some(blob_repo_path),
                    ..fidl::FileSystemRepositorySpec::EMPTY
                })
            }
            RepositorySpec::Pm { path } => {
                let path = path.into_string();
                fidl::RepositorySpec::Pm(fidl::PmRepositorySpec {
                    path: Some(path),
                    ..fidl::PmRepositorySpec::EMPTY
                })
            }
            RepositorySpec::Http { metadata_repo_url, blob_repo_url } => {
                fidl::RepositorySpec::Http(fidl::HttpRepositorySpec {
                    metadata_repo_url: Some(metadata_repo_url),
                    blob_repo_url: Some(blob_repo_url),
                    ..fidl::HttpRepositorySpec::EMPTY
                })
            }
            RepositorySpec::Gcs { metadata_repo_url, blob_repo_url } => {
                fidl::RepositorySpec::Gcs(fidl::GcsRepositorySpec {
                    metadata_repo_url: Some(metadata_repo_url),
                    blob_repo_url: Some(blob_repo_url),
                    ..fidl::GcsRepositorySpec::EMPTY
                })
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RepositoryStorageType {
    Ephemeral,
    Persistent,
}

impl From<fidl::RepositoryStorageType> for RepositoryStorageType {
    fn from(storage_type: fidl::RepositoryStorageType) -> Self {
        match storage_type {
            fidl::RepositoryStorageType::Ephemeral => RepositoryStorageType::Ephemeral,
            fidl::RepositoryStorageType::Persistent => RepositoryStorageType::Persistent,
        }
    }
}

impl From<RepositoryStorageType> for fidl::RepositoryStorageType {
    fn from(storage_type: RepositoryStorageType) -> Self {
        match storage_type {
            RepositoryStorageType::Ephemeral => fidl::RepositoryStorageType::Ephemeral,
            RepositoryStorageType::Persistent => fidl::RepositoryStorageType::Persistent,
        }
    }
}

impl From<RepositoryStorageType> for fidl_fuchsia_pkg_ext::RepositoryStorageType {
    fn from(storage_type: RepositoryStorageType) -> Self {
        match storage_type {
            RepositoryStorageType::Ephemeral => {
                fidl_fuchsia_pkg_ext::RepositoryStorageType::Ephemeral
            }
            RepositoryStorageType::Persistent => {
                fidl_fuchsia_pkg_ext::RepositoryStorageType::Persistent
            }
        }
    }
}

/// The below types exist to provide definitions with Serialize.
/// TODO(fxbug.dev/76041) They should be removed in favor of the
/// corresponding fidl-fuchsia-pkg-ext types.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub struct RepositoryConfig {
    pub name: String,
    pub spec: RepositorySpec,
}

impl TryFrom<fidl::RepositoryConfig> for RepositoryConfig {
    type Error = RepositoryError;

    fn try_from(repo_config: fidl::RepositoryConfig) -> Result<Self, Self::Error> {
        Ok(RepositoryConfig { name: repo_config.name, spec: repo_config.spec.try_into()? })
    }
}

impl From<RepositoryConfig> for fidl::RepositoryConfig {
    fn from(repo_config: RepositoryConfig) -> Self {
        fidl::RepositoryConfig { name: repo_config.name, spec: repo_config.spec.into() }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct RepositoryTarget {
    pub repo_name: String,
    pub target_identifier: Option<String>,
    pub aliases: Vec<String>,
    pub storage_type: Option<RepositoryStorageType>,
}

impl TryFrom<fidl::RepositoryTarget> for RepositoryTarget {
    type Error = RepositoryError;

    fn try_from(repo_target: fidl::RepositoryTarget) -> Result<Self, Self::Error> {
        Ok(RepositoryTarget {
            repo_name: repo_target.repo_name.ok_or(RepositoryError::MissingRepositoryName)?,
            target_identifier: repo_target.target_identifier,
            aliases: repo_target.aliases.unwrap_or_default(),
            storage_type: repo_target.storage_type.map(|storage_type| storage_type.into()),
        })
    }
}

impl From<RepositoryTarget> for fidl::RepositoryTarget {
    fn from(repo_target: RepositoryTarget) -> Self {
        fidl::RepositoryTarget {
            repo_name: Some(repo_target.repo_name),
            target_identifier: repo_target.target_identifier,
            aliases: Some(repo_target.aliases),
            storage_type: repo_target.storage_type.map(|storage_type| storage_type.into()),
            ..fidl::RepositoryTarget::EMPTY
        }
    }
}

#[derive(Debug, Error)]
pub enum RepositoryError {
    #[error("the repository name is missing")]
    MissingRepositoryName,

    #[error("repository does not exist")]
    NoMatchingRepository,

    #[error("error communicating with target device")]
    TargetCommunicationFailure,

    #[error("error interacting with the target's RepositoryManager")]
    RepositoryManagerError,

    #[error("error iteracting with the target's RewriteEngine")]
    RewriteEngineError,

    #[error("unknown repository spec type")]
    UnknownRepositorySpec,

    #[error("repository spec is missing a required field")]
    MissingRepositorySpecField,

    #[error("some unspecified error during I/O")]
    IoError,

    #[error("some unspecified internal error")]
    InternalError,

    #[error("repository metadata is expired")]
    ExpiredRepositoryMetadata,

    #[error("repository registration does not exist")]
    NoMatchingRegistration,

    #[error("repository server is not running")]
    ServerNotRunning,

    #[error("invalid url")]
    InvalidUrl,

    #[error("repository server address already in use")]
    ServerAddressAlreadyInUse,
}

impl From<fidl::RepositoryError> for RepositoryError {
    fn from(err: fidl::RepositoryError) -> Self {
        match err {
            fidl::RepositoryError::MissingRepositoryName => RepositoryError::MissingRepositoryName,
            fidl::RepositoryError::NoMatchingRepository => RepositoryError::NoMatchingRepository,
            fidl::RepositoryError::TargetCommunicationFailure => {
                RepositoryError::TargetCommunicationFailure
            }
            fidl::RepositoryError::RepositoryManagerError => {
                RepositoryError::RepositoryManagerError
            }
            fidl::RepositoryError::RewriteEngineError => RepositoryError::RewriteEngineError,
            fidl::RepositoryError::UnknownRepositorySpec => RepositoryError::UnknownRepositorySpec,
            fidl::RepositoryError::MissingRepositorySpecField => {
                RepositoryError::MissingRepositorySpecField
            }
            fidl::RepositoryError::IoError => RepositoryError::IoError,
            fidl::RepositoryError::InternalError => RepositoryError::InternalError,
            fidl::RepositoryError::ExpiredRepositoryMetadata => {
                RepositoryError::ExpiredRepositoryMetadata
            }
            fidl::RepositoryError::NoMatchingRegistration => {
                RepositoryError::NoMatchingRegistration
            }
            fidl::RepositoryError::ServerNotRunning => RepositoryError::ServerNotRunning,
            fidl::RepositoryError::InvalidUrl => RepositoryError::InvalidUrl,
            fidl::RepositoryError::ServerAddressAlreadyInUse => {
                RepositoryError::ServerAddressAlreadyInUse
            }
        }
    }
}

impl From<RepositoryError> for fidl::RepositoryError {
    fn from(err: RepositoryError) -> Self {
        match err {
            RepositoryError::MissingRepositoryName => fidl::RepositoryError::MissingRepositoryName,
            RepositoryError::NoMatchingRepository => fidl::RepositoryError::NoMatchingRepository,
            RepositoryError::TargetCommunicationFailure => {
                fidl::RepositoryError::TargetCommunicationFailure
            }
            RepositoryError::RepositoryManagerError => {
                fidl::RepositoryError::RepositoryManagerError
            }
            RepositoryError::RewriteEngineError => fidl::RepositoryError::RewriteEngineError,
            RepositoryError::UnknownRepositorySpec => fidl::RepositoryError::UnknownRepositorySpec,
            RepositoryError::MissingRepositorySpecField => {
                fidl::RepositoryError::MissingRepositorySpecField
            }
            RepositoryError::IoError => fidl::RepositoryError::IoError,
            RepositoryError::InternalError => fidl::RepositoryError::InternalError,
            RepositoryError::ExpiredRepositoryMetadata => {
                fidl::RepositoryError::ExpiredRepositoryMetadata
            }
            RepositoryError::NoMatchingRegistration => {
                fidl::RepositoryError::NoMatchingRegistration
            }
            RepositoryError::ServerNotRunning => fidl::RepositoryError::ServerNotRunning,
            RepositoryError::InvalidUrl => fidl::RepositoryError::InvalidUrl,
            RepositoryError::ServerAddressAlreadyInUse => {
                fidl::RepositoryError::ServerAddressAlreadyInUse
            }
        }
    }
}

/// Serializable version of  fidl_fuchsia_developer_ffx::PackageEntry
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub struct PackageEntry {
    /// Blob name
    pub path: Option<String>,
    /// Blob's merkle hash
    pub hash: Option<String>,
    /// Size in bytes of this blob.
    pub size: Option<u64>,
    /// Last modification timestamp (seconds since UNIX epoch). May be null
    /// depending on repository source.
    pub modified: Option<u64>,
}

impl From<&fidl::PackageEntry> for PackageEntry {
    fn from(entry: &fidl::PackageEntry) -> Self {
        PackageEntry {
            path: entry.path.clone(),
            hash: entry.hash.clone(),
            size: entry.size,
            modified: entry.modified,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub struct RepositoryPackage {
    /// Package name
    pub name: Option<String>,
    /// Package's merkle hash
    pub hash: Option<String>,
    /// Size in bytes of all blobs in this package.
    pub size: Option<u64>,
    /// Last modification timestamp (seconds since UNIX epoch). May be null depending on repository
    /// source.
    pub modified: Option<u64>,
    /// List of blobs in this package: currently, this is only components.
    pub entries: Vec<PackageEntry>,
}

impl From<fidl::RepositoryPackage> for RepositoryPackage {
    fn from(repo_package: fidl::RepositoryPackage) -> Self {
        RepositoryPackage {
            name: repo_package.name,
            hash: repo_package.hash,
            size: repo_package.size,
            modified: repo_package.modified,
            entries: match repo_package.entries {
                Some(list) => list.iter().map(|p| p.into()).collect(),
                None => vec![],
            },
        }
    }
}
