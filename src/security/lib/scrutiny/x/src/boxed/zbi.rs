// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO: Exercise all crate-local code in tests and/or other modules.
#![allow(dead_code)]

use super::api;
use super::data_source as ds;
use super::hash::Hash;
use derivative::Derivative;
use fuchsia_merkle::MerkleTree;
use scrutiny_utils::bootfs::BootfsReader;
use scrutiny_utils::zbi::ZbiReader;
use scrutiny_utils::zbi::ZbiType;
use std::cell::RefCell;
use std::collections::HashMap;
use std::fs;
use std::io::Cursor;
use std::rc::Rc;
use thiserror::Error;

#[derive(Debug, Error)]
pub(crate) enum Error {
    #[error("failed to read zbi from {path:?}: {error}")]
    Filesystem { path: Box<dyn api::Path>, error: std::io::Error },
    #[error("failed parse zbi image from path {path:?}: {error}")]
    ParseZbi { path: Box<dyn api::Path>, error: anyhow::Error },
    #[error("expected to find exactly 1 bootfs section in zbi, but found {num_bootfs_sections} in zbi at path {path:?}")]
    BootfsSections { path: Box<dyn api::Path>, num_bootfs_sections: usize },
}

mod data_source {
    use super::super::api;
    use super::super::data_source::DataSourceInfo;

    #[derive(Clone, Debug, Eq)]
    pub(crate) struct Zbi {
        path: Box<dyn api::Path>,
    }

    impl Zbi {
        pub fn new(path: Box<dyn api::Path>) -> Self {
            Self { path }
        }
    }

    impl PartialEq for Zbi {
        fn eq(&self, other: &Self) -> bool {
            self.path.as_ref() == other.path.as_ref()
        }
    }

    impl DataSourceInfo for Zbi {
        fn kind(&self) -> api::DataSourceKind {
            api::DataSourceKind::Zbi
        }

        fn path(&self) -> Option<Box<dyn api::Path>> {
            Some(self.path.clone())
        }

        fn version(&self) -> api::DataSourceVersion {
            // TODO: Add support for exposing the zbi version.
            api::DataSourceVersion::Unknown
        }
    }
}

#[derive(Clone, Debug)]
pub(crate) struct Zbi(Rc<ZbiData>);

impl Zbi {
    pub fn new(
        mut parent_data_source: Option<ds::DataSource>,
        path: Box<dyn api::Path>,
    ) -> Result<Self, Error> {
        let buffer = fs::read(path.as_ref())
            .map_err(|error| Error::Filesystem { path: path.clone(), error })?;
        let mut zbi_reader = ZbiReader::new(buffer);
        let zbi_sections =
            zbi_reader.parse().map_err(|error| Error::ParseZbi { path: path.clone(), error })?;
        let bootfs_sections = zbi_sections
            .into_iter()
            .filter(|section| section.section_type == ZbiType::StorageBootfs)
            .collect::<Vec<_>>();
        let bootfs_section = match bootfs_sections.as_slice() {
            [section] => section,
            sections => {
                return Err(Error::BootfsSections {
                    path: path.clone(),
                    num_bootfs_sections: sections.len(),
                });
            }
        };

        let data_source = ds::DataSource::new(Box::new(data_source::Zbi::new(path)));

        if let Some(parent_data_source) = parent_data_source.as_mut() {
            parent_data_source.add_child(data_source.clone());
        }
        let bootfs_reader = Rc::new(RefCell::new(BootfsReader::new(bootfs_section.buffer.clone())));

        Ok(Self(Rc::new(ZbiData { data_source: Box::new(data_source), bootfs_reader })))
    }
}

impl api::Zbi for Zbi {
    fn bootfs(
        &self,
    ) -> Result<Box<dyn Iterator<Item = (Box<dyn api::Path>, Box<dyn api::Blob>)>>, api::ZbiError>
    {
        let mut bootfs_reader = self.0.bootfs_reader.borrow_mut();

        let mut bootfs_files = HashMap::new();
        for (path_string, bytes) in bootfs_reader
            .parse()
            .map_err(|error| api::ZbiError::ParseBootfs {
                path: self.0.data_source.path().expect("zbi path"),
                error,
            })?
            .into_iter()
        {
            let path: Box<dyn api::Path> = Box::new(path_string);
            let hash: Hash = MerkleTree::from_reader(bytes.as_slice())
                .map_err(|error| api::ZbiError::Hash { bootfs_path: path.clone(), error })?
                .root()
                .into();
            bootfs_files.insert(path, (hash, bytes));
        }

        let data_source = self.0.data_source.clone();

        Ok(Box::new(bootfs_files.into_iter().map(move |(path, (hash, bytes))| {
            let hash: Box<dyn api::Hash> = Box::new(hash);
            let blob: Box<dyn api::Blob> =
                Box::new(BootfsBlob { data_source: data_source.clone(), hash, bytes });
            (path, blob)
        })))
    }
}

#[derive(Derivative)]
#[derivative(Debug)]
struct ZbiData {
    data_source: Box<dyn api::DataSource>,
    #[derivative(Debug = "ignore")]
    bootfs_reader: Rc<RefCell<BootfsReader>>,
}

struct BootfsBlob {
    data_source: Box<dyn api::DataSource>,
    hash: Box<dyn api::Hash>,
    bytes: Vec<u8>,
}

impl api::Blob for BootfsBlob {
    fn hash(&self) -> Box<dyn api::Hash> {
        self.hash.clone()
    }

    fn reader_seeker(&self) -> Result<Box<dyn api::ReaderSeeker>, api::BlobError> {
        Ok(Box::new(Cursor::new(self.bytes.clone())))
    }

    fn data_sources(&self) -> Box<dyn Iterator<Item = Box<dyn api::DataSource>>> {
        Box::new([self.data_source.clone()].into_iter())
    }
}
