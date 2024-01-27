// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Stream sockets, primarily TCP sockets.

use std::{
    convert::Infallible as Never,
    num::{NonZeroU16, NonZeroUsize},
    ops::{ControlFlow, DerefMut as _},
    sync::Arc,
};

use assert_matches::assert_matches;
use async_utils::stream::OneOrMany;
use fidl::{
    endpoints::{ClientEnd, ControlHandle as _, RequestStream as _},
    HandleBased as _,
};
use fidl_fuchsia_io as fio;
use fidl_fuchsia_net as fnet;
use fidl_fuchsia_posix as fposix;
use fidl_fuchsia_posix_socket as fposix_socket;
use fuchsia_async as fasync;
use fuchsia_zircon::{self as zx, Peered as _};
use futures::StreamExt as _;
use log::error;
use net_types::{
    ip::{IpAddress, IpVersion, IpVersionMarker, Ipv4, Ipv6},
    SpecifiedAddr, ZonedAddr,
};

use crate::bindings::{
    devices::Devices,
    socket::{IntoErrno, IpSockAddrExt, SockAddr, ZXSIO_SIGNAL_CONNECTED, ZXSIO_SIGNAL_INCOMING},
    util::{IntoFidl, NeedsDataNotifier, NeedsDataWatcher, TryIntoFidl},
    LockableContext, StackTime,
};

use net_types::ip::Ip;
use netstack3_core::{
    device::DeviceId,
    ip::IpExt,
    transport::tcp::{
        buffer::{Buffer, IntoBuffers, ReceiveBuffer, RingBuffer, SendBuffer, SendPayload},
        segment::Payload,
        socket::{
            accept, bind, close_conn, connect_bound, connect_unbound, create_socket,
            get_bound_info, get_connection_info, get_listener_info, listen, remove_bound,
            remove_unbound, set_bound_device, set_connection_device, set_listener_device,
            set_unbound_device, shutdown_conn, shutdown_listener, AcceptError, BindError, BoundId,
            BoundInfo, ConnectError, ConnectionId, ConnectionInfo, ListenerId, NoConnection,
            SocketAddr, TcpNonSyncContext, UnboundId,
        },
        state::Takeable,
        BufferSizes,
    },
    Ctx,
};

#[derive(Debug)]
enum SocketId<I: Ip> {
    Unbound(UnboundId<I>, LocalZirconSocketAndNotifier),
    Bound(BoundId<I>, LocalZirconSocketAndNotifier),
    Connection(ConnectionId<I>),
    Listener(ListenerId<I>),
}

pub(crate) trait SocketWorkerDispatcher:
    TcpNonSyncContext<
    ProvidedBuffers = LocalZirconSocketAndNotifier,
    ReturnedBuffers = PeerZirconSocketAndWatcher,
>
{
    /// Registers a newly created listener with its local zircon socket.
    ///
    /// # Panics
    /// Panics if `id` is already registered.
    fn register_listener<I: Ip>(&mut self, id: ListenerId<I>, socket: zx::Socket);
    /// Unregisters an existing listener when it is about to be closed.
    ///
    /// Returns the zircon socket that used to be registered.
    ///
    /// # Panics
    /// Panics if `id` is non-existent.
    fn unregister_listener<I: Ip>(&mut self, id: ListenerId<I>) -> zx::Socket;
}

impl SocketWorkerDispatcher for crate::bindings::BindingsNonSyncCtxImpl {
    fn register_listener<I: Ip>(&mut self, id: ListenerId<I>, socket: zx::Socket) {
        match I::VERSION {
            IpVersion::V4 => assert_matches!(self.tcp_v4_listeners.insert(id.into(), socket), None),
            IpVersion::V6 => assert_matches!(self.tcp_v6_listeners.insert(id.into(), socket), None),
        }
    }

    fn unregister_listener<I: Ip>(&mut self, id: ListenerId<I>) -> zx::Socket {
        match I::VERSION {
            IpVersion::V4 => {
                self.tcp_v4_listeners.remove(id.into()).expect("invalid v4 ListenerId")
            }
            IpVersion::V6 => {
                self.tcp_v6_listeners.remove(id.into()).expect("invalid v6 ListenerId")
            }
        }
    }
}

/// Local end of a zircon socket pair which will be later provided to state
/// machine inside Core.
#[derive(Debug)]
pub(crate) struct LocalZirconSocketAndNotifier(Arc<zx::Socket>, NeedsDataNotifier);

impl IntoBuffers<ReceiveBufferWithZirconSocket, SendBufferWithZirconSocket>
    for LocalZirconSocketAndNotifier
{
    fn into_buffers(
        self,
        buffer_sizes: BufferSizes,
    ) -> (ReceiveBufferWithZirconSocket, SendBufferWithZirconSocket) {
        let Self(socket, notifier) = self;
        let BufferSizes { send } = buffer_sizes;
        socket
            .signal_peer(zx::Signals::NONE, ZXSIO_SIGNAL_CONNECTED)
            .expect("failed to signal that the connection is established");
        notifier.schedule();
        (
            ReceiveBufferWithZirconSocket::new(Arc::clone(&socket)),
            SendBufferWithZirconSocket::new(socket, notifier, send),
        )
    }
}

impl Takeable for LocalZirconSocketAndNotifier {
    fn take(&mut self) -> Self {
        let Self(socket, notifier) = self;
        Self(Arc::clone(socket), notifier.clone())
    }
}

/// The peer end of the zircon socket that will later be vended to application,
/// together with objects that are used to receive signals from application.
#[derive(Debug)]
pub(crate) struct PeerZirconSocketAndWatcher {
    peer: zx::Socket,
    watcher: NeedsDataWatcher,
    socket: Arc<zx::Socket>,
}

impl TcpNonSyncContext for crate::bindings::BindingsNonSyncCtxImpl {
    type ReceiveBuffer = ReceiveBufferWithZirconSocket;
    type SendBuffer = SendBufferWithZirconSocket;
    type ReturnedBuffers = PeerZirconSocketAndWatcher;
    type ProvidedBuffers = LocalZirconSocketAndNotifier;

    fn on_new_connection<I: Ip>(&mut self, listener: ListenerId<I>) {
        let socket = match I::VERSION {
            IpVersion::V4 => self.tcp_v4_listeners.get(listener.into()).expect("invalid listener"),
            IpVersion::V6 => self.tcp_v6_listeners.get(listener.into()).expect("invalid listener"),
        };
        socket
            .signal_peer(zx::Signals::NONE, ZXSIO_SIGNAL_INCOMING)
            .expect("failed to signal that the new connection is available");
    }

    fn new_passive_open_buffers(
        buffer_sizes: BufferSizes,
    ) -> (Self::ReceiveBuffer, Self::SendBuffer, Self::ReturnedBuffers) {
        let (local, peer) =
            zx::Socket::create(zx::SocketOpts::STREAM).expect("failed to create sockets");
        let socket = Arc::new(local);
        let notifier = NeedsDataNotifier::default();
        let watcher = notifier.watcher();
        let (rbuf, sbuf) =
            LocalZirconSocketAndNotifier(Arc::clone(&socket), notifier).into_buffers(buffer_sizes);
        (rbuf, sbuf, PeerZirconSocketAndWatcher { peer, socket, watcher })
    }
}

#[derive(Debug)]
pub(crate) struct ReceiveBufferWithZirconSocket {
    socket: Arc<zx::Socket>,
    capacity: usize,
    out_of_order: RingBuffer,
}

impl ReceiveBufferWithZirconSocket {
    fn new(socket: Arc<zx::Socket>) -> Self {
        let capacity = socket.info().expect("failed to get socket info").tx_buf_max;
        Self { capacity, socket, out_of_order: RingBuffer::default() }
    }
}

impl Takeable for ReceiveBufferWithZirconSocket {
    fn take(&mut self) -> Self {
        core::mem::replace(
            self,
            Self {
                capacity: self.capacity,
                socket: Arc::clone(&self.socket),
                out_of_order: RingBuffer::new(0),
            },
        )
    }
}

impl Buffer for ReceiveBufferWithZirconSocket {
    fn len(&self) -> usize {
        let info = self.socket.info().expect("failed to get socket info");
        info.tx_buf_size
    }
}

impl ReceiveBuffer for ReceiveBufferWithZirconSocket {
    fn cap(&self) -> usize {
        self.capacity
    }

    fn write_at<P: Payload>(&mut self, offset: usize, data: &P) -> usize {
        self.out_of_order.write_at(offset, data)
    }

    fn make_readable(&mut self, count: usize) {
        self.out_of_order.make_readable(count);
        let mut shut_rd = false;
        let nread = self.out_of_order.read_with(|avail| {
            let mut total = 0;
            for chunk in avail {
                let written = match self.socket.write(*chunk) {
                    Ok(n) => n,
                    Err(zx::Status::BAD_STATE | zx::Status::PEER_CLOSED) => {
                        // These two status codes correspond two possible cases
                        // where the socket has been shutdown for read:
                        //   - BAD_STATE, the application has called `shutdown`,
                        //     but fido is still holding onto the peer socket.
                        //   - PEER_CLOSED, the application has called `close`,
                        //     or the socket is implicitly closed because the
                        //     application exits, fido is no longer holding onto
                        //     the peer socket, nor do we hold it in our
                        //     `SocketWorker` as it gets dropped after serving
                        //     the last request.
                        // In either case, we just discard the incoming bytes.
                        shut_rd = true;
                        return total;
                    }
                    Err(err) => panic!("failed to write into the zircon socket: {:?}", err),
                };
                assert_eq!(written, chunk.len());
                total += chunk.len();
            }
            total
        });
        // TODO(https://fxbug.dev/112391): Instead of inferring the state in
        // Bindings, we can reclaim the memory more promptly by teaching Core
        // about SHUT_RD.
        if shut_rd {
            if self.out_of_order.cap() != 0 {
                self.out_of_order = RingBuffer::new(0);
            }
            return;
        }
        assert_eq!(count, nread);
    }
}

impl Drop for ReceiveBufferWithZirconSocket {
    fn drop(&mut self) {
        // Make sure the FDIO is aware that we are not writing anymore so that
        // it can transition into the right state.
        self.socket
            .set_disposition(
                /* disposition */ Some(zx::SocketWriteDisposition::Disabled),
                /* peer_disposition */ None,
            )
            .expect("failed to set socket disposition");
    }
}

#[derive(Debug)]
pub(crate) struct SendBufferWithZirconSocket {
    capacity: usize,
    socket: Arc<zx::Socket>,
    ready_to_send: RingBuffer,
    notifier: NeedsDataNotifier,
}

impl Buffer for SendBufferWithZirconSocket {
    fn len(&self) -> usize {
        let info = self.socket.info().expect("failed to get socket info");
        info.rx_buf_size + self.ready_to_send.len()
    }
}

impl Takeable for SendBufferWithZirconSocket {
    fn take(&mut self) -> Self {
        let Self { capacity, socket, ready_to_send: data, notifier } = self;
        Self {
            capacity: *capacity,
            socket: Arc::clone(socket),
            ready_to_send: std::mem::replace(data, RingBuffer::new(0)),
            notifier: notifier.clone(),
        }
    }
}

impl SendBufferWithZirconSocket {
    /// The minimum send buffer size, in bytes.
    ///
    /// Borrowed from Linux: https://man7.org/linux/man-pages/man7/socket.7.html
    const MIN_CAPACITY: usize = 2048;
    /// The maximum send buffer size in bytes.
    ///
    /// 4MiB was picked to match Linux's behavior.
    const MAX_CAPACITY: usize = 1 << 22;

    fn new(socket: Arc<zx::Socket>, notifier: NeedsDataNotifier, target_capacity: usize) -> Self {
        let ring_buffer_size =
            usize::min(usize::max(target_capacity, Self::MIN_CAPACITY), Self::MAX_CAPACITY);
        let ready_to_send = RingBuffer::new(ring_buffer_size);
        let info = socket.info().expect("failed to get socket info");
        let capacity = info.rx_buf_max + ready_to_send.cap();
        Self { capacity, socket, ready_to_send, notifier }
    }

    fn poll(&mut self) {
        let want_bytes = self.ready_to_send.cap() - self.ready_to_send.len();
        if want_bytes == 0 {
            return;
        }
        let write_result =
            self.ready_to_send.writable_regions().into_iter().try_fold(0, |acc, b| {
                match self.socket.read(b) {
                    Ok(n) => {
                        if n == b.len() {
                            ControlFlow::Continue(acc + n)
                        } else {
                            ControlFlow::Break(acc + n)
                        }
                    }
                    Err(
                        zx::Status::SHOULD_WAIT | zx::Status::PEER_CLOSED | zx::Status::BAD_STATE,
                    ) => ControlFlow::Break(acc),
                    Err(e) => panic!("failed to read from the zircon socket: {:?}", e),
                }
            });
        let (ControlFlow::Continue(bytes_written) | ControlFlow::Break(bytes_written)) =
            write_result;

        self.ready_to_send.make_readable(bytes_written);
        if bytes_written < want_bytes {
            debug_assert!(write_result.is_break());
            self.notifier.schedule();
        }
    }
}

impl SendBuffer for SendBufferWithZirconSocket {
    fn mark_read(&mut self, count: usize) {
        self.ready_to_send.mark_read(count);
        self.poll()
    }

    fn peek_with<'a, F, R>(&'a mut self, offset: usize, f: F) -> R
    where
        F: FnOnce(SendPayload<'a>) -> R,
    {
        self.poll();
        self.ready_to_send.peek_with(offset, f)
    }
}

struct SocketWorker<I: IpExt, C> {
    id: SocketId<I>,
    ctx: C,
    peer: zx::Socket,
    _marker: IpVersionMarker<I>,
}

impl<I: IpExt, C> SocketWorker<I, C> {
    fn new(id: SocketId<I>, ctx: C, peer: zx::Socket) -> Self {
        Self { id, ctx, peer, _marker: Default::default() }
    }
}

pub(super) async fn spawn_worker<C>(
    domain: fposix_socket::Domain,
    proto: fposix_socket::StreamSocketProtocol,
    ctx: C,
    request_stream: fposix_socket::StreamSocketRequestStream,
) -> Result<(), fposix::Errno>
where
    C: LockableContext,
    C: Clone + Send + Sync + 'static,
    C::NonSyncCtx: SocketWorkerDispatcher + AsRef<Devices<DeviceId<StackTime>>>,
{
    let (local, peer) = zx::Socket::create(zx::SocketOpts::STREAM)
        .map_err(|_: zx::Status| fposix::Errno::Enobufs)?;
    let socket = Arc::new(local);
    match (domain, proto) {
        (fposix_socket::Domain::Ipv4, fposix_socket::StreamSocketProtocol::Tcp) => {
            let id = {
                let mut guard = ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                SocketId::Unbound(
                    create_socket::<Ipv4, _>(sync_ctx, non_sync_ctx),
                    LocalZirconSocketAndNotifier(Arc::clone(&socket), NeedsDataNotifier::default()),
                )
            };
            let worker = SocketWorker::<Ipv4, C>::new(id, ctx.clone(), peer);
            Ok(worker.spawn(request_stream))
        }
        (fposix_socket::Domain::Ipv6, fposix_socket::StreamSocketProtocol::Tcp) => {
            let id = {
                let mut guard = ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                SocketId::Unbound(
                    create_socket::<Ipv6, _>(sync_ctx, non_sync_ctx),
                    LocalZirconSocketAndNotifier(Arc::clone(&socket), NeedsDataNotifier::default()),
                )
            };
            let worker = SocketWorker::<Ipv6, C>::new(id, ctx.clone(), peer);
            Ok(worker.spawn(request_stream))
        }
    }
}

impl IntoErrno for AcceptError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            AcceptError::WouldBlock => fposix::Errno::Eagain,
        }
    }
}

impl IntoErrno for BindError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            BindError::NoLocalAddr => fposix::Errno::Eaddrnotavail,
            BindError::NoPort => fposix::Errno::Eaddrinuse,
            BindError::Conflict => fposix::Errno::Eaddrinuse,
        }
    }
}

impl IntoErrno for ConnectError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            ConnectError::NoRoute => fposix::Errno::Enetunreach,
            ConnectError::NoPort => fposix::Errno::Eaddrnotavail,
        }
    }
}

impl IntoErrno for NoConnection {
    fn into_errno(self) -> fidl_fuchsia_posix::Errno {
        fposix::Errno::Enotconn
    }
}

/// Spawns a task that sends more data from the `socket` each time we observe
/// a wakeup through the `watcher`.
fn spawn_send_task<I: IpExt, C>(
    ctx: C,
    socket: Arc<zx::Socket>,
    watcher: NeedsDataWatcher,
    id: ConnectionId<I>,
) where
    C: LockableContext,
    C: Clone + Send + Sync + 'static,
    C::NonSyncCtx: SocketWorkerDispatcher,
{
    fasync::Task::spawn(async move {
        watcher
            .for_each(|()| async {
                let observed = fasync::OnSignals::new(&*socket, zx::Signals::SOCKET_READABLE)
                    .await
                    .expect("failed to observe signals on zircon socket");
                assert!(observed.contains(zx::Signals::SOCKET_READABLE));
                let mut guard = ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                netstack3_core::transport::tcp::socket::do_send::<I, _>(
                    sync_ctx,
                    non_sync_ctx,
                    id.into(),
                );
            })
            .await
    })
    .detach();
}

impl<I: IpSockAddrExt + IpExt, C> SocketWorker<I, C>
where
    C: LockableContext,
    C: Clone + Send + Sync + 'static,
    C::NonSyncCtx: SocketWorkerDispatcher + AsRef<Devices<DeviceId<StackTime>>>,
{
    fn spawn(mut self, request_stream: fposix_socket::StreamSocketRequestStream) {
        fasync::Task::spawn(async move {
            // Keep a set of futures, one per pollable stream. Each future is a
            // `StreamFuture` and so will resolve into a tuple of the next item
            // in the stream and the rest of the stream.
            let mut futures = OneOrMany::new(request_stream.into_future());
            // Process requests in order as they arrive. When the last close
            // request arrives, save it so it can be deferred until after the
            // socket state is removed from Core.
            let last_close = loop {
                let Some((request, request_stream)) = futures.next().await else {
                    // No close request needs to be deferred.
                    break None
                };

                let request = match request {
                    None => continue,
                    Some(Err(e)) => {
                        log::warn!("got {} while processing stream requests", e);
                        continue;
                    }
                    Some(Ok(request)) => request,
                };

                match self.handle_request(request).await {
                    ControlFlow::Continue(None) => {}
                    ControlFlow::Break(close_responder) => {
                        let do_close = move || {
                            responder_send!(close_responder, &mut Ok(()));
                            request_stream.control_handle().shutdown();
                        };
                        if futures.is_empty() {
                            break Some(do_close);
                        }
                        do_close();
                        continue;
                    }
                    ControlFlow::Continue(Some(new_request_stream)) => {
                        futures.push(new_request_stream.into_future())
                    }
                }
                // `request_stream` received above is the tail of the stream,
                // which might have more requests. Stick it back into the
                // pending future set so we can receive and service those
                // requests. If the stream is exhausted or cancelled due to a
                // `Shutdown` response, it will be dropped internally by the
                // `FuturesUnordered`.
                futures.push(request_stream.into_future())
            };

            let mut guard = self.ctx.lock().await;
            let Ctx { sync_ctx, non_sync_ctx } = &mut *guard;
            match self.id {
                SocketId::Unbound(unbound, _) => remove_unbound::<I, _>(sync_ctx, unbound),
                SocketId::Bound(bound, _) => remove_bound::<I, _>(sync_ctx, bound),
                SocketId::Connection(conn) => close_conn::<I, _>(sync_ctx, non_sync_ctx, conn),
                SocketId::Listener(listener) => {
                    let bound = shutdown_listener::<I, _>(sync_ctx, non_sync_ctx, listener);
                    let _: zx::Socket = non_sync_ctx.unregister_listener(listener);
                    remove_bound::<I, _>(sync_ctx, bound)
                }
            }

            if let Some(do_close) = last_close {
                do_close()
            }
        })
        .detach()
    }

    async fn bind(&mut self, addr: fnet::SocketAddress) -> Result<(), fposix::Errno> {
        match self.id {
            SocketId::Unbound(unbound, ref mut local_socket) => {
                let addr = I::SocketAddress::from_sock_addr(addr)?;
                let mut guard = self.ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                let bound = bind::<I, _>(
                    sync_ctx,
                    non_sync_ctx,
                    unbound,
                    addr.addr(),
                    NonZeroU16::new(addr.port()),
                )
                .map_err(IntoErrno::into_errno)?;
                self.id = SocketId::Bound(bound, local_socket.take());
                Ok(())
            }
            SocketId::Bound(_, _) | SocketId::Connection(_) | SocketId::Listener(_) => {
                Err(fposix::Errno::Einval)
            }
        }
    }

    async fn connect(&mut self, addr: fnet::SocketAddress) -> Result<(), fposix::Errno> {
        let addr = I::SocketAddress::from_sock_addr(addr)?;
        let ip = SpecifiedAddr::new(addr.addr()).unwrap_or(I::LOOPBACK_ADDRESS);
        let port = NonZeroU16::new(addr.port()).ok_or(fposix::Errno::Einval)?;
        let mut guard = self.ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
        let (connection, socket, watcher) = match self.id {
            SocketId::Bound(bound, LocalZirconSocketAndNotifier(ref socket, ref notifier)) => {
                let connection = connect_bound::<I, _>(
                    sync_ctx,
                    non_sync_ctx,
                    bound,
                    SocketAddr { ip, port },
                    LocalZirconSocketAndNotifier(Arc::clone(socket), notifier.clone()),
                )
                .map_err(IntoErrno::into_errno)?;
                Ok((connection, Arc::clone(socket), notifier.watcher()))
            }
            SocketId::Unbound(unbound, LocalZirconSocketAndNotifier(ref socket, ref notifier)) => {
                let connected = connect_unbound::<I, _>(
                    sync_ctx,
                    non_sync_ctx,
                    unbound,
                    SocketAddr { ip, port },
                    LocalZirconSocketAndNotifier(Arc::clone(socket), notifier.clone()),
                )
                .map_err(IntoErrno::into_errno)?;
                Ok((connected, Arc::clone(socket), notifier.watcher()))
            }
            SocketId::Listener(_) => Err(fposix::Errno::Einval),
            SocketId::Connection(_) => Err(fposix::Errno::Eisconn),
        }?;
        spawn_send_task::<I, _>(self.ctx.clone(), socket, watcher, connection);
        self.id = SocketId::Connection(connection);
        Ok(())
    }

    async fn listen(&mut self, backlog: i16) -> Result<(), fposix::Errno> {
        match self.id {
            SocketId::Bound(bound, ref mut local_socket) => {
                let mut guard = self.ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                let backlog = NonZeroUsize::new(backlog as usize).ok_or(fposix::Errno::Einval)?;
                let listener = listen::<I, _>(sync_ctx, non_sync_ctx, bound, backlog);
                let LocalZirconSocketAndNotifier(local, _) = local_socket.take();
                self.id = SocketId::Listener(listener);
                non_sync_ctx.register_listener(
                    listener,
                    Arc::try_unwrap(local)
                        .expect("the local end of the socket should never be shared"),
                );
                Ok(())
            }
            SocketId::Unbound(_, _) | SocketId::Connection(_) | SocketId::Listener(_) => {
                Err(fposix::Errno::Einval)
            }
        }
    }

    async fn get_sock_name(&self) -> Result<fnet::SocketAddress, fposix::Errno> {
        let mut guard = self.ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx: _ } = guard.deref_mut();
        let fidl = match self.id {
            SocketId::Unbound(_, _) => return Err(fposix::Errno::Einval),
            SocketId::Bound(id, _) => {
                let BoundInfo { addr, port, device } = get_bound_info::<I, _>(sync_ctx, id);
                // TODO(https://fxbug.dev/102103): Support setting device.
                assert_eq!(device, None);
                (addr, port).into_fidl()
            }
            SocketId::Listener(id) => {
                let BoundInfo { addr, port, device } = get_listener_info::<I, _>(sync_ctx, id);
                // TODO(https://fxbug.dev/102103): Support setting device.
                assert_eq!(device, None);
                (addr, port).into_fidl()
            }
            SocketId::Connection(id) => {
                let ConnectionInfo { local_addr, remote_addr: _, device } =
                    get_connection_info::<I, _>(sync_ctx, id);
                // TODO(https://fxbug.dev/102103): Support setting device.
                assert_eq!(device, None);
                local_addr.into_fidl()
            }
        };
        Ok(fidl.into_sock_addr())
    }

    async fn get_peer_name(&self) -> Result<fnet::SocketAddress, fposix::Errno> {
        let mut guard = self.ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx: _ } = guard.deref_mut();
        match self.id {
            SocketId::Unbound(_, _) | SocketId::Bound(_, _) | SocketId::Listener(_) => {
                Err(fposix::Errno::Enotconn)
            }
            SocketId::Connection(id) => Ok({
                get_connection_info::<I, _>(sync_ctx, id).remote_addr.into_fidl().into_sock_addr()
            }),
        }
    }

    async fn accept(
        &mut self,
        want_addr: bool,
    ) -> Result<
        (Option<Box<fnet::SocketAddress>>, ClientEnd<fposix_socket::StreamSocketMarker>),
        fposix::Errno,
    > {
        match self.id {
            SocketId::Listener(listener) => {
                let mut guard = self.ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                let (accepted, SocketAddr { ip, port }, peer) =
                    accept::<I, _>(sync_ctx, non_sync_ctx, listener)
                        .map_err(IntoErrno::into_errno)?;
                let addr =
                    <I::SocketAddress as SockAddr>::new(Some(ZonedAddr::Unzoned(ip)), port.get());
                let PeerZirconSocketAndWatcher { peer, watcher, socket } = peer;
                let (client, request_stream) =
                    fidl::endpoints::create_request_stream::<fposix_socket::StreamSocketMarker>()
                        .expect("failed to create new fidl endpoints");
                spawn_send_task::<I, _>(self.ctx.clone(), socket, watcher, accepted);
                let worker = SocketWorker::<I, C>::new(
                    SocketId::Connection(accepted),
                    self.ctx.clone(),
                    peer,
                );
                worker.spawn(request_stream);
                Ok((want_addr.then(|| Box::new(addr.into_sock_addr())), client))
            }
            SocketId::Unbound(_, _) | SocketId::Connection(_) | SocketId::Bound(_, _) => {
                Err(fposix::Errno::Einval)
            }
        }
    }

    async fn shutdown(&mut self, mode: fposix_socket::ShutdownMode) -> Result<(), fposix::Errno> {
        match self.id {
            SocketId::Unbound(_, _) | SocketId::Bound(_, _) => Err(fposix::Errno::Enotconn),
            SocketId::Connection(conn_id) => {
                let mut my_disposition: Option<zx::SocketWriteDisposition> = None;
                let mut peer_disposition: Option<zx::SocketWriteDisposition> = None;
                if mode.contains(fposix_socket::ShutdownMode::WRITE) {
                    peer_disposition = Some(zx::SocketWriteDisposition::Disabled);
                    let mut guard = self.ctx.lock().await;
                    let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                    shutdown_conn::<I, _>(&sync_ctx, non_sync_ctx, conn_id)
                        .map_err(IntoErrno::into_errno)?;
                }
                if mode.contains(fposix_socket::ShutdownMode::READ) {
                    my_disposition = Some(zx::SocketWriteDisposition::Disabled);
                }
                self.peer
                    .set_disposition(peer_disposition, my_disposition)
                    .expect("failed to set socket disposition");
                Ok(())
            }
            SocketId::Listener(listener) => {
                if mode.contains(fposix_socket::ShutdownMode::READ) {
                    let mut guard = self.ctx.lock().await;
                    let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                    let bound = shutdown_listener::<I, _>(&sync_ctx, non_sync_ctx, listener);
                    let local = non_sync_ctx.unregister_listener(listener);
                    self.id = SocketId::Bound(
                        bound,
                        LocalZirconSocketAndNotifier(Arc::new(local), NeedsDataNotifier::default()),
                    );
                }
                Ok(())
            }
        }
    }

    async fn set_bind_to_device(&mut self, device: Option<&str>) -> Result<(), fposix::Errno> {
        let mut ctx = self.ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = ctx.deref_mut();
        let device = device
            .map(|name| {
                non_sync_ctx
                    .as_ref()
                    .get_device_by_name(name)
                    .map(|d| d.core_id().clone())
                    .ok_or(fposix::Errno::Enodev)
            })
            .transpose()?;

        match self.id {
            SocketId::Unbound(id, _) => {
                set_unbound_device(sync_ctx, non_sync_ctx, id, device);
                Ok(())
            }
            SocketId::Bound(id, _) => set_bound_device(sync_ctx, non_sync_ctx, id, device),
            SocketId::Listener(id) => set_listener_device(sync_ctx, non_sync_ctx, id, device),
            SocketId::Connection(id) => set_connection_device(sync_ctx, non_sync_ctx, id, device),
        }
        .map_err(IntoErrno::into_errno)
    }

    /// Returns a [`ControlFlow`] to indicate whether the parent stream should
    /// continue being polled or dropped.
    ///
    /// If `Some(stream)` is returned in the `Continue` case, `stream` is a new
    /// stream of events that should be polled concurrently with the parent
    /// stream.
    async fn handle_request(
        &mut self,
        request: fposix_socket::StreamSocketRequest,
    ) -> ControlFlow<
        fposix_socket::StreamSocketCloseResponder,
        Option<fposix_socket::StreamSocketRequestStream>,
    > {
        match request {
            fposix_socket::StreamSocketRequest::Bind { addr, responder } => {
                responder_send!(responder, &mut self.bind(addr).await);
            }
            fposix_socket::StreamSocketRequest::Connect { addr, responder } => {
                responder_send!(responder, &mut self.connect(addr).await);
            }
            fposix_socket::StreamSocketRequest::DescribeDeprecated { responder } => {
                let socket = self
                    .peer
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .expect("failed to duplicate the socket handle");
                responder_send!(
                    responder,
                    &mut fio::NodeInfoDeprecated::StreamSocket(fio::StreamSocket { socket })
                );
            }
            fposix_socket::StreamSocketRequest::Describe { responder } => {
                let socket = self
                    .peer
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .expect("failed to duplicate the socket handle");
                responder_send!(
                    responder,
                    fposix_socket::StreamSocketDescribeResponse {
                        socket: Some(socket),
                        ..fposix_socket::StreamSocketDescribeResponse::EMPTY
                    }
                );
            }
            fposix_socket::StreamSocketRequest::Listen { backlog, responder } => {
                responder_send!(responder, &mut self.listen(backlog).await);
            }
            fposix_socket::StreamSocketRequest::Accept { want_addr, responder } => {
                responder_send!(responder, &mut self.accept(want_addr).await);
            }
            fposix_socket::StreamSocketRequest::Reopen {
                rights_request,
                object_request: _,
                control_handle: _,
            } => {
                todo!("https://fxbug.dev/77623: rights_request={:?}", rights_request);
            }
            fposix_socket::StreamSocketRequest::Close { responder } => {
                // We don't just close the socket because this socket worker is
                // potentially shared by a bunch of sockets because the client
                // can call `dup` on this socket. We will do the cleanup at the
                // end of this task.
                return ControlFlow::Break(responder);
            }
            fposix_socket::StreamSocketRequest::GetConnectionInfo { responder: _ } => {
                todo!("https://fxbug.dev/77623");
            }
            fposix_socket::StreamSocketRequest::GetAttributes { query, responder: _ } => {
                todo!("https://fxbug.dev/77623: query={:?}", query);
            }
            fposix_socket::StreamSocketRequest::UpdateAttributes { payload, responder: _ } => {
                todo!("https://fxbug.dev/77623: attributes={:?}", payload);
            }
            fposix_socket::StreamSocketRequest::Sync { responder } => {
                responder_send!(responder, &mut Err(zx::Status::NOT_SUPPORTED.into_raw()));
            }
            fposix_socket::StreamSocketRequest::Clone { flags: _, object, control_handle: _ } => {
                let channel = fidl::AsyncChannel::from_channel(object.into_channel())
                    .expect("failed to create async channel");
                let events = fposix_socket::StreamSocketRequestStream::from_channel(channel);
                return ControlFlow::Continue(Some(events));
            }
            fposix_socket::StreamSocketRequest::Clone2 { request, control_handle: _ } => {
                let channel = fidl::AsyncChannel::from_channel(request.into_channel())
                    .expect("failed to create async channel");
                let events = fposix_socket::StreamSocketRequestStream::from_channel(channel);
                return ControlFlow::Continue(Some(events));
            }
            fposix_socket::StreamSocketRequest::SetBindToDevice { value, responder } => {
                let identifier = (!value.is_empty()).then_some(value.as_str());
                responder_send!(responder, &mut self.set_bind_to_device(identifier).await);
            }
            fposix_socket::StreamSocketRequest::GetAttr { responder } => {
                responder_send!(
                    responder,
                    zx::Status::NOT_SUPPORTED.into_raw(),
                    &mut fio::NodeAttributes {
                        mode: 0,
                        id: 0,
                        content_size: 0,
                        storage_size: 0,
                        link_count: 0,
                        creation_time: 0,
                        modification_time: 0
                    }
                );
            }
            fposix_socket::StreamSocketRequest::SetAttr { flags: _, attributes: _, responder } => {
                responder_send!(responder, zx::Status::NOT_SUPPORTED.into_raw());
            }
            fposix_socket::StreamSocketRequest::GetFlags { responder } => {
                responder_send!(
                    responder,
                    zx::Status::NOT_SUPPORTED.into_raw(),
                    fio::OpenFlags::empty()
                );
            }
            fposix_socket::StreamSocketRequest::SetFlags { flags: _, responder } => {
                responder_send!(responder, zx::Status::NOT_SUPPORTED.into_raw());
            }
            fposix_socket::StreamSocketRequest::Query { responder } => {
                responder_send!(responder, fposix_socket::STREAM_SOCKET_PROTOCOL_NAME.as_bytes());
            }
            fposix_socket::StreamSocketRequest::QueryFilesystem { responder } => {
                responder_send!(responder, zx::Status::NOT_SUPPORTED.into_raw(), None);
            }
            fposix_socket::StreamSocketRequest::SetReuseAddress { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetReuseAddress { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetError { responder } => {
                // TODO(https://fxbug.dev/103982): Retrieve the error.
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::SetBroadcast { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetBroadcast { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetSendBuffer { value_bytes: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetSendBuffer { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetReceiveBuffer { value_bytes: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetReceiveBuffer { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetKeepAlive { value: _, responder } => {
                error!("TODO(https://fxbug.dev/110483): implement the SO_KEEPALIVE socket option");
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetKeepAlive { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetOutOfBandInline { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetOutOfBandInline { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetNoCheck { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetNoCheck { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetLinger {
                linger: _,
                length_secs: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetLinger { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetReusePort { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetReusePort { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetAcceptConn { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetBindToDevice { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTimestampDeprecated { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTimestampDeprecated { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTimestamp { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTimestamp { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::Disconnect { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetSockName { responder } => {
                responder_send!(responder, &mut self.get_sock_name().await);
            }
            fposix_socket::StreamSocketRequest::GetPeerName { responder } => {
                responder_send!(responder, &mut self.get_peer_name().await);
            }
            fposix_socket::StreamSocketRequest::Shutdown { mode, responder } => {
                responder_send!(responder, &mut self.shutdown(mode).await);
            }
            fposix_socket::StreamSocketRequest::SetIpTypeOfService { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpTypeOfService { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpTtl { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpTtl { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpPacketInfo { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpPacketInfo { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpReceiveTypeOfService {
                value: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpReceiveTypeOfService { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpReceiveTtl { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpReceiveTtl { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpMulticastInterface {
                iface: _,
                address: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpMulticastInterface { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpMulticastTtl { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpMulticastTtl { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpMulticastLoopback { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpMulticastLoopback { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::AddIpMembership { membership: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::DropIpMembership { membership: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::AddIpv6Membership { membership: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::DropIpv6Membership { membership: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6MulticastInterface {
                value: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6MulticastInterface { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6UnicastHops { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6UnicastHops { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6ReceiveHopLimit { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6ReceiveHopLimit { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6MulticastHops { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6MulticastHops { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6MulticastLoopback {
                value: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6MulticastLoopback { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6Only { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6Only { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6ReceiveTrafficClass {
                value: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6ReceiveTrafficClass { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6TrafficClass { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6TrafficClass { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetIpv6ReceivePacketInfo {
                value: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetIpv6ReceivePacketInfo { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetInfo { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpNoDelay { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpNoDelay { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpMaxSegment { value_bytes: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpMaxSegment { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpCork { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpCork { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpKeepAliveIdle {
                value_secs: _,
                responder,
            } => {
                error!("TODO(https://fxbug.dev/110483): implement the TCP_KEEPIDLE socket option");
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetTcpKeepAliveIdle { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpKeepAliveInterval {
                value_secs: _,
                responder,
            } => {
                error!("TODO(https://fxbug.dev/110483): implement the TCP_KEEPINTVL socket option");
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetTcpKeepAliveInterval { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpKeepAliveCount { value: _, responder } => {
                error!("TODO(https://fxbug.dev/110483): implement the TCP_KEEPCNT socket option");
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetTcpKeepAliveCount { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpSynCount { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpSynCount { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpLinger { value_secs: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpLinger { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpDeferAccept { value_secs: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpDeferAccept { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpWindowClamp { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpWindowClamp { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpInfo { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpQuickAck { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpQuickAck { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpCongestion { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpCongestion { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::SetTcpUserTimeout {
                value_millis: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
            fposix_socket::StreamSocketRequest::GetTcpUserTimeout { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
            }
        }
        ControlFlow::Continue(None)
    }
}

impl<A: IpAddress> TryIntoFidl<<A::Version as IpSockAddrExt>::SocketAddress> for SocketAddr<A>
where
    A::Version: IpSockAddrExt,
{
    type Error = Never;

    fn try_into_fidl(self) -> Result<<A::Version as IpSockAddrExt>::SocketAddress, Self::Error> {
        let Self { ip, port } = self;
        Ok((Some(ip), port).into_fidl())
    }
}

#[cfg(test)]
mod tests {
    use test_case::test_case;

    use super::*;

    const TEST_BYTES: &'static [u8] = b"Hello";

    #[test]
    fn receive_buffer() {
        let (local, peer) =
            zx::Socket::create(zx::SocketOpts::STREAM).expect("failed to create zircon socket");
        let mut rbuf = ReceiveBufferWithZirconSocket::new(Arc::new(local));
        assert_eq!(rbuf.write_at(0, &TEST_BYTES), TEST_BYTES.len());
        assert_eq!(rbuf.write_at(TEST_BYTES.len() * 2, &TEST_BYTES), TEST_BYTES.len());
        assert_eq!(rbuf.write_at(TEST_BYTES.len(), &TEST_BYTES), TEST_BYTES.len());
        rbuf.make_readable(TEST_BYTES.len() * 3);
        let mut buf = [0u8; TEST_BYTES.len() * 3];
        assert_eq!(rbuf.len(), TEST_BYTES.len() * 3);
        assert_eq!(peer.read(&mut buf), Ok(TEST_BYTES.len() * 3));
        assert_eq!(&buf, b"HelloHelloHello");
    }

    #[test]
    fn send_buffer() {
        let (local, peer) =
            zx::Socket::create(zx::SocketOpts::STREAM).expect("failed to create zircon socket");
        let notifier = NeedsDataNotifier::default();
        let mut sbuf =
            SendBufferWithZirconSocket::new(Arc::new(local), notifier, u16::MAX as usize);
        assert_eq!(peer.write(TEST_BYTES), Ok(TEST_BYTES.len()));
        assert_eq!(sbuf.len(), TEST_BYTES.len());
        sbuf.peek_with(0, |avail| {
            assert_eq!(avail, SendPayload::Contiguous(TEST_BYTES));
        });
        assert_eq!(peer.write(TEST_BYTES), Ok(TEST_BYTES.len()));
        assert_eq!(sbuf.len(), TEST_BYTES.len() * 2);
        sbuf.mark_read(TEST_BYTES.len());
        assert_eq!(sbuf.len(), TEST_BYTES.len());
        sbuf.peek_with(0, |avail| {
            assert_eq!(avail, SendPayload::Contiguous(TEST_BYTES));
        });
    }

    #[test_case(0, SendBufferWithZirconSocket::MIN_CAPACITY; "below min")]
    #[test_case(1 << 16, 1 << 16; "in range")]
    #[test_case(1 << 32, SendBufferWithZirconSocket::MAX_CAPACITY; "above max")]
    fn send_buffer_limits(target: usize, expected: usize) {
        let (local, _peer) =
            zx::Socket::create(zx::SocketOpts::STREAM).expect("failed to create zircon socket");
        let notifier = NeedsDataNotifier::default();
        let sbuf = SendBufferWithZirconSocket::new(Arc::new(local), notifier, target);
        let ring_buffer_capacity = sbuf.capacity - sbuf.socket.info().unwrap().rx_buf_max;
        assert_eq!(ring_buffer_capacity, expected)
    }
}
