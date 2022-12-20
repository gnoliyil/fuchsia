// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::api::Blob as BlobApi;
use crate::api::DataSource as DataSourceApi;
use crate::api::Hash as HashApi;
use crate::data_source::BlobDirectory;
use crate::data_source::BlobFsArchive;
use crate::hash::Hash;
use fuchsia_hash::ParseHashError as FuchsiaParseHashError;
use fuchsia_merkle::Hash as FuchsiaMerkleHash;
use fuchsia_merkle::MerkleTree;
use scrutiny_utils::blobfs::BlobFsReader;
use scrutiny_utils::blobfs::BlobFsReaderBuilder;
use scrutiny_utils::io::ReadSeek;
use scrutiny_utils::io::TryClonableBufReaderFile;
use std::cell::RefCell;
use std::error;
use std::fs;
use std::fs::File;
use std::io;
use std::io::BufReader;
use std::path::Path;
use std::path::PathBuf;
use std::rc::Rc;
use std::str::FromStr;
use thiserror::Error;

pub(crate) trait BlobSet {
    /// Concrete type for the content-addressed hash used to identify blobs.
    type Hash: HashApi;

    /// Concrete type for individual blobs.
    type Blob: BlobApi<DataSource = Self::DataSource>;

    /// Concrete type for data sources of blobs.
    type DataSource: DataSourceApi;

    /// Concrete type for errors returned by `BlobSet::blob`.
    type Error: error::Error;

    /// Iterate over blobs in this set.
    fn iter(&self) -> Box<dyn Iterator<Item = Self::Blob>>;

    /// Access a particular blob in this set.
    fn blob(&self, hash: Self::Hash) -> Result<Self::Blob, Self::Error>;

    /// Iterate over this blob set's data sources.
    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>>;
}

// TODO(fxbug.dev/112505): `scrutiny_x` should contain its own blobfs archive reader pattern based
// on an API owned by the storage team. The implementation below uses a bespoke blobfs archive
// parser that is not owned by the storage team.

/// [`BlobSet`] implementation backed by a blobfs archive.
#[derive(Clone)]
pub(crate) struct BlobFsBlobSet(Rc<BlobFsBlobSetData>);

impl BlobFsBlobSet {
    /// Gets the hashes in this blobfs archive.
    pub fn blob_ids(&self) -> &Vec<Hash> {
        &self.0.blob_ids
    }

    /// Gets the path to this blobfs archive.
    pub fn archive_path(&self) -> &PathBuf {
        &self.0.archive_path
    }

    /// Gets the shared blobfs archive reader for this blobfs archive.
    pub fn reader(&self) -> &Rc<RefCell<BlobFsReader<TryClonableBufReaderFile>>> {
        &self.0.reader
    }

    /// # Panics
    ///
    /// Concurrent invocations of `BlobFsBlobSet::reader_seeker` is not supported.
    pub fn reader_seeker(&self, hash: &Hash) -> Result<Box<dyn ReadSeek>, BlobFsError> {
        let path = PathBuf::from(format!("{}", hash));
        let mut blobfs_reader = self.reader().borrow_mut();
        blobfs_reader.open(&path).map_err(|error| BlobFsError::BlobFsError {
            archive_path: self.archive_path().clone(),
            error,
        })
    }
}

/// Data stored in [`BlobFsBlobSet`] behind a shared reference.
#[derive(Clone)]
struct BlobFsBlobSetData {
    archive_path: PathBuf,
    blob_ids: Vec<Hash>,
    reader: Rc<RefCell<BlobFsReader<TryClonableBufReaderFile>>>,
}

#[derive(Debug, Error)]
pub(crate) enum BlobFsError {
    #[error("Blob not found: {hash}, in blobfs archive: {archive_path}")]
    BlobNotFound { archive_path: PathBuf, hash: Hash },
    #[error("Error reading from blobfs archive: {archive_path}: {error}")]
    BlobFsError { archive_path: PathBuf, error: anyhow::Error },
}

#[derive(Debug, Error)]
pub(crate) enum ParseHashPathError {
    #[error("Blob fuchsia merkle root string path contains non-unicode characters: {path_string}")]
    NonUnicodeCharacters { path_string: String },
    #[error("Path does not contain fuchsia merkle root string: {path_string}: {fuchsia_parse_hash_error}")]
    NonFuchsiaMerkleRoot { path_string: String, fuchsia_parse_hash_error: FuchsiaParseHashError },
}

fn parse_path_as_hash<P: AsRef<Path>>(path: P) -> Result<Hash, ParseHashPathError> {
    let path_ref = path.as_ref();
    let hash_str = path_ref.to_str().ok_or_else(|| ParseHashPathError::NonUnicodeCharacters {
        path_string: path_ref.to_string_lossy().to_string(),
    })?;
    let hash = FuchsiaMerkleHash::from_str(hash_str)
        .map_err(|fuchsia_parse_hash_error| ParseHashPathError::NonFuchsiaMerkleRoot {
            path_string: path_ref.to_string_lossy().to_string(),
            fuchsia_parse_hash_error,
        })?
        .into();
    Ok(hash)
}

pub(crate) struct BlobFsIterator {
    next_blob_id_idx: usize,
    blob_set: BlobFsBlobSet,
}

/// Iterator type returned by `BlobFsBlobSet::iter`.
impl BlobFsIterator {
    /// Constructs a [`BlobFsIterator`] that will iterate over all blobs in `blob_set`.
    pub fn new(blob_set: BlobFsBlobSet) -> Self {
        Self { next_blob_id_idx: 0, blob_set }
    }
}

impl Iterator for BlobFsIterator {
    type Item = BlobFsBlob;

    fn next(&mut self) -> Option<Self::Item> {
        let blob_ids = self.blob_set.blob_ids();
        if self.next_blob_id_idx >= blob_ids.len() {
            return None;
        }

        let blob_id_idx = self.next_blob_id_idx;
        self.next_blob_id_idx += 1;

        let hash = blob_ids[blob_id_idx].clone();
        let blob_set = self.blob_set.clone();
        Some(BlobFsBlob::new(hash, blob_set))
    }
}

impl BlobSet for BlobFsBlobSet {
    type Hash = Hash;
    type Blob = BlobFsBlob;
    type DataSource = BlobFsArchive;
    type Error = BlobFsError;

    fn iter(&self) -> Box<dyn Iterator<Item = Self::Blob>> {
        // Note: `self.clone()` is a cheap increment of a reference count.
        Box::new(BlobFsIterator::new(self.clone()))
    }

    fn blob(&self, hash: Self::Hash) -> Result<Self::Blob, Self::Error> {
        if self.blob_ids().contains(&hash) {
            // Note: `self.clone()` is a cheap increment of a reference count.
            Ok(BlobFsBlob::new(hash, self.clone()))
        } else {
            Err(BlobFsError::BlobNotFound { archive_path: self.archive_path().clone(), hash })
        }
    }

    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>> {
        Box::new([BlobFsArchive::new(self.archive_path().clone())].into_iter())
    }
}

pub(crate) struct BlobFsBlob {
    hash: Hash,
    blob_set: BlobFsBlobSet,
}

impl BlobFsBlob {
    /// Constructs a [`BlobFsBlob`] for accessing the blob with hash `hash` from `blob_set`.
    pub fn new(hash: Hash, blob_set: BlobFsBlobSet) -> Self {
        Self { hash, blob_set }
    }
}

impl BlobApi for BlobFsBlob {
    type Hash = Hash;
    type ReaderSeeker = Box<dyn ReadSeek>;
    type DataSource = BlobFsArchive;
    type Error = BlobFsError;

    fn hash(&self) -> Self::Hash {
        self.hash.clone()
    }

    /// # Panics
    ///
    /// Delegates to `BlobFsBlobSet::reader_seeker`.
    fn reader_seeker(&self) -> Result<Self::ReaderSeeker, Self::Error> {
        self.blob_set.reader_seeker(&self.hash)
    }

    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>> {
        self.blob_set.data_sources()
    }
}

/// Errors that may be emitted by `BlobFsBlobSetBuilder::build`.
#[derive(Debug, Error)]
pub(crate) enum BlobFsBlobSetBuilderError {
    #[error("Attempt to build blobfs set without specifying an archive path")]
    MissingArchivePath,
    #[error("Failed to open blobfs archive: {0}")]
    OpenError(#[from] io::Error),
    #[error("Error preparing blobfs archive reader: {0}")]
    BlobFsError(#[from] anyhow::Error),
    #[error("Failed to process blobfs path: {0}")]
    PathError(#[from] ParseHashPathError),
}

/// Builder pattern for constructing [`BlobFsBlobSet`].
pub(crate) struct BlobFsBlobSetBuilder {
    archive_path: Option<PathBuf>,
}

impl BlobFsBlobSetBuilder {
    /// Instantiates empty builder.
    pub fn new() -> Self {
        Self { archive_path: None }
    }

    /// Associates builder with blobfs archive specified by `archive_path`.
    pub fn archive<P: AsRef<Path>>(mut self, archive_path: P) -> Self {
        self.archive_path = Some(archive_path.as_ref().to_path_buf());
        self
    }

    /// Builds a [`BlobFsBlobSet`] based on data accumulated in builder.
    pub fn build(self) -> Result<BlobFsBlobSet, BlobFsBlobSetBuilderError> {
        if self.archive_path.is_none() {
            return Err(BlobFsBlobSetBuilderError::MissingArchivePath);
        }

        let archive_path = self.archive_path.unwrap();
        let reader_seeker =
            TryClonableBufReaderFile::from(BufReader::new(File::open(&archive_path)?));
        let blobfs_reader = BlobFsReaderBuilder::new().archive(reader_seeker)?.build()?;
        let blob_ids = blobfs_reader
            .blob_paths()
            .map(|blob_path| parse_path_as_hash(blob_path).map_err(BlobFsBlobSetBuilderError::from))
            .collect::<Result<Vec<Hash>, BlobFsBlobSetBuilderError>>()?;
        Ok(BlobFsBlobSet(Rc::new(BlobFsBlobSetData {
            archive_path,
            blob_ids,
            reader: Rc::new(RefCell::new(blobfs_reader)),
        })))
    }
}

/// [`BlobSet`] implementation backed by a directory of blobs named after their Fuchsia merkle root
/// hashes. This object wraps a reference-counted pointer to its state, which makes it cheap to
/// clone. Note that objects of this type are constructed via a builder that that is responsible
/// for pre-computing the identity of blobs that can be loaded from the underlying directory.
#[derive(Clone)]
pub(crate) struct BlobDirectoryBlobSet(Rc<BlobDirectoryBlobSetData>);

impl BlobDirectoryBlobSet {
    fn new(directory: PathBuf, blob_ids: Vec<Hash>) -> Self {
        Self(Rc::new(BlobDirectoryBlobSetData::new(directory, blob_ids)))
    }

    /// Gets the path to this blobs directory.
    pub fn directory(&self) -> &PathBuf {
        self.0.directory()
    }

    /// Gets the hashes in this blobs directory.
    pub fn blob_ids(&self) -> &Vec<Hash> {
        self.0.blob_ids()
    }
}

/// Internal state of a [`BlobDirectoryBlobSet`].
pub(crate) struct BlobDirectoryBlobSetData {
    /// Path to the underlying directory on the local filesystem.
    directory: PathBuf,

    /// Set of blob identities (content hashes) found in the underlying directory.
    blob_ids: Vec<Hash>,
}

impl BlobDirectoryBlobSetData {
    fn new(directory: PathBuf, blob_ids: Vec<Hash>) -> Self {
        Self { directory, blob_ids }
    }

    fn directory(&self) -> &PathBuf {
        &self.directory
    }

    fn blob_ids(&self) -> &Vec<Hash> {
        &self.blob_ids
    }
}

/// Errors that can be encountered accessing blobs via a [`BlobDirectoryBlobSet`].
#[derive(Debug, Error)]
pub(crate) enum BlobDirectoryError {
    #[error("Blob not found: {hash}, in blob directory: {directory}")]
    BlobNotFound { directory: PathBuf, hash: Hash },
    #[error("Error reading from blob directory: {directory}: {error}")]
    IoError { directory: PathBuf, error: io::Error },
}

/// [`Iterator`] implementation for for blobs backed by a [`BlobDirectoryBlobSet`].
pub(crate) struct BlobDirectoryIterator {
    next_blob_id_idx: usize,
    blob_set: BlobDirectoryBlobSet,
}

impl BlobDirectoryIterator {
    /// Constructs a [`BlobDirectoryIterator`] that will iterate over all blobs in `blob_set`.
    pub fn new(blob_set: BlobDirectoryBlobSet) -> Self {
        Self { next_blob_id_idx: 0, blob_set }
    }
}

impl Iterator for BlobDirectoryIterator {
    type Item = FileBlob;

    fn next(&mut self) -> Option<Self::Item> {
        let blob_ids = self.blob_set.blob_ids();
        if self.next_blob_id_idx >= blob_ids.len() {
            return None;
        }

        let blob_id_idx = self.next_blob_id_idx;
        self.next_blob_id_idx += 1;

        let hash = blob_ids[blob_id_idx].clone();
        let blob_set = self.blob_set.clone();
        Some(FileBlob::new(hash, blob_set))
    }
}

impl BlobSet for BlobDirectoryBlobSet {
    type Hash = Hash;
    type Blob = FileBlob;
    type DataSource = BlobDirectory;
    type Error = BlobDirectoryError;

    fn iter(&self) -> Box<dyn Iterator<Item = Self::Blob>> {
        Box::new(BlobDirectoryIterator::new(self.clone()))
    }

    fn blob(&self, hash: Self::Hash) -> Result<Self::Blob, Self::Error> {
        if self.blob_ids().contains(&hash) {
            Ok(FileBlob::new(hash, self.clone()))
        } else {
            Err(BlobDirectoryError::BlobNotFound { directory: self.directory().clone(), hash })
        }
    }

    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>> {
        Box::new([BlobDirectory::new(self.directory().clone())].into_iter())
    }
}

/// [`Blob`] implementation for a blobs backed by a [`BlobDirectoryBlobSet`].
pub(crate) struct FileBlob {
    hash: Hash,
    blob_set: BlobDirectoryBlobSet,
}

impl FileBlob {
    fn new(hash: Hash, blob_set: BlobDirectoryBlobSet) -> Self {
        Self { hash, blob_set }
    }
}

impl BlobApi for FileBlob {
    type Hash = Hash;
    type ReaderSeeker = Box<dyn ReadSeek>;
    type DataSource = BlobDirectory;
    type Error = BlobDirectoryError;

    fn hash(&self) -> Self::Hash {
        self.hash.clone()
    }

    fn reader_seeker(&self) -> Result<Self::ReaderSeeker, Self::Error> {
        let hash = format!("{}", self.hash());
        let path = self.blob_set.directory().join(&hash);
        Ok(Box::new(fs::File::open(&path).map_err(|error| BlobDirectoryError::IoError {
            directory: self.blob_set.directory().clone(),
            error,
        })?))
    }

    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>> {
        self.blob_set.data_sources()
    }
}

/// Errors that may be emitted by `BlobFsBlobSetBuilder::build`.
#[derive(Debug, Error)]
pub(crate) enum BlobDirectoryBlobSetBuilderError {
    #[error("Attempt to build blob directory blob set without specifying a directory")]
    MissingDirectory,
    #[error("Failed to list files in blob directory: {0}")]
    ListError(io::Error),
    #[error("Failed to stat directory entry: {0}")]
    DirEntryError(io::Error),
    #[error("Failed to losslessly convert file name to string: {0}")]
    PathStringError(String),
    #[error("Failed to process blob path: {0}")]
    PathError(#[from] ParseHashPathError),
    #[error("Failed to read blob from blob directory: {0}")]
    ReadBlobError(io::Error),
    #[error("Hash mismatch: hash from path: {hash_from_path}; computed hash: {computed_hash}")]
    HashMismatch { hash_from_path: Hash, computed_hash: Hash },
}

/// Builder pattern for constructing [`BlobDirectoryBlobSet`].
pub(crate) struct BlobDirectoryBlobSetBuilder {
    directory: Option<PathBuf>,
}

impl BlobDirectoryBlobSetBuilder {
    /// Instantiates empty builder.
    pub fn new() -> Self {
        Self { directory: None }
    }

    /// Associates builder with directory specified by `directory`.
    pub fn directory<P: AsRef<Path>>(mut self, directory: P) -> Self {
        self.directory = Some(directory.as_ref().to_path_buf());
        self
    }

    /// Builds a [`BlobDirectoryBlobSet`] based on data accumulated in builder. This builder
    /// enumerates entries in the underlying directory and ensures that:
    ///
    /// - All entries are files with content lowercase hash hex strings;
    /// - File names match content hash of file contents.
    ///
    /// After these checks are performed, the set of verified hashes is passed to a
    /// [`BlobDirectoryBlobSet`] constructor.
    pub fn build(self) -> Result<BlobDirectoryBlobSet, BlobDirectoryBlobSetBuilderError> {
        if self.directory.is_none() {
            return Err(BlobDirectoryBlobSetBuilderError::MissingDirectory);
        }

        let directory = self.directory.unwrap();
        let paths =
            fs::read_dir(&directory).map_err(BlobDirectoryBlobSetBuilderError::ListError)?;
        let mut blob_ids = vec![];
        for dir_entry_result in paths {
            let dir_entry =
                dir_entry_result.map_err(BlobDirectoryBlobSetBuilderError::DirEntryError)?;
            let file_name = dir_entry.file_name();
            let file_name = file_name.to_str().ok_or_else(|| {
                BlobDirectoryBlobSetBuilderError::PathStringError(String::from(
                    file_name.to_string_lossy(),
                ))
            })?;
            let hash_from_path = parse_path_as_hash(file_name)
                .map_err(BlobDirectoryBlobSetBuilderError::PathError)?;
            let mut blob_file = fs::File::open(directory.join(file_name))
                .map_err(BlobDirectoryBlobSetBuilderError::ReadBlobError)?;
            let fuchsia_hash = MerkleTree::from_reader(&mut blob_file)
                .map_err(BlobDirectoryBlobSetBuilderError::ReadBlobError)?
                .root();
            let computed_hash = Hash::from(fuchsia_hash);
            if hash_from_path != computed_hash {
                return Err(BlobDirectoryBlobSetBuilderError::HashMismatch {
                    hash_from_path,
                    computed_hash,
                });
            }
            blob_ids.push(computed_hash);
        }

        Ok(BlobDirectoryBlobSet::new(directory, blob_ids))
    }
}

#[cfg(test)]
mod tests {
    use super::BlobDirectoryBlobSetBuilder;
    use super::BlobDirectoryBlobSetBuilderError;
    use super::BlobDirectoryError;
    use super::BlobFsBlobSetBuilder;
    use super::BlobSet;
    use super::ParseHashPathError;
    use crate::api::Blob as BlobApi;
    use crate::hash::Hash;
    use fuchsia_hash::HASH_SIZE as FUCHSIA_HASH_SIZE;
    use fuchsia_merkle::Hash as FuchsiaMerkleHash;
    use fuchsia_merkle::MerkleTree;
    use maplit::hashmap;
    use std::fs;
    use std::io::Read;
    use std::io::Write;
    use tempfile::tempdir;

    macro_rules! fuchsia_hash {
        ($bytes:expr) => {
            MerkleTree::from_reader($bytes).unwrap().root()
        };
    }

    macro_rules! assert_blob_is {
        ($blob_set:expr, $blob:expr, $bytes:expr) => {
            let blob_fuchsia_hash = fuchsia_hash!($bytes);
            let blob_hash = Hash::from(blob_fuchsia_hash);
            assert_eq!(blob_hash, $blob.hash());
            let expected_data_sources: Vec<_> = $blob_set.data_sources().collect();
            let actual_data_sources: Vec<_> = $blob.data_sources().collect();
            assert_eq!(expected_data_sources, actual_data_sources);
            let mut blob_reader_seeker = $blob.reader_seeker().unwrap();
            let mut blob_contents = vec![];
            blob_reader_seeker.read_to_end(&mut blob_contents).unwrap();
            assert_eq!($bytes, blob_contents.as_slice());
        };
    }

    macro_rules! assert_blob_set_contains {
        ($blob_set:expr, $bytes:expr) => {
            let blob_fuchsia_hash = fuchsia_hash!($bytes);
            let blob_hash = Hash::from(blob_fuchsia_hash);
            let found_blob = $blob_set.blob(blob_hash.clone()).unwrap();
            assert_blob_is!($blob_set, found_blob, $bytes);
        };
    }

    macro_rules! mk_temp_dir {
        ($file_hash_map:expr) => {{
            let temp_dir = tempdir().unwrap();
            let dir_path = temp_dir.path();
            for (name, contents) in $file_hash_map.into_iter() {
                let path = dir_path.join(format!("{}", &name));
                let mut file = fs::File::create(&path).unwrap();
                file.write_all(&contents).unwrap();
            }
            temp_dir
        }};
    }

    #[fuchsia::test]
    fn empty_blobs_dir() {
        let temp_dir = tempdir().unwrap();
        BlobDirectoryBlobSetBuilder::new().directory(temp_dir.path()).build().unwrap();
    }

    #[fuchsia::test]
    fn single_blob_dir() {
        let blob_data = "Hello, World!";

        // Target directory contains one well-formed blob entry.
        let temp_dir = mk_temp_dir!(hashmap! {
            fuchsia_hash!(blob_data.as_bytes()) => blob_data.as_bytes(),
        });
        let blob_set =
            BlobDirectoryBlobSetBuilder::new().directory(temp_dir.path()).build().unwrap();

        let hash_not_in_set = Hash::from(FuchsiaMerkleHash::from([0u8; FUCHSIA_HASH_SIZE]));

        // Check error contents on "failed to find blob" case.
        let missing_blob = blob_set.blob(hash_not_in_set.clone()).err().unwrap();
        match missing_blob {
            BlobDirectoryError::BlobNotFound { directory, hash } => {
                assert_eq!(temp_dir.path().to_path_buf(), directory);
                assert_eq!(hash_not_in_set, hash);
            }
            _ => {
                assert!(false, "Expected BlobNotFound error, but got {:?}", missing_blob);
            }
        }

        // Check `BlobSet` and `Blob` APIs for single blob in blob set.
        assert_blob_set_contains!(blob_set, blob_data.as_bytes());

        // Check that `BlobSet::iter` yields the expected single well-formed `Blob`.
        let blobs: Vec<_> = blob_set.iter().collect();
        assert_eq!(1, blobs.len());
        let single_blob = &blobs[0];
        assert_blob_is!(blob_set, single_blob, blob_data.as_bytes());
    }

    #[fuchsia::test]
    fn multi_blob_dir() {
        let blob_data = vec!["Hello, World!", "Hello, Universe!"];

        // Target directory contains two well-formed blob entries.
        let temp_dir_map = hashmap! {
            fuchsia_hash!(blob_data[0].as_bytes()) => blob_data[0].as_bytes(),
            fuchsia_hash!(blob_data[1].as_bytes()) => blob_data[1].as_bytes(),
        };
        let temp_dir = mk_temp_dir!(&temp_dir_map);
        let blob_set =
            BlobDirectoryBlobSetBuilder::new().directory(temp_dir.path()).build().unwrap();

        let hash_not_in_set = Hash::from(FuchsiaMerkleHash::from([0u8; FUCHSIA_HASH_SIZE]));

        // Check error contents on "failed to find blob" case.
        let missing_blob = blob_set.blob(hash_not_in_set.clone()).err().unwrap();
        match missing_blob {
            BlobDirectoryError::BlobNotFound { directory, hash } => {
                assert_eq!(temp_dir.path().to_path_buf(), directory);
                assert_eq!(hash_not_in_set, hash);
            }
            _ => {
                assert!(false, "Expected BlobNotFound error, but got {:?}", missing_blob);
            }
        }

        // Check `BlobSet` and `Blob` APIs for two blobs in blob set.
        assert_blob_set_contains!(blob_set, blob_data[0].as_bytes());
        assert_blob_set_contains!(blob_set, blob_data[1].as_bytes());

        // Check that `BlobSet::iter` yields the expected two well-formed `Blob`.
        let blobs: Vec<_> = blob_set.iter().collect();
        assert_eq!(2, blobs.len());
        for blob in blobs {
            let blob_contents = *temp_dir_map.get(&blob.hash().into()).unwrap();
            assert_blob_is!(blob_set, blob, blob_contents);
        }
    }

    #[fuchsia::test]
    fn blob_dir_builder_incomplete() {
        match BlobDirectoryBlobSetBuilder::new().build().err().unwrap() {
            BlobDirectoryBlobSetBuilderError::MissingDirectory => {}
            err => {
                assert!(false, "Expected MissingDirectory error, but got {:?}", err);
            }
        };
    }

    #[fuchsia::test]
    fn blob_dir_directory_does_not_exist() {
        match BlobDirectoryBlobSetBuilder::new()
            .directory("/this/directory/definitely/does/not/exist")
            .build()
            .err()
            .unwrap()
        {
            BlobDirectoryBlobSetBuilderError::ListError(_) => {}
            err => {
                assert!(false, "Expected ListError error, but got {:?}", err);
            }
        };
    }

    #[fuchsia::test]
    fn blob_dir_invalid_path_subdir() {
        let temp_dir = tempdir().unwrap();
        let dir_path = temp_dir.path();

        let subdir_path = dir_path.join("subdir");
        fs::create_dir(subdir_path).unwrap();

        match BlobDirectoryBlobSetBuilder::new().directory(dir_path).build().err().unwrap() {
            BlobDirectoryBlobSetBuilderError::PathError(
                ParseHashPathError::NonFuchsiaMerkleRoot { path_string, .. },
            ) => {
                assert_eq!("subdir", &path_string);
            }
            err => {
                assert!(false, "Expected ReadBlobError error, but got {:?}", err);
            }
        };
    }

    #[fuchsia::test]
    fn blob_dir_valid_path_invalid_subdir() {
        let temp_dir = tempdir().unwrap();
        let dir_path = temp_dir.path();

        let valid_merkle_root = format!("{}", fuchsia_hash!("Hello, World!".as_bytes()));
        let subdir_path = dir_path.join(&valid_merkle_root);
        fs::create_dir(subdir_path).unwrap();

        match BlobDirectoryBlobSetBuilder::new().directory(dir_path).build().err().unwrap() {
            BlobDirectoryBlobSetBuilderError::ReadBlobError(_) => {}
            err => {
                assert!(false, "Expected ReadBlobError error, but got {:?}", err);
            }
        };
    }

    #[fuchsia::test]
    fn blob_dir_wrong_data() {
        let world_str = "Hello, World!";
        let universe_str = "Hello, Universe!";
        let world_bytes = world_str.as_bytes();
        let universe_bytes = universe_str.as_bytes();
        let world_hash = fuchsia_hash!(world_bytes);
        let universe_hash = fuchsia_hash!(universe_bytes);

        // Target directory contains a malformed blob entry: hello-world file-name-as-hash
        // contains hello-universe bytes.
        let temp_dir_map = hashmap! {
            world_hash => universe_bytes,
        };
        let temp_dir = mk_temp_dir!(&temp_dir_map);

        match BlobDirectoryBlobSetBuilder::new().directory(temp_dir.path()).build().err().unwrap() {
            BlobDirectoryBlobSetBuilderError::HashMismatch { hash_from_path, computed_hash } => {
                let (world_hash, universe_hash): (Hash, Hash) =
                    (world_hash.into(), universe_hash.into());
                assert_eq!(world_hash, hash_from_path);
                assert_eq!(universe_hash, computed_hash);
            }
            err => {
                assert!(false, "Expected HashMismatch error, but got {:?}", err);
            }
        };
    }

    // TODO(fxbug.dev/116719): Below is a vacuous test that exercise `super::BlobFs*` code. When
    // this code is updated to use a stable storage API, these tests should be updated to be
    // meaningful.
    #[fuchsia::test]
    fn exercise_blobfs_blob_set_api() {
        let _ = BlobFsBlobSetBuilder::new().archive("/some/path").build();
    }
}

#[cfg(test)]
pub mod fake {
    use super::BlobSet as BlobSetApi;
    use crate::api::Blob as BlobApi;
    use crate::api::Hash as HashApi;
    use crate::data_source::fake::DataSource;
    use crate::hash::test::HashGenerator;
    use std::collections::HashMap;
    use std::hash;
    use std::io::Cursor;
    use thiserror::Error;

    #[derive(Clone, Debug, Eq, Error, PartialEq)]
    pub(crate) enum BlobError {
        #[error("fake blob error")]
        BlobError,
    }

    #[derive(Clone, Debug, Eq, PartialEq)]
    pub(crate) struct Blob<H: HashApi> {
        hash: H,
        blob: Result<Cursor<Vec<u8>>, BlobError>,
        data_sources: Vec<DataSource>,
    }

    impl<H: Default + HashApi> Default for Blob<H> {
        fn default() -> Self {
            Self {
                hash: H::default(),
                blob: Ok(Cursor::new(vec![])),
                data_sources: vec![DataSource::default()],
            }
        }
    }

    impl<H: HashApi> hash::Hash for Blob<H> {
        fn hash<S>(&self, state: &mut S)
        where
            S: hash::Hasher,
        {
            self.hash.hash(state)
        }
    }

    #[derive(Clone, Debug, Eq, Error, PartialEq)]
    pub(crate) enum BlobSetError {
        #[error("fake blob set error: blob not found")]
        BlobNotFound,
        #[error("fake blob set error: {0}")]
        BlobError(#[from] BlobError),
    }

    impl<H: HashApi> Blob<H> {
        pub fn new(hash: H, blob: Vec<u8>) -> Self {
            Self { hash, blob: Ok(Cursor::new(blob)), data_sources: vec![DataSource::default()] }
        }

        pub fn new_error(hash: H) -> Self {
            Self {
                hash,
                blob: Err(BlobError::BlobError),
                data_sources: vec![DataSource::default()],
            }
        }

        pub fn new_with_data_sources(
            hash: H,
            blob: Vec<u8>,
            data_sources: Vec<DataSource>,
        ) -> Self {
            Self { hash, blob: Ok(Cursor::new(blob)), data_sources }
        }

        pub fn new_error_with_data_sources(hash: H, data_sources: Vec<DataSource>) -> Self {
            Self { hash, blob: Err(BlobError::BlobError), data_sources: data_sources }
        }
    }

    impl<H: HashApi> BlobApi for Blob<H> {
        type Hash = H;
        type ReaderSeeker = Cursor<Vec<u8>>;
        type DataSource = DataSource;
        type Error = BlobError;

        fn hash(&self) -> Self::Hash {
            self.hash.clone()
        }

        fn reader_seeker(&self) -> Result<Self::ReaderSeeker, Self::Error> {
            self.blob.clone()
        }

        fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>> {
            Box::new(self.data_sources.clone().into_iter())
        }
    }

    pub(crate) struct BlobSet<H: HashApi> {
        blobs: HashMap<H, Result<Blob<H>, BlobError>>,
    }

    impl<H: HashApi> BlobSet<H> {
        pub fn from_hash_map(blobs: HashMap<H, Result<Blob<H>, BlobError>>) -> Self {
            Self { blobs }
        }

        pub fn into_hash_map(self) -> HashMap<H, Result<Blob<H>, BlobError>> {
            self.blobs
        }
    }

    impl<H: HashApi + 'static> BlobSetApi for BlobSet<H> {
        type Blob = Blob<H>;
        type DataSource = DataSource;
        type Hash = H;
        type Error = BlobSetError;

        fn iter(&self) -> Box<dyn Iterator<Item = Self::Blob>> {
            Box::new(
                self.blobs
                    .values()
                    .filter_map(|blob_or_err| blob_or_err.as_ref().ok())
                    .map(|blob| blob.clone())
                    .collect::<Vec<Self::Blob>>()
                    .into_iter(),
            )
        }

        fn blob(&self, hash: Self::Hash) -> Result<Self::Blob, Self::Error> {
            match self.blobs.get(&hash) {
                Some(blob_or_err) => blob_or_err.clone().map_err(BlobSetError::from),
                None => Err(BlobSetError::BlobNotFound),
            }
        }

        fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>> {
            Box::new(vec![DataSource].into_iter())
        }
    }

    pub(crate) struct BlobSetBuilder<H: HashApi + HashGenerator> {
        next_id: H,
        blobs: HashMap<H, Result<Blob<H>, BlobError>>,
    }

    impl<H: HashApi + HashGenerator> BlobSetBuilder<H> {
        pub fn new() -> Self {
            Self { next_id: H::default(), blobs: HashMap::new() }
        }

        pub fn new_with_start_id(start_id: H) -> Self {
            Self { next_id: start_id, blobs: HashMap::new() }
        }

        pub fn blob(mut self, blob: Vec<u8>) -> (Self, H) {
            if self.blobs.contains_key(&self.next_id) {
                panic!("Duplicate ID inserting contents into fake blob set: {}", self.next_id)
            }
            let id = self.next_id.clone();
            self.next_id = self.next_id.next();
            self.blobs.insert(id.clone(), Ok(Blob::new(id.clone(), blob)));
            (self, id)
        }

        pub fn error(mut self, err: BlobError) -> (Self, H) {
            if self.blobs.contains_key(&self.next_id) {
                panic!("Duplicate ID inserting error into fake blob set: {}", self.next_id)
            }
            let id = self.next_id.clone();
            self.next_id = self.next_id.next();
            self.blobs.insert(id.clone(), Err(err));
            (self, id)
        }

        pub fn hash_blob(mut self, hash: H, blob: Vec<u8>) -> Self {
            if self.blobs.contains_key(&hash) {
                panic!("Duplicate ID inserting contents with predefined hash: {}", hash)
            }
            self.blobs.insert(hash.clone(), Ok(Blob::new(hash, blob)));
            self
        }

        pub fn hash_error(mut self, hash: H, err: BlobError) -> Self {
            if self.blobs.contains_key(&hash) {
                panic!("Duplicate ID inserting error with predefined hash: {}", hash)
            }
            self.blobs.insert(hash, Err(err));
            self
        }

        pub fn build(self) -> (BlobSet<H>, H) {
            (BlobSet::from_hash_map(self.blobs), self.next_id)
        }
    }

    mod test {
        use super::Blob;
        use super::BlobError;
        use super::BlobSet;
        use super::BlobSetBuilder;
        use super::BlobSetError;
        use crate::api::Blob as BlobApi;
        use crate::blob::BlobSet as BlobSetApi;
        use crate::data_source::fake::DataSource;
        use crate::hash::fake::Hash;
        use maplit::hashmap;
        use maplit::hashset;

        #[fuchsia::test]
        fn test_blob_new() {
            let (a, b, c) = (Blob::new(0, vec![]), Blob::new(0, vec![]), Blob::new(1, vec![]));
            assert_eq!(a.hash(), b.hash());
            assert!(a.hash() != c.hash());
            let v = vec![a, b, c];
            for x in v.into_iter() {
                assert!(x.reader_seeker().is_ok());
                let ds = x.data_sources().collect::<Vec<DataSource>>();
                assert_eq!(ds.len(), 1);
                assert_eq!(ds[0], DataSource);
            }
        }

        #[fuchsia::test]
        fn test_blob_new_error() {
            let (a, b, c) = (Blob::new_error(0), Blob::new_error(0), Blob::new_error(1));
            assert_eq!(a.hash(), b.hash());
            assert!(a.hash() != c.hash());
            let v = vec![a, b, c];
            for x in v.into_iter() {
                assert_eq!(x.reader_seeker(), Err(BlobError::BlobError));
                let ds = x.data_sources().collect::<Vec<DataSource>>();
                assert_eq!(ds.len(), 1);
                assert_eq!(ds[0], DataSource);
            }
        }

        #[fuchsia::test]
        fn test_blob_new_with_data_sources() {
            let ds_a = vec![];
            let ds_b = vec![DataSource];
            let ds_c = vec![DataSource, DataSource];
            let (a, b, c) = (
                Blob::new_with_data_sources(0, vec![], ds_a.clone()),
                Blob::new_with_data_sources(0, vec![], ds_b.clone()),
                Blob::new_with_data_sources(1, vec![], ds_c.clone()),
            );
            assert_eq!(a.hash(), b.hash());
            assert!(a.hash() != c.hash());

            let ds = a.data_sources().collect::<Vec<DataSource>>();
            assert_eq!(ds, ds_a);

            let ds = b.data_sources().collect::<Vec<DataSource>>();
            assert_eq!(ds, ds_b);

            let ds = c.data_sources().collect::<Vec<DataSource>>();
            assert_eq!(ds, ds_c);

            let v = vec![a, b, c];
            for x in v.iter() {
                assert!(x.reader_seeker().is_ok());
            }
        }

        #[fuchsia::test]
        fn test_blob_new_with_error_with_data_sources() {
            let ds_a = vec![];
            let ds_b = vec![DataSource];
            let ds_c = vec![DataSource, DataSource];
            let (a, b, c) = (
                Blob::new_error_with_data_sources(0, ds_a.clone()),
                Blob::new_error_with_data_sources(0, ds_b.clone()),
                Blob::new_error_with_data_sources(1, ds_c.clone()),
            );
            assert_eq!(a.hash(), b.hash());
            assert!(a.hash() != c.hash());

            let ds = a.data_sources().collect::<Vec<DataSource>>();
            assert_eq!(ds, ds_a);

            let ds = b.data_sources().collect::<Vec<DataSource>>();
            assert_eq!(ds, ds_b);

            let ds = c.data_sources().collect::<Vec<DataSource>>();
            assert_eq!(ds, ds_c);

            let v = vec![a, b, c];
            for x in v.iter() {
                assert_eq!(x.reader_seeker(), Err(BlobError::BlobError));
            }
        }

        #[fuchsia::test]
        fn test_blob_set_from_hash_map() {
            let blob_set = BlobSet::from_hash_map(hashmap! {
                0 => Ok(Blob::new(0, vec![])),
                1 => Err(BlobError::BlobError.into()),

                // `from_hash_map()` can be used to simulate a bad state where key != Blob.hash.
                2 => Ok(Blob::new(7, vec![])),
            });

            assert_eq!(blob_set.blob(0), Ok(Blob::new(0, vec![])));
            assert_eq!(blob_set.blob(1), Err(BlobError::BlobError.into()));
            assert_eq!(blob_set.blob(2), Ok(Blob::new(7, vec![])));
            assert_eq!(blob_set.blob(3), Err(BlobSetError::BlobNotFound));

            // Assert each member of `blobs` appears exactly once in iteration.
            let mut blobs = hashset! {
                Blob::new(0, vec![]),
                Blob::new(7, vec![]),
            };
            for blob in blob_set.iter() {
                // HashSet::remove() == true iff value was found in the set.
                assert!(blobs.remove(&blob));
            }
            assert_eq!(blobs.len(), 0);
        }

        #[fuchsia::test]
        fn test_blob_set_builder_simple() {
            let (blob_set, next_id) = BlobSetBuilder::<Hash>::new().build();
            assert_eq!(next_id, 0);
            assert_eq!(blob_set.into_hash_map(), hashmap! {});

            let (_, assigned_id) = BlobSetBuilder::<Hash>::new().blob(vec![]);
            assert_eq!(assigned_id, 0);
        }

        #[fuchsia::test]
        fn test_blob_set_builder_complex() {
            let mut hash = 7;
            let mut builder = BlobSetBuilder::new_with_start_id(hash);
            let mut data = hashmap! {};
            let blobs = vec![vec![], vec![0], vec![1]];
            let errs = vec![BlobError::BlobError];

            for blob in blobs.into_iter() {
                (builder, hash) = builder.blob(blob.clone());
                data.insert(hash, Ok(Blob::new(hash, blob)));
            }

            for err in errs.into_iter() {
                (builder, hash) = builder.error(err.clone());
                data.insert(hash, Err(err));
            }

            data.insert(1000, Ok(Blob::new(1000, vec![9])));
            builder = builder.hash_blob(1000, vec![9]);

            data.insert(2000, Err(BlobError::BlobError));
            builder = builder.hash_error(2000, BlobError::BlobError);

            let blob_set = builder.build().0;

            assert_eq!(blob_set.blob(0), Err(BlobSetError::BlobNotFound));
            assert_eq!(blob_set.blob(1), Err(BlobSetError::BlobNotFound));
            assert_eq!(blob_set.blob(3000), Err(BlobSetError::BlobNotFound));
            assert_eq!(data, blob_set.into_hash_map());
        }
    }
}
