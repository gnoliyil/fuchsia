// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use thiserror::Error;

#[derive(Eq, Error, Clone, Debug, PartialEq)]
pub enum FxfsError {
    #[error("Already exists")]
    AlreadyExists,
    #[error("Filesystem inconsistency")]
    Inconsistent,
    #[error("Internal error")]
    Internal,
    #[error("Expected directory")]
    NotDir,
    #[error("Expected file")]
    NotFile,
    #[error("Not found")]
    NotFound,
    #[error("Not empty")]
    NotEmpty,
    #[error("Read only filesystem")]
    ReadOnlyFilesystem,
    #[error("No space")]
    NoSpace,
    #[error("Deleted")]
    Deleted,
    #[error("Invalid arguments")]
    InvalidArgs,
    #[error("Too big")]
    TooBig,
    #[error("Invalid version")]
    InvalidVersion,
    #[error("Journal flush error")]
    JournalFlushError,
    #[error("Not supported")]
    NotSupported,
    #[error("Access denied")]
    AccessDenied,
    #[error("Out of range")]
    OutOfRange,
    #[error("Already bound")]
    AlreadyBound,
    #[error("Bad path")]
    BadPath,
    #[error("Wrong type")]
    WrongType,
}

impl FxfsError {
    /// A helper to match against this FxfsError against the root cause of an anyhow::Error.
    ///
    /// The main application of this helper is to allow us to match an anyhow::Error against a
    /// specific case of FxfsError in a boolean expression, such as:
    ///
    /// let result: Result<(), anyhow:Error> = foo();
    /// match result {
    ///   Ok(foo) => Ok(foo),
    ///   Err(e) if &FxfsError::NotFound.matches(e) => { ... }
    ///   Err(e) => Err(e)
    /// }
    pub fn matches(&self, error: &anyhow::Error) -> bool {
        if let Some(root_cause) = error.root_cause().downcast_ref::<FxfsError>() {
            self == root_cause
        } else {
            false
        }
    }
}

#[cfg(target_os = "fuchsia")]
mod fuchsia {
    use {super::*, fuchsia_zircon::Status};

    impl From<FxfsError> for Status {
        fn from(err: FxfsError) -> Status {
            match err {
                FxfsError::AlreadyExists => Status::ALREADY_EXISTS,
                FxfsError::Inconsistent => Status::IO_DATA_INTEGRITY,
                FxfsError::Internal => Status::INTERNAL,
                FxfsError::NotDir => Status::NOT_DIR,
                FxfsError::NotFile => Status::NOT_FILE,
                FxfsError::NotFound => Status::NOT_FOUND,
                FxfsError::NotEmpty => Status::NOT_EMPTY,
                FxfsError::ReadOnlyFilesystem => Status::ACCESS_DENIED,
                FxfsError::NoSpace => Status::NO_SPACE,
                FxfsError::Deleted => Status::ACCESS_DENIED,
                FxfsError::InvalidArgs => Status::INVALID_ARGS,
                FxfsError::TooBig => Status::FILE_BIG,
                FxfsError::InvalidVersion => Status::NOT_SUPPORTED,
                FxfsError::JournalFlushError => Status::IO,
                FxfsError::NotSupported => Status::NOT_SUPPORTED,
                FxfsError::AccessDenied => Status::ACCESS_DENIED,
                FxfsError::OutOfRange => Status::OUT_OF_RANGE,
                FxfsError::AlreadyBound => Status::ALREADY_BOUND,
                FxfsError::BadPath => Status::BAD_PATH,
                FxfsError::WrongType => Status::WRONG_TYPE,
            }
        }
    }
}
