// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO: remove pub mod in lib.rs

use crate::lossy_text::LossyText;
use crate::reassembler::Reassembler;
use anyhow::{format_err, Error};
use fuchsia_async::TimeoutExt;
use futures::channel::{mpsc, oneshot};
use futures::lock::Mutex;
use futures::prelude::*;
use overnet_core::{DeframerReader, FrameType, FramerWriter};
use std::collections::HashMap;
use std::time::Duration;

// Flag bits for fragment id's
pub const END_OF_MSG: u8 = 0x80;
const ACK: u8 = 0x40;

pub fn new_fragment_io(
    framer_writer: FramerWriter<LossyText>,
    deframer_reader: DeframerReader<LossyText>,
) -> (FragmentWriter, FragmentReader, impl Future<Output = Result<(), Error>>) {
    let (tx_write, rx_write) = mpsc::channel(0);
    let (tx_read, rx_read) = mpsc::channel(0);

    (
        FragmentWriter { frame_sender: tx_write },
        FragmentReader { frame_receiver: rx_read },
        run_fragment_io(rx_write, tx_read, framer_writer, deframer_reader),
    )
}

async fn run_fragment_io(
    rx_write: mpsc::Receiver<(FrameType, Vec<u8>)>,
    tx_read: mpsc::Sender<(Option<FrameType>, Vec<u8>)>,
    framer_writer: FramerWriter<LossyText>,
    deframer_reader: DeframerReader<LossyText>,
) -> Result<(), Error> {
    let framer_writer = &Mutex::new(framer_writer);
    let (tx_frag, rx_frag) = mpsc::channel(0);
    let ack_set = &Mutex::new(HashMap::new());
    let (mut tx_bat1, rx_bat1) = mpsc::channel(0);
    let (tx_bat2, rx_bat2) = mpsc::channel(0);
    let (tx_bat3, rx_bat3) = mpsc::channel(0);

    futures::future::try_join5(
        read_fragments(tx_read, framer_writer, deframer_reader, ack_set),
        split_fragments(rx_write, framer_writer, tx_frag),
        fragment_sender(rx_bat1, tx_bat2, framer_writer, ack_set),
        fragment_sender(rx_bat2, tx_bat3, framer_writer, ack_set),
        async move {
            // kick off the baton send and then run the third sender
            tx_bat1.send(rx_frag).await?;
            fragment_sender(rx_bat3, tx_bat1, framer_writer, ack_set).await
        },
    )
    .map_ok(drop)
    .await
}

async fn read_fragments(
    mut tx_read: mpsc::Sender<(Option<FrameType>, Vec<u8>)>,
    framer_writer: &Mutex<FramerWriter<LossyText>>,
    mut deframer_reader: DeframerReader<LossyText>,
    ack_set: &Mutex<HashMap<(u8, u8), oneshot::Sender<()>>>,
) -> Result<(), Error> {
    let mut reassembler = Reassembler::new();
    loop {
        let (frame_type, mut frame) = deframer_reader.read().await?;
        match frame_type {
            None | Some(FrameType::OvernetHello) => tx_read.send((frame_type, frame)).await?,
            Some(FrameType::Overnet) => {
                let fragment_id =
                    frame.pop().ok_or_else(|| format_err!("packet too short (no fragment id)"))?;
                let msg_id =
                    frame.pop().ok_or_else(|| format_err!("packet too short (no msg id)"))?;
                if fragment_id & ACK == ACK {
                    if !frame.is_empty() {
                        log::warn!("non-empty ack received for ({}, {})", msg_id, fragment_id);
                    }
                    if let Some(tx) = ack_set.lock().await.remove(&(msg_id, fragment_id & !ACK)) {
                        let _ = tx.send(());
                    }
                    continue;
                }
                framer_writer
                    .lock()
                    .await
                    .write(FrameType::Overnet, &[msg_id, fragment_id | ACK])
                    .await?;
                if let Some(frame) = reassembler.recv(msg_id, fragment_id, frame) {
                    tx_read.send((Some(FrameType::Overnet), frame)).await?;
                }
            }
        }
    }
}

async fn split_fragments(
    mut rx_write: mpsc::Receiver<(FrameType, Vec<u8>)>,
    framer_writer: &Mutex<FramerWriter<LossyText>>,
    mut tx_frag: mpsc::Sender<(u8, u8, Vec<u8>)>,
) -> Result<(), Error> {
    let mut msg_id = 0u8;
    const MAX_FRAGMENT_SIZE: usize = 160;
    while let Some((frame_type, frame)) = rx_write.next().await {
        match frame_type {
            FrameType::OvernetHello => framer_writer.lock().await.write(frame_type, &frame).await?,
            FrameType::Overnet => {
                if frame.len() > 64 * MAX_FRAGMENT_SIZE {
                    continue;
                }
                msg_id = msg_id.wrapping_add(1);
                let mut fragment_id = 0u8;
                let mut frame: &[u8] = &frame;
                while frame.len() > MAX_FRAGMENT_SIZE {
                    tx_frag
                        .send((msg_id, fragment_id, frame[..MAX_FRAGMENT_SIZE].to_vec()))
                        .await?;
                    frame = &frame[MAX_FRAGMENT_SIZE..];
                    fragment_id += 1;
                }
                tx_frag.send((msg_id, fragment_id | END_OF_MSG, frame.to_vec())).await?;
            }
        }
    }
    Ok(())
}

async fn fragment_sender(
    mut rx_baton: mpsc::Receiver<mpsc::Receiver<(u8, u8, Vec<u8>)>>,
    mut tx_baton: mpsc::Sender<mpsc::Receiver<(u8, u8, Vec<u8>)>>,
    framer_writer: &Mutex<FramerWriter<LossyText>>,
    ack_set: &Mutex<HashMap<(u8, u8), oneshot::Sender<()>>>,
) -> Result<(), Error> {
    'next_fragment: loop {
        let mut baton =
            rx_baton.next().await.ok_or_else(|| format_err!("fragment baton passer closed"))?;
        let (msg_id, fragment_id, mut fragment) =
            baton.next().await.ok_or_else(|| format_err!("fragment receiver closed"))?;
        tx_baton.send(baton).await?;

        let (tx_ack, mut rx_ack) = oneshot::channel();
        let tag = (msg_id, fragment_id);
        ack_set.lock().await.insert(tag, tx_ack);

        fragment.push(msg_id);
        fragment.push(fragment_id);

        enum RxError {
            Timeout,
            Canceled,
        }

        'retry_fragment: for _ in 0..10 {
            framer_writer.lock().await.write(FrameType::Overnet, &fragment).await?;
            match (&mut rx_ack)
                .map_err(|_| RxError::Canceled)
                .on_timeout(Duration::from_millis(100), || Err(RxError::Timeout))
                .await
            {
                Ok(()) => continue 'next_fragment,
                Err(RxError::Timeout) => continue 'retry_fragment,
                Err(RxError::Canceled) => break 'retry_fragment,
            }
        }

        // if we reach here we timed out a bunch
        ack_set.lock().await.remove(&tag);
    }
}

pub struct FragmentWriter {
    frame_sender: mpsc::Sender<(FrameType, Vec<u8>)>,
}

impl FragmentWriter {
    pub async fn write(&mut self, frame_type: FrameType, bytes: Vec<u8>) -> Result<(), Error> {
        Ok(self.frame_sender.send((frame_type, bytes)).await?)
    }
}

pub struct FragmentReader {
    frame_receiver: mpsc::Receiver<(Option<FrameType>, Vec<u8>)>,
}

impl FragmentReader {
    pub async fn read(&mut self) -> Result<(Option<FrameType>, Vec<u8>), Error> {
        Ok(self
            .frame_receiver
            .next()
            .await
            .ok_or_else(|| format_err!("failed to receive next frame"))?)
    }
}

#[cfg(test)]
mod test {

    use super::{new_fragment_io, FragmentReader, FragmentWriter};
    use crate::lossy_text::LossyText;
    use crate::test_util::{init, DodgyPipe};
    use anyhow::{format_err, Error};
    use fuchsia_async::TimeoutExt;
    use futures::future::{try_join, try_join4};
    use futures::prelude::*;
    use overnet_core::{new_deframer, new_framer, DeframerWriter, FrameType, FramerReader};
    use std::collections::HashSet;
    use std::time::Duration;

    async fn framer_write(
        mut framer_reader: FramerReader<LossyText>,
        mut pipe: impl AsyncWrite + Unpin,
    ) -> Result<(), Error> {
        loop {
            let frame = framer_reader.read().await?;
            pipe.write_all(&frame).await?;
        }
    }

    async fn deframer_read(
        mut pipe: impl AsyncRead + Unpin,
        mut deframer_writer: DeframerWriter<LossyText>,
    ) -> Result<(), Error> {
        loop {
            let mut buf = [0u8; 1];
            match pipe.read(&mut buf).await? {
                0 => return Ok(()),
                1 => deframer_writer.write(&buf).await?,
                _ => unreachable!(),
            }
        }
    }

    async fn must_not_become_readable(mut rx: FragmentReader) -> Result<(), Error> {
        loop {
            let (frame_type, _) = rx.read().await?;
            assert_eq!(frame_type, None);
        }
    }

    async fn run_test(
        name: &'static str,
        repeat: u64,
        failures_per_64kib: u16,
        frame_type: FrameType,
        messages: &'static [&[u8]],
    ) -> Result<(), Error> {
        init();
        futures::stream::iter(0..repeat)
            .map(Ok)
            .try_for_each_concurrent(10, move |i| async move {
                const INCOMING_BYTE_TIMEOUT: std::time::Duration =
                    std::time::Duration::from_millis(100);
                let (c2s_rx, c2s_tx) = DodgyPipe::new(failures_per_64kib).split();
                let (s2c_rx, s2c_tx) = DodgyPipe::new(failures_per_64kib).split();
                let (c_frm_tx, c_frm_rx) = new_framer(LossyText::new(INCOMING_BYTE_TIMEOUT), 256);
                let (s_frm_tx, s_frm_rx) = new_framer(LossyText::new(INCOMING_BYTE_TIMEOUT), 256);
                let (c_dfrm_tx, c_dfrm_rx) = new_deframer(LossyText::new(INCOMING_BYTE_TIMEOUT));
                let (s_dfrm_tx, s_dfrm_rx) = new_deframer(LossyText::new(INCOMING_BYTE_TIMEOUT));
                let (mut c_tx, c_rx, c_run) = new_fragment_io(c_frm_tx, c_dfrm_rx);
                let (_s_tx, mut s_rx, s_run) = new_fragment_io(s_frm_tx, s_dfrm_rx);
                let (support_fut, support_handle) = try_join4(
                    try_join(c_run, s_run),
                    try_join(framer_write(c_frm_rx, c2s_tx), framer_write(s_frm_rx, s2c_tx)),
                    try_join(deframer_read(c2s_rx, s_dfrm_tx), deframer_read(s2c_rx, c_dfrm_tx)),
                    must_not_become_readable(c_rx),
                )
                .map_ok(drop)
                .remote_handle();
                try_join(
                    async move {
                        support_fut.await;
                        Ok(())
                    },
                    async move {
                        one_test(name, i, frame_type, &messages, &mut c_tx, &mut s_rx)
                            .on_timeout(Duration::from_secs(120), || Err(format_err!("timeout")))
                            .await?;
                        drop(support_handle);
                        Ok(())
                    },
                )
                .map_ok(drop)
                .await
            })
            .await
    }

    async fn one_test(
        name: &str,
        iteration: u64,
        frame_type: FrameType,
        messages: &[&[u8]],
        tx: &mut FragmentWriter,
        rx: &mut FragmentReader,
    ) -> Result<(), Error> {
        try_join(
            async move {
                for (i, message) in messages.iter().enumerate() {
                    log::info!("{}[run {}, msg {}] begin write", name, iteration, i);
                    tx.write(frame_type, message.to_vec()).await?;
                    log::info!("{}[run {}, msg {}] done write", name, iteration, i);
                }
                Ok(())
            },
            async move {
                let n = messages.len();
                let mut found = HashSet::new();
                while found.len() != n {
                    log::info!("{}[run {}, msg {}] begin read", name, iteration, found.len());
                    let (got_frame_type, msg) = rx.read().await?;
                    log::info!(
                        "{}[run {}, msg {}] got frame {:?} len {}",
                        name,
                        iteration,
                        found.len(),
                        got_frame_type,
                        msg.len()
                    );
                    if got_frame_type.is_none() {
                        continue;
                    }
                    assert_eq!(got_frame_type.unwrap(), frame_type);
                    let index = messages
                        .iter()
                        .enumerate()
                        .find(|(_, message)| msg == **message)
                        .unwrap()
                        .0;
                    assert!(found.insert(index));
                }
                Ok(())
            },
        )
        .map_ok(drop)
        .await
    }

    const LONG_PACKET: &'static [u8; 8192] = std::include_bytes!("long_packet.bin");

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hello() -> Result<(), Error> {
        run_test("hello", 1, 0, FrameType::OvernetHello, &[b"hello world"]).await
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn simple() -> Result<(), Error> {
        run_test("hello", 1, 0, FrameType::Overnet, &[b"hello world"]).await
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn long() -> Result<(), Error> {
        run_test("hello", 1, 0, FrameType::Overnet, &[LONG_PACKET]).await
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn long_flaky() -> Result<(), Error> {
        run_test("hello", 100, 10, FrameType::Overnet, &[LONG_PACKET]).await
    }
}
