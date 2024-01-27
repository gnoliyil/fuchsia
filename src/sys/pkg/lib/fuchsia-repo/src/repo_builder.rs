// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{repo_client::RepoClient, repo_keys::RepoKeys, repository::RepoStorageProvider},
    anyhow::{anyhow, Context, Result},
    async_fs::File,
    camino::{Utf8Path, Utf8PathBuf},
    chrono::{DateTime, Duration, Utc},
    fuchsia_merkle::Hash,
    fuchsia_pkg::{BlobInfo, PackageManifest, PackageManifestList, PackagePath, SubpackageInfo},
    std::collections::{hash_map, BTreeMap, HashMap, HashSet, VecDeque},
    tempfile::TempDir,
    tuf::{
        crypto::HashAlgorithm, metadata::TargetPath, pouf::Pouf1,
        repo_builder::RepoBuilder as TufRepoBuilder, Database,
    },
};

/// Number of days from now before the root metadata is expired.
const DEFAULT_ROOT_EXPIRATION: i64 = 365;

/// Number of days from now before the targets metadata is expired.
const DEFAULT_TARGETS_EXPIRATION: i64 = 90;

/// Number of days from now before the snapshot metadata is expired.
const DEFAULT_SNAPSHOT_EXPIRATION: i64 = 30;

/// Number of days from now before the timestamp metadata is expired.
const DEFAULT_TIMESTAMP_EXPIRATION: i64 = 30;

pub struct ToBeStagedPackage {
    path: Option<Utf8PathBuf>,
    kind: ToBeStagedPackageKind,
}

enum ToBeStagedPackageKind {
    Manifest { manifest: PackageManifest },
    Archive { _archive_out: TempDir, manifest: PackageManifest },
}

impl ToBeStagedPackage {
    fn manifest(&self) -> &PackageManifest {
        match &self.kind {
            ToBeStagedPackageKind::Manifest { manifest } => manifest,
            ToBeStagedPackageKind::Archive { manifest, .. } => manifest,
        }
    }
}

/// RepoBuilder can create and manipulate package repositories.
pub struct RepoBuilder<'a, R: RepoStorageProvider> {
    signing_repo_keys: Option<&'a RepoKeys>,
    trusted_repo_keys: &'a RepoKeys,
    database: Option<&'a Database<Pouf1>>,
    repo: R,
    current_time: DateTime<Utc>,
    time_versioning: bool,
    refresh_metadata: bool,
    refresh_non_root_metadata: bool,
    inherit_from_trusted_targets: bool,
    named_packages: HashMap<PackagePath, ToBeStagedPackage>,
    additional_subpackages: HashMap<Hash, (Utf8PathBuf, PackageManifest)>,
    deps: HashSet<Utf8PathBuf>,
}

impl<'a, R> RepoBuilder<'a, &'a R>
where
    R: RepoStorageProvider,
{
    pub fn from_client(
        client: &'a RepoClient<R>,
        repo_keys: &'a RepoKeys,
    ) -> RepoBuilder<'a, &'a R> {
        Self::from_database(client.remote_repo(), repo_keys, client.database())
    }
}

impl<'a, R> RepoBuilder<'a, R>
where
    R: RepoStorageProvider,
{
    pub fn create(repo: R, repo_keys: &'a RepoKeys) -> RepoBuilder<'a, R> {
        Self::new(repo, repo_keys, None)
    }

    pub fn from_database(
        repo: R,
        repo_keys: &'a RepoKeys,
        database: &'a Database<Pouf1>,
    ) -> RepoBuilder<'a, R> {
        Self::new(repo, repo_keys, Some(database))
    }

    fn new(
        repo: R,
        trusted_repo_keys: &'a RepoKeys,
        database: Option<&'a Database<Pouf1>>,
    ) -> RepoBuilder<'a, R> {
        RepoBuilder {
            repo,
            signing_repo_keys: None,
            trusted_repo_keys,
            database,
            current_time: Utc::now(),
            time_versioning: false,
            refresh_metadata: false,
            refresh_non_root_metadata: false,
            inherit_from_trusted_targets: true,
            named_packages: HashMap::new(),
            additional_subpackages: HashMap::new(),
            deps: HashSet::new(),
        }
    }

    pub fn signing_repo_keys(mut self, signing_repo_keys: &'a RepoKeys) -> Self {
        self.signing_repo_keys = Some(signing_repo_keys);
        self
    }

    pub fn current_time(mut self, current_time: DateTime<Utc>) -> Self {
        self.current_time = current_time;
        self
    }

    pub fn time_versioning(mut self, time_versioning: bool) -> Self {
        self.time_versioning = time_versioning;
        self
    }

    /// Always generate new root, targets, snapshot, and timestamp metadata, even if unchanged and
    /// not expired.
    pub fn refresh_metadata(mut self, refresh_metadata: bool) -> Self {
        self.refresh_metadata = refresh_metadata;
        self
    }

    /// Generate a new targets, snapshot, and timestamp metadata, even if unchanged and not expired.
    pub fn refresh_non_root_metadata(mut self, refresh_non_root_metadata: bool) -> Self {
        self.refresh_non_root_metadata = refresh_non_root_metadata;
        self
    }

    /// Whether or not the new targets metadata inherits targets and delegations from the trusted
    /// targets metadata.
    ///
    /// Default is `true`.
    pub fn inherit_from_trusted_targets(mut self, inherit_from_trusted_targets: bool) -> Self {
        self.inherit_from_trusted_targets = inherit_from_trusted_targets;
        self
    }

    /// Stage a package manifest from the `path` to be published.
    pub async fn add_package(self, path: Utf8PathBuf) -> Result<RepoBuilder<'a, R>> {
        let contents = async_fs::read(path.as_std_path())
            .await
            .with_context(|| format!("reading package manifest {path}"))?;

        let package = PackageManifest::from_reader(&path, &contents[..])
            .with_context(|| format!("reading package manifest {path}"))?;

        self.add_package_manifest(Some(path), package).await
    }

    pub async fn add_package_manifest(
        self,
        path: Option<Utf8PathBuf>,
        manifest: PackageManifest,
    ) -> Result<RepoBuilder<'a, R>> {
        self.stage_package(ToBeStagedPackage {
            path,
            kind: ToBeStagedPackageKind::Manifest { manifest },
        })
        .await
    }

    /// Stage the package manifests from the iterator of paths to be published.
    pub async fn add_packages(
        mut self,
        paths: impl Iterator<Item = Utf8PathBuf>,
    ) -> Result<RepoBuilder<'a, R>> {
        for path in paths {
            self = self.add_package(path).await?;
        }
        Ok(self)
    }

    /// Stage a package archive from the `path` to be published.
    pub async fn add_package_archive(self, path: Utf8PathBuf) -> Result<RepoBuilder<'a, R>> {
        let archive_out = TempDir::new().unwrap();
        let manifest = PackageManifest::from_archive(path.as_std_path(), archive_out.path())
            .with_context(|| format!("reading package archive {path}"))
            .expect("archive to manifest");

        self.stage_package(ToBeStagedPackage {
            path: Some(path),
            kind: ToBeStagedPackageKind::Archive { _archive_out: archive_out, manifest },
        })
        .await
    }

    /// Stage the package archives from the iterator of paths to be published.
    pub async fn add_package_archives(
        mut self,
        paths: impl Iterator<Item = Utf8PathBuf>,
    ) -> Result<RepoBuilder<'a, R>> {
        for path in paths {
            self = self.add_package_archive(path).await?;
        }
        Ok(self)
    }

    /// Stage a top-level package manifest described by `package` from the
    /// `path` to be published. Duplicates are ignored unless registering two
    /// packages with the same package path and different package hashes.
    pub async fn stage_package(mut self, package: ToBeStagedPackage) -> Result<RepoBuilder<'a, R>> {
        // If the package was already included, by subpackage reference from
        // another package, then its blobs and subpackages were already added.
        // The package is being added as a named_package, so it can be removed
        // from additional_subpackages (if present).
        if self.additional_subpackages.remove(&package.manifest().hash()).is_none() {
            // The package was _not_ among the known additional_subpackages, so
            // add its blobs and subpackages.
            for blob in package.manifest().blobs() {
                self.deps.insert(blob.source_path.clone().into());
            }
            for subpackage in package.manifest().subpackages() {
                self = self.add_subpackage(Utf8PathBuf::from(&subpackage.manifest_path)).await?;
            }
        }

        if let Some(path) = package.path.clone() {
            self.deps.insert(path);
        }

        match self.named_packages.entry(package.manifest().package_path()) {
            hash_map::Entry::Vacant(entry) => {
                entry.insert(package);
            }
            hash_map::Entry::Occupied(entry) => {
                let old_package = entry.get();

                check_manifests_are_equivalent(entry.key(), old_package, &package)?;
            }
        }

        Ok(self)
    }

    /// Stage all the top-level package manifests from `iter` to be published.
    pub async fn add_package_manifests(
        mut self,
        iter: impl Iterator<Item = (Option<Utf8PathBuf>, PackageManifest)>,
    ) -> Result<RepoBuilder<'a, R>> {
        for (path, package) in iter {
            self = self.add_package_manifest(path, package).await?;
        }
        Ok(self)
    }

    /// Stage a subpackage's package manifest from the `path` to be published.
    async fn add_subpackage(mut self, path: Utf8PathBuf) -> Result<RepoBuilder<'a, R>> {
        let mut subpackage_paths = VecDeque::from([path]);

        while let Some(path) = subpackage_paths.pop_front() {
            if self.deps.get(&path).is_some() {
                // The package was already added, either as a named target package
                // or a previously referenced subpackage.
                continue;
            }

            let contents = async_fs::read(path.as_std_path())
                .await
                .with_context(|| format!("reading package manifest {path}"))?;

            let package = PackageManifest::from_reader(&path, &contents[..])
                .with_context(|| format!("reading package manifest {path}"))?;

            subpackage_paths.extend(self.add_subpackage_manifest(path, package).into_iter());
        }

        Ok(self)
    }

    /// Add a package that is a subpackage of another package. If the same
    /// package is later added as a named_package, it will be removed from
    /// the list of additional subpackages.
    ///
    /// Returns the paths to subpackages referenced from this subpackage.
    fn add_subpackage_manifest(
        &mut self,
        path: Utf8PathBuf,
        package: PackageManifest,
    ) -> Vec<Utf8PathBuf> {
        if !self.deps.insert(path.clone()) {
            // The package was already added, either as a named target package
            // or a previously referenced subpackage.
            return vec![];
        }

        // Track all the blobs from the package manifest as a dependency.
        for blob in package.blobs() {
            self.deps.insert(blob.source_path.clone().into());
        }

        let subpackages = package
            .subpackages()
            .iter()
            .map(|subpackage| Utf8PathBuf::from(&subpackage.manifest_path))
            .collect::<Vec<_>>();

        match self.additional_subpackages.entry(package.hash()) {
            hash_map::Entry::Vacant(entry) => {
                entry.insert((path, package));
            }
            hash_map::Entry::Occupied(_) => {
                // Multiple entries, even with different manifest paths, are
                // possible, but since the `additional_subpackages` hashmap is
                // only used to collect unique blobs, the duplicates can be
                // ignored.
            }
        }

        subpackages
    }

    /// Stage all the packages pointed to by the package list to be published.
    pub async fn add_package_list(mut self, path: Utf8PathBuf) -> Result<RepoBuilder<'a, R>> {
        let contents = async_fs::read(path.as_std_path())
            .await
            .with_context(|| format!("reading package manifest list {path}"))?;

        let package_list_manifest = PackageManifestList::from_reader(&contents[..])
            .with_context(|| format!("reading package manifest list {path}"))?;

        self.deps.insert(path);

        self.add_packages(package_list_manifest.into_iter()).await
    }

    /// Stage all the packages pointed to by the iterator of package lists to be published.
    pub async fn add_package_lists(
        mut self,
        paths: impl Iterator<Item = Utf8PathBuf>,
    ) -> Result<RepoBuilder<'a, R>> {
        for path in paths {
            self = self.add_package_list(path).await?;
        }

        Ok(self)
    }

    /// Read all remaining subpackages, and then commit the changes to the
    /// repository.
    ///
    /// Returns the list of the files that were read.
    pub async fn commit(self) -> Result<HashSet<Utf8PathBuf>> {
        let repo_builder = if let Some(database) = self.database.as_ref() {
            TufRepoBuilder::from_database(&self.repo, database)
        } else {
            TufRepoBuilder::create(&self.repo)
        };

        // Create a repo builder for the metadata, and initialize it with our repository keys.
        let mut repo_builder = repo_builder
            .current_time(self.current_time)
            .time_versioning(self.time_versioning)
            .root_expiration_duration(Duration::days(DEFAULT_ROOT_EXPIRATION))
            .targets_expiration_duration(Duration::days(DEFAULT_TARGETS_EXPIRATION))
            .snapshot_expiration_duration(Duration::days(DEFAULT_SNAPSHOT_EXPIRATION))
            .timestamp_expiration_duration(Duration::days(DEFAULT_TIMESTAMP_EXPIRATION));

        if let Some(signing_repo_keys) = self.signing_repo_keys {
            for key in signing_repo_keys.root_keys() {
                repo_builder = repo_builder.signing_root_keys(&[&**key]);
            }

            for key in signing_repo_keys.targets_keys() {
                repo_builder = repo_builder.signing_targets_keys(&[&**key]);
            }

            for key in signing_repo_keys.snapshot_keys() {
                repo_builder = repo_builder.signing_snapshot_keys(&[&**key]);
            }

            for key in signing_repo_keys.timestamp_keys() {
                repo_builder = repo_builder.signing_timestamp_keys(&[&**key]);
            }
        }

        for key in self.trusted_repo_keys.root_keys() {
            repo_builder = repo_builder.trusted_root_keys(&[&**key]);
        }

        for key in self.trusted_repo_keys.targets_keys() {
            repo_builder = repo_builder.trusted_targets_keys(&[&**key]);
        }

        for key in self.trusted_repo_keys.snapshot_keys() {
            repo_builder = repo_builder.trusted_snapshot_keys(&[&**key]);
        }

        for key in self.trusted_repo_keys.timestamp_keys() {
            repo_builder = repo_builder.trusted_timestamp_keys(&[&**key]);
        }

        // We can't generate a new root if we don't have any root keys.
        let mut repo_builder = if self.trusted_repo_keys.root_keys().is_empty() {
            repo_builder.skip_root()
        } else if self.refresh_metadata {
            repo_builder.stage_root()?
        } else {
            repo_builder.stage_root_if_necessary()?
        };

        repo_builder = repo_builder
            .inherit_from_trusted_targets(self.inherit_from_trusted_targets)
            .target_hash_algorithms(&[HashAlgorithm::Sha512]);

        // Gather up all package blobs, and separate out the the meta.far blobs.
        let mut staged_blobs = HashMap::new();
        let mut package_meta_fars = HashMap::new();

        for (package_path, package) in &self.named_packages {
            let mut meta_far_blob = None;
            for blob in package.manifest().blobs() {
                if blob.path == PackageManifest::META_FAR_BLOB_PATH {
                    if meta_far_blob.is_none() {
                        meta_far_blob = Some(blob.clone());
                    } else {
                        return Err(anyhow!("multiple meta.far blobs in a package"));
                    }
                }

                staged_blobs.insert(blob.merkle, blob.clone());
            }

            let meta_far_blob = meta_far_blob
                .ok_or_else(|| anyhow!("package does not contain entry for meta.far"))?;

            package_meta_fars.insert(package_path, meta_far_blob);
        }

        // Any additional_subpackages not added by name via `add_package()` are
        // anonymous subpackages. Stage the anonymous subpackage blobs, but
        // don't add the package to the repo as a named target.
        for (_, anonymous_subpackage) in self.additional_subpackages.values() {
            for blob in anonymous_subpackage.blobs() {
                staged_blobs.insert(blob.merkle, blob.clone());
            }
        }

        // Make sure all the blobs exist.
        for blob in staged_blobs.values() {
            async_fs::metadata(&blob.source_path)
                .await
                .with_context(|| format!("reading {}", blob.source_path))?;
        }

        // Stage the metadata blobs.
        for (package_path, meta_far_blob) in package_meta_fars {
            let target_path = TargetPath::new(package_path.to_string())?;
            let mut custom = HashMap::new();

            custom.insert("merkle".into(), serde_json::to_value(meta_far_blob.merkle)?);
            custom.insert("size".into(), serde_json::to_value(meta_far_blob.size)?);

            let f = File::open(&meta_far_blob.source_path).await?;

            repo_builder = repo_builder.add_target_with_custom(target_path, f, custom).await?;
        }

        // Stage the targets metadata. If we're forcing a metadata refresh, force a new targets,
        // snapshot, and timestamp, even if nothing changed in the contents.
        let repo_builder = if self.refresh_metadata || self.refresh_non_root_metadata {
            repo_builder.stage_targets()?
        } else {
            repo_builder.stage_targets_if_necessary()?
        };

        let repo_builder = repo_builder
            .snapshot_includes_length(true)
            .snapshot_includes_hashes(&[HashAlgorithm::Sha512]);

        let repo_builder = if self.refresh_metadata || self.refresh_non_root_metadata {
            repo_builder.stage_snapshot()?
        } else {
            repo_builder.stage_snapshot_if_necessary()?
        };

        let repo_builder = repo_builder
            .timestamp_includes_length(true)
            .timestamp_includes_hashes(&[HashAlgorithm::Sha512]);

        let repo_builder = if self.refresh_metadata || self.refresh_non_root_metadata {
            repo_builder.stage_timestamp()?
        } else {
            repo_builder.stage_timestamp_if_necessary()?
        };

        repo_builder.commit().await.context("publishing metadata")?;

        // Stage the blobs.
        for (blob_hash, blob) in staged_blobs {
            self.repo.store_blob(&blob_hash, Utf8Path::new(&blob.source_path)).await?;
        }

        Ok(self.deps)
    }
}

fn check_manifests_are_equivalent(
    package_path: &PackagePath,
    old_package: &ToBeStagedPackage,
    new_package: &ToBeStagedPackage,
) -> Result<()> {
    // Check if the packages conflict.
    if old_package.manifest().hash() == new_package.manifest().hash() {
        return Ok(());
    }

    // Create a message that tries to explain why we have a conflict.
    let old_manifest_path =
        old_package.path.as_ref().map(|path| path.as_str()).unwrap_or("<generated>");
    let new_manifest_path =
        new_package.path.as_ref().map(|path| path.as_str()).unwrap_or("<generated>");

    let mut msg = vec![format!(
        "conflict for repository path '{package_path}'\n  manifest paths:\n  - {old_manifest_path}\n  - {new_manifest_path}\n  differences:",
    )];

    #[derive(PartialEq, Eq)]
    enum BlobEntry {
        Contents(Vec<u8>),
        Blob(BlobInfo),
    }

    // Helper to read in all the package contents so we can compare entries.
    fn manifest_contents(manifest: &PackageManifest) -> Result<BTreeMap<String, BlobEntry>> {
        let mut entries = BTreeMap::new();

        for blob in manifest.blobs() {
            if blob.path == "meta/" {
                let file = std::fs::File::open(&blob.source_path)
                    .with_context(|| format!("reading {}", blob.path))?;
                let mut far = fuchsia_archive::Utf8Reader::new(file)?;

                let far_entries =
                    far.list().map(|entry| entry.path().to_owned()).collect::<Vec<_>>();
                for path in far_entries {
                    let contents = far.read_file(&path)?;
                    entries.insert(path, BlobEntry::Contents(contents));
                }
            }

            entries.insert(blob.path.clone(), BlobEntry::Blob(blob.clone()));
        }

        Ok(entries)
    }

    // Compare the contents and report any differences.
    if let (Ok(old_contents), Ok(mut new_contents)) =
        (manifest_contents(old_package.manifest()), manifest_contents(new_package.manifest()))
    {
        for (path, old_entry) in old_contents {
            if let Some(new_entry) = new_contents.remove(&path) {
                match (old_entry, new_entry) {
                    (BlobEntry::Blob(old_blob), BlobEntry::Blob(new_blob)) => {
                        if old_blob.merkle != new_blob.merkle {
                            msg.push(format!(
                                "  - {}: different contents found in:\n    - {}\n    - {}",
                                path, old_blob.source_path, new_blob.source_path
                            ));
                        }
                    }
                    (old_entry, new_entry) => {
                        if old_entry != new_entry {
                            msg.push(format!("  - {path}: different contents"));
                        }
                    }
                }
            } else {
                msg.push(format!("  - {path}: missing from manifest {new_manifest_path}"));
            }
        }

        for path in new_contents.into_keys() {
            msg.push(format!("  - {path}: missing from manifest {old_manifest_path}"));
        }
    }

    // Helper to read in all the subpackages so we can compare entries.
    fn manifest_subpackages(
        manifest: &PackageManifest,
    ) -> Result<BTreeMap<String, SubpackageInfo>> {
        let mut entries = BTreeMap::new();
        for subpackage in manifest.subpackages() {
            entries.insert(subpackage.name.clone(), subpackage.clone());
        }
        Ok(entries)
    }

    // Compare the subpackages and report any differences.
    if let (Ok(old_subpackages), Ok(mut new_subpackages)) =
        (manifest_subpackages(old_package.manifest()), manifest_subpackages(new_package.manifest()))
    {
        for (name, old_subpackage) in old_subpackages {
            if let Some(new_subpackage) = new_subpackages.remove(&name) {
                if old_subpackage.merkle != new_subpackage.merkle {
                    msg.push(format!(
                        "  - {}: different subpackages found in:\n    - {}\n    - {}",
                        name, old_subpackage.manifest_path, new_subpackage.manifest_path
                    ));
                }
            } else {
                msg.push(format!(
                    "  - {name}: subpackage missing from manifest {new_manifest_path}"
                ));
            }
        }

        for name in new_subpackages.into_keys() {
            msg.push(format!(
                "  - {name}: subpackage missing from manifest {old_manifest_path}"
            ));
        }
    }

    Err(anyhow!(msg.join("\n")))
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            repo_client::RepoClient,
            repository::{FileSystemRepository, PmRepository},
            test_utils,
        },
        assert_matches::assert_matches,
        camino::Utf8Path,
        fuchsia_pkg::PackageBuilder,
        pretty_assertions::{assert_eq, assert_ne},
        std::{
            collections::{BTreeMap, BTreeSet, HashMap},
            fs,
        },
        tuf::{
            crypto::Ed25519PrivateKey,
            metadata::{Metadata as _, MetadataPath},
        },
        walkdir::WalkDir,
    };

    pub(crate) fn read_dir(dir: &Utf8Path) -> BTreeMap<String, Vec<u8>> {
        let mut entries = BTreeMap::new();
        for entry in WalkDir::new(dir) {
            let entry = entry.unwrap();
            if entry.metadata().unwrap().is_file() {
                let path = entry.path().strip_prefix(dir).unwrap().to_str().unwrap().to_string();
                let contents = std::fs::read(entry.path()).unwrap();

                entries.insert(path, contents);
            }
        }

        entries
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_create() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        let repo = PmRepository::new(dir.to_path_buf());
        let repo_keys = test_utils::make_repo_keys();

        RepoBuilder::create(&repo, &repo_keys).commit().await.unwrap();

        // Make sure we can update a client from this metadata.
        let mut repo_client = RepoClient::from_trusted_remote(repo).await.unwrap();
        assert_matches!(repo_client.update().await, Ok(true));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_create_and_update_repo() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        let metadata_repo_path = dir.join("metadata");
        let blob_repo_path = dir.join("blobs");
        let repo = FileSystemRepository::new(metadata_repo_path, blob_repo_path.clone());
        let repo_keys = test_utils::make_repo_keys();

        let pkg1_dir = dir.join("package1");
        let (pkg1_meta_far_path, pkg1_manifest) =
            test_utils::make_package_manifest("package1", pkg1_dir.as_std_path(), Vec::new());
        let pkg1_manifest_path = pkg1_dir.join("package1.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg1_manifest_path).unwrap(), &pkg1_manifest)
            .unwrap();
        let pkg1_meta_far_contents = std::fs::read(&pkg1_meta_far_path).unwrap();

        RepoBuilder::create(&repo, &repo_keys)
            .add_package(pkg1_manifest_path)
            .await
            .unwrap()
            .commit()
            .await
            .unwrap();

        // Make sure we wrote all the blobs from package1.
        assert_eq!(
            read_dir(&blob_repo_path),
            BTreeMap::from([
                (test_utils::PKG1_HASH.into(), pkg1_meta_far_contents.clone()),
                (test_utils::PKG1_BIN_HASH.into(), b"binary package1".to_vec()),
                (test_utils::PKG1_LIB_HASH.into(), b"lib package1".to_vec()),
            ])
        );

        // Make sure we can update a client from this metadata.
        let mut repo_client = RepoClient::from_trusted_remote(repo).await.unwrap();
        assert_matches!(repo_client.update().await, Ok(true));

        assert_eq!(repo_client.database().trusted_root().version(), 1);
        assert_eq!(repo_client.database().trusted_targets().map(|m| m.version()), Some(1));
        assert_eq!(repo_client.database().trusted_snapshot().map(|m| m.version()), Some(1));
        assert_eq!(repo_client.database().trusted_timestamp().map(|m| m.version()), Some(1));

        // Create the next version of the metadata and add a new package to it.
        let pkg2_dir = dir.join("package2");
        let (pkg2_meta_far_path, pkg2_manifest) =
            test_utils::make_package_manifest("package2", pkg2_dir.as_std_path(), Vec::new());
        let pkg2_manifest_path = pkg2_dir.join("package2.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg2_manifest_path).unwrap(), &pkg2_manifest)
            .unwrap();
        let pkg2_meta_far_contents = std::fs::read(&pkg2_meta_far_path).unwrap();

        let archive_outdir = TempDir::new().unwrap();

        let archive_path = archive_outdir.path().join("p2.far");
        let archive_file = fs::File::create(archive_path.clone()).unwrap();
        pkg2_manifest.archive(&pkg2_dir, &archive_file).await.unwrap();

        RepoBuilder::from_client(&repo_client, &repo_keys)
            .add_package_archive(Utf8PathBuf::from_path_buf(archive_path).unwrap())
            .await
            .unwrap()
            .commit()
            .await
            .unwrap();

        // Make sure we wrote all the blobs from package1 and package2.
        assert_eq!(
            read_dir(&blob_repo_path),
            BTreeMap::from([
                (test_utils::PKG1_HASH.into(), pkg1_meta_far_contents.clone()),
                (test_utils::PKG1_BIN_HASH.into(), b"binary package1".to_vec()),
                (test_utils::PKG1_LIB_HASH.into(), b"lib package1".to_vec()),
                (test_utils::PKG2_HASH.into(), pkg2_meta_far_contents.clone()),
                (test_utils::PKG2_BIN_HASH.into(), b"binary package2".to_vec()),
                (test_utils::PKG2_LIB_HASH.into(), b"lib package2".to_vec()),
            ])
        );

        // Make sure we can resolve the new metadata.
        assert_matches!(repo_client.update().await, Ok(true));
        assert_eq!(repo_client.database().trusted_root().version(), 1);
        assert_eq!(repo_client.database().trusted_targets().map(|m| m.version()), Some(2));
        assert_eq!(repo_client.database().trusted_snapshot().map(|m| m.version()), Some(2));
        assert_eq!(repo_client.database().trusted_timestamp().map(|m| m.version()), Some(2));

        // Make sure the timestamp and snapshot metadata was generated with the snapshot and targets
        // length and hashes.
        let snapshot_description = repo_client.database().trusted_timestamp().unwrap().snapshot();
        assert!(snapshot_description.length().is_some());
        assert!(!snapshot_description.hashes().is_empty());

        let trusted_snapshot = repo_client.database().trusted_snapshot().unwrap();
        let targets_description = trusted_snapshot.meta().get(&MetadataPath::targets()).unwrap();
        assert!(targets_description.length().is_some());
        assert!(!targets_description.hashes().is_empty());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_create_and_update_repo_with_subpackages() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        let metadata_repo_path = dir.join("metadata");
        let blob_repo_path = dir.join("blobs");
        let repo = FileSystemRepository::new(metadata_repo_path, blob_repo_path.clone());
        let repo_keys = test_utils::make_repo_keys();

        const ANONYMOUS_SUBPACKAGE: &str = "anonymous_subpackage";
        const NAMED_SUBPACKAGE: &str = "named_subpackage";
        const SUPERPACKAGE: &str = "superpackage";

        // Create an anonymous subpackage (a subpackage that is not directly
        // added to the RepoBuilder, but is added indirectly because it is
        // referenced as a subpackage of another package added to the repo).
        let anonsubpkg_dir = dir.join(ANONYMOUS_SUBPACKAGE);
        let (anonsubpkg_meta_far_path, anonsubpkg_manifest) = test_utils::make_package_manifest(
            ANONYMOUS_SUBPACKAGE,
            anonsubpkg_dir.as_std_path(),
            Vec::new(),
        );
        let anonsubpkg_manifest_path = anonsubpkg_dir.join("anonymous_subpackage.manifest");
        serde_json::to_writer(
            std::fs::File::create(&anonsubpkg_manifest_path).unwrap(),
            &anonsubpkg_manifest,
        )
        .unwrap();
        let anonsubpkg_meta_far_contents = std::fs::read(&anonsubpkg_meta_far_path).unwrap();

        // Create a named package (named_subpackage), which will also be a
        // subpackage of "superpackage". This named_subpackage will include the
        // anonymous_subpackage.
        let namedsubpkg_dir = dir.join(NAMED_SUBPACKAGE);
        let (namedsubpkg_meta_far_path, namedsubpkg_manifest) = test_utils::make_package_manifest(
            NAMED_SUBPACKAGE,
            namedsubpkg_dir.as_std_path(),
            vec![(
                "anon_subpackage_of_namedsubpkg".parse().unwrap(),
                anonsubpkg_manifest.hash(),
                anonsubpkg_manifest_path.clone().into(),
            )],
        );
        let namedsubpkg_manifest_path = namedsubpkg_dir.join("named_subpackage.manifest");
        serde_json::to_writer(
            std::fs::File::create(&namedsubpkg_manifest_path).unwrap(),
            &namedsubpkg_manifest,
        )
        .unwrap();
        let namedsubpkg_meta_far_contents = std::fs::read(&namedsubpkg_meta_far_path).unwrap();

        // Create a named package ("superpackage"), which will also be a superpackage
        // of both named_subpackage and anonymous_subpackage. Note that
        // named_subpackage is ALSO a superpackage of anonymous_subpackage, so
        // anonymous_subpackage is referenced twice. It will only exist once in
        // the repo.
        let superpkg_dir = dir.join(SUPERPACKAGE);
        let (superpkg_meta_far_path, superpkg_manifest) = test_utils::make_package_manifest(
            SUPERPACKAGE,
            superpkg_dir.as_std_path(),
            vec![
                (
                    NAMED_SUBPACKAGE.parse().unwrap(),
                    namedsubpkg_manifest.hash(),
                    namedsubpkg_manifest_path.clone().into(),
                ),
                (
                    "anon_subpackage_of_superpkg".parse().unwrap(),
                    anonsubpkg_manifest.hash(),
                    anonsubpkg_manifest_path.clone().into(),
                ),
            ],
        );
        let superpkg_manifest_path = superpkg_dir.join("superpackage.manifest");
        serde_json::to_writer(
            std::fs::File::create(&superpkg_manifest_path).unwrap(),
            &superpkg_manifest,
        )
        .unwrap();
        let superpkg_meta_far_contents = std::fs::read(&superpkg_meta_far_path).unwrap();

        // Add the two named packages. The anonymous subpackage will be added
        // automatically.
        RepoBuilder::create(&repo, &repo_keys)
            .add_package(superpkg_manifest_path)
            .await
            .unwrap()
            .add_package(namedsubpkg_manifest_path)
            .await
            .unwrap()
            .commit()
            .await
            .unwrap();

        let repo_blobs = read_dir(&blob_repo_path);

        assert_eq!(
            repo_blobs.keys().map(|k| k.to_owned()).collect::<BTreeSet<String>>(),
            BTreeSet::from([
                test_utils::ANONSUBPKG_HASH.into(),
                test_utils::ANONSUBPKG_BIN_HASH.into(),
                test_utils::ANONSUBPKG_LIB_HASH.into(),
                test_utils::NAMEDSUBPKG_HASH.into(),
                test_utils::NAMEDSUBPKG_BIN_HASH.into(),
                test_utils::NAMEDSUBPKG_LIB_HASH.into(),
                test_utils::SUPERPKG_HASH.into(),
                test_utils::SUPERPKG_BIN_HASH.into(),
                test_utils::SUPERPKG_LIB_HASH.into(),
            ])
        );

        // Make sure we wrote all the blobs from package1.
        assert_eq!(
            read_dir(&blob_repo_path),
            BTreeMap::from([
                (test_utils::ANONSUBPKG_HASH.into(), anonsubpkg_meta_far_contents.clone()),
                (test_utils::ANONSUBPKG_BIN_HASH.into(), b"binary anonymous_subpackage".to_vec()),
                (test_utils::ANONSUBPKG_LIB_HASH.into(), b"lib anonymous_subpackage".to_vec()),
                (test_utils::NAMEDSUBPKG_HASH.into(), namedsubpkg_meta_far_contents.clone()),
                (test_utils::NAMEDSUBPKG_BIN_HASH.into(), b"binary named_subpackage".to_vec()),
                (test_utils::NAMEDSUBPKG_LIB_HASH.into(), b"lib named_subpackage".to_vec()),
                (test_utils::SUPERPKG_HASH.into(), superpkg_meta_far_contents.clone()),
                (test_utils::SUPERPKG_BIN_HASH.into(), b"binary superpackage".to_vec()),
                (test_utils::SUPERPKG_LIB_HASH.into(), b"lib superpackage".to_vec()),
            ])
        );

        // Make sure we can update a client from this metadata.
        let mut repo_client = RepoClient::from_trusted_remote(repo).await.unwrap();
        assert_matches!(repo_client.update().await, Ok(true));

        assert_eq!(repo_client.database().trusted_root().version(), 1);
        assert_eq!(repo_client.database().trusted_targets().map(|m| m.version()), Some(1));
        assert_eq!(repo_client.database().trusted_snapshot().map(|m| m.version()), Some(1));
        assert_eq!(repo_client.database().trusted_timestamp().map(|m| m.version()), Some(1));

        // Make sure we have targets for the named packages only.
        let trusted_targets = repo_client.database().trusted_targets().unwrap();
        assert!(trusted_targets
            .targets()
            .get(&TargetPath::new(format!("{SUPERPACKAGE}/0")).unwrap())
            .is_some());
        assert!(trusted_targets
            .targets()
            .get(&TargetPath::new(format!("{NAMED_SUBPACKAGE}/0")).unwrap())
            .is_some());
        assert!(trusted_targets
            .targets()
            .get(&TargetPath::new(format!("{ANONYMOUS_SUBPACKAGE}/0")).unwrap())
            .is_none());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_refresh_metadata_with_all_keys() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        // Load up the test metadata, which was created some time ago, and has a different
        // expiration date.
        let repo = test_utils::make_pm_repository(dir).await;

        // Download the older metadata before we refresh it.
        let mut repo_client = RepoClient::from_trusted_remote(&repo).await.unwrap();
        repo_client.update().await.unwrap();

        let root1 = (*repo_client.database().trusted_root()).clone();
        let targets1 = repo_client.database().trusted_targets().cloned().unwrap();
        let snapshot1 = repo_client.database().trusted_snapshot().cloned().unwrap();
        let timestamp1 = repo_client.database().trusted_timestamp().cloned().unwrap();

        // Update the metadata expiration.
        let repo_keys = RepoKeys::from_dir(&dir.join("keys").into_std_path_buf()).unwrap();
        RepoBuilder::from_database(repo_client.remote_repo(), &repo_keys, repo_client.database())
            .refresh_metadata(true)
            .commit()
            .await
            .unwrap();

        // Finally, make sure the metadata has changed.
        assert_matches!(repo_client.update().await, Ok(true));

        let root2 = (*repo_client.database().trusted_root()).clone();
        let targets2 = repo_client.database().trusted_targets().cloned().unwrap();
        let snapshot2 = repo_client.database().trusted_snapshot().cloned().unwrap();
        let timestamp2 = repo_client.database().trusted_timestamp().cloned().unwrap();

        // Make sure we generated new metadata.
        assert_ne!(root1, root2);
        assert_ne!(targets1, targets2);
        assert_ne!(snapshot1, snapshot2);
        assert_ne!(timestamp1, timestamp2);

        // We should have kept our old snapshot entries (except the target should have changed).
        assert_eq!(
            snapshot1
                .meta()
                .iter()
                .filter(|(k, _)| **k != MetadataPath::targets())
                .collect::<HashMap<_, _>>(),
            snapshot2
                .meta()
                .iter()
                .filter(|(k, _)| **k != MetadataPath::targets())
                .collect::<HashMap<_, _>>(),
        );

        // We should have kept our targets and delegations.
        assert_eq!(targets1.targets(), targets2.targets());
        assert_eq!(targets1.delegations(), targets2.delegations());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_refresh_metadata_with_some_keys() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        // Load the repo.
        let repo = test_utils::make_pm_repository(dir).await;

        // Download the older metadata before we refresh it.
        let mut repo_client = RepoClient::from_trusted_remote(&repo).await.unwrap();
        repo_client.update().await.unwrap();

        let root1 = (*repo_client.database().trusted_root()).clone();
        let targets1 = repo_client.database().trusted_targets().cloned().unwrap();
        let snapshot1 = repo_client.database().trusted_snapshot().cloned().unwrap();
        let timestamp1 = repo_client.database().trusted_timestamp().cloned().unwrap();

        // Load the repo, but delete the root private key file.
        let keys_dir = dir.join("keys");
        std::fs::remove_file(keys_dir.join("root.json")).unwrap();

        // Update the metadata expiration.
        let repo_keys = RepoKeys::from_dir(&dir.join("keys").into_std_path_buf()).unwrap();

        // Update the metadata expiration should succeed.
        RepoBuilder::from_database(repo_client.remote_repo(), &repo_keys, repo_client.database())
            .refresh_metadata(true)
            .commit()
            .await
            .unwrap();

        // Make sure the metadata has changed.
        assert_matches!(repo_client.update().await, Ok(true));

        let root2 = (*repo_client.database().trusted_root()).clone();
        let targets2 = repo_client.database().trusted_targets().cloned().unwrap();
        let snapshot2 = repo_client.database().trusted_snapshot().cloned().unwrap();
        let timestamp2 = repo_client.database().trusted_timestamp().cloned().unwrap();

        // Make sure we generated new metadata, except for the root metadata.
        assert_eq!(root1, root2);
        assert_ne!(targets1, targets2);
        assert_ne!(snapshot1, snapshot2);
        assert_ne!(timestamp1, timestamp2);

        // We should have kept our old snapshot entries (except the target should have changed).
        assert_eq!(
            snapshot1
                .meta()
                .iter()
                .filter(|(k, _)| **k != MetadataPath::targets())
                .collect::<HashMap<_, _>>(),
            snapshot2
                .meta()
                .iter()
                .filter(|(k, _)| **k != MetadataPath::targets())
                .collect::<HashMap<_, _>>(),
        );

        // We should have kept our targets and delegations.
        assert_eq!(targets1.targets(), targets2.targets());
        assert_eq!(targets1.delegations(), targets2.delegations());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_refresh_metadata_with_no_keys() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        // Load the repo.
        let repo = test_utils::make_pm_repository(dir).await;

        // Download the older metadata before we refresh it.
        let mut repo_client = RepoClient::from_trusted_remote(&repo).await.unwrap();
        repo_client.update().await.unwrap();

        let root1 = (*repo_client.database().trusted_root()).clone();
        let targets1 = repo_client.database().trusted_targets().cloned().unwrap();
        let snapshot1 = repo_client.database().trusted_snapshot().cloned().unwrap();
        let timestamp1 = repo_client.database().trusted_timestamp().cloned().unwrap();

        // Try to refresh the metadata with an empty key set, which should error out.
        let repo_keys = RepoKeys::builder().build();
        let res = RepoBuilder::from_database(
            repo_client.remote_repo(),
            &repo_keys,
            repo_client.database(),
        )
        .refresh_metadata(true)
        .commit()
        .await;
        assert_matches!(res, Err(_));

        // Updating the client should return that there were no changes.
        assert_matches!(repo_client.update().await, Ok(false));

        let root2 = (*repo_client.database().trusted_root()).clone();
        let targets2 = repo_client.database().trusted_targets().cloned().unwrap();
        let snapshot2 = repo_client.database().trusted_snapshot().cloned().unwrap();
        let timestamp2 = repo_client.database().trusted_timestamp().cloned().unwrap();

        // We should not have changed the metadata.
        assert_eq!(root1, root2);
        assert_eq!(targets1, targets2);
        assert_eq!(snapshot1, snapshot2);
        assert_eq!(timestamp1, timestamp2);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_refresh_metadata_with_root_metadata() {
        let tmp = tempfile::tempdir().unwrap();
        let root = Utf8Path::from_path(tmp.path()).unwrap();

        // First create a repository.
        let full_repo_path = root.join("full");
        let full_metadata_repo_path = full_repo_path.join("repository");
        test_utils::make_pm_repo_dir(full_repo_path.as_std_path()).await;

        // Then create a repository, which only has the root metadata in it.
        let test_repo_path = root.join("test");
        let test_metadata_repo_path = test_repo_path.join("repository");
        std::fs::create_dir_all(&test_metadata_repo_path).unwrap();

        std::fs::copy(
            full_metadata_repo_path.join("root.json"),
            test_metadata_repo_path.join("1.root.json"),
        )
        .unwrap();

        // Create a repo client and download the root metadata. Update should fail with missint TUF
        // metadata since we don't have any other metadata.
        let repo = PmRepository::new(test_repo_path);
        let mut repo_client = RepoClient::from_trusted_remote(&repo).await.unwrap();
        assert_matches!(
            repo_client.update().await,
            Err(crate::repository::Error::Tuf(tuf::Error::MetadataNotFound { path, .. }))
            if path == tuf::metadata::MetadataPath::timestamp()
        );

        assert!(repo_client.database().trusted_targets().is_none());
        assert!(repo_client.database().trusted_snapshot().is_none());
        assert!(repo_client.database().trusted_timestamp().is_none());

        // Update the metadata expiration. We'll use the keys from the full pm directory.
        let repo_keys =
            RepoKeys::from_dir(&full_repo_path.join("keys").into_std_path_buf()).unwrap();
        RepoBuilder::from_database(repo_client.remote_repo(), &repo_keys, repo_client.database())
            .refresh_metadata(true)
            .commit()
            .await
            .unwrap();

        // Updating the client should succeed since we created the missing metadata.
        assert_matches!(repo_client.update().await, Ok(true));

        assert!(repo_client.database().trusted_targets().is_some());
        assert!(repo_client.database().trusted_snapshot().is_some());
        assert!(repo_client.database().trusted_timestamp().is_some());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_inherit_from_trusted_targets() {
        let tmp = tempfile::tempdir().unwrap();
        let root = Utf8Path::from_path(tmp.path()).unwrap();
        let repo_dir = root.join("repo");

        // Load the repo, which already contains package1 and package2.
        let repo = test_utils::make_pm_repository(repo_dir).await;
        let mut repo_client = RepoClient::from_trusted_remote(&repo).await.unwrap();
        repo_client.update().await.unwrap();

        // Publish package3 to the repository.
        let pkg3_dir = root.join("pkg3");
        let (_, pkg3_manifest) =
            test_utils::make_package_manifest("package3", pkg3_dir.as_std_path(), Vec::new());
        let pkg3_manifest_path = pkg3_dir.join("package3.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg3_manifest_path).unwrap(), &pkg3_manifest)
            .unwrap();

        let repo_keys = test_utils::make_repo_keys();
        RepoBuilder::from_database(repo_client.remote_repo(), &repo_keys, repo_client.database())
            .add_package(pkg3_manifest_path)
            .await
            .unwrap()
            .commit()
            .await
            .unwrap();

        // Make sure we have metadata for package1, package2, and package3.
        assert_matches!(repo_client.update().await, Ok(true));
        let trusted_targets = repo_client.database().trusted_targets().unwrap();
        assert!(trusted_targets.targets().get("package1/0").is_some());
        assert!(trusted_targets.targets().get("package2/0").is_some());
        assert!(trusted_targets.targets().get("package3/0").is_some());

        // Now do another commit, but this time not inheriting the old packages.
        let pkg4_dir = root.join("pkg4");
        let (_, pkg4_manifest) =
            test_utils::make_package_manifest("package4", pkg4_dir.as_std_path(), Vec::new());
        let pkg4_manifest_path = pkg4_dir.join("package4.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg4_manifest_path).unwrap(), &pkg4_manifest)
            .unwrap();

        RepoBuilder::from_database(repo_client.remote_repo(), &repo_keys, repo_client.database())
            .inherit_from_trusted_targets(false)
            .add_package(pkg4_manifest_path)
            .await
            .unwrap()
            .commit()
            .await
            .unwrap();

        // We should only have metadata for package4.
        assert_matches!(repo_client.update().await, Ok(true));
        let trusted_targets = repo_client.database().trusted_targets().unwrap();
        assert!(trusted_targets.targets().get("package1/0").is_none());
        assert!(trusted_targets.targets().get("package2/0").is_none());
        assert!(trusted_targets.targets().get("package3/0").is_none());
        assert!(trusted_targets.targets().get("package4/0").is_some());
    }

    fn generate_ed25519_private_key() -> Ed25519PrivateKey {
        Ed25519PrivateKey::from_pkcs8(&Ed25519PrivateKey::pkcs8().unwrap()).unwrap()
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_key_rotation() {
        let tmp = tempfile::tempdir().unwrap();
        let root = Utf8Path::from_path(tmp.path()).unwrap();
        let repo_dir = root.join("repo");

        // First, make a repository.
        let repo = test_utils::make_pm_repository(repo_dir).await;
        let mut repo_client = RepoClient::from_trusted_remote(&repo).await.unwrap();
        repo_client.update().await.unwrap();

        // Then make a new RepoKeys with unique keys.
        let repo_trusted_keys = RepoKeys::builder()
            .add_root_key(Box::new(generate_ed25519_private_key()))
            .add_targets_key(Box::new(generate_ed25519_private_key()))
            .add_snapshot_key(Box::new(generate_ed25519_private_key()))
            .add_timestamp_key(Box::new(generate_ed25519_private_key()))
            .build();

        // Generate new metadata that trusts the new keys, but signs it with the old keys.
        let repo_signing_keys = repo.repo_keys().unwrap();
        RepoBuilder::from_database(
            repo_client.remote_repo(),
            &repo_trusted_keys,
            repo_client.database(),
        )
        .signing_repo_keys(&repo_signing_keys)
        .commit()
        .await
        .unwrap();

        // Make sure we can update.
        assert_matches!(repo_client.update().await, Ok(true));
        assert_eq!(repo_client.database().trusted_root().version(), 2);
        assert_eq!(repo_client.database().trusted_snapshot().unwrap().version(), 2);
        assert_eq!(repo_client.database().trusted_targets().unwrap().version(), 2);
        assert_eq!(repo_client.database().trusted_timestamp().unwrap().version(), 2);

        // Make sure we only trust the new keys.
        let trusted_root = repo_client.database().trusted_root();
        assert_eq!(
            trusted_root.root_keys().collect::<Vec<_>>(),
            repo_trusted_keys.root_keys().iter().map(|k| k.public()).collect::<Vec<_>>(),
        );

        assert_eq!(
            trusted_root.targets_keys().collect::<Vec<_>>(),
            repo_trusted_keys.targets_keys().iter().map(|k| k.public()).collect::<Vec<_>>(),
        );

        assert_eq!(
            trusted_root.snapshot_keys().collect::<Vec<_>>(),
            repo_trusted_keys.snapshot_keys().iter().map(|k| k.public()).collect::<Vec<_>>(),
        );

        assert_eq!(
            trusted_root.timestamp_keys().collect::<Vec<_>>(),
            repo_trusted_keys.timestamp_keys().iter().map(|k| k.public()).collect::<Vec<_>>(),
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_conflicting_package_manifests_errors_out() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        let metadata_repo_path = dir.join("metadata");
        let blob_repo_path = dir.join("blobs");
        let repo = FileSystemRepository::new(metadata_repo_path, blob_repo_path);
        let repo_keys = test_utils::make_repo_keys();

        let pkg1_dir = dir.join("package1");
        let (_, pkg1_manifest) =
            test_utils::make_package_manifest("package1", pkg1_dir.as_std_path(), Vec::new());
        let pkg1_manifest_path = pkg1_dir.join("package1.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg1_manifest_path).unwrap(), &pkg1_manifest)
            .unwrap();

        // Whoops, we created a package with the same package name but with different contents.
        let pkg2_dir = dir.join("package2");
        let pkg2_meta_far_path = pkg2_dir.join("meta.far");
        let pkg2_manifest =
            PackageBuilder::new("package1").build(&pkg2_dir, &pkg2_meta_far_path).unwrap();
        let pkg2_manifest_path = pkg2_dir.join("package2.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg2_manifest_path).unwrap(), &pkg2_manifest)
            .unwrap();

        assert!(RepoBuilder::create(&repo, &repo_keys)
            .add_package(pkg1_manifest_path)
            .await
            .unwrap()
            .add_package(pkg2_manifest_path)
            .await
            .is_err());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_conflicting_package_archives_errors_out() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        let metadata_repo_path = dir.join("metadata");
        let blob_repo_path = dir.join("blobs");
        let repo = FileSystemRepository::new(metadata_repo_path, blob_repo_path);
        let repo_keys = test_utils::make_repo_keys();

        let pkg1_dir = dir.join("package1");
        let (_, pkg1_manifest) =
            test_utils::make_package_manifest("package1", pkg1_dir.as_std_path(), Vec::new());
        let pkg1_manifest_path = pkg1_dir.join("package1.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg1_manifest_path).unwrap(), &pkg1_manifest)
            .unwrap();

        // Whoops, we created a package with the same package name but with different contents.
        let pkg2_dir = dir.join("package2");
        let pkg2_meta_far_path = pkg2_dir.join("meta.far");
        let pkg2_manifest =
            PackageBuilder::new("package1").build(&pkg2_dir, &pkg2_meta_far_path).unwrap();
        let pkg2_manifest_path = pkg2_dir.join("package2.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg2_manifest_path).unwrap(), &pkg2_manifest)
            .unwrap();

        let archive_outdir = TempDir::new().unwrap();

        let archive_path1 = archive_outdir.path().join("p1.far");
        let archive_file1 = fs::File::create(archive_path1.clone()).unwrap();
        pkg1_manifest.archive(&pkg1_dir, &archive_file1).await.unwrap();

        let archive_path2 = archive_outdir.path().join("p2.far");
        let archive_file2 = fs::File::create(archive_path2.clone()).unwrap();
        pkg2_manifest.archive(&pkg2_dir, &archive_file2).await.unwrap();

        assert!(RepoBuilder::create(&repo, &repo_keys)
            .add_package_archive(Utf8PathBuf::from_path_buf(archive_path1).unwrap())
            .await
            .unwrap()
            .add_package_archive(Utf8PathBuf::from_path_buf(archive_path2).unwrap())
            .await
            .is_err());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_conflicting_package_archive_and_manifest_errors_out() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = Utf8Path::from_path(tmp.path()).unwrap();

        let metadata_repo_path = dir.join("metadata");
        let blob_repo_path = dir.join("blobs");
        let repo = FileSystemRepository::new(metadata_repo_path, blob_repo_path);
        let repo_keys = test_utils::make_repo_keys();

        let pkg1_dir = dir.join("package1");
        let (_, pkg1_manifest) =
            test_utils::make_package_manifest("package1", pkg1_dir.as_std_path(), Vec::new());
        let pkg1_manifest_path = pkg1_dir.join("package1.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg1_manifest_path).unwrap(), &pkg1_manifest)
            .unwrap();

        // Whoops, we created a package with the same package name but with different contents.
        let pkg2_dir = dir.join("package2");
        let pkg2_meta_far_path = pkg2_dir.join("meta.far");
        let pkg2_manifest =
            PackageBuilder::new("package1").build(&pkg2_dir, &pkg2_meta_far_path).unwrap();
        let pkg2_manifest_path = pkg2_dir.join("package2.manifest");
        serde_json::to_writer(std::fs::File::create(&pkg2_manifest_path).unwrap(), &pkg2_manifest)
            .unwrap();

        let archive_outdir = TempDir::new().unwrap();

        let archive_path1 = archive_outdir.path().join("p1.far");
        let archive_file1 = fs::File::create(archive_path1.clone()).unwrap();
        pkg1_manifest.archive(&pkg1_dir, &archive_file1).await.unwrap();

        assert!(RepoBuilder::create(&repo, &repo_keys)
            .add_package_archive(Utf8PathBuf::from_path_buf(archive_path1).unwrap())
            .await
            .unwrap()
            .add_package(pkg2_manifest_path)
            .await
            .is_err());
    }
}
