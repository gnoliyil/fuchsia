// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource, PERMITTED_FLAGS},
        model::{
            component::{ComponentInstance, InstanceState},
            error::ModelError,
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
            model::Model,
            namespace::populate_and_get_logsink_decl,
            storage::admin_protocol::StorageAdmin,
        },
    },
    async_trait::async_trait,
    cm_rust::{CapabilityName, NativeIntoFidl},
    cm_task_scope::TaskScope,
    cm_util::channel,
    fidl::{
        endpoints::{ClientEnd, ServerEnd},
        prelude::*,
    },
    fidl_fuchsia_component_config as fcconfig, fidl_fuchsia_component_runner as fcrunner,
    fidl_fuchsia_io as fio, fidl_fuchsia_sys2 as fsys, fuchsia_zircon as zx,
    fuchsia_zircon::sys::ZX_CHANNEL_MAX_MSG_BYTES,
    futures::lock::Mutex,
    futures::StreamExt,
    lazy_static::lazy_static,
    measure_tape_for_instance::Measurable,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase, RelativeMoniker, RelativeMonikerBase},
    routing::component_instance::{ComponentInstanceInterface, ResolvedInstanceInterface},
    std::{
        convert::TryFrom,
        path::PathBuf,
        sync::{Arc, Weak},
    },
    tracing::warn,
};

lazy_static! {
    pub static ref REALM_QUERY_CAPABILITY_NAME: CapabilityName =
        fsys::RealmQueryMarker::PROTOCOL_NAME.into();
}

// Number of bytes the header of a vector occupies in a fidl message.
// TODO(https://fxbug.dev/98653): This should be a constant in a FIDL library.
const FIDL_VECTOR_HEADER_BYTES: usize = 16;

// Number of bytes the header of a fidl message occupies.
// TODO(https://fxbug.dev/98653): This should be a constant in a FIDL library.
const FIDL_HEADER_BYTES: usize = 16;

// Number of bytes of a manifest that can fit in a single message
// sent on a zircon channel.
const FIDL_MANIFEST_MAX_MSG_BYTES: usize =
    (ZX_CHANNEL_MAX_MSG_BYTES as usize) - (FIDL_HEADER_BYTES + FIDL_VECTOR_HEADER_BYTES);

// Serves the fuchsia.sys2.RealmQuery protocol.
pub struct RealmQuery {
    model: Arc<Model>,
}

impl RealmQuery {
    pub fn new(model: Arc<Model>) -> Self {
        Self { model }
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "RealmQuery",
            vec![EventType::CapabilityRouted],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    /// Given a `CapabilitySource`, determine if it is a framework-provided
    /// RealmQuery capability. If so, serve the capability.
    async fn on_capability_routed_async(
        self: Arc<Self>,
        source: CapabilitySource,
        capability_provider: Arc<Mutex<Option<Box<dyn CapabilityProvider>>>>,
    ) -> Result<(), ModelError> {
        // If this is a scoped framework directory capability, then check the source path
        if let CapabilitySource::Framework { capability, component } = source {
            if capability.matches_protocol(&REALM_QUERY_CAPABILITY_NAME) {
                // Set the capability provider, if not already set.
                let mut capability_provider = capability_provider.lock().await;
                if capability_provider.is_none() {
                    *capability_provider = Some(Box::new(RealmQueryCapabilityProvider::query(
                        self,
                        component.abs_moniker.clone(),
                    )));
                }
            }
        }
        Ok(())
    }

    /// Serve the fuchsia.sys2.RealmQuery protocol for a given scope on a given stream
    pub async fn serve(
        self: Arc<Self>,
        scope_moniker: AbsoluteMoniker,
        mut stream: fsys::RealmQueryRequestStream,
    ) {
        loop {
            let request = match stream.next().await {
                Some(Ok(request)) => request,
                Some(Err(error)) => {
                    warn!(?error, "Could not get next RealmQuery request");
                    break;
                }
                None => break,
            };
            let result = match request {
                fsys::RealmQueryRequest::GetInstance { moniker, responder } => {
                    let mut result = get_instance(&self.model, &scope_moniker, &moniker).await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::GetManifest { moniker, responder } => {
                    let mut result = get_manifest(&self.model, &scope_moniker, &moniker).await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::GetStructuredConfig { moniker, responder } => {
                    let mut result =
                        get_structured_config(&self.model, &scope_moniker, &moniker).await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::GetAllInstances { responder } => {
                    let mut result = get_all_instances(&self.model, &scope_moniker).await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::ConstructNamespace { moniker, responder } => {
                    let mut result =
                        construct_namespace(&self.model, &scope_moniker, &moniker).await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::Open {
                    moniker,
                    dir_type,
                    flags,
                    mode,
                    path,
                    object,
                    responder,
                } => {
                    let mut result = open(
                        &self.model,
                        &scope_moniker,
                        &moniker,
                        dir_type,
                        flags,
                        mode,
                        &path,
                        object,
                    )
                    .await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::ConnectToStorageAdmin {
                    moniker,
                    storage_name,
                    server_end,
                    responder,
                } => {
                    let mut result = connect_to_storage_admin(
                        &self.model,
                        &scope_moniker,
                        &moniker,
                        storage_name,
                        server_end,
                    )
                    .await;
                    responder.send(&mut result)
                }
                // The below methods are deprecated and will be removed in a future API release.
                fsys::RealmQueryRequest::GetInstanceInfo { moniker, responder } => {
                    let mut result = deprecated::get_instance_info_and_resolved_state(
                        &self.model,
                        &scope_moniker,
                        &moniker,
                    )
                    .await;
                    responder.send(&mut result)
                }
                fsys::RealmQueryRequest::GetInstanceDirectories { moniker, responder } => {
                    let mut result =
                        deprecated::get_instance_directories(&self.model, &scope_moniker, &moniker)
                            .await;
                    responder.send(&mut result)
                }
            };
            if let Err(error) = result {
                warn!(?error, "Could not respond to RealmQuery request");
                break;
            }
        }
    }
}

#[async_trait]
impl Hook for RealmQuery {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        match &event.payload {
            EventPayload::CapabilityRouted { source, capability_provider } => {
                self.on_capability_routed_async(source.clone(), capability_provider.clone())
                    .await?;
            }
            _ => {}
        }
        Ok(())
    }
}

pub struct RealmQueryCapabilityProvider {
    query: Arc<RealmQuery>,
    scope_moniker: AbsoluteMoniker,
}

impl RealmQueryCapabilityProvider {
    pub fn query(query: Arc<RealmQuery>, scope_moniker: AbsoluteMoniker) -> Self {
        Self { query, scope_moniker }
    }
}

#[async_trait]
impl CapabilityProvider for RealmQueryCapabilityProvider {
    async fn open(
        self: Box<Self>,
        task_scope: TaskScope,
        flags: fio::OpenFlags,
        relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), ModelError> {
        let forbidden = flags - PERMITTED_FLAGS;
        if !forbidden.is_empty() {
            warn!(?forbidden, "RealmQuery capability");
            return Ok(());
        }

        if relative_path.components().count() != 0 {
            warn!(
                path=%relative_path.display(),
                "RealmQuery capability got open request with non-empty",
            );
            return Ok(());
        }

        let server_end = channel::take_channel(server_end);

        let server_end = ServerEnd::<fsys::RealmQueryMarker>::new(server_end);
        let stream: fsys::RealmQueryRequestStream =
            server_end.into_stream().map_err(ModelError::stream_creation_error)?;
        task_scope
            .add_task(async move {
                self.query.serve(self.scope_moniker, stream).await;
            })
            .await;

        Ok(())
    }
}

/// Create the state matching the given moniker string in this scope
pub async fn get_instance(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    moniker_str: &str,
) -> Result<fsys::Instance, fsys::GetInstanceError> {
    // Construct the complete moniker using the scope moniker and the relative moniker string.
    let relative_moniker =
        RelativeMoniker::try_from(moniker_str).map_err(|_| fsys::GetInstanceError::BadMoniker)?;
    let moniker = scope_moniker.descendant(&relative_moniker);

    // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
    let instance = model.find(&moniker).await.ok_or(fsys::GetInstanceError::InstanceNotFound)?;
    let instance_id = model.component_id_index().look_up_moniker(&instance.abs_moniker).cloned();

    let resolved_info = {
        let state = instance.lock_state().await;
        let execution = instance.lock_execution().await;

        match &*state {
            InstanceState::Resolved(r) => {
                let url = r.address().url().to_string();

                let execution_info = if let Some(runtime) = &execution.runtime {
                    let start_reason = runtime.start_reason.to_string();
                    let execution_info = Some(fsys::ExecutionInfo {
                        start_reason: Some(start_reason),
                        ..fsys::ExecutionInfo::EMPTY
                    });
                    execution_info
                } else {
                    None
                };

                let resolved_info = Some(fsys::ResolvedInfo {
                    resolved_url: Some(url),
                    execution_info,
                    ..fsys::ResolvedInfo::EMPTY
                });

                resolved_info
            }
            _ => None,
        }
    };

    Ok(fsys::Instance {
        moniker: Some(relative_moniker.to_string()),
        url: Some(instance.component_url.clone()),
        instance_id: instance_id.map(|id| id.to_string()),
        resolved_info,
        ..fsys::Instance::EMPTY
    })
}

/// Encode the component manifest of an instance into a standalone persistable FIDL format.
pub async fn get_manifest(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    moniker_str: &str,
) -> Result<ClientEnd<fsys::ManifestBytesIteratorMarker>, fsys::GetManifestError> {
    // Construct the complete moniker using the scope moniker and the relative moniker string.
    let relative_moniker =
        RelativeMoniker::try_from(moniker_str).map_err(|_| fsys::GetManifestError::BadMoniker)?;
    let moniker = scope_moniker.descendant(&relative_moniker);

    // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
    let instance = model.find(&moniker).await.ok_or(fsys::GetManifestError::InstanceNotFound)?;

    let state = instance.lock_state().await;

    let decl = match &*state {
        InstanceState::Resolved(r) => r.decl().clone().native_into_fidl(),
        _ => return Err(fsys::GetManifestError::InstanceNotResolved),
    };

    let bytes = fidl::encoding::persist(&decl).map_err(|error| {
        warn!(%moniker, %error, "RealmQuery failed to encode manifest");
        fsys::GetManifestError::EncodeFailed
    })?;

    // Attach the iterator task to the scope root.
    let scope_root =
        model.find(scope_moniker).await.ok_or(fsys::GetManifestError::InstanceNotFound)?;

    let (client_end, server_end) =
        fidl::endpoints::create_endpoints::<fsys::ManifestBytesIteratorMarker>();

    // Attach the iterator task to the scope root.
    let task_scope = scope_root.nonblocking_task_scope();
    task_scope.add_task(serve_manifest_bytes_iterator(server_end, bytes)).await;

    Ok(client_end)
}

/// Get the structured config of an instance
pub async fn get_structured_config(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    moniker_str: &str,
) -> Result<fcconfig::ResolvedConfig, fsys::GetStructuredConfigError> {
    // Construct the complete moniker using the scope moniker and the relative moniker string.
    let relative_moniker = RelativeMoniker::try_from(moniker_str)
        .map_err(|_| fsys::GetStructuredConfigError::BadMoniker)?;
    let moniker = scope_moniker.descendant(&relative_moniker);

    // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
    let instance =
        model.find(&moniker).await.ok_or(fsys::GetStructuredConfigError::InstanceNotFound)?;

    let state = instance.lock_state().await;

    let config = match &*state {
        InstanceState::Resolved(r) => {
            r.config().ok_or(fsys::GetStructuredConfigError::NoConfig)?.clone().into()
        }
        _ => return Err(fsys::GetStructuredConfigError::InstanceNotResolved),
    };

    Ok(config)
}

async fn construct_namespace(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    moniker_str: &str,
) -> Result<Vec<fcrunner::ComponentNamespaceEntry>, fsys::ConstructNamespaceError> {
    // Construct the complete moniker using the scope moniker and the relative moniker string.
    let relative_moniker = RelativeMoniker::try_from(moniker_str)
        .map_err(|_| fsys::ConstructNamespaceError::BadMoniker)?;
    let moniker = scope_moniker.descendant(&relative_moniker);

    // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
    let instance =
        model.find(&moniker).await.ok_or(fsys::ConstructNamespaceError::InstanceNotFound)?;
    let mut state = instance.lock_state().await;
    match &mut *state {
        InstanceState::Resolved(r) => {
            let pkg_dir = r.package().map(|p| &p.package_dir);
            let (ns_entries, _) =
                populate_and_get_logsink_decl(pkg_dir, &instance, r.decl()).await.unwrap();
            Ok(ns_entries)
        }
        _ => Err(fsys::ConstructNamespaceError::InstanceNotResolved),
    }
}

async fn open(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    moniker_str: &str,
    dir_type: fsys::OpenDirType,
    flags: fio::OpenFlags,
    mode: fio::ModeType,
    path: &str,
    object: ServerEnd<fio::NodeMarker>,
) -> Result<(), fsys::OpenError> {
    // Construct the complete moniker using the scope moniker and the relative moniker string.
    let relative_moniker =
        RelativeMoniker::try_from(moniker_str).map_err(|_| fsys::OpenError::BadMoniker)?;
    let moniker = scope_moniker.descendant(&relative_moniker);

    // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
    let instance = model.find(&moniker).await.ok_or(fsys::OpenError::InstanceNotFound)?;

    match dir_type {
        fsys::OpenDirType::OutgoingDir => {
            let execution = instance.lock_execution().await;
            let dir = execution
                .runtime
                .as_ref()
                .ok_or(fsys::OpenError::InstanceNotRunning)?
                .outgoing_dir
                .as_ref()
                .ok_or(fsys::OpenError::NoSuchDir)?;
            dir.open(flags, mode, path, object).map_err(|_| fsys::OpenError::FidlError)
        }
        fsys::OpenDirType::RuntimeDir => {
            let execution = instance.lock_execution().await;
            let dir = execution
                .runtime
                .as_ref()
                .ok_or(fsys::OpenError::InstanceNotRunning)?
                .runtime_dir
                .as_ref()
                .ok_or(fsys::OpenError::NoSuchDir)?;
            dir.open(flags, mode, path, object).map_err(|_| fsys::OpenError::FidlError)
        }
        fsys::OpenDirType::PackageDir => {
            let mut state = instance.lock_state().await;
            match &mut *state {
                InstanceState::Resolved(r) => {
                    let pkg = r.package().ok_or(fsys::OpenError::NoSuchDir)?;
                    pkg.package_dir
                        .open(flags, mode, path, object)
                        .map_err(|_| fsys::OpenError::FidlError)
                }
                _ => Err(fsys::OpenError::InstanceNotResolved),
            }
        }
        fsys::OpenDirType::ExposedDir => {
            let mut state = instance.lock_state().await;
            match &mut *state {
                InstanceState::Resolved(r) => {
                    let path = vfs::path::Path::validate_and_split(path)
                        .map_err(|_| fsys::OpenError::BadPath)?;

                    r.get_exposed_dir().open(flags, path, object);
                    Ok(())
                }
                _ => Err(fsys::OpenError::InstanceNotResolved),
            }
        }
        fsys::OpenDirType::NamespaceDir => {
            let mut state = instance.lock_state().await;
            match &mut *state {
                InstanceState::Resolved(r) => {
                    let path = vfs::path::Path::validate_and_split(path)
                        .map_err(|_| fsys::OpenError::BadPath)?;

                    r.get_ns_dir().open(flags, path, object);
                    Ok(())
                }
                _ => Err(fsys::OpenError::InstanceNotResolved),
            }
        }
        _ => Err(fsys::OpenError::BadDirType),
    }
}

async fn connect_to_storage_admin(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    moniker_str: &str,
    storage_name: String,
    server_end: ServerEnd<fsys::StorageAdminMarker>,
) -> Result<(), fsys::ConnectToStorageAdminError> {
    // Construct the complete moniker using the scope moniker and the relative moniker string.
    let relative_moniker = RelativeMoniker::try_from(moniker_str)
        .map_err(|_| fsys::ConnectToStorageAdminError::BadMoniker)?;
    let moniker = scope_moniker.descendant(&relative_moniker);

    // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
    let instance =
        model.find(&moniker).await.ok_or(fsys::ConnectToStorageAdminError::InstanceNotFound)?;

    let storage_admin = StorageAdmin::new(Arc::downgrade(model));
    let task_scope = instance.nonblocking_task_scope();

    let storage_decl = {
        let mut state = instance.lock_state().await;
        match &mut *state {
            InstanceState::Resolved(r) => r
                .decl()
                .find_storage_source(&CapabilityName::from(storage_name))
                .ok_or(fsys::ConnectToStorageAdminError::StorageNotFound)?
                .clone(),
            _ => return Err(fsys::ConnectToStorageAdminError::InstanceNotResolved),
        }
    };

    task_scope
        .add_task(async move {
            if let Err(error) = Arc::new(storage_admin)
                .serve(storage_decl, instance.as_weak(), server_end.into_channel().into())
                .await
            {
                warn!(
                    %moniker, %error, "StorageAdmin created by LifecycleController failed to serve",
                );
            };
        })
        .await;
    Ok(())
}

/// Take a snapshot of all instances in the given scope and serves an instance iterator
/// over the snapshots.
async fn get_all_instances(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
) -> Result<ClientEnd<fsys::InstanceIteratorMarker>, fsys::GetAllInstancesError> {
    let mut instances = vec![];

    // Only take instances contained within the scope realm
    let scope_root =
        model.find(scope_moniker).await.ok_or(fsys::GetAllInstancesError::InstanceNotFound)?;

    let mut queue = vec![scope_root.clone()];

    while !queue.is_empty() {
        let cur = queue.pop().unwrap();

        let (instance, mut children) =
            get_fidl_instance_and_children(model, scope_moniker, &cur).await;
        instances.push(instance);
        queue.append(&mut children);
    }

    let (client_end, server_end) =
        fidl::endpoints::create_endpoints::<fsys::InstanceIteratorMarker>();

    // Attach the iterator task to the scope root.
    let task_scope = scope_root.nonblocking_task_scope();
    task_scope.add_task(serve_instance_iterator(server_end, instances)).await;

    Ok(client_end)
}

/// Create the detailed instance info matching the given moniker string in this scope
/// and return all live children of the instance.
async fn get_fidl_instance_and_children(
    model: &Arc<Model>,
    scope_moniker: &AbsoluteMoniker,
    instance: &Arc<ComponentInstance>,
) -> (fsys::Instance, Vec<Arc<ComponentInstance>>) {
    let relative_moniker = RelativeMoniker::scope_down(scope_moniker, &instance.abs_moniker)
        .expect("instance must have been a child of scope root");
    let instance_id = model.component_id_index().look_up_moniker(&instance.abs_moniker).cloned();

    let (resolved_info, children) = {
        let state = instance.lock_state().await;
        let execution = instance.lock_execution().await;
        match &*state {
            InstanceState::Resolved(r) => {
                let url = r.address().url().to_string();
                let children = r.children().map(|(_, c)| c.clone()).collect();

                let execution_info = if let Some(runtime) = &execution.runtime {
                    let start_reason = runtime.start_reason.to_string();
                    let execution_info = Some(fsys::ExecutionInfo {
                        start_reason: Some(start_reason),
                        ..fsys::ExecutionInfo::EMPTY
                    });
                    execution_info
                } else {
                    None
                };

                let resolved_info = Some(fsys::ResolvedInfo {
                    resolved_url: Some(url),
                    execution_info,
                    ..fsys::ResolvedInfo::EMPTY
                });
                (resolved_info, children)
            }
            _ => (None, vec![]),
        }
    };

    (
        fsys::Instance {
            moniker: Some(relative_moniker.to_string()),
            url: Some(instance.component_url.clone()),
            instance_id: instance_id.map(|id| id.to_string()),
            resolved_info,
            ..fsys::Instance::EMPTY
        },
        children,
    )
}

async fn serve_instance_iterator(
    server_end: ServerEnd<fsys::InstanceIteratorMarker>,
    mut instances: Vec<fsys::Instance>,
) {
    let mut stream: fsys::InstanceIteratorRequestStream = server_end.into_stream().unwrap();
    while let Some(Ok(fsys::InstanceIteratorRequest::Next { responder })) = stream.next().await {
        let mut bytes_used: usize = FIDL_HEADER_BYTES + FIDL_VECTOR_HEADER_BYTES;
        let mut instance_count = 0;

        // Determine how many info objects can be sent in a single FIDL message.
        // TODO(https://fxbug.dev/98653): This logic should be handled by FIDL.
        for instance in &instances {
            bytes_used += instance.measure().num_bytes;
            if bytes_used > ZX_CHANNEL_MAX_MSG_BYTES as usize {
                break;
            }
            instance_count += 1;
        }

        let batch: Vec<fsys::Instance> = instances.drain(0..instance_count).collect();
        let batch_size = batch.len();

        let result = responder.send(&mut batch.into_iter());
        if let Err(error) = result {
            warn!(?error, "RealmQuery encountered error sending instance batch");
            break;
        }

        // Close the iterator because all the data was sent.
        if batch_size == 0 {
            break;
        }
    }
}

async fn serve_manifest_bytes_iterator(
    server_end: ServerEnd<fsys::ManifestBytesIteratorMarker>,
    mut bytes: Vec<u8>,
) {
    let mut stream: fsys::ManifestBytesIteratorRequestStream = server_end.into_stream().unwrap();

    while let Some(Ok(fsys::ManifestBytesIteratorRequest::Next { responder })) = stream.next().await
    {
        let bytes_to_drain = std::cmp::min(FIDL_MANIFEST_MAX_MSG_BYTES, bytes.len());
        let batch: Vec<u8> = bytes.drain(0..bytes_to_drain).collect();
        let batch_size = batch.len();

        let result = responder.send(&batch);
        if let Err(error) = result {
            warn!(?error, "RealmQuery encountered error sending manifest bytes");
            break;
        }

        // Close the iterator because all the data was sent.
        if batch_size == 0 {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::component::StartReason,
        crate::model::testing::test_helpers::{TestEnvironmentBuilder, TestModelResult},
        assert_matches::assert_matches,
        cm_rust::*,
        cm_rust_testing::ComponentDeclBuilder,
        fidl::endpoints::{create_endpoints, create_proxy, create_proxy_and_stream},
        fidl_fuchsia_component_config as fconfig, fidl_fuchsia_component_decl as fcdecl,
        fidl_fuchsia_io as fio, fuchsia_async as fasync,
        routing_test_helpers::component_id_index::make_index_file,
    };

    fn is_closed(handle: impl fidl::AsHandleRef) -> bool {
        handle.wait_handle(zx::Signals::OBJECT_PEER_CLOSED, zx::Time::from_nanos(0)).is_ok()
    }

    #[fuchsia::test]
    async fn get_instance_test() {
        // Create index.
        let iid = format!("1234{}", "5".repeat(60));
        let index_file = make_index_file(component_id_index::Index {
            instances: vec![component_id_index::InstanceIdEntry {
                instance_id: Some(iid.clone()),
                appmgr_moniker: None,
                moniker: Some(AbsoluteMoniker::parse_str("/").unwrap()),
            }],
            ..component_id_index::Index::default()
        })
        .unwrap();

        let components = vec![("root", ComponentDeclBuilder::new().build())];

        let TestModelResult { model, builtin_environment, .. } = TestEnvironmentBuilder::new()
            .set_components(components)
            .set_component_id_index_path(index_file.path().to_str().map(str::to_string))
            .build()
            .await;

        let realm_query = {
            let env = builtin_environment.lock().await;
            env.realm_query.clone().unwrap()
        };

        let (query, query_request_stream) =
            create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

        let _query_task = fasync::Task::local(async move {
            realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
        });

        model.start().await;

        let instance = query.get_instance("./").await.unwrap().unwrap();

        assert_eq!(instance.moniker.unwrap(), ".");
        assert_eq!(instance.url.unwrap(), "test:///root");
        assert_eq!(instance.instance_id.unwrap(), iid);

        let resolved = instance.resolved_info.unwrap();
        assert_eq!(resolved.resolved_url.unwrap(), "test:///root");

        let execution = resolved.execution_info.unwrap();
        assert_eq!(execution.start_reason.unwrap(), StartReason::Root.to_string());
    }

    #[fuchsia::test]
    async fn manifest_test() {
        // Try to create a manifest that will exceed the size of a Zircon channel message.
        let mut manifest = ComponentDeclBuilder::new();

        for i in 0..10000 {
            let use_name = format!("use_{}", i);
            let expose_name = format!("expose_{}", i);
            let capability_path = format!("/svc/capability_{}", i);

            let use_decl = UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Framework,
                source_name: use_name.into(),
                target_path: CapabilityPath::try_from(capability_path.as_str()).unwrap(),
                dependency_type: DependencyType::Strong,
                availability: Availability::Required,
            });

            let expose_decl = ExposeDecl::Protocol(ExposeProtocolDecl {
                source: ExposeSource::Self_,
                source_name: expose_name.clone().into(),
                target: ExposeTarget::Parent,
                target_name: expose_name.into(),
                availability: Availability::Required,
            });

            manifest = manifest.use_(use_decl).expose(expose_decl);
        }

        let components = vec![("root", manifest.build())];

        let TestModelResult { model, builtin_environment, .. } =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let realm_query = {
            let env = builtin_environment.lock().await;
            env.realm_query.clone().unwrap()
        };

        let (query, query_request_stream) =
            create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

        let _query_task = fasync::Task::local(async move {
            realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
        });

        model.start().await;

        let iterator = query.get_manifest("./").await.unwrap().unwrap();
        let iterator = iterator.into_proxy().unwrap();

        let mut bytes = vec![];

        loop {
            let mut batch = iterator.next().await.unwrap();
            if batch.is_empty() {
                break;
            }
            bytes.append(&mut batch);
        }

        let manifest = fidl::encoding::unpersist::<fcdecl::Component>(&bytes).unwrap();

        // Component should have 10000 use and expose decls
        let uses = manifest.uses.unwrap();
        let exposes = manifest.exposes.unwrap();
        assert_eq!(uses.len(), 10000);

        for use_ in uses {
            let use_ = use_.fidl_into_native();
            assert!(use_.source_name().str().starts_with("use_"));
            assert!(use_.path().unwrap().to_string().starts_with("/svc/capability_"));
        }

        assert_eq!(exposes.len(), 10000);

        for expose in exposes {
            let expose = expose.fidl_into_native();
            assert!(expose.source_name().str().starts_with("expose_"));
        }
    }

    #[fuchsia::test]
    async fn structured_config_test() {
        let checksum = ConfigChecksum::Sha256([
            0x07, 0xA8, 0xE6, 0x85, 0xC8, 0x79, 0xA9, 0x79, 0xC3, 0x26, 0x17, 0xDC, 0x4E, 0x74,
            0x65, 0x7F, 0xF1, 0xF7, 0x73, 0xE7, 0x12, 0xEE, 0x51, 0xFD, 0xF6, 0x57, 0x43, 0x07,
            0xA7, 0xAF, 0x2E, 0x64,
        ]);

        let config = ConfigDecl {
            fields: vec![ConfigField { key: "my_field".to_string(), type_: ConfigValueType::Bool }],
            checksum: checksum.clone(),
            value_source: ConfigValueSource::PackagePath("meta/root.cvf".into()),
        };

        let config_values = ValuesData {
            values: vec![ValueSpec { value: Value::Single(SingleValue::Bool(true)) }],
            checksum: checksum.clone(),
        };

        let components = vec![("root", ComponentDeclBuilder::new().add_config(config).build())];

        let TestModelResult { model, builtin_environment, .. } = TestEnvironmentBuilder::new()
            .set_components(components)
            .set_config_values(vec![("meta/root.cvf", config_values)])
            .build()
            .await;

        let realm_query = {
            let env = builtin_environment.lock().await;
            env.realm_query.clone().unwrap()
        };

        let (query, query_request_stream) =
            create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

        let _query_task = fasync::Task::local(async move {
            realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
        });

        model.start().await;

        let config = query.get_structured_config("./").await.unwrap().unwrap();

        // Component should have one config field with right value
        assert_eq!(config.fields.len(), 1);
        let field = &config.fields[0];
        assert_eq!(field.key, "my_field");
        assert_matches!(field.value, fconfig::Value::Single(fconfig::SingleValue::Bool(true)));
        assert_eq!(config.checksum, checksum.native_into_fidl());
    }

    #[fuchsia::test]
    async fn open_test() {
        let use_decl = UseDecl::Protocol(UseProtocolDecl {
            source: UseSource::Framework,
            source_name: "foo".into(),
            target_path: CapabilityPath::try_from("/svc/foo").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        });

        let expose_decl = ExposeDecl::Protocol(ExposeProtocolDecl {
            source: ExposeSource::Self_,
            source_name: "bar".into(),
            target: ExposeTarget::Parent,
            target_name: "bar".into(),
            availability: cm_rust::Availability::Required,
        });

        let components = vec![(
            "root",
            ComponentDeclBuilder::new().use_(use_decl.clone()).expose(expose_decl.clone()).build(),
        )];

        let TestModelResult { model, builtin_environment, .. } =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let realm_query = {
            let env = builtin_environment.lock().await;
            env.realm_query.clone().unwrap()
        };

        let (query, query_request_stream) =
            create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

        let _query_task = fasync::Task::local(async move {
            realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
        });

        model.start().await;

        let (outgoing_dir, server_end) = create_endpoints::<fio::DirectoryMarker>();
        let server_end = ServerEnd::new(server_end.into_channel());
        query
            .open(
                "./",
                fsys::OpenDirType::OutgoingDir,
                fio::OpenFlags::RIGHT_READABLE,
                fio::ModeType::empty(),
                ".",
                server_end,
            )
            .await
            .unwrap()
            .unwrap();
        // The test runner has not been configured to serve the outgoing dir, so this directory
        // should just be closed.
        assert!(is_closed(outgoing_dir));

        let (runtime_dir, server_end) = create_endpoints::<fio::DirectoryMarker>();
        let server_end = ServerEnd::new(server_end.into_channel());
        query
            .open(
                "./",
                fsys::OpenDirType::RuntimeDir,
                fio::OpenFlags::RIGHT_READABLE,
                fio::ModeType::empty(),
                ".",
                server_end,
            )
            .await
            .unwrap()
            .unwrap();
        // The test runner has not been configured to serve the runtime dir, so this directory
        // should just be closed.
        assert!(is_closed(runtime_dir));

        let (pkg_dir, server_end) = create_proxy::<fio::DirectoryMarker>().unwrap();
        let server_end = ServerEnd::new(server_end.into_channel());
        query
            .open(
                "./",
                fsys::OpenDirType::PackageDir,
                fio::OpenFlags::RIGHT_READABLE,
                fio::ModeType::empty(),
                ".",
                server_end,
            )
            .await
            .unwrap()
            .unwrap();

        let (exposed_dir, server_end) = create_proxy::<fio::DirectoryMarker>().unwrap();
        let server_end = ServerEnd::new(server_end.into_channel());
        query
            .open(
                "./",
                fsys::OpenDirType::ExposedDir,
                fio::OpenFlags::RIGHT_READABLE,
                fio::ModeType::empty(),
                ".",
                server_end,
            )
            .await
            .unwrap()
            .unwrap();

        let (svc_dir, server_end) = create_proxy::<fio::DirectoryMarker>().unwrap();
        let server_end = ServerEnd::new(server_end.into_channel());
        query
            .open(
                "./",
                fsys::OpenDirType::NamespaceDir,
                fio::OpenFlags::empty(),
                fio::ModeType::empty(),
                "svc",
                server_end,
            )
            .await
            .unwrap()
            .unwrap();

        // Test resolvers provide a pkg dir with a fake file
        let entries = fuchsia_fs::directory::readdir(&pkg_dir).await.unwrap();
        assert_eq!(
            entries,
            vec![fuchsia_fs::directory::DirEntry {
                name: "fake_file".to_string(),
                kind: fuchsia_fs::directory::DirentKind::File
            }]
        );

        // Component Manager serves the exposed dir with the `bar` protocol
        let entries = fuchsia_fs::directory::readdir(&exposed_dir).await.unwrap();
        assert_eq!(
            entries,
            vec![fuchsia_fs::directory::DirEntry {
                name: "bar".to_string(),
                kind: fuchsia_fs::directory::DirentKind::Unknown
            }]
        );

        // Component Manager serves the namespace dir with the `foo` protocol.
        let entries = fuchsia_fs::directory::readdir(&svc_dir).await.unwrap();
        assert_eq!(
            entries,
            vec![fuchsia_fs::directory::DirEntry {
                name: "foo".to_string(),
                kind: fuchsia_fs::directory::DirentKind::Unknown
            }]
        );
    }

    #[fuchsia::test]
    async fn construct_namespace_test() {
        let use_decl = UseDecl::Protocol(UseProtocolDecl {
            source: UseSource::Framework,
            source_name: "foo".into(),
            target_path: CapabilityPath::try_from("/svc/foo").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        });

        let components = vec![("root", ComponentDeclBuilder::new().use_(use_decl.clone()).build())];

        let TestModelResult { model, builtin_environment, .. } =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let realm_query = {
            let env = builtin_environment.lock().await;
            env.realm_query.clone().unwrap()
        };

        let (query, query_request_stream) =
            create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

        let _query_task = fasync::Task::local(async move {
            realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
        });

        model.start().await;

        let mut ns = query.construct_namespace("./").await.unwrap().unwrap();

        assert_eq!(ns.len(), 2);

        // Test resolvers provide a pkg dir with a fake file
        let pkg_entry = ns.remove(0);
        assert_eq!(pkg_entry.path.unwrap(), "/pkg");
        let pkg_dir = pkg_entry.directory.unwrap().into_proxy().unwrap();

        let entries = fuchsia_fs::directory::readdir(&pkg_dir).await.unwrap();
        assert_eq!(
            entries,
            vec![fuchsia_fs::directory::DirEntry {
                name: "fake_file".to_string(),
                kind: fuchsia_fs::directory::DirentKind::File
            }]
        );

        // The component requested the `foo` protocol.
        let svc_entry = ns.remove(0);
        assert_eq!(svc_entry.path.unwrap(), "/svc");
        let svc_dir = svc_entry.directory.unwrap().into_proxy().unwrap();

        let entries = fuchsia_fs::directory::readdir(&svc_dir).await.unwrap();
        assert_eq!(
            entries,
            vec![fuchsia_fs::directory::DirEntry {
                name: "foo".to_string(),
                kind: fuchsia_fs::directory::DirentKind::Unknown
            }]
        );
    }

    #[fuchsia::test]
    async fn get_storage_admin_test() {
        let components = vec![
            (
                "root",
                ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .storage(StorageDecl {
                        name: "data".into(),
                        source: StorageDirectorySource::Child("a".to_string()),
                        backing_dir: "fs".into(),
                        subdir: Some("persistent".into()),
                        storage_id:
                            fidl_fuchsia_component_decl::StorageId::StaticInstanceIdOrMoniker,
                    })
                    .build(),
            ),
            (
                "a",
                ComponentDeclBuilder::new()
                    .directory(DirectoryDecl {
                        name: "fs".into(),
                        source_path: Some(CapabilityPath {
                            basename: "data".into(),
                            dirname: "/fs".into(),
                        }),
                        rights: fio::Operations::all(),
                    })
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source_name: "fs".into(),
                        target_name: "fs".into(),
                        subdir: None,
                        source: ExposeSource::Self_,
                        target: ExposeTarget::Parent,
                        rights: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
        ];

        let TestModelResult { model, builtin_environment, .. } =
            TestEnvironmentBuilder::new().set_components(components).build().await;

        let realm_query = {
            let env = builtin_environment.lock().await;
            env.realm_query.clone().unwrap()
        };

        let (query, query_request_stream) =
            create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

        let _query_task = fasync::Task::local(async move {
            realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
        });

        model.start().await;

        let (storage_admin, server_end) = create_proxy::<fsys::StorageAdminMarker>().unwrap();

        query.connect_to_storage_admin("./", "data", server_end).await.unwrap().unwrap();

        let (it_proxy, it_server) =
            create_proxy::<fsys::StorageIteratorMarker>().expect("create iterator");

        storage_admin.list_storage_in_realm("./", it_server).await.unwrap().unwrap();

        let res = it_proxy.next().await.unwrap();
        assert!(res.is_empty());
    }
}

// This module contains FIDL methods that are deprecated
// and will be removed in a future API release.
mod deprecated {
    use super::*;

    fn try_clone_dir_endpoint(
        dir: Option<&fio::DirectoryProxy>,
    ) -> Option<ClientEnd<fio::DirectoryMarker>> {
        if let Some(dir) = dir {
            if let Ok(cloned_dir) = fuchsia_fs::directory::clone_no_describe(&dir, None) {
                let cloned_dir_channel = cloned_dir.into_channel().unwrap().into_zx_channel();
                Some(ClientEnd::new(cloned_dir_channel))
            } else {
                None
            }
        } else {
            None
        }
    }

    /// Create the instance info and state matching the given moniker string in this scope
    pub async fn get_instance_info_and_resolved_state(
        model: &Arc<Model>,
        scope_moniker: &AbsoluteMoniker,
        moniker_str: &str,
    ) -> Result<(fsys::InstanceInfo, Option<Box<fsys::ResolvedState>>), fsys::RealmQueryError> {
        // Construct the complete moniker using the scope moniker and the relative moniker string.
        let relative_moniker = RelativeMoniker::try_from(moniker_str)
            .map_err(|_| fsys::RealmQueryError::BadMoniker)?;
        let moniker = scope_moniker.descendant(&relative_moniker);

        // TODO(https://fxbug.dev/108532): Close the connection if the scope root cannot be found.
        let instance = model.find(&moniker).await.ok_or(fsys::RealmQueryError::InstanceNotFound)?;

        let resolved = create_resolved_state(&instance).await;

        let instance_id = model.component_id_index().look_up_moniker(&moniker).cloned();

        let state = match &resolved {
            Some(r) => {
                if r.execution.is_some() {
                    fsys::InstanceState::Started
                } else {
                    fsys::InstanceState::Resolved
                }
            }
            None => fsys::InstanceState::Unresolved,
        };

        let info = fsys::InstanceInfo {
            moniker: relative_moniker.to_string(),
            url: instance.component_url.clone(),
            instance_id: instance_id.map(|id| id.to_string()),
            state,
        };

        Ok((info, resolved))
    }

    pub async fn get_instance_directories(
        model: &Arc<Model>,
        scope_moniker: &AbsoluteMoniker,
        moniker_str: &str,
    ) -> Result<Option<Box<fsys::ResolvedDirectories>>, fsys::RealmQueryError> {
        // Construct the complete moniker using the scope moniker and the relative moniker string.
        let relative_moniker = RelativeMoniker::try_from(moniker_str)
            .map_err(|_| fsys::RealmQueryError::BadMoniker)?;
        let moniker = scope_moniker.descendant(&relative_moniker);

        let instance = model.find(&moniker).await.ok_or(fsys::RealmQueryError::InstanceNotFound)?;

        let resolved_dirs = create_fidl_resolved_directories(&instance).await;

        Ok(resolved_dirs)
    }

    /// Locks on the instance and execution state of the component and creates a FIDL
    /// fuchsia.sys2.ResolvedState object.
    async fn create_resolved_state(
        component: &Arc<ComponentInstance>,
    ) -> Option<Box<fsys::ResolvedState>> {
        let state = component.lock_state().await;
        let execution = component.lock_execution().await;

        match &*state {
            InstanceState::Resolved(r) => {
                let uses =
                    r.decl().uses.clone().into_iter().map(|u| u.native_into_fidl()).collect();
                let exposes =
                    r.decl().exposes.clone().into_iter().map(|e| e.native_into_fidl()).collect();
                let config = r.config().cloned().map(|c| Box::new(c.into()));

                let pkg_dir = r.package().map(|p| &p.package_dir);
                let pkg_dir = try_clone_dir_endpoint(pkg_dir);

                let execution = if let Some(runtime) = &execution.runtime {
                    let out_dir = try_clone_dir_endpoint(runtime.outgoing_dir.as_ref());
                    let runtime_dir = try_clone_dir_endpoint(runtime.runtime_dir.as_ref());
                    let start_reason = runtime.start_reason.to_string();

                    Some(Box::new(fsys::ExecutionState { out_dir, runtime_dir, start_reason }))
                } else {
                    None
                };

                let (exposed_dir, expose_server) = fidl::endpoints::create_endpoints();
                r.get_exposed_dir().open(
                    fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
                    vfs::path::Path::dot(),
                    expose_server,
                );
                let exposed_dir = exposed_dir.into_channel();
                let exposed_dir =
                    fidl::endpoints::ClientEnd::<fio::DirectoryMarker>::new(exposed_dir);

                let (ns_dir, ns_server) = fidl::endpoints::create_endpoints();
                r.get_ns_dir().open(
                    fio::OpenFlags::RIGHT_READABLE
                        | fio::OpenFlags::RIGHT_WRITABLE
                        | fio::OpenFlags::RIGHT_EXECUTABLE,
                    vfs::path::Path::dot(),
                    ns_server,
                );
                let ns_dir = ns_dir.into_channel();
                let ns_dir = fidl::endpoints::ClientEnd::<fio::DirectoryMarker>::new(ns_dir);

                Some(Box::new(fsys::ResolvedState {
                    uses,
                    exposes,
                    config,
                    pkg_dir,
                    execution,
                    exposed_dir,
                    ns_dir,
                }))
            }
            _ => None,
        }
    }

    /// Locks on the instance and execution state of the component and creates a FIDL
    /// fuchsia.sys2.ResolvedDirectories object.
    async fn create_fidl_resolved_directories(
        component: &Arc<ComponentInstance>,
    ) -> Option<Box<fsys::ResolvedDirectories>> {
        let mut state = component.lock_state().await;
        let execution = component.lock_execution().await;

        match &mut *state {
            InstanceState::Resolved(r) => {
                let pkg_dir = r.package().map(|p| &p.package_dir);
                let pkg_dir_endpoint = try_clone_dir_endpoint(pkg_dir);

                let execution_dirs = if let Some(runtime) = &execution.runtime {
                    let out_dir = try_clone_dir_endpoint(runtime.outgoing_dir.as_ref());
                    let runtime_dir = try_clone_dir_endpoint(runtime.runtime_dir.as_ref());

                    Some(Box::new(fsys::ExecutionDirectories { out_dir, runtime_dir }))
                } else {
                    None
                };

                let (exposed_dir, expose_server) = fidl::endpoints::create_endpoints();
                r.get_exposed_dir().open(
                    fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
                    vfs::path::Path::dot(),
                    expose_server,
                );
                let exposed_dir = exposed_dir.into_channel();
                let exposed_dir =
                    fidl::endpoints::ClientEnd::<fio::DirectoryMarker>::new(exposed_dir);

                let pkg_dir = r.package().map(|p| &p.package_dir);
                let (ns_entries, _) =
                    populate_and_get_logsink_decl(pkg_dir, component, r.decl()).await.unwrap();

                Some(Box::new(fsys::ResolvedDirectories {
                    ns_entries,
                    pkg_dir: pkg_dir_endpoint,
                    exposed_dir,
                    execution_dirs,
                }))
            }
            _ => None,
        }
    }

    #[cfg(test)]
    mod tests {
        use {
            super::*,
            crate::model::component::StartReason,
            crate::model::testing::test_helpers::{TestEnvironmentBuilder, TestModelResult},
            assert_matches::assert_matches,
            cm_rust::*,
            cm_rust_testing::ComponentDeclBuilder,
            fidl::endpoints::create_proxy_and_stream,
            fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_config as fconfig,
            fidl_fuchsia_component_decl as fdecl, fuchsia_async as fasync,
            moniker::*,
            routing_test_helpers::component_id_index::make_index_file,
        };

        fn is_closed(handle: impl fidl::AsHandleRef) -> bool {
            handle.wait_handle(zx::Signals::OBJECT_PEER_CLOSED, zx::Time::from_nanos(0)).is_ok()
        }

        #[fuchsia::test]
        async fn read_all_properties() {
            // Create index.
            let iid = format!("1234{}", "5".repeat(60));
            let index_file = make_index_file(component_id_index::Index {
                instances: vec![component_id_index::InstanceIdEntry {
                    instance_id: Some(iid.clone()),
                    appmgr_moniker: None,
                    moniker: Some(AbsoluteMoniker::parse_str("/").unwrap()),
                }],
                ..component_id_index::Index::default()
            })
            .unwrap();

            let use_decl = UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Framework,
                source_name: "foo".into(),
                target_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                dependency_type: DependencyType::Strong,
                availability: Availability::Required,
            });

            let expose_decl = ExposeDecl::Protocol(ExposeProtocolDecl {
                source: ExposeSource::Self_,
                source_name: "bar".into(),
                target: ExposeTarget::Parent,
                target_name: "bar".into(),
                availability: cm_rust::Availability::Required,
            });

            let checksum = ConfigChecksum::Sha256([
                0x07, 0xA8, 0xE6, 0x85, 0xC8, 0x79, 0xA9, 0x79, 0xC3, 0x26, 0x17, 0xDC, 0x4E, 0x74,
                0x65, 0x7F, 0xF1, 0xF7, 0x73, 0xE7, 0x12, 0xEE, 0x51, 0xFD, 0xF6, 0x57, 0x43, 0x07,
                0xA7, 0xAF, 0x2E, 0x64,
            ]);

            let config = ConfigDecl {
                fields: vec![ConfigField {
                    key: "my_field".to_string(),
                    type_: ConfigValueType::Bool,
                }],
                checksum: checksum.clone(),
                value_source: ConfigValueSource::PackagePath("meta/root.cvf".into()),
            };

            let config_values = ValuesData {
                values: vec![ValueSpec { value: Value::Single(SingleValue::Bool(true)) }],
                checksum: checksum.clone(),
            };

            let components = vec![(
                "root",
                ComponentDeclBuilder::new()
                    .add_config(config)
                    .use_(use_decl.clone())
                    .expose(expose_decl.clone())
                    .build(),
            )];

            let TestModelResult { model, builtin_environment, .. } = TestEnvironmentBuilder::new()
                .set_components(components)
                .set_component_id_index_path(index_file.path().to_str().map(str::to_string))
                .set_config_values(vec![("meta/root.cvf", config_values)])
                .build()
                .await;

            let realm_query = {
                let env = builtin_environment.lock().await;
                env.realm_query.clone().unwrap()
            };

            let (query, query_request_stream) =
                create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

            let _query_task = fasync::Task::local(async move {
                realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
            });

            model.start().await;

            let (info, resolved) = query.get_instance_info("./").await.unwrap().unwrap();
            assert_eq!(info.moniker, ".");
            assert_eq!(info.url, "test:///root");
            assert_eq!(info.state, fsys::InstanceState::Started);
            assert_eq!(info.instance_id.clone().unwrap(), iid);

            let resolved = resolved.unwrap();
            let execution = resolved.execution.unwrap();

            // Component should have one config field with right value
            let config = resolved.config.unwrap();
            assert_eq!(config.fields.len(), 1);
            let field = &config.fields[0];
            assert_eq!(field.key, "my_field");
            assert_matches!(field.value, fconfig::Value::Single(fconfig::SingleValue::Bool(true)));
            assert_eq!(config.checksum, checksum.native_into_fidl());

            // Component should have one use and one expose decl
            assert_eq!(resolved.uses.len(), 1);
            assert_eq!(resolved.uses[0], use_decl.native_into_fidl());
            assert_eq!(resolved.exposes.len(), 1);
            assert_eq!(resolved.exposes[0], expose_decl.native_into_fidl());

            // Test resolvers provide a pkg dir with a fake file
            let pkg_dir = resolved.pkg_dir.unwrap();
            let pkg_dir = pkg_dir.into_proxy().unwrap();
            let entries = fuchsia_fs::directory::readdir(&pkg_dir).await.unwrap();
            assert_eq!(
                entries,
                vec![fuchsia_fs::directory::DirEntry {
                    name: "fake_file".to_string(),
                    kind: fuchsia_fs::directory::DirentKind::File
                }]
            );

            // Component Manager serves the exposed dir with the `bar` protocol
            let exposed_dir = resolved.exposed_dir.into_proxy().unwrap();
            let entries = fuchsia_fs::directory::readdir(&exposed_dir).await.unwrap();
            assert_eq!(
                entries,
                vec![fuchsia_fs::directory::DirEntry {
                    name: "bar".to_string(),
                    kind: fuchsia_fs::directory::DirentKind::Unknown
                }]
            );

            // Component Manager serves the namespace dir with the `foo` protocol.
            let ns_dir = resolved.ns_dir.into_proxy().unwrap();
            let entries = fuchsia_fs::directory::readdir(&ns_dir).await.unwrap();
            assert_eq!(
                entries,
                vec![
                    fuchsia_fs::directory::DirEntry {
                        name: "pkg".to_string(),
                        kind: fuchsia_fs::directory::DirentKind::Directory
                    },
                    fuchsia_fs::directory::DirEntry {
                        name: "svc".to_string(),
                        kind: fuchsia_fs::directory::DirentKind::Directory
                    },
                ]
            );
            let svc_dir =
                fuchsia_fs::directory::open_directory(&ns_dir, "svc", fio::OpenFlags::empty())
                    .await
                    .unwrap();
            let entries = fuchsia_fs::directory::readdir(&svc_dir).await.unwrap();
            assert_eq!(
                entries,
                vec![fuchsia_fs::directory::DirEntry {
                    name: "foo".to_string(),
                    kind: fuchsia_fs::directory::DirentKind::Unknown
                }]
            );

            // Test runners don't provide an out dir or a runtime dir
            assert!(is_closed(execution.out_dir.unwrap()));
            assert!(is_closed(execution.runtime_dir.unwrap()));
        }

        #[fuchsia::test]
        async fn observe_dynamic_lifecycle() {
            let components = vec![
                (
                    "root",
                    ComponentDeclBuilder::new()
                        .add_collection(CollectionDecl {
                            name: "my_coll".to_string(),
                            durability: fdecl::Durability::Transient,
                            environment: None,
                            allowed_offers: cm_types::AllowedOffers::StaticOnly,
                            allow_long_names: false,
                            persistent_storage: None,
                        })
                        .build(),
                ),
                ("a", ComponentDeclBuilder::new().build()),
            ];

            let TestModelResult { model, builtin_environment, .. } =
                TestEnvironmentBuilder::new().set_components(components).build().await;

            let realm_query = {
                let env = builtin_environment.lock().await;
                env.realm_query.clone().unwrap()
            };

            let (query, query_request_stream) =
                create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

            let _query_task = fasync::Task::local(async move {
                realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
            });

            model.start().await;

            let component_root = model.look_up(&AbsoluteMoniker::root()).await.unwrap();
            component_root
                .add_dynamic_child(
                    "my_coll".to_string(),
                    &ChildDecl {
                        name: "a".to_string(),
                        url: "test:///a".to_string(),
                        startup: fdecl::StartupMode::Lazy,
                        on_terminate: None,
                        environment: None,
                    },
                    fcomponent::CreateChildArgs::EMPTY,
                )
                .await
                .unwrap();

            // `a` should be unresolved
            let (info, resolved) = query.get_instance_info("./my_coll:a").await.unwrap().unwrap();
            assert_eq!(info.moniker, "./my_coll:a");
            assert_eq!(info.url, "test:///a");
            assert_eq!(info.state, fsys::InstanceState::Unresolved);
            assert!(info.instance_id.is_none());
            assert!(resolved.is_none());

            let moniker_a = AbsoluteMoniker::parse_str("/my_coll:a").unwrap();
            let component_a = model.look_up(&moniker_a).await.unwrap();

            // `a` should be resolved
            let (info, resolved) = query.get_instance_info("./my_coll:a").await.unwrap().unwrap();
            assert_eq!(info.state, fsys::InstanceState::Resolved);

            let resolved = resolved.unwrap();
            assert!(resolved.config.is_none());
            assert!(resolved.uses.is_empty());
            assert!(resolved.exposes.is_empty());
            assert!(resolved.pkg_dir.is_some());
            assert!(resolved.execution.is_none());

            let result = component_a.start(&StartReason::Debug).await.unwrap();
            assert_eq!(result, fsys::StartResult::Started);

            // `a` should be started
            let (info, resolved) = query.get_instance_info("./my_coll:a").await.unwrap().unwrap();
            assert_eq!(info.state, fsys::InstanceState::Started);

            let resolved = resolved.unwrap();
            assert!(resolved.config.is_none());
            assert!(resolved.uses.is_empty());
            assert!(resolved.exposes.is_empty());
            assert!(resolved.pkg_dir.is_some());

            let execution = resolved.execution.unwrap();
            assert!(is_closed(execution.out_dir.unwrap()));
            assert!(is_closed(execution.runtime_dir.unwrap()));

            component_a.stop_instance_internal(false).await.unwrap();

            // `a` should be stopped
            let (info, resolved) = query.get_instance_info("./my_coll:a").await.unwrap().unwrap();
            assert_eq!(info.state, fsys::InstanceState::Resolved);

            let resolved = resolved.unwrap();
            assert!(resolved.config.is_none());
            assert!(resolved.uses.is_empty());
            assert!(resolved.exposes.is_empty());
            assert!(resolved.pkg_dir.is_some());

            assert!(resolved.execution.is_none());

            let child_moniker = ChildMoniker::parse("my_coll:a").unwrap();
            component_root.remove_dynamic_child(&child_moniker).await.unwrap();

            // `a` should be destroyed after purge
            let err = query.get_instance_info("./my_coll:a").await.unwrap().unwrap_err();
            assert_eq!(err, fsys::RealmQueryError::InstanceNotFound);
        }

        #[fuchsia::test]
        async fn scoped_to_child() {
            let components = vec![
                ("root", ComponentDeclBuilder::new().add_lazy_child("a").build()),
                ("a", ComponentDeclBuilder::new().build()),
            ];

            let TestModelResult { model, builtin_environment, .. } =
                TestEnvironmentBuilder::new().set_components(components).build().await;

            let realm_query = {
                let env = builtin_environment.lock().await;
                env.realm_query.clone().unwrap()
            };

            let (query, query_request_stream) =
                create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

            let moniker_a = AbsoluteMoniker::parse_str("/a").unwrap();

            let _query_task = fasync::Task::local(async move {
                realm_query.serve(moniker_a, query_request_stream).await
            });

            model.start().await;

            // `a` should be unresolved
            let (info, resolved) = query.get_instance_info(".").await.unwrap().unwrap();
            assert_eq!(info.moniker, ".");
            assert_eq!(info.url, "test:///a");
            assert_eq!(info.state, fsys::InstanceState::Unresolved);
            assert!(info.instance_id.is_none());
            assert!(resolved.is_none());

            let moniker_a = AbsoluteMoniker::parse_str("/a").unwrap();
            let component_a = model.look_up(&moniker_a).await.unwrap();

            // `a` should be resolved
            let (info, resolved) = query.get_instance_info(".").await.unwrap().unwrap();
            assert_eq!(info.state, fsys::InstanceState::Resolved);

            let resolved = resolved.unwrap();
            assert!(resolved.config.is_none());
            assert!(resolved.uses.is_empty());
            assert!(resolved.exposes.is_empty());
            assert!(resolved.pkg_dir.is_some());

            assert!(resolved.execution.is_none());

            let result = component_a.start(&StartReason::Debug).await.unwrap();
            assert_eq!(result, fsys::StartResult::Started);

            // `a` should be started
            let (info, resolved) = query.get_instance_info(".").await.unwrap().unwrap();
            assert_eq!(info.state, fsys::InstanceState::Started);

            let resolved = resolved.unwrap();
            assert!(resolved.config.is_none());
            assert!(resolved.uses.is_empty());
            assert!(resolved.exposes.is_empty());
            assert!(resolved.pkg_dir.is_some());

            let execution = resolved.execution.unwrap();
            assert!(is_closed(execution.out_dir.unwrap()));
            assert!(is_closed(execution.runtime_dir.unwrap()));

            component_a.stop_instance_internal(false).await.unwrap();

            // `a` should be stopped
            let (info, resolved) = query.get_instance_info(".").await.unwrap().unwrap();
            assert_eq!(info.state, fsys::InstanceState::Resolved);

            let resolved = resolved.unwrap();
            assert!(resolved.config.is_none());
            assert!(resolved.uses.is_empty());
            assert!(resolved.exposes.is_empty());
            assert!(resolved.pkg_dir.is_some());

            assert!(resolved.execution.is_none());
        }

        #[fuchsia::test]
        async fn get_instance_directories() {
            let use_decl = UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Framework,
                source_name: "foo".into(),
                target_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                dependency_type: DependencyType::Strong,
                availability: Availability::Required,
            });

            let components =
                vec![("root", ComponentDeclBuilder::new().use_(use_decl.clone()).build())];

            let TestModelResult { model, builtin_environment, .. } =
                TestEnvironmentBuilder::new().set_components(components).build().await;

            let realm_query = {
                let env = builtin_environment.lock().await;
                env.realm_query.clone().unwrap()
            };

            let (query, query_request_stream) =
                create_proxy_and_stream::<fsys::RealmQueryMarker>().unwrap();

            let _query_task = fasync::Task::local(async move {
                realm_query.serve(AbsoluteMoniker::root(), query_request_stream).await
            });

            model.start().await;

            let resolved_dirs =
                query.get_instance_directories("./").await.unwrap().unwrap().unwrap();

            let ns_entries = resolved_dirs.ns_entries;
            assert_eq!(ns_entries.len(), 2);

            for entry in ns_entries {
                let path = entry.path.unwrap();
                let dir = entry.directory.unwrap().into_proxy().unwrap();
                match path.as_str() {
                    "/svc" => {
                        let entries = fuchsia_fs::directory::readdir(&dir).await.unwrap();
                        assert_eq!(
                            entries,
                            vec![fuchsia_fs::directory::DirEntry {
                                name: "foo".to_string(),
                                kind: fuchsia_fs::directory::DirentKind::Unknown
                            }]
                        );
                    }
                    "/pkg" => {}
                    path => panic!("unexpected directory: {}", path),
                }
            }

            assert!(resolved_dirs.pkg_dir.is_some());

            let execution_dirs = resolved_dirs.execution_dirs.unwrap();
            // Test runners don't provide an out dir or a runtime dir
            assert!(is_closed(execution_dirs.out_dir.unwrap()));
            assert!(is_closed(execution_dirs.runtime_dir.unwrap()));
        }
    }
}
