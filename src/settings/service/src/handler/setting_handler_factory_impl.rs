// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::base::SettingType;
use crate::handler::base::{
    Context, Environment, GenerateHandler, SettingHandlerFactory, SettingHandlerFactoryError,
};
use crate::handler::setting_handler::{Command, Payload, State};
use crate::message::base::{Audience, MessageEvent, MessengerType};
use crate::service;
use crate::service::message::{Delegate, Signature};
use crate::service_context::ServiceContext;
use async_trait::async_trait;
use futures::StreamExt;
use std::collections::HashMap;
use std::collections::HashSet;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

/// SettingHandlerFactoryImpl houses registered closures for generating setting
/// handlers.
pub(crate) struct SettingHandlerFactoryImpl {
    environment: Environment,
    generators: HashMap<SettingType, GenerateHandler>,

    /// Atomic counter used to generate new IDs, which uniquely identify a context.
    context_id_counter: Arc<AtomicU64>,
}

#[async_trait]
impl SettingHandlerFactory for SettingHandlerFactoryImpl {
    async fn generate(
        &mut self,
        setting_type: SettingType,
        delegate: Delegate,
        notifier_signature: Signature,
    ) -> Result<Signature, SettingHandlerFactoryError> {
        if !self.environment.settings.contains(&setting_type) {
            return Err(SettingHandlerFactoryError::SettingNotFound(setting_type));
        }

        let (messenger, receptor) = delegate
            .create(MessengerType::Unbound)
            .await
            .map_err(|_| SettingHandlerFactoryError::HandlerMessengerError)?;
        let signature = receptor.get_signature();

        let generate_function = self
            .generators
            .get(&setting_type)
            .ok_or(SettingHandlerFactoryError::GeneratorNotFound(setting_type))?;

        (generate_function)(Context::new(
            setting_type,
            messenger,
            receptor,
            notifier_signature,
            self.environment.clone(),
            self.context_id_counter.fetch_add(1, Ordering::Relaxed),
        ))
        .await
        .map_err(|e| {
            SettingHandlerFactoryError::HandlerStartupError(
                setting_type,
                format!("Failed to generate setting handler: {e:?}").into(),
            )
        })?;

        let (controller_messenger, _) = delegate
            .create(MessengerType::Unbound)
            .await
            .map_err(|_| SettingHandlerFactoryError::ControllerMessengerError)?;

        // At this point, we know the controller was constructed successfully.
        // Tell the controller to run the Startup phase to initialize its state.
        let mut controller_receptor = controller_messenger.message(
            Payload::Command(Command::ChangeState(State::Startup)).into(),
            Audience::Messenger(signature),
        );

        // Wait for the startup phase to be over before continuing.
        while let Some(message_event) = controller_receptor.next().await {
            match message_event {
                MessageEvent::Status(_) => {} // no-op
                MessageEvent::Message(service::Payload::Controller(Payload::Result(result)), _) => {
                    // Startup phase is complete. If it had no errors the proxy can assume it
                    // has an active controller with create() and startup() already run on it
                    // before handling its request.
                    return result.map(|_| signature).map_err(|e| {
                        SettingHandlerFactoryError::HandlerStartupError(
                            setting_type,
                            format!("Got bad startup response: {e:?}").into(),
                        )
                    });
                }
                e => {
                    return Err(SettingHandlerFactoryError::HandlerStartupError(
                        setting_type,
                        format!("Unexpected startup response: {e:?}").into(),
                    ));
                }
            }
        }

        panic!("Did not get any responses from {setting_type:?} controller startup");
    }
}

impl SettingHandlerFactoryImpl {
    pub(crate) fn new(
        settings: HashSet<SettingType>,
        service_context: Arc<ServiceContext>,
        context_id_counter: Arc<AtomicU64>,
    ) -> SettingHandlerFactoryImpl {
        SettingHandlerFactoryImpl {
            environment: Environment::new(settings, service_context),
            generators: HashMap::new(),
            context_id_counter,
        }
    }

    pub(crate) fn register(
        &mut self,
        setting_type: SettingType,
        generate_function: GenerateHandler,
    ) {
        // Ignores any previously registered generator.
        let _ = self.generators.insert(setting_type, generate_function);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::handler::base::Request;
    use crate::handler::setting_handler::controller::{Create, Handle};
    use crate::handler::setting_handler::{
        BoxedController, ClientImpl, ControllerError, ControllerStateResult, SettingHandlerResult,
    };
    use crate::service;
    use crate::service_context::ServiceContext;
    use fuchsia_async as fasync;
    use futures::future::FutureExt;
    use futures::select;

    /// Test controller used to test startup waiting behavior in SettingHandlerFactoryImpl.
    struct TestController {
        client: Arc<ClientImpl>,
    }

    #[async_trait]
    impl Create for TestController {
        async fn create(client: Arc<ClientImpl>) -> Result<Self, ControllerError> {
            Ok(Self { client })
        }
    }

    #[async_trait]
    impl Handle for TestController {
        // Not relevant.
        async fn handle(&self, _request: Request) -> Option<SettingHandlerResult> {
            None
        }

        // When we see a startup message, send a signal to signify we're done.
        async fn change_state(&mut self, state: State) -> Option<ControllerStateResult> {
            let _ = self.client.emit_state_event(state);
            match state {
                State::Startup => Some(Ok(())),
                _ => None,
            }
        }
    }

    #[fasync::run_until_stalled(test)]
    async fn ensure_startup_is_awaited() {
        let delegate = service::MessageHub::create_hub();
        let mut factory_impl = SettingHandlerFactoryImpl::new(
            [SettingType::Unknown].into(),
            Arc::new(ServiceContext::new(None, Some(delegate.clone()))),
            Arc::new(AtomicU64::new(0)),
        );

        // Register generation of controller with factory_impl.
        let generate_handler: GenerateHandler = Box::new(move |context| {
            Box::pin(ClientImpl::create(
                context,
                Box::new(move |proxy| {
                    Box::pin(async move {
                        Ok(Box::new(TestController::create(proxy).await.unwrap())
                            as BoxedController)
                    })
                }),
            ))
        });
        factory_impl.register(SettingType::Unknown, generate_handler);

        // Create a broker that only listens to replies.
        let (_, broker_receptor) =
            delegate.create_sink().await.expect("could not create event sink receptor");

        let (_, receptor) =
            delegate.create(MessengerType::Unbound).await.expect("messenger should be created");
        // Generate the controller, but don't await it yet so we can time it with the response
        // from the broker.
        let mut generate_future = factory_impl
            .generate(SettingType::Unknown, delegate, receptor.get_signature())
            .into_stream()
            .fuse();
        let mut broker_receptor = broker_receptor.fuse();

        // We need to validate that the generate_done always completes after the startup_done
        // signal. If this ever fails, it implies that the generator does not properly wait for
        // startup to complete.
        let mut generate_done = None;
        let mut startup_done = None;
        let mut idx: u8 = 0;
        while generate_done.is_none() || startup_done.is_none() {
            select! {
                maybe = broker_receptor.next() => {
                    let _ = maybe.expect("should have gotten a reply");
                    startup_done = Some(idx);
                    idx += 1;
                }
                result = generate_future.select_next_some() => {
                    let _ = result.expect("should have received a signature");
                    generate_done = Some(idx);
                    idx += 1;
                }

                complete => break,
            }
        }

        // Validate that the id for generate is larger than the one for startup,
        // implying that generate finished after startup was done.
        assert!(generate_done.unwrap() > startup_done.unwrap());
    }
}
