// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Definition of possible errors in this crate.

use fuchsia_zircon as zx;

/// Possible errors in the crate.
#[derive(thiserror::Error, Debug)]
#[allow(missing_docs)]
pub enum Error {
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    #[error("unknown RxFlags({0}) set by driver")]
    RxFlags(u32),
    #[error("unknown FrameType({0}) set by driver")]
    FrameType(u8),
    #[error("the task is dropped so session can make no progress")]
    NoProgress,
    #[error("invalid config: {0}")]
    Config(String),
    #[error("too many descriptors are chained ({0}), at most 4 are allowed")]
    LargeChain(usize),
    #[error("index out of bound {0} > {1}")]
    Index(usize, usize),
    #[error("failed to pad buffer to {0}, capacity {1}")]
    Pad(usize, usize),
    #[error("cannot allocate a buffer to meet the device requirement")]
    TxLength,
    #[error("failed to open session {0}: {1}")]
    Open(String, zx::Status),
    #[error("failed to create VMO {0}: {1}")]
    Vmo(&'static str, zx::Status),
    #[error("failed to {0} fifo {1}: {2}")]
    Fifo(&'static str, &'static str, zx::Status),
    #[error("failed to get size of {0} VMO: {1}")]
    VmoSize(&'static str, zx::Status),
    #[error("failed to map {0} VMO: {1}")]
    Map(&'static str, zx::Status),
    #[error("failed to validate netdev::DeviceInfo")]
    DeviceInfo(#[from] crate::session::DeviceInfoValidationError),
    #[error("failed to validate netdev::PortStatus")]
    PortStatus(#[from] crate::client::PortStatusValidationError),
    #[error("failed to attach port {0:?}: {1}")]
    Attach(crate::Port, zx::Status),
    #[error("failed to detach port {0:?}: {1}")]
    Detach(crate::Port, zx::Status),
    #[error("invalid base port identifier {0}")]
    InvalidPortId(u8),
    #[error("buffer is too small: {size} < {offset} + {length}")]
    TooSmall { size: usize, offset: usize, length: usize },
}

/// Common result type for methods in this crate.
pub type Result<T> = std::result::Result<T, Error>;
