// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Stream sockets, primarily TCP sockets.

use std::{
    convert::Infallible as Never,
    num::{NonZeroU16, NonZeroU64, NonZeroU8, NonZeroUsize, TryFromIntError},
    ops::{ControlFlow, DerefMut as _},
    sync::Arc,
    time::Duration,
};

use assert_matches::assert_matches;
use explicit::ResultExt as _;
use fidl::{
    endpoints::{ClientEnd, RequestStream as _},
    AsHandleRef as _, HandleBased as _,
};
use fidl_fuchsia_net as fnet;
use fidl_fuchsia_posix as fposix;
use fidl_fuchsia_posix_socket as fposix_socket;
use fuchsia_async as fasync;
use fuchsia_zircon::{self as zx, Peered as _};
use futures::StreamExt as _;
use net_types::{
    ip::{Ip, IpAddress, IpVersion, Ipv4, Ipv6},
    ZonedAddr,
};
use netstack3_core::{
    device::{DeviceId, WeakDeviceId},
    ip::IpExt,
    transport::tcp::{
        self,
        buffer::{
            Buffer, BufferLimits, IntoBuffers, ReceiveBuffer, RingBuffer, SendBuffer, SendPayload,
        },
        segment::Payload,
        socket::{
            accept, bind, close_conn, connect_bound, connect_unbound, create_socket,
            get_bound_info, get_connection_info, get_listener_info, listen, receive_buffer_size,
            remove_bound, remove_unbound, reuseaddr, send_buffer_size, set_bound_device,
            set_connection_device, set_listener_device, set_receive_buffer_size,
            set_reuseaddr_bound, set_reuseaddr_listener, set_reuseaddr_unbound,
            set_send_buffer_size, set_unbound_device, shutdown_conn, shutdown_listener,
            with_socket_options, with_socket_options_mut, AcceptError, BoundId, BoundInfo,
            ConnectError, ConnectionId, ConnectionInfo, ListenError, ListenerId, NoConnection,
            SetReuseAddrError, SocketAddr, UnboundId,
        },
        state::Takeable,
        BufferSizes, SocketOptions,
    },
    Ctx, SyncCtx,
};
use nonzero_ext::nonzero;
use once_cell::sync::Lazy;
use packet_formats::utils::NonZeroDuration;

use crate::bindings::{
    socket::{
        worker::{self, CloseResponder, SocketWorker},
        IntoErrno, IpSockAddrExt, SockAddr, SocketWorkerProperties, ZXSIO_SIGNAL_CONNECTED,
        ZXSIO_SIGNAL_INCOMING,
    },
    util::{
        ConversionContext, DeviceNotFoundError, NeedsDataNotifier, NeedsDataWatcher,
        TryFromFidlWithContext, TryIntoCoreWithContext, TryIntoFidlWithContext,
    },
    BindingsNonSyncCtxImpl, NetstackContext,
};

/// Maximum values allowed on linux: https://github.com/torvalds/linux/blob/0326074ff4652329f2a1a9c8685104576bd8d131/include/net/tcp.h#L159-L161
const MAX_TCP_KEEPIDLE_SECS: u64 = 32767;
const MAX_TCP_KEEPINTVL_SECS: u64 = 32767;
const MAX_TCP_KEEPCNT: u8 = 127;

#[derive(Debug)]
enum SocketId<I: Ip> {
    Unbound(UnboundId<I>, LocalZirconSocketAndNotifier),
    Bound(BoundId<I>, LocalZirconSocketAndNotifier),
    Connection(ConnectionId<I>, bool),
    Listener(ListenerId<I>),
}

#[derive(Debug)]
pub(crate) struct ListenerState(zx::Socket);

pub(crate) trait SocketWorkerDispatcher:
    tcp::socket::NonSyncContext<
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

    /// Returns a mutable reference to state for an existing listener.
    ///
    /// # Panics
    ///
    /// Panics if `id` does not correspond to a listener.
    fn get_listener_mut<I: Ip>(&mut self, id: ListenerId<I>) -> &mut ListenerState;
}

impl SocketWorkerDispatcher for BindingsNonSyncCtxImpl {
    fn register_listener<I: Ip>(&mut self, id: ListenerId<I>, socket: zx::Socket) {
        let state = ListenerState(socket);
        match I::VERSION {
            IpVersion::V4 => assert_matches!(self.tcp_v4_listeners.insert(id.into(), state), None),
            IpVersion::V6 => assert_matches!(self.tcp_v6_listeners.insert(id.into(), state), None),
        }
    }

    fn unregister_listener<I: Ip>(&mut self, id: ListenerId<I>) -> zx::Socket {
        let ListenerState(socket) = match I::VERSION {
            IpVersion::V4 => {
                self.tcp_v4_listeners.remove(id.into()).expect("invalid v4 ListenerId")
            }
            IpVersion::V6 => {
                self.tcp_v6_listeners.remove(id.into()).expect("invalid v6 ListenerId")
            }
        };
        socket
    }

    fn get_listener_mut<I: Ip>(&mut self, id: ListenerId<I>) -> &mut ListenerState {
        match I::VERSION {
            IpVersion::V4 => {
                self.tcp_v4_listeners.get_mut(id.into()).expect("invalid v4 ListenerId")
            }
            IpVersion::V6 => {
                self.tcp_v6_listeners.get_mut(id.into()).expect("invalid v6 ListenerId")
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
        let BufferSizes { send, receive } = buffer_sizes;
        socket
            .signal_peer(zx::Signals::NONE, ZXSIO_SIGNAL_CONNECTED)
            .expect("failed to signal that the connection is established");
        notifier.schedule();
        (
            ReceiveBufferWithZirconSocket::new(Arc::clone(&socket), receive),
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

impl tcp::socket::NonSyncContext for BindingsNonSyncCtxImpl {
    type ReceiveBuffer = ReceiveBufferWithZirconSocket;
    type SendBuffer = SendBufferWithZirconSocket;
    type ReturnedBuffers = PeerZirconSocketAndWatcher;
    type ProvidedBuffers = LocalZirconSocketAndNotifier;

    fn on_waiting_connections_change<I: Ip>(&mut self, listener: ListenerId<I>, count: usize) {
        let ListenerState(socket) = self.get_listener_mut(listener);
        if count == 0 {
            socket.signal_peer(ZXSIO_SIGNAL_INCOMING, zx::Signals::NONE)
        } else {
            socket.signal_peer(zx::Signals::NONE, ZXSIO_SIGNAL_INCOMING)
        }
        .expect("failed to signal for available connections");
    }

    fn new_passive_open_buffers(
        buffer_sizes: BufferSizes,
    ) -> (Self::ReceiveBuffer, Self::SendBuffer, Self::ReturnedBuffers) {
        let (local, peer) = zx::Socket::create_stream();
        let socket = Arc::new(local);
        let notifier = NeedsDataNotifier::default();
        let watcher = notifier.watcher();
        let (rbuf, sbuf) =
            LocalZirconSocketAndNotifier(Arc::clone(&socket), notifier).into_buffers(buffer_sizes);
        (rbuf, sbuf, PeerZirconSocketAndWatcher { peer, socket, watcher })
    }

    fn default_buffer_sizes() -> BufferSizes {
        static ZIRCON_SOCKET_BUFFER_SIZE: Lazy<usize> = Lazy::new(|| {
            let (local, _peer) = zx::Socket::create_stream();
            local.info().unwrap().tx_buf_max
        });
        static RING_BUFFER_DEFAULT_SIZE: Lazy<usize> =
            Lazy::new(|| RingBuffer::default().target_capacity());

        BufferSizes { receive: *ZIRCON_SOCKET_BUFFER_SIZE, send: *RING_BUFFER_DEFAULT_SIZE }
    }
}

#[derive(Debug)]
pub(crate) struct ReceiveBufferWithZirconSocket {
    socket: Arc<zx::Socket>,
    zx_socket_capacity: usize,
    // Invariant: `out_of_order` can never hold more bytes than
    // `zx_socket_capacity`.
    out_of_order: RingBuffer,
}

impl ReceiveBufferWithZirconSocket {
    /// The minimum receive buffer size, in bytes.
    ///
    /// Borrowed from Linux: https://man7.org/linux/man-pages/man7/socket.7.html
    const MIN_CAPACITY: usize = 256;

    fn new(socket: Arc<zx::Socket>, target_capacity: usize) -> Self {
        let info = socket.info().expect("failed to get socket info");
        let zx_socket_capacity = info.tx_buf_max;
        assert!(
            zx_socket_capacity >= Self::MIN_CAPACITY,
            "Zircon socket buffer is too small, {} < {}",
            zx_socket_capacity,
            Self::MIN_CAPACITY
        );

        let ring_buffer_size =
            usize::min(usize::max(target_capacity, Self::MIN_CAPACITY), zx_socket_capacity);
        let out_of_order = RingBuffer::new(ring_buffer_size);
        Self { zx_socket_capacity, socket, out_of_order }
    }
}

impl Takeable for ReceiveBufferWithZirconSocket {
    fn take(&mut self) -> Self {
        core::mem::replace(
            self,
            Self {
                zx_socket_capacity: self.zx_socket_capacity,
                socket: Arc::clone(&self.socket),
                out_of_order: RingBuffer::new(0),
            },
        )
    }
}

impl Buffer for ReceiveBufferWithZirconSocket {
    fn limits(&self) -> BufferLimits {
        let Self { socket, out_of_order, zx_socket_capacity } = self;
        let BufferLimits { len: _, capacity: out_of_order_capacity } = out_of_order.limits();

        debug_assert!(
            *zx_socket_capacity >= out_of_order_capacity,
            "ring buffer should never be this large; {} > {}",
            out_of_order_capacity,
            *zx_socket_capacity
        );

        let info = socket.info().expect("failed to get socket info");
        let len = info.tx_buf_size;
        // Ensure that capacity is always at least as large as the length, but
        // also reflects the requested capacity.
        let capacity = usize::max(len, out_of_order_capacity);
        BufferLimits { len, capacity }
    }

    fn target_capacity(&self) -> usize {
        let Self { socket: _, zx_socket_capacity: _, out_of_order } = self;
        out_of_order.target_capacity()
    }

    fn request_capacity(&mut self, size: usize) {
        let Self { zx_socket_capacity, socket: _, out_of_order } = self;

        let ring_buffer_size =
            usize::min(usize::max(size, Self::MIN_CAPACITY), *zx_socket_capacity);

        out_of_order.set_target_size(ring_buffer_size);
    }
}

impl ReceiveBuffer for ReceiveBufferWithZirconSocket {
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
            let BufferLimits { len: _, capacity } = self.out_of_order.limits();
            if capacity != 0 {
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
    zx_socket_capacity: usize,
    socket: Arc<zx::Socket>,
    ready_to_send: RingBuffer,
    notifier: NeedsDataNotifier,
}

impl Buffer for SendBufferWithZirconSocket {
    fn limits(&self) -> BufferLimits {
        let Self { zx_socket_capacity, socket, ready_to_send, notifier: _ } = self;
        let info = socket.info().expect("failed to get socket info");

        let BufferLimits { capacity: ready_to_send_capacity, len: ready_to_send_len } =
            ready_to_send.limits();
        let len = info.rx_buf_size + ready_to_send_len;
        let capacity = *zx_socket_capacity + ready_to_send_capacity;
        BufferLimits { capacity, len }
    }

    fn target_capacity(&self) -> usize {
        let Self { zx_socket_capacity, socket: _, ready_to_send, notifier: _ } = self;
        *zx_socket_capacity + ready_to_send.target_capacity()
    }

    fn request_capacity(&mut self, size: usize) {
        let ring_buffer_size = usize::min(usize::max(size, Self::MIN_CAPACITY), Self::MAX_CAPACITY);

        let Self { zx_socket_capacity: _, notifier: _, ready_to_send, socket: _ } = self;

        ready_to_send.set_target_size(ring_buffer_size);

        // Eagerly pull more data out of the Zircon socket into the ring buffer.
        self.poll()
    }
}

impl Takeable for SendBufferWithZirconSocket {
    fn take(&mut self) -> Self {
        let Self { zx_socket_capacity, socket, ready_to_send: data, notifier } = self;
        Self {
            zx_socket_capacity: *zx_socket_capacity,
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
        let zx_socket_capacity = info.rx_buf_max;
        Self { zx_socket_capacity, socket, ready_to_send, notifier }
    }

    fn poll(&mut self) {
        let want_bytes = {
            let BufferLimits { len, capacity } = self.ready_to_send.limits();
            capacity - len
        };
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
        let Self { ready_to_send, zx_socket_capacity: _, notifier: _, socket: _ } = self;
        // Since the reported readable bytes length includes the bytes in
        // `socket`, a reasonable caller could try to peek at those. Since only
        // the bytes in `ready_to_send` are peekable, don't pass through a
        // request that would result in an out-of-bounds peek.
        let BufferLimits { len, capacity: _ } = ready_to_send.limits();
        if offset >= len {
            f(SendPayload::Contiguous(&[]))
        } else {
            ready_to_send.peek_with(offset, f)
        }
    }
}

struct BindingData<I: IpExt> {
    id: SocketId<I>,
    peer: zx::Socket,
}

impl CloseResponder for fposix_socket::StreamSocketCloseResponder {
    fn send(self, arg: &mut fidl_fuchsia_unknown::CloseableCloseResult) -> Result<(), fidl::Error> {
        fposix_socket::StreamSocketCloseResponder::send(self, arg)
    }
}

impl<I: IpExt + IpSockAddrExt> worker::SocketWorkerHandler for BindingData<I>
where
    DeviceId<BindingsNonSyncCtxImpl>:
        TryFromFidlWithContext<<I::SocketAddress as SockAddr>::Zone, Error = DeviceNotFoundError>,
    WeakDeviceId<BindingsNonSyncCtxImpl>:
        TryIntoFidlWithContext<<I::SocketAddress as SockAddr>::Zone, Error = DeviceNotFoundError>,
{
    type Request = fposix_socket::StreamSocketRequest;
    type RequestStream = fposix_socket::StreamSocketRequestStream;
    type CloseResponder = fposix_socket::StreamSocketCloseResponder;

    fn new(
        sync_ctx: &mut SyncCtx<BindingsNonSyncCtxImpl>,
        non_sync_ctx: &mut BindingsNonSyncCtxImpl,
        properties: SocketWorkerProperties,
    ) -> Self {
        let (local, peer) = zx::Socket::create_stream();
        let socket = Arc::new(local);
        let SocketWorkerProperties {} = properties;
        let id = SocketId::Unbound(
            create_socket::<I, _>(sync_ctx, non_sync_ctx),
            LocalZirconSocketAndNotifier(Arc::clone(&socket), NeedsDataNotifier::default()),
        );
        Self { id, peer }
    }

    async fn handle_request(
        &mut self,
        ctx: &NetstackContext,
        request: Self::Request,
    ) -> ControlFlow<Self::CloseResponder, Option<Self::RequestStream>> {
        RequestHandler { ctx, data: self }.handle_request(request).await
    }

    fn close(
        self,
        sync_ctx: &mut SyncCtx<BindingsNonSyncCtxImpl>,
        non_sync_ctx: &mut BindingsNonSyncCtxImpl,
    ) {
        let Self { id, peer: _ } = self;
        match id {
            SocketId::Unbound(unbound, _) => remove_unbound::<I, _>(sync_ctx, unbound),
            SocketId::Bound(bound, _) => remove_bound::<I, _>(sync_ctx, bound),
            SocketId::Connection(conn, _) => close_conn::<I, _>(sync_ctx, non_sync_ctx, conn),
            SocketId::Listener(listener) => {
                let bound = shutdown_listener::<I, _>(sync_ctx, non_sync_ctx, listener);
                let _: zx::Socket = non_sync_ctx.unregister_listener(listener);
                remove_bound::<I, _>(sync_ctx, bound)
            }
        }
    }
}

pub(super) async fn spawn_worker(
    domain: fposix_socket::Domain,
    proto: fposix_socket::StreamSocketProtocol,
    ctx: crate::bindings::NetstackContext,
    request_stream: fposix_socket::StreamSocketRequestStream,
) where
    DeviceId<BindingsNonSyncCtxImpl>: TryFromFidlWithContext<Never, Error = DeviceNotFoundError>
        + TryFromFidlWithContext<NonZeroU64, Error = DeviceNotFoundError>,
{
    match (domain, proto) {
        (fposix_socket::Domain::Ipv4, fposix_socket::StreamSocketProtocol::Tcp) => {
            fasync::Task::spawn(SocketWorker::<BindingData<Ipv4>>::serve_stream(
                ctx,
                SocketWorkerProperties {},
                request_stream,
            ))
        }
        (fposix_socket::Domain::Ipv6, fposix_socket::StreamSocketProtocol::Tcp) => {
            fasync::Task::spawn(SocketWorker::<BindingData<Ipv6>>::serve_stream(
                ctx,
                SocketWorkerProperties {},
                request_stream,
            ))
        }
    }
    .detach()
}

impl IntoErrno for AcceptError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            AcceptError::WouldBlock => fposix::Errno::Eagain,
        }
    }
}

impl IntoErrno for ConnectError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            ConnectError::NoRoute => fposix::Errno::Enetunreach,
            ConnectError::NoPort => fposix::Errno::Eaddrnotavail,
            ConnectError::Zone(z) => z.into_errno(),
        }
    }
}

impl IntoErrno for NoConnection {
    fn into_errno(self) -> fidl_fuchsia_posix::Errno {
        fposix::Errno::Enotconn
    }
}

impl IntoErrno for ListenError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            ListenError::ListenerExists => fposix::Errno::Eaddrinuse,
        }
    }
}

impl IntoErrno for SetReuseAddrError {
    fn into_errno(self) -> fposix::Errno {
        let SetReuseAddrError = self;
        fposix::Errno::Eaddrinuse
    }
}

/// Spawns a task that sends more data from the `socket` each time we observe
/// a wakeup through the `watcher`.
fn spawn_send_task<I: IpExt>(
    ctx: crate::bindings::NetstackContext,
    socket: Arc<zx::Socket>,
    watcher: NeedsDataWatcher,
    id: ConnectionId<I>,
) {
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

struct RequestHandler<'a, I: IpExt> {
    data: &'a mut BindingData<I>,
    ctx: &'a NetstackContext,
}

impl<I: IpSockAddrExt + IpExt> RequestHandler<'_, I>
where
    DeviceId<BindingsNonSyncCtxImpl>:
        TryFromFidlWithContext<<I::SocketAddress as SockAddr>::Zone, Error = DeviceNotFoundError>,
    WeakDeviceId<BindingsNonSyncCtxImpl>:
        TryIntoFidlWithContext<<I::SocketAddress as SockAddr>::Zone, Error = DeviceNotFoundError>,
{
    async fn bind(self, addr: fnet::SocketAddress) -> Result<(), fposix::Errno> {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        match *id {
            SocketId::Unbound(unbound, ref mut local_socket) => {
                let addr = I::SocketAddress::from_sock_addr(addr)?;
                let mut guard = ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                let (addr, port) =
                    addr.try_into_core_with_ctx(non_sync_ctx).map_err(IntoErrno::into_errno)?;
                let bound =
                    bind::<I, _>(sync_ctx, non_sync_ctx, unbound, addr, NonZeroU16::new(port))
                        .map_err(IntoErrno::into_errno)?;
                *id = SocketId::Bound(bound, local_socket.take());
                Ok(())
            }
            SocketId::Bound(_, _) | SocketId::Connection(_, _) | SocketId::Listener(_) => {
                Err(fposix::Errno::Einval)
            }
        }
    }

    async fn connect(self, addr: fnet::SocketAddress) -> Result<(), fposix::Errno> {
        let Self { data: BindingData { id, peer }, ctx } = self;
        let addr = I::SocketAddress::from_sock_addr(addr)?;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
        let is_established =
            || match peer.wait_handle(ZXSIO_SIGNAL_CONNECTED, zx::Time::INFINITE_PAST) {
                Ok(signals) => {
                    debug_assert!(signals.contains(ZXSIO_SIGNAL_CONNECTED));
                    true
                }
                Err(zx::Status::TIMED_OUT) => false,
                Err(err) => panic!("unexpected error when observing signals: {:?}", err),
            };
        let (ip, remote_port) =
            addr.try_into_core_with_ctx(&non_sync_ctx).map_err(IntoErrno::into_errno)?;
        let port = NonZeroU16::new(remote_port).ok_or(fposix::Errno::Einval)?;
        let ip = ip.unwrap_or(ZonedAddr::Unzoned(I::LOOPBACK_ADDRESS));
        let (connection, socket, watcher) = match *id {
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
                    ip,
                    port,
                    LocalZirconSocketAndNotifier(Arc::clone(socket), notifier.clone()),
                )
                .map_err(IntoErrno::into_errno)?;
                Ok((connected, Arc::clone(socket), notifier.watcher()))
            }
            SocketId::Listener(_) => Err(fposix::Errno::Einval),
            SocketId::Connection(_, ref mut established) => {
                if *established {
                    Err(fposix::Errno::Eisconn)
                } else {
                    *established = is_established();
                    if *established {
                        return Ok(());
                    } else {
                        Err(fposix::Errno::Ealready)
                    }
                }
            }
        }?;
        // The following logic matches what Linux does - if the connection is
        // established before the return of the function then it will return
        // success. However realistically speaking, this is unlikely to happen
        // on Fuchsia. We keep this logic to keep parity with Linux and it has
        // a small performance penalty (an extra syscall). If we later decide
        // the benefit is not worth the cost, then we can just blindly return
        // EINPROGRESS.
        let established = is_established();
        spawn_send_task::<I>(ctx.clone(), socket, watcher, connection);
        *id = SocketId::Connection(connection, established);
        established.then(|| ()).ok_or(fposix::Errno::Einprogress)
    }

    async fn listen(self, backlog: i16) -> Result<(), fposix::Errno> {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        match *id {
            SocketId::Bound(bound, ref mut local_socket) => {
                let mut guard = ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                // The POSIX specification for `listen` [1] says
                //
                //   If listen() is called with a backlog argument value that is
                //   less than 0, the function behaves as if it had been called
                //   with a backlog argument value of 0.
                //
                //   A backlog argument of 0 may allow the socket to accept
                //   connections, in which case the length of the listen queue
                //   may be set to an implementation-defined minimum value.
                //
                // [1]: https://pubs.opengroup.org/onlinepubs/9699919799/functions/listen.html
                //
                // Always accept connections with a minimum backlog size of 1.
                // Use a maximum value of 4096 like Linux.
                const MINIMUM_BACKLOG_SIZE: NonZeroUsize = nonzero!(1usize);
                const MAXIMUM_BACKLOG_SIZE: NonZeroUsize = nonzero!(4096usize);

                let backlog = usize::try_from(backlog).unwrap_or(0);
                let backlog = NonZeroUsize::new(backlog).map_or(MINIMUM_BACKLOG_SIZE, |b| {
                    NonZeroUsize::min(
                        MAXIMUM_BACKLOG_SIZE,
                        NonZeroUsize::max(b, MINIMUM_BACKLOG_SIZE),
                    )
                });

                let listener = listen::<I, _>(sync_ctx, non_sync_ctx, bound, backlog)
                    .map_err(IntoErrno::into_errno)?;
                let LocalZirconSocketAndNotifier(local, _) = local_socket.take();
                *id = SocketId::Listener(listener);
                non_sync_ctx.register_listener(
                    listener,
                    Arc::try_unwrap(local)
                        .expect("the local end of the socket should never be shared"),
                );
                Ok(())
            }
            SocketId::Unbound(_, _) | SocketId::Connection(_, _) | SocketId::Listener(_) => {
                Err(fposix::Errno::Einval)
            }
        }
    }

    async fn get_sock_name(self) -> Result<fnet::SocketAddress, fposix::Errno> {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
        let fidl = match *id {
            SocketId::Unbound(_, _) => return Err(fposix::Errno::Einval),
            SocketId::Bound(id, _) => {
                let BoundInfo { addr, port, device: _ } = get_bound_info::<I, _>(sync_ctx, id);
                (addr, port).try_into_fidl_with_ctx(non_sync_ctx)
            }
            SocketId::Listener(id) => {
                let BoundInfo { addr, port, device: _ } = get_listener_info::<I, _>(sync_ctx, id);
                (addr, port).try_into_fidl_with_ctx(non_sync_ctx)
            }
            SocketId::Connection(id, _) => {
                let ConnectionInfo { local_addr, remote_addr: _, device: _ } =
                    get_connection_info::<I, _>(sync_ctx, id);
                local_addr.try_into_fidl_with_ctx(non_sync_ctx)
            }
        }
        .map_err(IntoErrno::into_errno)?;
        Ok(fidl.into_sock_addr())
    }

    async fn get_peer_name(self) -> Result<fnet::SocketAddress, fposix::Errno> {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
        match *id {
            SocketId::Unbound(_, _) | SocketId::Bound(_, _) | SocketId::Listener(_) => {
                Err(fposix::Errno::Enotconn)
            }
            SocketId::Connection(id, _) => Ok({
                get_connection_info::<I, _>(sync_ctx, id)
                    .remote_addr
                    .try_into_fidl_with_ctx(non_sync_ctx)
                    .map_err(IntoErrno::into_errno)?
                    .into_sock_addr()
            }),
        }
    }

    async fn accept(
        self,
        want_addr: bool,
    ) -> Result<
        (Option<Box<fnet::SocketAddress>>, ClientEnd<fposix_socket::StreamSocketMarker>),
        fposix::Errno,
    > {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        match *id {
            SocketId::Listener(listener) => {
                let mut guard = ctx.lock().await;
                let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                let (accepted, addr, peer) = accept::<I, _>(sync_ctx, non_sync_ctx, listener)
                    .map_err(IntoErrno::into_errno)?;
                let addr = addr
                    .try_into_fidl_with_ctx(&non_sync_ctx)
                    .unwrap_or_else(|DeviceNotFoundError| panic!("unknown device"));
                let PeerZirconSocketAndWatcher { peer, watcher, socket } = peer;
                let (client, request_stream) =
                    fidl::endpoints::create_request_stream::<fposix_socket::StreamSocketMarker>()
                        .expect("failed to create new fidl endpoints");
                spawn_send_task::<I>(ctx.clone(), socket, watcher, accepted);
                spawn_connected_socket_task(ctx.clone(), accepted, peer, request_stream);
                Ok((want_addr.then(|| Box::new(addr.into_sock_addr())), client))
            }
            SocketId::Unbound(_, _) | SocketId::Connection(_, _) | SocketId::Bound(_, _) => {
                Err(fposix::Errno::Einval)
            }
        }
    }

    async fn shutdown(self, mode: fposix_socket::ShutdownMode) -> Result<(), fposix::Errno> {
        let Self { data: BindingData { id, peer }, ctx } = self;
        match *id {
            SocketId::Unbound(_, _) | SocketId::Bound(_, _) => Err(fposix::Errno::Enotconn),
            SocketId::Connection(conn_id, _) => {
                let mut my_disposition: Option<zx::SocketWriteDisposition> = None;
                let mut peer_disposition: Option<zx::SocketWriteDisposition> = None;
                if mode.contains(fposix_socket::ShutdownMode::WRITE) {
                    peer_disposition = Some(zx::SocketWriteDisposition::Disabled);
                    let mut guard = ctx.lock().await;
                    let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                    shutdown_conn::<I, _>(&sync_ctx, non_sync_ctx, conn_id)
                        .map_err(IntoErrno::into_errno)?;
                }
                if mode.contains(fposix_socket::ShutdownMode::READ) {
                    my_disposition = Some(zx::SocketWriteDisposition::Disabled);
                }
                peer.set_disposition(peer_disposition, my_disposition)
                    .expect("failed to set socket disposition");
                Ok(())
            }
            SocketId::Listener(listener) => {
                if mode.contains(fposix_socket::ShutdownMode::READ) {
                    let mut guard = ctx.lock().await;
                    let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
                    let bound = shutdown_listener::<I, _>(&sync_ctx, non_sync_ctx, listener);
                    let local = non_sync_ctx.unregister_listener(listener);
                    *id = SocketId::Bound(
                        bound,
                        LocalZirconSocketAndNotifier(Arc::new(local), NeedsDataNotifier::default()),
                    );
                }
                Ok(())
            }
        }
    }

    async fn set_bind_to_device(self, device: Option<&str>) -> Result<(), fposix::Errno> {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut ctx = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = ctx.deref_mut();
        let device = device
            .map(|name| {
                non_sync_ctx
                    .devices
                    .get_device_by_name(name)
                    .map(|d| d.clone())
                    .ok_or(fposix::Errno::Enodev)
            })
            .transpose()?;

        match *id {
            SocketId::Unbound(id, _) => {
                set_unbound_device(sync_ctx, non_sync_ctx, id, device);
                Ok(())
            }
            SocketId::Bound(id, _) => set_bound_device(sync_ctx, non_sync_ctx, id, device),
            SocketId::Listener(id) => set_listener_device(sync_ctx, non_sync_ctx, id, device),
            SocketId::Connection(id, _) => {
                set_connection_device(sync_ctx, non_sync_ctx, id, device)
            }
        }
        .map_err(IntoErrno::into_errno)
    }

    async fn set_send_buffer_size(self, new_size: u64) {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = &mut *guard;
        let new_size =
            usize::try_from(new_size).ok_checked::<TryFromIntError>().unwrap_or(usize::MAX);
        match *id {
            SocketId::Unbound(id, _) => set_send_buffer_size(sync_ctx, non_sync_ctx, id, new_size),
            SocketId::Bound(id, _) => set_send_buffer_size(sync_ctx, non_sync_ctx, id, new_size),
            SocketId::Connection(id, _) => {
                set_send_buffer_size(sync_ctx, non_sync_ctx, id, new_size)
            }
            SocketId::Listener(id) => set_send_buffer_size(sync_ctx, non_sync_ctx, id, new_size),
        }
    }

    async fn send_buffer_size(self) -> u64 {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = &mut *guard;
        match *id {
            SocketId::Unbound(id, _) => send_buffer_size(sync_ctx, non_sync_ctx, id),
            SocketId::Bound(id, _) => send_buffer_size(sync_ctx, non_sync_ctx, id),
            SocketId::Connection(id, _) => send_buffer_size(sync_ctx, non_sync_ctx, id),
            SocketId::Listener(id) => send_buffer_size(sync_ctx, non_sync_ctx, id),
        }
        // If the socket doesn't have a send buffer (e.g. because it was shut
        // down for writing and all the data was sent to the peer), return 0.
        .unwrap_or(0)
        .try_into()
        .ok_checked::<TryFromIntError>()
        .unwrap_or(u64::MAX)
    }

    async fn set_receive_buffer_size(self, new_size: u64) {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = &mut *guard;
        let new_size =
            usize::try_from(new_size).ok_checked::<TryFromIntError>().unwrap_or(usize::MAX);
        match *id {
            SocketId::Unbound(id, _) => {
                set_receive_buffer_size(sync_ctx, non_sync_ctx, id, new_size)
            }
            SocketId::Bound(id, _) => set_receive_buffer_size(sync_ctx, non_sync_ctx, id, new_size),
            SocketId::Connection(id, _) => {
                set_receive_buffer_size(sync_ctx, non_sync_ctx, id, new_size)
            }
            SocketId::Listener(id) => set_receive_buffer_size(sync_ctx, non_sync_ctx, id, new_size),
        }
    }

    async fn receive_buffer_size(self) -> u64 {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = &mut *guard;
        match *id {
            SocketId::Unbound(id, _) => receive_buffer_size(sync_ctx, non_sync_ctx, id),
            SocketId::Bound(id, _) => receive_buffer_size(sync_ctx, non_sync_ctx, id),
            SocketId::Connection(id, _) => receive_buffer_size(sync_ctx, non_sync_ctx, id),
            SocketId::Listener(id) => receive_buffer_size(sync_ctx, non_sync_ctx, id),
        }
        // If the socket doesn't have a receive buffer (e.g. because the remote
        // end signalled FIN and all data was sent to the client), return 0.
        .unwrap_or(0)
        .try_into()
        .ok_checked::<TryFromIntError>()
        .unwrap_or(u64::MAX)
    }

    async fn set_reuse_address(self, value: bool) -> Result<(), fposix::Errno> {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx: _ } = &mut *guard;
        match *id {
            SocketId::Unbound(id, _) => Ok(set_reuseaddr_unbound(sync_ctx, id, value)),
            SocketId::Bound(id, _) => {
                set_reuseaddr_bound(sync_ctx, id, value).map_err(IntoErrno::into_errno)
            }
            SocketId::Listener(id) => {
                set_reuseaddr_listener(sync_ctx, id, value).map_err(IntoErrno::into_errno)
            }
            SocketId::Connection(_, _) => Err(fposix::Errno::Enoprotoopt),
        }
    }

    async fn reuse_address(self) -> bool {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx: _ } = &mut *guard;
        match *id {
            SocketId::Unbound(id, _) => reuseaddr(sync_ctx, id),
            SocketId::Bound(id, _) => reuseaddr(sync_ctx, id),
            SocketId::Listener(id) => reuseaddr(sync_ctx, id),
            SocketId::Connection(id, _) => reuseaddr(sync_ctx, id),
        }
    }

    /// Returns a [`ControlFlow`] to indicate whether the parent stream should
    /// continue being polled or dropped.
    ///
    /// If `Some(stream)` is returned in the `Continue` case, `stream` is a new
    /// stream of events that should be polled concurrently with the parent
    /// stream.
    async fn handle_request(
        self,
        request: fposix_socket::StreamSocketRequest,
    ) -> ControlFlow<
        fposix_socket::StreamSocketCloseResponder,
        Option<fposix_socket::StreamSocketRequestStream>,
    > {
        let Self { data: BindingData { id: _, peer }, ctx: _ } = self;
        match request {
            fposix_socket::StreamSocketRequest::Bind { addr, responder } => {
                responder_send!(responder, &mut self.bind(addr).await);
            }
            fposix_socket::StreamSocketRequest::Connect { addr, responder } => {
                responder_send!(responder, &mut self.connect(addr).await);
            }
            fposix_socket::StreamSocketRequest::Describe { responder } => {
                let socket = peer
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
            fposix_socket::StreamSocketRequest::Close { responder } => {
                // We don't just close the socket because this socket worker is
                // potentially shared by a bunch of sockets because the client
                // can call `dup` on this socket. We will do the cleanup at the
                // end of this task.
                return ControlFlow::Break(responder);
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
            fposix_socket::StreamSocketRequest::Query { responder } => {
                responder_send!(responder, fposix_socket::STREAM_SOCKET_PROTOCOL_NAME.as_bytes());
            }
            fposix_socket::StreamSocketRequest::SetReuseAddress { value, responder } => {
                responder_send!(responder, &mut self.set_reuse_address(value).await);
            }
            fposix_socket::StreamSocketRequest::GetReuseAddress { responder } => {
                responder_send!(responder, &mut Ok(self.reuse_address().await));
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
            fposix_socket::StreamSocketRequest::SetSendBuffer { value_bytes, responder } => {
                self.set_send_buffer_size(value_bytes).await;
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetSendBuffer { responder } => {
                responder_send!(responder, &mut Ok(self.send_buffer_size().await));
            }
            fposix_socket::StreamSocketRequest::SetReceiveBuffer { value_bytes, responder } => {
                responder_send!(
                    responder,
                    &mut Ok(self.set_receive_buffer_size(value_bytes).await)
                );
            }
            fposix_socket::StreamSocketRequest::GetReceiveBuffer { responder } => {
                responder_send!(responder, &mut Ok(self.receive_buffer_size().await));
            }
            fposix_socket::StreamSocketRequest::SetKeepAlive { value: enabled, responder } => {
                self.with_socket_options_mut(|so| so.keep_alive.enabled = enabled).await;
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetKeepAlive { responder } => {
                let enabled = self.with_socket_options(|so| so.keep_alive.enabled).await;
                responder_send!(responder, &mut Ok(enabled));
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
            fposix_socket::StreamSocketRequest::SetIpv6Only { value, responder } => {
                // TODO(https://fxbug.dev/21198): support dual-stack sockets.
                responder_send!(
                    responder,
                    &mut match I::VERSION {
                        IpVersion::V6 => value,
                        IpVersion::V4 => false,
                    }
                    .then_some(())
                    .ok_or(fposix::Errno::Eopnotsupp)
                );
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
            // Note for the following two options:
            // Nagle enabled means TCP delays sending segment, thus meaning
            // TCP_NODELAY is turned off. They have opposite meanings.
            fposix_socket::StreamSocketRequest::SetTcpNoDelay { value, responder } => {
                self.with_socket_options_mut(|so| {
                    so.nagle_enabled = !value;
                })
                .await;
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetTcpNoDelay { responder } => {
                let nagle_enabled = self.with_socket_options(|so| so.nagle_enabled).await;
                responder_send!(responder, &mut Ok(!nagle_enabled));
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
            fposix_socket::StreamSocketRequest::SetTcpKeepAliveIdle { value_secs, responder } => {
                match NonZeroU64::new(value_secs.into())
                    .filter(|value_secs| value_secs.get() <= MAX_TCP_KEEPIDLE_SECS)
                {
                    Some(secs) => {
                        self.with_socket_options_mut(|so| {
                            so.keep_alive.idle = NonZeroDuration::from_nonzero_secs(secs)
                        })
                        .await;
                        responder_send!(responder, &mut Ok(()));
                    }
                    None => {
                        responder_send!(responder, &mut Err(fposix::Errno::Einval));
                    }
                }
            }
            fposix_socket::StreamSocketRequest::GetTcpKeepAliveIdle { responder } => {
                let secs = self
                    .with_socket_options(|so| Duration::from(so.keep_alive.idle).as_secs())
                    .await;
                responder_send!(responder, &mut Ok(u32::try_from(secs).unwrap()));
            }
            fposix_socket::StreamSocketRequest::SetTcpKeepAliveInterval {
                value_secs,
                responder,
            } => {
                match NonZeroU64::new(value_secs.into())
                    .filter(|value_secs| value_secs.get() <= MAX_TCP_KEEPINTVL_SECS)
                {
                    Some(secs) => {
                        self.with_socket_options_mut(|so| {
                            so.keep_alive.interval = NonZeroDuration::from_nonzero_secs(secs)
                        })
                        .await;
                        responder_send!(responder, &mut Ok(()));
                    }
                    None => {
                        responder_send!(responder, &mut Err(fposix::Errno::Einval));
                    }
                }
            }
            fposix_socket::StreamSocketRequest::GetTcpKeepAliveInterval { responder } => {
                let secs = self
                    .with_socket_options(|so| Duration::from(so.keep_alive.interval).as_secs())
                    .await;
                responder_send!(responder, &mut Ok(u32::try_from(secs).unwrap()));
            }
            fposix_socket::StreamSocketRequest::SetTcpKeepAliveCount { value, responder } => {
                match u8::try_from(value)
                    .ok_checked::<TryFromIntError>()
                    .and_then(NonZeroU8::new)
                    .filter(|count| count.get() <= MAX_TCP_KEEPCNT)
                {
                    Some(count) => {
                        self.with_socket_options_mut(|so| {
                            so.keep_alive.count = count;
                        })
                        .await;
                        responder_send!(responder, &mut Ok(()));
                    }
                    None => {
                        responder_send!(responder, &mut Err(fposix::Errno::Einval));
                    }
                };
            }
            fposix_socket::StreamSocketRequest::GetTcpKeepAliveCount { responder } => {
                let count = self.with_socket_options(|so| so.keep_alive.count).await;
                responder_send!(responder, &mut Ok(u32::from(u8::from(count))));
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
            fposix_socket::StreamSocketRequest::SetTcpUserTimeout { value_millis, responder } => {
                let user_timeout =
                    NonZeroU64::new(value_millis.into()).map(NonZeroDuration::from_nonzero_millis);
                self.with_socket_options_mut(|so| {
                    so.user_timeout = user_timeout;
                })
                .await;
                responder_send!(responder, &mut Ok(()));
            }
            fposix_socket::StreamSocketRequest::GetTcpUserTimeout { responder } => {
                let millis = self
                    .with_socket_options(|so| {
                        so.user_timeout.map(|d| d.get().as_millis()).unwrap_or(0)
                    })
                    .await;
                let mut result =
                    u32::try_from(millis).map_err(|_: TryFromIntError| fposix::Errno::Einval);
                responder_send!(responder, &mut result);
            }
        }
        ControlFlow::Continue(None)
    }

    async fn with_socket_options_mut<R, F: FnOnce(&mut SocketOptions) -> R>(self, f: F) -> R {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = &mut *guard;
        match *id {
            SocketId::Unbound(id, _) => with_socket_options_mut(sync_ctx, non_sync_ctx, id, f),
            SocketId::Bound(id, _) => with_socket_options_mut(sync_ctx, non_sync_ctx, id, f),
            SocketId::Connection(id, _) => with_socket_options_mut(sync_ctx, non_sync_ctx, id, f),
            SocketId::Listener(id) => with_socket_options_mut(sync_ctx, non_sync_ctx, id, f),
        }
    }

    async fn with_socket_options<R, F: FnOnce(&SocketOptions) -> R>(self, f: F) -> R {
        let Self { data: BindingData { id, peer: _ }, ctx } = self;
        let guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx: _ } = &*guard;
        match *id {
            SocketId::Unbound(id, _) => with_socket_options(sync_ctx, id, f),
            SocketId::Bound(id, _) => with_socket_options(sync_ctx, id, f),
            SocketId::Connection(id, _) => with_socket_options(sync_ctx, id, f),
            SocketId::Listener(id) => with_socket_options(sync_ctx, id, f),
        }
    }
}

fn spawn_connected_socket_task<I: IpExt + IpSockAddrExt>(
    ctx: NetstackContext,
    accepted: ConnectionId<I>,
    peer: zx::Socket,
    request_stream: fposix_socket::StreamSocketRequestStream,
) where
    DeviceId<BindingsNonSyncCtxImpl>:
        TryFromFidlWithContext<<I::SocketAddress as SockAddr>::Zone, Error = DeviceNotFoundError>,
    WeakDeviceId<BindingsNonSyncCtxImpl>:
        TryIntoFidlWithContext<<I::SocketAddress as SockAddr>::Zone, Error = DeviceNotFoundError>,
{
    fasync::Task::spawn(SocketWorker::<BindingData<I>>::serve_stream_with(
        ctx,
        move |_: &mut SyncCtx<_>, _: &mut BindingsNonSyncCtxImpl, SocketWorkerProperties {}| {
            BindingData { id: SocketId::Connection(accepted, true), peer }
        },
        SocketWorkerProperties {},
        request_stream,
    ))
    .detach();
}

impl<A: IpAddress, D> TryIntoFidlWithContext<<A::Version as IpSockAddrExt>::SocketAddress>
    for SocketAddr<A, D>
where
    A::Version: IpSockAddrExt,
    D: TryIntoFidlWithContext<
        <<A::Version as IpSockAddrExt>::SocketAddress as SockAddr>::Zone,
        Error = DeviceNotFoundError,
    >,
{
    type Error = DeviceNotFoundError;

    fn try_into_fidl_with_ctx<C: ConversionContext>(
        self,
        ctx: &C,
    ) -> Result<<A::Version as IpSockAddrExt>::SocketAddress, Self::Error> {
        let Self { ip, port } = self;
        Ok((ip, port).try_into_fidl_with_ctx(ctx)?)
    }
}

#[cfg(test)]
mod tests {
    use net_types::ip::Ipv6Addr;
    use test_case::test_case;

    use crate::bindings::integration_tests::{StackSetupBuilder, TestSetupBuilder, TestStack};
    use crate::bindings::socket::testutil::TestSockAddr;

    use super::*;

    const TEST_BYTES: &'static [u8] = b"Hello";

    #[test]
    fn receive_buffer() {
        let (local, peer) = zx::Socket::create_stream();
        let mut rbuf = ReceiveBufferWithZirconSocket::new(Arc::new(local), u16::MAX as usize);
        assert_eq!(rbuf.write_at(0, &TEST_BYTES), TEST_BYTES.len());
        assert_eq!(rbuf.write_at(TEST_BYTES.len() * 2, &TEST_BYTES), TEST_BYTES.len());
        assert_eq!(rbuf.write_at(TEST_BYTES.len(), &TEST_BYTES), TEST_BYTES.len());
        rbuf.make_readable(TEST_BYTES.len() * 3);
        let mut buf = [0u8; TEST_BYTES.len() * 3];
        assert_eq!(rbuf.limits().len, TEST_BYTES.len() * 3);
        assert_eq!(peer.read(&mut buf), Ok(TEST_BYTES.len() * 3));
        assert_eq!(&buf, b"HelloHelloHello");
    }

    #[test]
    fn send_buffer() {
        let (local, peer) = zx::Socket::create_stream();
        let notifier = NeedsDataNotifier::default();
        let mut sbuf =
            SendBufferWithZirconSocket::new(Arc::new(local), notifier, u16::MAX as usize);
        assert_eq!(peer.write(TEST_BYTES), Ok(TEST_BYTES.len()));
        assert_eq!(sbuf.limits().len, TEST_BYTES.len());
        sbuf.peek_with(0, |avail| {
            assert_eq!(avail, SendPayload::Contiguous(TEST_BYTES));
        });
        assert_eq!(peer.write(TEST_BYTES), Ok(TEST_BYTES.len()));
        assert_eq!(sbuf.limits().len, TEST_BYTES.len() * 2);
        sbuf.mark_read(TEST_BYTES.len());
        assert_eq!(sbuf.limits().len, TEST_BYTES.len());
        sbuf.peek_with(0, |avail| {
            assert_eq!(avail, SendPayload::Contiguous(TEST_BYTES));
        });
    }

    #[test_case(0, SendBufferWithZirconSocket::MIN_CAPACITY; "below min")]
    #[test_case(1 << 16, 1 << 16; "in range")]
    #[test_case(1 << 32, SendBufferWithZirconSocket::MAX_CAPACITY; "above max")]
    fn send_buffer_limits(target: usize, expected: usize) {
        let (local, _peer) = zx::Socket::create_stream();
        let notifier = NeedsDataNotifier::default();
        let sbuf = SendBufferWithZirconSocket::new(Arc::new(local), notifier, target);
        let ring_buffer_capacity = sbuf.limits().capacity - sbuf.socket.info().unwrap().rx_buf_max;
        assert_eq!(ring_buffer_capacity, expected)
    }

    #[test]
    fn send_buffer_peek_past_ring_buffer() {
        let (local, peer) = zx::Socket::create_stream();
        let mut sbuf = SendBufferWithZirconSocket::new(
            Arc::new(local),
            NeedsDataNotifier::default(),
            SendBufferWithZirconSocket::MIN_CAPACITY,
        );

        // Fill the send buffer up completely.
        const BYTES: [u8; 1024] = [1; 1024];
        loop {
            match peer.write(&BYTES) {
                Ok(0) | Err(zx::Status::SHOULD_WAIT) => break,
                Ok(_) => sbuf.poll(),
                Err(e) => panic!("couldn't write: {:?}", e),
            }
        }

        assert!(
            sbuf.limits().len > SendBufferWithZirconSocket::MIN_CAPACITY,
            "len includes zx socket"
        );

        // Peeking past the end of the ring buffer should not cause a crash.
        sbuf.peek_with(SendBufferWithZirconSocket::MIN_CAPACITY, |payload| {
            assert_matches!(payload, SendPayload::Contiguous(&[]))
        })
    }

    #[test]
    fn send_buffer_resize_empties_zircon_socket() {
        // Regression test for https://fxbug.dev/119242.
        let (local, peer) = zx::Socket::create_stream();
        let notifier = NeedsDataNotifier::default();
        let mut sbuf = SendBufferWithZirconSocket::new(
            Arc::new(local),
            notifier,
            SendBufferWithZirconSocket::MIN_CAPACITY,
        );

        // Fill up the ring buffer and zircon socket.
        while peer.write(TEST_BYTES).map_or(false, |l| l == TEST_BYTES.len()) {
            sbuf.poll();
        }

        sbuf.request_capacity(SendBufferWithZirconSocket::MIN_CAPACITY + TEST_BYTES.len());
        assert_eq!(peer.write(TEST_BYTES), Ok(TEST_BYTES.len()));
    }

    #[test]
    fn send_buffer_resize_down_capacity() {
        // Regression test for https://fxbug.dev/121449.
        let (local, peer) = zx::Socket::create_stream();
        let notifier = NeedsDataNotifier::default();
        let mut sbuf = SendBufferWithZirconSocket::new(
            Arc::new(local),
            notifier,
            SendBufferWithZirconSocket::MAX_CAPACITY,
        );

        // Fill up the ring buffer and zircon socket.
        while peer.write(TEST_BYTES).map_or(false, |l| l == TEST_BYTES.len()) {
            sbuf.poll();
        }

        // Request a shrink of the send buffer.
        let capacity_before = sbuf.limits().capacity;
        sbuf.request_capacity(SendBufferWithZirconSocket::MIN_CAPACITY);

        // Empty out the ring buffer and zircon socket by reading from them.
        while {
            let len = sbuf.peek_with(0, |payload| payload.len());
            sbuf.mark_read(len);
            len != 0
        } {}

        let capacity = sbuf.limits().capacity;
        // The requested capacity isn't directly reflected in `cap` but we can
        // assert that its change is equal to the requested change.
        const EXPECTED_CAPACITY_DECREASE: usize =
            SendBufferWithZirconSocket::MAX_CAPACITY - SendBufferWithZirconSocket::MIN_CAPACITY;
        assert_eq!(
            capacity,
            capacity_before - EXPECTED_CAPACITY_DECREASE,
            "capacity_before: {}, expected decrease: {}",
            capacity_before,
            EXPECTED_CAPACITY_DECREASE
        );

        // The socket's capacity is a measure of how many readable bytes it can
        // hold. If the socket is implemented correctly, this loop will continue
        // until the send buffer's ring buffer is full and the socket buffer is
        // full, then exit.
        while sbuf.limits().len < capacity {
            let _: usize = peer.write(TEST_BYTES).expect("can write");
            sbuf.poll();
        }
    }

    async fn get_socket<A: TestSockAddr>(
        test_stack: &mut TestStack,
    ) -> fposix_socket::StreamSocketProxy {
        let socket_provider = test_stack.connect_socket_provider().unwrap();
        socket_provider
            .stream_socket(A::DOMAIN, fposix_socket::StreamSocketProtocol::Tcp)
            .await
            .unwrap()
            .expect("Socket succeeds")
            .into_proxy()
            .expect("conversion succeeds")
    }

    #[fasync::run_singlethreaded(test)]
    async fn accept_clears_available_signal() {
        // create a stack and add a single endpoint to it so we have the interface
        // id:
        let mut t = TestSetupBuilder::new()
            .add_endpoint()
            .add_stack(StackSetupBuilder::new())
            .build()
            .await
            .unwrap();
        let mut test_stack = t.get(0);

        let socket = get_socket::<fnet::Ipv6SocketAddress>(&mut test_stack).await;

        const PORT: u16 = 200;
        socket
            .bind(&mut fnet::Ipv6SocketAddress::create(Ipv6Addr::default(), PORT))
            .await
            .expect("FIDL succeeds")
            .expect("can bind");
        socket.listen(1).await.expect("FIDL succeeds").expect("can listen");

        let fposix_socket::StreamSocketDescribeResponse { socket: zx_socket, .. } =
            socket.describe().await.expect("FIDL succeeds");
        let zx_socket = zx_socket.expect("has socket");

        assert_eq!(
            zx_socket.wait_handle(ZXSIO_SIGNAL_INCOMING, zx::Time::INFINITE_PAST),
            Err(zx::Status::TIMED_OUT)
        );

        let connector = get_socket::<fnet::Ipv6SocketAddress>(&mut test_stack).await;

        while let Err(e) = connector
            .connect(&mut fnet::Ipv6SocketAddress::create(*Ipv6::LOOPBACK_ADDRESS, PORT))
            .await
            .expect("FIDL succeeds")
        {
            assert_eq!(e, fposix::Errno::Einprogress);
        }

        assert_matches!(
            zx_socket.wait_handle(ZXSIO_SIGNAL_INCOMING, zx::Time::INFINITE_PAST),
            Ok(_)
        );

        // Once the connection is accepted, the signal should no longer be set.
        let _: (Option<Box<fnet::SocketAddress>>, ClientEnd<fposix_socket::StreamSocketMarker>) =
            socket.accept(false).await.expect("FIDL succeeds").expect("has connection");

        assert_eq!(
            zx_socket.wait_handle(ZXSIO_SIGNAL_INCOMING, zx::Time::INFINITE_PAST),
            Err(zx::Status::TIMED_OUT)
        );
    }
}
