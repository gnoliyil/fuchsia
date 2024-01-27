// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A multi-producer, single-consumer queue for sending requests across asynchronous tasks.
//!
//! Channel creation provides `Receiver` and `Sender` handles. `Sender` can make requests that
//! await a response from the `Receiver`. Every message sent across the channel is packaged with
//! a `Responder` that is used to respond to that request. A `Sender` will wait until a response is
//! received before `Sender::request` completes.
//!
//! ### Disconnection
//! When all `Sender` handles have been dropped, it is no longer possible to send requests into the
//! channel. As such, `Receiver::receive` will return an error.
//!
//! ### Clean Shutdown
//! If a `Receiver` is dropped, it is possible for there to be messages in the channel that will
//! never be processed. If a clean shutdown is desired, a receiver can first call `Receiver::close`
//! to prevent further messages from being sent into the channel. Then, the receiver can handle all
//! messages in the channel and be dropped.

use {
    anyhow::Error,
    futures::{
        channel::{mpsc, oneshot},
        stream::{FusedStream, Stream},
        SinkExt,
    },
    std::{
        pin::Pin,
        task::{Context, Poll},
    },
};

/// The requesting end of a channel.
pub struct Sender<Req, Resp> {
    inner: mpsc::Sender<(Req, Responder<Resp>)>,
}

impl<Req, Resp> Clone for Sender<Req, Resp> {
    fn clone(&self) -> Self {
        Self { inner: self.inner.clone() }
    }
}

impl<Req, Resp> Sender<Req, Resp> {
    /// Send a request on the channel and wait for a response from the responding end of the
    /// channel.
    /// An error is returned if the `Receiver` has been dropped or the `Receiver` drops the
    /// `Responder` for this request.
    pub async fn request(&mut self, value: Req) -> Result<Resp, Error> {
        let (responder, response) = oneshot::channel();
        self.inner.send((value, Responder { inner: responder })).await?;
        Ok(response.await?)
    }
}

/// Responds to a single request with a value.
pub struct Responder<Resp> {
    inner: oneshot::Sender<Resp>,
}

impl<Resp> Responder<Resp> {
    /// Send a response value. If the `Sender` is no longer waiting on a response because the
    /// request future has been dropped, this method will return the original response `value` as
    /// an `Err`.
    pub fn respond(self, value: Resp) -> Result<(), Resp> {
        self.inner.send(value)
    }
}

/// The responding end of a channel.
// TODO(http://fxbug.dev/82145): Consider replacing this with this alias:
//
//   pub type Receiver<Req, Resp> = mpsc::Receiver<(Req, Responder<Resp>)>;
pub struct Receiver<Req, Resp> {
    inner: mpsc::Receiver<(Req, Responder<Resp>)>,
}

impl<Req, Resp> Receiver<Req, Resp> {
    /// Close the responding end of the channel.
    ///
    /// This prevents further messages from being sent on the channel while still enabling the
    /// receiver to drain messages that are buffered.
    pub fn close(&mut self) {
        self.inner.close();
    }

    /// Try to receive the next message without notifying a context if empty.
    ///
    /// This function will panic if called after `try_next` has returned `None` or `receive` has
    /// returned an `Err`.
    pub fn try_receive(&mut self) -> Result<Option<(Req, Responder<Resp>)>, Error> {
        Ok(self.inner.try_next()?)
    }
}

impl<Req, Resp> Stream for Receiver<Req, Resp> {
    type Item = (Req, Responder<Resp>);

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        Pin::new(&mut self.inner).poll_next(cx)
    }
}

impl<Req, Resp> FusedStream for Receiver<Req, Resp> {
    fn is_terminated(&self) -> bool {
        self.inner.is_terminated()
    }
}

/// Create a new asynchronous channel with a bounded capacity, returning the sender/receiver
/// halves.
///
/// This channel follows the semantics of a futures::mpsc::channel when at capacity.
pub fn channel<Req, Resp>(buffer: usize) -> (Sender<Req, Resp>, Receiver<Req, Resp>) {
    let (inner_sender, inner_receiver) = mpsc::channel(buffer);
    (Sender { inner: inner_sender }, Receiver { inner: inner_receiver })
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fuchsia_async as fasync,
        futures::{pin_mut, StreamExt},
        std::task::Poll,
    };

    macro_rules! unwrap_ready {
        ($poll:expr) => {
            match $poll {
                Poll::Ready(value) => value,
                Poll::Pending => panic!("not ready"),
            }
        };
    }

    #[test]
    fn sender_receives_response() {
        let mut ex = fasync::TestExecutor::new();
        let (mut sender, mut receiver) = channel(0);

        let received = receiver.next();
        pin_mut!(received);
        assert!(ex.run_until_stalled(&mut received).is_pending());

        let request = sender.request(());
        pin_mut!(request);
        assert!(ex.run_until_stalled(&mut request).is_pending());

        let ((), responder) = unwrap_ready!(ex.run_until_stalled(&mut received)).unwrap();

        assert!(ex.run_until_stalled(&mut request).is_pending());

        responder.respond(()).unwrap();

        unwrap_ready!(ex.run_until_stalled(&mut request)).unwrap();
    }

    #[test]
    fn cloned_senders_go_to_same_receiver() {
        let mut ex = fasync::TestExecutor::new();
        let (mut sender, mut receiver) = channel(0);
        let mut sender2 = sender.clone();

        let received = receiver.next();
        pin_mut!(received);
        assert!(ex.run_until_stalled(&mut received).is_pending());

        let request = sender.request(());
        pin_mut!(request);
        assert!(ex.run_until_stalled(&mut request).is_pending());

        let ((), responder) = unwrap_ready!(ex.run_until_stalled(&mut received)).unwrap();

        assert!(ex.run_until_stalled(&mut request).is_pending());

        responder.respond(()).unwrap();

        unwrap_ready!(ex.run_until_stalled(&mut request)).unwrap();

        let received = receiver.next();
        pin_mut!(received);
        assert!(ex.run_until_stalled(&mut received).is_pending());

        let request = sender2.request(());
        pin_mut!(request);
        assert!(ex.run_until_stalled(&mut request).is_pending());

        let ((), responder) = unwrap_ready!(ex.run_until_stalled(&mut received)).unwrap();

        assert!(ex.run_until_stalled(&mut request).is_pending());

        responder.respond(()).unwrap();

        unwrap_ready!(ex.run_until_stalled(&mut request)).unwrap();
    }

    #[test]
    fn sender_receives_error_on_dropped_receiver() {
        let mut ex = fasync::TestExecutor::new();
        let (mut sender, receiver) = channel::<(), ()>(0);

        let request = sender.request(());
        pin_mut!(request);
        assert!(ex.run_until_stalled(&mut request).is_pending());

        drop(receiver);

        assert!(unwrap_ready!(ex.run_until_stalled(&mut request)).is_err());
    }

    #[test]
    fn sender_receives_error_on_dropped_responder() {
        let mut ex = fasync::TestExecutor::new();
        let (mut sender, mut receiver) = channel::<(), ()>(0);

        let request = sender.request(());
        pin_mut!(request);
        assert!(ex.run_until_stalled(&mut request).is_pending());

        let received = receiver.next();
        pin_mut!(received);
        let ((), responder) = unwrap_ready!(ex.run_until_stalled(&mut received)).unwrap();

        assert!(ex.run_until_stalled(&mut request).is_pending());
        drop(responder);

        assert!(unwrap_ready!(ex.run_until_stalled(&mut request)).is_err());
    }

    #[test]
    fn receiver_receives_error_on_dropped_sender() {
        let mut ex = fasync::TestExecutor::new();
        let (sender, mut receiver) = channel::<(), ()>(0);

        let received = receiver.next();
        pin_mut!(received);
        assert!(ex.run_until_stalled(&mut received).is_pending());

        drop(sender);

        assert!(unwrap_ready!(ex.run_until_stalled(&mut received)).is_none());
    }

    #[test]
    fn responder_returns_error_on_dropped_sender() {
        let mut ex = fasync::TestExecutor::new();
        let (mut sender, mut receiver) = channel(0);

        {
            let request = sender.request(());
            pin_mut!(request);
            assert!(ex.run_until_stalled(&mut request).is_pending());
        } // request is dropped at the end of the block

        let received = receiver.next();
        pin_mut!(received);
        let ((), responder) = unwrap_ready!(ex.run_until_stalled(&mut received)).unwrap();

        drop(sender);

        assert!(responder.respond(()).is_err());
    }

    #[fasync::run_until_stalled(test)]
    async fn cannot_request_after_receiver_closed() {
        let (mut sender, mut receiver) = channel::<(), ()>(0);
        receiver.close();
        assert!(sender.request(()).await.is_err());
    }

    #[test]
    fn try_receive_returns_none_when_channel_is_empty() {
        let (_, mut receiver) = channel::<(), ()>(0);
        assert!(receiver.try_receive().unwrap().is_none());
    }

    #[test]
    fn try_receive_returns_none_after_none_result() {
        let (_, mut receiver) = channel::<(), ()>(0);
        assert!(receiver.try_receive().unwrap().is_none());
        assert!(receiver.try_receive().unwrap().is_none());
    }

    #[test]
    fn try_receive_returns_value_when_channel_has_value() {
        let mut ex = fasync::TestExecutor::new();
        let (mut sender, mut receiver) = channel::<(), ()>(0);

        let request = sender.request(());
        pin_mut!(request);
        assert!(ex.run_until_stalled(&mut request).is_pending());

        assert!(receiver.try_receive().unwrap().is_some());
    }
}
