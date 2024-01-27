// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(dead_code)]
use {
    crate::state::{PasswordError, State, StateMachine},
    async_trait::async_trait,
    async_utils::hanging_get::server::{HangingGet, Publisher},
    fidl::endpoints::ControlHandle,
    fidl_fuchsia_identity_authentication::{
        Error as ApiError, PasswordInteractionRequest, PasswordInteractionRequestStream,
        PasswordInteractionWatchStateResponder,
    },
    fuchsia_async::Task,
    fuchsia_zircon as zx,
    futures::{channel::mpsc, StreamExt},
    std::{cell::RefCell, rc::Rc},
    tracing::{error, warn},
};

/// A trait for types that can determine whether a supplied password is either
/// valid or potentially valid. A validator either supports enrollment or
/// authentication.
#[async_trait]
pub trait Validator<T: Sized> {
    async fn validate(&self, password: &str) -> Result<T, ValidationError>;
}

#[derive(Debug)]
pub enum ValidationError {
    // An error occurred inside the password authentication itself.
    InternalError(anyhow::Error),
    // An error occurred with a password authentication dependency.
    DependencyError(anyhow::Error),
    // A problem occurred with the supplied password.
    PasswordError(PasswordError),
}

type NotifyFn = Box<dyn Fn(&State, PasswordInteractionWatchStateResponder) -> bool>;

type PasswordInteractionStateHangingGet =
    HangingGet<State, PasswordInteractionWatchStateResponder, NotifyFn>;

type PasswordInteractionStatePublisher =
    Publisher<State, PasswordInteractionWatchStateResponder, NotifyFn>;

// The buffer parameter used when creating the change notification channel. Since we have one
// sender the actual channel capacity is one larger than this number.
const CHANNEL_SIZE: usize = 2;

/// A struct to handle interactive password interaction over a request stream, using the supplied
/// validator to validate passwords.
pub struct PasswordInteractionHandler<T> {
    hanging_get: RefCell<PasswordInteractionStateHangingGet>,
    state_machine: Rc<StateMachine>,
    validator: Box<dyn Validator<T>>,
    publish_task: Task<()>,
    _phantom: std::marker::PhantomData<T>,
}

impl<T> PasswordInteractionHandler<T> {
    pub async fn new(validator: Box<dyn Validator<T>>) -> Self {
        let (sender, mut receiver) = mpsc::channel(CHANNEL_SIZE);
        let state_machine = Rc::new(StateMachine::new(sender));
        let hanging_get = PasswordInteractionHandler::<T>::hanging_get(&state_machine).await;

        // Create an async task that publishes to the hanging get each time the state machine
        // notifies us of an update to the state.
        let publisher = hanging_get.new_publisher();
        let publish_task = Task::local(async move {
            while let Some(state) = receiver.next().await {
                publisher.set(state);
            }
            // We don't expect the state machine to ever close its channel so log if that happens.
            error!("State machine mpsc channel was closed");
        });

        Self {
            hanging_get: RefCell::new(hanging_get),
            state_machine,
            validator,
            publish_task,
            _phantom: std::marker::PhantomData,
        }
    }

    /// Creates a new `PasswordInteractionStateHangingGet` that begins with the current state of the
    /// supplied state machine and notifies that state machine on sending an error state to a FIDL
    /// observer. This relies on the fact that interaction state is local to a single FIDL client
    /// and therefore there cannot be multiple hanging get observers monitoring the same state.
    async fn hanging_get(state_machine: &Rc<StateMachine>) -> PasswordInteractionStateHangingGet {
        let initial_state = state_machine.get().await;
        let state_machine_for_notify = Rc::clone(state_machine);
        let notify_fn: NotifyFn = Box::new(move |state, responder| {
            let sending_error_state = state.is_error();
            if let Err(err) = responder.send(&(*state).into()) {
                warn!("Failed to send password interaction state: {:?}", err);
                return false;
            }
            if sending_error_state {
                // The act of successfully delivering an error to the client is what moves us out of
                // the error state.
                let state_machine_for_task = Rc::clone(&state_machine_for_notify);
                Task::local(async move { state_machine_for_task.leave_error().await }).detach();
            }
            // Clear the dirty bit on the hanging get observer so long as we successfully sent a
            // response.
            true
        });

        PasswordInteractionStateHangingGet::new(initial_state, notify_fn)
    }

    pub async fn handle_password_interaction_request_stream(
        self,
        mut stream: PasswordInteractionRequestStream,
    ) -> Result<T, ApiError> {
        let subscriber = self.hanging_get.borrow_mut().new_subscriber();

        while let Some(request) = stream.next().await {
            match request {
                Ok(PasswordInteractionRequest::SetPassword { password, control_handle }) => {
                    match self.state_machine.get().await {
                        State::WaitingForPassword => {
                            match self.validator.validate(&password).await {
                                Ok(res) => {
                                    // When authentication is successfully completed, close the
                                    // channel and return a response on the parent channel.
                                    control_handle.shutdown_with_epitaph(zx::Status::OK);
                                    return Ok(res);
                                }
                                Err(ValidationError::PasswordError(e)) => {
                                    warn!("Password was not valid: {:?}", e);
                                    self.state_machine.set_error(e).await;
                                }
                                Err(ValidationError::InternalError(e)) => {
                                    warn!("Responded with internal error: {:?}", e);
                                    control_handle.shutdown_with_epitaph(zx::Status::INTERNAL);
                                    return Err(ApiError::Internal);
                                }
                                Err(ValidationError::DependencyError(e)) => {
                                    warn!("Responded with a dependency error: {:?}", e);
                                    control_handle.shutdown_with_epitaph(zx::Status::BAD_STATE);
                                    return Err(ApiError::Resource);
                                }
                            }
                        }
                        state => {
                            warn!("Cannot call set_password while in state: {:?}", state);
                            self.state_machine
                                .set_error(PasswordError::NotWaitingForPassword)
                                .await;
                        }
                    }
                }
                Ok(PasswordInteractionRequest::WatchState { responder }) => {
                    subscriber.register(responder).map_err(|_| ApiError::Resource)?;
                }
                Err(e) => {
                    warn!("Responded with fidl error: {:?}", e);
                    return Err(ApiError::Resource);
                }
            }
        }
        Err(ApiError::Aborted)
    }
}

#[cfg(test)]
mod tests {
    use anyhow::anyhow;

    use {
        super::*,
        assert_matches::assert_matches,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_identity_authentication::{
            self as fauth, PasswordCondition, PasswordInteractionMarker, PasswordInteractionProxy,
            PasswordInteractionWatchStateResponse as WatchStateResponse,
        },
        fuchsia_async::Timer,
        lazy_static::lazy_static,
        std::time::Duration,
    };

    lazy_static! {
        static ref WAITING_FOR_PASSWORD: WatchStateResponse =
            WatchStateResponse::Waiting(vec![PasswordCondition::SetPassword(fauth::Empty)]);
    }

    struct TestValidateSuccess {}

    #[async_trait]
    impl Validator<()> for TestValidateSuccess {
        async fn validate(&self, _password: &str) -> Result<(), ValidationError> {
            Ok(())
        }
    }

    struct TestValidatePasswordError {}

    #[async_trait]
    impl Validator<()> for TestValidatePasswordError {
        async fn validate(&self, _password: &str) -> Result<(), ValidationError> {
            Err(ValidationError::PasswordError(PasswordError::TooShort(10)))
        }
    }

    struct TestValidateInternalError {}

    #[async_trait]
    impl Validator<()> for TestValidateInternalError {
        async fn validate(&self, _password: &str) -> Result<(), ValidationError> {
            Err(ValidationError::InternalError(anyhow!("error error")))
        }
    }

    /// Creates a `PasswordInteractionHandler` using the supplied `Validator`, then creates a
    /// new `PasswordInteraction` channel that the `PasswordInteractionHandler` operates on,
    /// returning a client proxy for this channel to use in testing.
    fn make_proxy<V: Validator<T> + 'static, T: Sized + 'static>(
        validator: V,
    ) -> PasswordInteractionProxy {
        let (proxy, stream) = create_proxy_and_stream::<PasswordInteractionMarker>()
            .expect("Failed to create password interaction proxy");
        let password_interaction_handler_fut = PasswordInteractionHandler::new(Box::new(validator));

        Task::local(async move {
            if password_interaction_handler_fut
                .await
                .handle_password_interaction_request_stream(stream)
                .await
                .is_err()
            {
                warn!("Failed to handle password interaction request stream");
            }
        })
        .detach();
        proxy
    }

    #[fuchsia::test]
    async fn interaction_handler_listener_gets_initial_state() {
        let validate = TestValidateSuccess {};
        let proxy = make_proxy(validate);

        let state = proxy.watch_state().await.expect("Failed to get interaction state");
        assert_eq!(state, *WAITING_FOR_PASSWORD);
    }

    #[fuchsia::test]
    async fn interaction_handler_listener_is_notified_on_state_change() {
        let validate = TestValidateSuccess {};
        let password_proxy = make_proxy(validate);
        // The initial state should be Waiting.
        let state = password_proxy.watch_state().await.expect("Failed to get interaction state");
        assert_eq!(state, *WAITING_FOR_PASSWORD);

        // Send a SetPassword event and verify that channel closes on success.
        let _ = password_proxy.set_password("password").unwrap();
        assert_matches!(
            password_proxy.take_event_stream().next().await.unwrap(),
            Err(fidl::Error::ClientChannelClosed { status: fidl::Status::OK, .. })
        );
    }

    #[fuchsia::test]
    async fn interaction_handler_listener_reports_error() {
        let validate = TestValidatePasswordError {};
        let password_proxy = make_proxy(validate);

        // The initial state should be Waiting.
        let state = password_proxy.watch_state().await.expect("Failed to get interaction state");
        assert_eq!(state, *WAITING_FOR_PASSWORD);

        // Send a SetPassword event and verify that state is Error.
        let _ = password_proxy.set_password("password").unwrap();
        let state = password_proxy.watch_state().await.expect("Failed to get interaction state");
        assert_eq!(state, WatchStateResponse::Error(PasswordError::TooShort(10).into()));
    }

    #[fuchsia::test]
    async fn interaction_handler_listener_reports_error_before_first_call_to_watch_state() {
        let validate = TestValidatePasswordError {};
        let password_proxy = make_proxy(validate);

        // Send a SetPassword event (which the validator will not accept), insert a short delay to
        // allow async processing, then verify a new WatchState returns Error.
        let _ = password_proxy.set_password("password").unwrap();
        Timer::new(Duration::from_millis(10)).await;
        let state = password_proxy.watch_state().await.expect("Failed to get interaction state");
        assert_eq!(state, WatchStateResponse::Error(PasswordError::TooShort(10).into()));

        // Check that the state has been reset to Waiting on following call to WatchState.
        let state = password_proxy.watch_state().await.expect("Failed to get interaction state");
        assert_eq!(state, *WAITING_FOR_PASSWORD);
    }

    #[fuchsia::test]
    async fn interaction_handler_closes_on_internal_error() {
        let validate = TestValidateInternalError {};
        let password_proxy = make_proxy(validate);

        // Send a SetPassword event and verify that an InternalError closes the channel.
        let _ = password_proxy.set_password("password").unwrap();

        assert_matches!(
            password_proxy.take_event_stream().next().await.unwrap(),
            Err(fidl::Error::ClientChannelClosed { status: fidl::Status::INTERNAL, .. })
        );
    }
}
