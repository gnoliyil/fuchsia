// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Common utilities used by both directory and file traits.

use {
    fidl::endpoints::ServerEnd,
    fidl::prelude::*,
    fidl_fuchsia_io as fio,
    fuchsia_zircon::Status,
    libc,
    std::{convert::TryFrom, sync::Arc},
};

/// Set of known rights.
const FS_RIGHTS: fio::OpenFlags = fio::OPEN_RIGHTS;

/// Flags visible to GetFlags. These are flags that have meaning after the open call; all other
/// flags are only significant at open time.
pub const GET_FLAGS_VISIBLE: fio::OpenFlags = fio::OpenFlags::empty()
    .union(fio::OpenFlags::RIGHT_READABLE)
    .union(fio::OpenFlags::RIGHT_WRITABLE)
    .union(fio::OpenFlags::RIGHT_EXECUTABLE)
    .union(fio::OpenFlags::APPEND)
    .union(fio::OpenFlags::NODE_REFERENCE);

/// Returns true if the rights flags in `flags` do not exceed those in `parent_flags`.
pub fn stricter_or_same_rights(parent_flags: fio::OpenFlags, flags: fio::OpenFlags) -> bool {
    let parent_rights = parent_flags & FS_RIGHTS;
    let rights = flags & FS_RIGHTS;
    return !rights.intersects(!parent_rights);
}

/// Common logic for rights processing during cloning a node, shared by both file and directory
/// implementations.
pub fn inherit_rights_for_clone(
    parent_flags: fio::OpenFlags,
    mut flags: fio::OpenFlags,
) -> Result<fio::OpenFlags, Status> {
    if flags.intersects(fio::OpenFlags::CLONE_SAME_RIGHTS) && flags.intersects(FS_RIGHTS) {
        return Err(Status::INVALID_ARGS);
    }

    // We preserve OPEN_FLAG_APPEND as this is what is the most convenient for the POSIX emulation.
    //
    // OPEN_FLAG_NODE_REFERENCE is enforced, according to our current FS permissions design.
    flags |= parent_flags & (fio::OpenFlags::APPEND | fio::OpenFlags::NODE_REFERENCE);

    // If CLONE_FLAG_SAME_RIGHTS is requested, cloned connection will inherit the same rights
    // as those from the originating connection.  We have ensured that no FS_RIGHTS flags are set
    // above.
    if flags.intersects(fio::OpenFlags::CLONE_SAME_RIGHTS) {
        flags &= !fio::OpenFlags::CLONE_SAME_RIGHTS;
        flags |= parent_flags & FS_RIGHTS;
    }

    if !stricter_or_same_rights(parent_flags, flags) {
        return Err(Status::ACCESS_DENIED);
    }

    // Ignore the POSIX flags for clone.
    flags &= !(fio::OpenFlags::POSIX_WRITABLE | fio::OpenFlags::POSIX_EXECUTABLE);

    Ok(flags)
}

/// Returns the current time in UTC nanoseconds since the UNIX epoch.
pub fn current_time() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| u64::try_from(d.as_nanos()).unwrap_or(0u64))
        .unwrap_or(0u64)
}

/// Creates a default-initialized NodeAttributes. Exists because NodeAttributes does not implement
/// Default.
pub fn node_attributes() -> fio::NodeAttributes {
    fio::NodeAttributes {
        id: 0,
        mode: 0,
        content_size: 0,
        storage_size: 0,
        link_count: 0,
        modification_time: 0,
        creation_time: 0,
    }
}

/// A helper method to send OnOpen event on the handle owned by the `server_end` in case `flags`
/// contains `OPEN_FLAG_STATUS`.
///
/// If the send operation fails for any reason, the error is ignored.  This helper is used during
/// an Open() or a Clone() FIDL methods, and these methods have no means to propagate errors to the
/// caller.  OnOpen event is the only way to do that, so there is nowhere to report errors in
/// OnOpen dispatch.  `server_end` will be closed, so there will be some kind of indication of the
/// issue.
///
/// # Panics
/// If `status` is `Status::OK`.  In this case `OnOpen` may need to contain a description of the
/// object, and server_end should not be dropped.
pub fn send_on_open_with_error(
    flags: fio::OpenFlags,
    server_end: ServerEnd<fio::NodeMarker>,
    status: Status,
) {
    if status == Status::OK {
        panic!("send_on_open_with_error() should not be used to respond with Status::OK");
    }

    if !flags.intersects(fio::OpenFlags::DESCRIBE) {
        // There is no reasonable way to report this error.  Assuming the `server_end` has just
        // disconnected or failed in some other way why we are trying to send OnOpen.
        let _ = server_end.close_with_epitaph(status);
        return;
    }

    match server_end.into_stream_and_control_handle() {
        Ok((_, control_handle)) => {
            // Same as above, ignore the error.
            let _ = control_handle.send_on_open_(status.into_raw(), None);
            control_handle.shutdown_with_epitaph(status);
        }
        Err(_) => {
            // Same as above, ignore the error.
            return;
        }
    }
}

/// Trait to be used as a supertrait when an object should allow dynamic casting to an Any.
///
/// Separate trait since [`into_any`] requires Self to be Sized, which cannot be satisfied in a
/// trait without preventing it from being object safe (thus disallowing dynamic dispatch).
/// Since we provide a generic implementation, the size of each concrete type is known.
pub trait IntoAny: std::any::Any {
    /// Cast the given object into a `dyn std::any::Any`.
    fn into_any(self: Arc<Self>) -> Arc<dyn std::any::Any + Send + Sync + 'static>;
}

impl<T: 'static + Send + Sync> IntoAny for T {
    fn into_any(self: Arc<Self>) -> Arc<dyn std::any::Any + Send + Sync + 'static> {
        self as Arc<dyn std::any::Any + Send + Sync + 'static>
    }
}

/// Returns equivalent POSIX mode/permission bits based on the specified rights.
/// Note that these only set the user bits.
pub fn rights_to_posix_mode_bits(readable: bool, writable: bool, executable: bool) -> u32 {
    return if readable { libc::S_IRUSR } else { 0 }
        | if writable { libc::S_IWUSR } else { 0 }
        | if executable { libc::S_IXUSR } else { 0 };
}

// TODO(https://fxbug.dev/124432): Consolidate with other implementations that do the same thing.
/// Map a set of io1 rights flags for files to their respective io2 Operations.
///
/// Changing this function can be dangerous! Allowed operations may have security implications.
pub fn io1_rights_to_io2_operations(flags: &fio::OpenFlags) -> fio::Operations {
    let mut operations = fio::Operations::empty();
    if flags.contains(fio::OpenFlags::RIGHT_READABLE) {
        operations |= fio::Operations::READ_BYTES;
    }
    if flags.contains(fio::OpenFlags::RIGHT_WRITABLE) {
        operations |= fio::Operations::WRITE_BYTES;
    }
    if flags.contains(fio::OpenFlags::RIGHT_EXECUTABLE) {
        operations |= fio::Operations::EXECUTE;
    }
    operations
}

#[cfg(test)]
mod tests {
    use super::inherit_rights_for_clone;

    use fidl_fuchsia_io as fio;

    // TODO This should be converted into a function as soon as backtrace support is in place.
    // The only reason this is a macro is to generate error messages that point to the test
    // function source location in their top stack frame.
    macro_rules! irfc_ok {
        ($parent_flags:expr, $flags:expr, $expected_new_flags:expr $(,)*) => {{
            let res = inherit_rights_for_clone($parent_flags, $flags);
            match res {
                Ok(new_flags) => assert_eq!(
                    $expected_new_flags, new_flags,
                    "`inherit_rights_for_clone` returned unexpected set of flags.\n\
                     Expected: {:X}\n\
                     Actual: {:X}",
                    $expected_new_flags, new_flags
                ),
                Err(status) => panic!("`inherit_rights_for_clone` failed.  Status: {}", status),
            }
        }};
    }

    #[test]
    fn node_reference_is_inherited() {
        irfc_ok!(
            fio::OpenFlags::NODE_REFERENCE,
            fio::OpenFlags::empty(),
            fio::OpenFlags::NODE_REFERENCE
        );
    }
}
