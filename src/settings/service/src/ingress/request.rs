// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Single request handling.
//!
//! The request mod defines the components necessary for executing a single, isolated command within
//! the [Job] ecosystem. The most common work type in the setting service that fall in this category
//! are set commands where the requested action can be succinctly captured in a single request in
//! the service.
//!
//! One must define two concrete trait implementations in order to use the components of this mod.
//! The first trait is the [From] trait for [Response]. While [Response] could be broken
//! down into its contained [SettingInfo](crate::base::SettingInfo) and
//! [Error](crate::handler::base::Error) types, callers often only care about the success of a call.
//! For example, set calls typically return an empty value upon success and therefore do not
//! have a value to convert. The second trait is [Responder], which takes the first trait
//! implementation as a parameter. Ths trait allows callers to customize how the response is handled
//! with their own type as defined in the [From<Response>] trait. One should note that the
//! responder itself is passed in on the callback. This allows for the consumption of any resources
//! in the one-time use callback.

use crate::base::SettingType;
use crate::handler::base::{Payload, Request, Response};
use crate::job::work::{Independent, Load};
use crate::job::Job;
use crate::message::base::Audience;
use crate::service::{message, Address};
use crate::trace;
use async_trait::async_trait;
use fuchsia_trace as ftrace;
use std::marker::PhantomData;

/// A [Responder] is passed into [Work] as a handler for responses generated by the work.
pub(crate) trait Responder<R: From<Response> + Send + Sync + 'static> {
    /// Invoked when a response to the request is ready.
    fn respond(self, response: R);
}

/// [Work] executes a single request and passes the results back to a specified responder. Consumers
/// of [Work] specify a [SettingType] along with [Request] for proper routing.
pub(crate) struct Work<R, T>
where
    R: From<Response> + Send + Sync + 'static,
    T: Responder<R> + Send + Sync + 'static,
{
    request: Request,
    setting_type: SettingType,
    responder: T,
    _data: PhantomData<R>,
}

impl<R: From<Response> + Send + Sync + 'static, T: Responder<R> + Send + Sync + 'static>
    Work<R, T>
{
    pub(crate) fn new(setting_type: SettingType, request: Request, responder: T) -> Self {
        Self { setting_type, request, responder, _data: PhantomData }
    }
}

/// [Work] implements the [Independent] trait as each request execution should be done in isolation
/// and executes in the order it was received, not waiting on any existing [Job] of the same group
/// to be executed (as is the case for [crate::job::work::Sequential]).
#[async_trait]
impl<R, T> Independent for Work<R, T>
where
    R: From<Response> + Send + Sync + 'static,
    T: Responder<R> + Send + Sync + 'static,
{
    async fn execute(self: Box<Self>, messenger: message::Messenger, id: ftrace::Id) {
        trace!(id, "Independent Work execute");
        // Send request through MessageHub.
        let mut response_listener = messenger
            .message(
                Payload::Request(self.request.clone()).into(),
                Audience::Address(Address::Handler(self.setting_type)),
            )
            .send();

        // On success, invoke the responder with the converted response.
        self.responder.respond(R::from(match response_listener.next_of::<Payload>().await {
            Ok((payload, _)) => match payload {
                Payload::Response(response) => response,
                _ => {
                    // While it's possible for the request to fail, this will be communicated
                    // through the response for logic related errors or the return value of
                    // receptor::next_of. Work should never encounter a different type of payload
                    // and therefore treat this scenario as fatal.
                    panic!("should not have received a different payload type:{payload:?}");
                }
            },
            _ => Err(crate::handler::base::Error::CommunicationError),
        }));
    }
}

/// The [From] implementation here is for conveniently converting a [Work] definition into a [Job].
/// Since [Work] is a singleshot request, it is automatically converted into a [Load::Independent]
/// workload.
impl<R, T> From<Work<R, T>> for Job
where
    R: From<Response> + Send + Sync + 'static,
    T: Responder<R> + Send + Sync + 'static,
{
    fn from(work: Work<R, T>) -> Job {
        Job::new(Load::Independent(Box::new(work)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::base::MessengerType;
    use crate::service::{Address, MessageHub};
    use assert_matches::assert_matches;
    use fuchsia_async as fasync;
    use futures::channel::oneshot::Sender;

    struct TestResponder {
        sender: Sender<Response>,
    }

    impl TestResponder {
        pub(crate) fn new(sender: Sender<Response>) -> Self {
            Self { sender }
        }
    }

    impl Responder<Response> for TestResponder {
        fn respond(self, response: Response) {
            self.sender.send(response).expect("send of response should succeed");
        }
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn test_request_basic_functionality() {
        // Create MessageHub for communication between components.
        let message_hub_delegate = MessageHub::create_hub();

        // Create mock handler endpoint to receive request.
        let mut handler_receiver = message_hub_delegate
            .create(MessengerType::Addressable(Address::Handler(SettingType::Unknown)))
            .await
            .expect("handler messenger should be created")
            .1;

        // Create job to send request.
        let request = Request::Restore;
        let (response_tx, response_rx) = futures::channel::oneshot::channel::<Response>();
        let work = Box::new(Work::new(
            SettingType::Unknown,
            request.clone(),
            TestResponder::new(response_tx),
        ));

        let work_messenger = message_hub_delegate
            .create(MessengerType::Unbound)
            .await
            .expect("messenger should be created")
            .0;

        // Retrieve signature before passing in messenger to work for verifying the sender of any
        // requests.
        let work_messenger_signature = work_messenger.get_signature();

        // Execute work asynchronously.
        fasync::Task::spawn(work.execute(work_messenger, 0.into())).detach();

        // Ensure the request is sent from the right sender.
        let (received_request, client) =
            handler_receiver.next_of::<Payload>().await.expect("should successfully get request");
        assert_matches!(received_request, Payload::Request(x) if x == request);
        assert!(client.get_author() == work_messenger_signature);

        // Ensure the response is received and forwarded by the work.
        let reply = Ok(None);
        let _ = client.reply(Payload::Response(reply.clone()).into());
        assert!(response_rx.await.expect("should receive successful response") == reply);
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn test_error_propagation() {
        // Create MessageHub for communication between components. Do not create any handler for the
        // test SettingType address.
        let message_hub_delegate = MessageHub::create_hub();

        let (response_tx, response_rx) = futures::channel::oneshot::channel::<Response>();

        // Create job to send request.
        let request = Request::Restore;
        let work = Box::new(Work::new(
            SettingType::Unknown,
            request.clone(),
            TestResponder::new(response_tx),
        ));

        // Execute work on async task.
        fasync::Task::spawn(
            work.execute(
                message_hub_delegate
                    .create(MessengerType::Unbound)
                    .await
                    .expect("messenger should be created")
                    .0,
                0.into(),
            ),
        )
        .detach();

        // Ensure an error was returned, which should match that generated by the request work load.
        assert_matches!(response_rx.await.expect("should receive successful response"),
                Err(x) if x == crate::handler::base::Error::CommunicationError);
    }
}
