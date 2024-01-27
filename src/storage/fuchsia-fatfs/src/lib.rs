// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::{filesystem::FatFilesystem, node::Node},
    anyhow::Error,
    fatfs::FsOptions,
    fidl_fuchsia_fs::{AdminRequest, AdminShutdownResponder},
    fuchsia_zircon::Status,
    std::pin::Pin,
    std::sync::Arc,
    vfs::{
        directory::{entry::DirectoryEntry, entry_container::Directory},
        execution_scope::ExecutionScope,
    },
};

mod directory;
mod file;
mod filesystem;
mod node;
mod refs;
mod types;
mod util;

pub use {directory::FatDirectory, util::fatfs_error_to_status};

#[cfg(fuzz)]
mod fuzzer;
#[cfg(fuzz)]
use fuzz::fuzz;
#[cfg(fuzz)]
#[fuzz]
fn fuzz_fatfs(fs: &[u8]) {
    fuzzer::fuzz_fatfs(fs);
}

pub use types::Disk;

/// Number of UCS-2 characters that fit in a VFAT LFN.
/// Note that FAT doesn't support the full range of Unicode characters (UCS-2 is only 16 bits),
/// and short file names can't encode the full 16-bit range of UCS-2.
/// This is the minimum possible value. For instance, a 300 byte UTF-8 string could fit inside 255
/// UCS-2 codepoints (if it had some 16 bit characters), but a 300 byte ASCII string would not fit.
pub const MAX_FILENAME_LEN: u32 = 255;

// An array used to initialize the FilesystemInfo |name| field. This just spells "fatfs" 0-padded to
// 32 bytes.
pub const FATFS_INFO_NAME: [i8; 32] = [
    0x66, 0x61, 0x74, 0x66, 0x73, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
];

pub trait RootDirectory: DirectoryEntry + Directory {}
impl<T: DirectoryEntry + Directory> RootDirectory for T {}

pub struct FatFs {
    inner: Pin<Arc<FatFilesystem>>,
    root: Arc<FatDirectory>,
}

impl FatFs {
    /// Create a new FatFs using the given ReadWriteSeek as the disk.
    pub fn new(disk: Box<dyn Disk>) -> Result<Self, Error> {
        let (inner, root) = FatFilesystem::new(disk, FsOptions::new())?;
        Ok(FatFs { inner, root })
    }

    #[cfg(test)]
    pub fn from_filesystem(inner: Pin<Arc<FatFilesystem>>, root: Arc<FatDirectory>) -> Self {
        FatFs { inner, root }
    }

    #[cfg(any(test, fuzz))]
    pub fn get_fatfs_root(&self) -> Arc<FatDirectory> {
        self.root.clone()
    }

    pub fn filesystem(&self) -> &FatFilesystem {
        return &self.inner;
    }

    pub fn is_present(&self) -> bool {
        self.inner.lock().unwrap().with_disk(|disk| disk.is_present())
    }

    /// Get the root directory of this filesystem.
    /// The caller must call close() on the returned entry when it's finished with it.
    pub fn get_root(&self) -> Result<Arc<FatDirectory>, Status> {
        // Make sure it's open.
        self.root.open_ref(&self.inner.lock().unwrap())?;
        Ok(self.root.clone())
    }

    pub async fn handle_admin(
        &self,
        scope: &ExecutionScope,
        req: AdminRequest,
    ) -> Option<AdminShutdownResponder> {
        match req {
            AdminRequest::Shutdown { responder } => {
                scope.shutdown();
                Some(responder)
            }
        }
    }

    /// Shut down the filesystem.
    pub fn shut_down(&self) -> Result<(), Status> {
        let mut fs = self.inner.lock().unwrap();
        self.root.shut_down(&fs)?;
        fs.shut_down()
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::types::{Dir, FileSystem},
        anyhow::{anyhow, Context, Error},
        fatfs::{format_volume, FormatVolumeOptions, FsOptions},
        fidl::endpoints::Proxy,
        fidl_fuchsia_io as fio,
        fuchsia_zircon::Status,
        futures::{future::BoxFuture, prelude::*},
        std::{collections::HashMap, io::Write, ops::Deref},
        vfs::{execution_scope::ExecutionScope, path::Path},
    };

    #[derive(Debug, PartialEq)]
    /// Helper class for creating a filesystem layout on a FAT disk programatically.
    pub enum TestDiskContents {
        File(String),
        Dir(HashMap<String, TestDiskContents>),
    }

    impl From<&str> for TestDiskContents {
        fn from(string: &str) -> Self {
            TestDiskContents::File(string.to_owned())
        }
    }

    impl TestDiskContents {
        /// Create a new, empty directory.
        pub fn dir() -> Self {
            TestDiskContents::Dir(HashMap::new())
        }

        /// Add a new child to this directory.
        pub fn add_child(mut self, name: &str, child: Self) -> Self {
            match &mut self {
                TestDiskContents::Dir(map) => map.insert(name.to_owned(), child),
                _ => panic!("Can't add to a file"),
            };
            self
        }

        /// Add this TestDiskContents to the given fatfs Dir
        pub fn create(&self, dir: &Dir<'_>) {
            match self {
                TestDiskContents::File(_) => {
                    panic!("Can't have the root directory be a file!");
                }
                TestDiskContents::Dir(map) => {
                    for (name, value) in map.iter() {
                        value.create_fs_structure(&name, dir);
                    }
                }
            };
        }

        fn create_fs_structure(&self, name: &str, dir: &Dir<'_>) {
            match self {
                TestDiskContents::File(content) => {
                    let mut file = dir.create_file(name).expect("Creating file to succeed");
                    file.truncate().expect("Truncate to succeed");
                    file.write_all(content.as_bytes()).expect("Write to succeed");
                }
                TestDiskContents::Dir(map) => {
                    let new_dir = dir.create_dir(name).expect("Creating directory to succeed");
                    for (name, value) in map.iter() {
                        value.create_fs_structure(&name, &new_dir);
                    }
                }
            };
        }

        pub fn verify(&self, remote: fio::NodeProxy) -> BoxFuture<'_, Result<(), Error>> {
            // Unfortunately, there is no way to verify from the server side, so we use
            // the fuchsia.io protocol to check everything is as expected.
            match self {
                TestDiskContents::File(content) => {
                    let remote = fio::FileProxy::new(remote.into_channel().unwrap());
                    let mut file_contents: Vec<u8> = Vec::with_capacity(content.len());

                    return async move {
                        loop {
                            let mut vec = remote
                                .read(content.len() as u64)
                                .await
                                .context("Read failed")?
                                .map_err(Status::from_raw)
                                .context("Read error")?;
                            if vec.len() == 0 {
                                break;
                            }
                            file_contents.append(&mut vec);
                        }

                        if file_contents.as_slice() != content.as_bytes() {
                            return Err(anyhow!(
                                "File contents mismatch: expected {}, got {}",
                                content,
                                String::from_utf8_lossy(&file_contents)
                            ));
                        }
                        Ok(())
                    }
                    .boxed();
                }
                TestDiskContents::Dir(map) => {
                    let remote = fio::DirectoryProxy::new(remote.into_channel().unwrap());
                    // TODO(simonshields): we should check that no other files exist, but
                    // GetDirents() is going to be a pain to deal with.

                    return async move {
                        for (name, value) in map.iter() {
                            let (proxy, server_end) =
                                fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
                            remote
                                .open(
                                    fio::OpenFlags::RIGHT_READABLE,
                                    fio::ModeType::empty(),
                                    name,
                                    server_end,
                                )
                                .context("Sending open failed")?;
                            value.verify(proxy).await.context(format!("Verifying {}", name))?;
                        }
                        Ok(())
                    }
                    .boxed();
                }
            }
        }
    }

    /// Helper class for creating an empty FAT-formatted VMO.
    pub struct TestFatDisk {
        fs: FileSystem,
    }

    impl TestFatDisk {
        /// Create an empty disk with size at least |size| bytes.
        pub fn empty_disk(size: u64) -> Self {
            let mut buffer: Vec<u8> = Vec::with_capacity(size as usize);
            buffer.resize(size as usize, 0);
            let cursor = std::io::Cursor::new(buffer.as_mut_slice());

            format_volume(cursor, FormatVolumeOptions::new()).expect("format volume to succeed");
            let wrapper: Box<dyn Disk> = Box::new(std::io::Cursor::new(buffer));
            TestFatDisk {
                fs: fatfs::FileSystem::new(wrapper, FsOptions::new())
                    .expect("creating FS to succeed"),
            }
        }

        /// Get the root directory (as a fatfs Dir).
        pub fn root_dir<'a>(&'a self) -> Dir<'a> {
            self.fs.root_dir()
        }

        /// Convert this TestFatDisk into a FatFs for testing against.
        pub fn into_fatfs(self) -> FatFs {
            self.fs.flush().unwrap();
            let (filesystem, root_dir) = FatFilesystem::from_filesystem(self.fs);
            FatFs::from_filesystem(filesystem, root_dir)
        }
    }

    impl Deref for TestFatDisk {
        type Target = FileSystem;

        fn deref(&self) -> &Self::Target {
            &self.fs
        }
    }

    const TEST_DISK_SIZE: u64 = 2048 << 10;

    #[fuchsia::test]
    #[ignore] // TODO(fxbug.dev/56138): Clean up tasks to prevent panic on drop in FatfsFileRef
    async fn test_create_disk() {
        let disk = TestFatDisk::empty_disk(TEST_DISK_SIZE);

        let structure = TestDiskContents::dir()
            .add_child("test", "This is a test file".into())
            .add_child("empty_folder", TestDiskContents::dir());

        structure.create(&disk.root_dir());

        let fatfs = disk.into_fatfs();
        let scope = ExecutionScope::new();
        let (proxy, remote) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
        let root = fatfs.get_root().expect("get_root OK");
        root.clone().open(scope, fio::OpenFlags::RIGHT_READABLE, Path::dot(), remote);
        root.close().expect("Close OK");

        structure.verify(proxy).await.expect("Verify succeeds");
    }

    #[fuchsia::test]
    fn test_unset_date() {
        let disk = TestFatDisk::empty_disk(TEST_DISK_SIZE);
        // FAT doesn't give the root directory a created/modified/access time,
        // so this is a good way to check that we return valid dates for a "zero" date.
        let root = disk.root_dir();
        let epoch = fatfs::DateTime {
            date: fatfs::Date { year: 1980, month: 1, day: 1 },
            time: fatfs::Time { hour: 0, min: 0, sec: 0, millis: 0 },
        };
        assert_eq!(root.created(), epoch);
        assert_eq!(root.modified(), epoch);
        assert_eq!(root.accessed(), epoch.date);
    }
}
