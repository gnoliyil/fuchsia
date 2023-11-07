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
    cm_rust::{ComponentDecl, ExposeDecl, ExposeDirectoryDecl, ExposeSource, ExposeTarget},
    cm_types::{Name, Path},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio, fuchsia_async as fasync, fuchsia_fs, fuchsia_zircon as zx,
    futures::stream::StreamExt,
    moniker::Moniker,
    std::{
        sync::{Arc, Weak},
        time::Duration,
    },
    thiserror::Error,
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
}

impl DirectoryReadyNotifier {
    pub fn new(model: Weak<Model>) -> Self {
        Self { model }
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "DirectoryReadyNotifier",
            vec![EventType::Started],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    async fn on_component_started(
        self: &Arc<Self>,
        target_moniker: &Moniker,
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
                    if let Ok(component) = model.find_and_maybe_resolve(&target_moniker).await {
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
        .ok_or_else(|| ModelError::open_directory_error(target.moniker.clone(), "/"));

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
        source_path: &Path,
        target_name: &Name,
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
                    Err(TryOpenError::Fidl(_)) | Err(TryOpenError::Enumerate(_)) => {
                        break Err(ModelError::open_directory_error(
                            target.moniker.clone(),
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
                            target.moniker.clone(),
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
        // Check that the directory is present first.
        let canonicalized_path = fuchsia_fs::canonicalize_path(&source_path);
        if !fuchsia_fs::directory::dir_contains(&outgoing_dir, canonicalized_path).await? {
            return Err(TryOpenError::Status(zx::Status::NOT_FOUND));
        }
        let (node, server_end) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
        outgoing_dir.open(
            // TODO(fxbug.dev/118292): we might be able to remove READABLE from here, but at the
            // moment driver_manager fails to expose inspect if we remove it.
            rights.into_legacy()
                | fio::OpenFlags::DESCRIBE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::DIRECTORY,
            fio::ModeType::empty(),
            &canonicalized_path,
            ServerEnd::new(server_end.into_channel()),
        )?;
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
            ExtendedComponent::ComponentInstance(component) => component,
            ExtendedComponent::ComponentManager => {
                return vec![];
            }
        };
        let decl = match *component.lock_state().await {
            InstanceState::Resolved(ref s) => s.decl().clone(),
            InstanceState::New | InstanceState::Unresolved(_) | InstanceState::Destroyed => {
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

#[derive(Error, Debug)]
enum TryOpenError {
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    #[error(transparent)]
    Status(#[from] zx::Status),
    #[error(transparent)]
    Enumerate(#[from] fuchsia_fs::directory::EnumerateError),
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
    use moniker::MonikerBase;
    use zerocopy::AsBytes;

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
                &"/foo".parse().unwrap(),
                &"foo".parse().unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(event.target_moniker, Moniker::root().into());
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
        let mut at_end = true;
        while let Some(Ok(req)) = request_stream.next().await {
            match req {
                fio::DirectoryRequest::Rewind { responder, .. } => {
                    at_end = false;
                    responder.send(zx::Status::OK.into_raw()).unwrap();
                }
                fio::DirectoryRequest::ReadDirents { responder, .. } => {
                    const SIZE: usize = 3;
                    #[derive(AsBytes)]
                    #[repr(C, packed)]
                    struct Dirent {
                        ino: u64,
                        size: u8,
                        kind: u8,
                        name: [u8; SIZE],
                    }

                    if at_end {
                        responder.send(zx::Status::OK.into_raw(), &[]).unwrap();
                        continue;
                    }
                    let dirent = Dirent {
                        ino: fio::INO_UNKNOWN,
                        size: SIZE as u8,
                        kind: fio::DirentType::Directory.into_primitive(),
                        name: "foo".as_bytes().try_into().unwrap(),
                    };
                    let buf = dirent.as_bytes();
                    at_end = true;
                    responder.send(zx::Status::OK.into_raw(), &buf).unwrap();
                }
                fio::DirectoryRequest::Open { path, object, .. } => {
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
