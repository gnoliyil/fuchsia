// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource},
        model::{
            error::{CapabilityProviderError, ModelError},
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
        },
    },
    ::routing::capability_source::InternalCapability,
    async_trait::async_trait,
    cm_types::Name,
    cm_util::{channel, TaskGroup},
    fidl::endpoints::{DiscoverableProtocolMarker, ServerEnd},
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    futures::{
        channel::mpsc::{unbounded, UnboundedSender},
        prelude::*,
    },
    lazy_static::lazy_static,
    moniker::Moniker,
    namespace::{self, NamespaceError},
    sandbox::AnyCapability,
    serve_processargs::{BuildNamespaceError, NamespaceBuilder},
    std::{
        path::PathBuf,
        sync::{Arc, Weak},
    },
    tracing::warn,
};

lazy_static! {
    pub static ref CAPABILITY_NAME: Name =
        fcomponent::NamespaceMarker::PROTOCOL_NAME.parse().unwrap();
}

pub struct NamespaceCapabilityProvider {
    host: Arc<NamespaceCapabilityHost>,
}

impl NamespaceCapabilityProvider {
    pub fn new(host: Arc<NamespaceCapabilityHost>) -> Self {
        Self { host }
    }
}

#[async_trait]
impl CapabilityProvider for NamespaceCapabilityProvider {
    async fn open(
        self: Box<Self>,
        task_group: TaskGroup,
        _flags: fio::OpenFlags,
        _relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), CapabilityProviderError> {
        let server_end = channel::take_channel(server_end);
        let host = self.host.clone();
        let server_end = ServerEnd::<fcomponent::NamespaceMarker>::new(server_end);
        let stream: fcomponent::NamespaceRequestStream =
            server_end.into_stream().map_err(|_| CapabilityProviderError::StreamCreationError)?;
        task_group.spawn(async move {
            // We only need to look up the component matching this scope.
            // These operations should all work, even if the component is not running.
            let serve_result = host.serve(stream).await;
            if let Err(error) = serve_result {
                // TODO: Set an epitaph to indicate this was an unexpected error.
                warn!(%error, "serve failed");
            }
        });
        Ok(())
    }
}

pub struct NamespaceCapabilityHost {
    namespace_tasks: TaskGroup,
}

impl NamespaceCapabilityHost {
    pub fn new() -> Self {
        Self { namespace_tasks: TaskGroup::new() }
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "NamespaceCapabilityHost",
            vec![EventType::CapabilityRouted],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    pub async fn serve(
        &self,
        mut stream: fcomponent::NamespaceRequestStream,
    ) -> Result<(), fidl::Error> {
        while let Some(request) = stream.try_next().await? {
            let method_name = request.method_name();
            let result = self.handle_request(request).await;
            match result {
                // If the error was PEER_CLOSED then we don't need to log it as a client can
                // disconnect while we are processing its request.
                Err(error) if !error.is_closed() => {
                    warn!(%method_name, %error, "Couldn't send Namespace response");
                }
                _ => {}
            }
        }
        Ok(())
    }

    async fn handle_request(
        &self,
        request: fcomponent::NamespaceRequest,
    ) -> Result<(), fidl::Error> {
        match request {
            fcomponent::NamespaceRequest::CreateFromDicts { entries, responder } => {
                let res = self.create_from_dicts(entries).await;
                responder.send(res)?;
            }
            fcomponent::NamespaceRequest::_UnknownMethod { ordinal, .. } => {
                warn!(%ordinal, "fuchsia.component/Namespace received unknown method");
            }
        }
        Ok(())
    }

    async fn create_from_dicts(
        &self,
        entries: Vec<fcomponent::NamespaceDictPair>,
    ) -> Result<Vec<fcomponent::NamespaceEntry>, fcomponent::NamespaceError> {
        let mut namespace_builder = NamespaceBuilder::new(Self::ignore_not_found());
        for entry in entries {
            let path = entry.path;
            let dict = entry.dict.into_proxy().unwrap();
            let items: Vec<_> = dict
                .read()
                .await
                .map_err(|_| fcomponent::NamespaceError::DictRead)?
                .map_err(|_| fcomponent::NamespaceError::DictRead)?;
            for item in items {
                let capability: AnyCapability = item.value.try_into().unwrap();
                let path = namespace::Path::new(format!("{}/{}", path, item.key))
                    .map_err(|_| fcomponent::NamespaceError::BadEntry)?;
                namespace_builder.add_object(capability, &path).map_err(Self::error_to_fidl)?;
            }
        }
        let (namespace, fut) = namespace_builder.serve().map_err(Self::error_to_fidl)?;
        self.namespace_tasks.spawn(fut);
        let out = namespace.flatten().into_iter().map(Into::into).collect();
        Ok(out)
    }

    fn error_to_fidl(e: BuildNamespaceError) -> fcomponent::NamespaceError {
        match e {
            BuildNamespaceError::NamespaceError(e) => match e {
                NamespaceError::Shadow(_) => fcomponent::NamespaceError::Shadow,
                NamespaceError::Duplicate(_) => fcomponent::NamespaceError::Duplicate,
                NamespaceError::EntryError(_) => fcomponent::NamespaceError::BadEntry,
            },
            BuildNamespaceError::Conversion { .. } => fcomponent::NamespaceError::Conversion,
        }
    }

    fn ignore_not_found() -> UnboundedSender<String> {
        let (sender, _receiver) = unbounded();
        sender
    }

    async fn on_framework_capability_routed<'a>(
        self: Arc<Self>,
        _scope_moniker: Moniker,
        capability: &'a InternalCapability,
        capability_provider: Option<Box<dyn CapabilityProvider>>,
    ) -> Result<Option<Box<dyn CapabilityProvider>>, ModelError> {
        // If some other capability has already been installed, then there's nothing to
        // do here.
        if capability_provider.is_none() && capability.matches_protocol(&CAPABILITY_NAME) {
            return Ok(Some(Box::new(NamespaceCapabilityProvider::new(self.clone()))
                as Box<dyn CapabilityProvider>));
        }

        Ok(capability_provider)
    }
}

#[async_trait]
impl Hook for NamespaceCapabilityHost {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        if let EventPayload::CapabilityRouted {
            source: CapabilitySource::Framework { capability, component },
            capability_provider,
        } = &event.payload
        {
            let mut capability_provider = capability_provider.lock().await;
            *capability_provider = self
                .on_framework_capability_routed(
                    component.moniker.clone(),
                    &capability,
                    capability_provider.take(),
                )
                .await?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        assert_matches::assert_matches,
        fidl::endpoints::{self, ProtocolMarker, Proxy, ServerEnd},
        fidl_fidl_examples_routing_echo as fecho, fidl_fuchsia_component_sandbox as fsandbox,
        fuchsia_async as fasync,
        fuchsia_component::client,
        sandbox::{Capability, Dict, Handle, Receiver},
    };

    async fn handle_receiver<T, Fut, F>(
        mut receiver_stream: fsandbox::ReceiverRequestStream,
        request_stream_handler: F,
    ) where
        T: endpoints::ProtocolMarker,
        Fut: Future<Output = ()> + Send,
        F: Fn(T::RequestStream) -> Fut + Send + Sync + Copy + 'static,
    {
        let mut task_group = fasync::TaskGroup::new();
        while let Some(request) = receiver_stream.try_next().await.unwrap() {
            match request {
                fsandbox::ReceiverRequest::Receive { capability, control_handle: _ } => {
                    task_group.spawn(async move {
                        let server_end = ServerEnd::<T>::new(fidl::Channel::from(capability));
                        let stream: T::RequestStream = server_end.into_stream().unwrap();
                        request_stream_handler(stream).await;
                    });
                }
            }
        }
    }

    async fn handle_echo_request_stream(response: &str, mut stream: fecho::EchoRequestStream) {
        while let Ok(Some(request)) = stream.try_next().await {
            match request {
                fecho::EchoRequest::EchoString { value: _, responder } => {
                    responder.send(Some(response)).unwrap();
                }
            }
        }
    }

    #[fuchsia::test]
    async fn namespace_create_from_dicts() {
        let mut tasks = fasync::TaskGroup::new();

        let host = NamespaceCapabilityHost::new();
        let (namespace_proxy, stream) =
            endpoints::create_proxy_and_stream::<fcomponent::NamespaceMarker>().unwrap();
        tasks.add(fasync::Task::spawn(async move {
            host.serve(stream).await.unwrap();
        }));

        let mut namespace_pairs = vec![];
        for (path, response) in [("/svc", "first"), ("/zzz/svc", "second")] {
            // Initialize the host and sender/receiver pair.
            let receiver = Receiver::<Handle>::new();
            let sender = receiver.new_sender();

            // Serve an Echo request handler on the Receiver.
            let (handle, receiver_fut) = receiver.to_zx_handle();
            let receiver_stream =
                ServerEnd::<fsandbox::ReceiverMarker>::new(fidl::Channel::from(handle));
            let receiver_stream = receiver_stream.into_stream().unwrap();
            tasks.add(fasync::Task::spawn(async move {
                let t1 = receiver_fut.unwrap();
                let t2 = handle_receiver::<fecho::EchoMarker, _, _>(receiver_stream, |stream| {
                    handle_echo_request_stream(response, stream)
                });
                futures::join!(t1, t2);
            }));

            // Create a dictionary and add the Sender to it.
            let mut dict = Dict::new();
            let (dict_proxy, stream) =
                endpoints::create_proxy_and_stream::<fsandbox::DictMarker>().unwrap();
            tasks.add(fasync::Task::spawn(async move {
                dict.serve_dict(stream).await.unwrap();
            }));
            let (handle, _) = sender.to_zx_handle();
            dict_proxy.insert(fecho::EchoMarker::DEBUG_NAME, handle).await.unwrap().unwrap();
            namespace_pairs.push(fcomponent::NamespaceDictPair {
                path: path.into(),
                dict: dict_proxy.into_channel().unwrap().into_zx_channel().into(),
            })
        }

        // Convert the dictionaries to a namespace.
        let mut namespace_entries =
            namespace_proxy.create_from_dicts(namespace_pairs).await.unwrap().unwrap();

        // Confirm that the Sender in the dictionary was converted to a service node, and we
        // can access the Echo protocol (served by the Receiver) through this node.
        let entry = namespace_entries.remove(0);
        assert_matches!(entry.path, Some(p) if p == "/svc");
        let dir = entry.directory.unwrap().into_proxy().unwrap();
        let echo = client::connect_to_protocol_at_dir_root::<fecho::EchoMarker>(&dir).unwrap();
        let response = echo.echo_string(None).await.unwrap();
        assert_matches!(response, Some(m) if m == "first");

        let entry = namespace_entries.remove(0);
        assert!(namespace_entries.is_empty());
        assert_matches!(entry.path, Some(p) if p == "/zzz/svc");
        let dir = entry.directory.unwrap().into_proxy().unwrap();
        let echo = client::connect_to_protocol_at_dir_root::<fecho::EchoMarker>(&dir).unwrap();
        let response = echo.echo_string(None).await.unwrap();
        assert_matches!(response, Some(m) if m == "second");
    }

    #[fuchsia::test]
    async fn namespace_create_from_dicts_err_shadow() {
        let mut tasks = fasync::TaskGroup::new();

        let host = NamespaceCapabilityHost::new();
        let (namespace_proxy, stream) =
            endpoints::create_proxy_and_stream::<fcomponent::NamespaceMarker>().unwrap();
        tasks.add(fasync::Task::spawn(async move {
            host.serve(stream).await.unwrap();
        }));

        // Two entries with a shadowing path.
        let mut namespace_pairs = vec![];
        for path in ["/svc", "/svc/shadow"] {
            // Initialize the host and sender/receiver pair.
            let receiver = Receiver::<Handle>::new();
            let sender = receiver.new_sender();

            // Serve an Echo request handler on the Receiver.
            let (handle, receiver_fut) = receiver.to_zx_handle();
            let receiver_stream =
                ServerEnd::<fsandbox::ReceiverMarker>::new(fidl::Channel::from(handle));
            let receiver_stream = receiver_stream.into_stream().unwrap();
            tasks.add(fasync::Task::spawn(async move {
                let t1 = receiver_fut.unwrap();
                let t2 = handle_receiver::<fecho::EchoMarker, _, _>(receiver_stream, |stream| {
                    handle_echo_request_stream("hello", stream)
                });
                futures::join!(t1, t2);
            }));

            // Create a dictionary and add the Sender to it.
            let mut dict = Dict::new();
            let (dict_proxy, stream) =
                endpoints::create_proxy_and_stream::<fsandbox::DictMarker>().unwrap();
            tasks.add(fasync::Task::spawn(async move {
                dict.serve_dict(stream).await.unwrap();
            }));
            let (handle, _) = sender.to_zx_handle();
            dict_proxy.insert(fecho::EchoMarker::DEBUG_NAME, handle).await.unwrap().unwrap();
            namespace_pairs.push(fcomponent::NamespaceDictPair {
                path: path.into(),
                dict: dict_proxy.into_channel().unwrap().into_zx_channel().into(),
            })
        }

        // Try to convert the dictionaries to a namespace. Expect an error because one path
        // shadows another.
        let res = namespace_proxy.create_from_dicts(namespace_pairs).await.unwrap();
        assert_matches!(res, Err(fcomponent::NamespaceError::Shadow));
    }

    #[fuchsia::test]
    async fn namespace_create_from_dicts_err_dict_read() {
        let mut tasks = fasync::TaskGroup::new();

        let host = NamespaceCapabilityHost::new();
        let (namespace_proxy, stream) =
            endpoints::create_proxy_and_stream::<fcomponent::NamespaceMarker>().unwrap();
        tasks.add(fasync::Task::spawn(async move {
            host.serve(stream).await.unwrap();
        }));

        // Create a dictionary and close the server end.
        let (dict_proxy, stream) =
            endpoints::create_proxy_and_stream::<fsandbox::DictMarker>().unwrap();
        drop(stream);
        let namespace_pairs = vec![fcomponent::NamespaceDictPair {
            path: "/svc".into(),
            dict: dict_proxy.into_channel().unwrap().into_zx_channel().into(),
        }];

        // Try to convert the dictionaries to a namespace. Expect an error because the dictionary
        // was unreadable.
        let res = namespace_proxy.create_from_dicts(namespace_pairs).await.unwrap();
        assert_matches!(res, Err(fcomponent::NamespaceError::DictRead));
    }
}
