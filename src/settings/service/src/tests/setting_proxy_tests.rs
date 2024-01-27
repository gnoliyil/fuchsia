// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::HashMap;
use std::sync::Arc;
use std::task::Poll;

use fuchsia_async as fasync;
use fuchsia_inspect::{assert_data_tree, Inspector};
use fuchsia_zircon::{Duration, DurationNum, Time};
use futures::channel::mpsc::{UnboundedReceiver, UnboundedSender};
use futures::channel::oneshot;
use futures::lock::Mutex;
use futures::StreamExt;

use async_trait::async_trait;

use assert_matches::assert_matches;

use crate::base::{SettingType, UnknownInfo};
use crate::handler::base::{
    Error as HandlerError, Payload as HandlerPayload, Request, Response, SettingHandlerFactory,
    SettingHandlerFactoryError,
};
use crate::handler::setting_handler::{
    self, Command, ControllerError, Event, ExitResult, SettingHandlerResult, State,
};
use crate::handler::setting_proxy::{SettingProxy, MAX_NODE_ERRORS};
use crate::inspect::listener_logger::ListenerInspectLogger;
use crate::message::base::{Audience, MessageEvent, MessengerType};
use crate::message::receptor::Receptor;
use crate::message::MessageHubUtil;
use crate::service::{
    self, message, Address as ServiceAddress, Payload as ServicePayload, TryFromWithClient,
};
use crate::{clock, event, Payload};

const TEARDOWN_TIMEOUT: Duration = Duration::from_seconds(5);
const SETTING_PROXY_MAX_ATTEMPTS: u64 = 3;
const SETTING_PROXY_TIMEOUT_MS: i64 = 1;

type ListenReceptor = Receptor<ServicePayload, ServiceAddress>;

struct SettingHandler {
    setting_type: SettingType,
    messenger: service::message::Messenger,
    state_tx: UnboundedSender<State>,
    responses: Vec<(Request, HandlerAction)>,
    done_tx: Option<oneshot::Sender<()>>,
    proxy_signature: service::message::Signature,
}

#[derive(Debug)]
enum HandlerAction {
    Ignore,
    Exit(ExitResult),
    Respond(SettingHandlerResult),
}

impl SettingHandler {
    #[allow(clippy::result_large_err)] // TODO(fxbug.dev/117896)
    fn process_state(&mut self, state: State) -> SettingHandlerResult {
        self.state_tx.unbounded_send(state).unwrap();
        Ok(None)
    }

    fn queue_action(&mut self, request: Request, action: HandlerAction) {
        self.responses.push((request, action))
    }

    fn notify(&self) {
        self.messenger
            .message(
                setting_handler::Payload::Event(Event::Changed(UnknownInfo(true).into())).into(),
                Audience::Messenger(self.proxy_signature),
            )
            .send()
            .ack();
    }

    fn process_request(&mut self, request: Request) -> Option<SettingHandlerResult> {
        if let Some((match_request, action)) = self.responses.pop() {
            if request == match_request {
                match action {
                    HandlerAction::Respond(result) => {
                        return Some(result);
                    }
                    HandlerAction::Ignore => {
                        return None;
                    }
                    HandlerAction::Exit(result) => {
                        self.messenger
                            .message(
                                setting_handler::Payload::Event(Event::Exited(result)).into(),
                                Audience::Messenger(self.proxy_signature),
                            )
                            .send()
                            .ack();
                        return None;
                    }
                }
            }
        }

        Some(Err(ControllerError::UnimplementedRequest(self.setting_type, request)))
    }

    fn create(
        messenger: service::message::Messenger,
        mut receptor: service::message::Receptor,
        proxy_signature: service::message::Signature,
        setting_type: SettingType,
        state_tx: UnboundedSender<State>,
        done_tx: Option<oneshot::Sender<()>>,
    ) -> Arc<Mutex<Self>> {
        let handler = Arc::new(Mutex::new(Self {
            messenger,
            setting_type,
            state_tx,
            responses: vec![],
            done_tx,
            proxy_signature,
        }));

        let handler_clone = handler.clone();
        fasync::Task::spawn(async move {
            while let Some(event) = receptor.next().await {
                match event {
                    MessageEvent::Message(
                        service::Payload::Controller(setting_handler::Payload::Command(
                            Command::HandleRequest(request),
                        )),
                        client,
                    ) => {
                        if let Some(response) = handler_clone.lock().await.process_request(request)
                        {
                            setting_handler::reply(client, response);
                        }
                    }
                    MessageEvent::Message(
                        service::Payload::Controller(setting_handler::Payload::Command(
                            Command::ChangeState(state),
                        )),
                        client,
                    ) => {
                        setting_handler::reply(
                            client,
                            handler_clone.lock().await.process_state(state),
                        );
                    }
                    _ => {}
                }
            }

            if let Some(done_tx) = handler_clone.lock().await.done_tx.take() {
                let _ = done_tx.send(());
            }
        })
        .detach();

        handler
    }
}

struct FakeFactory {
    handlers: HashMap<SettingType, service::message::Signature>,
    request_counts: HashMap<SettingType, u64>,
    delegate: service::message::Delegate,
}

impl FakeFactory {
    fn new(delegate: service::message::Delegate) -> Self {
        FakeFactory { handlers: HashMap::new(), request_counts: HashMap::new(), delegate }
    }

    async fn create(
        &mut self,
        setting_type: SettingType,
    ) -> (service::message::Messenger, service::message::Receptor) {
        let (client, receptor) = self.delegate.create(MessengerType::Unbound).await.unwrap();
        let _ = self.handlers.insert(setting_type, receptor.get_signature());

        (client, receptor)
    }

    fn get_request_count(&mut self, setting_type: SettingType) -> u64 {
        if let Some(count) = self.request_counts.get(&setting_type) {
            *count
        } else {
            0
        }
    }
}

#[async_trait]
impl SettingHandlerFactory for FakeFactory {
    async fn generate(
        &mut self,
        setting_type: SettingType,
        _: service::message::Delegate,
        _: service::message::Signature,
    ) -> Result<service::message::Signature, SettingHandlerFactoryError> {
        Ok(self
            .handlers
            .get(&setting_type)
            .copied()
            .map(|signature| {
                *self.request_counts.entry(setting_type).or_insert(0) += 1;
                signature
            })
            .unwrap())
    }
}

struct TestEnvironmentBuilder {
    setting_type: SettingType,
    done_tx: Option<oneshot::Sender<()>>,
    timeout: Option<(Duration, bool)>,
}

impl TestEnvironmentBuilder {
    fn new(setting_type: SettingType) -> Self {
        Self { setting_type, done_tx: None, timeout: None }
    }

    fn set_done_tx(mut self, tx: Option<oneshot::Sender<()>>) -> Self {
        self.done_tx = tx;
        self
    }

    fn set_timeout(mut self, duration: Duration, retry_on_timeout: bool) -> Self {
        self.timeout = Some((duration, retry_on_timeout));
        self
    }

    async fn build(self) -> TestEnvironment {
        let delegate = service::MessageHub::create_hub();

        let handler_factory = Arc::new(Mutex::new(FakeFactory::new(delegate.clone())));

        let inspector = Inspector::new();
        let listener_logger =
            Arc::new(Mutex::new(ListenerInspectLogger::with_inspector(&inspector)));
        let proxy_handler_signature = SettingProxy::create(
            self.setting_type,
            handler_factory.clone(),
            delegate.clone(),
            SETTING_PROXY_MAX_ATTEMPTS,
            TEARDOWN_TIMEOUT,
            self.timeout.map(|(duration, _)| duration),
            self.timeout.map_or(true, |(_, retry)| retry),
            inspector.root().create_child("test"),
            listener_logger.clone(),
        )
        .await
        .expect("proxy creation should succeed");

        let (service_client, _) = delegate.create(MessengerType::Unbound).await.unwrap();

        let (handler_messenger, handler_receptor) =
            handler_factory.lock().await.create(self.setting_type).await;
        let (state_tx, state_rx) = futures::channel::mpsc::unbounded::<State>();
        let handler = SettingHandler::create(
            handler_messenger,
            handler_receptor,
            proxy_handler_signature,
            self.setting_type,
            state_tx,
            self.done_tx,
        );

        TestEnvironment {
            proxy_handler_signature,
            service_client,
            handler_factory,
            setting_handler_rx: state_rx,
            setting_handler: handler,
            setting_type: self.setting_type,
            delegate,
            inspector,
        }
    }
}

struct TestEnvironment {
    proxy_handler_signature: service::message::Signature,
    service_client: service::message::Messenger,
    handler_factory: Arc<Mutex<FakeFactory>>,
    setting_handler_rx: UnboundedReceiver<State>,
    setting_handler: Arc<Mutex<SettingHandler>>,
    setting_type: SettingType,
    delegate: service::message::Delegate,
    inspector: Inspector,
}

impl TestEnvironment {
    async fn regenerate_handler(&mut self, done_tx: Option<oneshot::Sender<()>>) {
        let (handler_messenger, handler_receptor) =
            self.handler_factory.lock().await.create(self.setting_type).await;
        let (state_tx, state_rx) = futures::channel::mpsc::unbounded::<State>();
        self.setting_handler = SettingHandler::create(
            handler_messenger,
            handler_receptor,
            self.proxy_handler_signature,
            self.setting_type,
            state_tx,
            done_tx,
        );

        self.setting_handler_rx = state_rx;
    }
}

// Initializes an environment set up to handle Listen requests.
async fn init_listen_env() -> (TestEnvironment, ListenReceptor) {
    let setting_type = SettingType::Unknown;
    let environment = TestEnvironmentBuilder::new(setting_type).build().await;

    // Send a listen state and make sure sink is notified.
    let mut listen_receptor = environment
        .service_client
        .message(
            service::Payload::Setting(HandlerPayload::Request(Request::Listen)),
            Audience::Address(service::Address::Handler(setting_type)),
        )
        .send();

    assert!(listen_receptor.wait_for_acknowledge().await.is_ok(), "ack should be sent");
    (environment, listen_receptor)
}

// Executes a Listen request.
async fn run_listen(env: Arc<Mutex<TestEnvironment>>) {
    let mut environment = env.lock().await;
    environment.setting_handler.lock().await.notify();

    if let Some(state) = environment.setting_handler_rx.next().await {
        assert_eq!(state, State::Listen);
    } else {
        panic!("should have received state update");
    }
}

// Executes an EndListen request.
async fn run_end_listen(env: Arc<Mutex<TestEnvironment>>, listen_receptor: ListenReceptor) {
    let mut environment = env.lock().await;
    // Drop the listener so the service transitions into teardown.
    drop(listen_receptor);

    if let Some(state) = environment.setting_handler_rx.next().await {
        assert_eq!(state, State::EndListen);
    } else {
        panic!("should have received EndListen state update");
    }
}

// Ensures setting proxy registers with the MessageHub.
#[fuchsia_async::run_until_stalled(test)]
async fn test_message_hub_presence() {
    let setting_type = SettingType::Unknown;
    let environment = TestEnvironmentBuilder::new(setting_type).build().await;

    assert!(environment
        .delegate
        .contains(service::message::Signature::Address(service::Address::Handler(setting_type)))
        .await
        .expect("should have result"));
}

#[test]
fn test_notify() {
    let mut executor =
        fasync::TestExecutor::new_with_fake_time().expect("Failed to create executor");

    let environment_fut = init_listen_env();
    futures::pin_mut!(environment_fut);
    let (environment, receptor) =
        if let Poll::Ready((env, receptor)) = executor.run_until_stalled(&mut environment_fut) {
            (env, receptor)
        } else {
            panic!("environment creation stalled");
        };

    let env_handle = Arc::new(Mutex::new(environment));

    let listen_fut = run_listen(env_handle.clone());
    futures::pin_mut!(listen_fut);
    if let Poll::Ready(res) = executor.run_until_stalled(&mut listen_fut) {
        res
    } else {
        panic!("Listen failed");
    };

    let end_listen_fut = run_end_listen(env_handle.clone(), receptor);
    futures::pin_mut!(end_listen_fut);
    if let Poll::Ready(res) = executor.run_until_stalled(&mut end_listen_fut) {
        res
    } else {
        panic!("EndListen failed");
    };

    // Validate that the teardown timeout matches the constant.
    let deadline = crate::clock::now() + TEARDOWN_TIMEOUT;
    assert_eq!(Some(deadline), executor.wake_next_timer().map(Into::into));

    let env_fut = env_handle.lock();
    futures::pin_mut!(env_fut);
    let mut environment_lock =
        if let Poll::Ready(env_lock) = executor.run_until_stalled(&mut env_fut) {
            env_lock
        } else {
            panic!("Failed to acquire environment lock");
        };

    let state_fut = environment_lock.setting_handler_rx.next();
    futures::pin_mut!(state_fut);
    let state = if let Poll::Ready(state) = executor.run_until_stalled(&mut state_fut) {
        state
    } else {
        panic!("state retrieval stalled");
    };

    if let Some(state) = state {
        assert_eq!(state, State::Teardown);
    } else {
        panic!("should have received Teardown state update");
    }
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_request() {
    let setting_type = SettingType::Unknown;
    let environment = TestEnvironmentBuilder::new(setting_type).build().await;

    environment
        .setting_handler
        .lock()
        .await
        .queue_action(Request::Get, HandlerAction::Respond(Ok(None)));

    // Send initial request.
    let mut receptor = environment
        .service_client
        .message(
            HandlerPayload::Request(Request::Get).into(),
            Audience::Address(service::Address::Handler(setting_type)),
        )
        .send();

    if let Ok((HandlerPayload::Response(response), _)) = receptor.next_of::<HandlerPayload>().await
    {
        assert!(response.is_ok());
        assert_eq!(None, response.unwrap());
    } else {
        panic!("should have received response");
    }
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_request_order() {
    let setting_type = SettingType::Unknown;
    let request_id_1 = 0;
    let request_id_2 = 1;
    let environment = TestEnvironmentBuilder::new(setting_type).build().await;

    environment
        .setting_handler
        .lock()
        .await
        .queue_action(Request::Get, HandlerAction::Respond(Ok(None)));

    // Send multiple requests.
    let receptor_1 = environment
        .service_client
        .message(
            HandlerPayload::Request(Request::Get).into(),
            Audience::Address(service::Address::Handler(setting_type)),
        )
        .send();
    let receptor_2 = environment
        .service_client
        .message(
            HandlerPayload::Request(Request::Get).into(),
            Audience::Address(service::Address::Handler(setting_type)),
        )
        .send();

    // Wait for both requests to finish and add them to the list as they finish so we can verify the
    // order.
    let mut completed_request_ids = Vec::<u64>::new();
    let mut receptor_1_fuse = receptor_1.fuse();
    let mut receptor_2_fuse = receptor_2.fuse();
    loop {
        environment
            .setting_handler
            .lock()
            .await
            .queue_action(Request::Get, HandlerAction::Respond(Ok(None)));
        futures::select! {
            payload_1 = receptor_1_fuse.next() => {
                if let Ok((HandlerPayload::Response(_), _)) =
                        payload_1.map_or(Err(String::from("no event")),
                        HandlerPayload::try_from_with_client) {
                    // First request finishes first.
                    assert_eq!(completed_request_ids.len(), 0);
                    completed_request_ids.push(request_id_1);
                }
            },
            payload_2 = receptor_2_fuse.next() => {
                if let Ok((HandlerPayload::Response(_), _)) =
                        payload_2.map_or(Err(String::from("no event")),
                        HandlerPayload::try_from_with_client) {
                    assert_eq!(completed_request_ids.len(), 1);
                    completed_request_ids.push(request_id_2);
                }
            },
            complete => break,
        }
    }
}

struct ErrorFactory;

#[async_trait]
impl SettingHandlerFactory for ErrorFactory {
    async fn generate(
        &mut self,
        setting_type: SettingType,
        _delegate: message::Delegate,
        _notifier_signature: message::Signature,
    ) -> Result<message::Signature, SettingHandlerFactoryError> {
        Err(SettingHandlerFactoryError::HandlerStartupError(setting_type, "test error".into()))
    }
}

#[fasync::run_until_stalled(test)]
async fn inspect_catches_errors() {
    // Set the clock so that timestamps will always be 0.
    clock::mock::set(Time::from_nanos(0));

    const SETTING_TYPE: SettingType = SettingType::Unknown;

    let delegate = service::MessageHub::create_hub();
    let (service_client, _) = delegate.create(MessengerType::Unbound).await.unwrap();
    let handler_factory = Arc::new(Mutex::new(ErrorFactory));
    let inspector = Inspector::new();
    let _proxy_handler_signature = SettingProxy::create(
        SETTING_TYPE,
        handler_factory,
        delegate,
        SETTING_PROXY_MAX_ATTEMPTS,
        TEARDOWN_TIMEOUT,
        None,
        false,
        inspector.root().create_child("test"),
        Arc::new(Mutex::new(ListenerInspectLogger::with_inspector(&inspector))),
    )
    .await
    .expect("proxy creation should succeed");

    let mut receptor = service_client
        .message(
            HandlerPayload::Request(Request::Get).into(),
            Audience::Address(service::Address::Handler(SETTING_TYPE)),
        )
        .send();

    let (payload, _) = receptor.next_payload().await.expect("should get payload");
    assert_matches::assert_matches!(
        payload,
        Payload::Setting(HandlerPayload::Response(Err(HandlerError::UnhandledType(
            SettingType::Unknown,
        ))))
    );

    assert_data_tree!(inspector, root: contains {
        test: {
            errors: {
                "00000000000000000000": {
                    timestamp: "0.000000000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                }
            }
        }
    });
}

#[fasync::run_until_stalled(test)]
async fn test_active_listener_inspect() {
    let (env, receptor) = init_listen_env().await;
    let env_handle = Arc::new(Mutex::new(env));
    run_listen(env_handle.clone()).await;

    // Logger handle must be locally scoped so the lock doesn't deadlock with the code under test.
    {
        let env_handle_clone = env_handle.clone();
        let env_lock = env_handle_clone.lock().await;
        let inspector = &env_lock.inspector;
        assert_data_tree!(inspector, root: contains {
            active_listeners: {
                "Unknown": {
                    count: 1u64,
                }
            }
        });
    }

    run_end_listen(env_handle.clone(), receptor).await;

    // Logger handle must be locally scoped so the lock doesn't deadlock with the code under test.
    {
        let env_handle_clone = env_handle.clone();
        let env_lock = env_handle_clone.lock().await;
        let inspector = &env_lock.inspector;
        assert_data_tree!(inspector, root: contains {
            active_listeners: {
                "Unknown": {
                    count: 0u64,
                }
            }
        });
    }
}

#[fasync::run_until_stalled(test)]
async fn inspect_errors_roll_after_limit() {
    // Set the clock so that timestamps will always be 0.
    clock::mock::set(Time::from_nanos(0));
    const SETTING_TYPE: SettingType = SettingType::Unknown;

    let delegate = service::MessageHub::create_hub();
    let (service_client, _) = delegate.create(MessengerType::Unbound).await.unwrap();
    let handler_factory = Arc::new(Mutex::new(ErrorFactory));
    let inspector = Inspector::new();
    let _proxy_handler_signature = SettingProxy::create(
        SETTING_TYPE,
        handler_factory,
        delegate,
        SETTING_PROXY_MAX_ATTEMPTS,
        TEARDOWN_TIMEOUT,
        None,
        false,
        inspector.root().create_child("test"),
        Arc::new(Mutex::new(ListenerInspectLogger::with_inspector(&inspector))),
    )
    .await
    .expect("proxy creation should succeed");

    for i in 0..(MAX_NODE_ERRORS + 1) {
        clock::mock::set(Time::from_nanos(i as i64 * 1000));
        let mut receptor = service_client
            .message(
                HandlerPayload::Request(Request::Get).into(),
                Audience::Address(service::Address::Handler(SETTING_TYPE)),
            )
            .send();

        let (payload, _) = receptor.next_payload().await.expect("should get payload");
        assert_matches::assert_matches!(
            payload,
            Payload::Setting(HandlerPayload::Response(Err(HandlerError::UnhandledType(
                SettingType::Unknown,
            ))))
        );
    }

    assert_data_tree!(inspector, root: contains {
        test: {
            errors: {
                "00000000000000000001": {
                    timestamp: "0.000001000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000002": {
                    timestamp: "0.000002000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000003": {
                    timestamp: "0.000003000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000004": {
                    timestamp: "0.000004000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000005": {
                    timestamp: "0.000005000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000006": {
                    timestamp: "0.000006000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000007": {
                    timestamp: "0.000007000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000008": {
                    timestamp: "0.000008000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000009": {
                    timestamp: "0.000009000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
                "00000000000000000010": {
                    timestamp: "0.000010000",
                    value: "HandlerStartupError(Unknown, \"test error\")"
                },
            }
        }
    });
}

#[test]
fn test_regeneration() {
    let setting_type = SettingType::Unknown;

    let mut executor =
        fasync::TestExecutor::new_with_fake_time().expect("Failed to create executor");

    async fn run_once(setting_type: SettingType) -> (oneshot::Receiver<()>, TestEnvironment) {
        let (done_tx, done_rx) = oneshot::channel();
        let environment =
            TestEnvironmentBuilder::new(setting_type).set_done_tx(Some(done_tx)).build().await;

        // Send initial request.
        assert!(
            get_response(
                environment
                    .service_client
                    .message(
                        HandlerPayload::Request(Request::Get).into(),
                        Audience::Address(service::Address::Handler(setting_type)),
                    )
                    .send()
            )
            .await
            .is_some(),
            "response should have been received"
        );

        // Ensure the handler was only created once.
        assert_eq!(1, environment.handler_factory.lock().await.get_request_count(setting_type));

        // The subsequent teardown should happen here.
        (done_rx, environment)
    }

    let environment_fut = run_once(setting_type);
    futures::pin_mut!(environment_fut);
    let (done_rx, mut environment) =
        if let Poll::Ready(output) = executor.run_until_stalled(&mut environment_fut) {
            output
        } else {
            panic!("initial call stalled");
        };

    let _ = executor.wake_next_timer();

    futures::pin_mut!(done_rx);
    assert_matches::assert_matches!(executor.run_until_stalled(&mut done_rx), Poll::Ready(Ok(_)));

    let mut hit_teardown = false;
    loop {
        let state_fut = environment.setting_handler_rx.next();
        futures::pin_mut!(state_fut);
        let state = if let Poll::Ready(state) = executor.run_until_stalled(&mut state_fut) {
            state
        } else {
            panic!("getting next state stalled");
        };
        match state {
            Some(State::Teardown) => {
                hit_teardown = true;
                break;
            }
            None => break,
            _ => {}
        }
    }
    assert!(hit_teardown, "Handler should have torn down");

    async fn complete(mut environment: TestEnvironment, setting_type: SettingType) {
        drop(environment.setting_handler);

        // Now that the handler is dropped, the setting_handler_tx should be dropped too and the rx
        // end will return none.
        assert!(
            environment.setting_handler_rx.next().await.is_none(),
            "There should be no more states after teardown"
        );

        let (handler_messenger, handler_receptor) =
            environment.handler_factory.lock().await.create(setting_type).await;
        let (state_tx, _) = futures::channel::mpsc::unbounded::<State>();
        let _handler = SettingHandler::create(
            handler_messenger,
            handler_receptor,
            environment.proxy_handler_signature,
            setting_type,
            state_tx,
            None,
        );

        // Send followup request.
        assert!(
            get_response(
                environment
                    .service_client
                    .message(
                        HandlerPayload::Request(Request::Get).into(),
                        Audience::Address(service::Address::Handler(setting_type)),
                    )
                    .send()
            )
            .await
            .is_some(),
            "response should have been received"
        );

        // Check that the handler was re-generated.
        assert_eq!(2, environment.handler_factory.lock().await.get_request_count(setting_type));
    }

    let complete_fut = complete(environment, setting_type);
    futures::pin_mut!(complete_fut);
    assert_eq!(executor.run_until_stalled(&mut complete_fut), Poll::Ready(()));
}

// Exercises the retry flow, ensuring the setting proxy goes through the
// defined number of tests and correctly reports back activity.
#[test]
fn test_retry() {
    let setting_type = SettingType::Unknown;
    async fn run_retries(setting_type: SettingType) -> (TestEnvironment, message::Receptor) {
        let environment = TestEnvironmentBuilder::new(setting_type).build().await;

        let mut event_receptor = service::build_event_listener(&environment.delegate).await;

        // Queue up external failure responses in the handler.
        for _ in 0..SETTING_PROXY_MAX_ATTEMPTS {
            environment.setting_handler.lock().await.queue_action(
                Request::Get,
                HandlerAction::Respond(Err(ControllerError::ExternalFailure(
                    setting_type,
                    "test_component".into(),
                    "connect".into(),
                    "Error".into(),
                ))),
            );
        }

        let request = Request::Get;

        // Send request.
        let handler_result = get_response(
            environment
                .service_client
                .message(
                    HandlerPayload::Request(request.clone()).into(),
                    Audience::Address(service::Address::Handler(setting_type)),
                )
                .send(),
        )
        .await
        .expect("result should be present");

        assert_eq!(
            handler_result,
            Err(HandlerError::ExternalFailure(
                SettingType::Unknown,
                "test_component".into(),
                "connect".into(),
                "Error".into()
            )),
            "error should have been encountered",
        );

        // For each failed attempt, make sure a retry event was broadcasted
        for _ in 0..SETTING_PROXY_MAX_ATTEMPTS {
            verify_handler_event(
                setting_type,
                event_receptor
                    .next_of::<event::Payload>()
                    .await
                    .expect("should be notified of external failure")
                    .0,
                event::handler::Event::Request(event::handler::Action::Execute, request.clone()),
            );
            verify_handler_event(
                setting_type,
                event_receptor
                    .next_of::<event::Payload>()
                    .await
                    .expect("should be notified of external failure")
                    .0,
                event::handler::Event::Request(event::handler::Action::Retry, request.clone()),
            );
        }

        // Ensure that the final event reports that attempts were exceeded
        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of external failure")
                .0,
            event::handler::Event::Request(
                event::handler::Action::AttemptsExceeded,
                request.clone(),
            ),
        );

        (environment, event_receptor)
    }

    async fn run_to_response(
        mut environment: TestEnvironment,
        mut _event_receptor: message::Receptor,
        setting_type: SettingType,
    ) -> (TestEnvironment, message::Receptor) {
        // Regenerate setting handler
        environment.regenerate_handler(None).await;

        // Queue successful response
        environment
            .setting_handler
            .lock()
            .await
            .queue_action(Request::Get, HandlerAction::Respond(Ok(None)));

        let request = Request::Get;
        // Ensure subsequent request succeeds
        assert_matches::assert_matches!(
            get_response(
                environment
                    .service_client
                    .message(
                        HandlerPayload::Request(request.clone()).into(),
                        Audience::Address(service::Address::Handler(setting_type)),
                    )
                    .send(),
            )
            .await,
            Some(Ok(_))
        );

        (environment, _event_receptor)
    }

    let mut executor =
        fasync::TestExecutor::new_with_fake_time().expect("Failed to create executor");

    let environment_fut = run_retries(setting_type);
    futures::pin_mut!(environment_fut);
    let (environment, event_receptor) =
        if let Poll::Ready(output) = executor.run_until_stalled(&mut environment_fut) {
            output
        } else {
            panic!("environment creation and retries stalled");
        };

    let _ = executor.wake_next_timer();

    let environment_fut = run_to_response(environment, event_receptor, setting_type);
    futures::pin_mut!(environment_fut);
    let (_environment, mut event_receptor) =
        if let Poll::Ready(output) = executor.run_until_stalled(&mut environment_fut) {
            output
        } else {
            panic!("running final step stalled");
        };

    let _ = executor.wake_next_timer();

    let event_fut = event_receptor.next_of::<event::Payload>();
    futures::pin_mut!(event_fut);
    let state = if let Poll::Ready(Ok((payload, _))) = executor.run_until_stalled(&mut event_fut) {
        payload
    } else {
        panic!("state retrieval stalled or had no result");
    };

    // Make sure SettingHandler tears down
    verify_handler_event(setting_type, state, event::handler::Event::Teardown);
}

// Ensures early exit triggers retry flow.
#[fuchsia_async::run_until_stalled(test)]
async fn test_early_exit() {
    let exit_result = Ok(());
    let setting_type = SettingType::Unknown;
    let environment = TestEnvironmentBuilder::new(setting_type).build().await;

    let mut event_receptor = service::build_event_listener(&environment.delegate).await;

    // Queue up external failure responses in the handler.
    for _ in 0..SETTING_PROXY_MAX_ATTEMPTS {
        environment
            .setting_handler
            .lock()
            .await
            .queue_action(Request::Get, HandlerAction::Exit(exit_result.clone()));
    }

    let request = Request::Get;

    // Send request.
    let handler_result = get_response(
        environment
            .service_client
            .message(
                HandlerPayload::Request(request.clone()).into(),
                Audience::Address(service::Address::Handler(setting_type)),
            )
            .send(),
    )
    .await
    .expect("result should be present");

    // Make sure the result is an `ControllerError::IrrecoverableError`
    if let Err(error) = handler_result {
        assert_eq!(error, HandlerError::IrrecoverableError);
    } else {
        panic!("error should have been encountered");
    }

    //For each failed attempt, make sure a retry event was broadcasted
    for _ in 0..SETTING_PROXY_MAX_ATTEMPTS {
        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of external failure")
                .0,
            event::handler::Event::Request(event::handler::Action::Execute, request.clone()),
        );
        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of external failure")
                .0,
            event::handler::Event::Exit(exit_result.clone()),
        );
        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of external failure")
                .0,
            event::handler::Event::Request(event::handler::Action::Retry, request.clone()),
        );
    }

    // Ensure that the final event reports that attempts were exceeded
    verify_handler_event(
        setting_type,
        event_receptor
            .next_of::<event::Payload>()
            .await
            .expect("should be notified of external failure")
            .0,
        event::handler::Event::Request(event::handler::Action::AttemptsExceeded, request.clone()),
    );
}

// Ensures timeouts trigger retry flow.
#[test]
fn test_timeout() {
    let mut executor =
        fuchsia_async::TestExecutor::new_with_fake_time().expect("Failed to create executor");

    let fut = async move {
        let setting_type = SettingType::Unknown;
        let environment = TestEnvironmentBuilder::new(setting_type)
            .set_timeout(SETTING_PROXY_TIMEOUT_MS.millis(), true)
            .build()
            .await;

        let mut event_receptor = service::build_event_listener(&environment.delegate).await;

        // Queue up to ignore resquests
        for _ in 0..SETTING_PROXY_MAX_ATTEMPTS {
            environment
                .setting_handler
                .lock()
                .await
                .queue_action(Request::Get, HandlerAction::Ignore);
        }

        let request = Request::Get;

        // Send request.
        let mut receptor = environment
            .delegate
            .create(MessengerType::Unbound)
            .await
            .expect("messenger should be created")
            .0
            .message(
                HandlerPayload::Request(request.clone()).into(),
                Audience::Address(service::Address::Handler(setting_type)),
            )
            .send();

        assert_matches!(
            receptor.next_of::<HandlerPayload>().await.expect("should receive response").0,
            HandlerPayload::Response(Err(HandlerError::TimeoutError))
        );

        // For each failed attempt, make sure a retry event was broadcasted
        for _ in 0..SETTING_PROXY_MAX_ATTEMPTS {
            verify_handler_event(
                setting_type,
                event_receptor
                    .next_of::<event::Payload>()
                    .await
                    .expect("should be notified of execute")
                    .0,
                event::handler::Event::Request(event::handler::Action::Execute, request.clone()),
            );
            verify_handler_event(
                setting_type,
                event_receptor
                    .next_of::<event::Payload>()
                    .await
                    .expect("should be notified of timeout")
                    .0,
                event::handler::Event::Request(event::handler::Action::Timeout, request.clone()),
            );
            verify_handler_event(
                setting_type,
                event_receptor
                    .next_of::<event::Payload>()
                    .await
                    .expect("should be notified of reattempt")
                    .0,
                event::handler::Event::Request(event::handler::Action::Retry, request.clone()),
            );
        }

        // Ensure that the final event reports that attempts were exceeded
        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of exceeded attempts")
                .0,
            event::handler::Event::Request(
                event::handler::Action::AttemptsExceeded,
                request.clone(),
            ),
        );
    };

    pin_utils::pin_mut!(fut);
    loop {
        executor.wake_main_future();
        let new_time = fuchsia_async::Time::from_nanos(
            executor.now().into_nanos()
                + fuchsia_zircon::Duration::from_millis(SETTING_PROXY_TIMEOUT_MS).into_nanos(),
        );
        match executor.run_one_step(&mut fut) {
            Some(Poll::Ready(x)) => break x,
            None => panic!("Executor stalled"),
            Some(Poll::Pending) => {
                executor.set_fake_time(new_time);
            }
        }
    }
}

// Ensures that timeouts cause an error when retry is not enabled for them.
#[test]
fn test_timeout_no_retry() {
    let mut executor =
        fuchsia_async::TestExecutor::new_with_fake_time().expect("Failed to create executor");

    let fut = async move {
        let setting_type = SettingType::Unknown;
        let environment = TestEnvironmentBuilder::new(setting_type)
            .set_timeout(SETTING_PROXY_TIMEOUT_MS.millis(), false)
            .build()
            .await;

        let mut event_receptor = service::build_event_listener(&environment.delegate).await;

        // Queue up to ignore resquests
        environment
            .setting_handler
            .lock()
            .await
            .queue_action(Request::Get, HandlerAction::Respond(Ok(None)));

        let request = Request::Get;

        // Send request.
        let handler_result = get_response(
            environment
                .service_client
                .message(
                    HandlerPayload::Request(request.clone()).into(),
                    Audience::Address(service::Address::Handler(setting_type)),
                )
                .send(),
        )
        .await
        .expect("result should be present");

        // Make sure the result is an `ControllerError::TimeoutError`
        assert!(
            matches!(handler_result, Err(HandlerError::TimeoutError)),
            "error should have been encountered"
        );

        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of execution")
                .0,
            event::handler::Event::Request(event::handler::Action::Execute, request.clone()),
        );
        verify_handler_event(
            setting_type,
            event_receptor
                .next_of::<event::Payload>()
                .await
                .expect("should be notified of timeout")
                .0,
            event::handler::Event::Request(event::handler::Action::Timeout, request),
        );
    };

    pin_utils::pin_mut!(fut);
    loop {
        executor.wake_main_future();
        let new_time = fuchsia_async::Time::from_nanos(
            executor.now().into_nanos()
                + fuchsia_zircon::Duration::from_millis(SETTING_PROXY_TIMEOUT_MS).into_nanos(),
        );
        match executor.run_one_step(&mut fut) {
            Some(Poll::Ready(x)) => break x,
            None => panic!("Executor stalled"),
            Some(Poll::Pending) => {
                executor.set_fake_time(new_time);
            }
        }
    }
}

/// Checks that the supplied message event specifies the supplied handler event.
fn verify_handler_event(
    setting_type: SettingType,
    event_payload: event::Payload,
    event: event::handler::Event,
) {
    if let event::Payload::Event(event::Event::Handler(captured_type, captured_event)) =
        event_payload
    {
        assert_eq!(captured_type, setting_type);
        assert_eq!(event, captured_event);
        return;
    }

    panic!("should have matched the provided event");
}

async fn get_response(mut receptor: service::message::Receptor) -> Option<Response> {
    if let Ok((HandlerPayload::Response(response), _)) = receptor.next_of::<HandlerPayload>().await
    {
        Some(response)
    } else {
        None
    }
}
