//! FUSE user-space library async version implementation.
//!
//! This is an improved rewrite of the FUSE user-space library to fully take advantage of Rust's
//! architecture.
//!
//! This library doesn't depend on `libfuse`, unless enable `unprivileged` feature, this feature
//! will support mount the filesystem without root permission by using `fusermount3` binary.
//!
//! # Features:
//!
//! - `file-lock`: enable POSIX file lock feature.
//! - `async-std-runtime`: use [async_std](https://docs.rs/async-std) runtime.
//! - `tokio-runtime`: use [tokio](https://docs.rs/tokio) runtime.
//! - `unprivileged`: allow mount filesystem without root permission by using `fusermount3`.
//!
//! # Notes:
//!
//! You must enable `async-std-runtime` or `tokio-runtime` feature.

use std::{
    convert::TryInto,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

/// re-export [`async_trait`][async_trait::async_trait].
pub use async_trait::async_trait;
pub use errno::Errno;
pub use helper::{mode_from_kind_and_perm, perm_from_mode_and_kind};
pub use mount_options::MountOptions;
use nix::sys::stat::mode_t;
use raw::abi::{
    fuse_setattr_in, FATTR_ATIME, FATTR_ATIME_NOW, FATTR_CTIME, FATTR_GID, FATTR_LOCKOWNER,
    FATTR_MODE, FATTR_MTIME, FATTR_MTIME_NOW, FATTR_SIZE, FATTR_UID,
};

mod errno;
mod helper;
mod mount_options;
pub mod notify;
pub mod path;
pub mod raw;

/// Filesystem Inode.
pub type Inode = u64;

/// pre-defined Result, the Err type is [`Errno`].
pub type Result<T> = std::result::Result<T, Errno>;

/// File types
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum FileType {
    /// Named pipe (S_IFIFO)
    NamedPipe,
    /// Character device (S_IFCHR)
    CharDevice,
    /// Block device (S_IFBLK)
    BlockDevice,
    /// Directory (S_IFDIR)
    Directory,
    /// Regular file (S_IFREG)
    RegularFile,
    /// Symbolic link (S_IFLNK)
    Symlink,
    /// Unix domain socket (S_IFSOCK)
    Socket,
}

impl From<FileType> for mode_t {
    fn from(kind: FileType) -> Self {
        match kind {
            FileType::NamedPipe => libc::S_IFIFO,
            FileType::CharDevice => libc::S_IFCHR,
            FileType::BlockDevice => libc::S_IFBLK,
            FileType::Directory => libc::S_IFDIR,
            FileType::RegularFile => libc::S_IFREG,
            FileType::Symlink => libc::S_IFLNK,
            FileType::Socket => libc::S_IFSOCK,
        }
    }
}

/// the setattr argument.
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct SetAttr {
    /// set file or directory mode.
    pub mode: Option<mode_t>,
    /// set file or directory uid.
    pub uid: Option<u32>,
    /// set file or directory gid.
    pub gid: Option<u32>,
    /// set file or directory size.
    pub size: Option<u64>,
    /// the lock_owner argument.
    pub lock_owner: Option<u64>,
    /// set file or directory atime.
    pub atime: Option<Timestamp>,
    /// set file or directory mtime.
    pub mtime: Option<Timestamp>,
    /// set file or directory ctime.
    pub ctime: Option<Timestamp>,
    #[cfg(target_os = "macos")]
    pub crtime: Option<Timestamp>,
    #[cfg(target_os = "macos")]
    pub chgtime: Option<Timestamp>,
    #[cfg(target_os = "macos")]
    pub bkuptime: Option<Timestamp>,
    #[cfg(target_os = "macos")]
    pub flags: Option<u32>,
}

/// Helper for constructing Timestamps from fuse_setattr_in, which sign-casts
/// the seconds.
macro_rules! fsai2ts {
    ( $secs: expr, $nsecs: expr) => {
        Some(Timestamp::new($secs as i64, $nsecs))
    };
}

impl From<&fuse_setattr_in> for SetAttr {
    fn from(setattr_in: &fuse_setattr_in) -> Self {
        let mut set_attr = Self::default();

        if setattr_in.valid & FATTR_MODE > 0 {
            set_attr.mode = Some(setattr_in.mode as mode_t);
        }

        if setattr_in.valid & FATTR_UID > 0 {
            set_attr.uid = Some(setattr_in.uid);
        }

        if setattr_in.valid & FATTR_GID > 0 {
            set_attr.gid = Some(setattr_in.gid);
        }

        if setattr_in.valid & FATTR_SIZE > 0 {
            set_attr.size = Some(setattr_in.size);
        }

        if setattr_in.valid & FATTR_ATIME > 0 {
            set_attr.atime = fsai2ts!(setattr_in.atime, setattr_in.atimensec);
        }

        if setattr_in.valid & FATTR_ATIME_NOW > 0 {
            set_attr.atime = Some(SystemTime::now().into());
        }

        if setattr_in.valid & FATTR_MTIME > 0 {
            set_attr.mtime = fsai2ts!(setattr_in.mtime, setattr_in.mtimensec);
        }

        if setattr_in.valid & FATTR_MTIME_NOW > 0 {
            set_attr.mtime = Some(SystemTime::now().into());
        }

        if setattr_in.valid & FATTR_LOCKOWNER > 0 {
            set_attr.lock_owner = Some(setattr_in.lock_owner);
        }

        if setattr_in.valid & FATTR_CTIME > 0 {
            set_attr.ctime = fsai2ts!(setattr_in.ctime, setattr_in.ctimensec);
        }

        set_attr
    }
}

/// A file's timestamp, according to FUSE.
///
/// Nearly the same as a `libc::timespec`, except for the width of the nsec
/// field.
// Could implement From for Duration, and/or libc::timespec, if desired
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct Timestamp {
    pub sec: i64,
    pub nsec: u32,
}

impl Timestamp {
    /// Create a new timestamp from its component parts.
    ///
    /// `nsec` should be less than 1_000_000_000.
    pub fn new(sec: i64, nsec: u32) -> Self {
        Timestamp { sec, nsec }
    }
}

impl From<SystemTime> for Timestamp {
    fn from(t: SystemTime) -> Self {
        let d = t
            .duration_since(UNIX_EPOCH)
            .unwrap_or_else(|_| Duration::from_secs(0));
        Timestamp {
            sec: d.as_secs().try_into().unwrap_or(i64::MAX),
            nsec: d.subsec_nanos(),
        }
    }
}
