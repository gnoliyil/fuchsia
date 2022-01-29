// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Custom error types for the netstack.

use packet_formats::error::*;
use thiserror::Error;

use crate::ip::device::state::AddressError;

/// Results returned from many functions in the netstack.
pub type Result<T> = core::result::Result<T, NetstackError>;

/// Top-level error type the netstack.
#[derive(Error, Debug, PartialEq)]
pub enum NetstackError {
    #[error("{}", _0)]
    /// Errors related to packet parsing.
    Parse(ParseError),

    /// Error when item already exists.
    #[error("Item already exists")]
    Exists,

    /// Error when item is not found.
    #[error("Item not found")]
    NotFound,

    /// Errors related to sending UDP frames/packets.
    #[error("{}", _0)]
    SendUdp(crate::transport::udp::SendError),

    /// Errors related to connections.
    #[error("{}", _0)]
    Connect(SocketError),

    /// Error when there is no route to an address.
    #[error("No route to address")]
    NoRoute,

    /// Error when a maximum transmission unit (MTU) is exceeded.
    #[error("MTU exceeded")]
    Mtu,
    // Add error types here as we add more to the stack.
}

impl From<AddressError> for NetstackError {
    fn from(error: AddressError) -> Self {
        match error {
            AddressError::AlreadyExists => Self::Exists,
            AddressError::NotFound => Self::NotFound,
        }
    }
}

/// Error when something exists unexpectedly, such as trying to add an
/// element when the element is already present.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct ExistsError;

impl From<ExistsError> for NetstackError {
    fn from(_: ExistsError) -> NetstackError {
        NetstackError::Exists
    }
}

impl From<ExistsError> for SocketError {
    fn from(_: ExistsError) -> SocketError {
        SocketError::Local(LocalAddressError::AddressInUse)
    }
}

/// Error when something unexpectedly doesn't exist, such as trying to
/// remove an element when the element is not present.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct NotFoundError;

impl From<NotFoundError> for NetstackError {
    fn from(_: NotFoundError) -> NetstackError {
        NetstackError::NotFound
    }
}

/// Error type for errors common to local addresses.
#[derive(Error, Debug, PartialEq)]
pub enum LocalAddressError {
    /// Cannot bind to address.
    #[error("can't bind to address")]
    CannotBindToAddress,

    /// Failed to allocate local port.
    #[error("failed to allocate local port")]
    FailedToAllocateLocalPort,

    /// Specified local address does not match any expected address.
    #[error("specified local address does not match any expected address")]
    AddressMismatch,

    /// The requested address/socket pair is in use.
    #[error("Address in use")]
    AddressInUse,
}

// TODO(joshlf): Once we support a more general model of sockets in which UDP
// and ICMP connections are special cases of UDP and ICMP sockets, we can
// introduce a more specialized ListenerError which does not contain the NoRoute
// variant.

/// An error encountered when attempting to create a UDP, TCP, or ICMP connection.
#[derive(Error, Debug, PartialEq)]
pub enum RemoteAddressError {
    /// No route to host.
    #[error("no route to host")]
    NoRoute,
}

/// Error type for connection errors.
#[derive(Error, Debug, PartialEq)]
pub enum SocketError {
    #[error("{}", _0)]
    /// Errors related to the local address.
    Local(LocalAddressError),

    #[error("{}", _0)]
    /// Errors related to the remote address.
    Remote(RemoteAddressError),
}

/// Error when no route exists to a remote address.
#[derive(Debug, PartialEq, Eq)]
pub struct NoRouteError;

impl From<NoRouteError> for NetstackError {
    fn from(_: NoRouteError) -> NetstackError {
        NetstackError::NoRoute
    }
}

impl From<NoRouteError> for SocketError {
    fn from(_: NoRouteError) -> SocketError {
        SocketError::Remote(RemoteAddressError::NoRoute)
    }
}
