// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{
    num::NonZeroU16,
    ops::{ControlFlow, DerefMut},
};

use fidl_fuchsia_posix as fposix;
use fidl_fuchsia_posix_socket_packet as fppacket;

use fidl::{endpoints::RequestStream as _, Peered as _};
use fuchsia_async as fasync;
use fuchsia_zircon::{self as zx, HandleBased as _};
use futures::TryStreamExt as _;
use log::error;
use netstack3_core::{
    device::{
        socket::{Protocol, SocketId, SocketInfo, TargetDevice},
        DeviceId, WeakDeviceId,
    },
    Ctx, SyncCtx,
};

use crate::bindings::{
    devices::BindingId,
    socket::{
        worker::{self, CloseResponder, SocketWorker},
        IntoErrno, SocketWorkerProperties, ZXSIO_SIGNAL_OUTGOING,
    },
    util::{DeviceNotFoundError, IntoFidl, TryIntoCoreWithContext as _, TryIntoFidlWithContext},
    BindingsNonSyncCtxImpl, NetstackContext,
};

pub(crate) async fn serve(
    ctx: NetstackContext,
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
                        move |sync_ctx, _: &mut BindingsNonSyncCtxImpl, properties| {
                            BindingData::new(sync_ctx, kind, properties)
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
    peer_event: zx::EventPair,
    // TODO(https://fxbug.dev/106735): use this for signaling.
    _local_event: zx::EventPair,
    state: State,
}

impl BindingData {
    fn new(
        sync_ctx: &mut SyncCtx<BindingsNonSyncCtxImpl>,
        kind: fppacket::Kind,
        SocketWorkerProperties {}: SocketWorkerProperties,
    ) -> Self {
        let (local_event, peer_event) = zx::EventPair::create();
        match local_event.signal_peer(zx::Signals::NONE, ZXSIO_SIGNAL_OUTGOING) {
            Ok(()) => (),
            Err(e) => error!("socket failed to signal peer: {:?}", e),
        }

        BindingData {
            _local_event: local_event,
            peer_event,
            state: State { id: netstack3_core::device::socket::create(sync_ctx), kind },
        }
    }
}

#[derive(Debug)]
struct State {
    id: SocketId,
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
        _non_sync_ctx: &mut BindingsNonSyncCtxImpl,
    ) {
        let Self { peer_event: _, state, _local_event: _ } = self;
        let State { id, kind: _ } = state;
        netstack3_core::device::socket::remove(sync_ctx, id)
    }
}

struct RequestHandler<'a> {
    ctx: &'a NetstackContext,
    data: &'a mut BindingData,
}

impl<'a> RequestHandler<'a> {
    fn describe(self) -> fppacket::SocketDescribeResponse {
        let Self { ctx: _, data: BindingData { peer_event, state: _, _local_event: _ } } = self;
        let peer = peer_event
            .duplicate_handle(
                // The client only needs to be able to receive signals so don't
                // allow it to set signals.
                zx::Rights::BASIC,
            )
            .unwrap_or_else(|s: zx::Status| panic!("failed to duplicate handle: {s}"));
        fppacket::SocketDescribeResponse {
            event: Some(peer),
            ..fppacket::SocketDescribeResponse::EMPTY
        }
    }

    async fn bind(
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
        let Self { ctx, data: BindingData { peer_event: _, _local_event: _, state } } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();
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

    async fn get_info(
        self,
    ) -> Result<
        (fppacket::Kind, Option<Box<fppacket::ProtocolAssociation>>, fppacket::BoundInterface),
        fposix::Errno,
    > {
        let Self {
            ctx,
            data: BindingData { peer_event: _, _local_event, state: State { id, kind } },
        } = self;
        let mut guard = ctx.lock().await;
        let Ctx { sync_ctx, non_sync_ctx } = guard.deref_mut();

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

    async fn handle_request(
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
            fppacket::SocketRequest::SetReceiveBuffer { value_bytes: _, responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
            fppacket::SocketRequest::GetReceiveBuffer { responder } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
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
                responder_send!(responder, &mut self.bind(protocol, bound_interface_id).await)
            }
            fppacket::SocketRequest::GetInfo { responder } => {
                responder_send!(responder, &mut self.get_info().await);
            }
            fppacket::SocketRequest::RecvMsg {
                want_packet_info: _,
                data_len: _,
                want_control: _,
                flags: _,
                responder,
            } => responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp)),
            fppacket::SocketRequest::SendMsg {
                packet_info: _,
                data: _,
                control: _,
                flags: _,
                responder,
            } => {
                responder_send!(responder, &mut Err(fposix::Errno::Eopnotsupp))
            }
        }
        ControlFlow::Continue(None)
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
        let (iface_type, addr) = match &device {
            DeviceId::Ethernet(e) => (
                fppacket::HardwareType::Ethernet,
                fppacket::HardwareAddress::Eui48(e.external_state().mac.into_fidl()),
            ),
            DeviceId::Loopback(_) => (
                fppacket::HardwareType::Loopback,
                // Pretend that the loopback interface has an all-zeroes MAC
                // address to match Linux behavior.
                fppacket::HardwareAddress::Eui48(fidl_fuchsia_net::MacAddress { octets: [0; 6] }),
            ),
        };
        let id = ctx.get_binding_id(device).get();
        Ok(fppacket::InterfaceProperties { addr, id, type_: iface_type })
    }
}
