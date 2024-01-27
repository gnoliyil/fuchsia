// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::job::{self, data};
use crate::service::message::{Audience, Messenger, Signature};
use crate::service::test::Payload;
use async_trait::async_trait;
use fuchsia_trace as ftrace;
use futures::future::BoxFuture;
use std::sync::Arc;

pub(crate) mod channel {
    use crate::job;
    use crate::service::message::Messenger;
    use async_trait::async_trait;
    use fuchsia_trace as ftrace;
    use futures::channel::mpsc::UnboundedSender;

    #[derive(Debug)]
    pub(crate) enum State {
        Execute,
    }

    pub(crate) struct Workload {
        state_sender: UnboundedSender<State>,
    }

    impl Workload {
        pub(crate) fn new(state_sender: UnboundedSender<State>) -> Self {
            Self { state_sender }
        }
    }

    #[async_trait]
    impl job::work::Independent for Workload {
        async fn execute(self: Box<Self>, _messenger: Messenger, _id: ftrace::Id) {
            self.state_sender.unbounded_send(State::Execute).expect("should succeed");
        }
    }
}

/// [StubWorkload] provides a blank workload to be a placeholder in tests.
pub(crate) struct StubWorkload;

impl StubWorkload {
    pub(crate) fn new() -> Box<Self> {
        Box::new(Self {})
    }
}

#[async_trait]
impl job::work::Independent for StubWorkload {
    async fn execute(self: Box<Self>, _messenger: Messenger, _id: ftrace::Id) {}
}

#[async_trait]
impl job::work::Sequential for StubWorkload {
    async fn execute(
        self: Box<Self>,
        _messenger: Messenger,
        _store: job::data::StoreHandle,
        _id: ftrace::Id,
    ) -> Result<(), job::work::Error> {
        Ok(())
    }
}

/// [Workload] provides a simple implementation of [Workload](job::Workload) for sending a test
/// Payload to a given target.
pub(crate) struct Workload {
    /// The payload to be delivered.
    payload: Payload,
    /// The [Signature] of the recipient to receive the payload.
    target: Signature,
}

impl Workload {
    pub(crate) fn new(payload: Payload, target: Signature) -> Box<Self> {
        Box::new(Self { payload, target })
    }
}

#[async_trait]
impl job::work::Independent for Workload {
    async fn execute(self: Box<Self>, messenger: Messenger, _id: ftrace::Id) {
        messenger.message(self.payload.into(), Audience::Messenger(self.target)).ack();
    }
}

#[async_trait]
impl job::work::Sequential for Workload {
    async fn execute(
        self: Box<Self>,
        messenger: Messenger,
        _store: data::StoreHandle,
        _id: ftrace::Id,
    ) -> Result<(), job::work::Error> {
        messenger.message(self.payload.clone().into(), Audience::Messenger(self.target)).ack();
        Ok(())
    }
}

/// [Workload] provides a simple implementation of [Workload](job::Workload) for sending a test
/// Payload to a given target.
pub(crate) struct Sequential<
    T: Fn(Messenger, data::StoreHandle) -> BoxFuture<'static, ()> + Send + Sync,
> {
    /// The payload to be delivered.
    callback: Arc<T>,
}

impl<T: Fn(Messenger, data::StoreHandle) -> BoxFuture<'static, ()> + Send + Sync> Sequential<T> {
    pub(crate) fn boxed(callback: T) -> Box<Self> {
        Box::new(Self { callback: Arc::new(callback) })
    }
}

#[async_trait]
impl<T: Fn(Messenger, data::StoreHandle) -> BoxFuture<'static, ()> + Send + Sync>
    job::work::Sequential for Sequential<T>
{
    async fn execute(
        self: Box<Self>,
        messenger: Messenger,
        store: data::StoreHandle,
        _id: ftrace::Id,
    ) -> Result<(), job::work::Error> {
        (self.callback)(messenger, store).await;
        Ok(())
    }
}
