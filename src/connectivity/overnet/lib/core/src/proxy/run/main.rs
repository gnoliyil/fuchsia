// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Main loops (and associated spawn functions) for proxying... handles moving data from one point
//! to another, and calling into crate::proxy::xfer once a handle transfer is required.

use super::super::{
    handle::ReadValue,
    stream::{Frame, StreamReader, StreamReaderBinder, StreamWriter, StreamWriterBinder},
    Proxy, ProxyTransferInitiationReceiver, Proxyable, ProxyableRW, RemoveFromProxyTable,
    StreamRefSender,
};
use crate::labels::{NodeId, TransferKey};
use crate::peer::{FramedStreamReader, FramedStreamWriter};
use anyhow::{bail, format_err, Context as _, Error};
use fuchsia_zircon_status as zx_status;
use futures::{future::Either, prelude::*};
use std::sync::Arc;

#[cfg(not(target_os = "fuchsia"))]
use fuchsia_async::emulated_handle::ChannelProxyProtocol;

// We run two tasks to proxy a handle - one to handle handle->stream, the other to handle
// stream->handle. When we want to perform a transfer operation we end up wanting to think about
// just one task, so we provide a join operation here.
#[derive(Debug)]
enum FinishProxyLoopAction<Hdl: Proxyable> {
    InitiateTransfer {
        paired_handle: fidl::Handle,
        drain_stream: FramedStreamWriter,
        stream_ref_sender: StreamRefSender,
        stream_reader: StreamReader<Hdl::Message>,
    },
    FollowTransfer {
        initiate_transfer: ProxyTransferInitiationReceiver,
        new_destination_node: NodeId,
        transfer_key: TransferKey,
        stream_reader: StreamReader<Hdl::Message>,
    },
    Shutdown {
        result: Result<(), zx_status::Status>,
        stream_reader: StreamReader<Hdl::Message>,
    },
}

struct FinishProxyLoopSender<Hdl: Proxyable> {
    chan: futures::channel::oneshot::Sender<FinishProxyLoopAction<Hdl>>,
}
type FinishProxyLoopReceiver<Hdl> = futures::channel::oneshot::Receiver<FinishProxyLoopAction<Hdl>>;

impl<Hdl: 'static + Proxyable> FinishProxyLoopSender<Hdl> {
    fn and_then(self, then: FinishProxyLoopAction<Hdl>) -> Result<(), Error> {
        Ok(self.chan.send(then).map_err(|_| format_err!("Join channel broken"))?)
    }

    // This join is to initiate a new transfer.
    fn and_then_initiate(
        self,
        paired_handle: fidl::Handle,
        drain_stream: FramedStreamWriter,
        stream_ref_sender: StreamRefSender,
        stream_reader: StreamReader<Hdl::Message>,
    ) -> Result<(), Error> {
        self.and_then(FinishProxyLoopAction::InitiateTransfer {
            paired_handle,
            drain_stream,
            stream_ref_sender,
            stream_reader,
        })
    }

    // This join is to follow a transfer initiated by the remote end.
    fn and_then_follow(
        self,
        initiate_transfer: ProxyTransferInitiationReceiver,
        new_destination_node: NodeId,
        transfer_key: TransferKey,
        stream_reader: StreamReader<Hdl::Message>,
    ) -> Result<(), Error> {
        self.and_then(FinishProxyLoopAction::FollowTransfer {
            initiate_transfer,
            new_destination_node,
            transfer_key,
            stream_reader,
        })
    }

    fn and_then_shutdown(
        self,
        result: Result<(), zx_status::Status>,
        stream_reader: StreamReader<Hdl::Message>,
    ) -> Result<(), Error> {
        self.and_then(FinishProxyLoopAction::Shutdown { result, stream_reader })
    }
}

fn new_task_joiner<Hdl: Proxyable>() -> (FinishProxyLoopSender<Hdl>, FinishProxyLoopReceiver<Hdl>) {
    let (tx, rx) = futures::channel::oneshot::channel();
    (FinishProxyLoopSender { chan: tx }, rx)
}

// Spawn a proxy (two tasks, one for each direction of proxying).
pub(crate) async fn run_main_loop<Hdl: 'static + for<'a> ProxyableRW<'a>>(
    proxy: Arc<Proxy<Hdl>>,
    initiate_transfer: ProxyTransferInitiationReceiver,
    stream_writer: FramedStreamWriter,
    initial_stream_reader: Option<FramedStreamReader>,
    stream_reader: FramedStreamReader,
) -> Result<(), Error> {
    #[cfg(not(target_os = "fuchsia"))]
    if stream_writer.is_circuit() {
        proxy.set_channel_proxy_protocol(ChannelProxyProtocol::Cso);
    } else {
        proxy.set_channel_proxy_protocol(ChannelProxyProtocol::Legacy);
    }

    let (tx_join, rx_join) = new_task_joiner();
    let hdl = proxy.hdl();
    let mut stream_writer = stream_writer.bind(hdl);
    let initial_stream_reader = initial_stream_reader.map(|s| s.bind(hdl));
    let mut stream_reader = stream_reader.bind(hdl);

    // TODO: don't detach
    futures::future::try_join(
        async {
            if !stream_reader.is_initiator() {
                stream_reader.expect_hello().await?;
            } else {
                stream_writer.send_hello().await?;
            }
            Ok::<(), Error>(())
        },
        async {
            if let Some(initial_stream_reader) = initial_stream_reader {
                drain(proxy.clone(), initial_stream_reader).await?;
            }
            Ok(())
        },
    )
    .await?;
    futures::future::try_join(
        stream_to_handle(proxy.clone(), initiate_transfer, stream_reader, tx_join)
            .map_err(|e| e.context("stream_to_handle")),
        handle_to_stream(proxy, stream_writer, rx_join).map_err(|e| e.context("handle_to_stream")),
    )
    .map_ok(drop)
    .await
}

async fn handle_to_stream<Hdl: 'static + for<'a> ProxyableRW<'a>>(
    proxy: Arc<Proxy<Hdl>>,
    mut stream: StreamWriter<Hdl::Message>,
    mut finish_proxy_loop: FinishProxyLoopReceiver<Hdl>,
) -> Result<(), Error> {
    let mut message = Default::default();
    let finish_proxy_loop_action = loop {
        let sr =
            futures::future::select(&mut finish_proxy_loop, proxy.read_from_handle(&mut message))
                .await;
        match sr {
            Either::Left((finish_proxy_loop_action, _)) => {
                // Note: Proxy guarantees that read_from_handle can be dropped safely without losing data.
                break finish_proxy_loop_action;
            }
            Either::Right((Err(zx_status::Status::PEER_CLOSED), _)) => {
                if let Some(finish_proxy_loop_action) = finish_proxy_loop.now_or_never() {
                    break finish_proxy_loop_action;
                }
                stream.send_shutdown(Ok(())).await.context("send_shutdown")?;
                return Ok(());
            }
            Either::Right((Err(x), _)) => {
                stream.send_shutdown(Err(x)).await.context("send_shutdown")?;
                return Err(x).context("read_from_handle");
            }
            Either::Right((Ok(ReadValue::Message), _)) => {
                drop(sr);
                stream.send_data(&mut message).await.context("send_data")?;
            }
            Either::Right((Ok(ReadValue::SignalUpdate(signal_update)), _)) => {
                stream.send_signal(signal_update).await.context("send_signal")?;
            }
        };
    };
    let proxy = Arc::try_unwrap(proxy).map_err(|_| format_err!("Proxy should be isolated"))?;
    match finish_proxy_loop_action {
        Ok(FinishProxyLoopAction::InitiateTransfer {
            paired_handle,
            drain_stream,
            stream_ref_sender,
            stream_reader,
        }) => {
            super::xfer::initiate(
                proxy,
                paired_handle,
                stream,
                stream_reader,
                drain_stream,
                stream_ref_sender,
            )
            .await
        }
        Ok(FinishProxyLoopAction::FollowTransfer {
            initiate_transfer,
            new_destination_node,
            transfer_key,
            stream_reader,
        }) => {
            super::xfer::follow(
                proxy,
                initiate_transfer,
                stream,
                new_destination_node,
                transfer_key,
                stream_reader,
            )
            .await
        }
        Ok(FinishProxyLoopAction::Shutdown { result, stream_reader }) => {
            join_shutdown(proxy, stream, stream_reader, result).await
        }
        Err(futures::channel::oneshot::Canceled) => unreachable!(),
    }
}

async fn join_shutdown<Hdl: 'static + Proxyable>(
    proxy: Proxy<Hdl>,
    stream_writer: StreamWriter<Hdl::Message>,
    stream_reader: StreamReader<Hdl::Message>,
    result: Result<(), zx_status::Status>,
) -> Result<(), Error> {
    stream_writer.send_shutdown(result).await?;
    let _ = stream_reader.expect_shutdown(Ok(())).await;
    drop(proxy);
    Ok(())
}

async fn drain<Hdl: 'static + for<'a> ProxyableRW<'a>>(
    proxy: Arc<Proxy<Hdl>>,
    mut drain_stream: StreamReader<Hdl::Message>,
) -> Result<(), Error> {
    loop {
        let frame = drain_stream.next().await?;
        match frame {
            Frame::Data(message) => proxy.write_to_handle(message).await?,
            Frame::SignalUpdate(signal_update) => proxy.apply_signal_update(signal_update)?,
            Frame::EndTransfer => break,
            Frame::BeginTransfer(_, _) => bail!("BeginTransfer on drain stream"),
            Frame::AckTransfer => bail!("AckTransfer on drain stream"),
            Frame::Hello => bail!("Hello frame disallowed on drain streams"),
            Frame::Shutdown(r) => bail!("Stream shutdown during drain: {:?}", r),
        }
    }
    Ok(())
}

async fn stream_to_handle<Hdl: 'static + for<'a> ProxyableRW<'a>>(
    proxy: Arc<Proxy<Hdl>>,
    mut initiate_transfer: ProxyTransferInitiationReceiver,
    mut stream: StreamReader<Hdl::Message>,
    finish_proxy_loop: FinishProxyLoopSender<Hdl>,
) -> Result<(), Error> {
    let removed_from_proxy_table = loop {
        let frame = match futures::future::select(&mut initiate_transfer, stream.next()).await {
            Either::Left((removed_from_proxy_table, _)) => {
                // Note: StreamReader guarantees it's safe to drop a partial read without
                // losing data.
                break removed_from_proxy_table;
            }
            Either::Right((frame, _)) => frame.context("stream.next()")?,
        };
        match frame {
            Frame::Data(message) => {
                if let Err(e) = proxy.write_to_handle(message).await {
                    let _ = finish_proxy_loop.and_then_shutdown(Err(e), stream);
                    match e {
                        zx_status::Status::PEER_CLOSED => {
                            return Ok(());
                        }
                        _ => return Err(e).context("write_to_handle"),
                    }
                }
            }
            Frame::SignalUpdate(signal_update) => proxy.apply_signal_update(signal_update)?,
            Frame::BeginTransfer(new_destination_node, transfer_key) => {
                return finish_proxy_loop
                    .and_then_follow(initiate_transfer, new_destination_node, transfer_key, stream)
                    .context("finish_proxy_loop")
            }
            Frame::EndTransfer => bail!("Received EndTransfer on a regular stream"),
            Frame::AckTransfer => bail!("Received AckTransfer before sending a BeginTransfer"),
            Frame::Hello => bail!("Hello frame received after stream established"),
            Frame::Shutdown(result) => {
                let _ = finish_proxy_loop.and_then_shutdown(result, stream);
                return result.context("Remote shutdown");
            }
        }
    };
    match removed_from_proxy_table {
        Err(e) => Err(e.into()),
        Ok(RemoveFromProxyTable::Dropped) => unreachable!(),
        Ok(RemoveFromProxyTable::InitiateTransfer {
            paired_handle,
            drain_stream,
            stream_ref_sender,
        }) => Ok(finish_proxy_loop.and_then_initiate(
            paired_handle,
            drain_stream,
            stream_ref_sender,
            stream,
        )?),
    }
}
