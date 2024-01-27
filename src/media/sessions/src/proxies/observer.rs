// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::Result;
use fidl_fuchsia_media_sessions2::*;
use fuchsia_zircon as zx;
use futures::{
    stream::{Fuse, Map, Stream, StreamExt},
    Future,
};
use std::{
    pin::Pin,
    task::{Context, Poll},
};
use tracing::warn;

pub enum WatchStatusResponder {
    SessionControl(SessionControlWatchStatusResponder),
    SessionObserver(SessionObserverWatchStatusResponder),
}

impl From<SessionControlWatchStatusResponder> for WatchStatusResponder {
    fn from(src: SessionControlWatchStatusResponder) -> Self {
        WatchStatusResponder::SessionControl(src)
    }
}

impl From<SessionObserverWatchStatusResponder> for WatchStatusResponder {
    fn from(src: SessionObserverWatchStatusResponder) -> Self {
        WatchStatusResponder::SessionObserver(src)
    }
}

impl WatchStatusResponder {
    fn send(self, status: SessionInfoDelta) -> std::result::Result<(), fidl::Error> {
        match self {
            WatchStatusResponder::SessionControl(responder) => responder.send(status),
            WatchStatusResponder::SessionObserver(responder) => responder.send(status),
        }
    }
}

/// Serves a hanging get for a session's status.
///
/// Multiple protocols expose hanging gets for a session's status. Session status'
/// are stored in instances of `SessionInfoDelta` which are emitted as responses
/// to the hanging gets.
///
/// This struct implements the logic between a stream of updates to the session's
/// status and a stream of responders to the client's hanging gets.
///
/// It enforces the rule that only one hanging get may exist at a time, and
/// answers requests with the latest status.
pub struct Observer<S1, S2, I> {
    status: Fuse<S1>,
    responders: Fuse<Map<S2, fn(I) -> WatchStatusResponder>>,
    current_status: Option<SessionInfoDelta>,
    current_responder: Option<WatchStatusResponder>,
}

#[derive(Debug)]
enum UpdateError {
    Disconnect,
    DuplicateRequestForStatus,
}

impl<S1, S2, I> Observer<S1, S2, I>
where
    I: Into<WatchStatusResponder>,
    S1: Stream<Item = SessionInfoDelta> + Unpin,
    S2: Stream<Item = I> + Unpin,
{
    pub fn new(status: S1, responders: S2) -> Self {
        Self {
            status: status.fuse(),
            responders: responders.map(Into::into as fn(I) -> WatchStatusResponder).fuse(),
            current_responder: None,
            current_status: None,
        }
    }

    fn update(&mut self, cx: &mut Context<'_>) -> std::result::Result<(), UpdateError> {
        while let Poll::Ready(next_status) = Pin::new(&mut self.status).poll_next(cx) {
            match next_status {
                Some(next_status) => self.current_status.replace(next_status),
                None => return Err(UpdateError::Disconnect),
            };
        }

        while let Poll::Ready(next_responder) = Pin::new(&mut self.responders).poll_next(cx) {
            match next_responder {
                Some(next_responder) => {
                    if self.current_responder.is_some() {
                        return Err(UpdateError::DuplicateRequestForStatus);
                    } else {
                        self.current_responder.replace(next_responder);
                    }
                }
                None => return Err(UpdateError::Disconnect),
            }
        }

        Ok(())
    }
}

impl<S1, S2, I> Future for Observer<S1, S2, I>
where
    I: Into<WatchStatusResponder>,
    S1: Stream<Item = SessionInfoDelta> + Unpin,
    S2: Stream<Item = I> + Unpin,
{
    type Output = Result<()>;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        match self.update(cx) {
            Err(UpdateError::DuplicateRequestForStatus) => {
                warn!(
                    tag = "observer",
                    concat!(
                        "Client sent a request for status ",
                        "without waiting for a response to a prior request."
                    )
                );

                return Poll::Ready(Ok(()));
            }
            Err(UpdateError::Disconnect) => return Poll::Ready(Ok(())),
            Ok(_) => {}
        };

        if self.current_responder.is_some() && self.current_status.is_some() {
            let responder = self.current_responder.take().expect("Get responder; variant checked");
            let status = self.current_status.take().expect("Get status; variant checked");
            match responder.send(status) {
                Err(fidl::Error::ServerResponseWrite(e)) if e == zx::Status::PEER_CLOSED => {
                    return Poll::Ready(Ok(()));
                }
                Err(e) => {
                    warn!(tag = "observer", "Error writing status to observer: {:?}", e);
                }
                Ok(_) => {}
            }
        }

        Poll::Pending
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::CHANNEL_BUFFER_SIZE;
    use assert_matches::assert_matches;
    use fidl::endpoints::create_proxy;
    use fuchsia_async as fasync;
    use futures::{channel::mpsc, future, sink::SinkExt, task::noop_waker, FutureExt};

    #[fuchsia::test]
    async fn clients_waits_for_new_status() -> Result<()> {
        let (mut status_sink, status_stream) = mpsc::channel(CHANNEL_BUFFER_SIZE);
        let (session_control_proxy, session_control_server) =
            create_proxy::<SessionControlMarker>()?;
        let session_control_requests = session_control_server.into_stream()?;
        let responders = session_control_requests
            .filter_map(|r| future::ready(r.ok()))
            .filter_map(|r| future::ready(r.into_watch_status()));

        let observer = Observer::new(status_stream, responders);
        fasync::Task::spawn(observer.map(drop)).detach();

        let waker = noop_waker();
        let mut ctx = Context::from_waker(&waker);
        let mut status_fut = session_control_proxy.watch_status();
        let poll_result = Pin::new(&mut status_fut).poll(&mut ctx);
        assert_matches!(poll_result, Poll::Pending);

        status_sink.send(SessionInfoDelta::default()).await?;
        let result = status_fut.await;
        assert_matches!(result, Ok(_));

        Ok(())
    }

    #[fuchsia::test]
    async fn client_gets_cached_player_status() -> Result<()> {
        let (mut status_sink, status_stream) = mpsc::channel(CHANNEL_BUFFER_SIZE);
        let (session_control_proxy, session_control_server) =
            create_proxy::<SessionControlMarker>()?;
        let session_control_requests = session_control_server.into_stream()?;
        let responders = session_control_requests
            .filter_map(|r| future::ready(r.ok()))
            .filter_map(|r| future::ready(r.into_watch_status()));

        let observer = Observer::new(status_stream, responders);
        fasync::Task::spawn(observer.map(drop)).detach();

        status_sink.send(SessionInfoDelta::default()).await?;
        assert_matches!(session_control_proxy.watch_status().await, Ok(_));

        Ok(())
    }
}
