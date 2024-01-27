// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::handle::{Message, Proxyable, ProxyableHandle, RouterHolder, Serializer};
use crate::coding::{self, decode_fidl_with_context, encode_fidl_with_context};
use crate::labels::{NodeId, TransferKey};
use crate::peer::{
    FrameType, FramedStreamReader, FramedStreamWriter, MessageStats, PeerConn, PeerConnRef,
};
use crate::router::Router;
use anyhow::{format_err, Context as _, Error};
use fidl_fuchsia_overnet_protocol::{BeginTransfer, Empty, SignalUpdate, StreamControl};
use fuchsia_zircon_status as zx_status;
use futures::{
    future::{poll_fn, BoxFuture},
    prelude::*,
    ready,
};
use std::pin::Pin;
use std::sync::{Arc, Weak};
use std::task::{Context, Poll};

pub(crate) struct StreamWriter<Msg: Message> {
    stream: FramedStreamWriter,
    send_buffer: Vec<u8>,
    stats: Arc<MessageStats>,
    router: Weak<Router>,
    closed: bool,
    _phantom_msg: std::marker::PhantomData<Msg>,
}

impl<Msg: Message> StreamWriter<Msg> {
    pub fn conn(&self) -> PeerConnRef<'_> {
        self.stream.conn()
    }

    pub fn id(&self) -> u64 {
        self.stream.id()
    }

    pub async fn send_data(&mut self, msg: &mut Msg) -> Result<(), Error> {
        assert_ne!(self.closed, true);
        let mut s = Msg::Serializer::new();
        let send_buffer = &mut self.send_buffer;
        let conn = self.stream.conn();
        let mut rh = RouterHolder::Unused(&self.router);
        let stats = &self.stats;
        let coding_context = coding::DEFAULT_CONTEXT;
        poll_fn(|fut_ctx| {
            s.poll_ser(msg, send_buffer, conn, stats, &mut rh, fut_ctx, coding_context)
        })
        .await
        .with_context(|| format_err!("Serializing message {:?}", msg))?;
        self.stream
            .send(FrameType::Data(coding_context), &self.send_buffer, false, &self.stats)
            .await
            .with_context(|| format_err!("sending data {:?} ser={:?}", msg, self.send_buffer))
    }

    async fn send_control(&mut self, mut msg: StreamControl, fin: bool) -> Result<(), Error> {
        assert_ne!(self.closed, true);
        let coding_context = coding::DEFAULT_CONTEXT;
        let msg = encode_fidl_with_context(coding_context, &mut msg)
            .with_context(|| format_err!("encoding control message {:?}", msg))?;
        if fin {
            self.closed = true;
        }
        self.stream
            .send(FrameType::Control(coding_context), msg.as_slice(), fin, &self.stats)
            .await
            .with_context(|| format_err!("sending control message {:?}", msg))
    }

    pub async fn send_signal(&mut self, mut msg: SignalUpdate) -> Result<(), Error> {
        assert_ne!(self.closed, true);
        let coding_context = coding::DEFAULT_CONTEXT;
        let msg = encode_fidl_with_context(coding_context, &mut msg)
            .with_context(|| format_err!("encoding control message {:?}", msg))?;
        self.stream
            .send(FrameType::Signal(coding_context), msg.as_slice(), false, &self.stats)
            .await
            .with_context(|| format_err!("sending control message {:?}", msg))
    }

    pub async fn send_ack_transfer(mut self) -> Result<(), Error> {
        Ok(self.send_control(StreamControl::AckTransfer(Empty {}), true).await?)
    }

    pub async fn send_end_transfer(mut self) -> Result<(), Error> {
        Ok(self.send_control(StreamControl::EndTransfer(Empty {}), true).await?)
    }

    pub async fn send_begin_transfer(
        &mut self,
        new_destination_node: NodeId,
        transfer_key: TransferKey,
    ) -> Result<(), Error> {
        Ok(self
            .send_control(
                StreamControl::BeginTransfer(BeginTransfer {
                    new_destination_node: new_destination_node.into(),
                    transfer_key,
                }),
                false,
            )
            .await?)
    }

    pub async fn send_hello(&mut self) -> Result<(), Error> {
        self.stream
            .send(FrameType::Hello, &[], false, &self.stats)
            .await
            .with_context(|| format_err!("sending hello"))
    }

    pub async fn send_shutdown(mut self, r: Result<(), zx_status::Status>) -> Result<(), Error> {
        self.send_control(
            StreamControl::Shutdown(
                match r {
                    Ok(()) => zx_status::Status::OK,
                    Err(s) => s,
                }
                .into_raw(),
            ),
            true,
        )
        .await
    }
}

pub(crate) trait StreamWriterBinder {
    fn bind<Msg: Message, H: Proxyable<Message = Msg>>(
        self,
        hdl: &ProxyableHandle<H>,
    ) -> StreamWriter<Msg>;
}

impl StreamWriterBinder for FramedStreamWriter {
    fn bind<Msg: Message, H: Proxyable<Message = Msg>>(
        self,
        hdl: &ProxyableHandle<H>,
    ) -> StreamWriter<Msg> {
        StreamWriter {
            stream: self,
            send_buffer: Vec::new(),
            stats: hdl.stats().clone(),
            router: hdl.router().clone(),
            closed: false,
            _phantom_msg: std::marker::PhantomData,
        }
    }
}

#[derive(PartialEq, Debug)]
pub(crate) enum Frame<'a, Msg: Message> {
    Hello,
    Data(&'a mut Msg),
    SignalUpdate(SignalUpdate),
    BeginTransfer(NodeId, TransferKey),
    AckTransfer,
    EndTransfer,
    Shutdown(Result<(), zx_status::Status>),
}

#[derive(Debug)]
pub(crate) struct StreamReader<Msg: Message> {
    stream: FramedStreamReader,
    incoming_message: Msg,
    router: Weak<Router>,
    stats: Arc<MessageStats>,
    state: ReadNextState<Msg::Parser>,
}

#[derive(Debug)]
pub(crate) struct ReadNext<'a, Msg: Message> {
    read_next_frame_or_peer_conn_ref: ReadNextFrameOrPeerConnRef<'a>,
    state: &'a mut ReadNextState<Msg::Parser>,
    conn: PeerConn,
    incoming_message: Option<&'a mut Msg>,
    stats: &'a Arc<MessageStats>,
    router_holder: RouterHolder<'a>,
}

enum ReadNextFrameOrPeerConnRef<'a> {
    ReadNextFrame(BoxFuture<'a, Result<Option<(FrameType, Vec<u8>)>, Error>>),
    PeerConnRef(PeerConnRef<'a>),
}

impl std::fmt::Debug for ReadNextFrameOrPeerConnRef<'_> {
    fn fmt(&self, writer: &mut std::fmt::Formatter<'_>) -> Result<(), std::fmt::Error> {
        match self {
            Self::ReadNextFrame(_) => {
                write!(writer, "ReadNextFrameOrPeerConnRef::ReadNextFrame(...)")
            }
            Self::PeerConnRef(x) => {
                write!(writer, "ReadNextFrameOrPeerConnRef::PeerConnRef({x:?})")
            }
        }
    }
}

impl<'a> ReadNextFrameOrPeerConnRef<'a> {
    fn as_read_next_frame_mut(
        &mut self,
    ) -> Option<&mut BoxFuture<'a, Result<Option<(FrameType, Vec<u8>)>, Error>>> {
        match self {
            Self::ReadNextFrame(x) => Some(x),
            _ => None,
        }
    }
}

#[derive(Debug)]
enum ReadNextState<Parser> {
    Reading,
    DeserializingData(coding::Context, Vec<u8>, Parser),
}

impl<'a, Msg: Message> ReadNext<'a, Msg> {
    fn poll_inner(&mut self, ctx: &mut Context<'_>) -> Poll<Result<Frame<'a, Msg>, Error>> {
        loop {
            return Poll::Ready(Ok(match *self.state {
                ReadNextState::Reading => {
                    let Some((frame_type, mut bytes)) = ready!(self
                        .read_next_frame_or_peer_conn_ref
                        .as_read_next_frame_mut()
                        .unwrap()
                        .poll_unpin(ctx))? else {
                            return Poll::Ready(Err(format_err!("unexpected end of stream")));
                        };

                    match frame_type {
                        FrameType::Hello => {
                            if bytes.len() != 0 {
                                return Poll::Ready(Err(format_err!("Hello frame must be empty")));
                            }
                            Frame::Hello
                        }
                        FrameType::Data(coding_context) => {
                            *self.state = ReadNextState::DeserializingData(
                                coding_context,
                                bytes,
                                Msg::Parser::new(),
                            );
                            continue;
                        }
                        FrameType::Signal(coding_context) => Frame::SignalUpdate(
                            decode_fidl_with_context(coding_context, &mut bytes)?,
                        ),
                        FrameType::Control(coding_context) => {
                            match decode_fidl_with_context(coding_context, &mut bytes)? {
                                StreamControl::AckTransfer(Empty {}) => Frame::AckTransfer,
                                StreamControl::EndTransfer(Empty {}) => Frame::EndTransfer,
                                StreamControl::Shutdown(status_code) => {
                                    Frame::Shutdown(zx_status::Status::ok(status_code))
                                }

                                StreamControl::BeginTransfer(BeginTransfer {
                                    new_destination_node,
                                    transfer_key,
                                }) => {
                                    Frame::BeginTransfer(new_destination_node.into(), transfer_key)
                                }
                            }
                        }
                    }
                }
                ReadNextState::DeserializingData(coding_context, ref mut bytes, ref mut parser) => {
                    ready!(parser.poll_ser(
                        self.incoming_message.as_mut().unwrap(),
                        bytes,
                        self.conn.as_ref(),
                        self.stats,
                        &mut self.router_holder,
                        ctx,
                        coding_context,
                    ))?;
                    *self.state = ReadNextState::Reading;
                    Frame::Data(self.incoming_message.take().unwrap())
                }
            }));
        }
    }
}

impl<'a, Msg: Message> Future for ReadNext<'a, Msg> {
    type Output = Result<Frame<'a, Msg>, Error>;
    fn poll(self: Pin<&mut Self>, ctx: &mut Context<'_>) -> Poll<Self::Output> {
        Pin::into_inner(self).poll_inner(ctx)
    }
}

impl<Msg: Message> StreamReader<Msg> {
    pub fn conn(&self) -> PeerConnRef<'_> {
        self.stream.conn()
    }

    pub fn is_initiator(&self) -> bool {
        self.stream.is_initiator()
    }

    pub fn next<'a>(&'a mut self) -> ReadNext<'a, Msg> {
        let conn = self.stream.conn().into_peer_conn();
        ReadNext {
            read_next_frame_or_peer_conn_ref: match self.state {
                ReadNextState::Reading => {
                    ReadNextFrameOrPeerConnRef::ReadNextFrame(self.stream.next().boxed())
                }
                ReadNextState::DeserializingData(_, _, _) => {
                    ReadNextFrameOrPeerConnRef::PeerConnRef(self.stream.conn())
                }
            },
            state: &mut self.state,
            conn,
            incoming_message: Some(&mut self.incoming_message),
            stats: &self.stats,
            router_holder: RouterHolder::Unused(&self.router),
        }
    }

    async fn expect(&mut self, frame: Frame<'_, Msg>) -> Result<(), Error> {
        let received = self.next().await?;
        if received != frame {
            let msg = format_err!("Expected {:?} got {:?}", frame, received);
            self.stream.abandon().await;
            Err(msg)
        } else {
            Ok(())
        }
    }

    pub async fn expect_ack_transfer(mut self) -> Result<(), Error> {
        self.expect(Frame::AckTransfer).await
    }

    pub async fn expect_hello(&mut self) -> Result<(), Error> {
        self.expect(Frame::Hello).await
    }

    pub async fn expect_shutdown(
        mut self,
        result: Result<(), zx_status::Status>,
    ) -> Result<(), Error> {
        self.expect(Frame::Shutdown(result)).await
    }
}

pub(crate) trait StreamReaderBinder {
    fn bind<Msg: Message, H: Proxyable<Message = Msg>>(
        self,
        hdl: &ProxyableHandle<H>,
    ) -> StreamReader<Msg>;
}

impl StreamReaderBinder for FramedStreamReader {
    fn bind<Msg: Message, H: Proxyable<Message = Msg>>(
        self,
        hdl: &ProxyableHandle<H>,
    ) -> StreamReader<Msg> {
        StreamReader {
            stream: self,
            incoming_message: Default::default(),
            router: hdl.router().clone(),
            stats: hdl.stats().clone(),
            state: ReadNextState::Reading,
        }
    }
}
