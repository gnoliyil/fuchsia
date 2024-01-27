// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::Context;
use anyhow::{anyhow, Result};
use async_lock::RwLock;
use async_trait::async_trait;
use async_utils::async_once::Once;
use core::marker::PhantomData;
use fidl::{
    endpoints::{DiscoverableProtocolMarker, ProtocolMarker, Request, RequestStream},
    server::ServeInner,
};
use futures::{
    future::{LocalBoxFuture, Shared},
    prelude::*,
};
use std::{rc::Rc, sync::Arc};

/// A `FidlProtocol` type represents a protocol that can be run on the FFX
/// Daemon.
///
/// # The Protocol Type
///
/// The `FidlProtocol` type represents the implementation of a single FIDL
/// protocol. Each protocol used on the Daemon for the purposes of defining a
/// protocol should map one-to-one to a `FidlProtocol` implementation.
///
/// In other words, if you create a FIDL protocol `FooProtocol`, you can only
/// have one protocol whose [`FidlProtocol::Protocol`] type is defined as being
/// `FooProtocolMarker`. Violating this requirement will cause an error at build
/// time.
///
/// # Examples
///
/// One of the simplest examples can be found under
/// protocol, though. `//src/developer/ffx/daemon/protocols/echo`. Other more
/// advanced examples can be found in `//src/developer/ffx/daemon/protocols/`.
///
/// # Do You Need a Protocol?
///
/// Before implementing a protocol, consider whether or not you _need_ a
/// Daemon protocol. If the answer to any of the following questions are "yes,"
/// then a Daemon protocol may be recommended:
///
/// * Do you intend to have a long-running task in the background kicked off by
///   either an existing protocol or by some frontend interaction?
/// * Do you need something running for the lifetime of the Daemon? (Uploading
///   statistics to a pre-determined URL, for example).
///
/// Most other uses beyond the above aren't recommended for protocols, though if
/// you're unsure, make sure to consult the FFX team.
///
#[async_trait(?Send)]
pub trait FidlProtocol: Unpin + Default {
    /// This determines the protocol the FidlProtocol will implement. This type
    /// determines the input of the [`FidlProtocol::serve`] and
    /// [`FidlProtocol::handle`] functions.
    type Protocol: DiscoverableProtocolMarker;

    /// Type to determine how streams are handled when they come in. For
    /// example, setting this type to `FidlStreamHandler<Self>` will ensure that
    /// there will only ever be one instance of this protocol ever allocated.
    /// This is very useful for keeping track of persistent state across all
    /// different client streams with this protocol.
    ///
    /// See the documentation for other stream handlers in this crate if this
    /// does not sound sufficient.
    type StreamHandler: StreamHandler + Default;

    /// Dictates how the handle function is invoked across the lifetime of a
    /// single FIDL request stream. The default is to handle each request in
    /// serial. This can be changed as needed, but will likely only ever need
    /// to remain the default implementation.
    async fn serve<'a>(
        &'a self,
        cx: &'a Context,
        mut stream: <Self::Protocol as ProtocolMarker>::RequestStream,
    ) -> Result<()> {
        while let Ok(Some(req)) = stream.try_next().await {
            self.handle(cx, req).await?
        }
        Ok(())
    }

    /// Handles each individual request coming from a FIDL request stream. If
    /// interacting with another protocol, or some specific Daemon internal is
    /// necessary, use the [`Context`] object.
    async fn handle(&self, cx: &Context, req: Request<Self::Protocol>) -> Result<()>;

    /// Invoked before any streams are opened for the protocol. This will only
    /// ever be invoked once through the lifetime of this protocol.
    async fn start(&mut self, _cx: &Context) -> Result<()> {
        Ok(())
    }

    /// Invoked for cleanup at the end of the protocol's lifetime. This is
    /// invoked after every active request stream has been stopped and every
    /// running [`FidlProtocol::handle`] function has exited. This function
    /// will only ever be invoked once.
    async fn stop(&mut self, _cx: &Context) -> Result<()> {
        Ok(())
    }
}

/// `FidlInstancedStreamHandler` allows for a protocol to have an instance for
/// each individual FIDL request stream. This can be useful for a protocol that
/// does not need to run for a long time, or only is interested in state across
/// several calls in only one stream.
///
/// Once the stream has completed [`FidlProtocol::stop`] will be called on the
/// instance.
pub struct FidlInstancedStreamHandler<F: FidlProtocol> {
    _protocol: PhantomData<F>,
}

impl<F> Default for FidlInstancedStreamHandler<F>
where
    F: FidlProtocol,
{
    fn default() -> Self {
        FidlInstancedStreamHandler { _protocol: PhantomData }
    }
}

/// A `StreamHandler` is something that takes a FIDL request stream and
/// determines how to allocate/cleanup a protocol accordingly.
#[async_trait(?Send)]
pub trait StreamHandler {
    /// Starts the protocol, if possible. For instanced protocols this would be
    /// a no-op as the protocol would no longer later be usable by any caller.
    ///
    /// For a non-instanced protocol this is typically invoked before
    /// [`StreamHandler::open`] begins handling the first stream.
    async fn start(&self, cx: Context) -> Result<()>;

    /// Called when opening a new stream. This stream may be shutdown outside
    /// of the control of this object by the protocol register via the
    /// `Arc<ServeInner>` struct.
    async fn open(
        &self,
        cx: Context,
        server: Arc<ServeInner>,
    ) -> Result<LocalBoxFuture<'static, Result<()>>>;

    /// Called after every other stream handled via the `open` function has
    /// been shut down.
    async fn shutdown(&self, cx: &Context) -> Result<()>;
}

#[async_trait(?Send)]
impl<F> StreamHandler for FidlInstancedStreamHandler<F>
where
    F: FidlProtocol + 'static,
{
    /// This is a no-op, as the protocol cannot be interacted with by the caller
    /// afterwards.
    async fn start(&self, _cx: Context) -> Result<()> {
        Ok(())
    }

    /// Default implementation for the stream handler. The future returned by this
    /// lives for the lifetime of the stream represented by `server`
    async fn open(
        &self,
        cx: Context,
        server: Arc<ServeInner>,
    ) -> Result<LocalBoxFuture<'static, Result<()>>> {
        let stream = <F::Protocol as ProtocolMarker>::RequestStream::from_inner(server, false);
        let mut svc = F::default();
        svc.start(&cx).await?;
        let fut = Box::pin(async move {
            let serve_res = svc.serve(&cx, stream).await.map_err(|e| {
                tracing::warn!(
                    "protocol failure while handling stream. Stopping protocol: {:?}",
                    e
                );
                e
            });
            svc.stop(&cx).await?;
            serve_res
        });
        Ok(fut)
    }

    async fn shutdown(&self, _cx: &Context) -> Result<()> {
        Ok(())
    }
}

type StartFut = Shared<LocalBoxFuture<'static, Rc<Result<()>>>>;

/// The recommended stream handler for protocols. This type will only ever open
/// one instance of the protocol across all streams.
#[derive(Default)]
pub struct FidlStreamHandler<F: FidlProtocol> {
    protocol: Rc<RwLock<Option<F>>>,
    start_fut: Once<StartFut>,
}

impl<F: FidlProtocol> FidlStreamHandler<F>
where
    F: 'static,
{
    fn make_start_fut(&self, cx: Context) -> Shared<LocalBoxFuture<'static, Rc<Result<()>>>> {
        let inner = Rc::downgrade(&self.protocol);
        async move {
            // In order to have a clonable result here, this needs
            // to return an Rc of the error which is then later
            // transposed into another error (see error below).
            //
            // TODO(awdavies): Implement this using a singleflight
            // mechanism, allowing for the protocol to attempt to
            // start more than once.
            if let Some(inner) = inner.upgrade() {
                if let Some(ref mut inner) = *inner.write().await {
                    Rc::new(inner.start(&cx).await)
                } else {
                    Rc::new(Err(anyhow!("singleton has been shut down")))
                }
            } else {
                Rc::new(Err(anyhow!("lost Rc<_> to protocol singleton")))
            }
        }
        .boxed_local()
        .shared()
    }

    async fn start_protocol(&self, cx: &Context) -> Result<()> {
        let cx = cx.clone();
        // async_once interacts with what we're doing here in a way that causes us to need to yield
        // a future from a future.
        #[allow(clippy::async_yields_async)]
        let fut = self
            .start_fut
            .get_or_init(async move {
                self.protocol.write().await.replace(F::default());
                self.make_start_fut(cx)
            })
            .await
            .clone();

        let res = fut.await;
        match &*res {
            Ok(_) => Ok(()),
            Err(e) => Err(anyhow!("singleton fut err: {:#?}", e)),
        }
    }
}

#[async_trait(?Send)]
impl<F> StreamHandler for FidlStreamHandler<F>
where
    F: FidlProtocol + 'static,
{
    /// Starts the protocol at most once. Invoking this more than once will
    /// return the same result each time (including errors).
    async fn start(&self, cx: Context) -> Result<()> {
        self.start_protocol(&cx).await
    }

    async fn open(
        &self,
        cx: Context,
        server: Arc<ServeInner>,
    ) -> Result<LocalBoxFuture<'static, Result<()>>> {
        self.start_protocol(&cx).await?;
        let stream = <F::Protocol as ProtocolMarker>::RequestStream::from_inner(server, false);
        let protocol = Rc::downgrade(&self.protocol);
        let fut = async move {
            if let Some(protocol) = protocol.upgrade() {
                protocol
                    .read()
                    .await
                    .as_ref()
                    .ok_or(anyhow!("protocol has been shutdown"))?
                    .serve(&cx, stream)
                    .await
            } else {
                tracing::debug!("dropped singleton protocol Rc<_>");
                Ok(())
            }
        };
        Ok(Box::pin(fut))
    }

    async fn shutdown(&self, cx: &Context) -> Result<()> {
        self.protocol
            .write()
            .await
            .take()
            .ok_or(anyhow!("protocol has been stopped"))?
            .stop(cx)
            .await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testing::FakeDaemonBuilder;
    use anyhow::anyhow;
    use fidl_fuchsia_developer_ffx as ffx;
    use fidl_fuchsia_ffx_test as ffx_test;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn noop_protocol_closure() -> impl Fn(&Context, ffx_test::NoopRequest) -> Result<()> {
        |_, req| match req {
            ffx_test::NoopRequest::DoNoop { responder } => responder.send().map_err(Into::into),
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn noop_test() {
        let daemon = FakeDaemonBuilder::new()
            .register_instanced_protocol_closure::<ffx_test::NoopMarker, _>(noop_protocol_closure())
            .build();
        let proxy = daemon.open_proxy::<ffx_test::NoopMarker>().await;
        assert!(proxy.do_noop().await.is_ok());
        daemon.shutdown().await.unwrap();
    }

    #[derive(Default)]
    struct CounterProtocol {
        count: AtomicU64,
    }

    #[async_trait(?Send)]
    impl FidlProtocol for CounterProtocol {
        type Protocol = ffx_test::CounterMarker;
        type StreamHandler = FidlInstancedStreamHandler<Self>;

        async fn handle(&self, cx: &Context, req: ffx_test::CounterRequest) -> Result<()> {
            // This is just here for some additional stress.
            let noop_proxy = cx
                .open_target_proxy::<ffx_test::NoopMarker>(
                    None,
                    "core/appmgr:out:fuchsia.ffx.test.Noop",
                )
                .await?;
            noop_proxy.do_noop().await?;
            match req {
                ffx_test::CounterRequest::AddOne { responder } => {
                    self.count.fetch_add(1, Ordering::SeqCst);
                    responder.send().map_err(Into::into)
                }
                ffx_test::CounterRequest::GetCount { responder } => {
                    responder.send(self.count.load(Ordering::SeqCst)).map_err(Into::into)
                }
            }
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn counter_test() {
        let daemon = FakeDaemonBuilder::new()
            .register_instanced_protocol_closure::<ffx_test::NoopMarker, _>(noop_protocol_closure())
            .register_fidl_protocol::<CounterProtocol>()
            .target(ffx::TargetInfo {
                nodename: Some("foobar".to_string()),
                ..ffx::TargetInfo::EMPTY
            })
            .build();
        let counter_proxy = daemon.open_proxy::<ffx_test::CounterMarker>().await;
        counter_proxy.add_one().await.unwrap();
        counter_proxy.add_one().await.unwrap();
        counter_proxy.add_one().await.unwrap();
        assert_eq!(3, counter_proxy.get_count().await.unwrap());
        drop(counter_proxy);
        // Ensure no state is kept between instances.
        let counter_proxy = daemon.open_proxy::<ffx_test::CounterMarker>().await;
        assert_eq!(0, counter_proxy.get_count().await.unwrap());
    }

    /// This is a struct that panics if stop hasn't been called. When attempting
    /// to run `serve` it will always err as well.
    #[derive(Default)]
    struct NoopProtocolPanicker {
        stopped: bool,
    }

    #[async_trait(?Send)]
    impl FidlProtocol for NoopProtocolPanicker {
        type Protocol = ffx_test::NoopMarker;
        type StreamHandler = FidlInstancedStreamHandler<Self>;

        async fn handle(&self, _cx: &Context, _req: ffx_test::NoopRequest) -> Result<()> {
            Err(anyhow!("this is intended to fail every time"))
        }

        async fn stop(&mut self, _cx: &Context) -> Result<()> {
            self.stopped = true;
            Ok(())
        }
    }

    impl Drop for NoopProtocolPanicker {
        fn drop(&mut self) {
            assert!(self.stopped)
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn protocol_error_ensures_drop_test() -> Result<()> {
        let daemon =
            FakeDaemonBuilder::new().register_fidl_protocol::<NoopProtocolPanicker>().build();
        let proxy = daemon.open_proxy::<ffx_test::NoopMarker>().await;
        assert!(proxy.do_noop().await.is_err());
        daemon.shutdown().await
    }

    #[derive(Default)]
    struct SingletonCounterProtocol {
        count: AtomicU64,
        started: bool,
    }

    #[async_trait(?Send)]
    impl FidlProtocol for SingletonCounterProtocol {
        type Protocol = ffx_test::CounterMarker;
        type StreamHandler = FidlStreamHandler<Self>;

        async fn handle(&self, cx: &Context, req: ffx_test::CounterRequest) -> Result<()> {
            // This is just here for some additional stress.
            let noop_proxy = cx
                .open_target_proxy::<ffx_test::NoopMarker>(
                    None,
                    "core/appmgr:out:fuchsia.ffx.test.Noop",
                )
                .await?;
            noop_proxy.do_noop().await?;
            match req {
                ffx_test::CounterRequest::AddOne { responder } => {
                    self.count.fetch_add(1, Ordering::SeqCst);
                    responder.send().map_err(Into::into)
                }
                ffx_test::CounterRequest::GetCount { responder } => {
                    responder.send(self.count.load(Ordering::SeqCst)).map_err(Into::into)
                }
            }
        }

        async fn start(&mut self, cx: &Context) -> Result<()> {
            let noop_proxy = cx
                .open_target_proxy::<ffx_test::NoopMarker>(
                    None,
                    "core/appmgr:out:fuchsia.ffx.test.Noop",
                )
                .await
                .unwrap();
            noop_proxy.do_noop().await.unwrap();
            if self.started {
                panic!("this can only be run once");
            }
            self.started = true;
            Ok(())
        }

        async fn stop(&mut self, _cx: &Context) -> Result<()> {
            if !self.started {
                panic!("this must be started before being shut down");
            }
            Ok(())
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn counter_test_singleton() {
        let daemon = FakeDaemonBuilder::new()
            .register_instanced_protocol_closure::<ffx_test::NoopMarker, _>(noop_protocol_closure())
            .register_fidl_protocol::<SingletonCounterProtocol>()
            .target(ffx::TargetInfo {
                nodename: Some("foobar".to_string()),
                ..ffx::TargetInfo::EMPTY
            })
            .build();
        let (counter_proxy1, counter_proxy2, counter_proxy3) = futures::join!(
            daemon.open_proxy::<ffx_test::CounterMarker>(),
            daemon.open_proxy::<ffx_test::CounterMarker>(),
            daemon.open_proxy::<ffx_test::CounterMarker>()
        );
        assert_eq!(0, counter_proxy1.get_count().await.unwrap());
        assert_eq!(0, counter_proxy2.get_count().await.unwrap());
        assert_eq!(0, counter_proxy3.get_count().await.unwrap());
        counter_proxy3.add_one().await.unwrap();
        assert_eq!(1, counter_proxy1.get_count().await.unwrap());
        assert_eq!(1, counter_proxy2.get_count().await.unwrap());
        assert_eq!(1, counter_proxy3.get_count().await.unwrap());
        counter_proxy1.add_one().await.unwrap();
        assert_eq!(2, counter_proxy1.get_count().await.unwrap());
        assert_eq!(2, counter_proxy2.get_count().await.unwrap());
        assert_eq!(2, counter_proxy3.get_count().await.unwrap());
        drop(counter_proxy1);
        drop(counter_proxy2);
        drop(counter_proxy3);
        let counter_proxy = daemon.open_proxy::<ffx_test::CounterMarker>().await;
        assert_eq!(2, counter_proxy.get_count().await.unwrap());
        daemon.shutdown().await.unwrap();
    }
}
