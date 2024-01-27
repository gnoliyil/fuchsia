// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        component::{ComponentInstance, InstanceState},
        error::ModelError,
        events::synthesizer::{EventSynthesisProvider, ExtendedComponent},
        hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
        model::Model,
    },
    ::routing::{event::EventFilter, rights::Rights},
    async_trait::async_trait,
    cm_rust::{
        CapabilityName, CapabilityPath, ComponentDecl, ExposeDecl, ExposeDirectoryDecl,
        ExposeSource, ExposeTarget,
    },
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio, fuchsia_async as fasync, fuchsia_fs, fuchsia_zircon as zx,
    futures::stream::StreamExt,
    moniker::AbsoluteMoniker,
    std::{
        sync::{Arc, Mutex, Weak},
        time::Duration,
    },
};

// TODO(fxbug.dev/49198): The out/diagnostics directory propagation for runners includes a retry.
// The reason of this is that flutter fills the out/ directory *after*
// serving it. Therefore we need to watch that directory to notify.
// Sadly the PseudoDir exposed in the SDK (and used by flutter) returns ZX_ERR_NOT_SUPPORTED on
// Watch.
const OPEN_OUT_SUBDIR_RETRY_INITIAL_DELAY_MS: u64 = 500;
const OPEN_OUT_SUBDIR_RETRY_MAX_DELAY_MS: u64 = 15000;
const OPEN_OUT_SUBDIR_MAX_RETRIES: usize = 30;

/// Awaits for `Started` events and for each capability exposed to framework, dispatches a
/// `DirectoryReady` event.
pub struct DirectoryReadyNotifier {
    model: Weak<Model>,
    /// Capabilities offered by component manager that we wish to provide through `DirectoryReady`
    /// events. For example, the diagnostics directory hosting inspect data.
    builtin_capabilities: Mutex<Vec<(String, fio::NodeProxy)>>,
}

impl DirectoryReadyNotifier {
    pub fn new(model: Weak<Model>) -> Self {
        Self { model, builtin_capabilities: Mutex::new(Vec::new()) }
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "DirectoryReadyNotifier",
            vec![EventType::Started],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    pub fn register_component_manager_capability(
        &self,
        name: impl Into<String>,
        node: fio::NodeProxy,
    ) {
        if let Ok(mut guard) = self.builtin_capabilities.lock() {
            guard.push((name.into(), node));
        }
    }

    async fn on_component_started(
        self: &Arc<Self>,
        target_moniker: &AbsoluteMoniker,
        outgoing_dir: &fio::DirectoryProxy,
        decl: ComponentDecl,
    ) -> Result<(), ModelError> {
        // Don't block the handling on the event on the exposed capabilities being ready
        let this = self.clone();
        let target_moniker = target_moniker.clone();
        let outgoing_dir = Clone::clone(outgoing_dir);
        fasync::Task::spawn(async move {
            // If we can't find the component then we can't dispatch any DirectoryReady event,
            // error or otherwise. This isn't necessarily an error as the model or component might've been
            // destroyed in the intervening time, so we just exit early.
            let target = match this.model.upgrade() {
                Some(model) => {
                    if let Ok(component) = model.look_up(&target_moniker).await {
                        component
                    } else {
                        return;
                    }
                }
                None => return,
            };

            let matching_exposes = filter_matching_exposes(&decl, None);
            this.dispatch_capabilities_ready(outgoing_dir, &decl, matching_exposes, &target).await;
        })
        .detach();
        Ok(())
    }

    /// Waits for the outgoing directory to be ready and then notifies hooks of all the capabilities
    /// inside it that were exposed to the framework by the component.
    async fn dispatch_capabilities_ready(
        &self,
        outgoing_dir: fio::DirectoryProxy,
        decl: &ComponentDecl,
        matching_exposes: Vec<&ExposeDecl>,
        target: &Arc<ComponentInstance>,
    ) {
        let directory_ready_events =
            self.create_events(Some(outgoing_dir), decl, matching_exposes, target).await;
        for directory_ready_event in directory_ready_events {
            target.hooks.dispatch(&directory_ready_event).await;
        }
    }

    async fn create_events(
        &self,
        outgoing_dir: Option<fio::DirectoryProxy>,
        decl: &ComponentDecl,
        matching_exposes: Vec<&ExposeDecl>,
        target: &Arc<ComponentInstance>,
    ) -> Vec<Event> {
        // Forward along the result for opening the outgoing directory into the DirectoryReady
        // dispatch in order to propagate any potential errors as an event.
        let outgoing_dir_result = async move {
            let outgoing_dir = outgoing_dir?;
            let outgoing_dir = fuchsia_fs::directory::clone_no_describe(
                &outgoing_dir,
                Some(fio::OpenFlags::CLONE_SAME_RIGHTS | fio::OpenFlags::DESCRIBE),
            )
            .ok()?;
            let mut events = outgoing_dir.take_event_stream();
            let () = match events.next().await {
                Some(Ok(fio::DirectoryEvent::OnOpen_ { s: status, info: _ })) => {
                    zx::Status::ok(status).ok()
                }
                Some(Ok(fio::DirectoryEvent::OnRepresentation { .. })) => Some(()),
                _ => None,
            }?;
            Some(outgoing_dir)
        }
        .await
        .ok_or_else(|| ModelError::open_directory_error(target.abs_moniker.clone(), "/"));

        let mut events = Vec::new();
        for expose_decl in matching_exposes {
            let event = match expose_decl {
                ExposeDecl::Directory(ExposeDirectoryDecl { source_name, target_name, .. }) => {
                    let (source_path, rights) = {
                        if let Some(directory_decl) = decl.find_directory_source(source_name) {
                            (
                                directory_decl
                                    .source_path
                                    .as_ref()
                                    .expect("missing directory source path"),
                                directory_decl.rights,
                            )
                        } else {
                            panic!("Missing directory declaration for expose: {:?}", decl);
                        }
                    };
                    self.create_event(
                        &target,
                        outgoing_dir_result.as_ref(),
                        Rights::from(rights),
                        source_path,
                        target_name,
                    )
                    .await
                }
                _ => {
                    unreachable!("should have skipped above");
                }
            };
            if let Some(event) = event {
                events.push(event);
            }
        }

        events
    }

    /// Creates an event with the directory at the given `target_path` inside the provided
    /// outgoing directory if the capability is available.
    async fn create_event(
        &self,
        target: &Arc<ComponentInstance>,
        outgoing_dir_result: Result<&fio::DirectoryProxy, &ModelError>,
        rights: Rights,
        source_path: &CapabilityPath,
        target_name: &CapabilityName,
    ) -> Option<Event> {
        let target_name = target_name.to_string();

        let node_result = async move {
            // DirProxy.open fails on absolute paths.
            let source_path = source_path.to_string();

            let outgoing_dir = outgoing_dir_result.map_err(|e| e.clone())?;

            let mut current_delay = 0;
            let mut retries = 0;
            loop {
                match self.try_opening(&outgoing_dir, &source_path, &rights).await {
                    Ok(node) => return Ok(node),
                    Err(TryOpenError::Fidl(_)) => {
                        break Err(ModelError::open_directory_error(
                            target.abs_moniker.clone(),
                            source_path.clone(),
                        ));
                    }
                    Err(TryOpenError::Status(status)) => {
                        // If the directory doesn't exist, retry.
                        if status == zx::Status::NOT_FOUND {
                            if retries < OPEN_OUT_SUBDIR_MAX_RETRIES {
                                retries += 1;
                                current_delay = std::cmp::min(
                                    OPEN_OUT_SUBDIR_RETRY_MAX_DELAY_MS,
                                    current_delay + OPEN_OUT_SUBDIR_RETRY_INITIAL_DELAY_MS,
                                );
                                fasync::Timer::new(Duration::from_millis(current_delay)).await;
                                continue;
                            }
                        }
                        break Err(ModelError::open_directory_error(
                            target.abs_moniker.clone(),
                            source_path.clone(),
                        ));
                    }
                }
            }
        }
        .await;

        match node_result {
            Ok(node) => {
                Some(Event::new(&target, EventPayload::DirectoryReady { name: target_name, node }))
            }
            Err(_) => None,
        }
    }

    async fn try_opening(
        &self,
        outgoing_dir: &fio::DirectoryProxy,
        source_path: &str,
        rights: &Rights,
    ) -> Result<fio::NodeProxy, TryOpenError> {
        let canonicalized_path = fuchsia_fs::canonicalize_path(&source_path);
        let (node, server_end) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
        outgoing_dir
            .open(
                // TODO(fxbug.dev/118292): we might be able to remove READABLE from here, but at the
                // moment driver_manager fails to expose inspect if we remove it.
                rights.into_legacy()
                    | fio::OpenFlags::DESCRIBE
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::DIRECTORY,
                fio::ModeType::empty(),
                &canonicalized_path,
                ServerEnd::new(server_end.into_channel()),
            )
            .map_err(TryOpenError::Fidl)?;
        let mut events = node.take_event_stream();
        match events.next().await {
            Some(Ok(fio::NodeEvent::OnOpen_ { s: status, .. })) => {
                let zx_status = zx::Status::from_raw(status);
                if zx_status != zx::Status::OK {
                    return Err(TryOpenError::Status(zx_status));
                }
            }
            Some(Ok(fio::NodeEvent::OnRepresentation { .. })) => {}
            _ => {
                return Err(TryOpenError::Status(zx::Status::PEER_CLOSED));
            }
        }
        Ok(node)
    }

    async fn provide_builtin(&self, filter: &EventFilter) -> Vec<Event> {
        if let Ok(capabilities) = self.builtin_capabilities.lock() {
            (*capabilities)
                .iter()
                .filter_map(|(name, node)| {
                    if !filter.contains("name", vec![name.to_string()]) {
                        return None;
                    }
                    let (node_clone, server_end) = fidl::endpoints::create_proxy().unwrap();
                    let event = node
                        .clone(fio::OpenFlags::CLONE_SAME_RIGHTS, server_end)
                        .map(|_| {
                            Event::new_builtin(EventPayload::DirectoryReady {
                                name: name.clone(),
                                node: node_clone,
                            })
                        })
                        .ok();
                    event
                })
                .collect()
        } else {
            vec![]
        }
    }
}

fn filter_matching_exposes<'a>(
    decl: &'a ComponentDecl,
    filter: Option<&EventFilter>,
) -> Vec<&'a ExposeDecl> {
    decl.exposes
        .iter()
        .filter(|expose_decl| {
            match expose_decl {
                ExposeDecl::Directory(ExposeDirectoryDecl {
                    source, target, target_name, ..
                }) => {
                    if let Some(filter) = filter {
                        if !filter.contains("name", vec![target_name.to_string()]) {
                            return false;
                        }
                    }
                    if target != &ExposeTarget::Framework || source != &ExposeSource::Self_ {
                        return false;
                    }
                }
                _ => {
                    return false;
                }
            }
            true
        })
        .collect()
}

#[async_trait]
impl EventSynthesisProvider for DirectoryReadyNotifier {
    async fn provide(&self, component: ExtendedComponent, filter: &EventFilter) -> Vec<Event> {
        let component = match component {
            ExtendedComponent::ComponentManager => {
                return self.provide_builtin(filter).await;
            }
            ExtendedComponent::ComponentInstance(component) => component,
        };
        let decl = match *component.lock_state().await {
            InstanceState::Resolved(ref s) => s.decl().clone(),
            InstanceState::New | InstanceState::Unresolved | InstanceState::Destroyed => {
                return vec![];
            }
        };
        let matching_exposes = filter_matching_exposes(&decl, Some(&filter));
        if matching_exposes.is_empty() {
            // Short-circuit if there are no matching exposes so we don't wait for the component's
            // outgoing directory if there are no DirectoryReady events to send.
            return vec![];
        }

        let outgoing_dir = {
            let execution = component.lock_execution().await;
            match execution.runtime.as_ref() {
                Some(runtime) => runtime.outgoing_dir.clone(),
                None => return vec![],
            }
        };
        self.create_events(outgoing_dir, &decl, matching_exposes, &component).await
    }
}

enum TryOpenError {
    Fidl(fidl::Error),
    Status(zx::Status),
}

#[async_trait]
impl Hook for DirectoryReadyNotifier {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        let target_moniker = event
            .target_moniker
            .unwrap_instance_moniker_or(ModelError::UnexpectedComponentManagerMoniker)?;
        match &event.payload {
            EventPayload::Started { runtime, component_decl, .. } => {
                if filter_matching_exposes(&component_decl, None).is_empty() {
                    // Short-circuit if there are no matching exposes so we don't spawn a task
                    // if there's nothing to do. In particular, don't wait for the component's
                    // outgoing directory if there are no DirectoryReady events to send.
                    return Ok(());
                }
                if let Some(outgoing_dir) = &runtime.outgoing_dir {
                    self.on_component_started(
                        &target_moniker,
                        outgoing_dir,
                        component_decl.clone(),
                    )
                    .await?;
                }
            }
            _ => {}
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{
        context::ModelContext,
        environment::Environment,
        testing::test_helpers::{TestEnvironmentBuilder, TestModelResult},
    };
    use cm_rust_testing::ComponentDeclBuilder;
    use moniker::AbsoluteMonikerBase;
    use std::convert::TryFrom;

    #[fuchsia::test]
    async fn verify_get_event_retry() {
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fio::DirectoryMarker>().unwrap();
        let _task = fasync::Task::spawn(async move {
            serve_fake_dir(stream).await;
        });

        let components = vec![("root", ComponentDeclBuilder::new().build())];
        let TestModelResult { model, .. } =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let component = Arc::new(ComponentInstance::new_root(
            Environment::empty(),
            Arc::new(ModelContext::new_for_test()),
            Weak::new(),
            "test:///root".to_string(),
        ));
        let notifier = DirectoryReadyNotifier::new(Arc::downgrade(&model));
        let event = notifier
            .create_event(
                &component,
                Ok(&proxy),
                Rights::from(fio::R_STAR_DIR),
                &CapabilityPath::try_from("/foo").unwrap(),
                &CapabilityName::from("foo"),
            )
            .await
            .unwrap();
        assert_eq!(event.target_moniker, AbsoluteMoniker::root().into());
        assert_eq!(event.component_url, "test:///root");
        let payload = event.payload;

        match payload {
            EventPayload::DirectoryReady { name, .. } => {
                assert_eq!(name, "foo");
            }
            other => {
                panic!("Unexpected payload: {:?}", other);
            }
        }
    }

    /// Serves a fake directory that returns NOT_FOUND for the given path until the third request.
    async fn serve_fake_dir(mut request_stream: fio::DirectoryRequestStream) {
        let mut requests = 0;
        while let Some(req) = request_stream.next().await {
            match req {
                Ok(fio::DirectoryRequest::Open { path, object, .. }) => {
                    assert_eq!("foo", path);
                    let (_stream, control_handle) =
                        object.into_stream_and_control_handle().unwrap();
                    if requests >= 3 {
                        control_handle
                            .send_on_open_(
                                zx::Status::OK.into_raw(),
                                Some(fio::NodeInfoDeprecated::Directory(fio::DirectoryObject {})),
                            )
                            .unwrap();
                    } else {
                        control_handle
                            .send_on_open_(zx::Status::NOT_FOUND.into_raw(), None)
                            .unwrap();
                    }
                    requests += 1;
                }
                _ => {}
            }
        }
    }
}
