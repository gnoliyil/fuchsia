// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    events::{
        error::EventError,
        router::{Dispatcher, EventProducer},
        types::*,
    },
    identity::ComponentIdentity,
};
use fidl_fuchsia_io as fio;
use fidl_fuchsia_sys_internal::{
    ComponentEventListenerMarker, ComponentEventListenerRequest, ComponentEventProviderProxy,
    SourceIdentity,
};
use fuchsia_zircon as zx;
use futures::StreamExt;
use std::convert::TryFrom;
use tracing::{debug, warn};

pub struct ComponentEventProvider {
    proxy: ComponentEventProviderProxy,
    dispatcher: Dispatcher,
}

macro_rules! break_on_disconnect {
    ($result:expr) => {{
        match $result {
            Err(EventError::SendError(err)) => {
                if err.is_disconnected() {
                    break;
                }
            }
            Err(err) => {
                warn!(?err, "Error handling event");
            }
            Ok(_) => {}
        }
    }};
}

impl ComponentEventProvider {
    pub fn new(proxy: ComponentEventProviderProxy) -> Self {
        Self { proxy, dispatcher: Dispatcher::default() }
    }

    pub async fn spawn(mut self) -> Result<(), EventError> {
        let (events_client_end, mut stream) =
            fidl::endpoints::create_request_stream::<ComponentEventListenerMarker>()?;
        self.proxy.set_listener(events_client_end)?;
        while let Some(request) = stream.next().await {
            match request {
                Ok(ComponentEventListenerRequest::OnDiagnosticsDirReady {
                    component,
                    directory,
                    ..
                }) => {
                    break_on_disconnect!(
                        self.handle_on_directory_ready(component, directory).await
                    );
                }
                other => {
                    debug!(?other, "unexpected component event listener request");
                }
            }
        }
        Ok(())
    }

    async fn handle_on_directory_ready(
        &mut self,
        component: SourceIdentity,
        directory: fidl::endpoints::ClientEnd<fio::DirectoryMarker>,
    ) -> Result<(), EventError> {
        let component = ComponentIdentity::try_from(component)?;
        if let Ok(directory) = directory.into_proxy() {
            self.dispatcher.emit(Event {
                timestamp: zx::Time::get_monotonic(),
                payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                    component,
                    directory: Some(directory),
                }),
            })?;
        }
        Ok(())
    }
}

impl EventProducer for ComponentEventProvider {
    fn set_dispatcher(&mut self, dispatcher: Dispatcher) {
        self.dispatcher = dispatcher;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_sys_internal::{
        ComponentEventProviderMarker, ComponentEventProviderRequest, SourceIdentity,
    };
    use fuchsia_async as fasync;
    use futures::{channel::oneshot, StreamExt};
    use lazy_static::lazy_static;
    use std::collections::BTreeSet;

    lazy_static! {
        static ref MOCK_URL: String = "NO-OP URL".to_string();
    }

    #[derive(Clone)]
    struct ClonableSourceIdentity {
        realm_path: Vec<String>,
        component_name: String,
        instance_id: String,
    }

    impl From<ClonableSourceIdentity> for SourceIdentity {
        fn from(other: ClonableSourceIdentity) -> Self {
            SourceIdentity {
                realm_path: Some(other.realm_path),
                component_url: Some(MOCK_URL.clone()),
                component_name: Some(other.component_name),
                instance_id: Some(other.instance_id),
                ..SourceIdentity::EMPTY
            }
        }
    }

    impl From<ClonableSourceIdentity> for ComponentIdentity {
        fn from(other: ClonableSourceIdentity) -> Self {
            let mut moniker = other.realm_path;
            moniker.push(other.component_name);
            ComponentIdentity::from_identifier_and_url(
                ComponentIdentifier::Legacy {
                    moniker: moniker.into(),
                    instance_id: other.instance_id,
                },
                &*MOCK_URL,
            )
        }
    }

    #[fuchsia::test]
    async fn component_event_stream() {
        let (mut provider, listener_receiver) = spawn_fake_component_event_provider();
        let events = BTreeSet::from([EventType::DiagnosticsReady]);

        let (mut event_stream, dispatcher) = Dispatcher::new_for_test(events);
        provider.set_dispatcher(dispatcher);

        let _task = fasync::Task::spawn(async move { provider.spawn().await });

        let listener = listener_receiver
            .await
            .expect("failed to receive listener")
            .into_proxy()
            .expect("failed to get listener proxy");

        let identity: ClonableSourceIdentity = ClonableSourceIdentity {
            realm_path: vec!["root".to_string(), "a".to_string()],
            component_name: "test.cmx".to_string(),
            instance_id: "12345".to_string(),
        };
        let (dir, _) = fidl::endpoints::create_request_stream::<fio::DirectoryMarker>().unwrap();
        listener
            .on_diagnostics_dir_ready(identity.clone().into(), dir)
            .expect("failed to send event 2");

        let event = event_stream.next().await.unwrap();
        match event.payload {
            EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                component: observed_identity,
                directory: Some(_),
            }) => {
                assert_eq!(
                    observed_identity.relative_moniker.to_string(),
                    format!("{}/{}", identity.realm_path.join("/"), &identity.component_name)
                );
            }
            payload => unreachable!("never gets {:?}", payload),
        }
    }

    fn spawn_fake_component_event_provider() -> (
        ComponentEventProvider,
        oneshot::Receiver<fidl::endpoints::ClientEnd<ComponentEventListenerMarker>>,
    ) {
        let (provider, mut request_stream) =
            fidl::endpoints::create_proxy_and_stream::<ComponentEventProviderMarker>().unwrap();
        let (sender, receiver) = oneshot::channel();
        fasync::Task::local(async move {
            if let Some(Ok(request)) = request_stream.next().await {
                match request {
                    ComponentEventProviderRequest::SetListener { listener, .. } => {
                        sender.send(listener).expect("failed to send listener");
                    }
                }
            }
        })
        .detach();
        (ComponentEventProvider::new(provider), receiver)
    }
}
