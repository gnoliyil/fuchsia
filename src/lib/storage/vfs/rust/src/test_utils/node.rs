// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Helper methods for the `NodeProxy` objects.

use {
    fidl::endpoints::{create_proxy, ProtocolMarker, ServerEnd},
    fidl_fuchsia_io as fio,
};

pub fn open_get_proxy<M>(proxy: &fio::DirectoryProxy, flags: fio::OpenFlags, path: &str) -> M::Proxy
where
    M: ProtocolMarker,
{
    let (new_proxy, new_server_end) =
        create_proxy::<M>().expect("Failed to create connection endpoints");

    proxy
        .open(
            flags,
            fio::ModeType::empty(),
            path,
            ServerEnd::<fio::NodeMarker>::new(new_server_end.into_channel()),
        )
        .unwrap();

    new_proxy
}

/// This trait repeats parts of the `NodeProxy` trait, and is implemented for `NodeProxy`,
/// `FileProxy`, and `DirectoryProxy`, which all share the same API.  FIDL currently does not
/// expose the API inheritance, so with this trait we have a workaround.  As soon as FIDL will
/// export the necessary information we should remove this trait, as it is just a workaround. The
/// downsides is that this mapping needs to be manually updated, and that it is not something all
/// the users of FIDL of `NodeProxy` would be familiar with - unnecessary complexity.
///
/// Add methods to this trait when they are necessary to reduce the maintenance effort.
pub trait NodeProxyApi {
    fn clone(
        &self,
        flags: fio::OpenFlags,
        server_end: ServerEnd<fio::NodeMarker>,
    ) -> Result<(), fidl::Error>;
}

/// Calls .clone() on the proxy object, and returns a client side of the connection passed into the
/// clone() method.
pub fn clone_get_proxy<M, Proxy>(proxy: &Proxy, flags: fio::OpenFlags) -> M::Proxy
where
    M: ProtocolMarker,
    Proxy: NodeProxyApi,
{
    let (new_proxy, new_server_end) =
        create_proxy::<M>().expect("Failed to create connection endpoints");

    proxy.clone(flags, new_server_end.into_channel().into()).unwrap();

    new_proxy
}

impl NodeProxyApi for fio::NodeProxy {
    fn clone(
        &self,
        flags: fio::OpenFlags,
        server_end: ServerEnd<fio::NodeMarker>,
    ) -> Result<(), fidl::Error> {
        Self::clone(self, flags, server_end)
    }
}

impl NodeProxyApi for fio::FileProxy {
    fn clone(
        &self,
        flags: fio::OpenFlags,
        server_end: ServerEnd<fio::NodeMarker>,
    ) -> Result<(), fidl::Error> {
        Self::clone(self, flags, server_end)
    }
}

impl NodeProxyApi for fio::DirectoryProxy {
    fn clone(
        &self,
        flags: fio::OpenFlags,
        server_end: ServerEnd<fio::NodeMarker>,
    ) -> Result<(), fidl::Error> {
        Self::clone(self, flags, server_end)
    }
}
