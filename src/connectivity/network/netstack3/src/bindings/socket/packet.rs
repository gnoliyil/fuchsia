// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{num::NonZeroU16, ops::ControlFlow, sync::Arc};

use fidl_fuchsia_posix as fposix;
use fidl_fuchsia_posix_socket as fpsocket;
use fidl_fuchsia_posix_socket_packet as fppacket;

use fidl::{endpoints::RequestStream as _, Peered as _};
use fuchsia_async as fasync;
use fuchsia_zircon::{self as zx, HandleBased as _};
use futures::TryStreamExt as _;
use log::error;
use net_types::ethernet::Mac;
use netstack3_core::{
    device::{
        socket::{
            DeviceSocketTypes, Frame, NonSyncContext, Protocol, SendDatagramError,
            SendDatagramParams, SendFrameError, SendFrameParams, SocketId, SocketInfo,
            TargetDevice,
        },
        DeviceId, FrameDestination, WeakDeviceId,
    },
    sync::Mutex,
    SyncCtx,
};
use packet::Buf;

use crate::bindings::{
    devices::BindingId,
    socket::{
        queue::{BodyLen, MessageQueue},
        worker::{self, CloseResponder, SocketWorker},
        IntoErrno, SocketWorkerProperties, ZXSIO_SIGNAL_OUTGOING,
    },
    util::{
        ConversionContext as _, DeviceNotFoundError, IntoCore as _, IntoFidl as _,
        TryFromFidlWithContext, TryIntoCoreWithContext as _, TryIntoFidlWithContext,
    },
    BindingsNonSyncCtxImpl, Ctx,
};

/// State held in the non-sync context for a single socket.
#[derive(Debug)]
pub(crate) struct SocketState {
    /// The received messages for the socket.
    ///
    /// This is shared with the worker that handles requests for the socket.
    queue: Arc<Mutex<MessageQueue<Message>>>,
    kind: fppacket::Kind,
}

impl DeviceSocketTypes for BindingsNonSyncCtxImpl {
    type SocketState = SocketState;
}

impl NonSyncContext<DeviceId<Self>> for BindingsNonSyncCtxImpl {
    fn receive_frame(
        &self,
        state: &Self::SocketState,
        device: &DeviceId<Self>,
        frame: Frame<&[u8]>,
        raw: &[u8],
    ) {
        let SocketState { queue, kind } = state;

        let Frame::Ethernet { frame_dst, src_mac, dst_mac, protocol, body } = frame;
        let packet_type = match frame_dst {
            FrameDestination::Broadcast => fppacket::PacketType::Broadcast,
            FrameDestination::Multicast => fppacket::PacketType::Multicast,
            FrameDestination::Individual { local } => local
                .then_some(fppacket::PacketType::Host)
                .unwrap_or(fppacket::PacketType::OtherHost),
        };
        let message = match kind {
            fppacket::Kind::Network => body,
            fppacket::Kind::Link => raw,
        };

        let data = MessageData {
            dst_mac,
            src_mac,
            packet_type,
            protocol: protocol.unwrap_or(0),
            interface_type: iface_type(device),
            interface_id: self.get_binding_id(device.clone()).get(),
        };
        queue.lock().receive(IntoMessage(data, message));
    }
}

pub(crate) async fn serve(
    ctx: Ctx,
    stream: fppacket::ProviderRequestStream,
) -> Result<(), fidl::Error> {
    let ctx = &ctx;
    stream
        .try_for_each(|req| async {
            match req {
                fppacket::ProviderRequest::Socket { responder, kind } => {
                    let (client, request_stream) = fidl::endpoints::create_request_stream()
                        .unwrap_or_else(|e: fidl::Error| {
                            panic!("failed to create a new request stream: {e}")
                        });
                    fasync::Task::spawn(SocketWorker::serve_stream_with(
                        ctx.clone(),
                        move |sync_ctx, non_sync_ctx, properties| {
                            BindingData::new(sync_ctx, non_sync_ctx, kind, properties)
                        },
                        SocketWorkerProperties {},
                        request_stream,
                    ))
                    .detach();
                    responder_send!(responder, &mut Ok(client));
                }
            }
            Ok(())
        })
        .await
}

#[derive(Debug)]
struct BindingData {
    /// The event to hand off for [`fppacket::SocketRequest::Describe`].
    peer_event: zx::EventPair,
    /// Shared queue of messages received for the socket.
    ///
    /// This is the "read" end of the queue; the non-sync context holds a
    /// reference to the same queue for delivering incoming messages.
    message_queue: Arc<Mutex<MessageQueue<Message>>>,
    state: State,
}

struct IntoMessage<'a>(MessageData, &'a [u8]);

impl BodyLen for IntoMessage<'_> {
    fn body_len(&self) -> usize {
        let Self(_data, body) = self;
        body.len()
    }
}

impl From<IntoMessage<'_>> for Message {
    fn from(IntoMessage(data, body): IntoMessage<'_>) -> Self {
        Self { data, body: body.into() }
    }
}

#[derive(Debug)]
struct Message {
    data: MessageData,
    body: Vec<u8>,
}

#[derive(Debug)]
struct MessageData {
    src_mac: Mac,
    dst_mac: Mac,
    protocol: u16,
    interface_type: fppacket::HardwareType,
    interface_id: u64,
    packet_type: fppacket::PacketType,
}

impl BodyLen for Message {
    fn body_len(&self) -> usize {
        self.body.len()
    }
}

impl BindingData {
    fn new(
        sync_ctx: &SyncCtx<BindingsNonSyncCtxImpl>,
        _non_sync_ctx: &mut BindingsNonSyncCtxImpl,
        kind: fppacket::Kind,
        SocketWorkerProperties {}: SocketWorkerProperties,
    ) -> Self {
        let (local_event, peer_event) = zx::EventPair::create();
        match local_event.signal_peer(zx::Signals::NONE, ZXSIO_SIGNAL_OUTGOING) {
            Ok(()) => (),
            Err(e) => error!("socket failed to signal peer: {:?}", e),
        }

        let message_queue = Arc::new(Mutex::new(MessageQueue::new(local_event)));
        let id = netstack3_core::device::socket::create(
            sync_ctx,
            SocketState { queue: message_queue.clone(), kind },
        );

        BindingData { message_queue, peer_event, state: State { id, kind } }
    }
}

#[derive(Debug)]
struct State {
    id: SocketId<BindingsNonSyncCtxImpl>,
    kind: fppacket::Kind,
}

impl CloseResponder for fppacket::SocketCloseResponder {
    fn send(self, arg: &mut fidl_fuchsia_unknown::CloseableCloseResult) -> Result<(), fidl::Error> {
        fppacket::SocketCloseResponder::send(self, arg)
    }
}

impl worker::SocketWorkerHandler for BindingData {
    type Request = fppacket::SocketRequest;
    type RequestStream = fppacket::SocketRequestStream;
    type CloseResponder = fppacket::SocketCloseResponder;

    fn handle_request(
        &mut self,
        ctx: &Ctx,
        request: Self::Request,
    ) -> ControlFlow<Self::CloseResponder, Option<Self::RequestStream>> {
        RequestHandler { ctx, data: self }.handle_request(request)
    }

    fn close(
        self,
        sync_ctx: &SyncCtx<BindingsNonSyncCtxImpl>,
        _non_sync_ctx: &mut BindingsNonSyncCtxImpl,
    ) {
        let Self { peer_event: _, state, message_queue: _ } = self;
        let State { id, kind: _ } = state;
        netstack3_core::device::socket::remove(sync_ctx, id)
    }
}

struct RequestHandler<'a> {
    ctx: &'a Ctx,
    data: &'a mut BindingData,
}

impl<'a> RequestHandler<'a> {
    fn describe(self) -> fppacket::SocketDescribeResponse {
        let Self { ctx: _, data: BindingData { peer_event, state: _, message_queue: _ } } = self;
        let peer = peer_event
            .duplicate_handle(
                // The client only needs to be able to receive signals so don't
                // allow it to set signals.
                zx::Rights::BASIC,
            )
            .unwrap_or_else(|s: zx::Status| panic!("failed to duplicate handle: {s}"));
        fppacket::SocketDescribeResponse { event: Some(peer), ..Default::default() }
    }

    fn bind(
        self,
        protocol: Option<Box<fppacket::ProtocolAssociation>>,
        interface: fppacket::BoundInterfaceId,
    ) -> Result<(), fposix::Errno> {
        let protocol = protocol
            .map(|protocol| {
                Ok(match *protocol {
                    fppacket::ProtocolAssociation::All(fppacket::Empty) => Protocol::All,
                    fppacket::ProtocolAssociation::Specified(p) => {
                        Protocol::Specific(NonZeroU16::new(p).ok_or(fposix::Errno::Einval)?)
                    }
                })
            })
            .transpose()?;
        let Self { ctx, data: BindingData { peer_event: _, message_queue: _, state } } = self;
        let mut ctx = ctx.clone();
        let Ctx { sync_ctx, non_sync_ctx } = &mut ctx;
        let device = match interface {
            fppacket::BoundInterfaceId::All(fppacket::Empty) => None,
            fppacket::BoundInterfaceId::Specified(id) => {
                let id = BindingId::new(id).ok_or(DeviceNotFoundError.into_errno())?;
                Some(id.try_into_core_with_ctx(non_sync_ctx).map_err(IntoErrno::into_errno)?)
            }
        };
        let device_selector = match device.as_ref() {
            Some(d) => TargetDevice::SpecificDevice(d),
            None => TargetDevice::AnyDevice,
        };

        let State { id, kind: _ } = state;
        match protocol {
            Some(protocol) => netstack3_core::device::socket::set_device_and_protocol(
                sync_ctx,
                id,
                device_selector,
                protocol,
            ),
            None => netstack3_core::device::socket::set_device(sync_ctx, id, device_selector),
        }
        Ok(())
    }

    fn get_info(
        self,
    ) -> Result<
        (fppacket::Kind, Option<Box<fppacket::ProtocolAssociation>>, fppacket::BoundInterface),
        fposix::Errno,
    > {
        let Self {
            ctx,
            data: BindingData { peer_event: _, message_queue: _, state: State { id, kind } },
        } = self;
        let mut ctx = ctx.clone();
        let Ctx { sync_ctx, non_sync_ctx } = &mut ctx;

        let SocketInfo { device, protocol } =
            netstack3_core::device::socket::get_info(sync_ctx, id);
        let interface = match device {
            TargetDevice::AnyDevice => fppacket::BoundInterface::All(fppacket::Empty),
            TargetDevice::SpecificDevice(d) => fppacket::BoundInterface::Specified(
                d.try_into_fidl_with_ctx(non_sync_ctx).map_err(IntoErrno::into_errno)?,
            ),
        };

        let protocol = protocol.map(|p| {
            Box::new(match p {
                Protocol::All => fppacket::ProtocolAssociation::All(fppacket::Empty),
                Protocol::Specific(p) => fppacket::ProtocolAssociation::Specified(p.get()),
            })
        });

        Ok((*kind, protocol, interface))
    }

    fn receive(self) -> Result<Message, fposix::Errno> {
        let Self {
            ctx: _,
            data: BindingData { peer_event: _, message_queue, state: State { id: _, kind: _ } },
        } = self;

        let mut queue = message_queue.lock();
        queue.pop().ok_or(fposix::EWOULDBLOCK)
    }

    fn set_receive_buffer(self, size: u64) {
        let Self {
            ctx: _,
            data: BindingData { peer_event: _, message_queue, state: State { id: _, kind: _ } },
        } = self;

        let mut queue = message_queue.lock();
        queue.set_max_available_messages_size(size.try_into().unwrap_or(usize::MAX))
    }

    fn receive_buffer(self) -> u64 {
        let Self {
            ctx: _,
            data: BindingData { peer_event: _, message_queue, state: State { id: _, kind: _ } },
        } = self;

        let queue = message_queue.lock();
        queue.max_available_messages_size().try_into().unwrap_or(u64::MAX)
    }

    fn send_msg(
        self,
        packet_info: Option<Box<fppacket::PacketInfo>>,
        data: Vec<u8>,
    ) -> Result<(), fposix::Errno> {
        let Self {
            ctx,
            data: BindingData { peer_event: _, message_queue: _, state: State { ref mut id, kind } },
        } = self;
        let mut ctx = ctx.clone();
        let Ctx { sync_ctx, non_sync_ctx } = &mut ctx;

        let data = Buf::new(data, ..);
        match kind {
            fppacket::Kind::Network => {
                let params = packet_info.try_into_core_with_ctx(non_sync_ctx)?;
                netstack3_core::device::socket::send_datagram(
                    sync_ctx,
                    non_sync_ctx,
                    id,
                    params,
                    data,
                )
                .map_err(|(_, e): (Buf<Vec<u8>>, _)| e.into_errno())
            }
            fppacket::Kind::Link => {
                let params = packet_info
                    .try_into_core_with_ctx(non_sync_ctx)
                    .map_err(IntoErrno::into_errno)?;
                netstack3_core::device::socket::send_frame(sync_ctx, non_sync_ctx, id, params, data)
                    .map_err(|(_, e): (Buf<Vec<u8>>, _)| e.into_errno())
            }
        }
    }

    fn handle_request(
        self,
        request: fppacket::SocketRequest,
    ) -> ControlFlow<fppacket::SocketCloseResponder, Option<fppacket::SocketRequestStream>> {
        match request {
            fppacket::SocketRequest::Clone2 { request, control_handle: _ } => {
                let channel = fidl::AsyncChannel::from_channel(request.into_channel())
                    .expect("failed to create async channel");
                let stream = fppacket::SocketRequestStream::from_channel(channel);
                return ControlFlow::Continue(Some(stream));
            }
            fppacket::SocketRequest::Close { responder } => {
                return ControlFlow::Break(responder);
            }
            fppacket::SocketRequest::Query { responder } => {
                responder_send!(responder, fppacket::SOCKET_PROTOCOL_NAME.as_bytes());
            }
            fppacket::SocketRequest::SetReuseAddress { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetReuseAddress { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetError { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetBroadcast { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetBroadcast { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetSendBuffer { value_bytes: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetSendBuffer { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetReceiveBuffer { value_bytes, responder } => {
                responder_send!(responder, &mut Ok(self.set_receive_buffer(value_bytes)));
            }
            fppacket::SocketRequest::GetReceiveBuffer { responder } => {
                responder_send!(responder, &mut Ok(self.receive_buffer()));
            }
            fppacket::SocketRequest::SetKeepAlive { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetKeepAlive { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetOutOfBandInline { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetOutOfBandInline { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetNoCheck { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetNoCheck { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetLinger { linger: _, length_secs: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetLinger { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetReusePort { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetReusePort { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetAcceptConn { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetBindToDevice { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetBindToDevice { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::SetTimestamp { value: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetTimestamp { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::Describe { responder } => {
                responder_send!(responder, self.describe());
            }
            fppacket::SocketRequest::Bind { protocol, bound_interface_id, responder } => {
                responder_send!(responder, &mut self.bind(protocol, bound_interface_id))
            }
            fppacket::SocketRequest::GetInfo { responder } => {
                responder_send!(responder, &mut self.get_info());
            }
            fppacket::SocketRequest::RecvMsg {
                want_packet_info,
                data_len,
                want_control,
                flags,
                responder,
            } => {
                let params = RecvMsgParams { want_packet_info, data_len, want_control, flags };
                let response = self.receive();
                responder_send!(responder, &mut response.map(|r| params.apply_to(r)))
            }
            fppacket::SocketRequest::SendMsg { packet_info, data, control, flags, responder } => {
                if ![
                    fppacket::SendControlData::default(),
                    fppacket::SendControlData {
                        socket: Some(fpsocket::SocketSendControlData::default()),
                        ..Default::default()
                    },
                ]
                .contains(&control)
                {
                    tracing::warn!("unsupported control data: {:?}", control);
                    responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
                } else if flags != fpsocket::SendMsgFlags::empty() {
                    tracing::warn!("unsupported control flags: {:?}", flags);
                    responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp));
                } else {
                    responder_send!(responder, &mut self.send_msg(packet_info, data));
                }
            }
        }
        ControlFlow::Continue(None)
    }
}

fn iface_type(device: &DeviceId<BindingsNonSyncCtxImpl>) -> fppacket::HardwareType {
    match device {
        DeviceId::Ethernet(_) => fppacket::HardwareType::Ethernet,
        DeviceId::Loopback(_) => fppacket::HardwareType::Loopback,
    }
}

impl TryIntoFidlWithContext<fppacket::InterfaceProperties>
    for WeakDeviceId<BindingsNonSyncCtxImpl>
{
    type Error = DeviceNotFoundError;
    fn try_into_fidl_with_ctx<C: crate::bindings::util::ConversionContext>(
        self,
        ctx: &C,
    ) -> Result<fppacket::InterfaceProperties, Self::Error> {
        let device = self.upgrade().ok_or(DeviceNotFoundError)?;
        let iface_type = iface_type(&device);
        let addr = match &device {
            DeviceId::Ethernet(e) => {
                fppacket::HardwareAddress::Eui48(e.external_state().mac.into_fidl())
            }
            DeviceId::Loopback(_) => {
                // Pretend that the loopback interface has an all-zeroes MAC
                // address to match Linux behavior.
                fppacket::HardwareAddress::Eui48(fidl_fuchsia_net::MacAddress { octets: [0; 6] })
            }
        };
        let id = ctx.get_binding_id(device).get();
        Ok(fppacket::InterfaceProperties { addr, id, type_: iface_type })
    }
}

impl<D> TryFromFidlWithContext<Option<Box<fppacket::PacketInfo>>> for SendDatagramParams<D>
where
    D: TryFromFidlWithContext<BindingId, Error = DeviceNotFoundError>,
{
    type Error = fposix::Errno;

    fn try_from_fidl_with_ctx<C: crate::bindings::util::ConversionContext>(
        ctx: &C,
        packet_info: Option<Box<fppacket::PacketInfo>>,
    ) -> Result<Self, Self::Error> {
        packet_info.ok_or(fposix::Errno::Einval).and_then(|info| {
            let fppacket::PacketInfo { protocol, interface_id, addr } = *info;
            let device = BindingId::new(interface_id)
                .map(|id| id.try_into_core_with_ctx(ctx))
                .transpose()
                .map_err(IntoErrno::into_errno)?;
            let protocol = NonZeroU16::new(protocol);
            let dest_addr = match addr {
                fppacket::HardwareAddress::Eui48(mac) => Some(mac.into_core()),
                fppacket::HardwareAddress::None(fppacket::Empty) => None,
                fppacket::HardwareAddressUnknown!() => None,
            }
            .ok_or(fposix::Errno::Einval)?;
            Ok(Self { frame: SendFrameParams { device }, protocol, dest_addr })
        })
    }
}

impl<D> TryFromFidlWithContext<Option<Box<fppacket::PacketInfo>>> for SendFrameParams<D>
where
    D: TryFromFidlWithContext<BindingId, Error = DeviceNotFoundError>,
{
    type Error = DeviceNotFoundError;

    fn try_from_fidl_with_ctx<C: crate::bindings::util::ConversionContext>(
        ctx: &C,
        packet_info: Option<Box<fppacket::PacketInfo>>,
    ) -> Result<Self, Self::Error> {
        packet_info.map_or(Ok(SendFrameParams::default()), |info| {
            let fppacket::PacketInfo { protocol, interface_id, addr } = *info;
            // Ignore protocol and addr since the frame to send already includes
            // any link-layer headers.
            let _ = (protocol, addr);
            let device = BindingId::new(interface_id)
                .map(|id| id.try_into_core_with_ctx(ctx))
                .transpose()?;
            Ok(SendFrameParams { device })
        })
    }
}

struct RecvMsgParams {
    want_packet_info: bool,
    data_len: u32,
    want_control: bool,
    flags: fidl_fuchsia_posix_socket::RecvMsgFlags,
}

impl RecvMsgParams {
    fn apply_to(
        self,
        response: Message,
    ) -> (Option<Box<fppacket::RecvPacketInfo>>, Vec<u8>, fppacket::RecvControlData, u32) {
        let Self { want_packet_info, data_len, want_control, flags } = self;
        let data_len = data_len.try_into().unwrap_or(usize::MAX);

        let Message { data, mut body } = response;
        let MessageData { src_mac, dst_mac, packet_type, interface_id, interface_type, protocol } =
            data;

        let truncated = body.len().saturating_sub(data_len);
        body.truncate(data_len);

        let packet_info = if want_packet_info {
            let info = fppacket::RecvPacketInfo {
                packet_type,
                interface_type,
                packet_info: fppacket::PacketInfo {
                    protocol,
                    interface_id,
                    addr: fppacket::HardwareAddress::Eui48(src_mac.into_fidl()),
                },
            };
            Some(Box::new(info))
        } else {
            None
        };

        // TODO(https://fxbug.dev/106735): Return control data and flags.
        let _ = (want_control, flags, dst_mac);
        let control = fppacket::RecvControlData::default();

        (packet_info, body, control, truncated.try_into().unwrap_or(u32::MAX))
    }
}

impl IntoErrno for SendDatagramError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            Self::Frame(f) => f.into_errno(),
            Self::NoProtocol => fposix::Errno::Einval,
        }
    }
}

impl IntoErrno for SendFrameError {
    fn into_errno(self) -> fposix::Errno {
        match self {
            SendFrameError::NoDevice => fposix::Errno::Einval,
            SendFrameError::SendFailed => fposix::Errno::Enobufs,
        }
    }
}
