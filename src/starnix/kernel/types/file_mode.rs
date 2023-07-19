// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::ops;

use crate::types::{uapi, *};

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub struct FileMode(u32);

impl FileMode {
    pub const IFLNK: FileMode = FileMode(uapi::S_IFLNK);
    pub const IFREG: FileMode = FileMode(uapi::S_IFREG);
    pub const IFDIR: FileMode = FileMode(uapi::S_IFDIR);
    pub const IFCHR: FileMode = FileMode(uapi::S_IFCHR);
    pub const IFBLK: FileMode = FileMode(uapi::S_IFBLK);
    pub const IFIFO: FileMode = FileMode(uapi::S_IFIFO);
    pub const IFSOCK: FileMode = FileMode(uapi::S_IFSOCK);

    pub const ISUID: FileMode = FileMode(uapi::S_ISUID);
    pub const ISGID: FileMode = FileMode(uapi::S_ISGID);
    pub const ISVTX: FileMode = FileMode(uapi::S_ISVTX);
    pub const IRWXU: FileMode = FileMode(uapi::S_IRWXU);
    pub const IRUSR: FileMode = FileMode(uapi::S_IRUSR);
    pub const IWUSR: FileMode = FileMode(uapi::S_IWUSR);
    pub const IXUSR: FileMode = FileMode(uapi::S_IXUSR);
    pub const IRWXG: FileMode = FileMode(uapi::S_IRWXG);
    pub const IRGRP: FileMode = FileMode(uapi::S_IRGRP);
    pub const IWGRP: FileMode = FileMode(uapi::S_IWGRP);
    pub const IXGRP: FileMode = FileMode(uapi::S_IXGRP);
    pub const IRWXO: FileMode = FileMode(uapi::S_IRWXO);
    pub const IROTH: FileMode = FileMode(uapi::S_IROTH);
    pub const IWOTH: FileMode = FileMode(uapi::S_IWOTH);
    pub const IXOTH: FileMode = FileMode(uapi::S_IXOTH);

    pub const IFMT: FileMode = FileMode(uapi::S_IFMT);

    pub const DEFAULT_UMASK: FileMode = FileMode(0o022);
    pub const ALLOW_ALL: FileMode = FileMode(0o777);
    pub const PERMISSIONS: FileMode = FileMode(0o7777);
    pub const EMPTY: FileMode = FileMode(0);

    pub const fn from_bits(mask: u32) -> FileMode {
        FileMode(mask)
    }

    pub fn from_string(mask: &[u8]) -> Result<FileMode, Errno> {
        if mask[0] != b'0' {
            return error!(EINVAL);
        }
        let mask = std::str::from_utf8(mask).map_err(|_| errno!(EINVAL))?;
        let mask = u32::from_str_radix(mask, 8).map_err(|_| errno!(EINVAL))?;
        Ok(Self::from_bits(mask))
    }

    pub const fn bits(&self) -> u32 {
        self.0
    }

    pub const fn contains(&self, other: FileMode) -> bool {
        self.0 & other.0 == other.bits()
    }

    pub const fn intersects(&self, other: FileMode) -> bool {
        self.0 & other.0 != 0
    }

    pub fn fmt(&self) -> FileMode {
        FileMode(self.bits() & uapi::S_IFMT)
    }

    pub const fn with_type(&self, file_type: FileMode) -> FileMode {
        FileMode((self.bits() & Self::PERMISSIONS.bits()) | (file_type.bits() & uapi::S_IFMT))
    }

    pub const fn is_lnk(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFLNK
    }

    pub const fn is_reg(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFREG
    }

    pub const fn is_dir(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFDIR
    }

    pub const fn is_chr(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFCHR
    }

    pub const fn is_blk(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFBLK
    }

    pub const fn is_fifo(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFIFO
    }

    pub const fn is_sock(&self) -> bool {
        (self.bits() & uapi::S_IFMT) == uapi::S_IFSOCK
    }
}

impl ops::BitOr for FileMode {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl ops::BitAnd for FileMode {
    type Output = Self;

    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl ops::BitOrAssign for FileMode {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl ops::BitAndAssign for FileMode {
    fn bitand_assign(&mut self, rhs: Self) {
        self.0 &= rhs.0;
    }
}

impl ops::Not for FileMode {
    type Output = Self;

    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

macro_rules! mode {
    ($type:ident, $mode:expr) => {
        crate::types::FileMode::from_bits($mode) | crate::types::FileMode::$type
    };
}

bitflags::bitflags! {
    pub struct Access: u32 {
        const EXIST = 0;
        const EXEC = 1;
        const WRITE = 2;
        const READ = 4;
        const NOATIME = 8;
    }
}
impl Access {
    pub fn from_open_flags(flags: OpenFlags) -> Self {
        let base_flags = match flags & OpenFlags::ACCESS_MASK {
            OpenFlags::RDONLY => Self::READ,
            OpenFlags::WRONLY => Self::WRITE,
            OpenFlags::RDWR => Self::READ | Self::WRITE,
            _ => Self::EXIST, // Nonstandard access modes can be opened but will fail to read or write.
        };
        let noatime =
            if flags.contains(OpenFlags::NOATIME) { Access::NOATIME } else { Access::empty() };

        base_flags | noatime
    }

    pub fn rwx_bits(&self) -> u32 {
        self.bits & 0o7
    }
}

// Public re-export of macros allows them to be used like regular rust items.
pub(crate) use mode;

#[cfg(test)]
mod test {
    use super::*;

    #[::fuchsia::test]
    fn test_file_mode_from_string() {
        assert_eq!(FileMode::from_string(b"0123"), Ok(FileMode(0o123)));
        assert!(FileMode::from_string(b"123").is_err());
        assert!(FileMode::from_string(b"\x80").is_err());
        assert!(FileMode::from_string(b"0999").is_err());
    }
}
