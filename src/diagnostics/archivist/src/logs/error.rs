// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

use crate::events::error::EventError;
use fuchsia_zircon as zx;
use thiserror::Error;

use super::listener::ListenerError;
use diagnostics_message::error::MessageError;

#[derive(Debug, Error)]
pub enum LogsError {
    #[error("couldn't connect to {protocol}: {source}")]
    ConnectingToService { protocol: &'static str, source: anyhow::Error },

    #[error("couldn't retrieve the ReadOnlyLog debuglog handle: {source}")]
    RetrievingDebugLog { source: fidl::Error },

    #[error("malformed event: `{source}`")]
    MalformedEvent {
        #[from]
        source: EventError,
    },

    #[error("error while handling {protocol} requests: {source}")]
    HandlingRequests { protocol: &'static str, source: fidl::Error },

    #[error("error from a listener: {source}")]
    Listener {
        #[from]
        source: ListenerError,
    },
}

#[derive(Debug, Error)]
pub enum StreamError {
    #[error("couldn't read from socket. Status: {0}")]
    Io(#[from] zx::Status),
    #[error(transparent)]
    Message(#[from] MessageError),
    #[error("couldn't convert debuglog message")]
    DebugLogMessage,
}

#[cfg(test)]
impl PartialEq for StreamError {
    fn eq(&self, other: &Self) -> bool {
        use StreamError::*;
        match (self, other) {
            (Io(s1), Io(s2)) => s1 == s2,
            (Message(source), Message(s2)) => source == s2,
            _ => false,
        }
    }
}
