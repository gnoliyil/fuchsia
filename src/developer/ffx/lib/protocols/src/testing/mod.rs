// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
mod fake_daemon;

pub use fake_daemon::{FakeDaemon, FakeDaemonBuilder};

use crate::{Context, FidlProtocol};
use fidl::{
    endpoints::{create_endpoints, ProtocolMarker, Proxy},
    AsyncChannel,
};
use fuchsia_async::Task;
use std::{cell::RefCell, rc::Rc};

/// A simple proxy made from a FIDL protocol. This is necessary if your proxy
/// has some specific state you would like to have control over. You can inspect
/// the protocol's internals or call specific functions via use of this method.
///
/// The lifetime of the FIDL protocol is as follows:
/// * invokes start, panicking on failure.
/// * create a `fuchsia_async::Task<()>` which, inside:
///   * invokes serve, panicking on failure.
///   * invokes stop at the end of `serve`, panicking on failure.
///
/// Note: the proxy you receive isn't registered with the FakeDaemon. If you
/// would like to test the `stop` functionality, you will need to drop
/// the proxy returned by this function, then await the returned task.
pub async fn create_proxy<F: FidlProtocol + 'static>(
    f: Rc<RefCell<F>>,
    fake_daemon: &FakeDaemon,
) -> (<F::Protocol as ProtocolMarker>::Proxy, Task<()>) {
    let (client, server) = create_endpoints::<F::Protocol>().unwrap();
    let client = AsyncChannel::from_channel(client.into_channel()).unwrap();
    let client = <F::Protocol as ProtocolMarker>::Proxy::from_channel(client);
    let cx = Context::new(fake_daemon.clone());
    let svc = f.clone();
    svc.borrow_mut().start(&cx).await.unwrap();
    let task = Task::local(async move {
        let stream = server.into_stream().unwrap();
        svc.borrow().serve(&cx, stream).await.unwrap();
        svc.borrow_mut().stop(&cx).await.unwrap();
    });
    (client, task)
}
