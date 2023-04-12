// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Common utilities used by several directory implementations.

use crate::{
    common::{io2_conversions, send_on_open_with_error, stricter_or_same_rights},
    directory::entry::EntryInfo,
};

use {
    byteorder::{LittleEndian, WriteBytesExt as _},
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    static_assertions::assert_eq_size,
    std::{io::Write as _, mem::size_of},
};

struct OptionsWithDescribe<T: TryFrom<fio::OpenFlags>> {
    describe: bool,
    options: Result<T, <T as TryFrom<fio::OpenFlags>>::Error>,
}

impl<T: TryFrom<fio::OpenFlags>> From<fio::OpenFlags> for OptionsWithDescribe<T> {
    fn from(flags: fio::OpenFlags) -> Self {
        let describe = flags.intersects(fio::OpenFlags::DESCRIBE);
        let options = (flags & !fio::OpenFlags::DESCRIBE).try_into();
        Self { describe, options }
    }
}

/// A directory can be open either as a directory or a node.
pub struct DirectoryOptions {
    pub(crate) node: bool,
    pub(crate) rights: fio::Operations,
}

impl DirectoryOptions {
    pub(crate) fn into_io1(&self) -> fio::OpenFlags {
        let Self { node, rights } = self;
        let mut flags = io2_conversions::io2_to_io1(*rights);
        if *node {
            flags |= fio::OpenFlags::NODE_REFERENCE;
        }
        flags
    }
}

/// Checks flags provided for a new connection.  Returns adjusted flags (cleaning up some
/// ambiguities) or a fidl Status error, in case new new connection flags are not permitting the
/// connection to be opened.
///
/// OPEN_FLAG_NODE_REFERENCE is preserved.
///
/// Changing this function can be dangerous!  Flags operations may have security implications.
impl TryFrom<fio::OpenFlags> for DirectoryOptions {
    type Error = zx::Status;

    fn try_from(mut flags: fio::OpenFlags) -> Result<Self, Self::Error> {
        let node = flags.intersects(fio::OpenFlags::NODE_REFERENCE);
        if node {
            flags &= fio::OPEN_FLAGS_ALLOWED_WITH_NODE_REFERENCE;
        }

        if flags.intersects(fio::OpenFlags::DIRECTORY) {
            flags &= !fio::OpenFlags::DIRECTORY;
        }

        if flags.intersects(fio::OpenFlags::NOT_DIRECTORY) {
            return Err(zx::Status::NOT_FILE);
        }

        // Parent connection must check the POSIX flags in `check_child_connection_flags`, so if any
        // are still present, we expand their respective rights and remove any remaining flags.
        if flags.intersects(fio::OpenFlags::POSIX_EXECUTABLE) {
            flags |= fio::OpenFlags::RIGHT_EXECUTABLE;
        }
        if flags.intersects(fio::OpenFlags::POSIX_WRITABLE) {
            flags |= fio::OpenFlags::RIGHT_WRITABLE;
        }
        flags &= !(fio::OpenFlags::POSIX_WRITABLE | fio::OpenFlags::POSIX_EXECUTABLE);

        let allowed_flags = fio::OpenFlags::NODE_REFERENCE
            | fio::OpenFlags::DESCRIBE
            | fio::OpenFlags::CREATE
            | fio::OpenFlags::CREATE_IF_ABSENT
            | fio::OpenFlags::DIRECTORY
            | fio::OpenFlags::RIGHT_READABLE
            | fio::OpenFlags::RIGHT_WRITABLE
            | fio::OpenFlags::RIGHT_EXECUTABLE;

        let prohibited_flags = fio::OpenFlags::APPEND | fio::OpenFlags::TRUNCATE;

        if flags.intersects(prohibited_flags) {
            return Err(zx::Status::INVALID_ARGS);
        }

        if flags.intersects(!allowed_flags) {
            return Err(zx::Status::NOT_SUPPORTED);
        }

        Ok(Self { node, rights: io2_conversions::io1_to_io2(flags) })
    }
}

pub fn with_directory_options<T>(
    flags: fio::OpenFlags,
    server_end: fidl::endpoints::ServerEnd<fio::NodeMarker>,
    op: impl FnOnce(bool, DirectoryOptions, fidl::endpoints::ServerEnd<fio::NodeMarker>) -> T,
) -> Option<T> {
    let OptionsWithDescribe { describe, options } = flags.into();
    match options {
        Ok(options) => Some(op(describe, options, server_end)),
        Err(status) => {
            let () = send_on_open_with_error(describe, server_end, status);
            None
        }
    }
}

/// Directories need to make sure that connections to child entries do not receive more rights than
/// the connection to the directory itself.  Plus there is special handling of the OPEN_FLAG_POSIX_*
/// flags. This function should be called before calling [`new_connection_validate_flags`] if both
/// are needed.
pub(crate) fn check_child_connection_flags(
    parent_flags: fio::OpenFlags,
    mut flags: fio::OpenFlags,
) -> Result<fio::OpenFlags, zx::Status> {
    if flags & (fio::OpenFlags::NOT_DIRECTORY | fio::OpenFlags::DIRECTORY)
        == fio::OpenFlags::NOT_DIRECTORY | fio::OpenFlags::DIRECTORY
    {
        return Err(zx::Status::INVALID_ARGS);
    }

    // Can only specify OPEN_FLAG_CREATE_IF_ABSENT if OPEN_FLAG_CREATE is also specified.
    if flags.intersects(fio::OpenFlags::CREATE_IF_ABSENT)
        && !flags.intersects(fio::OpenFlags::CREATE)
    {
        return Err(zx::Status::INVALID_ARGS);
    }

    // Can only use CLONE_FLAG_SAME_RIGHTS when calling Clone.
    if flags.intersects(fio::OpenFlags::CLONE_SAME_RIGHTS) {
        return Err(zx::Status::INVALID_ARGS);
    }

    // Remove POSIX flags when the respective rights are not available ("soft fail").
    if !parent_flags.intersects(fio::OpenFlags::RIGHT_EXECUTABLE) {
        flags &= !fio::OpenFlags::POSIX_EXECUTABLE;
    }
    if !parent_flags.intersects(fio::OpenFlags::RIGHT_WRITABLE) {
        flags &= !fio::OpenFlags::POSIX_WRITABLE;
    }

    // Can only use CREATE flags if the parent connection is writable.
    if flags.intersects(fio::OpenFlags::CREATE)
        && !parent_flags.intersects(fio::OpenFlags::RIGHT_WRITABLE)
    {
        return Err(zx::Status::ACCESS_DENIED);
    }

    if stricter_or_same_rights(parent_flags, flags) {
        Ok(flags)
    } else {
        Err(zx::Status::ACCESS_DENIED)
    }
}

/// A helper to generate binary encodings for the ReadDirents response.  This function will append
/// an entry description as specified by `entry` and `name` to the `buf`, and would return `true`.
/// In case this would cause the buffer size to exceed `max_bytes`, the buffer is then left
/// untouched and a `false` value is returned.
pub(crate) fn encode_dirent(
    buf: &mut Vec<u8>,
    max_bytes: u64,
    entry: &EntryInfo,
    name: &str,
) -> bool {
    let header_size = size_of::<u64>() + size_of::<u8>() + size_of::<u8>();

    assert_eq_size!(u64, usize);

    if buf.len() + header_size + name.len() > max_bytes as usize {
        return false;
    }

    assert!(
        name.len() <= fio::MAX_FILENAME as usize,
        "Entry names are expected to be no longer than MAX_FILENAME ({}) bytes.\n\
         Got entry: '{}'\n\
         Length: {} bytes",
        fio::MAX_FILENAME,
        name,
        name.len()
    );

    assert!(
        fio::MAX_FILENAME <= u8::max_value() as u64,
        "Expecting to be able to store MAX_FILENAME ({}) in one byte.",
        fio::MAX_FILENAME
    );

    buf.write_u64::<LittleEndian>(entry.inode())
        .expect("out should be an in memory buffer that grows as needed");
    buf.write_u8(name.len() as u8).expect("out should be an in memory buffer that grows as needed");
    buf.write_u8(entry.type_().into_primitive())
        .expect("out should be an in memory buffer that grows as needed");
    buf.write_all(name.as_ref()).expect("out should be an in memory buffer that grows as needed");

    true
}

#[cfg(test)]
mod tests {
    use super::{check_child_connection_flags, DirectoryOptions, OptionsWithDescribe};
    use crate::test_utils::build_flag_combinations;

    use {fidl_fuchsia_io as fio, fuchsia_zircon as zx};

    // TODO(https://fxbug.dev/77623): Remove and directly test
    // `<DirectoryOptions as TryFrom<fio::OpenFlags>>`.
    fn new_connection_validate_flags(flags: fio::OpenFlags) -> Result<fio::OpenFlags, zx::Status> {
        let OptionsWithDescribe { describe, options } = flags.into();
        let options: DirectoryOptions = options?;
        let mut flags = options.into_io1();
        if describe {
            flags |= fio::OpenFlags::DESCRIBE;
        }
        Ok(flags)
    }

    #[track_caller]
    fn ncvf_ok(flags: fio::OpenFlags, expected_new_flags: fio::OpenFlags) {
        let res = new_connection_validate_flags(flags);
        match res {
            Ok(new_flags) => assert_eq!(
                expected_new_flags, new_flags,
                "new_connection_validate_flags returned unexpected set of flags.\n\
                    Expected: {:X}\n\
                    Actual: {:X}",
                expected_new_flags, new_flags
            ),
            Err(status) => panic!("new_connection_validate_flags failed.  Status: {}", status),
        }
    }

    #[track_caller]
    fn ncvf_err(flags: fio::OpenFlags, expected_status: zx::Status) {
        let res = new_connection_validate_flags(flags);
        match res {
            Ok(new_flags) => panic!(
                "new_connection_validate_flags should have failed.  \
                    Got new flags: {:X}",
                new_flags
            ),
            Err(status) => assert_eq!(expected_status, status),
        }
    }

    #[test]
    fn new_connection_validate_flags_node_reference() {
        // OPEN_FLAG_NODE_REFERENCE and OPEN_FLAG_DESCRIBE should be preserved.
        const PRESERVED_FLAGS: fio::OpenFlags = fio::OpenFlags::empty()
            .union(fio::OpenFlags::NODE_REFERENCE)
            .union(fio::OpenFlags::DESCRIBE);
        for open_flags in build_flag_combinations(
            fio::OpenFlags::NODE_REFERENCE.bits(),
            (fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE
                | fio::OpenFlags::DIRECTORY)
                .bits(),
        ) {
            let open_flags = fio::OpenFlags::from_bits_truncate(open_flags);
            ncvf_ok(open_flags, open_flags & PRESERVED_FLAGS);
        }

        ncvf_err(
            fio::OpenFlags::NODE_REFERENCE | fio::OpenFlags::NOT_DIRECTORY,
            zx::Status::NOT_FILE,
        );
    }

    #[test]
    fn new_connection_validate_flags_posix() {
        for open_flags in build_flag_combinations(
            0,
            (fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::POSIX_EXECUTABLE
                | fio::OpenFlags::POSIX_WRITABLE)
                .bits(),
        ) {
            let open_flags = fio::OpenFlags::from_bits_truncate(open_flags);
            let mut expected_rights = open_flags & fio::OpenFlags::RIGHT_READABLE;
            if open_flags.intersects(fio::OpenFlags::POSIX_WRITABLE) {
                expected_rights |= fio::OpenFlags::RIGHT_WRITABLE
            }
            if open_flags.intersects(fio::OpenFlags::POSIX_EXECUTABLE) {
                expected_rights |= fio::OpenFlags::RIGHT_EXECUTABLE
            }
            ncvf_ok(open_flags, expected_rights);
        }
    }

    #[test]
    fn new_connection_validate_flags_append() {
        ncvf_err(fio::OpenFlags::RIGHT_WRITABLE | fio::OpenFlags::APPEND, zx::Status::INVALID_ARGS);
    }

    #[test]
    fn new_connection_validate_flags_truncate() {
        ncvf_err(
            fio::OpenFlags::RIGHT_WRITABLE | fio::OpenFlags::TRUNCATE,
            zx::Status::INVALID_ARGS,
        );
    }

    #[test]
    fn check_child_connection_flags_create_flags() {
        assert_eq!(
            check_child_connection_flags(fio::OpenFlags::RIGHT_WRITABLE, fio::OpenFlags::CREATE,),
            Ok(fio::OpenFlags::CREATE)
        );
        assert_eq!(
            check_child_connection_flags(
                fio::OpenFlags::RIGHT_WRITABLE,
                fio::OpenFlags::CREATE | fio::OpenFlags::CREATE_IF_ABSENT,
            ),
            Ok(fio::OpenFlags::CREATE | fio::OpenFlags::CREATE_IF_ABSENT)
        );

        assert_eq!(
            check_child_connection_flags(fio::OpenFlags::empty(), fio::OpenFlags::CREATE),
            Err(zx::Status::ACCESS_DENIED),
        );
        assert_eq!(
            check_child_connection_flags(
                fio::OpenFlags::empty(),
                fio::OpenFlags::CREATE | fio::OpenFlags::CREATE_IF_ABSENT,
            ),
            Err(zx::Status::ACCESS_DENIED),
        );

        // Need to specify OPEN_FLAG_CREATE if passing OPEN_FLAG_CREATE_IF_ABSENT.
        assert_eq!(
            check_child_connection_flags(
                fio::OpenFlags::RIGHT_WRITABLE,
                fio::OpenFlags::CREATE_IF_ABSENT,
            ),
            Err(zx::Status::INVALID_ARGS),
        );
    }

    #[test]
    fn check_child_connection_flags_invalid() {
        // Cannot specify both OPEN_FLAG_DIRECTORY and OPEN_FLAG_NOT_DIRECTORY.
        assert_eq!(
            check_child_connection_flags(
                fio::OpenFlags::empty(),
                fio::OpenFlags::DIRECTORY | fio::OpenFlags::NOT_DIRECTORY,
            ),
            Err(zx::Status::INVALID_ARGS),
        );

        // Cannot specify CLONE_FLAG_SAME_RIGHTS when opening a resource (only permitted via clone).
        assert_eq!(
            check_child_connection_flags(
                fio::OpenFlags::empty(),
                fio::OpenFlags::CLONE_SAME_RIGHTS,
            ),
            Err(zx::Status::INVALID_ARGS),
        );
    }
}
