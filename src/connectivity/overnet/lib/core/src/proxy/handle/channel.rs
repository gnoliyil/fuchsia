// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::{
    IntoProxied, Message, Proxyable, ProxyableRW, ReadValue, RouterHolder, Serializer, IO,
};
use crate::coding::{self, decode_fidl_with_context, encode_fidl_with_context};
use crate::peer::{MessageStats, PeerConnRef};
use anyhow::{Context as _, Error};
use fidl::{AsHandleRef, AsyncChannel, HandleBased, Peered, Signals};
use fidl_fuchsia_overnet_protocol::{ZirconChannelMessage, ZirconHandle};
use fuchsia_zircon_status as zx_status;
use futures::{prelude::*, ready};
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

#[cfg(not(target_os = "fuchsia"))]
use fuchsia_async::emulated_handle::ChannelProxyProtocol;

pub(crate) struct Channel {
    chan: AsyncChannel,
}

impl std::fmt::Debug for Channel {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.chan.fmt(f)
    }
}

impl Proxyable for Channel {
    type Message = ChannelMessage;

    fn from_fidl_handle(hdl: fidl::Handle) -> Result<Self, Error> {
        Ok(fidl::Channel::from_handle(hdl).into_proxied()?)
    }

    fn into_fidl_handle(self) -> Result<fidl::Handle, Error> {
        Ok(self.chan.into_zx_channel().into_handle())
    }

    fn signal_peer(&self, clear: Signals, set: Signals) -> Result<(), Error> {
        let chan: &fidl::Channel = self.chan.as_ref();
        chan.signal_peer(clear, set)?;
        Ok(())
    }

    #[cfg(not(target_os = "fuchsia"))]
    fn set_channel_proxy_protocol(&self, proto: ChannelProxyProtocol) {
        self.chan.set_channel_proxy_protocol(proto);
    }
}

impl<'a> ProxyableRW<'a> for Channel {
    type Reader = ChannelReader<'a>;
    type Writer = ChannelWriter;
}

impl IntoProxied for fidl::Channel {
    type Proxied = Channel;
    fn into_proxied(self) -> Result<Channel, Error> {
        Ok(Channel { chan: AsyncChannel::from_channel(self)? })
    }
}

pub(crate) struct ChannelReader<'a> {
    collector: super::signals::Collector<'a>,
}

impl<'a> IO<'a> for ChannelReader<'a> {
    type Proxyable = Channel;
    type Output = ReadValue;
    fn new() -> ChannelReader<'a> {
        ChannelReader { collector: Default::default() }
    }
    fn poll_io(
        &mut self,
        msg: &mut ChannelMessage,
        channel: &'a Channel,
        fut_ctx: &mut Context<'_>,
    ) -> Poll<Result<ReadValue, zx_status::Status>> {
        let read_result = channel.chan.read(fut_ctx, &mut msg.bytes, &mut msg.handles);
        self.collector.after_read(fut_ctx, channel.chan.as_handle_ref(), read_result, false)
    }
}

pub(crate) struct ChannelWriter;

impl IO<'_> for ChannelWriter {
    type Proxyable = Channel;
    type Output = ();
    fn new() -> ChannelWriter {
        ChannelWriter
    }
    fn poll_io(
        &mut self,
        msg: &mut ChannelMessage,
        channel: &Channel,
        _: &mut Context<'_>,
    ) -> Poll<Result<(), zx_status::Status>> {
        Poll::Ready(Ok(channel.chan.write(&msg.bytes, &mut msg.handles)?))
    }
}

#[derive(Default, Debug)]
pub(crate) struct ChannelMessage {
    bytes: Vec<u8>,
    handles: Vec<fidl::Handle>,
}

impl Message for ChannelMessage {
    type Parser = ChannelMessageParser;
    type Serializer = ChannelMessageSerializer;
}

impl PartialEq for ChannelMessage {
    fn eq(&self, rhs: &Self) -> bool {
        if !self.handles.is_empty() {
            return false;
        }
        if !rhs.handles.is_empty() {
            return false;
        }
        return self.bytes == rhs.bytes;
    }
}

pub(crate) enum ChannelMessageParser {
    New,
    Pending {
        bytes: Vec<u8>,
        handles: Pin<Box<dyn 'static + Send + Future<Output = Result<Vec<fidl::Handle>, Error>>>>,
    },
    Done,
}

impl std::fmt::Debug for ChannelMessageParser {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ChannelMessageParser::New => "New",
            ChannelMessageParser::Pending { .. } => "Pending",
            ChannelMessageParser::Done => "Done",
        }
        .fmt(f)
    }
}

impl Serializer for ChannelMessageParser {
    type Message = ChannelMessage;
    fn new() -> Self {
        Self::New
    }
    fn poll_ser(
        &mut self,
        msg: &mut Self::Message,
        serialized: &mut Vec<u8>,
        conn: PeerConnRef<'_>,
        stats: &Arc<MessageStats>,
        router: &mut RouterHolder<'_>,
        fut_ctx: &mut Context<'_>,
        coding_context: coding::Context,
    ) -> Poll<Result<(), Error>> {
        tracing::trace!(?msg, ?serialized, ?self, "ChannelMessageParser::poll_ser",);
        match self {
            ChannelMessageParser::New => {
                let ZirconChannelMessage { mut bytes, handles: unbound_handles } =
                    decode_fidl_with_context(coding_context, serialized)?;
                // Special case no handles case to avoid allocation dance
                if unbound_handles.is_empty() {
                    msg.handles.clear();
                    std::mem::swap(&mut msg.bytes, &mut bytes);
                    *self = ChannelMessageParser::Done;
                    return Poll::Ready(Ok(()));
                }
                let closure_conn = conn.into_peer_conn();
                let closure_stats = stats.clone();
                let closure_router = router.get()?.clone();
                *self = ChannelMessageParser::Pending {
                    bytes,
                    handles: async move {
                        let mut handles = Vec::new();
                        for hdl in unbound_handles.into_iter() {
                            handles.push(
                                closure_router
                                    .clone()
                                    .recv_proxied(hdl, closure_conn.as_ref(), closure_stats.clone())
                                    .await?,
                            );
                        }
                        Ok(handles)
                    }
                    .boxed(),
                };
                self.poll_ser(msg, serialized, conn, stats, router, fut_ctx, coding_context)
            }
            ChannelMessageParser::Pending { ref mut bytes, handles } => {
                let mut handles = ready!(handles.as_mut().poll(fut_ctx))?;
                std::mem::swap(&mut msg.handles, &mut handles);
                std::mem::swap(&mut msg.bytes, bytes);
                *self = ChannelMessageParser::Done;
                Poll::Ready(Ok(()))
            }
            ChannelMessageParser::Done => unreachable!(),
        }
    }
}

pub(crate) enum ChannelMessageSerializer {
    New,
    Pending(Pin<Box<dyn 'static + Send + Future<Output = Result<Vec<ZirconHandle>, Error>>>>),
    Done,
}

impl Serializer for ChannelMessageSerializer {
    type Message = ChannelMessage;
    fn new() -> Self {
        Self::New
    }
    fn poll_ser(
        &mut self,
        msg: &mut Self::Message,
        serialized: &mut Vec<u8>,
        conn: PeerConnRef<'_>,
        stats: &Arc<MessageStats>,
        router: &mut RouterHolder<'_>,
        fut_ctx: &mut Context<'_>,
        coding_context: coding::Context,
    ) -> Poll<Result<(), Error>> {
        let self_val = match self {
            ChannelMessageSerializer::New => "New",
            ChannelMessageSerializer::Pending { .. } => "Pending",
            ChannelMessageSerializer::Done => "Done",
        };
        tracing::trace!(?msg, ?serialized, self = self_val, "ChannelMessageSerializer::poll_ser");
        match self {
            ChannelMessageSerializer::New => {
                let handles = std::mem::replace(&mut msg.handles, Vec::new());
                // Special case no handles case to avoid allocation dance
                if handles.is_empty() {
                    *serialized = encode_fidl_with_context(
                        coding::DEFAULT_CONTEXT,
                        &mut ZirconChannelMessage {
                            bytes: std::mem::replace(&mut msg.bytes, Vec::new()),
                            handles: Vec::new(),
                        },
                    )?;
                    *self = ChannelMessageSerializer::Done;
                    return Poll::Ready(Ok(()));
                }
                let closure_conn = conn.into_peer_conn();
                let closure_stats = stats.clone();
                let closure_router = router.get()?.clone();
                *self = ChannelMessageSerializer::Pending(
                    async move {
                        let mut send_handles = Vec::new();
                        for handle in handles {
                            // save for debugging
                            let raw_handle = handle.raw_handle();
                            send_handles.push(
                                closure_router
                                    .send_proxied(
                                        handle,
                                        closure_conn.as_ref(),
                                        closure_stats.clone(),
                                    )
                                    .await
                                    .with_context(|| format!("Sending handle {:?}", raw_handle))?,
                            );
                        }
                        Ok(send_handles)
                    }
                    .boxed(),
                );
                self.poll_ser(msg, serialized, conn, stats, router, fut_ctx, coding_context)
            }
            ChannelMessageSerializer::Pending(handles) => {
                let handles = ready!(handles.as_mut().poll(fut_ctx))?;
                *serialized = encode_fidl_with_context(
                    coding::DEFAULT_CONTEXT,
                    &mut ZirconChannelMessage {
                        bytes: std::mem::replace(&mut msg.bytes, Vec::new()),
                        handles,
                    },
                )?;
                *self = ChannelMessageSerializer::Done;
                Poll::Ready(Ok(()))
            }
            ChannelMessageSerializer::Done => unreachable!(),
        }
    }
}
